// proc.cpp —— fork + execvp + poll 的子进程执行器,以及后台线程包装 Runner。
//
// 这个文件是全工程最容易挂死的地方,所有关键不变式都写在注释里,改动前请先读:
//
//  I1  绝不 popen/system:拿不到 pid(无法超时 kill)、无法分离 stdout/stderr。
//  I2  fork 之后、exec 之前的子进程里**只允许 async-signal-safe 调用**:
//      不 new、不构造 std::string、不 printf、不 setenv。所有字符串在 fork 之前
//      就准备成 char* const[](见 ChildCtx / prepared 局部变量)。
//  I3  读端永不主动停止:输出到达 max_* 之后继续 read 并丢弃,只置 truncated。
//      一旦停止读取,管道写满 -> 子进程永久阻塞 -> 用户看到的就是"编辑器挂死"。
//  I4  stdin 的 EPIPE / POLLERR / POLLHUP 是**正常情况**(子进程不读 stdin 就退出),
//      只关掉写端,绝不当错误、绝不提前返回。
//  I5  两个输出管道都 EOF(或已 SIGKILL 且排空宽限到期)之后才 waitpid。
//      先 waitpid 再排空 = 经典死锁。
//  I6  最终等待用 waitpid(WNOHANG) 轮询 + 到期升级 kill,保证"任何路径都不会永久
//      阻塞在 waitpid 上"(例如子进程先关掉 stdout/stderr 再死循环)。
//  I7  不装 SIGCHLD handler(总是 waitpid 指定 pid),不干扰 ncurses / libcurl。
//  I8  只用 POSIX:pipe/fcntl/poll/dup2/setpgid/kill/waitpid。不用 pipe2/ppoll/
//      signalfd/epoll/pidfd(macOS 没有)。
#include "proc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "ai.h"  // AppEvent / EvKind —— Runner 推事件需要完整定义

// environ:macOS 的 dylib 语义要求走 _NSGetEnviron(),Linux 直接用全局符号。
#if defined(__APPLE__)
#include <crt_externs.h>
#define CPPIDE_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define CPPIDE_ENVIRON environ
#endif

namespace {

// ---------------------------------------------------------------- 小工具

int64_t monoMs() {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 0;
#if defined(CLOCK_MONOTONIC)
  clock_gettime(CLOCK_MONOTONIC, &ts);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  return static_cast<int64_t>(ts.tv_sec) * 1000 + static_cast<int64_t>(ts.tv_nsec) / 1000000;
}

void closeFd(int& fd) {
  if (fd >= 0) {
    int saved = errno;
    ::close(fd);
    errno = saved;
    fd = -1;
  }
}

bool setCloexec(int fd) {
  int fl = ::fcntl(fd, F_GETFD);
  if (fl < 0) return false;
  return ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC) == 0;
}

bool setNonblock(int fd) {
  int fl = ::fcntl(fd, F_GETFL);
  if (fl < 0) return false;
  return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

// 建一条管道,两端都 FD_CLOEXEC(不用 pipe2:macOS 没有)。
bool makePipe(int fds[2]) {
  fds[0] = fds[1] = -1;
  if (::pipe(fds) != 0) return false;
  if (!setCloexec(fds[0]) || !setCloexec(fds[1])) {
    closeFd(fds[0]);
    closeFd(fds[1]);
    return false;
  }
  return true;
}

// 把 fd 抬到 >= 3。否则子进程里 dup2 到 0/1/2 时可能把还没用上的管道端覆盖掉
// (只在调用者已经关掉 stdin/stdout 的畸形环境下才会发生,但代价极低)。
bool ensureHigh(int& fd) {
  if (fd < 0 || fd >= 3) return true;
  int nf = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
  if (nf < 0) return false;
  ::close(fd);
  fd = nf;
  return true;
}

// 父进程忽略 SIGPIPE:往"已经退出的子进程的 stdin"写会拿到 SIGPIPE,默认动作是
// 杀掉我们自己。main() 也会做一次,但 runProcess 作为库函数不能依赖调用者。
// 只在当前处置仍是 SIG_DFL 时才改,不覆盖调用者自己装的 handler。
void ignoreSigpipeOnce() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction old;
    memset(&old, 0, sizeof(old));
    if (::sigaction(SIGPIPE, nullptr, &old) == 0 && old.sa_handler == SIG_DFL) {
      struct sigaction sa;
      memset(&sa, 0, sizeof(sa));
      sa.sa_handler = SIG_IGN;
      ::sigemptyset(&sa.sa_mask);
      ::sigaction(SIGPIPE, &sa, nullptr);
    }
  });
}

// 建 pipe 到 fork 之间的互斥。
//
// 为什么需要它:pipe() 之后到 fcntl(FD_CLOEXEC) 之前有一个窗口,如果**另一个线程**
// 正好在这个窗口里 fork(),它的子进程就会继承我们这几个还没打上 CLOEXEC 的管道 fd,
// 于是那个子进程一直握着我们 stdout 的写端 -> 我们永远读不到 EOF = 挂死。
// macOS 没有 pipe2(),没法原子地建带 CLOEXEC 的管道,所以只能用锁把
// "建管道 -> fork -> 关子端" 这一段串起来。fork 本身只有几十微秒,代价可以忽略。
// (子进程继承到的是"已加锁"的副本,但它只 exec,永远不碰这把锁。)
std::mutex& spawnMutex() {
  static std::mutex m;
  return m;
}

// 追加到上限:超出部分丢弃并置 truncated。绝不因此停止读取(见 I3)。
void appendCapped(std::string& dst, bool& trunc, size_t cap, const char* buf, size_t n) {
  if (n == 0) return;
  if (dst.size() >= cap) {
    trunc = true;
    return;
  }
  size_t room = cap - dst.size();
  if (n <= room) {
    dst.append(buf, n);
  } else {
    dst.append(buf, room);
    trunc = true;
  }
}

// ------------------------------------------------- 子进程侧(async-signal-safe)

// fork 之前准备好的一切;子进程只读这些 POD,不做任何分配。
struct ChildCtx {
  const char* prog = nullptr;
  char* const* argv = nullptr;
  char* const* envp = nullptr;
  const char* cwd = nullptr;  // nullptr = 继承
  int fd_in = -1;
  int fd_out = -1;
  int fd_err = -1;
  int fd_exec = -1;
  int maxfd = 256;
};

enum : char { kStageChdir = 'c', kStageExec = 'e' };

// 通过 exec 状态管道回报 errno,然后 _exit(127)。fork 之后唯一可靠的报错通道。
[[noreturn]] void childFail(int fd, char stage, int err) {
  char msg[1 + sizeof(int)];
  msg[0] = stage;
  memcpy(msg + 1, &err, sizeof(int));
  ssize_t n = ::write(fd, msg, sizeof(msg));
  (void)n;
  ::_exit(127);
}

bool childDup(int from, int to) {
  if (from == to) {
    // dup2(x,x) 是 no-op 且**不会**清掉 FD_CLOEXEC,必须手动清,否则 exec 后
    // 这个标准 fd 会凭空消失。
    int fl = ::fcntl(to, F_GETFD);
    if (fl < 0) return false;
    return ::fcntl(to, F_SETFD, fl & ~FD_CLOEXEC) == 0;
  }
  for (;;) {
    if (::dup2(from, to) >= 0) return true;
    if (errno == EINTR) continue;
    return false;
  }
}

// [[noreturn]]:要么 exec 成功(不返回),要么 childFail -> _exit。
[[noreturn]] void childMain(const ChildCtx& c) {
  // 1) 自成进程组:父进程才能 kill(-pgid) 连带干掉孙子进程。
  ::setpgid(0, 0);

  // 2) 信号:父进程忽略了 SIGPIPE(还可能被 ncurses/libcurl 改过别的),
  //    不能把这些处置继承给子进程。同时解除信号屏蔽。
  ::signal(SIGPIPE, SIG_DFL);
  ::signal(SIGINT, SIG_DFL);
  ::signal(SIGTERM, SIG_DFL);
  ::signal(SIGHUP, SIG_DFL);
  ::signal(SIGQUIT, SIG_DFL);
  ::signal(SIGCHLD, SIG_DFL);
  {
    sigset_t empty;
    ::sigemptyset(&empty);
    ::sigprocmask(SIG_SETMASK, &empty, nullptr);
  }

  // 2.5) 禁 core dump。用户程序段错误时,内核会把 core 落在**子进程的 cwd**,
  //      也就是用户的项目目录里 —— 需求明确"不做调试器集成",那个 core 没人会用,
  //      对 ICPC 训练只是垃圾文件。禁掉不影响 waitpid 的 WIFSIGNALED/WTERMSIG,
  //      所以"被信号 SIGSEGV 终止"照样汇报得出来。
  //      setrlimit 是纯系统调用(async-signal-safe),满足 I2。失败也无所谓,忽略。
  {
    struct rlimit no_core;
    no_core.rlim_cur = 0;
    no_core.rlim_max = 0;
    (void)::setrlimit(RLIMIT_CORE, &no_core);
  }

  // 3) 三个标准 fd。
  if (!childDup(c.fd_in, STDIN_FILENO)) childFail(c.fd_exec, kStageExec, errno);
  if (!childDup(c.fd_out, STDOUT_FILENO)) childFail(c.fd_exec, kStageExec, errno);
  if (!childDup(c.fd_err, STDERR_FILENO)) childFail(c.fd_exec, kStageExec, errno);

  // 4) 关掉其它所有 fd(exec 状态管道除外 —— 它必须活到 exec 那一刻)。
  //    我们自己的 fd 都是 CLOEXEC,这一步主要是清掉从父进程继承来的非 CLOEXEC 泄漏。
  for (int fd = 3; fd < c.maxfd; ++fd) {
    if (fd == c.fd_exec) continue;
    ::close(fd);
  }

  // 5) cwd。
  if (c.cwd != nullptr && ::chdir(c.cwd) != 0) childFail(c.fd_exec, kStageChdir, errno);

  // 6) 环境:整张 envp 在 fork 之前就合并好了,这里只换一个指针
  //    (setenv 会 malloc,fork 之后不可用)。execvp 走的就是 environ。
  if (c.envp != nullptr) CPPIDE_ENVIRON = const_cast<char**>(c.envp);

  // 7) exec。成功则 CLOEXEC 让 fd_exec 自动关闭 -> 父进程读到 EOF。
  ::execvp(c.prog, const_cast<char* const*>(c.argv));
  childFail(c.fd_exec, kStageExec, errno);
}

// ------------------------------------------------------------ 父进程侧辅助

// 从 "K=V" 里取出 "K="(含等号)。没有 '=' 则返回整串 + "="。
std::string envKeyPrefix(const std::string& kv) {
  size_t p = kv.find('=');
  if (p == std::string::npos) return kv + "=";
  return kv.substr(0, p + 1);
}

struct WaitStatus {
  bool reaped = false;
  int raw = 0;
};

}  // namespace

// ---------------------------------------------------------------- 公开小函数

const char* signalName(int sig) {
  switch (sig) {
    case SIGHUP: return "SIGHUP";
    case SIGINT: return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL: return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    case SIGABRT: return "SIGABRT";
    case SIGFPE: return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGBUS: return "SIGBUS";
    case SIGSEGV: return "SIGSEGV";
    case SIGSYS: return "SIGSYS";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    case SIGURG: return "SIGURG";
    case SIGSTOP: return "SIGSTOP";
    case SIGTSTP: return "SIGTSTP";
    case SIGCONT: return "SIGCONT";
    case SIGCHLD: return "SIGCHLD";
    case SIGTTIN: return "SIGTTIN";
    case SIGTTOU: return "SIGTTOU";
    case SIGXCPU: return "SIGXCPU";
    case SIGXFSZ: return "SIGXFSZ";
    case SIGVTALRM: return "SIGVTALRM";
    case SIGPROF: return "SIGPROF";
    case SIGUSR1: return "SIGUSR1";
    case SIGUSR2: return "SIGUSR2";
#ifdef SIGWINCH
    case SIGWINCH: return "SIGWINCH";
#endif
    default: return "SIG?";
  }
}

std::string commandLineForDisplay(const ProcSpec& spec) {
  auto quote = [](const std::string& s) -> std::string {
    bool need = s.empty();
    for (char ch : s) {
      if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '/' || ch == '.' || ch == '-' ||
            ch == '_' || ch == '=' || ch == '+' || ch == ':' || ch == ',' || ch == '@' ||
            static_cast<unsigned char>(ch) >= 0x80)) {
        need = true;
        break;
      }
    }
    if (!need) return s;
    std::string out = "'";
    for (char ch : s) {
      if (ch == '\'') {
        out += "'\\''";
      } else {
        out += ch;
      }
    }
    out += "'";
    return out;
  };
  std::string out = quote(spec.prog);
  for (const std::string& a : spec.args) {
    out += ' ';
    out += quote(a);
  }
  return out;
}

std::string ProcResult::summary() const {
  std::string s;
  switch (outcome) {
    case ProcOutcome::SpawnFailed:
      s = "无法启动:" + (spawn_error.empty() ? std::string("未知错误") : spawn_error);
      return s;  // 没跑起来,不谈用时/截断
    case ProcOutcome::Timeout:
      s = "超时 " + std::to_string(wall_ms) + "ms,已终止";
      break;
    case ProcOutcome::Killed:
      s = "已终止";
      if (signaled) {
        s += " · 信号 ";
        s += signalName(signal);
      }
      s += " · 用时 " + std::to_string(wall_ms) + "ms";
      break;
    case ProcOutcome::Ok:
      if (signaled) {
        s = std::string("被信号 ") + signalName(signal) + " 终止";
      } else {
        s = "退出码 " + std::to_string(exit_code);
      }
      s += " · 用时 " + std::to_string(wall_ms) + "ms";
      break;
  }
  if (stdout_truncated || stderr_truncated) s += " · 输出已截断";
  return s;
}

// ---------------------------------------------------------------- runProcess

ProcResult runProcess(const ProcSpec& spec, std::atomic<pid_t>* out_pgid) {
  ProcResult res;
  const int64_t t0 = monoMs();
  auto finish = [&]() -> ProcResult {
    res.wall_ms = monoMs() - t0;
    if (out_pgid) out_pgid->store(0);
    return std::move(res);  // 移动:输出可能有好几 MB,别白拷一份
  };

  ignoreSigpipeOnce();
  if (out_pgid) out_pgid->store(0);

  // ---- fork 之前:把所有字符串固化成 char* const[](见 I2)
  std::vector<std::string> argv_store;
  argv_store.reserve(spec.args.size() + 1);
  argv_store.push_back(spec.prog);
  for (const std::string& a : spec.args) argv_store.push_back(a);
  std::vector<char*> argv;
  argv.reserve(argv_store.size() + 1);
  for (std::string& s : argv_store) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);

  // env:合并 environ 与 env_extra(同名以 env_extra 为准)。整张表 fork 前建好。
  std::vector<std::string> env_store;
  std::vector<char*> envp;
  {
    std::vector<std::string> prefixes;
    prefixes.reserve(spec.env_extra.size());
    for (const std::string& kv : spec.env_extra) prefixes.push_back(envKeyPrefix(kv));
    char** e = CPPIDE_ENVIRON;
    if (e) {
      for (; *e; ++e) {
        std::string cur(*e);
        bool overridden = false;
        for (const std::string& p : prefixes) {
          if (cur.size() >= p.size() && cur.compare(0, p.size(), p) == 0) {
            overridden = true;
            break;
          }
        }
        if (!overridden) env_store.push_back(std::move(cur));
      }
    }
    for (const std::string& kv : spec.env_extra) env_store.push_back(kv);
    envp.reserve(env_store.size() + 1);
    for (std::string& s : env_store) envp.push_back(const_cast<char*>(s.c_str()));
    envp.push_back(nullptr);
  }

  const std::string cwd_store = spec.cwd;
  int maxfd = static_cast<int>(::sysconf(_SC_OPEN_MAX));
  if (maxfd < 64 || maxfd > 4096) maxfd = 4096;

  // ---- 4 个管道,全部 FD_CLOEXEC。从这里到"父进程关掉子端"全程持锁(见 spawnMutex)。
  std::unique_lock<std::mutex> spawn_lk(spawnMutex());
  int p_in[2] = {-1, -1}, p_out[2] = {-1, -1}, p_err[2] = {-1, -1}, p_exec[2] = {-1, -1};
  auto closeAllPipes = [&] {
    closeFd(p_in[0]);
    closeFd(p_in[1]);
    closeFd(p_out[0]);
    closeFd(p_out[1]);
    closeFd(p_err[0]);
    closeFd(p_err[1]);
    closeFd(p_exec[0]);
    closeFd(p_exec[1]);
  };
  if (!makePipe(p_in) || !makePipe(p_out) || !makePipe(p_err) || !makePipe(p_exec)) {
    res.outcome = ProcOutcome::SpawnFailed;
    res.spawn_error = strerror(errno);
    closeAllPipes();
    return finish();
  }
  // 子进程侧要 dup2 的 fd 必须 >= 3(见 ensureHigh)。
  if (!ensureHigh(p_in[0]) || !ensureHigh(p_out[1]) || !ensureHigh(p_err[1]) ||
      !ensureHigh(p_exec[1])) {
    res.outcome = ProcOutcome::SpawnFailed;
    res.spawn_error = strerror(errno);
    closeAllPipes();
    return finish();
  }

  ChildCtx ctx;
  ctx.prog = argv_store[0].c_str();
  ctx.argv = argv.data();
  ctx.envp = envp.data();
  ctx.cwd = cwd_store.empty() ? nullptr : cwd_store.c_str();
  ctx.fd_in = p_in[0];
  ctx.fd_out = p_out[1];
  ctx.fd_err = p_err[1];
  ctx.fd_exec = p_exec[1];
  ctx.maxfd = maxfd;

  const pid_t pid = ::fork();
  if (pid < 0) {
    res.outcome = ProcOutcome::SpawnFailed;
    res.spawn_error = strerror(errno);
    closeAllPipes();
    return finish();
  }
  if (pid == 0) {
    // ==== 子进程:此后只允许 async-signal-safe 调用 ====
    childMain(ctx);  // [[noreturn]]
  }

  // ==== 父进程 ====
  // pgid 立刻交出去:killInFlight() 可能在 exec 还没完成时就被按下(Esc)。
  // 与子进程的 setpgid(0,0) 竞争无害 —— 两边都设成同一个值。
  ::setpgid(pid, pid);
  const pid_t pgid = pid;
  if (out_pgid) out_pgid->store(pgid);

  closeFd(p_in[0]);
  closeFd(p_out[1]);
  closeFd(p_err[1]);
  closeFd(p_exec[1]);
  spawn_lk.unlock();  // 子端已关,别人可以安全地 fork 了(poll 循环绝不持锁)

  int fd_in = p_in[1];
  int fd_out = p_out[0];
  int fd_err = p_err[0];
  int fd_exec = p_exec[0];

  // ---- 读 exec 状态管道:EOF = exec 成功;有数据 = errno + 阶段
  {
    char buf[1 + sizeof(int)];
    size_t got = 0;
    for (;;) {
      ssize_t n = ::read(fd_exec, buf + got, sizeof(buf) - got);
      if (n > 0) {
        got += static_cast<size_t>(n);
        if (got >= sizeof(buf)) break;
        continue;
      }
      if (n == 0) break;  // EOF
      if (errno == EINTR) continue;
      break;
    }
    closeFd(fd_exec);
    if (got >= sizeof(buf)) {
      int err = 0;
      memcpy(&err, buf + 1, sizeof(int));
      closeFd(fd_in);
      closeFd(fd_out);
      closeFd(fd_err);
      // 子进程已经 _exit(127),这里必然立刻回收,不会留僵尸。
      int st = 0;
      while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
      }
      res.outcome = ProcOutcome::SpawnFailed;
      res.exit_code = 127;
      const char* what = (buf[0] == kStageChdir) ? "无法进入目录 " : "无法执行 ";
      res.spawn_error = std::string(what) +
                        (buf[0] == kStageChdir ? cwd_store : spec.prog) + ":" + strerror(err);
      return finish();
    }
  }

  setNonblock(fd_in);
  setNonblock(fd_out);
  setNonblock(fd_err);

  // stdin_data 为空 -> 立刻给子进程 EOF
  size_t in_off = 0;
  if (spec.stdin_data.empty()) closeFd(fd_in);

  const bool has_deadline = spec.timeout_ms > 0;
  const int64_t deadline = t0 + (has_deadline ? spec.timeout_ms : 0);

  int kill_stage = 0;             // 0=未杀 1=已 SIGTERM 2=已 SIGKILL
  int64_t term_at = 0;            // SIGTERM -> SIGKILL 的 200ms 宽限
  int64_t kill_drain_until = 0;   // SIGKILL 之后最多再排空多久
  bool timed_out = false;
  bool hard_killed = false;
  size_t total_bytes = 0;

  const int kTermGraceMs = 200;
  const int kDrainAfterKillMs = 500;

  auto sendSignal = [&](int sig) {
    // 先打进程组(连带孙子进程),再补一发给直接子进程,防止 setpgid 竞争漏杀。
    ::kill(-pgid, sig);
    ::kill(pid, sig);
  };

  // 到期升级:超时 -> SIGTERM -> 200ms -> SIGKILL。poll 循环与 wait 循环共用。
  auto escalate = [&](int64_t now) {
    if (kill_stage == 0 && has_deadline && now >= deadline) {
      timed_out = true;
      kill_stage = 1;
      term_at = now;
      sendSignal(SIGTERM);
    }
    if (kill_stage == 1 && now - term_at >= kTermGraceMs) {
      kill_stage = 2;
      kill_drain_until = now + kDrainAfterKillMs;
      sendSignal(SIGKILL);
    }
  };

  auto hardKill = [&](int64_t now) {
    if (kill_stage < 2) {
      kill_stage = 2;
      kill_drain_until = now + kDrainAfterKillMs;
      sendSignal(SIGKILL);
    }
  };

  char buf[65536];

  // 读一个 fd 直到 EAGAIN / EOF。到达上限只置 truncated,绝不停止读(I3)。
  auto drainFd = [&](int& fd, std::string& dst, bool& trunc, size_t cap) {
    int rounds = 0;
    while (fd >= 0 && rounds++ < 32) {
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0) {
        total_bytes += static_cast<size_t>(n);
        appendCapped(dst, trunc, cap, buf, static_cast<size_t>(n));
        continue;
      }
      if (n == 0) {
        closeFd(fd);
        return;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      closeFd(fd);  // EIO 等:视作 EOF
      return;
    }
  };

  // ---- 主 poll 循环
  // 循环条件把 fd_in 也算进去:有的程序先关掉 stdout/stderr 再慢慢吃 stdin
  // (例如 `exec 1>&- 2>&-; cat > f`),只看输出端会让它的 stdin 被提前截断。
  // 这仍然是有界的 —— 超时升级逻辑就在循环里。
  while (fd_out >= 0 || fd_err >= 0 || fd_in >= 0) {
    int64_t now = monoMs();
    escalate(now);
    if (kill_stage == 2 && now >= kill_drain_until) break;  // 已 SIGKILL 且排空宽限到期

    struct pollfd pfd[3];
    int idx_in = -1, idx_out = -1, idx_err = -1;
    int nfds = 0;
    if (fd_in >= 0) {
      pfd[nfds].fd = fd_in;
      pfd[nfds].events = POLLOUT;
      pfd[nfds].revents = 0;
      idx_in = nfds++;
    }
    if (fd_out >= 0) {
      pfd[nfds].fd = fd_out;
      pfd[nfds].events = POLLIN;
      pfd[nfds].revents = 0;
      idx_out = nfds++;
    }
    if (fd_err >= 0) {
      pfd[nfds].fd = fd_err;
      pfd[nfds].events = POLLIN;
      pfd[nfds].revents = 0;
      idx_err = nfds++;
    }

    // 超时值由截止时刻推算;再加一个 100ms 上限,保证任何情况下都会回来复查。
    int wait_ms = 100;
    if (kill_stage == 0 && has_deadline) {
      int64_t left = deadline - now;
      if (left < wait_ms) wait_ms = static_cast<int>(left);
    } else if (kill_stage == 1) {
      int64_t left = term_at + kTermGraceMs - now;
      if (left < wait_ms) wait_ms = static_cast<int>(left);
    } else if (kill_stage == 2) {
      int64_t left = kill_drain_until - now;
      if (left < wait_ms) wait_ms = static_cast<int>(left);
    }
    if (wait_ms < 0) wait_ms = 0;

    int pr = ::poll(pfd, static_cast<nfds_t>(nfds), wait_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;  // I8:EINTR 必须重试
      // poll 本身坏了(不该发生):杀掉子进程,别把 waitpid 悬在那儿。
      hardKill(monoMs());
      break;
    }
    if (pr == 0) continue;  // 只是到点了,回去 escalate

    // 写 stdin:EPIPE / POLLERR / POLLHUP 都是正常情况(I4)
    if (idx_in >= 0 && fd_in >= 0 && pfd[idx_in].revents != 0) {
      short rev = pfd[idx_in].revents;
      if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
        closeFd(fd_in);
      } else if (rev & POLLOUT) {
        size_t remain = spec.stdin_data.size() - in_off;
        size_t chunk = remain > 65536 ? 65536 : remain;
        ssize_t n = ::write(fd_in, spec.stdin_data.data() + in_off, chunk);
        if (n > 0) {
          in_off += static_cast<size_t>(n);
          if (in_off >= spec.stdin_data.size()) closeFd(fd_in);  // 写完立刻 close
        } else if (n < 0) {
          if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            // 下一轮再来
          } else {
            closeFd(fd_in);  // EPIPE 等:子进程不读了,正常
          }
        }
      }
    }

    if (idx_out >= 0 && fd_out >= 0 && (pfd[idx_out].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
      drainFd(fd_out, res.stdout_text, res.stdout_truncated, spec.max_stdout);
    if (idx_err >= 0 && fd_err >= 0 && (pfd[idx_err].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
      drainFd(fd_err, res.stderr_text, res.stderr_truncated, spec.max_stderr);

    // 狂喷输出的程序没有等超时的意义
    if (spec.hard_kill_bytes > 0 && total_bytes > spec.hard_kill_bytes && kill_stage < 2) {
      hard_killed = true;
      hardKill(monoMs());
    }
  }

  closeFd(fd_in);
  closeFd(fd_out);
  closeFd(fd_err);

  // ---- 只有到这里才 waitpid(I5)。用 WNOHANG 轮询,保证不会永久阻塞(I6)。
  WaitStatus ws;
  {
    int64_t last_kick = 0;
    for (;;) {
      int st = 0;
      pid_t r = ::waitpid(pid, &st, WNOHANG);
      if (r == pid) {
        ws.reaped = true;
        ws.raw = st;
        break;
      }
      if (r < 0) {
        if (errno == EINTR) continue;
        break;  // ECHILD:被别人回收了(不该发生)
      }
      int64_t now = monoMs();
      escalate(now);
      // 已经 SIGKILL 但还没死:每 500ms 补一发(极端情况的兜底)。
      if (kill_stage >= 2) {
        if (last_kick == 0) last_kick = now;
        if (now - last_kick >= 500) {
          last_kick = now;
          sendSignal(SIGKILL);
        }
      } else if (!has_deadline) {
        // 无超时限制:子进程可能先关掉 stdout/stderr 再慢慢算,老实等。
      }
      ::poll(nullptr, 0, 5);  // 睡 5ms(不用 usleep:poll 已经在用了)
    }
  }
  if (out_pgid) out_pgid->store(0);

  if (ws.reaped) {
    if (WIFEXITED(ws.raw)) {
      res.exit_code = WEXITSTATUS(ws.raw);
      res.signaled = false;
      res.signal = 0;
    } else if (WIFSIGNALED(ws.raw)) {
      res.signaled = true;
      res.signal = WTERMSIG(ws.raw);
      res.exit_code = 128 + res.signal;
    }
  }

  if (timed_out) {
    res.outcome = ProcOutcome::Timeout;
  } else if (hard_killed) {
    res.outcome = ProcOutcome::Killed;
  } else if (res.signaled && res.signal == SIGKILL) {
    // 没超时也没超量却被 SIGKILL:只可能是外部的 killInFlight()。
    res.outcome = ProcOutcome::Killed;
  } else {
    res.outcome = ProcOutcome::Ok;
  }
  return finish();
}

// ------------------------------------------------------------------- Runner

Runner::Runner(Mailbox<AppEvent>* out) : out_(out) {
  th_ = std::thread([this] { workerLoop(); });
}

Runner::~Runner() {
  // 关队列 + 干掉在飞作业 + join。绝不 detach。
  //
  // 为什么要 killInFlight():否则一个 30s 死循环作业会让编辑器退出卡 30 秒;
  // 退出期用户已经不关心结果了。
  //
  // ★ 为什么是"循环 kill 到 busy_ 落下"而不是只 kill 一次:
  //   只 kill 一次会漏掉这个窗口 —— 工作线程刚 waitPop 到作业、还没把 pgid
  //   写进 cur_pgid_ 时,kill 打空,然后那个作业会跑满它自己的 timeout_ms。
  //   这里用 Dekker 式握手保证不漏:
  //     工作线程:先 busy_=true,再读 stop_;
  //     析构线程:先 stop_=true,再读 busy_。
  //   两个都是 seq_cst,所以至少有一方能看到对方 —— 要么工作线程看到 stop_ 而
  //   放弃作业,要么我们看到 busy_==true 并一直补刀直到它落下。
  //   (这个 bug 被 tests/test_proc.cpp 的"submit/kill/析构 竞争 ×40"抓到过。)
  stop_.store(true);
  jobs_.close();
  while (busy_.load()) {
    killInFlight();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (th_.joinable()) th_.join();
  jobs_.drain();
}

Runner::JobId Runner::submit(ProcSpec spec, JobKind kind) {
  Job j;
  j.id = next_id_.fetch_add(1);
  j.kind = kind;
  j.spec = std::move(spec);
  JobId id = j.id;
  jobs_.push(std::move(j));
  return id;
}

void Runner::killInFlight() {
  pid_t g = cur_pgid_.load();
  if (g > 0) {
    ::kill(-g, SIGKILL);
    ::kill(g, SIGKILL);
  }
}

bool Runner::busy() const { return busy_.load() || !jobs_.empty(); }

Runner::JobId Runner::currentJob() const { return cur_job_.load(); }

void Runner::cancelQueued() { jobs_.drain(); }

void Runner::workerLoop() {
  for (;;) {
    if (stop_.load()) break;
    Job j;
    if (!jobs_.waitPop(j, 200)) {
      if (stop_.load() || jobs_.closed()) break;
      continue;
    }
    // ★ 顺序要紧:先立 busy_,再读 stop_。见 ~Runner() 里的 Dekker 握手说明。
    busy_.store(true);
    if (stop_.load()) {  // 退出期:丢弃尚未开始的作业
      busy_.store(false);
      break;
    }
    cur_job_.store(j.id);
    if (out_) {
      AppEvent ev;
      ev.kind = (j.kind == JobKind::Compile) ? EvKind::CompileStarted : EvKind::RunStarted;
      ev.gen = j.id;
      out_->push(std::move(ev));
    }

    auto pr = std::make_shared<ProcResult>(runProcess(j.spec, &cur_pgid_));
    cur_pgid_.store(0);

    if (out_) {
      AppEvent ev;
      ev.kind = (j.kind == JobKind::Compile) ? EvKind::CompileDone : EvKind::RunDone;
      ev.gen = j.id;
      ev.run = std::move(pr);
      out_->push(std::move(ev));
    }
    cur_job_.store(0);
    busy_.store(false);
  }
  cur_job_.store(0);
  busy_.store(false);
}

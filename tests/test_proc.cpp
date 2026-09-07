// tests/test_proc.cpp —— proc.cpp 的行为测试。
//
// 这个测试文件测的是"挂死",所以它自己**绝不允许挂死**:
//   * main() 一进来就 alarm(90) —— 任何路径卡住都会被 SIGALRM 打死并打印现场;
//   * 每个用例结束后断言自己的墙钟上限(卡在某个用例上会当场失败而不是拖到最后);
//   * 所有等待事件的循环都带轮次上限,没有一个 while(true)。
//
// 故意**不**在 main 里 signal(SIGPIPE, SIG_IGN):这样"给提前退出的子进程灌 10MB
// stdin"这条用例真的在检验 runProcess 自己的 SIGPIPE 防护(而不是被测试掩盖掉)。
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../src/ai.h"
#include "../src/proc.h"

// ------------------------------------------------------------------ 基础设施

static int g_checks = 0;
static int g_cases = 0;
static const char* g_case = "(none)";

#define CHECK(x)                                                                       \
  do {                                                                                 \
    ++g_checks;                                                                        \
    if (!(x)) {                                                                        \
      fprintf(stderr, "\n*** FAIL [%s] %s:%d: %s\n", g_case, __FILE__, __LINE__, #x);  \
      fflush(stderr);                                                                  \
      abort();                                                                         \
    }                                                                                  \
  } while (0)

#define CHECK_EQ_S(a, b)                                                            \
  do {                                                                              \
    ++g_checks;                                                                     \
    std::string aa = (a), bb = (b);                                                 \
    if (aa != bb) {                                                                 \
      fprintf(stderr, "\n*** FAIL [%s] %s:%d: \"%s\" != \"%s\"\n", g_case, __FILE__, \
              __LINE__, aa.substr(0, 200).c_str(), bb.substr(0, 200).c_str());      \
      fflush(stderr);                                                               \
      abort();                                                                      \
    }                                                                               \
  } while (0)

static int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 每个用例的墙钟守卫:超出 budget 直接失败(说明某处比预期慢/卡住了)。
struct Case {
  int64_t t0;
  int64_t budget;
  const char* name;
  Case(const char* n, int64_t b) : t0(nowMs()), budget(b), name(n) {
    g_case = n;
    ++g_cases;
    printf("  [%02d] %-34s ", g_cases, n);
    fflush(stdout);
  }
  ~Case() {
    int64_t d = nowMs() - t0;
    printf("%5lldms%s\n", (long long)d, d > budget ? "  <== 超预算!" : "");
    fflush(stdout);
    if (d > budget) {
      fprintf(stderr, "*** FAIL [%s] 墙钟 %lldms > 预算 %lldms\n", name, (long long)d,
              (long long)budget);
      abort();
    }
  }
};

static void alarmHandler(int) {
  const char msg[] = "\n*** FAIL: 测试整体超时(某处挂死了)\n";
  ssize_t n = write(2, msg, sizeof(msg) - 1);
  (void)n;
  _exit(70);
}

// /proc/self/fd 里的 fd 数量;非 Linux 返回 -1(跳过该断言)。
static int fdCount() {
  DIR* d = opendir("/proc/self/fd");
  if (!d) return -1;
  int n = 0;
  while (readdir(d) != nullptr) ++n;
  closedir(d);
  return n;
}

// 读 /proc/<pid>/stat,取出状态字符与 pgrp。返回 false = 进程不存在。
// 注意:容器里的 PID 1 通常不收养/回收孤儿,所以被 kill 掉的**孙子进程**会变成
// 挂在 PID 1 下的僵尸。僵尸仍然算进程组成员,于是 kill(-pgid,0) 依旧成功 ——
// 因此"进程组已经被杀干净"这件事**不能**用 kill(-pgid,0)==ESRCH 来断言,
// 必须逐个看状态位,把 Z/X 当作已死。
static bool readStat(pid_t pid, char* state_out, pid_t* pgrp_out) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
  FILE* f = fopen(path, "r");
  if (!f) return false;
  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  char* rp = strrchr(buf, ')');
  if (!rp) return false;
  char st = 0;
  long ppid = 0, pgrp = 0;
  if (sscanf(rp + 1, " %c %ld %ld", &st, &ppid, &pgrp) != 3) return false;
  if (state_out) *state_out = st;
  if (pgrp_out) *pgrp_out = static_cast<pid_t>(pgrp);
  return true;
}

// 进程是否还"活着"(僵尸算死)。
static bool procAlive(pid_t pid) {
  char st = 0;
  if (!readStat(pid, &st, nullptr)) return false;
  return !(st == 'Z' || st == 'X');
}

// 进程组里仍然活着(非僵尸)的进程数。/proc 不可用时返回 0(跳过断言)。
static int groupLiveCount(pid_t pgid) {
  DIR* d = opendir("/proc");
  if (!d) return 0;
  int live = 0;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    pid_t pid = static_cast<pid_t>(atol(e->d_name));
    char st = 0;
    pid_t pg = 0;
    if (!readStat(pid, &st, &pg)) continue;
    if (pg == pgid && st != 'Z' && st != 'X') ++live;
  }
  closedir(d);
  return live;
}

// 等进程组彻底死透(最多 1 秒),然后断言。
static int groupLiveAfterSettle(pid_t pgid) {
  for (int i = 0; i < 100 && groupLiveCount(pgid) > 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return groupLiveCount(pgid);
}

static ProcSpec shSpec(const std::string& script, int timeout_ms = 5000) {
  ProcSpec s;
  s.prog = "/bin/sh";
  s.args = {"-c", script};
  s.timeout_ms = timeout_ms;
  return s;
}

// 在后台线程跑 runProcess,主线程把 out_pgid 的快照抓下来(runProcess 返回前会清 0)。
struct Watched {
  ProcResult res;
  pid_t pgid = 0;
  int64_t ms = 0;
};

static Watched runWatched(const ProcSpec& spec) {
  Watched w;
  std::atomic<pid_t> pg{0};
  std::atomic<bool> done{false};
  int64_t t0 = nowMs();
  std::thread th([&] {
    w.res = runProcess(spec, &pg);
    done.store(true);
  });
  // 轮询抓 pgid,并在子进程还活着时把它记下来。
  while (!done.load()) {
    pid_t g = pg.load();
    if (g > 0) w.pgid = g;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  th.join();
  w.ms = nowMs() - t0;
  return w;
}

// 从 mailbox 里等一个事件,最多等 timeout_ms(带轮次上限,不可能死等)。
static bool waitEvent(Mailbox<AppEvent>& mb, AppEvent& out, int timeout_ms) {
  int64_t deadline = nowMs() + timeout_ms;
  while (nowMs() < deadline) {
    if (mb.tryPop(out)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

// ---------------------------------------------------------------------- 用例

static void t_pure_helpers() {
  Case c("纯函数:signalName/命令行/摘要", 200);
  CHECK_EQ_S(signalName(SIGSEGV), "SIGSEGV");
  CHECK_EQ_S(signalName(SIGKILL), "SIGKILL");
  CHECK_EQ_S(signalName(SIGTERM), "SIGTERM");
  CHECK_EQ_S(signalName(12345), "SIG?");

  ProcSpec s;
  s.prog = "/usr/bin/g++";
  s.args = {"-O2", "/tmp/a b.cpp", "-o", "out"};
  CHECK_EQ_S(commandLineForDisplay(s), "/usr/bin/g++ -O2 '/tmp/a b.cpp' -o out");

  ProcResult r;
  r.outcome = ProcOutcome::Ok;
  r.exit_code = 0;
  r.wall_ms = 12;
  CHECK_EQ_S(r.summary(), "退出码 0 · 用时 12ms");
  CHECK(r.ok());
  r.stdout_truncated = true;
  CHECK_EQ_S(r.summary(), "退出码 0 · 用时 12ms · 输出已截断");
  r.stdout_truncated = false;

  r.exit_code = 1;
  CHECK(!r.ok());
  CHECK_EQ_S(r.summary(), "退出码 1 · 用时 12ms");

  r.signaled = true;
  r.signal = SIGSEGV;
  r.wall_ms = 3;
  CHECK(!r.ok());
  CHECK_EQ_S(r.summary(), "被信号 SIGSEGV 终止 · 用时 3ms");

  ProcResult t;
  t.outcome = ProcOutcome::Timeout;
  t.wall_ms = 5000;
  CHECK_EQ_S(t.summary(), "超时 5000ms,已终止");
  CHECK(!t.ok());

  ProcResult f;
  f.outcome = ProcOutcome::SpawnFailed;
  f.spawn_error = "无法执行 nope:No such file or directory";
  CHECK_EQ_S(f.summary(), "无法启动:无法执行 nope:No such file or directory");
}

static void t_echo_hello() {
  Case c("正常:echo hello(走 PATH)", 3000);
  ProcSpec s;
  s.prog = "echo";  // 不带路径,验证 execvp 的 PATH 解析
  s.args = {"hello"};
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK(r.exit_code == 0);
  CHECK(!r.signaled);
  CHECK(r.ok());
  CHECK_EQ_S(r.stdout_text, "hello\n");
  CHECK_EQ_S(r.stderr_text, "");
  CHECK(!r.stdout_truncated && !r.stderr_truncated);
  CHECK(r.spawn_error.empty());
}

static void t_argv_no_shell() {
  Case c("argv 逐个传递(不经过 shell)", 3000);
  ProcSpec s;
  s.prog = "/bin/echo";
  s.args = {"a b", "*", "$HOME", "'"};
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK_EQ_S(r.stdout_text, "a b * $HOME '\n");
}

static void t_exit_code() {
  Case c("退出码非 0 + stderr 分离", 3000);
  ProcResult r = runProcess(shSpec("printf out; printf err >&2; exit 42"));
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK(r.exit_code == 42);
  CHECK(!r.signaled);
  CHECK(!r.ok());
  CHECK_EQ_S(r.stdout_text, "out");
  CHECK_EQ_S(r.stderr_text, "err");
}

static void t_signaled() {
  Case c("被信号杀死:kill -SEGV $$", 3000);
  // ★ 让子进程在 /tmp 里死,并且先把 core 上限压到 0。
  //   为什么:本机 /proc/sys/kernel/core_pattern == "core",内核会把 core 落在
  //   **子进程的 cwd**;不设 cwd 的话每跑一次 `make tests` 就在仓库根目录
  //   留下一个 430KB 的 `core` 文件(实测确实留下了)。
  //   `ulimit -c 0` 由 /bin/sh 自己执行,不需要改 proc.cpp。
  //   (proc.cpp 的 childMain 现在也会 setrlimit(RLIMIT_CORE, 0);这里的
  //    ulimit 保留下来当第二道保险,并让本用例只关心"信号汇报"这一件事。
  //    proc.cpp 那道防线由 t_no_core_dump 单独盯。)
  ProcSpec spec = shSpec("ulimit -c 0 2>/dev/null; kill -SEGV $$");
  spec.cwd = "/tmp";
  ProcResult r = runProcess(spec);
  CHECK(r.outcome == ProcOutcome::Ok);  // 不是超时也不是我们杀的
  CHECK(r.signaled);
  CHECK(r.signal == SIGSEGV);
  CHECK(!r.ok());
  CHECK(r.summary().find("被信号 SIGSEGV 终止") == 0);
}

// 子进程被 SIGSEGV 打死后,**不许**在它的 cwd 里留下 core 文件。
// 依赖 proc.cpp::childMain 的 setrlimit(RLIMIT_CORE, {0,0});脚本里故意
// **不写** `ulimit -c 0`,否则测的就是 /bin/sh 而不是 proc.cpp 了。
static void t_no_core_dump() {
  Case c("段错误不留 core 文件", 5000);

  char dir[] = "/tmp/cppide_core_XXXXXX";
  CHECK(::mkdtemp(dir) != nullptr);

  // 前置条件自检:core_pattern 若是 "|/usr/bin/..." 之类的管道,或本进程的
  // RLIMIT_CORE 本来就是 0,那"没有 core"就不构成证据 —— 此时只做信号断言。
  bool pattern_is_plain_file = false;
  if (FILE* f = ::fopen("/proc/sys/kernel/core_pattern", "r")) {
    char pat[256] = {0};
    if (::fgets(pat, sizeof pat, f) != nullptr) {
      pattern_is_plain_file = (pat[0] != '|' && pat[0] != '/');
    }
    ::fclose(f);
  }
  struct rlimit rl{};
  const bool core_allowed =
      (::getrlimit(RLIMIT_CORE, &rl) == 0) && (rl.rlim_cur != 0);

  ProcSpec spec = shSpec("kill -SEGV $$");
  spec.cwd = dir;
  ProcResult r = runProcess(spec);
  CHECK(r.signaled);
  CHECK(r.signal == SIGSEGV);   // 禁 core 不影响信号汇报

  // core 是内核在子进程死后写的,给它一点时间(通常是同步的,但别赌)。
  ::usleep(150 * 1000);

  int found = 0;
  if (DIR* d = ::opendir(dir)) {
    while (struct dirent* e = ::readdir(d)) {
      if (::strncmp(e->d_name, "core", 4) == 0) ++found;
    }
    ::closedir(d);
  }
  if (pattern_is_plain_file && core_allowed) {
    if (found != 0) {
      fprintf(stderr, "\n*** 子进程在 %s 里留下了 %d 个 core 文件\n", dir, found);
    }
    CHECK(found == 0);
  } else {
    fprintf(stderr, "  (core_pattern/RLIMIT_CORE 不满足前置条件,只断言信号汇报)\n");
    ++g_checks;
  }

  // 收尾:目录必须能被删掉(顺带证明里面确实是空的)
  if (found == 0) CHECK(::rmdir(dir) == 0);
}

static void t_timeout_busyloop() {
  Case c("死循环 -> Timeout 且进程真的死了", 3000);
  Watched w = runWatched(shSpec("while :; do :; done", 400));
  CHECK(w.res.outcome == ProcOutcome::Timeout);
  CHECK(w.res.wall_ms >= 380);
  CHECK(w.ms < 1500);
  CHECK(w.res.signaled);
  CHECK(w.res.signal == SIGTERM);  // 老实的 sh 收到 SIGTERM 就死
  CHECK(w.res.summary().find("超时") == 0);
  // 进程组必须已经没了
  CHECK(w.pgid > 0);
  CHECK(groupLiveAfterSettle(w.pgid) == 0);  // 整个进程组必须一个活口都不剩
}

static void t_timeout_ignores_term() {
  Case c("无视 SIGTERM -> 升级到 SIGKILL", 3000);
  Watched w = runWatched(shSpec("trap '' TERM; while :; do :; done", 400));
  CHECK(w.res.outcome == ProcOutcome::Timeout);
  CHECK(w.res.signaled);
  CHECK(w.res.signal == SIGKILL);  // 200ms 宽限后必须升级
  CHECK(w.res.wall_ms >= 580);     // 400 + 200 宽限
  CHECK(w.ms < 1600);
  CHECK(w.pgid > 0);
  CHECK(groupLiveAfterSettle(w.pgid) == 0);  // 整个进程组必须一个活口都不剩
}

static void t_huge_output_hard_kill() {
  Case c("巨量输出 -> hard_kill_bytes 生效", 8000);
  ProcSpec s = shSpec("yes ABCDEFGHIJKLMNOP", 30000);  // 超时很大,必须靠 hard_kill 收住
  s.max_stdout = 4096;
  s.max_stderr = 4096;
  s.hard_kill_bytes = 1u << 20;
  Watched w = runWatched(s);
  CHECK(w.res.outcome == ProcOutcome::Killed);
  CHECK(w.res.stdout_truncated);
  CHECK(w.res.stdout_text.size() == 4096);
  CHECK(w.ms < 6000);  // 远早于 30s 超时
  CHECK(w.res.summary().find("输出已截断") != std::string::npos);
  CHECK(w.pgid > 0);
  CHECK(groupLiveAfterSettle(w.pgid) == 0);  // 整个进程组必须一个活口都不剩
}

static void t_huge_output_dev_zero() {
  Case c("100MB /dev/zero -> 不挂死", 20000);
  ProcSpec s = shSpec("head -c 100000000 /dev/zero", 30000);
  s.max_stdout = 1u << 20;
  s.hard_kill_bytes = 8u << 20;
  ProcResult r = runProcess(s);
  // 8MB 就 kill,不可能等到 100MB 传完
  CHECK(r.outcome == ProcOutcome::Killed);
  CHECK(r.stdout_truncated);
  CHECK(r.stdout_text.size() == (1u << 20));
}

static void t_truncate_flags_only() {
  Case c("到上限只截断,进程仍正常结束", 8000);
  ProcSpec s = shSpec("head -c 200000 /dev/zero | tr '\\0' x; printf E >&2; exit 7", 20000);
  s.max_stdout = 1000;
  s.max_stderr = 1000;
  s.hard_kill_bytes = 64u << 20;
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);  // 没被我们杀
  CHECK(r.exit_code == 7);
  CHECK(r.stdout_text.size() == 1000);
  CHECK(r.stdout_truncated);
  CHECK(r.stdout_text == std::string(1000, 'x'));
  CHECK_EQ_S(r.stderr_text, "E");
  CHECK(!r.stderr_truncated);
}

static void t_stdin_early_exit() {
  Case c("10MB stdin 灌给不读的进程", 8000);
  ProcSpec s = shSpec("exit 3", 10000);
  s.stdin_data.assign(10u << 20, 'Z');  // 10MB,子进程一个字节都不读
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK(r.exit_code == 3);
  CHECK_EQ_S(r.stdout_text, "");
  // 关键:父进程既没被 SIGPIPE 打死(否则这行根本执行不到),也没卡住
}

static void t_stdin_roundtrip() {
  Case c("cat 回显 10MB stdin 完整", 20000);
  std::string data;
  data.reserve(10u << 20);
  unsigned x = 12345;
  while (data.size() < (10u << 20)) {
    x = x * 1103515245u + 12345u;
    data.push_back(static_cast<char>('a' + (x >> 16) % 26));
  }
  ProcSpec s;
  s.prog = "cat";
  s.timeout_ms = 25000;
  s.stdin_data = data;
  s.max_stdout = 32u << 20;
  s.hard_kill_bytes = 64u << 20;
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK(r.exit_code == 0);
  CHECK(!r.stdout_truncated);
  CHECK(r.stdout_text.size() == data.size());
  CHECK(r.stdout_text == data);
}

static void t_interleaved_out_err() {
  Case c("stdout/stderr 同时狂喷不死锁", 20000);
  // 两条 400KB 的流并发写,远超 64KB 管道缓冲 —— 只读一边就会死锁。
  ProcSpec s = shSpec(
      "(head -c 400000 /dev/zero | tr '\\0' a) & "
      "(head -c 400000 /dev/zero | tr '\\0' b >&2); wait",
      20000);
  s.max_stdout = 4u << 20;
  s.max_stderr = 4u << 20;
  s.hard_kill_bytes = 64u << 20;
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK(r.exit_code == 0);
  CHECK(!r.stdout_truncated && !r.stderr_truncated);
  CHECK(r.stdout_text.size() == 400000);
  CHECK(r.stderr_text.size() == 400000);
  CHECK(r.stdout_text == std::string(400000, 'a'));
  CHECK(r.stderr_text == std::string(400000, 'b'));
}

static void t_grandchild_killed() {
  Case c("孙子进程随进程组一起被杀", 5000);
  // 子进程 fork 出一个后台孙子(它 exec 成 sleep 30)后自己立刻退出。
  // 孙子仍握着 stdout 写端 -> 父进程读不到 EOF -> 必须靠超时 kill 整个进程组。
  Watched w = runWatched(shSpec("sh -c 'echo GC:$$; exec sleep 30' & exit 0", 500));
  CHECK(w.res.outcome == ProcOutcome::Timeout);
  CHECK(w.ms < 3000);  // 绝不能等 30 秒
  size_t p = w.res.stdout_text.find("GC:");
  CHECK(p != std::string::npos);
  pid_t gc = static_cast<pid_t>(atol(w.res.stdout_text.c_str() + p + 3));
  CHECK(gc > 1);
  // 给内核一点时间收尸
  for (int i = 0; i < 100 && procAlive(gc); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK(!procAlive(gc));  // 孙子必须已经死了(僵尸也算死)
  CHECK(w.pgid > 0);
}

static void t_spawn_failed() {
  Case c("execvp 失败 -> SpawnFailed", 3000);
  ProcSpec s;
  s.prog = "cppide_definitely_no_such_program_42";
  s.args = {"x"};
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::SpawnFailed);
  CHECK(!r.spawn_error.empty());
  CHECK(r.spawn_error.find("cppide_definitely_no_such_program_42") != std::string::npos);
  CHECK(r.spawn_error.find(strerror(ENOENT)) != std::string::npos);
  CHECK(!r.ok());
  CHECK(r.summary().find("无法启动") == 0);
  // 反复失败也不能泄漏 fd / 留僵尸
  for (int i = 0; i < 50; ++i) {
    ProcResult r2 = runProcess(s);
    CHECK(r2.outcome == ProcOutcome::SpawnFailed);
  }
  CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
}

static void t_bad_cwd() {
  Case c("cwd 不存在 -> SpawnFailed", 3000);
  ProcSpec s = shSpec("echo hi");
  s.cwd = "/cppide_no_such_dir_42/sub";
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::SpawnFailed);
  CHECK(r.spawn_error.find("/cppide_no_such_dir_42/sub") != std::string::npos);
  CHECK(r.stdout_text.empty());
}

static void t_good_cwd() {
  Case c("cwd 生效", 3000);
  ProcSpec s = shSpec("pwd");
  s.cwd = "/tmp";
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK(r.stdout_text.find("/tmp") == 0);
}

static void t_env_extra() {
  Case c("env_extra 生效并覆盖同名变量", 3000);
  setenv("LC_ALL", "zh_CN.UTF-8", 1);       // 故意先污染
  setenv("CPPIDE_KEEP", "kept", 1);         // 应当被继承
  ProcSpec s = shSpec("printf '%s|%s|%s' \"$LC_ALL\" \"$CPPIDE_X\" \"$CPPIDE_KEEP\"");
  s.env_extra = {"LC_ALL=C", "CPPIDE_X=42"};
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK_EQ_S(r.stdout_text, "C|42|kept");
  unsetenv("LC_ALL");
  unsetenv("CPPIDE_KEEP");
}

static void t_empty_stdin_eof() {
  Case c("stdin_data 为空 -> 立刻 EOF", 3000);
  ProcSpec s;
  s.prog = "cat";
  s.timeout_ms = 2000;
  ProcResult r = runProcess(s);  // 不给 stdin_data:必须立刻 EOF,不能卡到超时
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK(r.exit_code == 0);
  CHECK(r.stdout_text.empty());
  CHECK(r.wall_ms < 1500);
}

static void t_no_timeout_limit() {
  Case c("timeout_ms<=0 表示不限时", 4000);
  ProcSpec s = shSpec("sleep 0.3; echo done", 0);
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK_EQ_S(r.stdout_text, "done\n");
  CHECK(r.wall_ms >= 250);
}

static void t_closed_stdio_then_loop() {
  Case c("先关 stdout/stderr 再死循环", 4000);
  // 两个输出管道立刻 EOF,但进程还在跑 -> 若实现直接 waitpid 阻塞就永远回不来。
  Watched w = runWatched(shSpec("exec 1>&- 2>&-; while :; do :; done", 400));
  CHECK(w.res.outcome == ProcOutcome::Timeout);
  CHECK(w.ms < 2500);
  CHECK(w.pgid > 0);
  CHECK(groupLiveAfterSettle(w.pgid) == 0);  // 整个进程组必须一个活口都不剩
}

// ------------------------------------------------------------------- Runner

static void t_runner_serial() {
  Case c("Runner:多作业串行 + 事件入箱", 10000);
  Mailbox<AppEvent> mb;
  std::vector<Runner::JobId> ids;
  {
    Runner rn(&mb);
    for (int i = 0; i < 3; ++i)
      ids.push_back(rn.submit(shSpec("printf J" + std::to_string(i)), JobKind::Run));
    CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3);
    CHECK(rn.busy());  // 有排队就算 busy
    // 等 3 个 Done
    int done = 0;
    std::vector<AppEvent> evs;
    int64_t deadline = nowMs() + 8000;
    while (done < 3 && nowMs() < deadline) {
      AppEvent ev;
      if (!waitEvent(mb, ev, 200)) continue;
      if (ev.kind == EvKind::RunDone) ++done;
      evs.push_back(std::move(ev));
    }
    CHECK(done == 3);
    // 事件顺序必须是 S1 D1 S2 D2 S3 D3(串行的证据)
    CHECK(evs.size() == 6);
    for (int i = 0; i < 3; ++i) {
      CHECK(evs[2 * i].kind == EvKind::RunStarted);
      CHECK(evs[2 * i].gen == static_cast<uint64_t>(i + 1));
      CHECK(evs[2 * i + 1].kind == EvKind::RunDone);
      CHECK(evs[2 * i + 1].gen == static_cast<uint64_t>(i + 1));
      CHECK(evs[2 * i + 1].run != nullptr);
      CHECK(evs[2 * i + 1].run->outcome == ProcOutcome::Ok);
      CHECK_EQ_S(evs[2 * i + 1].run->stdout_text, "J" + std::to_string(i));
      CHECK(evs[2 * i].run == nullptr);
      CHECK(evs[2 * i].compile == nullptr);  // Runner 不填 compile
      CHECK(evs[2 * i + 1].compile == nullptr);
    }
    // 全部跑完后应当空闲
    for (int i = 0; i < 100 && rn.busy(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!rn.busy());
    CHECK(rn.currentJob() == 0);
  }
}

static void t_runner_compile_kind() {
  Case c("Runner:JobKind::Compile 事件种类", 8000);
  Mailbox<AppEvent> mb;
  {
    Runner rn(&mb);
    ProcSpec s = shSpec("printf 'x.cpp:1:1: error: boom' >&2; exit 1");
    s.env_extra = {"LC_ALL=C"};
    Runner::JobId id = rn.submit(s, JobKind::Compile);
    AppEvent a, b;
    CHECK(waitEvent(mb, a, 5000));
    CHECK(waitEvent(mb, b, 5000));
    CHECK(a.kind == EvKind::CompileStarted && a.gen == id);
    CHECK(b.kind == EvKind::CompileDone && b.gen == id);
    CHECK(b.run != nullptr);
    CHECK(b.run->exit_code == 1);
    CHECK(b.run->stderr_text.find("error: boom") != std::string::npos);
    CHECK(b.compile == nullptr);  // 诊断解析是 App 的活
  }
}

static void t_runner_kill_inflight() {
  Case c("Runner:killInFlight 杀在飞死循环", 8000);
  Mailbox<AppEvent> mb;
  int64_t t0 = nowMs();
  {
    Runner rn(&mb);
    rn.submit(shSpec("while :; do :; done", 30000), JobKind::Run);
    AppEvent ev;
    CHECK(waitEvent(mb, ev, 3000));
    CHECK(ev.kind == EvKind::RunStarted);
    // 反复按 Esc:直到 Done 出现(pgid 可能还没写进原子变量,所以要重试)
    bool got = false;
    int64_t deadline = nowMs() + 5000;
    while (nowMs() < deadline) {
      rn.killInFlight();
      if (mb.tryPop(ev)) {
        got = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(got);
    CHECK(ev.kind == EvKind::RunDone);
    CHECK(ev.run != nullptr);
    CHECK(ev.run->outcome == ProcOutcome::Killed);
    CHECK(ev.run->signaled && ev.run->signal == SIGKILL);
    CHECK(nowMs() - t0 < 6000);  // 绝不等 30 秒超时
  }
}

static void t_runner_cancel_queued() {
  Case c("Runner:cancelQueued 丢弃排队作业", 8000);
  Mailbox<AppEvent> mb;
  {
    Runner rn(&mb);
    rn.submit(shSpec("sleep 0.4", 5000), JobKind::Run);
    for (int i = 0; i < 5; ++i) rn.submit(shSpec("echo nope", 5000), JobKind::Run);
    rn.cancelQueued();
    // 只有第 1 个会跑完(它已经在飞或者也可能刚好被丢掉 —— 两者都可接受),
    // 但绝不能有 6 个 Done。
    int done = 0;
    int64_t deadline = nowMs() + 1200;
    while (nowMs() < deadline) {
      AppEvent ev;
      if (mb.tryPop(ev)) {
        if (ev.kind == EvKind::RunDone) ++done;
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(done <= 1);
  }
}

static void t_runner_dtor_inflight() {
  Case c("Runner:析构时有在飞作业(易死锁)", 6000);
  Mailbox<AppEvent> mb;
  int64_t t0 = nowMs();
  pid_t pg = 0;
  {
    Runner rn(&mb);
    rn.submit(shSpec("trap '' TERM; while :; do :; done", 60000), JobKind::Run);
    for (int i = 0; i < 5; ++i) rn.submit(shSpec("echo q", 60000), JobKind::Run);
    AppEvent ev;
    CHECK(waitEvent(mb, ev, 3000));
    CHECK(ev.kind == EvKind::RunStarted);
    // 让 runProcess 真正 spawn 出来
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pg = static_cast<pid_t>(ev.gen);  // 仅占位,真正的 pgid 拿不到(私有成员)
    (void)pg;
  }  // <- 析构:必须立刻返回,不能等 60 秒超时,也不能死锁
  int64_t d = nowMs() - t0;
  CHECK(d < 4000);
  // 析构后不应有任何遗留子进程
  for (int i = 0; i < 200; ++i) {
    pid_t r = waitpid(-1, nullptr, WNOHANG);
    if (r == -1 && errno == ECHILD) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
}

static void t_runner_no_mailbox() {
  Case c("Runner:out=nullptr 也不崩", 5000);
  {
    Runner rn(nullptr);
    rn.submit(shSpec("echo x"), JobKind::Run);
    for (int i = 0; i < 200 && rn.busy(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!rn.busy());
  }
}

static void t_runner_empty_dtor() {
  Case c("Runner:空闲析构 < 300ms", 2000);
  int64_t t0 = nowMs();
  {
    Mailbox<AppEvent> mb;
    Runner rn(&mb);
  }
  CHECK(nowMs() - t0 < 800);
}

static void t_closed_stdio_but_reads_stdin() {
  Case c("先关 stdout/stderr 再吃完 stdin", 8000);
  // 两个输出管道立刻 EOF,但 stdin 还得灌 2MB 进去 —— 循环条件若只看输出端,
  // stdin 会被提前截断(wc -c 就会少)。
  ProcSpec s = shSpec("exec 1>&- 2>&-; wc -c > /tmp/cppide_stdin_len.txt", 6000);
  s.stdin_data.assign(2u << 20, 'q');
  ProcResult r = runProcess(s);
  CHECK(r.outcome == ProcOutcome::Ok);
  CHECK(r.exit_code == 0);
  FILE* f = fopen("/tmp/cppide_stdin_len.txt", "r");
  CHECK(f != nullptr);
  long got = 0;
  CHECK(fscanf(f, "%ld", &got) == 1);
  fclose(f);
  CHECK(got == static_cast<long>(2u << 20));
  remove("/tmp/cppide_stdin_len.txt");
}

// ----------------------------------------------------------- 并发压力

// TSan 在本机不可用(见 wave2-proc.md),这两条用例是它的替代品:用真实并发把
// "多线程同时 fork" 与 "submit/killInFlight/析构 互相竞争" 反复撞出来。
static void t_concurrent_runprocess() {
  Case c("4 线程并发 runProcess × 25", 20000);
  int before = fdCount();
  std::atomic<int> ok{0};
  std::vector<std::thread> ths;
  for (int t = 0; t < 4; ++t) {
    ths.emplace_back([&, t] {
      for (int i = 0; i < 25; ++i) {
        ProcSpec s = shSpec("printf T" + std::to_string(t), 5000);
        ProcResult r = runProcess(s);
        // 关键:另一个线程的 fork 绝不能把我们的管道写端漏进它的子进程,
        // 否则这里就会读不到 EOF -> 卡到超时。
        if (r.outcome == ProcOutcome::Ok && r.stdout_text == "T" + std::to_string(t))
          ok.fetch_add(1);
      }
    });
  }
  for (auto& th : ths) th.join();
  CHECK(ok.load() == 100);
  int after = fdCount();
  if (before >= 0 && after >= 0) CHECK(after == before);
  CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
}

static void t_runner_race_stress() {
  Case c("Runner:submit/kill/析构 竞争 ×40", 25000);
  for (int iter = 0; iter < 40; ++iter) {
    Mailbox<AppEvent> mb;
    std::atomic<bool> stop{false};
    {
      Runner rn(&mb);
      // 一个线程不停按 Esc,另一边不停 submit,主线程随即析构
      std::thread killer([&] {
        while (!stop.load()) {
          rn.killInFlight();
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
      });
      for (int i = 0; i < 4; ++i) {
        rn.submit(shSpec("while :; do :; done", 30000), JobKind::Run);
        rn.submit(shSpec("printf x", 30000), JobKind::Run);
      }
      if (iter % 3 == 0) rn.cancelQueued();
      std::this_thread::sleep_for(std::chrono::milliseconds(iter % 7));
      stop.store(true);
      killer.join();
    }  // 析构:必须干净退出,绝不死锁、绝不等 30 秒
    AppEvent ev;
    while (mb.tryPop(ev)) {
    }
  }
  // 一轮竞争之后不能留下任何子进程
  for (int i = 0; i < 300; ++i) {
    pid_t r = waitpid(-1, nullptr, WNOHANG);
    if (r == -1 && errno == ECHILD) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
}

// ----------------------------------------------------------- 资源泄漏总检查

static void t_no_fd_leak_500() {
  Case c("500 次 spawn:无 fd 泄漏/无僵尸", 30000);
  ProcSpec s;
  s.prog = "true";
  s.timeout_ms = 5000;
  ProcResult warm = runProcess(s);
  CHECK(warm.outcome == ProcOutcome::Ok);
  int before = fdCount();
  for (int i = 0; i < 500; ++i) {
    ProcResult r = runProcess(s);
    CHECK(r.outcome == ProcOutcome::Ok);
    CHECK(r.exit_code == 0);
  }
  int after = fdCount();
  if (before >= 0 && after >= 0) {
    printf("(fd %d->%d) ", before, after);
    CHECK(after == before);
  }
  // 没有任何子进程残留(僵尸会让 waitpid 返回 >0)
  CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
}

// -------------------------------------------------------------------- main

int main() {
  signal(SIGALRM, alarmHandler);
  alarm(90);  // 兜底:任何挂死都会在这里被打断并报错
  int64_t t0 = nowMs();
  int fd0 = fdCount();
  printf("test_proc 开始(fd=%d)\n", fd0);

  t_pure_helpers();
  t_echo_hello();
  t_argv_no_shell();
  t_exit_code();
  t_signaled();
  t_no_core_dump();
  t_empty_stdin_eof();
  t_no_timeout_limit();
  t_timeout_busyloop();
  t_timeout_ignores_term();
  t_closed_stdio_then_loop();
  t_closed_stdio_but_reads_stdin();
  t_truncate_flags_only();
  t_huge_output_hard_kill();
  t_huge_output_dev_zero();
  t_stdin_early_exit();
  t_stdin_roundtrip();
  t_interleaved_out_err();
  t_grandchild_killed();
  t_spawn_failed();
  t_bad_cwd();
  t_good_cwd();
  t_env_extra();
  t_runner_serial();
  t_runner_compile_kind();
  t_runner_kill_inflight();
  t_runner_cancel_queued();
  t_runner_dtor_inflight();
  t_runner_no_mailbox();
  t_runner_empty_dtor();
  t_concurrent_runprocess();
  t_runner_race_stress();
  t_no_fd_leak_500();

  g_case = "收尾";
  int fd1 = fdCount();
  if (fd0 >= 0 && fd1 >= 0) CHECK(fd1 == fd0);
  CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);

  int64_t total = nowMs() - t0;
  printf("test_proc 通过:%d 个用例 / %d 处断言 / 总耗时 %lldms(fd=%d)\n", g_cases, g_checks,
         (long long)total, fd1);
  CHECK(total < 60000);  // 硬性要求:整份测试 60 秒内跑完
  return 0;
}

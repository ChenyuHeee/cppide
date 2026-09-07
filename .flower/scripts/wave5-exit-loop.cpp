// desc: pty 里反复"启动 cppide -> Ctrl-X (-> y) -> 退出",统计有没有卡住不退的
//
// 用法: wave5-exit-loop <轮数> <每轮超时ms> <prog> [args...]
//   轮数为偶数时:前一半跑"干净缓冲区 + Ctrl-X"(不需要确认),
//                 后一半跑"打一个字变脏 + Ctrl-X + y"(走确认分支)。
//
// 判据:每轮从"发出 Ctrl-X"到"子进程被 waitpid 收掉"的毫秒数。
//   超过 <每轮超时ms> 就 SIGKILL 并记一次 STUCK,同时打印当时 /proc/<pid>/wchan
//   与 /proc/<pid>/status 的 State/Threads —— 这正是定位"卡在哪个 join"的证据。
//
// 退出码:0 = 一次都没卡;1 = 有卡住的。
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 排空 pty 最多 ms 毫秒;对端关闭返回 false(把剩余时间睡掉,别提前返回)。
bool drain(int fd, int ms, std::string* sink) {
  char buf[8192];
  int left = ms;
  bool alive = true;
  while (left > 0) {
    struct pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    const int step = left > 20 ? 20 : left;
    const int r = ::poll(&p, 1, step);
    left -= step;
    if (r <= 0) continue;
    const ssize_t got = ::read(fd, buf, sizeof(buf));
    if (got > 0) {
      if (sink) sink->append(buf, static_cast<size_t>(got));
    } else if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
      alive = false;
      if (left > 0) ::usleep(static_cast<unsigned>(left) * 1000);
      break;
    }
  }
  return alive;
}

std::string readProcFile(pid_t pid, const char* what) {
  std::ostringstream path;
  path << "/proc/" << pid << "/" << what;
  std::ifstream f(path.str());
  if (!f) return "(读不到)";
  std::ostringstream os;
  os << f.rdbuf();
  std::string s = os.str();
  while (!s.empty() && (s.back() == '\n' || s.back() == '\0')) s.pop_back();
  return s;
}

// 卡住时的现场:wchan(内核在等什么)+ State/Threads(还剩几个线程没 join)。
std::string snapshot(pid_t pid) {
  std::string out = "wchan=" + readProcFile(pid, "wchan");
  const std::string st = readProcFile(pid, "status");
  for (const char* key : {"State:", "Threads:"}) {
    const size_t p = st.find(key);
    if (p == std::string::npos) continue;
    const size_t e = st.find('\n', p);
    out += " | " + st.substr(p, (e == std::string::npos ? st.size() : e) - p);
  }
  // 每个线程的 wchan:定位"卡在哪个 join"的关键证据。
  std::ostringstream taskdir;
  taskdir << "/proc/" << pid << "/task";
  out += " | 线程:";
  if (FILE* pp = ::popen(("ls " + taskdir.str() + " 2>/dev/null").c_str(), "r")) {
    char line[64];
    while (std::fgets(line, sizeof(line), pp)) {
      std::string tid(line);
      while (!tid.empty() && (tid.back() == '\n' || tid.back() == ' ')) tid.pop_back();
      if (tid.empty()) continue;
      std::ifstream f(taskdir.str() + "/" + tid + "/wchan");
      std::string w;
      if (f) std::getline(f, w);
      std::ifstream f2(taskdir.str() + "/" + tid + "/comm");
      std::string c;
      if (f2) std::getline(f2, c);
      out += " [" + tid + " " + c + " " + (w.empty() ? "-" : w) + "]";
    }
    ::pclose(pp);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "用法: %s <轮数> <每轮超时ms> <prog> [args...]\n", argv[0]);
    return 64;
  }
  const int rounds = std::atoi(argv[1]);
  const int budget_ms = std::atoi(argv[2]);
  std::vector<char*> child_argv;
  for (int i = 3; i < argc; ++i) child_argv.push_back(argv[i]);
  child_argv.push_back(nullptr);

  ::signal(SIGCHLD, SIG_DFL);
  ::signal(SIGPIPE, SIG_IGN);

  int stuck = 0, bad_rc = 0;
  std::vector<int> times;
  times.reserve(static_cast<size_t>(rounds));

  for (int it = 0; it < rounds; ++it) {
    const bool dirty_variant = (it >= rounds / 2);

    struct winsize ws{};
    ws.ws_row = 24;
    ws.ws_col = 80;
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
      std::perror("forkpty");
      return 65;
    }
    if (pid == 0) {
      ::setenv("TERM", "xterm-256color", 1);
      ::setenv("LANG", "C.UTF-8", 1);
      ::unsetenv("LC_ALL");
      // 绝不读到本机真实配置/真实 key。
      ::setenv("CPPIDE_CONFIG", "/nonexistent/cppide-exit-loop.json", 1);
      ::unsetenv("CPPIDE_API_KEY");
      ::unsetenv("DEEPSEEK_API_KEY");
      ::execvp(child_argv[0], child_argv.data());
      ::_exit(127);
    }

    std::string out;
    drain(master, 500, &out);      // 等首帧

    if (dirty_variant) {
      const char* typed = "x";
      (void)!::write(master, typed, 1);
      drain(master, 150, &out);
    }
    // 可选:按 Ctrl-X 之前先喂一串按键 + 等一会儿。
    // 用来构造"退出时还有在飞的 AI 请求 / 编译作业"这种最容易卡 join 的场景。
    // 例:PREKEYS=$'\x14int a' PREWAIT=1500  (\x14 = Ctrl-T 切到写代码模式)
    if (const char* pre = std::getenv("CPPIDE_EXIT_LOOP_PREKEYS")) {
      const size_t n = std::strlen(pre);
      if (n) (void)!::write(master, pre, n);
      const char* pw = std::getenv("CPPIDE_EXIT_LOOP_PREWAIT");
      drain(master, pw ? std::atoi(pw) : 800, &out);
    }

    const int64_t t0 = nowMs();
    const char ctrl_x = 24;
    (void)!::write(master, &ctrl_x, 1);
    // 'y' 无条件补一发:缓冲区脏时它是"确认退出",干净时进程已经在退了,
    // 这一次 write 落到已关闭的 pty 上不会有任何副作用。
    // ★ 踩过的坑:PREKEYS 里只要打了字,缓冲区就是脏的,"干净变体"也会弹确认框。
    //   不补这发 y,harness 会把"编辑器正老老实实等你确认"误判成"卡死"。
    {
      drain(master, 200, &out);
      const char y = 'y';
      (void)!::write(master, &y, 1);
    }

    // 等它自己退。到点先抓现场,再 SIGKILL。
    int status = 0;
    bool exited = false, killed = false;
    std::string scene;
    for (;;) {
      const pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        exited = true;
        break;
      }
      if (w < 0 && errno == ECHILD) {
        exited = true;
        break;
      }
      if (nowMs() - t0 > budget_ms) {
        scene = snapshot(pid);
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        killed = true;
        break;
      }
      drain(master, 20, &out);
    }
    const int ms = static_cast<int>(nowMs() - t0);
    times.push_back(ms);
    ::close(master);

    if (killed) {
      ++stuck;
      std::printf("第 %d 轮 [%s] STUCK %dms\n    %s\n", it + 1,
                  dirty_variant ? "脏缓冲区+y" : "干净+Ctrl-X", ms, scene.c_str());
    } else if (exited && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      ++bad_rc;
      std::printf("第 %d 轮 [%s] 退出码 %d(%dms)\n", it + 1,
                  dirty_variant ? "脏缓冲区+y" : "干净+Ctrl-X", WEXITSTATUS(status), ms);
    } else if (exited && WIFSIGNALED(status)) {
      ++bad_rc;
      std::printf("第 %d 轮 [%s] 被信号 %d 打死(%dms)\n", it + 1,
                  dirty_variant ? "脏缓冲区+y" : "干净+Ctrl-X", WTERMSIG(status), ms);
    }
    if ((it + 1) % 10 == 0) {
      std::printf("... 已跑 %d/%d 轮,卡住 %d,坏退出码 %d\n", it + 1, rounds, stuck, bad_rc);
      std::fflush(stdout);
    }
  }

  std::sort(times.begin(), times.end());
  const int n = static_cast<int>(times.size());
  long long sum = 0;
  for (int t : times) sum += t;
  std::printf("\n==== %d 轮汇总 ====\n", n);
  std::printf("Ctrl-X -> 进程真正退出的耗时:min=%dms  p50=%dms  p95=%dms  max=%dms  平均=%.1fms\n",
              n ? times.front() : -1, n ? times[static_cast<size_t>(n / 2)] : -1,
              n ? times[static_cast<size_t>(std::min(n - 1, n * 95 / 100))] : -1,
              n ? times.back() : -1, n ? static_cast<double>(sum) / n : 0.0);
  std::printf("卡住(超过 %dms 没退)的轮数:%d / %d\n", budget_ms, stuck, n);
  std::printf("非 0 退出码 / 被信号打死的轮数:%d / %d\n", bad_rc, n);
  return (stuck == 0 && bad_rc == 0) ? 0 : 1;
}

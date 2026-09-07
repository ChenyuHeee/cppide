// desc: forkpty 驱动器 —— 在伪终端里真跑 TUI 程序,按脚本喂按键并断言屏幕输出
//
// 用法: pty-drive <rows> <cols> <script> <raw_out> -- <prog> [args...]
//
// 脚本(每行一条,# 开头是注释):
//   send <转义串>     写字节到 pty(支持 \xNN \e \n \r \t \\ \s)
//   wait <ms>         排空输出 ms 毫秒
//   expect <文本>     排空直到过滤后的输出里出现该子串(默认 8000ms 超时),否则 FAIL
//   expectlast <文本> 同 expect,但只在"最近 N 字节"里找(N=200000),用于"当前屏幕"语义
//   expectnot <文本>  断言此刻过滤后的输出里没有该子串
//   mark <名字>       记一个位置:后续 expectfrom 只从这里往后找
//   expectfrom <文本> 从最后一个 mark 之后开始找
//
// 退出码: 0 全部断言通过且子进程正常退出;1 断言失败;2 子进程异常退出/超时
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string g_raw;    // pty 原始字节
std::string g_plain;  // 过滤掉转义序列后的"屏幕文本"
size_t g_mark = 0;

// 把 ESC 序列与控制字节滤掉,只留可见字节;每遇到光标移动就补一个空格,
// 避免相邻字段被粘成一个词。
void feed(const char* buf, size_t n) {
  g_raw.append(buf, n);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(buf[i]);
    if (c == 0x1b) {  // ESC:吃掉整段序列
      size_t j = i + 1;
      if (j < n && buf[j] == '[') {
        ++j;
        while (j < n) {
          const unsigned char d = static_cast<unsigned char>(buf[j]);
          if (d >= 0x40 && d <= 0x7e) break;
          ++j;
        }
      } else if (j < n && (buf[j] == ']' || buf[j] == 'P')) {
        while (j < n && static_cast<unsigned char>(buf[j]) != 0x07) ++j;
      } else if (j < n) {
        ++j;  // ESC ( B 之类的双字节
        if (j < n && (buf[j - 1] == '(' || buf[j - 1] == ')')) ++j;
        --j;
      }
      i = j;
      g_plain.push_back(' ');
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n') {
      g_plain.push_back('\n');
      continue;
    }
    if (c < 0x20 || c == 0x7f) {
      g_plain.push_back(' ');
      continue;
    }
    g_plain.push_back(static_cast<char>(c));
  }
}

// 读 pty 最多 ms 毫秒。返回 false 表示对端已关闭(子进程正在退出/已退出)。
// ★ 对端关闭后必须把剩余时间睡掉:否则 drain(100) 会瞬间返回,调用方的
//   "等 5 秒"其实一毫秒都没等到,于是把"正在退出"误判成"卡死不退"。
bool drain(int fd, int ms) {
  char buf[8192];
  int left = ms;
  bool alive = true;
  while (left > 0) {
    struct pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    const int step = left > 50 ? 50 : left;
    const int r = ::poll(&p, 1, step);
    left -= step;
    if (r <= 0) continue;
    const ssize_t got = ::read(fd, buf, sizeof(buf));
    if (got > 0) {
      feed(buf, static_cast<size_t>(got));
    } else if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
      alive = false;  // EIO = slave 已关闭
      if (left > 0) ::usleep(static_cast<useconds_t>(left) * 1000);
      break;
    }
  }
  return alive;
}

std::string unescape(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\') {
      o.push_back(s[i]);
      continue;
    }
    if (++i >= s.size()) break;
    switch (s[i]) {
      case 'n': o.push_back('\n'); break;
      case 'r': o.push_back('\r'); break;
      case 't': o.push_back('\t'); break;
      case 'e': o.push_back('\x1b'); break;
      case 's': o.push_back(' '); break;
      case '\\': o.push_back('\\'); break;
      case 'x': {
        std::string h;
        while (h.size() < 2 && i + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])))
          h.push_back(s[++i]);
        o.push_back(static_cast<char>(std::strtol(h.c_str(), nullptr, 16)));
        break;
      }
      default: o.push_back(s[i]); break;
    }
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 7) {
    std::fprintf(stderr, "用法: %s <rows> <cols> <script> <raw_out> -- <prog> [args...]\n", argv[0]);
    return 64;
  }
  const int rows = std::atoi(argv[1]);
  const int cols = std::atoi(argv[2]);
  const std::string script_path = argv[3];
  const std::string raw_out = argv[4];
  int pi = 5;
  if (std::string(argv[pi]) == "--") ++pi;
  std::vector<char*> child_argv;
  for (int i = pi; i < argc; ++i) child_argv.push_back(argv[i]);
  child_argv.push_back(nullptr);

  std::vector<std::string> script;
  {
    std::ifstream f(script_path);
    std::string ln;
    while (std::getline(f, ln)) script.push_back(ln);
  }

  struct winsize ws{};
  ws.ws_row = static_cast<unsigned short>(rows);
  ws.ws_col = static_cast<unsigned short>(cols);
  // 父环境可能把 SIGCHLD 设成 SIG_IGN(某些 shell/容器会),那样子进程会被内核
  // 自动回收,waitpid 只会返回 -1/ECHILD,于是永远等不到"退出"。必须复位。
  ::signal(SIGCHLD, SIG_DFL);
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
    ::setenv("LINES", std::to_string(rows).c_str(), 1);
    ::setenv("COLUMNS", std::to_string(cols).c_str(), 1);
    ::execvp(child_argv[0], child_argv.data());
    std::fprintf(stderr, "execvp %s 失败: %s\n", child_argv[0], std::strerror(errno));
    ::_exit(127);
  }

  int fails = 0;
  bool alive = true;
  drain(master, 400);  // 等首帧

  for (const std::string& raw_line : script) {
    std::string line = raw_line;
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t sp = line.find(' ');
    const std::string cmd = line.substr(0, sp);
    const std::string arg = (sp == std::string::npos) ? std::string() : line.substr(sp + 1);

    if (cmd == "send") {
      const std::string bytes = unescape(arg);
      if (!bytes.empty() && ::write(master, bytes.data(), bytes.size()) < 0) {
        std::printf("FAIL  write 失败: %s\n", std::strerror(errno));
        ++fails;
      }
      alive = drain(master, 250) && alive;
    } else if (cmd == "wait") {
      alive = drain(master, std::atoi(arg.c_str())) && alive;
    } else if (cmd == "mark") {
      g_mark = g_plain.size();
      std::printf("mark  %s @%zu\n", arg.c_str(), g_mark);
    } else if (cmd == "expect" || cmd == "expectfrom") {
      const size_t from = (cmd == "expectfrom") ? g_mark : 0;
      int waited = 0;
      bool found = g_plain.find(arg, from) != std::string::npos;
      while (!found && waited < 8000) {
        if (!drain(master, 100)) {
          alive = false;
          // 子进程已退出:再找一次就下结论
          found = g_plain.find(arg, from) != std::string::npos;
          break;
        }
        waited += 100;
        found = g_plain.find(arg, from) != std::string::npos;
      }
      if (found) {
        std::printf("PASS  expect \"%s\"\n", arg.c_str());
      } else {
        std::printf("FAIL  expect \"%s\"(等了 %dms)\n", arg.c_str(), waited);
        ++fails;
      }
    } else if (cmd == "expectnot") {
      if (g_plain.find(arg) == std::string::npos) {
        std::printf("PASS  expectnot \"%s\"\n", arg.c_str());
      } else {
        std::printf("FAIL  expectnot \"%s\" 竟然出现了\n", arg.c_str());
        ++fails;
      }
    } else {
      std::printf("FAIL  未知脚本命令: %s\n", cmd.c_str());
      ++fails;
    }
  }

  // 收尾:最多再等 5 秒让子进程自己退出
  int status = 0;
  bool exited = false;
  bool status_known = false;
  for (int i = 0; i < 50; ++i) {
    drain(master, 100);
    const pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      exited = true;
      status_known = true;
      break;
    }
    if (w < 0 && errno == ECHILD) {   // 已被自动回收:进程确实没了,但拿不到状态
      exited = true;
      break;
    }
  }
  if (!exited) {
    std::printf("FAIL  子进程 5 秒内没有退出,SIGKILL 之\n");
    ::kill(pid, SIGKILL);
    if (::waitpid(pid, &status, 0) == pid) status_known = true;
    ++fails;
  }
  ::close(master);

  {
    std::ofstream f(raw_out, std::ios::binary);
    f << g_plain;
    std::ofstream f2(raw_out + ".raw", std::ios::binary);
    f2 << g_raw;
  }

  std::printf("---- 子进程 ----\n");
  if (!status_known) {
    std::printf("已退出(SIGCHLD 被父环境忽略,拿不到具体状态)\n");
  } else if (WIFEXITED(status)) {
    std::printf("退出码 = %d\n", WEXITSTATUS(status));
    if (WEXITSTATUS(status) != 0) ++fails;
  } else if (WIFSIGNALED(status)) {
    std::printf("被信号 %d 终止\n", WTERMSIG(status));
    ++fails;
  }
  std::printf("pty 输出 %zu 字节(过滤后 %zu),已写入 %s\n", g_raw.size(), g_plain.size(),
              raw_out.c_str());
  std::printf("%s(失败 %d 条)\n", fails == 0 ? "== 全部通过 ==" : "== 有失败 ==", fails);
  (void)alive;
  return fails == 0 ? 0 : 1;
}

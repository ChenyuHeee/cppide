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
//
// Wave 5 新增(敌意路径实测用):
//   resize <行>x<列>          ioctl(TIOCSWINSZ) + SIGWINCH,再排空 120ms
//   resizestorm <n> <RxC> <RxC>  在两个尺寸之间来回切 n 次(每次只等 8ms,故意很凶)
//   cursorin <行> <列>        断言原始流里**最后一个** CUP 序列落在该尺寸之内
//   nocorrupt                 断言原始流里没有非法 UTF-8 / 没有裸 0x00-0x08 之类的脏字节
//   rss                       打印子进程当前 RSS 与峰值 RSS(VmHWM,KB)
//   bench <n> <转义串>        重复 n 次"发键 -> 等到输出停 30ms",报单帧耗时分位数
//   sendfile <路径>           把文件内容原样喂进 pty(大文件输入用)
//   repeat <n> <转义串>       把转义串发 n 次(每次之间不等待)
//   expectnotfrom <文本>      只在最后一个 mark 之后断言"没有出现"(判提示是否消失)
//   mark <名字>       记一个位置:后续 expectfrom 只从这里往后找
//   expectfrom <文本> 从最后一个 mark 之后开始找
//
// 退出码: 0 全部断言通过且子进程正常退出;1 断言失败;2 子进程异常退出/超时
//
// 环境变量:
//   PTY_TERM   子进程的 TERM(默认 xterm-256color)。验 8 色降级时设 PTY_TERM=xterm。
//
// ⚠ 参数名的坑(审计问题 5):第 4 个参数 <raw_out> 里写的是**过滤掉转义序列后的
//   屏幕文本**;**真正的原始字节流在 <raw_out>.raw**。要断言颜色/属性(SGR)
//   必须读 .raw 那个文件,读错会得出"全都同色"的假结论。
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <termios.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string g_raw;    // pty 原始字节
std::string g_plain;  // 过滤掉转义序列后的"屏幕文本"
size_t g_mark = 0;
pid_t g_child = -1;
int g_master = -1;

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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


// ---------------------------------------------------------------- Wave 5 辅助

// 改 pty 尺寸并补一发 SIGWINCH(有些内核不会自动给前台进程组发)。
void setWinsize(int rows, int cols) {
  struct winsize ws{};
  ws.ws_row = static_cast<unsigned short>(rows);
  ws.ws_col = static_cast<unsigned short>(cols);
  ::ioctl(g_master, TIOCSWINSZ, &ws);
  if (g_child > 0) ::kill(g_child, SIGWINCH);
}

bool parseSize(const std::string& s, int& r, int& c) {
  const size_t x = s.find('x');
  if (x == std::string::npos) return false;
  r = std::atoi(s.substr(0, x).c_str());
  c = std::atoi(s.substr(x + 1).c_str());
  return r > 0 && c > 0;
}

// 原始流里最后一个 CUP(ESC [ r ; c H)的行列。没有则返回 false。
bool lastCup(int& row, int& col) {
  bool found = false;
  for (size_t i = 0; i + 3 < g_raw.size(); ++i) {
    if (static_cast<unsigned char>(g_raw[i]) != 0x1b || g_raw[i + 1] != '[') continue;
    size_t j = i + 2;
    int a = 0, b = 0, ndig = 0;
    while (j < g_raw.size() && g_raw[j] >= '0' && g_raw[j] <= '9') {
      a = a * 10 + (g_raw[j] - '0');
      ++j;
      ++ndig;
    }
    if (j >= g_raw.size() || g_raw[j] != ';') continue;
    ++j;
    int ndig2 = 0;
    while (j < g_raw.size() && g_raw[j] >= '0' && g_raw[j] <= '9') {
      b = b * 10 + (g_raw[j] - '0');
      ++j;
      ++ndig2;
    }
    if (j >= g_raw.size() || g_raw[j] != 'H') continue;
    if (!ndig || !ndig2) continue;
    row = a;
    col = b;
    found = true;
  }
  return found;
}

// 原始流里的可见字节是否构成合法 UTF-8(转义序列/控制字节先跳过)。
// 花屏最常见的成因就是把一个宽字符切成两半输出。
bool rawUtf8Clean(std::string& why) {
  size_t i = 0;
  const size_t n = g_raw.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(g_raw[i]);
    if (c == 0x1b) {  // 跳过整段转义序列
      size_t j = i + 1;
      if (j < n && g_raw[j] == '[') {
        ++j;
        while (j < n) {
          const unsigned char d = static_cast<unsigned char>(g_raw[j]);
          if (d >= 0x40 && d <= 0x7e) break;
          ++j;
        }
        ++j;
      } else if (j < n && (g_raw[j] == ']' || g_raw[j] == 'P')) {
        while (j < n && static_cast<unsigned char>(g_raw[j]) != 0x07) ++j;
        ++j;
      } else {
        j += 2;
      }
      i = j;
      continue;
    }
    if (c < 0x80) {
      ++i;
      continue;
    }
    int need = 0;
    if ((c & 0xE0) == 0xC0) need = 1;
    else if ((c & 0xF0) == 0xE0) need = 2;
    else if ((c & 0xF8) == 0xF0) need = 3;
    else {
      char b[128];
      std::snprintf(b, sizeof(b), "偏移 %zu 处出现非法 UTF-8 首字节 0x%02X", i, c);
      why = b;
      return false;
    }
    if (i + static_cast<size_t>(need) >= n) break;  // 流尾被截断,不算错
    for (int k = 1; k <= need; ++k) {
      if ((static_cast<unsigned char>(g_raw[i + static_cast<size_t>(k)]) & 0xC0) != 0x80) {
        char b[160];
        std::snprintf(b, sizeof(b),
                      "偏移 %zu 处 UTF-8 续字节不合法(首字节 0x%02X,第 %d 个续字节 0x%02X)",
                      i, c, k, static_cast<unsigned char>(g_raw[i + static_cast<size_t>(k)]));
        why = b;
        return false;
      }
    }
    i += static_cast<size_t>(need) + 1;
  }
  return true;
}

long procKb(pid_t pid, const char* key) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.compare(0, std::strlen(key), key) == 0) {
      long v = 0;
      for (char ch : line) {
        if (ch >= '0' && ch <= '9') v = v * 10 + (ch - '0');
        else if (v) break;
      }
      return v;
    }
  }
  return -1;
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
    // TERM 默认 xterm-256color;需要验 8 色/单色降级时用环境变量 PTY_TERM 覆盖
    // (例:PTY_TERM=xterm 跑一遍,断言 ghost 走的是 A_DIM 而不是某个颜色)。
    const char* term_override = ::getenv("PTY_TERM");
    ::setenv("TERM", (term_override && *term_override) ? term_override : "xterm-256color", 1);
    ::setenv("LANG", "C.UTF-8", 1);
    ::unsetenv("LC_ALL");
    ::setenv("LINES", std::to_string(rows).c_str(), 1);
    ::setenv("COLUMNS", std::to_string(cols).c_str(), 1);
    ::execvp(child_argv[0], child_argv.data());
    std::fprintf(stderr, "execvp %s 失败: %s\n", child_argv[0], std::strerror(errno));
    ::_exit(127);
  }

  g_child = pid;
  g_master = master;

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
    } else if (cmd == "expectnotfrom") {
      // 只看最后一个 mark 之后的输出。判"某个提示消失了"必须用它:
      // expectnot 查的是整段历史,而"消失前"的那几帧里那个词本来就在。
      if (g_plain.find(arg, g_mark) == std::string::npos) {
        std::printf("PASS  expectnotfrom \"%s\"(mark 之后没有出现)\n", arg.c_str());
      } else {
        std::printf("FAIL  expectnotfrom \"%s\" 在 mark 之后仍然出现\n", arg.c_str());
        ++fails;
      }
    } else if (cmd == "expectnot") {
      if (g_plain.find(arg) == std::string::npos) {
        std::printf("PASS  expectnot \"%s\"\n", arg.c_str());
      } else {
        std::printf("FAIL  expectnot \"%s\" 竟然出现了\n", arg.c_str());
        ++fails;
      }
    } else if (cmd == "expectlast") {
      const size_t kWin = 200000;
      const size_t from = g_plain.size() > kWin ? g_plain.size() - kWin : 0;
      int waited = 0;
      bool found = g_plain.find(arg, from) != std::string::npos;
      while (!found && waited < 8000) {
        if (!drain(master, 100)) {
          alive = false;
          found = g_plain.find(arg, g_plain.size() > kWin ? g_plain.size() - kWin : 0) !=
                  std::string::npos;
          break;
        }
        waited += 100;
        found = g_plain.find(arg, g_plain.size() > kWin ? g_plain.size() - kWin : 0) !=
                std::string::npos;
      }
      if (found) std::printf("PASS  expectlast \"%s\"\n", arg.c_str());
      else {
        std::printf("FAIL  expectlast \"%s\"(等了 %dms)\n", arg.c_str(), waited);
        ++fails;
      }
    } else if (cmd == "resize") {
      int r = 0, c = 0;
      if (!parseSize(arg, r, c)) {
        std::printf("FAIL  resize 参数应为 <行>x<列>: %s\n", arg.c_str());
        ++fails;
      } else {
        setWinsize(r, c);
        alive = drain(master, 150) && alive;
        std::printf("resize  -> %dx%d\n", r, c);
      }
    } else if (cmd == "resizestorm") {
      // resizestorm <n> <RxC> <RxC>
      // resizestorm <n> <RxC> <RxC> [每步固定等待ms]
      // 不给第 4 个参数 = 尽可能快(drain 一有数据就返回,等于"手一直在拉窗口",
      // 走的是 ncurses 合并 KEY_RESIZE 的路径);给了则每步硬等这么多毫秒,
      // 保证编辑器**真的把每一次缩放都画完**,走的是逐次重排的路径。两条都要测。
      int n = 0, r1 = 0, c1 = 0, r2 = 0, c2 = 0, pace = 0;
      char s1[64] = {0}, s2[64] = {0};
      if (std::sscanf(arg.c_str(), "%d %63s %63s %d", &n, s1, s2, &pace) < 3 ||
          !parseSize(s1, r1, c1) || !parseSize(s2, r2, c2)) {
        std::printf("FAIL  resizestorm 用法: resizestorm <n> <RxC> <RxC> [pace_ms]\n");
        ++fails;
      } else {
        const int64_t t0 = nowMs();
        for (int i = 0; i < n; ++i) {
          if (i % 2 == 0) setWinsize(r1, c1);
          else setWinsize(r2, c2);
          if (pace > 0) {
            const int64_t until = nowMs() + pace;
            bool ok = true;
            while (nowMs() < until && ok) ok = drain(master, 5);
            if (!ok) {
              alive = false;
              std::printf("FAIL  resizestorm 第 %d 次时子进程已退出(疑似崩溃)\n", i + 1);
              ++fails;
              break;
            }
            continue;
          }
          if (!drain(master, 8)) {
            alive = false;
            std::printf("FAIL  resizestorm 第 %d 次时子进程已退出(疑似崩溃)\n", i + 1);
            ++fails;
            break;
          }
        }
        std::printf("resizestorm  %d 次 %dx%d <-> %dx%d(pace=%dms),耗时 %lldms\n", n, r1,
                    c1, r2, c2, pace, static_cast<long long>(nowMs() - t0));
      }
    } else if (cmd == "cursorin") {
      int rows_lim = 0, cols_lim = 0;
      if (std::sscanf(arg.c_str(), "%d %d", &rows_lim, &cols_lim) != 2) {
        std::printf("FAIL  cursorin 用法: cursorin <行> <列>\n");
        ++fails;
      } else {
        int r = 0, c = 0;
        if (!lastCup(r, c)) {
          std::printf("FAIL  cursorin:原始流里找不到任何 CUP 序列\n");
          ++fails;
        } else if (r >= 1 && r <= rows_lim && c >= 1 && c <= cols_lim) {
          std::printf("PASS  cursorin 最后一次定位 (%d,%d) 落在 %dx%d 之内\n", r, c,
                      rows_lim, cols_lim);
        } else {
          std::printf("FAIL  cursorin 最后一次定位 (%d,%d) 越出 %dx%d\n", r, c, rows_lim,
                      cols_lim);
          ++fails;
        }
      }
    } else if (cmd == "nocorrupt") {
      std::string why;
      if (rawUtf8Clean(why)) {
        std::printf("PASS  nocorrupt(原始流 %zu 字节全是合法 UTF-8)\n", g_raw.size());
      } else {
        std::printf("FAIL  nocorrupt: %s\n", why.c_str());
        ++fails;
      }
    } else if (cmd == "rss") {
      const long rss = procKb(pid, "VmRSS:");
      const long hwm = procKb(pid, "VmHWM:");
      std::printf("rss   %s 当前 RSS=%ldKB 峰值 RSS(VmHWM)=%ldKB\n", arg.c_str(), rss, hwm);
    } else if (cmd == "bench") {
      // bench <n> <转义串>:重复 n 次"发键 -> 等到输出静默 30ms",报单帧耗时
      int n = 0;
      size_t sp2 = arg.find(' ');
      n = std::atoi(arg.substr(0, sp2).c_str());
      const std::string keys = unescape(sp2 == std::string::npos ? std::string()
                                                                 : arg.substr(sp2 + 1));
      std::vector<int> ms;
      for (int i = 0; i < n && alive; ++i) {
        if (!keys.empty()) (void)!::write(master, keys.data(), keys.size());
        const int64_t t0 = nowMs();
        int64_t last_byte = t0;
        char b[8192];
        for (;;) {
          struct pollfd p{};
          p.fd = master;
          p.events = POLLIN;
          const int pr = ::poll(&p, 1, 10);
          if (pr > 0) {
            const ssize_t got = ::read(master, b, sizeof(b));
            if (got > 0) {
              feed(b, static_cast<size_t>(got));
              last_byte = nowMs();
              continue;
            }
            alive = false;
            break;
          }
          if (nowMs() - last_byte >= 30) break;      // 输出静默 = 这一帧画完了
          if (nowMs() - t0 > 3000) break;            // 上限
        }
        ms.push_back(static_cast<int>(last_byte - t0));
      }
      std::sort(ms.begin(), ms.end());
      if (!ms.empty()) {
        long long sum = 0;
        for (int v : ms) sum += v;
        std::printf("bench %d 次 keys=%s : min=%dms p50=%dms p95=%dms max=%dms 平均=%.1fms\n",
                    static_cast<int>(ms.size()), arg.substr(0, sp2).c_str(), ms.front(),
                    ms[ms.size() / 2], ms[std::min(ms.size() - 1, ms.size() * 95 / 100)],
                    ms.back(), static_cast<double>(sum) / static_cast<double>(ms.size()));
      }
    } else if (cmd == "sendfile") {
      std::ifstream f(arg, std::ios::binary);
      if (!f) {
        std::printf("FAIL  sendfile 读不到 %s\n", arg.c_str());
        ++fails;
      } else {
        std::string data((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        size_t off = 0;
        while (off < data.size()) {
          const size_t chunk = std::min<size_t>(4096, data.size() - off);
          if (::write(master, data.data() + off, chunk) < 0) break;
          off += chunk;
          alive = drain(master, 5) && alive;
        }
        std::printf("sendfile %s(%zu 字节)\n", arg.c_str(), off);
        alive = drain(master, 300) && alive;
      }
    } else if (cmd == "repeat") {
      int n = 0;
      const size_t sp2 = arg.find(' ');
      n = std::atoi(arg.substr(0, sp2).c_str());
      const std::string bytes = unescape(sp2 == std::string::npos ? std::string()
                                                                  : arg.substr(sp2 + 1));
      for (int i = 0; i < n; ++i) {
        if (!bytes.empty() && ::write(master, bytes.data(), bytes.size()) < 0) break;
        alive = drain(master, 12) && alive;
      }
      std::printf("repeat %d 次\n", n);
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

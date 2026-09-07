#!/usr/bin/env bash
# desc: Wave4 ui.cpp 验收:零 warning + check-headers + test_ui(O2/ASan) + pty 真渲染三档 + 终端状态恢复实测
set -u
cd /work

OUT=/tmp/wave4-ui
mkdir -p "$OUT"
PASS=0; FAIL=0
ok()  { echo "  [ ok ] $*"; PASS=$((PASS+1)); }
bad() { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }
hdr() { echo; echo "=== $* ==="; }

# ncurses 依赖(Linux 必须 -lncursesw:add_wch/get_wch 只在 w 版里)
NCLIB=-lncursesw
[ "$(uname -s)" = "Darwin" ] && NCLIB=-lncurses
# ui.cpp 依赖 ai.cpp 的 aiModeZh,而 ai.cpp 由并行 agent 维护:能编就用真的,
# 编不过就用一个只含 aiModeZh 的桩,免得别人的半成品挡住本模块的自证。
LINK_OBJ="src/util.o src/keys.o src/highlight.o src/textbuf.o src/panel.o src/editor.o
          src/config.o src/json.o src/aihttp.o src/proc.o src/build.o"

# ---------------------------------------------------------------- 1. 零 warning
hdr "1. 编译 src/ui.cpp(-Wall -Wextra 必须零 warning)"
if timeout 300 g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/ui.cpp -o "$OUT/ui.o" \
     2>"$OUT/ui-warn.log" && [ ! -s "$OUT/ui-warn.log" ]; then
  ok "ui.cpp 零 warning"
else
  bad "ui.cpp 有 warning/错误:"; sed 's/^/       /' "$OUT/ui-warn.log" | head -30
fi

hdr "2. make check-headers"
if timeout 300 make check-headers >"$OUT/check-headers.log" 2>&1; then
  ok "$(tail -1 "$OUT/check-headers.log")"
else
  bad "check-headers 失败"; tail -20 "$OUT/check-headers.log" | sed 's/^/       /'
fi

hdr "3. 依赖对象"
for f in $LINK_OBJ; do
  [ -f "$f" ] || timeout 300 make "$f" >/dev/null 2>&1
done
if timeout 300 g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/ai.cpp -o "$OUT/ai.o" \
     >"$OUT/ai.log" 2>&1; then
  AIOBJ="$OUT/ai.o"; ok "src/ai.cpp 可编译,链接真实 aiModeZh"
else
  cat > "$OUT/stub_ai.cpp" <<'EOF'
#include "ai.h"
const char* aiModeZh(AiMode m) { return m == AiMode::Code ? "写代码模式" : "练习模式"; }
EOF
  timeout 120 g++ -std=c++17 -O2 -Isrc -c "$OUT/stub_ai.cpp" -o "$OUT/ai.o"
  AIOBJ="$OUT/ai.o"; ok "src/ai.cpp 暂不可编译 -> 用 aiModeZh 桩链接(仅影响本脚本)"
fi

# ---------------------------------------------------------------- 4. test_ui
hdr "4. tests/test_ui.cpp(纯逻辑) —— O2"
if timeout 300 g++ -std=c++17 -O2 -Wall -Wextra -Isrc -o "$OUT/test_ui" \
     tests/test_ui.cpp "$OUT/ui.o" "$AIOBJ" $LINK_OBJ $NCLIB -lcurl -lpthread \
     >"$OUT/test_ui-build.log" 2>&1; then
  if timeout 120 "$OUT/test_ui" >"$OUT/test_ui.log" 2>&1; then
    ok "$(tail -1 "$OUT/test_ui.log")"
  else
    bad "test_ui(O2)失败"; tail -20 "$OUT/test_ui.log" | sed 's/^/       /'
  fi
else
  bad "test_ui 编译失败"; tail -20 "$OUT/test_ui-build.log" | sed 's/^/       /'
fi

hdr "5. tests/test_ui.cpp —— ASan + UBSan"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g"
if timeout 600 g++ -std=c++17 $SAN -Wall -Wextra -Isrc -o "$OUT/test_ui_san" \
     tests/test_ui.cpp src/ui.cpp src/ai.cpp src/util.cpp src/keys.cpp src/highlight.cpp \
     src/textbuf.cpp src/panel.cpp src/editor.cpp src/config.cpp src/json.cpp \
     src/aihttp.cpp src/proc.cpp src/build.cpp $NCLIB -lcurl -lpthread \
     >"$OUT/test_ui-san-build.log" 2>&1; then
  if UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
     timeout 300 "$OUT/test_ui_san" >"$OUT/test_ui-san.log" 2>&1; then
    ok "ASan/UBSan 通过:$(tail -1 "$OUT/test_ui-san.log")"
  else
    bad "ASan/UBSan 报错"; tail -30 "$OUT/test_ui-san.log" | sed 's/^/       /'
  fi
else
  bad "ASan 版编译失败"; tail -20 "$OUT/test_ui-san-build.log" | sed 's/^/       /'
fi

# ---------------------------------------------------------------- 6. pty 驱动
hdr "6. pty 真渲染 + 终端状态恢复"
cat > "$OUT/pty_drv.cpp" <<'CPPEOF'
// pty 驱动:在真伪终端里跑 Ui 的初始化 / 渲染 / 收尾,并回读屏幕做机器断言。
//   render <rows> <cols> <frame> <snap-file>   子进程渲染一帧并回读屏幕
//   restore <mode>                            终端状态恢复实测(raw-exit/atexit/segv/terminate)
// 回读用 ncurses 自己的 win_wch:拿到的是"终端上真正显示的字符与颜色对",
// 比解析 ANSI 转义序列可靠得多,也让"练习模式行号槽染色"这类要求可机器验证。
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#define NCURSES_NOMACROS 1
#include <curses.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "app.h"
#include "build.h"
#include "ui.h"
#include "util.h"

// ---------------------------------------------------------- 假 AppModel
struct Fake {
  Config cfg;
  Editor ed{cfg};
  Panel panels[4] = {Panel(PanelId::Compile), Panel(PanelId::Run), Panel(PanelId::Ai),
                     Panel(PanelId::Input)};
  StdinBuffer stdin_buf;
  CompileOutcome co;

  Fake() {
    cfg.tab_width = 4;
    cfg.show_line_numbers = true;
    cfg.panel_height = 8;
    std::vector<std::string> src = {
        "#include <bits/stdc++.h>",
        "using namespace std;",
        "// 读入 n 个整数并求和 —— 这一行是中文注释,用来测宽字符排版",
        "int main() {",
        "    int n; cin >> n;",
        "    vector<long long> a(n);   // TODO: 处理 n == 0 的情况",
        "    for (int i = 0; i < n; i++) cin >> a[i];",
        "    long long s = 0;",
        "    /* 块注释:累加",
        "       第二行 */",
        "    for (long long v : a) s += v;",
        "    cout << \"总和 = \" << s << '\\n';",
        "    return 0;",
        "}"};
    ed.buffer().reset(src);
    ed.buffer().setPath("/tmp/主程序.cpp");
    ed.refreshLang();
    ed.setCursor(Pos{4, (int)std::strlen("    int n; cin >> n;")});

    panels[0].append("主程序.cpp:6:5: error: 'x' 未声明(注意:中文诊断需 LC_ALL=C 才不会出现)", true, 0);
    panels[0].append("主程序.cpp:9:3: warning: unused variable 'y'", true, 1);
    panels[0].append("-- 编译命令:c++ -O2 -std=c++17 -Wall 主程序.cpp -o /tmp/a.out");
    panels[0].setErrorCount(2);
    panels[0].setUnread(true);
    panels[1].append("13\n运行结束:退出码 0 · 用时 12ms");
    panels[1].setUnread(true);
    panels[2].append("AI:先想想前缀和能不能把这题的 O(n^2) 降到 O(n)?这是练习模式,我不会直接写代码给你。");
    stdin_buf.setData("5\n1 2 3 4 5\n");
    co.ok = false;
    co.errors = 2;
    co.warnings = 1;
  }

  AppModel model(int frame) {
    AppModel m;
    m.cfg = &cfg;
    m.ed = &ed;
    m.panels = panels;
    m.panel_count = 4;
    m.stdin_buf = &stdin_buf;
    m.last_compile = &co;
    m.panel_visible = true;
    m.panel_height = cfg.panel_height;
    m.file_name = util::basename(ed.buffer().path());
    m.file_dirty = true;
    m.cursor_line = ed.cursor().line + 1;
    m.cursor_col = ed.cursorDisplayCol() + 1;
    m.lang_zh = "C++";
    m.build_summary_zh = "2 错误 1 警告";
    m.errors = 2;
    m.warnings = 1;
    if (frame == 0) {           // 练习模式 + 面板聚焦 + 运行中
      m.ai_mode = AiMode::Practice;
      m.ai_state = AiState::Thinking;
      m.ai_state_zh = "思考中…";
      m.focus = 0;
      m.running = true;
    } else if (frame == 1) {    // 写代码模式 + 多行 ghost + 提示行
      m.ai_mode = AiMode::Code;
      m.ai_state = AiState::Idle;
      m.ai_state_zh = "就绪";
      m.focus = -1;
      m.prompt = PromptKind::GotoLine;
      m.prompt_label = "跳到行号: ";
      m.prompt_input = "128";
      m.prompt_cursor = 3;
      m.status_msg = "已保存 主程序.cpp(中文文件名)";
    } else {                    // F1 帮助浮层
      m.ai_mode = AiMode::Code;
      m.ai_state = AiState::Error;
      m.ai_state_zh = "错误";
      m.focus = -1;
      m.help_visible = true;
    }
    return m;
  }
};

// ---------------------------------------------------------- 屏幕回读
static char pairTag(short p) {
  if (p <= 0) return '.';
  if (p < 10) return static_cast<char>('0' + p);
  if (p < 36) return static_cast<char>('a' + (p - 10));
  return '?';
}

static int childRender(int frame, const char* snap_path) {
  std::setlocale(LC_ALL, "");   // 约定 §7:必须早于 initscr
  Fake fk;
  Ui ui;
  std::string err;
  if (!ui.init(err)) { std::fprintf(stderr, "init 失败: %s\n", err.c_str()); return 3; }

  AppModel m = fk.model(frame);
  const Layout& L = ui.relayout(m);
  // 模拟 app.cpp 的职责:把布局尺寸交给 Editor / Panel
  fk.ed.setViewSize(L.editor.w, L.editor.h);
  fk.ed.ensureCursorVisible();
  for (int i = 0; i < 4; ++i) fk.panels[i].setViewSize(L.panel.w, L.panel.h);
  fk.stdin_buf.setViewSize(L.panel.w, L.panel.h);
  if (frame == 1) {   // 多行 ghost(只有写代码模式才可能有)
    Ghost g;
    g.text = "for (int i = 0; i < n; i++) {\n        sum += a[i];\n        cnt++;\n    }";
    g.anchor = fk.ed.cursor();
    g.sink = AiSink::GhostText;
    g.complete = true;
    g.gen = fk.ed.expectedGen();
    fk.ed.setGhost(g);
  }
  m = fk.model(frame);
  ui.draw(m);

  // 回读整屏(必须在 endwin 之前)
  const int rows = ui.rows(), cols = ui.cols();
  std::string text, pairs;
  std::vector<short> pgrid(static_cast<size_t>(rows) * static_cast<size_t>(cols), -1);
  int bad_cells = 0;
  for (int y = 0; y < rows; ++y) {
    std::string tline, pline;
    for (int x = 0; x < cols; ++x) {
      if (::wmove(stdscr, y, x) == ERR) { ++bad_cells; continue; }
      cchar_t cc;
      if (::win_wch(stdscr, &cc) == ERR) { ++bad_cells; continue; }
      wchar_t wch[CCHARW_MAX + 1] = {0};
      attr_t at = 0;
      short pr = 0;
      if (::getcchar(&cc, wch, &at, &pr, nullptr) == ERR) { ++bad_cells; continue; }
      pline += pairTag(pr);
      pgrid[static_cast<size_t>(y) * static_cast<size_t>(cols) + static_cast<size_t>(x)] = pr;
      if (wch[0] == 0) { tline += ' '; continue; }
      std::wstring ws(wch);
      tline += util::fromWide(ws);
      // 宽字符在 ncurses 里占两格:第二格回读到的是同一个字符,
      // 直接拼会把中文重复一遍。跳过它(并给颜色对图补一格)。
      const int cw = util::charDisplayWidth(static_cast<uint32_t>(wch[0]));
      if (cw == 2 && x + 1 < cols) { pline += pairTag(pr); ++x; }
    }
    text += tline; text += "\n";
    pairs += pline; pairs += "\n";
  }
  const int status_pair_x0 = rows - 1;
  ui.shutdown();

  // 机器断言:模式指示器与练习模式的行号槽染色
  int rc = 0;
  if (FILE* f = std::fopen(snap_path, "w")) {
    std::fprintf(f, "### frame=%d  %dx%d  bad_cells=%d\n", frame, cols, rows, bad_cells);
    std::fprintf(f, "--- 屏幕文本 ---\n%s", text.c_str());
    std::fprintf(f, "--- 颜色对(0-9a-z,'.'=默认对)---\n%s", pairs.c_str());
    std::fprintf(f, "--- 布局 ---\n");
    std::fprintf(f, "gutter y=%d x=%d h=%d w=%d\n", L.gutter.y, L.gutter.x, L.gutter.h, L.gutter.w);
    std::fprintf(f, "editor y=%d x=%d h=%d w=%d\n", L.editor.y, L.editor.x, L.editor.h, L.editor.w);
    std::fprintf(f, "tabs   y=%d x=%d h=%d w=%d\n", L.tabs.y, L.tabs.x, L.tabs.h, L.tabs.w);
    std::fprintf(f, "panel  y=%d x=%d h=%d w=%d\n", L.panel.y, L.panel.x, L.panel.h, L.panel.w);
    std::fprintf(f, "prompt y=%d x=%d h=%d w=%d\n", L.prompt.y, L.prompt.x, L.prompt.h, L.prompt.w);
    std::fprintf(f, "status y=%d x=%d h=%d w=%d\n", L.status.y, L.status.x, L.status.h, L.status.w);
    std::fprintf(f, "theme  has_color=%d colors=%d pairs=%d wide_ok=%d status_row=%d\n",
                 (int)ui.theme().has_color, ui.theme().colors, ui.theme().pairs,
                 (int)ui.theme().wide_ok, status_pair_x0);
    std::fclose(f);
  } else {
    rc = 4;
  }
  if (bad_cells != 0) rc = 5;

  // ★ 机器断言:模式指示器的"颜色"这一重冗余(文字/位置由快照人工核对)
  //   练习模式 -> 状态栏最左格 P_StatusPractice + 行号槽整体 P_LineNoPractice
  //   写代码模式 -> P_StatusCode + P_LineNo
  if (ui.theme().has_color && !L.status.empty() && !L.gutter.empty()) {
    const bool practice = (frame == 0);
    const short want_badge = practice ? Theme::P_StatusPractice : Theme::P_StatusCode;
    const short want_gut = practice ? Theme::P_LineNoPractice : Theme::P_LineNo;
    const size_t badge_i = static_cast<size_t>(rows - 1) * static_cast<size_t>(cols);
    if (pgrid[badge_i] != want_badge) {
      std::fprintf(stderr, "模式徽章颜色对错误: got=%d want=%d\n", pgrid[badge_i], want_badge);
      rc = 6;
    }
    // frame 2 是 F1 帮助浮层:它本来就会盖住行号槽,这条断言不适用
    for (int y = L.gutter.y; frame != 2 && y < L.gutter.y + L.gutter.h; ++y)
      for (int x = L.gutter.x; x < L.gutter.x + L.gutter.w; ++x) {
        const short got = pgrid[static_cast<size_t>(y) * static_cast<size_t>(cols) +
                                static_cast<size_t>(x)];
        // ghost 覆盖行会把行号槽换成 ghost 色,那是设计如此,跳过
        if (got == Theme::P_Ghost) continue;
        if (got != want_gut) {
          std::fprintf(stderr, "行号槽颜色对错误 (%d,%d): got=%d want=%d\n", y, x, got, want_gut);
          rc = 7;
          y = L.gutter.y + L.gutter.h;
          break;
        }
      }
  }
  return rc;
}

static int childRestore(const char* mode) {
  std::setlocale(LC_ALL, "");
  Ui ui;
  std::string err;
  if (!ui.init(err)) return 3;
  ::wmove(stdscr, 0, 0);
  const wchar_t* w = L"raw+noecho 中…";
  ::waddnwstr(stdscr, w, 14);
  ::wrefresh(stdscr);
  if (!std::strcmp(mode, "raw-exit")) ::_exit(0);   // 故意不 endwin,也绕过 atexit
  if (!std::strcmp(mode, "atexit")) std::exit(0);   // 只靠 atexit 网
  if (!std::strcmp(mode, "segv")) { ::raise(SIGSEGV); return 9; }
  if (!std::strcmp(mode, "terminate")) throw std::runtime_error("boom");
  ui.shutdown();
  return 0;
}

// ---------------------------------------------------------- 真读键
// 走完整输入路径:raw + keypad + escdelay + wget_wch + Alt 的 Esc 前缀合成。
// 期望值直接写在这里,对不上就非零退出 —— 这是唯一能证明"终端真的按下这些键
// 时上层拿到的是约定键码"的办法。
static int childKeys(const char* snap_path) {
  std::setlocale(LC_ALL, "");
  Ui ui;
  std::string err;
  if (!ui.init(err)) return 3;
  struct Exp { int key; const char* what; };
  const Exp exp[] = {
      {Char('a'),        "a"},
      {KEY_LEFT,         "<-"},
      {Alt(KEY_LEFT),    "Alt-<-"},
      {Alt(Char('1')),   "Alt-1"},
      {Ctrl('O'),        "Ctrl-O"},
      {Char(0x4F60),     "你"},
      {13,               "Enter"},
      {27,               "bare-Esc"},
      {Char('z'),        "z"},
  };
  const int n = (int)(sizeof(exp) / sizeof(exp[0]));
  std::vector<int> got;
  for (int i = 0; i < n; ++i) {
    int k = kKeyNone;
    for (int tries = 0; tries < 40 && k == kKeyNone; ++tries) k = ui.getKeyBlockingFor(60);
    got.push_back(k);
  }
  std::vector<std::string> desc;
  for (int k : got) desc.push_back(ui.describeKey(k));
  ui.shutdown();

  int rc = 0;
  if (FILE* f = std::fopen(snap_path, "w")) {
    for (int i = 0; i < n; ++i) {
      const bool okk = (got[(size_t)i] == exp[i].key);
      if (!okk) rc = 8;
      std::fprintf(f, "%-10s 期望=0x%08X 实到=0x%08X %s  | %s\n", exp[i].what,
                   (unsigned)exp[i].key, (unsigned)got[(size_t)i], okk ? "OK" : "MISMATCH",
                   desc[(size_t)i].c_str());
    }
    std::fclose(f);
  } else {
    rc = 4;
  }
  return rc;
}

// ---------------------------------------------------------- 父进程
typedef void (*FeedFn)(int master);

static int runInPty(int rows, int cols, char* const* argv_child, std::string& out,
                    int& wstatus, struct termios& tio_after, int& tio_ok,
                    FeedFn feed = nullptr) {
  struct winsize ws;
  std::memset(&ws, 0, sizeof(ws));
  ws.ws_row = static_cast<unsigned short>(rows);
  ws.ws_col = static_cast<unsigned short>(cols);
  int master = -1;
  const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
  if (pid < 0) return -1;
  if (pid == 0) {
    ::execv("/proc/self/exe", argv_child);
    ::_exit(127);
  }
  if (feed) feed(master);
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(master, buf, sizeof(buf));
    if (n > 0) { out.append(buf, static_cast<size_t>(n)); continue; }
    if (n < 0 && errno == EINTR) continue;
    break;   // 0 = EOF,EIO = 从进程已退出并关闭了 slave
  }
  ::waitpid(pid, &wstatus, 0);
  tio_ok = ::tcgetattr(master, &tio_after);
  ::close(master);
  return 0;
}

static std::string escape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == 0x1b) o += "<ESC>";
    else if (c == '\r') o += "<CR>";
    else if (c == '\n') o += "<LF>\n";
    else if (c < 0x20 || c == 0x7f) { char b[8]; std::snprintf(b, sizeof b, "<%02X>", c); o += b; }
    else o += static_cast<char>(c);
  }
  return o;
}

// 按真实终端的字节序列喂键。分批 + 停顿:裸 Esc 必须等 escdelay 过期才成立,
// 与它后面的键混在一次 write 里就会被合成 Alt(下一个键)。
static void feedKeys(int master) {
  // ★ 方向键必须用 SS3 形式 "\EOD":keypad(TRUE) 会让 ncurses 送出 smkx,
  //   把终端切到 application cursor key 模式,此时 xterm 发的是 ESC O D 而不是
  //   ESC [ D(xterm-256color 的 kcub1 就是 \EOD)。用 CSI 形式喂进来 ncurses
  //   匹配不上,会退化成"裸 Esc + '[' + 'D'" —— 那是喂错了,不是 ui.cpp 的问题。
  const char* batch1 = "a"            // 可打印
                       "\x1bOD"        // KEY_LEFT
                       "\x1b\x1bOD"    // Alt-←(Esc 前缀 + KEY_LEFT)
                       "\x1b" "1"      // Alt-1
                       "\x0f"          // Ctrl-O
                       "\xe4\xbd\xa0"  // 你(UTF-8)
                       "\r";           // Enter
  (void)!::write(master, batch1, std::strlen(batch1));
  ::usleep(300 * 1000);
  (void)!::write(master, "\x1b", 1);    // 裸 Esc:后面 300ms 内没有别的字节
  ::usleep(300 * 1000);
  (void)!::write(master, "z", 1);
  ::usleep(100 * 1000);
}

int main(int argc, char** argv) {
  if (argc >= 2 && !std::strcmp(argv[1], "child-render"))
    return childRender(std::atoi(argv[2]), argv[3]);
  if (argc >= 2 && !std::strcmp(argv[1], "child-restore")) return childRestore(argv[2]);
  if (argc >= 3 && !std::strcmp(argv[1], "child-keys")) return childKeys(argv[2]);

  if (argc >= 3 && !std::strcmp(argv[1], "keys")) {
    char a0[] = "pty_drv", a1[] = "child-keys";
    std::string sp(argv[2]);
    char* av[] = {a0, a1, const_cast<char*>(sp.c_str()), nullptr};
    std::string out; int st = 0, tio_ok = -1; struct termios t;
    if (runInPty(24, 80, av, out, st, t, tio_ok, feedKeys) != 0) { std::puts("forkpty 失败"); return 2; }
    std::printf("keys: exited=%d code=%d signaled=%d sig=%d\n", WIFEXITED(st),
                WIFEXITED(st) ? WEXITSTATUS(st) : -1, WIFSIGNALED(st),
                WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
  }

  if (argc >= 5 && !std::strcmp(argv[1], "render")) {
    const int rows = std::atoi(argv[2]), cols = std::atoi(argv[3]);
    const int frame = std::atoi(argv[4]);
    const char* snap = argv[5];
    char a0[] = "pty_drv", a1[] = "child-render";
    char a2[16]; std::snprintf(a2, sizeof a2, "%d", frame);
    std::string sp(snap);
    char* av[] = {a0, a1, a2, const_cast<char*>(sp.c_str()), nullptr};
    std::string out; int st = 0, tio_ok = -1; struct termios t;
    if (runInPty(rows, cols, av, out, st, t, tio_ok) != 0) { std::puts("forkpty 失败"); return 2; }
    std::printf("render %dx%d frame=%d: exited=%d code=%d signaled=%d sig=%d bytes=%zu\n",
                cols, rows, frame, WIFEXITED(st), WIFEXITED(st) ? WEXITSTATUS(st) : -1,
                WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0, out.size());
    std::string raw = std::string(snap) + ".ansi";
    if (FILE* f = std::fopen(raw.c_str(), "w")) { std::fputs(escape(out).c_str(), f); std::fclose(f); }
    return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 1;
  }

  if (argc >= 3 && !std::strcmp(argv[1], "restore")) {
    char a0[] = "pty_drv", a1[] = "child-restore";
    std::string mode(argv[2]);
    char* av[] = {a0, a1, const_cast<char*>(mode.c_str()), nullptr};
    std::string out; int st = 0, tio_ok = -1; struct termios t;
    std::memset(&t, 0, sizeof(t));
    if (runInPty(24, 80, av, out, st, t, tio_ok) != 0) { std::puts("forkpty 失败"); return 2; }
    const int icanon = (tio_ok == 0) && (t.c_lflag & ICANON) ? 1 : 0;
    const int echo = (tio_ok == 0) && (t.c_lflag & ECHO) ? 1 : 0;
    std::printf("restore %-10s exited=%d code=%d signaled=%d sig=%d  tcgetattr=%d "
                "ICANON=%d ECHO=%d\n",
                mode.c_str(), WIFEXITED(st), WIFEXITED(st) ? WEXITSTATUS(st) : -1,
                WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0, tio_ok, icanon, echo);
    if (mode == "raw-exit") return (icanon == 0 && echo == 0) ? 0 : 1;   // 对照组:必须"没恢复"
    return (icanon == 1 && echo == 1) ? 0 : 1;                          // 其余必须已恢复
  }
  std::puts("用法: pty_drv render <rows> <cols> <frame> <snap> | pty_drv restore <mode>");
  return 2;
}
CPPEOF

if timeout 600 g++ -std=c++17 -O2 -Wall -Wextra -Isrc -o "$OUT/pty_drv" "$OUT/pty_drv.cpp" \
     "$OUT/ui.o" "$AIOBJ" $LINK_OBJ $NCLIB -lcurl -lpthread -lutil \
     >"$OUT/pty-build.log" 2>&1; then
  ok "pty 驱动编译通过"
else
  bad "pty 驱动编译失败"; tail -30 "$OUT/pty-build.log" | sed 's/^/       /'
fi

if [ -x "$OUT/pty_drv" ]; then
  export TERM=${TERM_FOR_TEST:-xterm-256color}
  for size in "24 80" "16 60" "5 20"; do
    set -- $size
    R=$1; C=$2
    for frame in 0 1 2; do
      snap="$OUT/snap-${C}x${R}-f${frame}.txt"
      if timeout 60 "$OUT/pty_drv" render "$R" "$C" "$frame" "$snap" \
           >>"$OUT/pty-run.log" 2>&1; then
        ok "渲染 ${C}x${R} frame=$frame -> $(basename "$snap")"
      else
        bad "渲染 ${C}x${R} frame=$frame 失败"; tail -3 "$OUT/pty-run.log" | sed 's/^/       /'
      fi
    done
  done
  hdr "7. pty 里真按键(raw+keypad+escdelay+Alt 合成 全路径)"
  if timeout 60 "$OUT/pty_drv" keys "$OUT/keys.txt" >>"$OUT/keys-run.log" 2>&1; then
    ok "9 个键全部归一化正确(见 $OUT/keys.txt)"
  else
    bad "按键归一化有不匹配:"; sed 's/^/       /' "$OUT/keys.txt" 2>/dev/null | head -12
  fi

  hdr "8. 终端状态恢复(pty 里读 termios 的 ICANON/ECHO)"
  for mode in raw-exit atexit segv terminate; do
    if timeout 60 "$OUT/pty_drv" restore "$mode" >>"$OUT/restore.log" 2>&1; then
      ok "$(grep -- "restore $mode" "$OUT/restore.log" | tail -1)"
    else
      bad "$(grep -- "restore $mode" "$OUT/restore.log" | tail -1)"
    fi
  done
fi

hdr "SUMMARY"
echo "  passed=$PASS failed=$FAIL   产物目录:$OUT"
[ "$FAIL" -eq 0 ] || exit 1

// ui.cpp —— ncurses 渲染层(全工程唯一可以见到 curses 的翻译单元)
//
// ★ 必须在 include <curses.h> 之前 #define NCURSES_NOMACROS:
//   curses.h 把 clear()/erase()/move()/refresh()/timeout()/scroll()/border()/instr()
//   定义成函数式宏,一旦生效,panel.h 的 `int scroll() const;`、TextBuffer::erase、
//   Panel::clear 之类的成员声明会被预处理器当成宏调用而报出莫名其妙的语法错。
//   (跨模块约定 §1)因此本文件只用带 w 前缀的真函数:wmove/werase/wrefresh/
//   wtimeout/waddnwstr/wattrset,不用任何 stdscr 便捷宏。
//
// ★ 前置条件:setlocale(LC_ALL, "") 必须由 main() 在 Ui::init() 之前调用
//   (跨模块约定 §7)。本文件**不重复调用** setlocale —— 但没有它,宽字符
//   输出与 util 的宽度计算会全部按 1 列算,中文排版会整体错位。
//   单测/驱动程序需要自己在 init 前设 locale。
//
// ★ 只读契约:Ui 只通过 const AppModel& 取数据,不持有可写 TextBuffer/Editor
//   (architecture §6 结构性保证的一部分)。
//
// ★ 崩溃安全网(architecture §8.1):atexit + std::set_terminate +
//   SIGSEGV/SIGBUS/SIGABRT handler 内 endwin() 后重新 raise。
//   "把用户终端留在 raw+noecho" 是用户可感的最恶劣故障,优先级高于一切。

#define NCURSES_NOMACROS 1
#include <curses.h>

#include <algorithm>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>
#include <vector>

#include "ai.h"
#include "app.h"
#include "editor.h"
#include "highlight.h"
#include "keys.h"
#include "panel.h"
#include "textbuf.h"
#include "ui.h"
#include "util.h"

// ============================================================ 崩溃安全网
namespace {

// signal handler 只允许碰它:0 = 屏幕已归还终端,1 = ncurses 正持有终端。
volatile sig_atomic_t g_screen_live = 0;
bool g_nets_installed = false;
std::terminate_handler g_prev_terminate = nullptr;

// 幂等:重复调用只有第一次真的 endwin()。
// 只做 endwin(),不碰任何堆对象 —— 它会在 signal handler 里被调用。
void endwinOnce() {
  if (g_screen_live) {
    g_screen_live = 0;
    ::endwin();
  }
}

void onFatalSignal(int sig) {
  endwinOnce();
  // 恢复默认处置后重新 raise:保留 core dump / 正确的退出状态。
  // (signal() 是 POSIX 明确列出的 async-signal-safe 函数)
  ::signal(sig, SIG_DFL);
  ::raise(sig);
  ::_exit(128 + sig);   // 理论上到不了这里
}

void onTerminate() {
  endwinOnce();
  std::fputs("cppide: 未捕获的异常,已恢复终端状态。\n", stderr);
  if (g_prev_terminate && g_prev_terminate != onTerminate) g_prev_terminate();
  std::abort();   // SIGABRT handler 里的 endwinOnce 是幂等的
}

void installCrashNets() {
  if (g_nets_installed) return;
  g_nets_installed = true;
  std::atexit(endwinOnce);
  g_prev_terminate = std::set_terminate(onTerminate);
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onFatalSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND | SA_NODEFER;
  const int sigs[] = {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL};
  for (int s : sigs) ::sigaction(s, &sa, nullptr);
}

// 把"普通(非 DECCKM)模式"的光标键序列也登记进 ncurses 的键树。
//
// 为什么必须做:`keypad(stdscr, TRUE)` 会输出 smkx(`\E[?1h\E=`)把终端切到
// application cursor keys 模式,此时终端发的是 `ESC O A/B/C/D`,而 terminfo 里
// xterm-256color 的 kcuu1 **只登记了这一套**(kcuu1=\EOA)。
// 一旦终端不理 smkx(部分 tmux/screen 配置、精简终端、把方向键塞进宏或粘贴的场景),
// 它发来的是 `ESC [ A`,ncurses 认不出来 -> 返回裸 27 -> 落到我们的 Alt 合成里 ->
// 得到 `Alt-'['`,紧跟的 'A' 被当成普通字符 **插进缓冲区**。
// 症状:"按一下方向键,编辑区里多一个字母 A/B/C/D",而且没人会想到是 DECCKM。
// 实测(pty,TERM=xterm-256color):发 `\e[B` 会插一个 'B',发 `\eOB` 才是下移。
//
// define_key 是 ncurses 扩展(NCURSES_EXT_FUNCS),macOS 自带的 ncurses 也有;
// 没有它的实现上这段整体跳过,行为退回原状(不会编译失败)。
void registerCsiCursorKeys() {
#if defined(NCURSES_VERSION)
  struct Entry {
    const char* seq;
    int key;
  };
  static const Entry kEntries[] = {
      {"\033[A", KEY_UP},    {"\033[B", KEY_DOWN},  {"\033[C", KEY_RIGHT},
      {"\033[D", KEY_LEFT},  {"\033[H", KEY_HOME},  {"\033[F", KEY_END},
      {"\033[1~", KEY_HOME}, {"\033[4~", KEY_END},
      // 反过来也补一遍:某些终端在**普通**模式下也发 ESC O x。
      {"\033OA", KEY_UP},    {"\033OB", KEY_DOWN},  {"\033OC", KEY_RIGHT},
      {"\033OD", KEY_LEFT},  {"\033OH", KEY_HOME},  {"\033OF", KEY_END},
  };
  for (const Entry& e : kEntries) ::define_key(e.seq, e.key);
#endif
}

}  // namespace

// ============================================================ 纯逻辑(可单测)
// 这些函数刻意**不进 ui.h**:它们是 ui.cpp 的实现细节,不是对外契约。
// tests/test_ui.cpp 自行 extern 声明同名同签名的原型来钉住行为。
namespace ui_detail {

// 注意 extern:namespace 作用域的 const int 默认是内部链接,
// 不写 extern 的话 tests/test_ui.cpp 引用不到。
extern const int kMinRows = 16;        // 最小可用高度
extern const int kMinCols = 60;        // 最小可用宽度
extern const int kMinEditorRows = 3;   // 面板再高也要给编辑区留这么多行
extern const int kMinEditorCols = 20;  // 行号槽再宽也要给正文留这么多列

int gutterWidth(int line_count, int cols, bool show_line_numbers) {
  if (!show_line_numbers || cols <= 0) return 0;
  int digits = 1;
  for (int n = (line_count > 0 ? line_count : 1); n >= 10; n /= 10) ++digits;
  int numw = std::max(3, digits);
  int w = numw + 2;                       // [标记列][数字][空格]
  if (w > cols - kMinEditorCols) w = cols - kMinEditorCols;
  if (w < 0) w = 0;
  return w;
}

// 纯函数式布局:只吃标量,便于单测各种退化尺寸。
// 不变式:所有 h/w 均 >= 0;editor.h + tabs.h + panel.h + prompt.h + status.h <= rows。
Layout computeLayout(int rows, int cols, bool show_line_numbers, int line_count,
                     bool panel_visible, int panel_height, bool has_prompt) {
  Layout L{};
  if (rows < kMinRows || cols < kMinCols) return L;   // 退化尺寸:只画一行提示

  L.status = Rect{rows - 1, 0, 1, cols};
  int remain = rows - 1;

  const int prompt_h = has_prompt ? 1 : 0;
  remain -= prompt_h;

  int panel_total = 0;   // 含 1 行标签栏
  if (panel_visible) {
    int want = panel_height > 0 ? panel_height : 1;
    const int max_total = remain - kMinEditorRows;
    if (max_total >= 2) panel_total = std::min(want + 1, max_total);
  }
  const int editor_h = remain - panel_total;

  const int gw = gutterWidth(line_count, cols, show_line_numbers);
  L.gutter = Rect{0, 0, editor_h, gw};
  L.editor = Rect{0, gw, editor_h, cols - gw};
  if (panel_total > 0) {
    L.tabs  = Rect{editor_h, 0, 1, cols};
    L.panel = Rect{editor_h + 1, 0, panel_total - 1, cols};
  }
  if (prompt_h > 0) L.prompt = Rect{editor_h + panel_total, 0, 1, cols};
  return L;
}

// ghost 多行时,除首行(内联在锚点之后)外还要**覆盖**几个屏幕行。
// anchor_row 为锚点所在的屏幕行(0 起,相对编辑区);越界返回 0。
int ghostOverlayCount(int anchor_row, int ghost_lines, int view_h) {
  if (view_h <= 0 || anchor_row < 0 || anchor_row >= view_h) return 0;
  int extra = ghost_lines - 1;
  if (extra <= 0) return 0;
  const int avail = view_h - 1 - anchor_row;
  if (avail <= 0) return 0;
  return std::min(extra, avail);
}

// wget_wch 的返回值 + 键码 -> keys.h 的归一化键码空间。
//   rc == ERR           -> kKeyNone
//   rc == KEY_CODE_YES  -> KEY_* 原样(KEY_RESIZE 换成 kKeyResize)
//   rc == OK            -> 控制字符 0..0x1F / 0x7F 原样,其余 Char(cp)
// 为什么必须分开:KEY_DOWN(258) 与 U+0102 数值重叠,直接返回码点会把 "Ă"
// 当成方向键(跨模块约定 §2)。
int normalizeKey(int rc, unsigned int wch) {
  if (rc == ERR) return kKeyNone;
  if (rc == KEY_CODE_YES) {
    const int k = static_cast<int>(wch);
    if (k == KEY_RESIZE) return kKeyResize;
    return k;
  }
  const unsigned int cp = wch;
  if (cp > 0x10FFFFu) return kKeyNone;
  // ★ Enter 必须归一化成 13。踩过的坑:tty 的 ICRNL 会把终端发来的 CR(13)
  //   翻成 LF(10),而 ncurses 的 raw() **不清 ICRNL**(它只关 IXON/BRKINT/
  //   PARMRK/ICANON/ISIG/IEXTEN),现代 ncurses 的 nonl() 也只影响**输出**
  //   (只置 SP->_nl,不碰 termios)。于是 Enter 会以 10 的形式到达,而按
  //   跨模块约定 §18,Ctrl-J(10) 是"一个都不许绑"的键 —— 结果就是回车完全失灵。
  //   §18 里 Ctrl-J 之所以不绑,理由正是"它就是 Enter 的别名",所以在归一化
  //   这一层把 10 折成 13 与约定同向。实测见 .flower/artifacts/wave4-ui.md §4。
  if (cp == 10u) return 13;
  if (cp < 0x20u || cp == 0x7Fu) return static_cast<int>(cp);
  return Char(cp);
}

// 收到 27 之后的第二次(非阻塞)读的结果 -> 最终键码。
// second 必须**已经归一化**,否则 Alt-← 会拿到裸 '[' 而永远匹配不上
// Alt(KEY_LEFT)(跨模块约定 §2)。
int combineAlt(int second) {
  if (second == kKeyNone) return 27;          // 裸 Esc
  if (second == kKeyResize) return kKeyResize; // 缩放优先,别打 Alt 位
  if (second == 27) return 27;                // ESC ESC:仍按裸 Esc
  if (isAlt(second)) return second;           // 已带 Alt 位,不重复打
  return Alt(second);
}

// 截断到最多 max_cols 显示列;被截断时末尾放 "…"(自身占 1 列)。
// 绝不切断 UTF-8 字符,绝不返回超宽的串。
std::string fitDisplay(const std::string& s, int max_cols, int tab_w) {
  if (max_cols <= 0) return std::string();
  if (util::displayWidth(s, tab_w, 0) <= max_cols) return s;
  const int budget = max_cols - 1;   // 给 "…" 留一列
  std::string out;
  int col = 0;
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    const size_t n = util::utf8Decode(s, i, cp);
    const int w = (cp == '\t') ? tab_w : util::charDisplayWidth(cp);
    if (col + w > budget) break;
    out.append(s, i, n);
    col += w;
    i += n;
  }
  out += "\xe2\x80\xa6";   // U+2026 …
  return out;
}

}  // namespace ui_detail

// ============================================================ 绘制小工具
namespace {

using ui_detail::fitDisplay;

int scrRows() { return stdscr ? ::getmaxy(stdscr) : 0; }
int scrCols() { return stdscr ? ::getmaxx(stdscr) : 0; }

inline int A(unsigned long a) { return static_cast<int>(a); }

// 在 (y,x) 起、宽 w 列填充同一个 1 列宽字符。越界自动裁剪。
void fillRow(int y, int x, int w, int attr, wchar_t ch = L' ') {
  if (!stdscr) return;
  if (y < 0 || y >= scrRows()) return;
  if (x < 0) { w += x; x = 0; }
  if (w <= 0 || x >= scrCols()) return;
  w = std::min(w, scrCols() - x);
  ::wattrset(stdscr, attr);
  const wchar_t buf[2] = {ch, 0};
  for (int i = 0; i < w; ++i) {
    ::wmove(stdscr, y, x + i);
    ::waddnwstr(stdscr, buf, 1);
  }
}

// 画一行文本:UTF-8 -> 宽字符,按显示宽度排版(中文 2 列),Tab 展开到制表位。
//   x0/w      屏幕上可写的区间 [x0, x0+w)
//   from_col  从该串的第几个**显示列**开始画(横向滚动)
//   spans     非空时按语法高亮着色(须覆盖整行,见 highlight.h 契约)
//   base_attr spans 为空时用它;spans 非空时它作为附加位(如 ghost 的 A_DIM)
// 返回画完后所在的显示列(相对该串起点)。
// 不变式:绝不把一个多字节/宽字符切成两半;绝不写出 [x0, x0+w) 之外。
int putSpanned(int y, int x0, int w, const std::string& s,
               const std::vector<Span>* spans, int from_col, int tab_w,
               int base_attr, const Theme& th) {
  if (!stdscr || w <= 0) return 0;
  if (y < 0 || y >= scrRows()) return 0;
  if (x0 < 0) { w += x0; x0 = 0; }
  if (w <= 0 || x0 >= scrCols()) return 0;
  w = std::min(w, scrCols() - x0);

  size_t span_i = 0;
  int col = 0;               // 该串内的显示列
  bool pen_valid = false;    // 终端光标是否正好停在下一个要写的格子上
  wchar_t wbuf[2] = {0, 0};

  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    const size_t n = util::utf8Decode(s, i, cp);
    int cw;
    if (cp == '\t') {
      cw = tab_w - (col % tab_w);
      if (cw <= 0) cw = tab_w;
    } else {
      cw = util::charDisplayWidth(cp);
    }

    int attr = base_attr;
    if (spans) {
      while (span_i + 1 < spans->size() &&
             static_cast<size_t>((*spans)[span_i].start + (*spans)[span_i].len) <= i)
        ++span_i;
      if (span_i < spans->size()) attr |= th.attrForTok((*spans)[span_i].tok);
    }

    if (col + cw <= from_col) {              // 完全在左边界之外
      col += cw; i += n; pen_valid = false; continue;
    }
    if (col - from_col >= w) break;          // 已越过右边界
    if (col < from_col) {                    // 宽字符跨在左边界上:留空,不半个字
      col += cw; i += n; pen_valid = false; continue;
    }
    if (col - from_col + cw > w) break;      // 右边界放不下整个字符:留空

    const int x = x0 + (col - from_col);
    ::wattrset(stdscr, attr);
    if (cw == 0) {
      // 组合字符(零宽):不 move,直接附着到上一个格子
      if (pen_valid) { wbuf[0] = static_cast<wchar_t>(cp); ::waddnwstr(stdscr, wbuf, 1); }
      i += n;
      continue;
    }
    ::wmove(stdscr, y, x);
    if (cp == '\t') {
      for (int k = 0; k < cw; ++k) { wbuf[0] = L' '; ::waddnwstr(stdscr, wbuf, 1); }
    } else if (cp < 0x20 || cp == 0x7F) {
      wbuf[0] = L'?';                        // 控制字符占位,不让终端解释它
      ::waddnwstr(stdscr, wbuf, 1);
    } else {
      wbuf[0] = static_cast<wchar_t>(cp);
      ::waddnwstr(stdscr, wbuf, 1);
    }
    pen_valid = true;
    col += cw;
    i += n;
  }
  return col;
}

int putStr(int y, int x, int w, const std::string& s, int attr, int tab_w,
           const Theme& th) {
  return putSpanned(y, x, w, s, nullptr, 0, tab_w, attr, th);
}

int tabWidthOf(const AppModel& m) {
  if (!m.cfg) return 4;
  int t = m.cfg->tab_width;
  if (t < 1) t = 1;
  if (t > 16) t = 16;
  return t;
}

const char* sepFor(const Theme& th) { return th.wide_ok ? " \xe2\x94\x82 " : " | "; }

}  // namespace

// ============================================================ Theme
namespace {

struct PairDef { short pair; short fg; short bg; };

// 只用 8 个基础 ANSI 前景色(architecture §3 末尾),背景一律 -1(终端默认),
// 需要"底色"的几处(模式徽章 / 标签 / 状态栏 / 帮助浮层)才显式给 bg。
const PairDef kPairs[] = {
    {Theme::P_Normal,          -1,            -1},
    {Theme::P_Keyword,         COLOR_YELLOW,  -1},
    {Theme::P_Type,            COLOR_GREEN,   -1},
    {Theme::P_Preproc,         COLOR_MAGENTA, -1},
    {Theme::P_String,          COLOR_CYAN,    -1},
    {Theme::P_Char,            COLOR_CYAN,    -1},
    {Theme::P_Number,          COLOR_RED,     -1},
    {Theme::P_Comment,         COLOR_BLUE,    -1},
    {Theme::P_Operator,        -1,            -1},
    {Theme::P_Func,            COLOR_CYAN,    -1},
    {Theme::P_Todo,            COLOR_RED,     -1},
    // §3 的方案:COLORS >= 256 时 ghost 用 gray(color 8),**否则 A_DIM**。
    // 这里给 -1(终端默认前景)只是占位:8 色档 ghostAttr() 根本不用这个 pair,
    // 走 A_DIM;256 色档下面会把 fg 改写成 8。绝不要写成蓝色 —— 8 色终端上
    // 一旦有人误用这个 pair,ghost 就会变成"和注释一样的蓝",看着像真代码。
    {Theme::P_Ghost,           -1,            -1},
    {Theme::P_LineNo,          COLOR_BLUE,    -1},
    {Theme::P_LineNoPractice,  COLOR_BLACK,   COLOR_YELLOW},
    {Theme::P_StatusCode,      COLOR_BLACK,   COLOR_GREEN},
    {Theme::P_StatusPractice,  COLOR_BLACK,   COLOR_YELLOW},
    {Theme::P_StatusBar,       COLOR_BLACK,   COLOR_WHITE},
    {Theme::P_TabActive,       COLOR_BLACK,   COLOR_CYAN},
    {Theme::P_TabInactive,     -1,            -1},
    {Theme::P_TabUnread,       COLOR_YELLOW,  -1},
    {Theme::P_Error,           COLOR_RED,     -1},
    {Theme::P_Warn,            COLOR_YELLOW,  -1},
    {Theme::P_Meta,            COLOR_BLUE,    -1},
    {Theme::P_Help,            COLOR_WHITE,   COLOR_BLUE},
};

}  // namespace

void Theme::init() {
  has_color = false;
  colors = 0;
  pairs = 0;
  wide_ok = (MB_CUR_MAX > 1);

  if (!stdscr) return;
  if (!::has_colors()) return;              // 单色终端:全靠 A_BOLD/A_DIM/A_REVERSE
  if (::start_color() != OK) return;
  colors = COLORS;
  pairs = COLOR_PAIRS;
  if (colors < 8 || pairs < static_cast<int>(P_Count)) {
    // 颜色太少(或颜色对不够)就整体退回属性方案,免得画出不可读的组合。
    if (colors < 8) { has_color = false; return; }
  }
  const bool def_ok = (::use_default_colors() == OK);
  has_color = true;

  for (const PairDef& d : kPairs) {
    if (d.pair >= pairs) continue;          // 颜色对不够就不初始化,退化为 pair 0
    short fg = d.fg, bg = d.bg;
    if (!def_ok) {                          // 拿不到"终端默认色"就用白/黑
      if (fg < 0) fg = COLOR_WHITE;
      if (bg < 0) bg = COLOR_BLACK;
    }
    if (d.pair == P_Ghost && colors >= 256) fg = 8;   // §3:256 色时 ghost 用灰色
                                                      // (< 256 色时这个 pair 不被使用,
                                                      //  ghostAttr() 走 A_DIM)
    ::init_pair(d.pair, fg, bg);
  }
}

short Theme::pairForTok(Tok t) const {
  switch (t) {
    case Tok::Keyword:       return P_Keyword;
    case Tok::Type:          return P_Type;
    case Tok::Preproc:       return P_Preproc;
    case Tok::String:        return P_String;
    case Tok::Char:          return P_Char;
    case Tok::Number:        return P_Number;
    case Tok::Comment:       return P_Comment;
    case Tok::Operator:      return P_Operator;
    case Tok::Func:          return P_Func;
    case Tok::TodoInComment: return P_Todo;
    case Tok::Normal:        break;
  }
  return P_Normal;
}

int Theme::attrForTok(Tok t) const {
  if (!has_color) {
    // 单色降级:仍然要能区分注释/关键字,不能一片白。
    switch (t) {
      case Tok::Comment:       return A(A_DIM);
      case Tok::Keyword:
      case Tok::Type:          return A(A_BOLD);
      case Tok::TodoInComment: return A(A_BOLD | A_UNDERLINE);
      default:                 return A(A_NORMAL);
    }
  }
  int a = A(COLOR_PAIR(pairForTok(t)));
  if (t == Tok::Keyword || t == Tok::TodoInComment) a |= A(A_BOLD);
  return a;
}

int Theme::ghostAttr() const {
  // §3:256 色档用 gray(color 8);8 色档与单色档一律 A_DIM(暗),不许用蓝色 ——
  // 蓝色在 8 色终端上和注释同色,ghost 会被误读成已经写进文件的代码。
  if (has_color && colors >= 256 && pairs > P_Ghost) return A(COLOR_PAIR(P_Ghost));
  return A(A_DIM);
}

int Theme::modeAttr(bool practice) const {
  // 三重冗余的第 2 条:配色(练习=黄底,写代码=绿底)。反色由"深字浅底"体现;
  // 无颜色时退回 A_REVERSE,保证"最左格一定看得出来"。
  if (has_color && pairs > P_StatusPractice) {
    return A(COLOR_PAIR(practice ? P_StatusPractice : P_StatusCode) | A_BOLD);
  }
  return A(A_REVERSE | A_BOLD) | (practice ? A(A_UNDERLINE) : 0);
}

// ============================================================ Ui 生命周期
Ui::Ui() = default;

Ui::~Ui() { shutdown(); }

bool Ui::init(std::string& err) {
  if (inited_) return true;
  // 崩溃安全网先装:initscr 之后任何一步失败都不能把终端留在 raw 状态。
  installCrashNets();

  if (::initscr() == nullptr) {
    err = "无法初始化终端(initscr 失败):请在真实终端中运行,并检查 TERM 是否正确。";
    return false;
  }
  g_screen_live = 1;
  inited_ = true;

  ::raw();                      // 不是 cbreak:要拿到 Ctrl-字符的原始字节
  ::noecho();
  ::nonl();                     // Enter 保持 13,且省掉输出侧的 CR 翻译
  ::keypad(stdscr, TRUE);       // 方向键/F 键 -> KEY_*
  ::meta(stdscr, TRUE);         // 允许 8 位输入
  ::set_escdelay(25);           // Esc 前缀合成 Alt 的等待窗口
  ::curs_set(1);
  ::scrollok(stdscr, FALSE);    // 写到右下角不要滚屏
  ::idlok(stdscr, FALSE);
  ::leaveok(stdscr, FALSE);
  ::intrflush(stdscr, FALSE);

  registerCsiCursorKeys();

  theme_.init();
  rows_ = scrRows();
  cols_ = scrCols();
  hl_rev_ = -1;
  pending_clear_ = true;
  err.clear();
  return true;
}

void Ui::shutdown() {
  if (!inited_) {
    endwinOnce();   // 幂等:即使没 init 过也确保终端是干净的
    return;
  }
  if (stdscr) {
    ::curs_set(1);
    ::wattrset(stdscr, A(A_NORMAL));
    ::keypad(stdscr, FALSE);
    ::noraw();
    ::echo();
    ::nl();
  }
  endwinOnce();     // endwin() 自身会 reset_shell_mode,恢复 ICANON/ECHO
  inited_ = false;
}

void Ui::emergencyShutdown() { endwinOnce(); }

const Theme& Ui::theme() const { return theme_; }

int Ui::rows() const { return rows_; }
int Ui::cols() const { return cols_; }

bool Ui::tooSmall() const {
  return rows_ < ui_detail::kMinRows || cols_ < ui_detail::kMinCols;
}

void Ui::handleResize() {
  if (!inited_) return;
  // ncurses 在 SIGWINCH 后自己调过 resizeterm 并返回 KEY_RESIZE,
  // 这里只重取尺寸 + 强制全量重绘(不自装 SIGWINCH handler)。
  rows_ = scrRows();
  cols_ = scrCols();
  hl_rev_ = -1;
  pending_clear_ = true;
}

void Ui::forceFullRedraw() {
  pending_clear_ = true;
  // Ctrl-L 同时把影子高亮缓存整个丢掉:它是按"光标行"近似失效的,
  // 万一颜色真的错了,这里是用户可用的兜底修复手段。
  hl_cache_.clear();
  hl_rev_ = -1;
}

void Ui::beep() {
  if (inited_) ::beep();
}

// ============================================================ 输入
int Ui::getKeyBlockingFor(int timeout_ms) {
  if (!inited_ || !stdscr) return kKeyNone;
  // 用 wtimeout(ms) 而不是 halfdelay():后者只有 100ms 粒度,撑不起 60ms tick。
  ::wtimeout(stdscr, timeout_ms < 0 ? -1 : timeout_ms);
  wint_t wch = 0;
  const int rc = ::wget_wch(stdscr, &wch);
  const int k = ui_detail::normalizeKey(rc, static_cast<unsigned int>(wch));
  if (k != 27) return k;

  // 收到 Esc:做一次**非阻塞**读。有字符 -> Alt(c);ERR -> 裸 Esc。
  // 第二次读也必须先归一化,否则 Alt-← 会拿到裸 '['(跨模块约定 §2)。
  ::wtimeout(stdscr, 0);
  wint_t wch2 = 0;
  const int rc2 = ::wget_wch(stdscr, &wch2);
  ::wtimeout(stdscr, timeout_ms < 0 ? -1 : timeout_ms);
  const int k2 = ui_detail::normalizeKey(rc2, static_cast<unsigned int>(wch2));
  return ui_detail::combineAlt(k2);
}

// ============================================================ 布局
const Layout& Ui::relayout(const AppModel& m) {
  const bool show_ln = m.cfg ? m.cfg->show_line_numbers : true;
  const int line_count = m.ed ? m.ed->buffer().lineCount() : 1;
  layout_ = ui_detail::computeLayout(rows_, cols_, show_ln, line_count,
                                     m.panel_visible, m.panel_height,
                                     m.prompt != PromptKind::None);
  return layout_;
}

const Layout& Ui::layout() const { return layout_; }

// ============================================================ 一帧
void Ui::draw(const AppModel& m) {
  if (!inited_ || !stdscr) return;
  rows_ = scrRows();
  cols_ = scrCols();

  ::wattrset(stdscr, A(A_NORMAL));
  ::werase(stdscr);

  // 无条件重算:退化尺寸下 computeLayout 返回全 0,免得 layout() 留着上一帧
  // 那个大屏幕的旧矩形被别人当真。
  relayout(m);
  if (tooSmall()) {
    drawTooSmall();
  } else {
    cursor_y_ = layout_.editor.y;
    cursor_x_ = layout_.editor.x;
    drawEditor(m);
    drawGhost(m);
    drawTabs(m);
    drawPanel(m);
    drawPrompt(m);
    drawStatus(m);
    if (m.help_visible) drawHelp(m);
  }

  // 帮助浮层"关掉了"检测:关掉时把页码 +1,于是下一次打开就是下一页
  // (见 ui.h 的说明:AppModel 冻结,这是唯一不改签名的翻页通道)。
  // 放在 draw 的末尾:drawHelp 这一帧用的还是本次打开对应的页码。
  if (!m.help_visible && help_was_visible_) ++help_page_;
  help_was_visible_ = m.help_visible;

  // 真实光标最后定位 —— ghost/面板/浮层都不许把终端插入符带走。
  ::wattrset(stdscr, A(A_NORMAL));
  if (tooSmall() || m.help_visible) {
    ::curs_set(0);
    ::wmove(stdscr, 0, 0);
  } else {
    ::curs_set(1);
    const int cy = std::max(0, std::min(cursor_y_, rows_ - 1));
    const int cx = std::max(0, std::min(cursor_x_, cols_ - 1));
    ::wmove(stdscr, cy, cx);
  }
  if (pending_clear_) {
    ::clearok(stdscr, TRUE);
    pending_clear_ = false;
  }
  ::wrefresh(stdscr);
}

void Ui::drawTooSmall() {
  if (rows_ <= 0 || cols_ <= 0) return;
  char buf[128];
  std::snprintf(buf, sizeof(buf), "终端太小:当前 %dx%d,至少需要 %dx%d", cols_, rows_,
                ui_detail::kMinCols, ui_detail::kMinRows);
  const std::string msg = fitDisplay(buf, cols_, 4);
  fillRow(0, 0, cols_, A(A_REVERSE));
  putStr(0, 0, cols_, msg, A(A_REVERSE), 4, theme_);
}

// ------------------------------------------------------------ 编辑区
void Ui::drawEditor(const AppModel& m) {
  const Rect& er = layout_.editor;
  const Rect& gr = layout_.gutter;
  if (er.empty() || !m.ed) return;

  const Editor& ed = *m.ed;
  const TextBuffer& buf = ed.buffer();
  const Viewport& vp = ed.viewport();
  const int tab_w = tabWidthOf(m);
  const int top = std::max(0, vp.top);
  const int left = std::max(0, vp.left);
  const bool practice = (m.ai_mode == AiMode::Practice);
  const int nlines = buf.lineCount();

  // 三重冗余的第 3 条:练习模式下行号槽整体染同一强调色(眼睛盯着代码也能看见)。
  const int gutter_attr =
      theme_.has_color
          ? A(COLOR_PAIR(practice ? Theme::P_LineNoPractice : Theme::P_LineNo))
          : (practice ? A(A_REVERSE) : A(A_DIM));

  // 影子高亮缓存:AppModel 只给 const Editor&,拿不到 Editor::hlCache()
  // (非 const 成员),所以 Ui 自己维护一份。失效判据 = TextBuffer::revision(),
  // 失效起点近似取"本帧与上一帧光标行的较小者"(编辑总发生在光标处)。
  // 近似失败的最坏后果只是某几行颜色暂时不对,Ctrl-L 可修(forceFullRedraw)。
  const int cur_line = ed.cursor().line;
  const int rev = buf.revision();
  if (rev != hl_rev_) {
    hl_cache_.resize(nlines);
    hl_cache_.invalidateFrom(std::max(0, std::min(cur_line, hl_prev_cursor_line_)));
    hl_rev_ = rev;
  }
  hl_prev_cursor_line_ = cur_line;

  const int numw = gr.w >= 2 ? gr.w - 2 : 0;
  for (int row = 0; row < er.h; ++row) {
    const int ln = top + row;
    const int y = er.y + row;
    if (!gr.empty()) {
      fillRow(y, gr.x, gr.w, gutter_attr);
      if (ln < nlines && numw > 0) {
        // 手工右对齐(不用 "%*d":宽度是变量时 GCC 会报 -Wformat-truncation)
        std::string num = std::to_string(ln + 1);
        if (static_cast<int>(num.size()) < numw)
          num.insert(num.begin(), static_cast<size_t>(numw) - num.size(), ' ');
        putStr(y, gr.x + 1, numw, num, gutter_attr, tab_w, theme_);
      }
    }
    if (ln >= nlines) continue;
    const std::string& s = buf.line(ln);
    hl_cache_.spansFor(buf, ed.highlighter(), ln, spans_);
    putSpanned(y, er.x, er.w, s, &spans_, left, tab_w, 0, theme_);
  }

  // 真实光标(可能被后面的 prompt/panel 覆盖,以最后写入者为准)
  const Pos cur = ed.cursor();
  const int cy = er.y + (cur.line - top);
  const int cx = er.x + (ed.cursorDisplayCol() - left);
  cursor_y_ = std::max(er.y, std::min(cy, er.y + er.h - 1));
  cursor_x_ = std::max(er.x, std::min(cx, er.x + er.w - 1));
}

// ------------------------------------------------------------ ghost 覆盖
void Ui::drawGhost(const AppModel& m) {
  const Rect& er = layout_.editor;
  const Rect& gr = layout_.gutter;
  if (er.empty() || !m.ed) return;
  const Editor& ed = *m.ed;
  const Ghost& g = ed.ghost();
  if (g.text.empty()) return;
  // sink != GhostText 的 ghost 根本进不了 Editor(§6),这里不必再判模式。

  const TextBuffer& buf = ed.buffer();
  const Viewport& vp = ed.viewport();
  const int tab_w = tabWidthOf(m);
  const int top = std::max(0, vp.top);
  const int left = std::max(0, vp.left);
  const int attr = theme_.ghostAttr();

  const int anchor_row = g.anchor.line - top;
  if (anchor_row < 0 || anchor_row >= er.h) return;   // 锚点不在屏幕上,整块不画

  const std::vector<std::string> lines = util::splitLines(g.text);
  if (lines.empty()) return;

  // 首行:内联在锚点之后(覆盖该行右侧,不推挤下面任何真实内容)
  const int acol = util::byteToDisplayCol(buf.line(g.anchor.line), g.anchor.col, tab_w);
  const int x = er.x + (acol - left);
  if (acol >= left && x < er.x + er.w) {
    putSpanned(er.y + anchor_row, x, er.x + er.w - x, lines[0], nullptr, 0, tab_w,
               attr, theme_);
  }

  // 其余行:**覆盖**绘制在下方屏幕行上,行号槽画 '~' 表示"这是预览"
  const int overlay = ui_detail::ghostOverlayCount(
      anchor_row, static_cast<int>(lines.size()), er.h);
  for (int i = 1; i <= overlay; ++i) {
    const int y = er.y + anchor_row + i;
    fillRow(y, er.x, er.w, attr);            // 先抹掉该行真实内容再画预览
    if (!gr.empty()) {
      fillRow(y, gr.x, gr.w, attr);
      putStr(y, gr.x, 1, "~", attr, tab_w, theme_);
    }
    putSpanned(y, er.x, er.w, lines[static_cast<size_t>(i)], nullptr, 0, tab_w, attr,
               theme_);
  }
}

// ------------------------------------------------------------ 标签栏
namespace {

// AppModel 没有"面板区当前显示哪个标签"这一项(focus == -1 表示编辑区聚焦)。
// 约定:聚焦面板时画该面板;聚焦编辑区时画第一个有未读输出的面板,
// 都没有未读则画【编译】。这样后台编译/运行完成可见,又不抢焦点。
int activePanel(const AppModel& m) {
  const int n = m.panel_count > 0 ? m.panel_count : 0;
  if (m.focus >= 0 && m.focus < n) return m.focus;
  if (!m.panels) return 0;
  for (int i = 0; i < n; ++i)
    if (m.panels[i].unread()) return i;
  return 0;
}

std::string tabLabel(const Panel& p) {
  std::string s = "[";
  s += p.titleZh();
  if (p.errorCount() > 0) { s += " "; s += std::to_string(p.errorCount()); }
  if (p.unread()) s += "*";
  s += "]";
  return s;
}

}  // namespace

void Ui::drawTabs(const AppModel& m) {
  const Rect& tr = layout_.tabs;
  if (tr.empty() || !m.panels) return;
  const int tab_w = tabWidthOf(m);
  const int meta = theme_.has_color ? A(COLOR_PAIR(Theme::P_Meta)) : A(A_DIM);
  const wchar_t line_ch = theme_.wide_ok ? L'\x2500' : L'-';
  fillRow(tr.y, tr.x, tr.w, meta, line_ch);

  const int active = activePanel(m);
  int x = tr.x + 1;
  const int x_end = tr.x + tr.w;
  for (int i = 0; i < m.panel_count && i < kPanelCount; ++i) {
    const Panel& p = m.panels[i];
    const std::string label = tabLabel(p);
    const int lw = util::displayWidth(label, tab_w, 0);
    if (x + lw >= x_end) break;              // 放不下就不画(标签栏绝不换行)
    int attr;
    if (i == active) {
      attr = theme_.has_color ? A(COLOR_PAIR(Theme::P_TabActive) | A_BOLD)
                              : A(A_REVERSE | A_BOLD);
    } else if (p.unread()) {
      attr = theme_.has_color ? A(COLOR_PAIR(Theme::P_TabUnread) | A_BOLD) : A(A_BOLD);
    } else {
      attr = theme_.has_color ? A(COLOR_PAIR(Theme::P_TabInactive)) : A(A_NORMAL);
    }
    putStr(tr.y, x, x_end - x, label, attr, tab_w, theme_);
    x += lw + 1;                             // 标签之间留一个 '─'
  }

  // 面板聚焦时在右端提示怎么回到编辑区(§8.3)
  if (m.focus >= 0) {
    const std::string hint = "Esc 回编辑区";
    const int hw = util::displayWidth(hint, tab_w, 0);
    if (x_end - hw - 1 > x) {
      putStr(tr.y, x_end - hw - 1, hw, hint, meta, tab_w, theme_);
    }
  }
}

// ------------------------------------------------------------ 面板内容
void Ui::drawPanel(const AppModel& m) {
  const Rect& pr = layout_.panel;
  if (pr.empty()) return;
  const int tab_w = tabWidthOf(m);
  const int active = activePanel(m);
  const bool focused = (m.focus == active);

  // 【输入】面板的内容是一个 TextBuffer(见 panel.h),不走 Panel 的折行。
  if (active == static_cast<int>(PanelId::Input) && m.stdin_buf) {
    const StdinBuffer& sb = *m.stdin_buf;
    const TextBuffer& b = sb.buffer();
    const int top = std::max(0, sb.scroll());
    for (int row = 0; row < pr.h; ++row) {
      const int ln = top + row;
      if (ln >= b.lineCount()) break;
      putStr(pr.y + row, pr.x, pr.w, b.line(ln), A(A_NORMAL), tab_w, theme_);
    }
    if (focused) {
      const Pos c = sb.cursor();
      const int cy = pr.y + (c.line - top);
      const int cx = pr.x + util::byteToDisplayCol(b.line(c.line), c.col, tab_w);
      cursor_y_ = std::max(pr.y, std::min(cy, pr.y + pr.h - 1));
      cursor_x_ = std::max(pr.x, std::min(cx, pr.x + pr.w - 1));
    }
    return;
  }

  if (!m.panels) return;
  const Panel& p = m.panels[active];
  const std::vector<VisualLine>& vls = p.layout();   // 折行由 Panel 负责
  const int scroll = std::max(0, p.scroll());
  const int cursor_logical = p.cursorLine();

  for (int row = 0; row < pr.h; ++row) {
    const size_t vi = static_cast<size_t>(scroll) + static_cast<size_t>(row);
    if (vi >= vls.size()) break;
    const VisualLine& vl = vls[vi];
    const PanelLine& pl = p.lineAt(vl.logical);
    // 防御:VisualLine 的区间来自 Panel 的缓存,这里仍然自己夹一次
    const size_t start = std::min(static_cast<size_t>(std::max(0, vl.start)), pl.text.size());
    const size_t len = std::min(static_cast<size_t>(std::max(0, vl.len)), pl.text.size() - start);
    const std::string seg = pl.text.substr(start, len);

    int attr;
    if (pl.is_stderr) {
      attr = theme_.has_color ? A(COLOR_PAIR(Theme::P_Error)) : A(A_BOLD);
    } else if (pl.is_meta) {
      attr = theme_.has_color ? A(COLOR_PAIR(Theme::P_Meta)) : A(A_DIM);
    } else {
      attr = A(A_NORMAL);
    }
    if (focused && vl.logical == cursor_logical) {
      fillRow(pr.y + row, pr.x, pr.w, attr | A(A_REVERSE));
      attr |= A(A_REVERSE);
    }
    putStr(pr.y + row, pr.x, pr.w, seg, attr, tab_w, theme_);
  }

  if (focused) {
    const int vcur = p.logicalToVisual(cursor_logical) - scroll;
    cursor_y_ = std::max(pr.y, std::min(pr.y + vcur, pr.y + pr.h - 1));
    cursor_x_ = pr.x;
  }
}

// ------------------------------------------------------------ 提示行
void Ui::drawPrompt(const AppModel& m) {
  const Rect& r = layout_.prompt;
  if (r.empty()) return;
  const int tab_w = tabWidthOf(m);
  const int lab_attr = theme_.has_color ? A(COLOR_PAIR(Theme::P_Meta) | A_BOLD) : A(A_BOLD);
  fillRow(r.y, r.x, r.w, A(A_NORMAL));

  const std::string label = m.prompt_label.empty() ? std::string("> ") : m.prompt_label;
  const int lw = std::min(util::displayWidth(label, tab_w, 0), r.w);
  putStr(r.y, r.x, lw, label, lab_attr, tab_w, theme_);

  const int ix = r.x + lw;
  const int iw = r.w - lw;
  if (iw > 0) {
    // 输入串比可见宽度长时,横向滚动到光标可见
    const int cur_col = util::byteToDisplayCol(
        m.prompt_input, std::max(0, m.prompt_cursor), tab_w);
    const int from = std::max(0, cur_col - (iw - 1));
    putSpanned(r.y, ix, iw, m.prompt_input, nullptr, from, tab_w, A(A_NORMAL), theme_);
    cursor_y_ = r.y;
    cursor_x_ = std::max(r.x, std::min(ix + cur_col - from, r.x + r.w - 1));
  }
}

// ------------------------------------------------------------ 状态栏
void Ui::drawStatus(const AppModel& m) {
  const Rect& r = layout_.status;
  if (r.empty()) return;
  const int tab_w = tabWidthOf(m);
  const int bar = theme_.has_color && theme_.pairs > Theme::P_StatusBar
                      ? A(COLOR_PAIR(Theme::P_StatusBar))
                      : A(A_REVERSE);
  fillRow(r.y, r.x, r.w, bar);

  const bool practice = (m.ai_mode == AiMode::Practice);

  // 1) 模式徽章:固定在最左格 + 反色 + 配色(文字/颜色/位置三重冗余)
  std::string badge = " ";
  badge += aiModeZh(m.ai_mode);
  badge += " ";
  const int badge_w = std::min(util::displayWidth(badge, tab_w, 0), r.w);
  putStr(r.y, r.x, badge_w, badge, theme_.modeAttr(practice), tab_w, theme_);

  // 右端:F1帮助(固定位置,永不被挤掉)
  const std::string help = "F1帮助";
  const int help_w = util::displayWidth(help, tab_w, 0);
  int right_x = r.x + r.w;
  if (r.w - badge_w > help_w + 2) {
    right_x = r.x + r.w - help_w;
    putStr(r.y, right_x, help_w, help, bar | A(A_BOLD), tab_w, theme_);
  }

  // 2) 中间字段:左 -> 右,放不下就从优先级最低的开始丢
  // prio 小 = 空间不够时先丢;order = §8.1 规定的左->右次序(重新放回时用它复位)
  struct Field { std::string text; int attr; int prio; int order; };
  std::vector<Field> fs;
  int ord = 0;

  std::string fname = m.file_name.empty() ? std::string("未命名") : m.file_name;
  if (m.file_dirty) fname += "*";
  fs.push_back({fname, bar | A(A_BOLD), 90, ord++});

  char pos[48];
  std::snprintf(pos, sizeof(pos), "%d:%d", m.cursor_line, m.cursor_col);
  fs.push_back({pos, bar, 80, ord++});

  if (!m.lang_zh.empty()) fs.push_back({m.lang_zh, bar, 20, ord});
  ++ord;

  // 上次构建结果;有临时消息时让临时消息占这个位置(它更要紧)
  if (!m.status_msg.empty()) {
    int a = bar;
    if (m.errors > 0) a = theme_.has_color ? A(COLOR_PAIR(Theme::P_Error) | A_BOLD)
                                           : A(A_REVERSE | A_BOLD);
    fs.push_back({m.status_msg, a, 60, ord});
  } else if (!m.build_summary_zh.empty()) {
    int a = bar;
    if (m.errors > 0) a = bar | A(A_BOLD);
    else if (m.warnings > 0) a = bar | A(A_BOLD);
    fs.push_back({m.build_summary_zh, a, 40, ord});
  }
  ++ord;

  // AI 状态:就绪 / 思考中… / 未配置 / 错误
  {
    const char* zh = (m.ai_state_zh && *m.ai_state_zh) ? m.ai_state_zh : nullptr;
    if (!zh) {
      switch (m.ai_state) {
        case AiState::Disabled: zh = "未配置"; break;
        case AiState::Idle:     zh = "就绪";   break;
        case AiState::Thinking: zh = "思考中…"; break;
        case AiState::Error:    zh = "错误";   break;
      }
    }
    std::string s = "AI ";
    s += zh;
    fs.push_back({s, bar, 50, ord++});
  }

  // 运行中:§4.2 第三道防线的可见提示,优先级仅次于徽章
  if (m.running)
    fs.push_back({"运行中… Esc 终止", bar | A(A_BOLD) | A(A_UNDERLINE), 95, ord++});

  const std::string sep = sepFor(theme_);
  const int sep_w = util::displayWidth(sep, tab_w, 0);
  int avail = right_x - (r.x + badge_w) - 1;
  if (avail < 0) avail = 0;

  // 丢字段:反复找当前 prio 最小的丢掉,直到装得下
  std::vector<Field> dropped;
  for (;;) {
    int need = 0;
    for (size_t i = 0; i < fs.size(); ++i)
      need += util::displayWidth(fs[i].text, tab_w, 0) + sep_w;
    if (need <= avail || fs.empty()) break;
    size_t victim = 0;
    for (size_t i = 1; i < fs.size(); ++i)
      if (fs[i].prio < fs[victim].prio) victim = i;
    dropped.push_back(fs[victim]);
    fs.erase(fs.begin() + static_cast<long>(victim));
  }
  // 丢完还剩下空间的话,把最后被丢掉的那个(= 被丢者里优先级最高的)
  // 截断后放回去 —— 否则常见的 80 列终端上"上次构建结果"会整条消失。
  if (!dropped.empty()) {
    int used = 0;
    for (size_t i = 0; i < fs.size(); ++i)
      used += util::displayWidth(fs[i].text, tab_w, 0) + sep_w;
    const int room = avail - used - sep_w;
    if (room >= 3) {
      Field f = dropped.back();
      f.text = fitDisplay(f.text, room, tab_w);
      fs.push_back(f);
      std::stable_sort(fs.begin(), fs.end(),
                       [](const Field& a2, const Field& b2) { return a2.order < b2.order; });
    }
  }

  int x = r.x + badge_w;
  for (size_t i = 0; i < fs.size(); ++i) {
    if (x >= right_x) break;
    putStr(r.y, x, right_x - x, sep, bar, tab_w, theme_);
    x += sep_w;
    if (x >= right_x) break;
    const std::string t = fitDisplay(fs[i].text, right_x - x, tab_w);
    const int w = util::displayWidth(t, tab_w, 0);
    putStr(r.y, x, right_x - x, t, fs[i].attr, tab_w, theme_);
    x += w;
  }
}

// ------------------------------------------------------------ F1 帮助浮层
//
// helpLines() 有 71 行、最宽 76 显示列;80x24 的终端上浮层内部只有 ~19 行,
// 单页放不下 —— 修之前用户在标准终端上永远看不到 Ctrl-O / Alt-1 这些键
// (屏幕只会写一句"终端太矮,帮助未显示完",然后就没有然后了)。
// 现在的做法:
//   1) 浮层够宽时(>= 2 列内容 + 间隔)自动分**两栏**,能装的行数翻倍;
//   2) 还是装不完就**分页**,底部写明"第 X/Y 页 · 再按 F1 看下一页";
//      页码由 Ui 自己维护(AppModel 冻结,没有别的通道),利用"任意键关闭浮层"
//      这一现有行为 —— 连按两下 F1 就是下一页。
void Ui::drawHelp(const AppModel& m) {
  if (rows_ <= 0 || cols_ <= 0) return;
  const int tab_w = tabWidthOf(m);
  const std::vector<std::string> lines = helpLines();
  const int n = static_cast<int>(lines.size());

  int content_w = 0;
  for (const std::string& s : lines)
    content_w = std::max(content_w, util::displayWidth(s, tab_w, 0));
  const int box_w = std::min(cols_ - 2, std::max(20, content_w + 4));
  const int box_h = std::min(rows_ - 2, n + 4);
  if (box_w <= 2 || box_h <= 2) return;
  const int y0 = (rows_ - box_h) / 2;
  const int x0 = (cols_ - box_w) / 2;

  const int attr = theme_.has_color && theme_.pairs > Theme::P_Help
                       ? A(COLOR_PAIR(Theme::P_Help))
                       : A(A_REVERSE);
  const bool wide = theme_.wide_ok;
  const wchar_t hline = wide ? L'\x2500' : L'-';

  for (int i = 0; i < box_h; ++i) {
    // 左右各多抹一列:浮层边界正好落在宽字符中间时,那个字符的另一半
    // 会留在屏幕上变成半个乱码(ncurses 只保证被写的那一格被清掉)。
    fillRow(y0 + i, x0 - 1, box_w + 2, A(A_NORMAL));
    fillRow(y0 + i, x0, box_w, attr);
  }
  fillRow(y0, x0, box_w, attr, hline);
  fillRow(y0 + box_h - 1, x0, box_w, attr, hline);

  // 内容区:上下各一行边框,再留 1 行给页脚。
  const int inner_w = box_w - 4;
  const int inner_h = std::max(1, box_h - 3);
  const int rows_for_text = std::max(1, inner_h - 1);   // 最后一行是页脚

  // 分栏:只有**每栏都放得下最宽那行**时才分栏。
  // 为什么这么保守:helpLines() 里有 17 行宽于 40 列(最宽 76),
  // 一旦按"每栏 24 列"硬分,`(编译进行中 Esc = 终止编译作业)` 这种带缩进的
  // 续行会被整条截没 —— 实测 80 列下分 2 栏,这一行在屏幕上一个字都看不到。
  // 于是 80x24 保持单栏 + 4 页;>= 160 列的宽终端才会真的用上 2 栏。
  const int kGap = 2;
  int cols_n = content_w > 0 ? inner_w / (content_w + kGap) : 1;
  cols_n = std::max(1, std::min(cols_n, 3));
  const int col_w = (inner_w - (cols_n - 1) * kGap) / cols_n;

  const int per_page = rows_for_text * cols_n;
  const int pages = (n + per_page - 1) / per_page;
  help_pages_ = std::max(1, pages);   // 供 scrollHelp 归一化(它算不出这个数)
  if (help_page_ < 0) help_page_ = 0;
  const int page = pages > 0 ? (help_page_ % pages) : 0;
  const int first = page * per_page;

  for (int i = 0; i < per_page; ++i) {
    const int idx = first + i;
    if (idx >= n) break;
    const int c = i / rows_for_text;           // 先填满一栏再换下一栏
    const int r = i % rows_for_text;
    const int x = x0 + 2 + c * (col_w + kGap);
    putStr(y0 + 1 + r, x, col_w,
           fitDisplay(lines[static_cast<size_t>(idx)], col_w, tab_w), attr, tab_w, theme_);
  }

  std::string title = " cppide 帮助(Esc 或 F1 关闭)";
  if (pages > 1) {
    title += " 第 " + std::to_string(page + 1) + "/" + std::to_string(pages) + " 页";
  }
  putStr(y0, x0 + 1, box_w - 2, fitDisplay(title, box_w - 2, tab_w), attr | A(A_BOLD),
         tab_w, theme_);

  // 页脚同时写明两种翻页方式:PgDn/PgUp/↓/↑(主路径,App::onKey 路由到 scrollHelp)
  // 与"再按 F1"(兜底路径,靠"任意键关闭 + 重开即下一页")。
  // ★ 别把这句写长:inner_w 在 80 列终端上只有 74,而 `·`/`↓`/`↑` 这些
  //   East-Asian-Ambiguous 字符在中文 locale 下 wcwidth 给的是 **2**,
  //   按 1 估算会被 fitDisplay 截成 "… Esc 关…"(实测过)。
  const std::string footer =
      pages > 1 ? ("第 " + std::to_string(page + 1) + "/" + std::to_string(pages) +
                   " 页 · PgDn/PgUp 或 ↓/↑ 翻页 · 再按 F1 也翻页 · Esc 关闭")
                : std::string("完整列表也可用 cppide --help 查看");
  putStr(y0 + box_h - 2, x0 + 2, inner_w, fitDisplay(footer, inner_w, tab_w),
         attr | A(A_BOLD), tab_w, theme_);
}

// F1 帮助浮层翻页(ui.h 里有它存在的理由:总页数只有渲染层算得出来)。
// delta 以页为单位,结果在 [0, help_pages_) 内**回绕**。
// help_page_ 本身是个不取模的累加值(drawHelp 里才 % pages,`draw()` 关浮层时 ++),
// 所以这里先取模再加 delta;C++ 的 % 对负数给负结果,必须 +pages 再取一次模,
// 否则 PgUp 会算出负页码,drawHelp 里 `page * per_page` 变成负下标。
void Ui::scrollHelp(int delta) {
  const int pages = help_pages_ > 0 ? help_pages_ : 1;
  if (pages <= 1 || delta == 0) {
    if (pages <= 1) help_page_ = 0;
    return;
  }
  const int cur = ((help_page_ % pages) + pages) % pages;
  help_page_ = ((cur + (delta % pages)) % pages + pages) % pages;
}

// ============================================================ --doctor
std::string Ui::doctorReport() {
  std::string s;
  s += "== cppide --doctor ==\n";
  s += "TERM            : " + util::envOr("TERM", "(未设置)") + "\n";
  s += "LANG            : " + util::envOr("LANG", "(未设置)") + "\n";
  s += "LC_ALL          : " + util::envOr("LC_ALL", "(未设置)") + "\n";
  const char* loc = std::setlocale(LC_CTYPE, nullptr);
  s += "LC_CTYPE(生效)  : " + std::string(loc ? loc : "?") + "\n";
  s += "ncurses         : " + std::string(::curses_version()) + "\n";
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(MB_CUR_MAX));
  s += "MB_CUR_MAX      : " + std::string(buf) + "(>1 才能输出中文)\n";
  std::snprintf(buf, sizeof(buf), "%zu", sizeof(wchar_t));
  s += "sizeof(wchar_t) : " + std::string(buf) + "\n";
  if (stdscr) {
    std::snprintf(buf, sizeof(buf), "%d", COLORS);
    s += "COLORS          : " + std::string(buf) + "\n";
    std::snprintf(buf, sizeof(buf), "%d", COLOR_PAIRS);
    s += "COLOR_PAIRS     : " + std::string(buf) + "\n";
    std::snprintf(buf, sizeof(buf), "%dx%d", ::getmaxx(stdscr), ::getmaxy(stdscr));
    s += "屏幕尺寸        : " + std::string(buf) + "\n";
  } else {
    const int c = ::tigetnum("colors");
    std::snprintf(buf, sizeof(buf), "%d", c);
    s += "COLORS(terminfo): " + std::string(buf) + "(尚未 initscr)\n";
  }
  std::snprintf(buf, sizeof(buf), "%dx%d", ui_detail::kMinCols, ui_detail::kMinRows);
  s += "最小可用尺寸    : " + std::string(buf) + "\n";
  s += "提示            : 按键后会回显实际键码;Ctrl-X 退出 --doctor。\n";
  return s;
}

std::string Ui::describeKey(int key) const {
  char buf[96];
  if (key == kKeyNone) return "(超时,无按键)";
  std::string s = keyName(key);
  std::snprintf(buf, sizeof(buf), "  键码=0x%X", static_cast<unsigned>(key));
  s += buf;
  if (isChar(key)) {
    std::snprintf(buf, sizeof(buf), "  字符 U+%04X", charOf(key));
    s += buf;
  }
  if (isAlt(key)) s += "  [Alt 位]";
  const Action a = lookupAction(key);
  s += "  动作=";
  s += actionName(a);
  s += "(";
  s += actionZh(a);
  s += ")";
  return s;
}

// tests/test_ui.cpp —— src/ui.cpp 里可单测的**纯逻辑**
//
// ui.cpp 的绘制部分需要真终端,单测覆盖不到(那部分由 pty 驱动程序验证,
// 见 .flower/scripts/wave4-ui-pty.sh 与 .flower/artifacts/wave4-ui.md)。
// 这里钉住的是四类纯函数:
//   1. 键码归一化(wget_wch 的 rc/wch -> keys.h 键码空间)与 Alt 的 Esc 前缀合成
//   2. 布局计算:80x24 / 60x16 / 40x10 / 1x1 / 0x0 的各区尺寸不变式
//   3. 状态栏字段截断 fitDisplay(中文不许切半个字)
//   4. ghost 覆盖行数计算
//
// 这些函数刻意不进 ui.h(它们是实现细节,不是对外契约),所以原型在这里
// 手工 extern 声明 —— 签名对不上就链接失败,等于多一道看门人。
#define NCURSES_NOMACROS 1

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <curses.h>   // 只为拿到真实的 KEY_* / OK / ERR / KEY_CODE_YES 数值

#include "keys.h"
#include "ui.h"
#include "util.h"

// ------------------------------------------------------------------ 基础设施
static int g_checks = 0;
static int g_cases = 0;
static const char* g_case = "(none)";

#define CHECK(x)                                                                      \
  do {                                                                                \
    ++g_checks;                                                                       \
    if (!(x)) {                                                                       \
      fprintf(stderr, "\n*** FAIL [%s] %s:%d: %s\n", g_case, __FILE__, __LINE__, #x); \
      fflush(stderr);                                                                 \
      abort();                                                                        \
    }                                                                                 \
  } while (0)

#define CHECK_EQ_I(a, b)                                                        \
  do {                                                                          \
    ++g_checks;                                                                 \
    long long aa = (long long)(a), bb = (long long)(b);                         \
    if (aa != bb) {                                                             \
      fprintf(stderr, "\n*** FAIL [%s] %s:%d: %s == %s  (%lld vs %lld)\n",      \
              g_case, __FILE__, __LINE__, #a, #b, aa, bb);                      \
      fflush(stderr);                                                           \
      abort();                                                                  \
    }                                                                           \
  } while (0)

#define CHECK_EQ_S(a, b)                                                        \
  do {                                                                          \
    ++g_checks;                                                                 \
    std::string aa = (a), bb = (b);                                             \
    if (aa != bb) {                                                             \
      fprintf(stderr, "\n*** FAIL [%s] %s:%d: %s == %s  (\"%s\" vs \"%s\")\n",  \
              g_case, __FILE__, __LINE__, #a, #b, aa.c_str(), bb.c_str());      \
      fflush(stderr);                                                           \
      abort();                                                                  \
    }                                                                           \
  } while (0)

#define CASE(name)      \
  do {                  \
    g_case = name;      \
    ++g_cases;          \
    printf("  %s\n", name); \
  } while (0)

// ------------------------------------------- ui.cpp 的内部纯函数(手工声明)
namespace ui_detail {
extern const int kMinRows;
extern const int kMinCols;
extern const int kMinEditorRows;
extern const int kMinEditorCols;
int gutterWidth(int line_count, int cols, bool show_line_numbers);
Layout computeLayout(int rows, int cols, bool show_line_numbers, int line_count,
                     bool panel_visible, int panel_height, bool has_prompt);
int ghostOverlayCount(int anchor_row, int ghost_lines, int view_h);
int normalizeKey(int rc, unsigned int wch);
int combineAlt(int second);
std::string fitDisplay(const std::string& s, int max_cols, int tab_w);
}  // namespace ui_detail

using namespace ui_detail;

// ================================================================ 1. 键码
static void case_normalize_keys() {
  CASE("键码归一化:ERR / KEY_CODE_YES / 普通码点");
  // 空闲超时
  CHECK_EQ_I(normalizeKey(ERR, 0), kKeyNone);
  CHECK_EQ_I(normalizeKey(ERR, 12345), kKeyNone);

  // 功能键原样(0x101..0x1FF)
  CHECK_EQ_I(normalizeKey(KEY_CODE_YES, KEY_DOWN), KEY_DOWN);
  CHECK_EQ_I(normalizeKey(KEY_CODE_YES, KEY_LEFT), KEY_LEFT);
  CHECK_EQ_I(normalizeKey(KEY_CODE_YES, KEY_BACKSPACE), KEY_BACKSPACE);
  CHECK_EQ_I(normalizeKey(KEY_CODE_YES, KEY_F(1)), KEY_F(1));
  CHECK(normalizeKey(KEY_CODE_YES, KEY_DOWN) >= 0x101);
  CHECK(normalizeKey(KEY_CODE_YES, KEY_DOWN) <= 0x1FF);

  // KEY_RESIZE 归一化
  CHECK_EQ_I(normalizeKey(KEY_CODE_YES, KEY_RESIZE), kKeyResize);

  // 控制字符原样
  CHECK_EQ_I(normalizeKey(OK, 27), 27);
  CHECK_EQ_I(normalizeKey(OK, 13), 13);              // Enter
  // ★ tty 的 ICRNL 会把 CR 翻成 LF,而 ncurses 的 raw() 不清 ICRNL、
  //   nonl() 只管输出 —— 于是 Enter 会以 10 到达。10 必须折成 13,
  //   否则按 §18(Ctrl-J 一个都不许绑)回车会完全失灵。
  CHECK_EQ_I(normalizeKey(OK, 10), 13);
  CHECK(lookupAction(normalizeKey(OK, 10)) == Action::Enter);
  CHECK(lookupAction(10) == Action::None);           // 裸 10 仍然是"未绑定"
  CHECK_EQ_I(normalizeKey(OK, 9), 9);                // Tab
  CHECK(lookupAction(normalizeKey(OK, 9)) == Action::Tab);
  CHECK_EQ_I(normalizeKey(OK, 0x7F), 127);           // Backspace
  CHECK(lookupAction(normalizeKey(OK, 0x7F)) == Action::Backspace);
  CHECK_EQ_I(normalizeKey(OK, (unsigned)Ctrl('O')), Ctrl('O'));
  CHECK_EQ_I(normalizeKey(OK, 0), 0);

  // 可打印字符抬到 kCharBase 之上
  CHECK_EQ_I(normalizeKey(OK, 'a'), Char('a'));
  CHECK(isChar(normalizeKey(OK, 'a')));
  CHECK_EQ_I(normalizeKey(OK, 0x4F60), Char(0x4F60));   // 你
  CHECK(isChar(normalizeKey(OK, 0x4F60)));

  // ★ 那个坑:U+0102 与 KEY_DOWN(258) 数值重叠,归一化后必须分得开
  CHECK_EQ_I((int)KEY_DOWN, 258);
  CHECK_EQ_I(normalizeKey(OK, 0x0102), Char(0x0102));
  CHECK(normalizeKey(OK, 0x0102) != normalizeKey(KEY_CODE_YES, KEY_DOWN));
  CHECK(lookupAction(normalizeKey(OK, 0x0102)) == Action::None);
  CHECK(lookupAction(normalizeKey(KEY_CODE_YES, KEY_DOWN)) == Action::MoveDown);

  // 非法码点不许溢出到别的键
  CHECK_EQ_I(normalizeKey(OK, 0x110000), kKeyNone);
  CHECK_EQ_I(normalizeKey(OK, 0xFFFFFFFFu), kKeyNone);
}

static void case_alt_synthesis() {
  CASE("Alt 的 Esc 前缀合成");
  // 第二次读拿不到东西 -> 裸 Esc
  CHECK_EQ_I(combineAlt(kKeyNone), 27);
  // ESC ESC -> 仍按裸 Esc(不造 Alt-Esc 这种怪键)
  CHECK_EQ_I(combineAlt(27), 27);
  // 缩放优先,不打 Alt 位
  CHECK_EQ_I(combineAlt(kKeyResize), kKeyResize);
  // 普通字符 / 功能键
  CHECK_EQ_I(combineAlt(Char('1')), Alt(Char('1')));
  CHECK_EQ_I(combineAlt(KEY_LEFT), Alt(KEY_LEFT));
  CHECK(isAlt(combineAlt(KEY_LEFT)));
  CHECK_EQ_I(altBase(combineAlt(KEY_LEFT)), KEY_LEFT);
  // 已带 Alt 位不重复打
  CHECK_EQ_I(combineAlt(Alt(Char('a'))), Alt(Char('a')));

  // ★ 端到端:终端发 ESC 后再发 ESC [ D(被 ncurses 折成 KEY_LEFT)
  //   两步都必须先归一化,否则拿到的是裸 '[',Alt(KEY_LEFT) 永远匹配不上
  {
    const int first = normalizeKey(OK, 27);
    CHECK_EQ_I(first, 27);
    const int second = normalizeKey(KEY_CODE_YES, KEY_LEFT);
    CHECK(lookupAction(combineAlt(second)) == Action::MoveWordLeft);
  }
  // Alt-1 .. Alt-4 / Alt-0 / Alt-= / Alt-- 走同一条路
  CHECK(lookupAction(combineAlt(normalizeKey(OK, '1'))) == Action::FocusCompile);
  CHECK(lookupAction(combineAlt(normalizeKey(OK, '2'))) == Action::FocusRun);
  CHECK(lookupAction(combineAlt(normalizeKey(OK, '3'))) == Action::FocusAi);
  CHECK(lookupAction(combineAlt(normalizeKey(OK, '4'))) == Action::FocusInput);
  CHECK(lookupAction(combineAlt(normalizeKey(OK, '0'))) == Action::TogglePanel);
  CHECK(lookupAction(combineAlt(normalizeKey(OK, '='))) == Action::PanelTaller);
  CHECK(lookupAction(combineAlt(normalizeKey(OK, '-'))) == Action::PanelShorter);
  // 裸 Esc 必须落在 Action::Escape
  CHECK(lookupAction(combineAlt(kKeyNone)) == Action::Escape);
}

// ================================================================ 2. 布局
static void checkLayoutInvariants(const Layout& L, int rows, int cols) {
  const Rect* rs[] = {&L.gutter, &L.editor, &L.tabs, &L.panel, &L.prompt, &L.status};
  for (const Rect* r : rs) {
    CHECK(r->h >= 0);
    CHECK(r->w >= 0);
    CHECK(r->y >= 0);
    CHECK(r->x >= 0);
    // 不许越界写:每个区必须完全落在屏幕内
    CHECK(r->y + r->h <= (rows > 0 ? rows : 0));
    CHECK(r->x + r->w <= (cols > 0 ? cols : 0));
  }
  // 纵向各区互不重叠且总和不超屏(gutter 与 editor 同行,故只算 editor)
  const int total = L.editor.h + L.tabs.h + L.panel.h + L.prompt.h + L.status.h;
  CHECK(total <= (rows > 0 ? rows : 0));
  // gutter 与 editor 横向拼满整屏、不重叠
  CHECK_EQ_I(L.gutter.h, L.editor.h);
  if (L.editor.h > 0) {
    CHECK_EQ_I(L.gutter.x, 0);
    CHECK_EQ_I(L.editor.x, L.gutter.w);
    CHECK_EQ_I(L.gutter.w + L.editor.w, cols);
  }
  // 纵向顺序:编辑区 -> 标签栏 -> 面板 -> 提示行 -> 状态栏
  if (L.tabs.h > 0) CHECK_EQ_I(L.tabs.y, L.editor.y + L.editor.h);
  if (L.panel.h > 0) CHECK_EQ_I(L.panel.y, L.tabs.y + L.tabs.h);
  if (L.prompt.h > 0) CHECK_EQ_I(L.prompt.y, rows - 2);
  if (L.status.h > 0) CHECK_EQ_I(L.status.y, rows - 1);
}

static void case_layout_sizes() {
  CASE("布局:80x24 / 60x16 / 40x10 / 1x1 / 0x0 与退化输入");

  // ---- 80x24,10 行面板,无提示行
  {
    const Layout L = computeLayout(24, 80, true, 42, true, 10, false);
    checkLayoutInvariants(L, 24, 80);
    CHECK_EQ_I(L.status.h, 1);
    CHECK_EQ_I(L.status.y, 23);
    CHECK_EQ_I(L.tabs.h, 1);
    CHECK_EQ_I(L.panel.h, 10);
    CHECK_EQ_I(L.prompt.h, 0);
    CHECK_EQ_I(L.editor.h, 24 - 1 - 1 - 10);
    CHECK_EQ_I(L.gutter.w, 5);            // max(3, 2 位数) + 2
    CHECK_EQ_I(L.editor.w, 75);
  }
  // ---- 80x24 + 提示行
  {
    const Layout L = computeLayout(24, 80, true, 42, true, 10, true);
    checkLayoutInvariants(L, 24, 80);
    CHECK_EQ_I(L.prompt.h, 1);
    CHECK_EQ_I(L.prompt.y, 22);
    CHECK_EQ_I(L.editor.h, 24 - 1 - 1 - 1 - 10);
  }
  // ---- 面板折叠
  {
    const Layout L = computeLayout(24, 80, true, 42, false, 10, false);
    checkLayoutInvariants(L, 24, 80);
    CHECK_EQ_I(L.tabs.h, 0);
    CHECK_EQ_I(L.panel.h, 0);
    CHECK_EQ_I(L.editor.h, 23);
  }
  // ---- 关掉行号
  {
    const Layout L = computeLayout(24, 80, false, 42, true, 10, false);
    checkLayoutInvariants(L, 24, 80);
    CHECK_EQ_I(L.gutter.w, 0);
    CHECK_EQ_I(L.editor.w, 80);
  }
  // ---- 60x16:最小可用尺寸,面板要求 10 行也必须给编辑区留 kMinEditorRows
  {
    const Layout L = computeLayout(16, 60, true, 1, true, 10, false);
    checkLayoutInvariants(L, 16, 60);
    CHECK(L.editor.h >= kMinEditorRows);
    CHECK(L.editor.w >= kMinEditorCols);
    CHECK_EQ_I(L.editor.h + L.tabs.h + L.panel.h + L.status.h, 16);
  }
  // ---- 60x16 + 提示行 + 超大面板高度:仍不许挤掉编辑区
  {
    const Layout L = computeLayout(16, 60, true, 9999, true, 999, true);
    checkLayoutInvariants(L, 16, 60);
    CHECK(L.editor.h >= kMinEditorRows);
    CHECK_EQ_I(L.editor.h + L.tabs.h + L.panel.h + L.prompt.h + L.status.h, 16);
  }
  // ---- 退化尺寸:只画一行提示,布局全 0(绝不给出负数/越界矩形)
  {
    const int bad[][2] = {{10, 40}, {15, 80}, {24, 59}, {1, 1}, {0, 0}, {-3, -9}};
    for (const auto& rc : bad) {
      const Layout L = computeLayout(rc[0], rc[1], true, 100, true, 10, true);
      checkLayoutInvariants(L, rc[0], rc[1]);
      CHECK(L.editor.empty());
      CHECK(L.status.empty());
      CHECK(L.panel.empty());
      CHECK(L.tabs.empty());
      CHECK(L.prompt.empty());
    }
  }
  // ---- 大量尺寸扫一遍,只查不变式(防越界写的总闸)
  for (int rows = 0; rows <= 40; ++rows) {
    for (int cols = 0; cols <= 120; cols += 7) {
      for (int ph = 0; ph <= 30; ph += 5) {
        for (int vis = 0; vis < 2; ++vis) {
          for (int pr = 0; pr < 2; ++pr) {
            const Layout L =
                computeLayout(rows, cols, true, 12345, vis != 0, ph, pr != 0);
            checkLayoutInvariants(L, rows, cols);
          }
        }
      }
    }
  }
}

static void case_gutter_width() {
  CASE("行号槽宽度");
  CHECK_EQ_I(gutterWidth(1, 80, true), 5);          // max(3,1)+2
  CHECK_EQ_I(gutterWidth(999, 80, true), 5);
  CHECK_EQ_I(gutterWidth(1000, 80, true), 6);
  CHECK_EQ_I(gutterWidth(999999, 80, true), 8);
  CHECK_EQ_I(gutterWidth(42, 80, false), 0);        // 关掉行号
  CHECK_EQ_I(gutterWidth(0, 80, true), 5);          // 空缓冲区也当 1 行
  CHECK_EQ_I(gutterWidth(-5, 80, true), 5);
  // 极窄终端:正文至少留 kMinEditorCols 列
  CHECK(gutterWidth(999999, 24, true) <= 24 - kMinEditorCols);
  CHECK(gutterWidth(999999, 10, true) == 0);
  CHECK(gutterWidth(1, 0, true) == 0);
}

// ================================================================ 3. ghost
static void case_ghost_overlay() {
  CASE("ghost 覆盖行数");
  // 单行 ghost:只内联,不覆盖任何行
  CHECK_EQ_I(ghostOverlayCount(0, 1, 20), 0);
  CHECK_EQ_I(ghostOverlayCount(5, 1, 20), 0);
  CHECK_EQ_I(ghostOverlayCount(5, 0, 20), 0);
  // 多行:首行内联,其余覆盖到下方
  CHECK_EQ_I(ghostOverlayCount(0, 4, 20), 3);
  CHECK_EQ_I(ghostOverlayCount(5, 8, 20), 7);
  // 贴着编辑区底部:只画得下的那几行(绝不画到面板/状态栏上)
  CHECK_EQ_I(ghostOverlayCount(18, 8, 20), 1);
  CHECK_EQ_I(ghostOverlayCount(19, 8, 20), 0);
  CHECK_EQ_I(ghostOverlayCount(19, 2, 20), 0);
  // 锚点不在屏幕上 / 编辑区不存在
  CHECK_EQ_I(ghostOverlayCount(-1, 8, 20), 0);
  CHECK_EQ_I(ghostOverlayCount(20, 8, 20), 0);
  CHECK_EQ_I(ghostOverlayCount(0, 8, 0), 0);
  CHECK_EQ_I(ghostOverlayCount(0, 8, -4), 0);
  // 覆盖行不许超出编辑区
  for (int h = 1; h <= 30; ++h)
    for (int a = -2; a <= h + 2; ++a)
      for (int g = 0; g <= 12; ++g) {
        const int n = ghostOverlayCount(a, g, h);
        CHECK(n >= 0);
        if (n > 0) CHECK(a + n <= h - 1);
      }
}

// ================================================================ 4. 截断
static void case_fit_display() {
  CASE("状态栏字段截断(中文不许切半个字)");
  const int tw = 4;
  CHECK_EQ_S(fitDisplay("abc", 10, tw), "abc");     // 装得下就原样
  CHECK_EQ_S(fitDisplay("abcde", 5, tw), "abcde");  // 刚好装下
  CHECK_EQ_S(fitDisplay("", 5, tw), "");
  CHECK_EQ_S(fitDisplay("abc", 0, tw), "");
  CHECK_EQ_S(fitDisplay("abc", -1, tw), "");
  // 截断:末尾放 … (自身占 1 列)
  CHECK_EQ_S(fitDisplay("abcdef", 4, tw), "abc\xe2\x80\xa6");
  CHECK_EQ_I(util::displayWidth(fitDisplay("abcdef", 4, tw), tw, 0), 4);
  // 中文:2 列/字,绝不切出半个字符
  {
    const std::string zh = "编译成功 用时 320ms";
    for (int w = 1; w <= 30; ++w) {
      const std::string out = fitDisplay(zh, w, tw);
      CHECK(util::displayWidth(out, tw, 0) <= w);
      CHECK(util::utf8Valid(out));
    }
    // 宽度 5:能放 2 个汉字(4 列)+ …
    CHECK_EQ_S(fitDisplay(zh, 5, tw), "编译\xe2\x80\xa6");
    // 宽度 4:一个汉字放不下第二个,只能 1 个 + …
    CHECK_EQ_S(fitDisplay(zh, 4, tw), "编\xe2\x80\xa6");
  }
  // 每个前缀长度都保证有效 UTF-8 且不超宽
  {
    const std::string mixed = "main.cpp*  行12:列5  练习模式  AI思考中…";
    for (int w = 0; w <= 60; ++w) {
      const std::string out = fitDisplay(mixed, w, tw);
      CHECK(util::displayWidth(out, tw, 0) <= w);
      CHECK(util::utf8Valid(out));
    }
  }
}

// ================================================================ main
int main() {
  // ui.cpp 的宽度计算依赖 locale(约定 §7:真程序里由 main() 在 initscr 前调)
  std::setlocale(LC_ALL, "");
  printf("test_ui:\n");
  case_normalize_keys();
  case_alt_synthesis();
  case_layout_sizes();
  case_gutter_width();
  case_ghost_overlay();
  case_fit_display();
  printf("test_ui: 全部通过(%d 个用例 / %d 个断言)\n", g_cases, g_checks);
  return 0;
}

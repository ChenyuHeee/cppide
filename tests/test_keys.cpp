// tests/test_keys.cpp —— src/keys.cpp 的行为测试(architecture.md §8.2 / §8.3)
//
// 这个测试有一件别的测试做不到的事:它是**唯一** include 真正 <curses.h> 的地方,
// 用来给 keys.cpp 里硬编码的那份 ncurses 键码镜像做逐个比对。
// keys.cpp 刻意不 include curses.h(纪律见 keys.h / wave1-report §2.3),
// 代价就是必须有人证明那些数字没写错 —— 就是这里。
//
// 覆盖:
//   1. keys.cpp 的键码镜像 == 真实的 KEY_*(static_assert,编译期就红)
//   2. §8.2 全表逐条查表
//   3. 刻意不绑的键(Ctrl-C/Z/S/Q/\/Y/J/H)查不到动作
//   4. Ctrl-M(13)/Ctrl-I(9) 的特殊约定:有动作,但键名永远不叫 Ctrl-M/Ctrl-I
//   5. Alt 的 Esc 前缀合成(等价实现 + 查表)
//   6. 归一化键码空间不与 KEY_* 冲突(KEY_DOWN(258) vs U+0102 那个坑)
//   7. 绑定表自洽:键唯一、不绑裸可打印字符、每个 Action 至少一个键
//   8. keyName / actionName / actionZh / helpLines / isTextInput
#define NCURSES_NOMACROS 1

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <curses.h>

#include "keys.h"
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

#define CHECK_EQ_S(a, b)                                                             \
  do {                                                                               \
    ++g_checks;                                                                      \
    std::string aa = (a), bb = (b);                                                  \
    if (aa != bb) {                                                                  \
      fprintf(stderr, "\n*** FAIL [%s] %s:%d: \"%s\" != \"%s\"\n", g_case, __FILE__, \
              __LINE__, aa.c_str(), bb.c_str());                                     \
      fflush(stderr);                                                                \
      abort();                                                                       \
    }                                                                                \
  } while (0)

#define CHECK_EQ_I(a, b)                                                            \
  do {                                                                              \
    ++g_checks;                                                                     \
    long aa = (long)(a), bb = (long)(b);                                            \
    if (aa != bb) {                                                                 \
      fprintf(stderr, "\n*** FAIL [%s] %s:%d: %ld != %ld\n", g_case, __FILE__,      \
              __LINE__, aa, bb);                                                    \
      fflush(stderr);                                                               \
      abort();                                                                      \
    }                                                                               \
  } while (0)

static void begin(const char* name) {
  g_case = name;
  ++g_cases;
  printf("  ok  %s\n", name);
  fflush(stdout);
}

// ------------------------------------------------------------------ 1. 键码镜像
// keys.cpp 里写的是裸数字。这些 static_assert 是它们唯一的看门人。
static_assert(KEY_DOWN == 258, "keys.cpp 的 kNcDown 必须等于 KEY_DOWN");
static_assert(KEY_UP == 259, "kNcUp");
static_assert(KEY_LEFT == 260, "kNcLeft");
static_assert(KEY_RIGHT == 261, "kNcRight");
static_assert(KEY_HOME == 262, "kNcHome");
static_assert(KEY_BACKSPACE == 263, "kNcBksp");
static_assert(KEY_F0 == 264, "kNcF0");
static_assert(KEY_F(1) == 265, "F1");
static_assert(KEY_F(12) == 276, "F12");
static_assert(KEY_DC == 330, "kNcDC");
static_assert(KEY_IC == 331, "kNcIC");
static_assert(KEY_NPAGE == 338, "kNcNPage");
static_assert(KEY_PPAGE == 339, "kNcPPage");
static_assert(KEY_ENTER == 343, "kNcEnter");
static_assert(KEY_BTAB == 353, "kNcBTab");
static_assert(KEY_END == 360, "kNcEnd");
static_assert(KEY_SEND == 386, "kNcSEnd");
static_assert(KEY_SHOME == 391, "kNcSHome");
static_assert(KEY_RESIZE == 410, "kNcResize");
// 归一化键码空间的前提:所有 KEY_* 都落在 0x101..0x1FF。
static_assert(KEY_MIN == 257 && KEY_MAX == 511, "KEY_* 必须落在 0x101..0x1FF");
static_assert(kCharBase > KEY_MAX, "可打印字符基址必须高于所有 KEY_*");
static_assert(kCharBase + 0x10FFFF < kAltFlag, "Char 空间不能撞上 Alt 位");
// ★ wave1-report §3.1(5) 提到的那个坑:KEY_DOWN(258) 与 U+0102 数值重叠。
static_assert(KEY_DOWN == 0x102, "这就是坑本身");
static_assert(Char(0x102) != KEY_DOWN, "抬到 kCharBase 之上正是为了绕开它");

// ------------------------------------------------------------------ ui.cpp 的合成规则
// keys.h 没有给出合成函数的签名(合成要做非阻塞读,是 ui.cpp 的活)。
// 这里放一份**等价实现**,把规则钉死在测试里:.flower/notes/跨模块约定.md
// 要求 ui.cpp 逐字照做。
static int synthesizeAfterEsc(int second) {
  if (second == kKeyNone) return 27;   // 非阻塞读到 ERR => 裸 Esc
  return Alt(second);                  // second 必须已经归一化过
}

// ------------------------------------------------------------------ 用例
static void case_table_82() {
  begin("§8.2 全表逐条查表");
  struct Row {
    int key;
    Action act;
  };
  const Row rows[] = {
      // 移动
      {KEY_LEFT, Action::MoveLeft},
      {KEY_RIGHT, Action::MoveRight},
      {KEY_UP, Action::MoveUp},
      {KEY_DOWN, Action::MoveDown},
      {KEY_PPAGE, Action::PageUp},
      {KEY_NPAGE, Action::PageDown},
      {KEY_HOME, Action::MoveHome},
      {KEY_END, Action::MoveEnd},
      {Alt(KEY_LEFT), Action::MoveWordLeft},
      {Alt(KEY_RIGHT), Action::MoveWordRight},
      // 编辑
      {13, Action::Enter},
      {KEY_ENTER, Action::Enter},
      {127, Action::Backspace},
      {KEY_BACKSPACE, Action::Backspace},
      {KEY_DC, Action::Delete},
      {9, Action::Tab},
      {KEY_BTAB, Action::ShiftTab},
      {27, Action::Escape},
      // Ctrl 主绑定
      {Ctrl('O'), Action::Save},
      {Ctrl('X'), Action::Quit},
      {Ctrl('B'), Action::Compile},
      {Ctrl('R'), Action::Run},
      {Ctrl('E'), Action::CompileAndRun},
      {Ctrl('T'), Action::ToggleAiMode},
      {Ctrl('A'), Action::AskAi},
      {Ctrl('U'), Action::Undo},
      {Ctrl('W'), Action::Redo},
      {Ctrl('K'), Action::CutLine},
      {Ctrl('V'), Action::PasteLine},
      {Ctrl('D'), Action::CopyLine},
      {Ctrl('G'), Action::GotoLine},
      {Ctrl('F'), Action::Find},
      {Ctrl('N'), Action::NextDiag},
      {Ctrl('P'), Action::PrevDiag},
      {Ctrl('L'), Action::Redraw},
      // Alt 面板键
      {Alt(Char('1')), Action::FocusCompile},
      {Alt(Char('2')), Action::FocusRun},
      {Alt(Char('3')), Action::FocusAi},
      {Alt(Char('4')), Action::FocusInput},
      {Alt(Char('0')), Action::TogglePanel},
      {Alt(Char('=')), Action::PanelTaller},
      {Alt(Char('-')), Action::PanelShorter},
      // F 键别名(§8.2 末两行)
      {KEY_F(1), Action::Help},
      {KEY_F(2), Action::Save},
      {KEY_F(4), Action::AskAi},
      {KEY_F(5), Action::Compile},
      {KEY_F(6), Action::Run},
      {KEY_F(8), Action::ToggleAiMode},
      {KEY_F(10), Action::Quit},
  };
  for (const Row& r : rows) {
    if (lookupAction(r.key) != r.act) {
      fprintf(stderr, "\n*** 键 %d(%s)期望 %s,实得 %s\n", r.key,
              keyName(r.key).c_str(), actionName(r.act),
              actionName(lookupAction(r.key)));
      fflush(stderr);
      abort();
    }
    ++g_checks;
  }
  printf("      §8.2 表内 %zu 条绑定全部命中\n", sizeof(rows) / sizeof(rows[0]));
}

static void case_forbidden_keys() {
  begin("刻意不绑的键查不到动作");
  // §8.2:Ctrl-C(SIGINT)/Ctrl-Z(SIGTSTP)/Ctrl-S,Ctrl-Q(XON/XOFF)/
  //       Ctrl-\(SIGQUIT+core)/Ctrl-Y(macOS VDSUSP)/Ctrl-J/Ctrl-H
  struct F {
    int key;
    const char* why;
  };
  const F bad[] = {
      {Ctrl('C'), "SIGINT"},
      {Ctrl('Z'), "SIGTSTP"},
      {Ctrl('S'), "XOFF 冻屏"},
      {Ctrl('Q'), "XON"},
      {Ctrl('\\'), "SIGQUIT + core"},
      {Ctrl('Y'), "macOS VDSUSP"},
      {Ctrl('J'), "Enter 的别名(10)"},
      {Ctrl('H'), "Backspace 的别名(8)"},
  };
  for (const F& f : bad) {
    if (lookupAction(f.key) != Action::None) {
      fprintf(stderr, "\n*** 键码 %d(%s)被绑成了 %s —— 绝不允许\n", f.key, f.why,
              actionName(lookupAction(f.key)));
      fflush(stderr);
      abort();
    }
    ++g_checks;
  }
  // 数值确认:免得哪天 Ctrl() 的实现变了导致这条测试测了空气。
  CHECK_EQ_I(Ctrl('C'), 3);
  CHECK_EQ_I(Ctrl('Z'), 26);
  CHECK_EQ_I(Ctrl('S'), 19);
  CHECK_EQ_I(Ctrl('Q'), 17);
  CHECK_EQ_I(Ctrl('Y'), 25);
  CHECK_EQ_I(Ctrl('\\'), 28);
  CHECK_EQ_I(Ctrl('J'), 10);
  CHECK_EQ_I(Ctrl('H'), 8);
  // 也不许有任何绑定项落在这些键码上(查表实现万一将来换成 hash 也照样挡住)。
  std::size_t n = 0;
  const Binding* b = bindings(n);
  for (std::size_t i = 0; i < n; ++i) {
    for (const F& f : bad) CHECK(b[i].key != f.key);
  }
}

static void case_ctrl_m_i_aliases() {
  begin("Ctrl-M / Ctrl-I / Ctrl-? 的特殊约定");
  // 它们的键码 == Enter/Tab/Backspace,必须有动作(否则回车都用不了),
  // 但永远不能以 "Ctrl-X" 的形式出现在任何键名里。
  CHECK_EQ_I(Ctrl('M'), 13);
  CHECK_EQ_I(Ctrl('I'), 9);
  CHECK(lookupAction(13) == Action::Enter);
  CHECK(lookupAction(9) == Action::Tab);
  CHECK_EQ_S(keyName(13), "Enter");
  CHECK_EQ_S(keyName(9), "Tab");
  CHECK_EQ_S(keyName(127), "Backspace");
  std::size_t n = 0;
  const Binding* b = bindings(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (b[i].key == 13 || b[i].key == 9 || b[i].key == 127 ||
        b[i].key == KEY_BACKSPACE || b[i].key == KEY_ENTER) {
      CHECK(std::strstr(b[i].key_zh, "Ctrl-") == nullptr);
    }
  }
}

static void case_alt_synthesis() {
  begin("Alt 的 Esc 前缀合成");
  // 裸 Esc:非阻塞读拿不到第二个字节
  CHECK_EQ_I(synthesizeAfterEsc(kKeyNone), 27);
  CHECK(lookupAction(synthesizeAfterEsc(kKeyNone)) == Action::Escape);
  // Esc + '1' => Alt-1 => 聚焦编译面板
  CHECK(lookupAction(synthesizeAfterEsc(Char('1'))) == Action::FocusCompile);
  CHECK(lookupAction(synthesizeAfterEsc(Char('2'))) == Action::FocusRun);
  CHECK(lookupAction(synthesizeAfterEsc(Char('3'))) == Action::FocusAi);
  CHECK(lookupAction(synthesizeAfterEsc(Char('4'))) == Action::FocusInput);
  CHECK(lookupAction(synthesizeAfterEsc(Char('0'))) == Action::TogglePanel);
  CHECK(lookupAction(synthesizeAfterEsc(Char('='))) == Action::PanelTaller);
  CHECK(lookupAction(synthesizeAfterEsc(Char('-'))) == Action::PanelShorter);
  // Esc + 已归一化的方向键 => 按词移动(Terminal.app 下 Option-← 就是这条路)
  CHECK(lookupAction(synthesizeAfterEsc(KEY_LEFT)) == Action::MoveWordLeft);
  CHECK(lookupAction(synthesizeAfterEsc(KEY_RIGHT)) == Action::MoveWordRight);
  // 未绑定的 Alt 组合不能误命中
  CHECK(lookupAction(synthesizeAfterEsc(Char('9'))) == Action::None);
  CHECK(lookupAction(synthesizeAfterEsc(Char('q'))) == Action::None);
  // 位运算自洽
  CHECK(isAlt(Alt(Char('1'))));
  CHECK(!isAlt(Char('1')));
  CHECK(!isAlt(KEY_LEFT));
  CHECK_EQ_I(altBase(Alt(Char('1'))), Char('1'));
  CHECK_EQ_I(altBase(Alt(KEY_LEFT)), KEY_LEFT);
  CHECK_EQ_I(altBase(Alt(13)), 13);
  // Alt 位不会把 Char 判定搅乱
  CHECK(!isChar(Alt(Char('a'))));
  CHECK(isChar(altBase(Alt(Char('a')))));
  // Esc Esc(有些终端 Option-Esc)不该是任何动作
  CHECK(lookupAction(synthesizeAfterEsc(27)) == Action::None);
  CHECK_EQ_S(keyName(synthesizeAfterEsc(27)), "Esc Esc");
  CHECK_EQ_S(keyName(Alt(Char('1'))), "Alt-1");
  CHECK_EQ_S(keyName(Alt(KEY_LEFT)), "Alt-\xe2\x86\x90");
}

static void case_keycode_space_no_collision() {
  begin("归一化键码空间与 KEY_* 无冲突(258 vs U+0102)");
  // 坑的本体:直接返回码点会把 U+0102 "Ă" 当成 KEY_DOWN。
  CHECK_EQ_I(KEY_DOWN, 0x102);
  CHECK(lookupAction(KEY_DOWN) == Action::MoveDown);
  CHECK(lookupAction(Char(0x102)) == Action::None);   // "Ă" 不是方向键
  CHECK(isTextInput(Char(0x102)));                    // 它是要插入缓冲区的字符
  CHECK(!isTextInput(KEY_DOWN));
  // 全码点扫一遍:任何 Char(cp) 都不可能落进 KEY_* 区间、不可能带 Alt 位、
  // 也不可能命中任何绑定。
  std::size_t n = 0;
  const Binding* b = bindings(n);
  std::set<int> bound;
  for (std::size_t i = 0; i < n; ++i) bound.insert(b[i].key);
  int scanned = 0;
  for (uint32_t cp = 0; cp <= 0x10FFFF; ++cp) {
    int k = Char(cp);
    if (k <= KEY_MAX) {
      fprintf(stderr, "\n*** Char(0x%X)=%d 落进了 KEY_* 区间\n", cp, k);
      abort();
    }
    if (isAlt(k)) {
      fprintf(stderr, "\n*** Char(0x%X) 带上了 Alt 位\n", cp);
      abort();
    }
    if (!isChar(k)) {
      fprintf(stderr, "\n*** isChar(Char(0x%X)) 为假\n", cp);
      abort();
    }
    if (charOf(k) != cp) {
      fprintf(stderr, "\n*** charOf 往返失败 0x%X\n", cp);
      abort();
    }
    if (bound.count(k) != 0) {
      fprintf(stderr, "\n*** 码点 0x%X 被绑成了快捷键,打这个字就会触发动作\n", cp);
      abort();
    }
    ++scanned;
  }
  g_checks += 5;
  // 所有 KEY_* 与控制字符都不会被 isChar 误判
  for (int k = 0; k <= KEY_MAX; ++k) {
    CHECK(!isChar(k));
    CHECK(!isTextInput(k));
  }
  CHECK(!isChar(kKeyNone));
  CHECK(!isChar(kKeyResize));
  CHECK(!isTextInput(kKeyResize));
  CHECK(lookupAction(kKeyNone) == Action::None);
  CHECK(lookupAction(kKeyResize) == Action::None);
  printf("      扫了 %d 个码点 + 0..%d 全部键码,零冲突\n", scanned, KEY_MAX);
}

static void case_table_selfconsistent() {
  begin("绑定表自洽:键唯一 / 每个动作有键 / 键名非空");
  std::size_t n = 0;
  const Binding* b = bindings(n);
  CHECK(n >= 40);
  std::set<int> seen;
  bool has[static_cast<int>(Action::Count)] = {false};
  for (std::size_t i = 0; i < n; ++i) {
    // 同一个键码不能出现两次(否则查表结果取决于表序,是隐蔽 bug)
    if (!seen.insert(b[i].key).second) {
      fprintf(stderr, "\n*** 键码 %d(%s)在表里出现了两次\n", b[i].key,
              keyName(b[i].key).c_str());
      abort();
    }
    CHECK(b[i].key_zh != nullptr && b[i].key_zh[0] != '\0');
    CHECK(b[i].action != Action::None && b[i].action != Action::Count);
    // 绝不绑裸可打印字符:否则输入 'a' 就会触发动作
    CHECK(!isChar(b[i].key));
    CHECK(!isTextInput(b[i].key));
    // 查表必须自洽
    CHECK(lookupAction(b[i].key) == b[i].action);
    has[static_cast<int>(b[i].action)] = true;
  }
  for (int a = static_cast<int>(Action::MoveLeft);
       a < static_cast<int>(Action::Count); ++a) {
    if (!has[a]) {
      fprintf(stderr, "\n*** 动作 %s 一个键都没绑\n",
              actionName(static_cast<Action>(a)));
      abort();
    }
    ++g_checks;
  }
  printf("      %zu 条绑定,%d 个动作全部有键\n", n,
         static_cast<int>(Action::Count) - 1);
}

static void case_key_names() {
  begin("keyName");
  CHECK_EQ_S(keyName(Ctrl('O')), "Ctrl-O");
  CHECK_EQ_S(keyName(Ctrl('A')), "Ctrl-A");
  CHECK_EQ_S(keyName(Ctrl('Z')), "Ctrl-Z");   // 不绑,但键名照样要能显示(--doctor)
  CHECK_EQ_S(keyName(0), "Ctrl-@");
  CHECK_EQ_S(keyName(28), "Ctrl-\\");
  CHECK_EQ_S(keyName(31), "Ctrl-_");
  CHECK_EQ_S(keyName(27), "Esc");
  CHECK_EQ_S(keyName(KEY_F(1)), "F1");
  CHECK_EQ_S(keyName(KEY_F(5)), "F5");
  CHECK_EQ_S(keyName(KEY_F(12)), "F12");
  CHECK_EQ_S(keyName(KEY_LEFT), "\xe2\x86\x90");
  CHECK_EQ_S(keyName(KEY_RIGHT), "\xe2\x86\x92");
  CHECK_EQ_S(keyName(KEY_UP), "\xe2\x86\x91");
  CHECK_EQ_S(keyName(KEY_DOWN), "\xe2\x86\x93");
  CHECK_EQ_S(keyName(KEY_HOME), "Home");
  CHECK_EQ_S(keyName(KEY_END), "End");
  CHECK_EQ_S(keyName(KEY_PPAGE), "PgUp");
  CHECK_EQ_S(keyName(KEY_NPAGE), "PgDn");
  CHECK_EQ_S(keyName(KEY_DC), "Delete");
  CHECK_EQ_S(keyName(KEY_BTAB), "Shift-Tab");
  CHECK_EQ_S(keyName(KEY_RESIZE), "Resize");
  CHECK_EQ_S(keyName(Char('a')), "a");
  CHECK_EQ_S(keyName(Char('Z')), "Z");
  CHECK_EQ_S(keyName(Char(' ')), "空格");
  CHECK_EQ_S(keyName(Char(0x4E2D)), "中");         // U+4E2D
  CHECK_EQ_S(keyName(Char(0x1F600)), "\xf0\x9f\x98\x80");   // 4 字节 UTF-8
  CHECK_EQ_S(keyName(kKeyNone), "(无按键)");
  CHECK_EQ_S(keyName(kKeyResize), "终端尺寸变化");
  // 兜底路径不许崩、不许返回空串
  const int weird[] = {-999, -2, 300, 500, 511, 0x1FF, 0x7FFFFFF, kAltFlag | 999};
  for (int k : weird) {
    std::string s = keyName(k);
    CHECK(!s.empty());
  }
  // 表里每个键的 key_zh 与 keyName 至少要"看起来是一件事":
  // 别名(Enter/Backspace 各有两个键码)允许相同,但都不能是空串。
  std::size_t n = 0;
  const Binding* b = bindings(n);
  for (std::size_t i = 0; i < n; ++i) CHECK(!keyName(b[i].key).empty());
}

static void case_action_names() {
  begin("actionName / actionZh");
  std::set<std::string> en, zh;
  for (int a = 0; a <= static_cast<int>(Action::Count); ++a) {
    Action act = static_cast<Action>(a);
    const char* e = actionName(act);
    const char* z = actionZh(act);
    CHECK(e != nullptr && e[0] != '\0');
    CHECK(z != nullptr && z[0] != '\0');
    if (act != Action::None && act != Action::Count) {
      CHECK(en.insert(e).second);   // 英文标识必须唯一
      zh.insert(z);
    }
  }
  CHECK(en.size() == static_cast<std::size_t>(Action::Count) - 1);
  CHECK(zh.size() == en.size());    // 中文说明也别重复
  printf("      %zu 个动作各有唯一的中英文名\n", en.size());
}

static void case_help_lines() {
  begin("helpLines");
  std::vector<std::string> v = helpLines();
  CHECK(v.size() >= 40);
  std::string all;
  for (const std::string& s : v) {
    CHECK(s.find('\n') == std::string::npos);   // 每元素一行
    CHECK(s.find('\t') == std::string::npos);   // 已排好版,不能靠 Tab 对齐
    // 浮层最窄也有 60 列可用;超长会被裁,这里守一个上限
    CHECK(util::displayWidth(s, 8, 0) <= 78);
    all += s;
    all += '\n';
  }
  // 关键键位必须出现在帮助里
  const char* must[] = {"Ctrl-O", "Ctrl-X", "Ctrl-B", "Ctrl-R", "Ctrl-E",
                        "Ctrl-T", "Ctrl-A", "F1",     "F5",     "Alt-1",
                        "撤销",   "编译",   "帮助"};
  for (const char* m : must) {
    if (all.find(m) == std::string::npos) {
      fprintf(stderr, "\n*** 帮助浮层里找不到 \"%s\"\n", m);
      abort();
    }
    ++g_checks;
  }
  // 不绑的键要在帮助里说明清楚(否则用户按了没反应会以为是 bug)
  CHECK(all.find("Ctrl-C") != std::string::npos);
  printf("      %zu 行,最宽 %d 列\n", v.size(), [&v] {
    int w = 0;
    for (const std::string& s : v) {
      int x = util::displayWidth(s, 8, 0);
      if (x > w) w = x;
    }
    return w;
  }());
}

static void case_text_input() {
  begin("isTextInput");
  CHECK(isTextInput(Char('a')));
  CHECK(isTextInput(Char('Z')));
  CHECK(isTextInput(Char('0')));
  CHECK(isTextInput(Char(' ')));
  CHECK(isTextInput(Char('=')));
  CHECK(isTextInput(Char(0x4E2D)));     // 中
  CHECK(isTextInput(Char(0x3002)));     // 。
  CHECK(isTextInput(Char(0x1F600)));    // emoji
  CHECK(isTextInput(Char(0xFF0C)));     // 全角逗号
  CHECK(!isTextInput(Char(0)));
  CHECK(!isTextInput(Char(9)));         // Tab 的码点不该走文本插入
  CHECK(!isTextInput(Char(13)));
  CHECK(!isTextInput(Char(27)));
  CHECK(!isTextInput(Char(0x7F)));
  CHECK(!isTextInput(Char(0x85)));      // C1 控制字符 NEL
  CHECK(!isTextInput(Char(0xD800)));    // 代理区
  CHECK(!isTextInput(Char(0xDFFF)));
  CHECK(!isTextInput(Char(0x110000)));  // 超出 Unicode
  CHECK(!isTextInput(0));
  CHECK(!isTextInput(9));
  CHECK(!isTextInput(13));
  CHECK(!isTextInput(27));
  CHECK(!isTextInput(127));
  CHECK(!isTextInput(kKeyNone));
  CHECK(!isTextInput(kKeyResize));
  CHECK(!isTextInput(KEY_LEFT));
  CHECK(!isTextInput(KEY_F(5)));
  CHECK(!isTextInput(Alt(Char('a'))));  // Alt-a 是组合键,不是文本
  CHECK(!isTextInput(Ctrl('O')));
}

int main() {
  printf("test_keys:\n");
  case_table_82();
  case_forbidden_keys();
  case_ctrl_m_i_aliases();
  case_alt_synthesis();
  case_keycode_space_no_collision();
  case_table_selfconsistent();
  case_key_names();
  case_action_names();
  case_help_lines();
  case_text_input();
  printf("test_keys: 全部通过(%d 个用例 / %d 个断言)\n", g_cases, g_checks);
  return 0;
}

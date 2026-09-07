// tests/test_editor.cpp —— Editor 的纯 assert 单测(main 返回 0 = 通过)
//
// 链接:src/editor.cpp src/textbuf.cpp src/util.cpp src/highlight.cpp
// 覆盖重点:
//   1. §6“练习模式绝不插入缓冲区”的**反证测试**(最重要,NDEBUG 下也必须通过)
//   2. 光标穿越中文 / emoji / Tab 时永不落进字符中间;上下移动保持 desired col
//   3. 自动缩进、跨行 Backspace、空缓冲区上的各种操作
//   4. 视口:文件末尾 / 超长行右端 / 退化尺寸(1 或 0)
//   5. 行级剪切累积 + 粘贴往返
//   6. 一次动作一次撤销(接受 8 行 ghost 后单次 undo 全回退)
//   7. 越界与退化输入
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include "../src/editor.h"
#include "../src/util.h"

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond)                                                         \
  do {                                                                      \
    ++g_checks;                                                             \
    if (!(cond)) {                                                          \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
      ++g_fail;                                                             \
    }                                                                       \
  } while (0)

#define CHECK_EQI(a, b)                                                     \
  do {                                                                      \
    ++g_checks;                                                             \
    long long va_ = (long long)(a), vb_ = (long long)(b);                   \
    if (va_ != vb_) {                                                       \
      std::printf("FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__,       \
                  __LINE__, #a, #b, va_, vb_);                              \
      ++g_fail;                                                             \
    }                                                                       \
  } while (0)

#define CHECK_EQS(a, b)                                                     \
  do {                                                                      \
    ++g_checks;                                                             \
    std::string va_ = (a), vb_ = (b);                                       \
    if (va_ != vb_) {                                                       \
      std::printf("FAIL %s:%d  %s == %s  (\"%s\" vs \"%s\")\n", __FILE__,   \
                  __LINE__, #a, #b, va_.c_str(), vb_.c_str());              \
      ++g_fail;                                                             \
    }                                                                       \
  } while (0)

#define CHECK_POS(p, L, C)                                                  \
  do {                                                                      \
    Pos p_ = (p);                                                           \
    CHECK_EQI(p_.line, L);                                                  \
    CHECK_EQI(p_.col, C);                                                   \
  } while (0)

static void setLines(Editor& ed, std::vector<std::string> ls) {
  ed.buffer().reset(std::move(ls));
  ed.onBufferChanged(0);
  ed.setCursor(Pos{0, 0});
  ed.buffer().clearHistory();
}

// =====================================================================
// 1. §6 反证测试:AI 文本绝不可能绕过 acceptGhost 的四道拦截进入缓冲区
// =====================================================================

// 一个“除了被拦截的那一项之外全都合格”的 ghost 工厂。
static Ghost mkGhost(const std::string& text, uint64_t gen, AiSink sink,
                     bool complete, Pos anchor) {
  Ghost g;
  g.text = text;
  g.gen = gen;
  g.sink = sink;
  g.complete = complete;
  g.anchor = anchor;
  return g;
}

static void testGhostSecurity() {
  const std::vector<std::string> orig = {"int main() {", "  return 0;", "}"};

  // ---- (a) sink == PanelOnly:setGhost 的运行期防线 + acceptGhost 的结构性拦截 ----
  // 注意:setGhost 里还有一个 assert,非 NDEBUG 构建下它会先 abort,
  // 所以这一段只在 NDEBUG 下执行 —— 反过来正好证明“不是靠 assert 拦住的”。
#ifdef NDEBUG
  {
    Config cfg;
    Editor ed(cfg);
    setLines(ed, orig);
    ed.setViewSize(80, 24);
    ed.setExpectedGen(42);
    const std::string before = ed.buffer().text();
    const int rev = ed.buffer().revision();

    ed.setGhost(mkGhost("EVIL_CODE();\n", 42, AiSink::PanelOnly, true, Pos{0, 12}));
    CHECK(ed.ghost().empty());                 // 退化结果是“没有 ghost”
    CHECK(!ed.acceptGhost());                  // ★ 不接受
    CHECK_EQS(ed.buffer().text(), before);     // ★ 逐字节未变
    CHECK_EQI(ed.buffer().revision(), rev);
    CHECK(!ed.buffer().dirty());
    CHECK(!ed.buffer().canUndo());             // 连撤销记录都没产生

    // 反复来几次也一样
    for (int i = 0; i < 5; ++i) {
      ed.setGhost(mkGhost("EVIL\n", 42, AiSink::PanelOnly, true, ed.cursor()));
      CHECK(!ed.acceptGhost());
    }
    CHECK_EQS(ed.buffer().text(), before);
  }
#endif

  // ---- (b) gen 过期:sink/complete/text 全合格,只有 gen 不匹配 ----
  {
    Config cfg;
    Editor ed(cfg);
    setLines(ed, orig);
    ed.setViewSize(80, 24);
    ed.setExpectedGen(8);
    const std::string before = ed.buffer().text();

    ed.setGhost(mkGhost("stale();\n", 7, AiSink::GhostText, true, Pos{0, 12}));
    CHECK(!ed.ghost().empty());                // 这次 ghost 是设上去了
    CHECK(!ed.acceptGhost());                  // 但 gen 过期 -> 不接受
    CHECK_EQS(ed.buffer().text(), before);
    CHECK(!ed.buffer().canUndo());

    // 把 expected_gen_ 对上就应当能接受(证明上面拦的确实是 gen 这一条)
    ed.setExpectedGen(7);
    CHECK(ed.acceptGhost());
    CHECK(ed.buffer().text() != before);
    CHECK(ed.undo());
    CHECK_EQS(ed.buffer().text(), before);
  }

  // ---- (c) complete == false:按 Tab(= acceptGhost)不插入 ----
  {
    Config cfg;
    Editor ed(cfg);
    setLines(ed, orig);
    ed.setViewSize(80, 24);
    ed.setExpectedGen(3);
    const std::string before = ed.buffer().text();

    ed.setGhost(mkGhost("half_written(", 3, AiSink::GhostText, false, Pos{0, 12}));
    CHECK(!ed.ghost().complete);
    CHECK(!ed.acceptGhost());
    CHECK_EQS(ed.buffer().text(), before);
    CHECK(!ed.buffer().canUndo());

    // 流式再来一段,仍然未 complete -> 仍然不接受
    ed.appendGhostDelta(3, "1);");
    CHECK_EQS(ed.ghost().text, std::string("half_written(1);"));
    CHECK(!ed.acceptGhost());
    CHECK_EQS(ed.buffer().text(), before);

    // markGhostComplete 用错 gen -> 无效
    ed.markGhostComplete(4);
    CHECK(!ed.ghost().complete);
    CHECK(!ed.acceptGhost());
    CHECK_EQS(ed.buffer().text(), before);

    // 正确 gen -> 才 complete,才能接受
    ed.markGhostComplete(3);
    CHECK(ed.ghost().complete);
    CHECK(ed.acceptGhost());
    CHECK_EQS(ed.buffer().line(0), std::string("int main() {half_written(1);"));
  }

  // ---- (d) text 为空 ----
  {
    Config cfg;
    Editor ed(cfg);
    setLines(ed, orig);
    ed.setExpectedGen(1);
    const std::string before = ed.buffer().text();
    ed.setGhost(mkGhost("", 1, AiSink::GhostText, true, Pos{0, 0}));
    CHECK(ed.ghost().empty());
    CHECK(!ed.acceptGhost());
    CHECK_EQS(ed.buffer().text(), before);
    CHECK(!ed.buffer().canUndo());
  }

  // ---- (e) 流式片段的 gen 过滤:过期 delta 不许污染当前 ghost ----
  {
    Config cfg;
    Editor ed(cfg);
    setLines(ed, orig);
    ed.setExpectedGen(9);
    ed.setGhost(mkGhost("ok", 9, AiSink::GhostText, false, Pos{0, 0}));
    ed.appendGhostDelta(8, "STALE");           // 过期 gen
    CHECK_EQS(ed.ghost().text, std::string("ok"));
    ed.appendGhostDelta(9, "!");
    CHECK_EQS(ed.ghost().text, std::string("ok!"));
    // clearGhost 之后任何 delta 都进不来(默认 sink 是 PanelOnly)
    ed.clearGhost();
    ed.appendGhostDelta(9, "X");
    ed.appendGhostDelta(0, "X");
    CHECK(ed.ghost().empty());
    CHECK_EQI((int)ed.ghost().sink, (int)AiSink::PanelOnly);
    CHECK(!ed.acceptGhost());
  }

  // ---- (f) 用户一编辑,旧 ghost 立即作废(不会在 Tab 时冒出来) ----
  {
    Config cfg;
    Editor ed(cfg);
    setLines(ed, orig);
    ed.setExpectedGen(5);
    ed.setGhost(mkGhost("suggestion();\n", 5, AiSink::GhostText, true, Pos{0, 12}));
    CHECK(!ed.ghost().empty());
    ed.setCursor(Pos{1, 2});
    ed.insertText("x");                        // 打一个字
    CHECK(ed.ghost().empty());
    CHECK(!ed.acceptGhost());
  }
}

#ifndef NDEBUG
// 非 NDEBUG 构建:证明 setGhost 里那条 assert 真的存在(fork 出去等它 abort)。
static void testSetGhostAssertFires() {
  ++g_checks;
  pid_t pid = fork();
  if (pid == 0) {
    // 子进程会 abort,stderr 的 "Assertion failed" 属于预期噪音,吞掉它。
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
    // ★ 顺手把 core 上限压到 0:本机 /proc/sys/kernel/core_pattern == "core",
    //   内核会把 core 落在 cwd,于是每跑一次 `make tests` 就在仓库根目录留下
    //   一个 2MB 的 `core` 文件(实测确实留下了)。这里只影响这个注定要 abort
    //   的子进程,不改变被测行为。
    struct rlimit rl;
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    (void)setrlimit(RLIMIT_CORE, &rl);
    Config cfg;
    Editor ed(cfg);
    ed.setGhost(mkGhost("x", 0, AiSink::PanelOnly, true, Pos{0, 0}));
    _exit(0);                                  // assert 没触发 -> 用 0 退出报失败
  }
  if (pid < 0) { std::printf("FAIL fork\n"); ++g_fail; return; }
  int st = 0;
  (void)waitpid(pid, &st, 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
    std::printf("FAIL setGhost(PanelOnly) 没有触发 assert\n");
    ++g_fail;
  }
}
#endif

// =====================================================================
// 2. 一次动作一次撤销:8 行 ghost
// =====================================================================
static void testGhostSingleUndo() {
  Config cfg;
  Editor ed(cfg);
  setLines(ed, {"int main() {", "}"});
  ed.setViewSize(80, 24);
  ed.setCursor(Pos{0, 12});
  ed.setExpectedGen(11);

  std::string ghost;
  for (int i = 0; i < 8; ++i) {
    ghost += "\n  line" + std::to_string(i) + "();";
  }
  const std::string before = ed.buffer().text();
  const int before_lines = ed.buffer().lineCount();

  Ghost g = mkGhost(ghost, 11, AiSink::GhostText, true, ed.cursor());
  ed.setGhost(g);
  CHECK_EQI(ed.ghost().lineCount(), 9);        // 8 个 '\n' -> 9 段
  CHECK(ed.acceptGhost());
  CHECK_EQI(ed.buffer().lineCount(), before_lines + 8);
  CHECK(ed.ghost().empty());                   // 接受后 ghost 被清
  CHECK_EQS(ed.buffer().line(1), std::string("  line0();"));
  CHECK_EQS(ed.buffer().line(8), std::string("  line7();"));
  CHECK_POS(ed.cursor(), 8, 10);
  CHECK(!ed.acceptGhost());                    // 已清空,再按 Tab 不重复插入

  // ★ 单次 undo 完全回退
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);
  CHECK_EQI(ed.buffer().lineCount(), before_lines);
  CHECK(!ed.buffer().canUndo());
  // redo 也是一次
  CHECK(ed.redo());
  CHECK_EQI(ed.buffer().lineCount(), before_lines + 8);
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);
}

// =====================================================================
// 3. 光标与 UTF-8 / Tab
// =====================================================================
static void testCursorUtf8() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);

  // "中文abc" = 3+3+1+1+1 = 9 字节;合法起始字节只有 0,3,6,7,8,9
  setLines(ed, {"\xE4\xB8\xAD\xE6\x96\x87" "abc"});
  CHECK_EQI(ed.buffer().lineLen(0), 9);
  const int ok[] = {0, 3, 6, 7, 8, 9};
  int idx = 0;
  for (; idx < 5; ++idx) {
    CHECK_EQI(ed.cursor().col, ok[idx]);
    ed.moveRight();
  }
  CHECK_EQI(ed.cursor().col, 9);
  ed.moveRight();                              // 行尾且只有一行:不动
  CHECK_EQI(ed.cursor().col, 9);
  for (idx = 5; idx > 0; --idx) {
    CHECK_EQI(ed.cursor().col, ok[idx]);
    ed.moveLeft();
  }
  CHECK_EQI(ed.cursor().col, 0);
  ed.moveLeft();
  CHECK_EQI(ed.cursor().col, 0);

  // emoji(4 字节)+ Tab 混排:U+1F600 = F0 9F 98 80
  setLines(ed, {"a\t\xF0\x9F\x98\x80" "b"});   // a Tab emoji b = 1+1+4+1 = 7
  CHECK_EQI(ed.buffer().lineLen(0), 7);
  const int ok2[] = {0, 1, 2, 6, 7};
  for (int i = 0; i < 4; ++i) {
    CHECK_EQI(ed.cursor().col, ok2[i]);
    ed.moveRight();
  }
  CHECK_EQI(ed.cursor().col, 7);
  for (int i = 4; i > 0; --i) {
    CHECK_EQI(ed.cursor().col, ok2[i]);
    ed.moveLeft();
  }

  // 左右移动跨行
  setLines(ed, {"ab", "\xE4\xB8\xAD"});
  ed.setCursor(Pos{0, 2});
  ed.moveRight();
  CHECK_POS(ed.cursor(), 1, 0);
  ed.moveRight();
  CHECK_POS(ed.cursor(), 1, 3);                // 整个汉字一步跨过
  ed.moveLeft();
  CHECK_POS(ed.cursor(), 1, 0);
  ed.moveLeft();
  CHECK_POS(ed.cursor(), 0, 2);

  // 显示列换算:Tab 展开到制表位、宽字符 2 格
  setLines(ed, {"\t\xE4\xB8\xAD" "x"});        // Tab 中 x
  ed.setCursor(Pos{0, 0});
  CHECK_EQI(ed.cursorDisplayCol(), 0);
  ed.setCursor(Pos{0, 1});
  CHECK_EQI(ed.cursorDisplayCol(), 4);
  ed.setCursor(Pos{0, 4});
  CHECK_EQI(ed.cursorDisplayCol(), 6);
}

static void testDesiredCol() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  setLines(ed, {"aaaaaaaaaa", "bb", "cccccccccc"});

  ed.setCursor(Pos{0, 8});
  CHECK_EQI(ed.desiredDisplayCol(), 8);
  ed.moveDown();
  CHECK_POS(ed.cursor(), 1, 2);                // 短行:落到行尾
  CHECK_EQI(ed.desiredDisplayCol(), 8);        // ★ desired 不丢
  ed.moveDown();
  CHECK_POS(ed.cursor(), 2, 8);                // ★ 回到长行,列恢复
  CHECK_EQI(ed.desiredDisplayCol(), 8);
  ed.moveUp();
  CHECK_POS(ed.cursor(), 1, 2);
  ed.moveUp();
  CHECK_POS(ed.cursor(), 0, 8);

  // 左右移动会重置 desired
  ed.moveLeft();
  CHECK_EQI(ed.desiredDisplayCol(), 7);
  ed.moveDown();
  CHECK_POS(ed.cursor(), 1, 2);
  ed.moveDown();
  CHECK_POS(ed.cursor(), 2, 7);

  // 上下穿过宽字符:落在汉字中间时向左吸附到字符起始字节
  setLines(ed, {"abcdefg", "\t\xE4\xB8\xAD" "x", "abcdefg"});
  ed.setCursor(Pos{0, 5});                     // dc = 5
  CHECK_EQI(ed.desiredDisplayCol(), 5);
  ed.moveDown();
  // "\t中x":dc 0->\t, 4..5->中, 6->x。dc=5 落在“中”的后半格 -> 吸附到 byte 1
  CHECK_POS(ed.cursor(), 1, 1);
  CHECK_EQI(ed.desiredDisplayCol(), 5);
  ed.moveDown();
  CHECK_POS(ed.cursor(), 2, 5);                // ★ 穿过宽字符行后列不丢

  // 第 0 行按上:到行首;最后一行按下:到行尾
  ed.setCursor(Pos{0, 4});
  ed.moveUp();
  CHECK_POS(ed.cursor(), 0, 0);
  CHECK_EQI(ed.desiredDisplayCol(), 0);
  ed.setCursor(Pos{2, 3});
  ed.moveDown();
  CHECK_POS(ed.cursor(), 2, 7);
}

static void testWordMove() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  setLines(ed, {"foo bar_baz  qux(1)"});
  const int fwd[] = {4, 13, 16, 17, 18, 19};
  for (int i = 0; i < 6; ++i) {
    ed.moveWordRight();
    CHECK_EQI(ed.cursor().col, fwd[i]);
  }
  ed.moveWordRight();                          // 行尾,只有一行
  CHECK_EQI(ed.cursor().col, 19);
  const int back[] = {18, 17, 16, 13, 4, 0};
  for (int i = 0; i < 6; ++i) {
    ed.moveWordLeft();
    CHECK_EQI(ed.cursor().col, back[i]);
  }
  ed.moveWordLeft();
  CHECK_EQI(ed.cursor().col, 0);

  // 中文整段算一个词,且绝不停在字节中间
  setLines(ed, {"\xE4\xB8\xAD\xE6\x96\x87" " abc"});   // 中文 abc
  ed.moveWordRight();
  CHECK_EQI(ed.cursor().col, 7);               // 6 + 1 个空格
  ed.moveWordLeft();
  CHECK_EQI(ed.cursor().col, 0);

  // 跨行
  setLines(ed, {"ab", "cd"});
  ed.setCursor(Pos{0, 2});
  ed.moveWordRight();
  CHECK_POS(ed.cursor(), 1, 0);
  ed.moveWordLeft();
  CHECK_POS(ed.cursor(), 0, 2);
}

static void testHomeEnd() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  setLines(ed, {"    foo", "       "});
  ed.setCursor(Pos{0, 7});
  ed.moveHome();
  CHECK_EQI(ed.cursor().col, 4);               // 智能 Home:首个非空白
  ed.moveHome();
  CHECK_EQI(ed.cursor().col, 0);               // 再按:列 0
  ed.moveEnd();
  CHECK_EQI(ed.cursor().col, 7);
  ed.setCursor(Pos{1, 3});                     // 整行空白
  ed.moveHome();
  CHECK_EQI(ed.cursor().col, 0);

  setLines(ed, {"a", "b", "c"});
  ed.moveBufferEnd();
  CHECK_POS(ed.cursor(), 2, 1);
  ed.moveBufferStart();
  CHECK_POS(ed.cursor(), 0, 0);
}

// =====================================================================
// 4. 编辑操作
// =====================================================================
static void testInsertAndNewline() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  setLines(ed, {""});

  ed.insertText("i");
  ed.insertText("n");
  ed.insertText("t");
  CHECK_EQS(ed.buffer().line(0), std::string("int"));
  // 多字节字符整体插入
  ed.insertText("\xE4\xB8\xAD");
  CHECK_EQS(ed.buffer().line(0), std::string("int\xE4\xB8\xAD"));
  CHECK_EQI(ed.cursor().col, 6);
  ed.insertText("\xF0\x9F\x98\x80");
  CHECK_EQI(ed.cursor().col, 10);
  CHECK_EQI(ed.buffer().lineCount(), 1);

  // 空串插入是 no-op
  const int rev = ed.buffer().revision();
  ed.insertText("");
  CHECK_EQI(ed.buffer().revision(), rev);

  // 一次动作一次撤销:多字符 insertText 一次撤销干净
  setLines(ed, {"x"});
  ed.setCursor(Pos{0, 1});
  ed.insertText("hello");
  CHECK_EQS(ed.buffer().line(0), std::string("xhello"));
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().line(0), std::string("x"));

  // ---- Enter + 自动缩进 ----
  setLines(ed, {"    foo"});
  ed.setCursor(Pos{0, 7});
  ed.insertNewline();
  CHECK_EQI(ed.buffer().lineCount(), 2);
  CHECK_EQS(ed.buffer().line(1), std::string("    "));
  CHECK_POS(ed.cursor(), 1, 4);
  CHECK(ed.undo());                            // 回车 + 缩进 = 一次撤销
  CHECK_EQI(ed.buffer().lineCount(), 1);
  CHECK_EQS(ed.buffer().line(0), std::string("    foo"));

  // '{' 结尾多缩一级
  setLines(ed, {"  if (x) {"});
  ed.setCursor(Pos{0, 10});
  ed.insertNewline();
  CHECK_EQS(ed.buffer().line(1), std::string("      "));   // 2 + 4
  CHECK_POS(ed.cursor(), 1, 6);

  // 行尾 '{' 后面还有空白也算
  setLines(ed, {"\tif (x) {   "});
  ed.setCursor(Pos{0, 12});
  ed.insertNewline();
  CHECK_EQS(ed.buffer().line(1), std::string("\t    "));

  // 在缩进中间回车:只复制光标之前那段空白
  setLines(ed, {"    foo"});
  ed.setCursor(Pos{0, 2});
  ed.insertNewline();
  CHECK_EQS(ed.buffer().line(0), std::string("  "));
  CHECK_EQS(ed.buffer().line(1), std::string("    foo"));  // "  " + "  foo"
  CHECK_POS(ed.cursor(), 1, 2);

  // 关掉 auto_indent
  Config c2;
  c2.auto_indent = false;
  Editor e2(c2);
  e2.setViewSize(80, 24);
  setLines(e2, {"    foo"});
  e2.setCursor(Pos{0, 7});
  e2.insertNewline();
  CHECK_EQS(e2.buffer().line(1), std::string(""));
}

static void testBackspaceDelete() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);

  // 跨行合并
  setLines(ed, {"ab", "cd"});
  ed.setCursor(Pos{1, 0});
  ed.backspace();
  CHECK_EQI(ed.buffer().lineCount(), 1);
  CHECK_EQS(ed.buffer().line(0), std::string("abcd"));
  CHECK_POS(ed.cursor(), 0, 2);
  CHECK(ed.undo());
  CHECK_EQI(ed.buffer().lineCount(), 2);

  // 多字节字符整体退格
  setLines(ed, {"a\xE4\xB8\xAD"});
  ed.setCursor(Pos{0, 4});
  ed.backspace();
  CHECK_EQS(ed.buffer().line(0), std::string("a"));

  // expand_tab:纯空格缩进区按缩进宽度整体退格
  setLines(ed, {"        x"});
  ed.setCursor(Pos{0, 8});
  ed.backspace();
  CHECK_EQS(ed.buffer().line(0), std::string("    x"));
  CHECK_POS(ed.cursor(), 0, 4);
  ed.backspace();
  CHECK_EQS(ed.buffer().line(0), std::string("x"));
  CHECK_POS(ed.cursor(), 0, 0);
  ed.backspace();                              // 缓冲区起点:no-op
  CHECK_EQS(ed.buffer().line(0), std::string("x"));

  // 非对齐的空格数:退到最近的制表位
  setLines(ed, {"      x"});                   // 6 个空格
  ed.setCursor(Pos{0, 6});
  ed.backspace();
  CHECK_EQS(ed.buffer().line(0), std::string("    x"));

  // 缩进区之后的普通空格不整体退(前面有非空格)
  setLines(ed, {"a    b"});
  ed.setCursor(Pos{0, 5});
  ed.backspace();
  CHECK_EQS(ed.buffer().line(0), std::string("a   b"));

  // expand_tab = false:退一个字节
  Config c2;
  c2.expand_tab = false;
  Editor e2(c2);
  e2.setViewSize(80, 24);
  setLines(e2, {"    x"});
  e2.setCursor(Pos{0, 4});
  e2.backspace();
  CHECK_EQS(e2.buffer().line(0), std::string("   x"));

  // ---- Delete ----
  setLines(ed, {"ab", "cd"});
  ed.setCursor(Pos{0, 0});
  ed.del();
  CHECK_EQS(ed.buffer().line(0), std::string("b"));
  CHECK_POS(ed.cursor(), 0, 0);
  ed.setCursor(Pos{0, 1});
  ed.del();                                    // 行尾:与下一行合并
  CHECK_EQI(ed.buffer().lineCount(), 1);
  CHECK_EQS(ed.buffer().line(0), std::string("bcd"));

  // 多字节字符整体删除
  setLines(ed, {"\xE4\xB8\xAD" "a"});
  ed.setCursor(Pos{0, 0});
  ed.del();
  CHECK_EQS(ed.buffer().line(0), std::string("a"));

  // ★ 最后一行行尾按 Delete:什么都不发生
  setLines(ed, {"a", "bb"});
  ed.setCursor(Pos{1, 2});
  const std::string before = ed.buffer().text();
  const int rev = ed.buffer().revision();
  ed.del();
  ed.del();
  CHECK_EQS(ed.buffer().text(), before);
  CHECK_EQI(ed.buffer().revision(), rev);
  CHECK(!ed.buffer().canUndo());
}

static void testIndentUnindent() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);

  setLines(ed, {"ab"});
  ed.indent();
  CHECK_EQS(ed.buffer().line(0), std::string("    ab"));
  CHECK_POS(ed.cursor(), 0, 4);
  ed.indent();
  CHECK_EQS(ed.buffer().line(0), std::string("        ab"));
  ed.unindent();
  CHECK_EQS(ed.buffer().line(0), std::string("    ab"));
  CHECK_POS(ed.cursor(), 0, 4);
  ed.unindent();
  CHECK_EQS(ed.buffer().line(0), std::string("ab"));
  CHECK_POS(ed.cursor(), 0, 0);
  const int rev = ed.buffer().revision();
  ed.unindent();                               // 没有缩进可退:no-op
  CHECK_EQI(ed.buffer().revision(), rev);

  // 光标不在行首时 indent 补到下一个制表位
  setLines(ed, {"ab"});
  ed.setCursor(Pos{0, 1});
  ed.indent();
  CHECK_EQS(ed.buffer().line(0), std::string("a   b"));   // 1 -> 4,补 3 个空格
  CHECK_POS(ed.cursor(), 0, 4);

  // Tab 字符:unindent 去掉一个 '\t'
  setLines(ed, {"\t\tab"});
  ed.setCursor(Pos{0, 2});
  ed.unindent();
  CHECK_EQS(ed.buffer().line(0), std::string("\tab"));
  CHECK_POS(ed.cursor(), 0, 1);

  // expand_tab = false
  Config c2;
  c2.expand_tab = false;
  Editor e2(c2);
  e2.setViewSize(80, 24);
  setLines(e2, {"ab"});
  e2.indent();
  CHECK_EQS(e2.buffer().line(0), std::string("\tab"));
  e2.unindent();
  CHECK_EQS(e2.buffer().line(0), std::string("ab"));
}

static void testLineClipboard() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  const std::vector<std::string> orig = {"a", "b", "c", "d", "e"};
  setLines(ed, orig);
  const std::string before = ed.buffer().text();

  // ---- Ctrl-K 连续按累积 ----
  ed.cutLine();
  CHECK_EQS(ed.clipboard(), std::string("a\n"));
  ed.cutLine();
  ed.cutLine();
  CHECK_EQS(ed.clipboard(), std::string("a\nb\nc\n"));
  CHECK_EQI(ed.buffer().lineCount(), 2);
  CHECK_EQS(ed.buffer().line(0), std::string("d"));
  CHECK_POS(ed.cursor(), 0, 0);

  // ---- Ctrl-V 粘贴行:往返回到原样 ----
  ed.pasteLines();
  CHECK_EQS(ed.buffer().text(), before);
  CHECK_EQI(ed.buffer().lineCount(), 5);
  CHECK_POS(ed.cursor(), 3, 0);

  // 中间移动会打断累积
  setLines(ed, orig);
  ed.cutLine();
  ed.moveDown();
  ed.cutLine();
  CHECK_EQS(ed.clipboard(), std::string("c\n"));

  // 最后一行剪切:不留空行
  setLines(ed, {"a", "b"});
  ed.setCursor(Pos{1, 0});
  ed.cutLine();
  CHECK_EQI(ed.buffer().lineCount(), 1);
  CHECK_EQS(ed.buffer().line(0), std::string("a"));
  CHECK_POS(ed.cursor(), 0, 0);

  // 只剩一行时剪切:清空内容但保留那一行
  setLines(ed, {"only"});
  ed.cutLine();
  CHECK_EQI(ed.buffer().lineCount(), 1);
  CHECK_EQS(ed.buffer().line(0), std::string(""));
  CHECK_EQS(ed.clipboard(), std::string("only\n"));

  // ---- Ctrl-D 复制当前行(不改缓冲区) ----
  setLines(ed, {"aa", "bb"});
  const int rev = ed.buffer().revision();
  ed.copyLine();
  CHECK_EQS(ed.clipboard(), std::string("aa\n"));
  CHECK_EQI(ed.buffer().revision(), rev);
  ed.moveDown();
  ed.copyLine();
  CHECK_EQS(ed.clipboard(), std::string("bb\n"));
  ed.copyLine();                               // 连续按累积
  CHECK_EQS(ed.clipboard(), std::string("bb\nbb\n"));

  // setClipboard 的非行模式粘贴:插在光标处
  setLines(ed, {"xy"});
  ed.setClipboard("Q");
  ed.setCursor(Pos{0, 1});
  ed.pasteLines();
  CHECK_EQS(ed.buffer().line(0), std::string("xQy"));
  CHECK_POS(ed.cursor(), 0, 2);

  // 一次动作一次撤销
  setLines(ed, {"a", "b", "c"});
  ed.cutLine();
  ed.cutLine();
  CHECK_EQI(ed.buffer().lineCount(), 1);
  CHECK(ed.undo());
  CHECK(ed.undo());
  CHECK_EQI(ed.buffer().lineCount(), 3);

  // 空剪贴板粘贴:no-op
  Config c3;
  Editor e3(c3);
  setLines(e3, {"z"});
  const int r3 = e3.buffer().revision();
  e3.pasteLines();
  CHECK_EQI(e3.buffer().revision(), r3);
}

// =====================================================================
// 5. 视口
// =====================================================================
static void testViewport() {
  Config cfg;
  Editor ed(cfg);
  std::vector<std::string> many;
  for (int i = 0; i < 200; ++i) many.push_back("line" + std::to_string(i));
  setLines(ed, many);
  ed.setViewSize(40, 10);

  // 顶部:top 不为负,也不早滚
  CHECK_EQI(ed.viewport().top, 0);
  for (int i = 0; i < 7; ++i) ed.moveDown();   // 到第 7 行
  CHECK_POS(ed.cursor(), 7, 0);
  CHECK_EQI(ed.viewport().top, 0);
  ed.moveDown();                               // 第 8 行:需要下边距 2 -> top=1
  CHECK_EQI(ed.viewport().top, 1);
  CHECK(ed.cursor().line - ed.viewport().top <= ed.viewport().h - 1);

  // 向上滚:保留 2 行上边距
  ed.setCursor(Pos{50, 0});
  const int top50 = ed.viewport().top;
  CHECK(50 - top50 >= 2);
  CHECK(top50 + 10 - 1 - 50 >= 2);

  // ★ 文件末尾:top 贴到 maxtop,不留空屏
  ed.moveBufferEnd();
  CHECK_POS(ed.cursor(), 199, 7);
  CHECK_EQI(ed.viewport().top, 190);           // 200 - 10
  CHECK(ed.cursor().line >= ed.viewport().top);
  CHECK(ed.cursor().line <= ed.viewport().top + ed.viewport().h - 1);

  ed.moveBufferStart();
  CHECK_EQI(ed.viewport().top, 0);

  // ★ 超长行右端:横向按 8 列跳步,距右边缘至少 4 列
  setLines(ed, {std::string(200, 'a'), "short"});
  ed.setViewSize(40, 10);
  CHECK_EQI(ed.viewport().left, 0);
  ed.moveEnd();
  CHECK_EQI(ed.cursorDisplayCol(), 200);
  const Viewport& v = ed.viewport();
  CHECK_EQI(v.left % 8, 0);                    // 8 列跳步对齐
  CHECK(v.left <= 200);
  CHECK(200 <= v.left + v.w - 1 - 4);           // 距右边缘 >= 4 列
  CHECK_EQI(v.left, 168);
  ed.moveHome();
  CHECK_EQI(ed.viewport().left, 0);            // 回到行首:left 归零

  // 逐格右移时 left 应当每 8 列跳一次(不是每列滚一格)
  ed.setCursor(Pos{0, 0});
  int changes = 0, last = ed.viewport().left;
  for (int i = 0; i < 64; ++i) {
    ed.moveRight();
    if (ed.viewport().left != last) { ++changes; last = ed.viewport().left; }
  }
  CHECK(changes > 0);
  CHECK(changes <= 64 / 8 + 1);

  // 换到短行:left 必须能回退到能看见光标的位置
  ed.setCursor(Pos{0, 200});
  CHECK(ed.viewport().left > 0);
  ed.setCursor(Pos{1, 5});
  CHECK(ed.viewport().left <= 5);

  // scrollBy
  setLines(ed, many);
  ed.setViewSize(40, 10);
  ed.scrollBy(50);
  CHECK_EQI(ed.viewport().top, 50);
  CHECK(ed.cursor().line >= 50);
  ed.scrollBy(-1000);
  CHECK_EQI(ed.viewport().top, 0);
  ed.scrollBy(100000);
  CHECK_EQI(ed.viewport().top, 190);
}

static void testDegenerateViewport() {
  Config cfg;
  Editor ed(cfg);
  std::vector<std::string> many;
  for (int i = 0; i < 30; ++i) many.push_back(std::string(50, 'x'));
  setLines(ed, many);

  const int sizes[][2] = {{1, 1}, {0, 0}, {1, 0}, {0, 1}, {2, 2}, {3, 1}, {-5, -5}};
  for (const auto& s : sizes) {
    ed.setViewSize(s[0], s[1]);
    ed.moveBufferEnd();
    ed.moveBufferStart();
    ed.setCursor(Pos{15, 25});
    ed.moveDown();
    ed.moveUp();
    ed.moveRight();
    ed.moveLeft();
    ed.movePageDown();
    ed.movePageUp();
    ed.scrollBy(3);
    ed.scrollBy(-3);
    ed.insertText("q");
    ed.insertNewline();
    ed.backspace();
    ed.del();
    ed.indent();
    ed.unindent();
    ed.cutLine();
    ed.pasteLines();
    // 不变式:top/left 非负,且(有可见区时)光标在可见区内
    CHECK(ed.viewport().top >= 0);
    CHECK(ed.viewport().left >= 0);
    CHECK(ed.viewport().top <= ed.buffer().lineCount() - 1);
    if (ed.viewport().h > 0) {
      CHECK(ed.cursor().line >= ed.viewport().top);
      CHECK(ed.cursor().line <= ed.viewport().top + ed.viewport().h - 1);
    }
    if (ed.viewport().w > 0) {
      CHECK(ed.cursorDisplayCol() >= ed.viewport().left);
    }
  }
}

// =====================================================================
// 6. 跳行 / 查找 / 越界与退化输入
// =====================================================================
static void testGotoAndFind() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  setLines(ed, {"hello world", "hello again", "goodbye", "\xE4\xB8\xAD\xE6\x96\x87"});

  // ---- gotoLine:1 起,越界与负数都要夹住 ----
  ed.gotoLine(3);
  CHECK_POS(ed.cursor(), 2, 0);
  ed.gotoLine(1, 6);
  CHECK_POS(ed.cursor(), 0, 6);
  ed.gotoLine(-5);
  CHECK_POS(ed.cursor(), 0, 0);
  ed.gotoLine(0);
  CHECK_POS(ed.cursor(), 0, 0);
  ed.gotoLine(9999);
  CHECK_EQI(ed.cursor().line, 3);
  ed.gotoLine(2, 100000);                      // 列越界 -> 夹到行尾
  CHECK_POS(ed.cursor(), 1, 11);
  ed.gotoLine(4, 1);                           // 落在汉字中间 -> 吸附到起始字节
  CHECK_POS(ed.cursor(), 3, 0);
  ed.gotoLine(4, -3);
  CHECK_POS(ed.cursor(), 3, 0);

  // ---- find ----
  ed.setCursor(Pos{0, 0});
  CHECK(ed.find("hello", true, true));         // 从光标后找 -> 第 2 行
  CHECK_POS(ed.cursor(), 1, 0);
  CHECK_EQS(ed.lastSearch(), std::string("hello"));
  CHECK(ed.find("hello", true, false));        // 从头找 -> 第 1 行
  CHECK_POS(ed.cursor(), 0, 0);
  CHECK(ed.find("world", true, true));
  CHECK_POS(ed.cursor(), 0, 6);
  CHECK(ed.find("goodbye", true, true));
  CHECK_POS(ed.cursor(), 2, 0);

  // 回绕一次
  ed.setCursor(Pos{2, 0});
  CHECK(ed.find("hello", true, true));
  CHECK_POS(ed.cursor(), 0, 0);

  // 反向
  ed.setCursor(Pos{1, 0});
  CHECK(ed.find("hello", false, true));
  CHECK_POS(ed.cursor(), 0, 0);
  ed.setCursor(Pos{1, 5});
  CHECK(ed.find("hello", false, true));
  CHECK_POS(ed.cursor(), 1, 0);
  ed.setCursor(Pos{0, 0});
  CHECK(ed.find("hello", false, true));        // 回绕到后面
  CHECK_POS(ed.cursor(), 1, 0);
  CHECK(ed.find("goodbye", false, false));
  CHECK_POS(ed.cursor(), 2, 0);

  // UTF-8 needle
  CHECK(ed.find("\xE6\x96\x87", true, false));
  CHECK_POS(ed.cursor(), 3, 3);

  // 找不到
  CHECK(!ed.find("nonexistent", true, true));
  CHECK(!ed.find("nonexistent", false, true));

  // ★ 空串:返回 false 且不改 lastSearch、不动光标
  ed.setLastSearch("keep");
  const Pos save = ed.cursor();
  CHECK(!ed.find("", true, true));
  CHECK(!ed.find("", false, false));
  CHECK_EQS(ed.lastSearch(), std::string("keep"));
  CHECK_POS(ed.cursor(), save.line, save.col);

  // 单行缓冲区上的回绕不许死循环
  setLines(ed, {"aXa"});
  ed.setCursor(Pos{0, 2});
  CHECK(ed.find("X", true, true));
  CHECK_POS(ed.cursor(), 0, 1);
  CHECK(ed.find("X", false, true));
  CHECK_POS(ed.cursor(), 0, 1);
}

static void testEmptyBuffer() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  CHECK_EQI(ed.buffer().lineCount(), 1);
  CHECK_EQS(ed.buffer().line(0), std::string(""));

  // 各种操作都不许崩,状态保持合法
  ed.moveLeft();  ed.moveRight(); ed.moveUp(); ed.moveDown();
  ed.moveWordLeft(); ed.moveWordRight();
  ed.moveHome(); ed.moveEnd();
  ed.movePageUp(); ed.movePageDown();
  ed.moveBufferStart(); ed.moveBufferEnd();
  ed.backspace(); ed.del(); ed.unindent();
  ed.copyLine();
  CHECK_POS(ed.cursor(), 0, 0);
  CHECK_EQI(ed.buffer().lineCount(), 1);
  CHECK(!ed.buffer().canUndo());
  CHECK(!ed.undo());
  CHECK(!ed.redo());
  CHECK(!ed.find("x", true, true));
  CHECK(!ed.acceptGhost());

  ed.setClipboard("");                         // 上面 copyLine 留下了内容,先清掉
  ed.cutLine();
  CHECK_EQI(ed.buffer().lineCount(), 1);
  CHECK_EQS(ed.clipboard(), std::string("\n"));
  ed.pasteLines();
  CHECK_EQI(ed.buffer().lineCount(), 2);

  Config c2;
  Editor e2(c2);
  e2.setViewSize(80, 24);
  e2.indent();
  CHECK_EQS(e2.buffer().line(0), std::string("    "));
  e2.insertNewline();
  CHECK_EQI(e2.buffer().lineCount(), 2);
  CHECK_EQS(e2.buffer().line(1), std::string("    "));
  CHECK(e2.undo());
  CHECK_EQI(e2.buffer().lineCount(), 1);
  CHECK(e2.undo());
  CHECK_EQS(e2.buffer().line(0), std::string(""));
  CHECK(!e2.undo());
}

// =====================================================================
// 7. 高亮缓存失效 / 语言推导 / 文件读写
// =====================================================================
static void testHighlightInvalidation() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  std::vector<std::string> ls;
  for (int i = 0; i < 60; ++i) ls.push_back("int x" + std::to_string(i) + " = 0;");
  setLines(ed, ls);

  // 先把缓存推到第 50 行
  (void)ed.hlCache().entryState(ed.buffer(), ed.highlighter(), 50);
  CHECK(ed.hlCache().validUpTo() >= 51);

  // 编辑第 10 行 -> 缓存必须从第 10 行起失效
  ed.setCursor(Pos{10, 0});
  ed.insertText("/");                          // 不改行数,走快路径
  CHECK(ed.hlCache().validUpTo() <= 11);

  // 插入行 -> resize 整体作废
  (void)ed.hlCache().entryState(ed.buffer(), ed.highlighter(), 50);
  ed.setCursor(Pos{20, 0});
  ed.insertNewline();
  CHECK(ed.hlCache().validUpTo() <= 21);
  CHECK_EQI(ed.buffer().lineCount(), 61);

  // 真敲一个块注释开头,后续行的行首状态必须变成 ST_BLOCKCOMMENT
  setLines(ed, {"a;", "b;", "c;"});
  ed.setCursor(Pos{0, 0});
  ed.insertText("/");
  ed.insertText("*");
  CHECK_EQI(ed.hlCache().entryState(ed.buffer(), ed.highlighter(), 1), ST_BLOCKCOMMENT);
  CHECK_EQI(ed.hlCache().entryState(ed.buffer(), ed.highlighter(), 2), ST_BLOCKCOMMENT);
  // 连续单字符插入落在 800ms 合并窗口内 -> 一组,一次 undo 就回到 "a;"
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().line(0), std::string("a;"));
  CHECK_EQI(ed.hlCache().entryState(ed.buffer(), ed.highlighter(), 1), ST_NONE);

  // spansFor 覆盖整行且不越界
  std::vector<Span> sp;
  ed.hlCache().spansFor(ed.buffer(), ed.highlighter(), 0, sp);
  int total = 0;
  for (const Span& s : sp) total += s.len;
  CHECK_EQI(total, ed.buffer().lineLen(0));
}

static void testLangAndFile() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  // 未命名缓冲区按 C++ 高亮
  CHECK_EQI((int)ed.highlighter().lang(), (int)Lang::Cpp);

  const std::string dir = util::tempDir();
  const std::string path = dir + "/cppide_test_editor_" +
                           std::to_string((long)getpid()) + ".c";
  std::string err;
  {
    // 先造一个文件
    CHECK(util::writeFileAtomic(path, "int a;\nint b;\n", err));
  }
  CHECK(ed.loadFile(path, err));
  CHECK_EQI(ed.buffer().lineCount(), 2);
  CHECK_EQI((int)ed.highlighter().lang(), (int)Lang::C);
  CHECK_POS(ed.cursor(), 0, 0);
  CHECK(!ed.buffer().dirty());

  ed.moveBufferEnd();
  ed.insertText("x");
  CHECK(ed.buffer().dirty());
  CHECK(ed.saveFile(path, err));
  CHECK(!ed.buffer().dirty());

  // 另存为 .cpp:必须先 setPath,语言随之变
  const std::string path2 = path.substr(0, path.size() - 2) + ".cpp";
  CHECK(ed.saveFile(path2, err));
  CHECK_EQS(ed.buffer().path(), path2);
  CHECK_EQI((int)ed.highlighter().lang(), (int)Lang::Cpp);
  CHECK(!ed.buffer().dirty());

  // 读不存在的文件:失败但不崩,缓冲区不变
  const std::string keep = ed.buffer().text();
  CHECK(!ed.loadFile(dir + "/cppide_no_such_file_zzz", err));
  CHECK(!err.empty());
  CHECK_EQS(ed.buffer().text(), keep);

  ::unlink(path.c_str());
  ::unlink(path2.c_str());
}

// =====================================================================
// 8. 一次动作 = 一次撤销 的整体核对
// =====================================================================
static void testOneActionOneUndo() {
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(80, 24);
  const std::vector<std::string> orig = {"void f() {", "    int a = 1;", "}"};
  setLines(ed, orig);
  const std::string before = ed.buffer().text();

  struct Step { const char* name; };
  // 每个动作都应该正好一次 undo 回到 before
  ed.setCursor(Pos{1, 16});
  ed.insertNewline();
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);

  ed.setCursor(Pos{1, 4});
  ed.indent();
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);

  ed.setCursor(Pos{1, 8});
  ed.unindent();
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);

  ed.setCursor(Pos{2, 0});
  ed.backspace();
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);

  ed.setCursor(Pos{0, 10});
  ed.del();
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);

  ed.setCursor(Pos{1, 0});
  ed.cutLine();
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);

  ed.setClipboard("A\nB\n");
  ed.setCursor(Pos{1, 0});
  ed.pasteLines();
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);

  ed.setCursor(Pos{1, 4});
  ed.insertText("paste me");
  CHECK(ed.undo());
  CHECK_EQS(ed.buffer().text(), before);
  CHECK(!ed.buffer().canUndo());
  (void)sizeof(Step);
}

// =====================================================================
static void testFuzzInvariants() {
  // 随机敲一通,断言不变式:光标永在 UTF-8 边界、永在合法范围、视口自洽。
  Config cfg;
  Editor ed(cfg);
  ed.setViewSize(37, 9);
  setLines(ed, {"int main() {", "    \xE4\xB8\xAD\xE6\x96\x87 = \"x\";",
                "\t\xF0\x9F\x98\x80 // c", "}"});
  unsigned seed = 12345u;
  const char* chars[] = {"a", " ", "{", "}", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80", "\t", "/"};
  for (int i = 0; i < 4000; ++i) {
    seed = seed * 1103515245u + 12345u;
    switch ((seed >> 16) % 22) {
      case 0:  ed.moveLeft(); break;
      case 1:  ed.moveRight(); break;
      case 2:  ed.moveUp(); break;
      case 3:  ed.moveDown(); break;
      case 4:  ed.moveWordLeft(); break;
      case 5:  ed.moveWordRight(); break;
      case 6:  ed.moveHome(); break;
      case 7:  ed.moveEnd(); break;
      case 8:  ed.insertText(chars[(seed >> 8) % 8]); break;
      case 9:  ed.insertNewline(); break;
      case 10: ed.backspace(); break;
      case 11: ed.del(); break;
      case 12: ed.indent(); break;
      case 13: ed.unindent(); break;
      case 14: ed.cutLine(); break;
      case 15: ed.pasteLines(); break;
      case 16: ed.copyLine(); break;
      case 17: (void)ed.undo(); break;
      case 18: (void)ed.redo(); break;
      case 19: ed.scrollBy((int)((seed >> 4) % 21) - 10); break;
      case 20: ed.gotoLine((int)((seed >> 4) % 13) - 3); break;
      default: (void)ed.find("\xE4\xB8\xAD", ((seed >> 4) & 1) != 0, true); break;
    }
    const Pos c = ed.cursor();
    if (c.line < 0 || c.line >= ed.buffer().lineCount()) { CHECK(false); break; }
    const std::string& l = ed.buffer().line(c.line);
    if (c.col < 0 || c.col > (int)l.size()) { CHECK(false); break; }
    // ★ 永不落进 UTF-8 字符中间
    if (c.col < (int)l.size() &&
        util::utf8IsContinuation((unsigned char)l[(size_t)c.col])) {
      CHECK(false);
      break;
    }
    if (ed.viewport().top < 0 || ed.viewport().left < 0) { CHECK(false); break; }
    if (ed.viewport().top > ed.buffer().lineCount() - 1) { CHECK(false); break; }
    if (ed.viewport().h > 0 &&
        (c.line < ed.viewport().top ||
         c.line > ed.viewport().top + ed.viewport().h - 1)) {
      CHECK(false);
      break;
    }
    // ghost 在整个过程里从未被设置过 -> 永远不可接受
    if (ed.acceptGhost()) { CHECK(false); break; }
  }
  CHECK(ed.buffer().lineCount() >= 1);
}

int main() {
  if (!setlocale(LC_ALL, "")) setlocale(LC_ALL, "C.UTF-8");
  if (util::charDisplayWidth(0x4E2D) != 2) {
    if (!setlocale(LC_ALL, "C.UTF-8")) setlocale(LC_ALL, "C.utf8");
  }
  // 本测试的宽字符断言以“汉字宽 2”为前提;拿不到 UTF-8 locale 就直说。
  if (util::charDisplayWidth(0x4E2D) != 2) {
    std::printf("test_editor: 环境缺少 UTF-8 locale,宽字符相关断言无意义\n");
    return 1;
  }

  testGhostSecurity();
#ifndef NDEBUG
  testSetGhostAssertFires();
#endif
  testGhostSingleUndo();
  testCursorUtf8();
  testDesiredCol();
  testWordMove();
  testHomeEnd();
  testInsertAndNewline();
  testBackspaceDelete();
  testIndentUnindent();
  testLineClipboard();
  testViewport();
  testDegenerateViewport();
  testGotoAndFind();
  testEmptyBuffer();
  testHighlightInvalidation();
  testLangAndFile();
  testOneActionOneUndo();
  testFuzzInvariants();

  if (g_fail != 0) {
    std::printf("test_editor: FAILED (%d/%d)\n", g_fail, g_checks);
    return 1;
  }
#ifdef NDEBUG
  std::printf("test_editor: OK (%d checks, NDEBUG)\n", g_checks);
#else
  std::printf("test_editor: OK (%d checks, assert 开启)\n", g_checks);
#endif
  return 0;
}

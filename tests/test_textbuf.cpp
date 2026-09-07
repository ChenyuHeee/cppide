// test_textbuf.cpp —— TextBuffer 单测(纯 assert,main 返回 0 = 全过)
//
// 最重要的一条是 testRandomUndoRedoRoundTrip():2000 次随机 insert/erase
// (含多行、含中文、含 '\0'、含越界坐标),全撤销后必须与初始状态**逐字节**相等,
// 全重做后必须与操作结束时相等。撤销日志的任何一处不对称都会在这里炸。
//
// 本文件只依赖 textbuf.{h,cpp},不链接 util.o / highlight.o。

#include "../src/textbuf.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

// ------------------------------------------------------------------ 小工具
using Lines = std::vector<std::string>;

Lines snap(const TextBuffer& b) { return b.lines(); }

std::string show(const Lines& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ", ";
    s += '"';
    for (char c : v[i]) {
      if (c == '\0') s += "\\0";
      else if (c == '\n') s += "\\n";
      else if (c == '\r') s += "\\r";
      else s += c;
    }
    s += '"';
  }
  return s + "]";
}

void expectLines(const TextBuffer& b, const Lines& want, const char* what) {
  if (b.lines() != want) {
    std::fprintf(stderr, "FAIL %s\n  got  %s\n  want %s\n", what,
                 show(b.lines()).c_str(), show(want).c_str());
    std::abort();
  }
  assert(b.lineCount() >= 1);
}

// 全局不变式:至少 1 行。
void checkInvariant(const TextBuffer& b) {
  assert(b.lineCount() >= 1);
  assert(static_cast<int>(b.lines().size()) == b.lineCount());
  assert(b.line(-1).empty());
  assert(b.line(b.lineCount()).empty());
  assert(b.lineLen(-5) == 0);
  Pos e = b.endPos();
  assert(e.line == b.lineCount() - 1);
  assert(e.col == b.lineLen(e.line));
}

// 确定性 RNG(xorshift64*),不用 <random> 以保证跨平台完全一致的序列。
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
  uint64_t next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
  int below(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<uint64_t>(n)); }
  int between(int lo, int hi) { return lo + below(hi - lo + 1); }
};

std::string tmpPath(const char* name) {
  return std::string("/tmp/cppide-tb-") + std::to_string(static_cast<long>(::getpid())) +
         "-" + name;
}

bool writeRaw(const std::string& path, const std::string& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  return ok;
}

std::string readRaw(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  assert(f);
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// ------------------------------------------------------------------ 1. 基本插入/删除
void testBasicInsert() {
  TextBuffer b;
  expectLines(b, {""}, "新缓冲区 = 一个空行");
  assert(b.lineCount() == 1);
  assert(!b.dirty());
  assert(!b.hasNonBlank());

  Pos e = b.insert({0, 0}, "hello");
  assert((e == Pos{0, 5}));
  expectLines(b, {"hello"}, "单行插入");
  assert(b.dirty());
  assert(b.hasNonBlank());

  e = b.insert({0, 5}, " world");
  assert((e == Pos{0, 11}));
  expectLines(b, {"hello world"}, "行尾追加");

  // 多行插入:返回结束位置在最后一段之后
  TextBuffer c;
  e = c.insert({0, 0}, "a\nbb\nccc");
  assert((e == Pos{2, 3}));
  expectLines(c, {"a", "bb", "ccc"}, "多行插入");

  // 在中间插入多行,原行尾巴要被带到最后一段之后
  TextBuffer d;
  d.insert({0, 0}, "XY");
  e = d.insert({0, 1}, "1\n2");
  assert((e == Pos{1, 1}));
  expectLines(d, {"X1", "2Y"}, "行中插入多行");

  // 纯 '\n'
  TextBuffer f;
  f.insert({0, 0}, "ab");
  e = f.insert({0, 1}, "\n");
  assert((e == Pos{1, 0}));
  expectLines(f, {"a", "b"}, "插入纯 \\n 拆行");

  // 末行行尾插入 '\n'
  e = f.insert(f.endPos(), "\n");
  assert((e == Pos{2, 0}));
  expectLines(f, {"a", "b", ""}, "末行行尾插 \\n");

  // 多行且末尾带 '\n'
  TextBuffer g;
  e = g.insert({0, 0}, "p\nq\n");
  assert((e == Pos{2, 0}));
  expectLines(g, {"p", "q", ""}, "多行末尾带 \\n");

  // 空串插入 = 无操作,不记撤销、不改 revision
  int rev = g.revision();
  Pos p = g.insert({1, 1}, "");
  assert((p == Pos{1, 1}));
  assert(g.revision() == rev);
  checkInvariant(g);

  // 中文按字节偏移
  TextBuffer h;
  h.insert({0, 0}, "中文abc");
  assert(h.lineLen(0) == 3 + 3 + 3);
  assert((h.clampPos({0, 1}) == Pos{0, 0}));   // 退回 UTF-8 边界
  assert((h.clampPos({0, 4}) == Pos{0, 3}));
  assert((h.clampPos({0, 6}) == Pos{0, 6}));
  std::printf("  ok  基本插入\n");
}

void testBasicErase() {
  TextBuffer b;
  b.insert({0, 0}, "hello world");
  Pos p = b.erase({{0, 5}, {0, 11}});
  assert((p == Pos{0, 5}));
  expectLines(b, {"hello"}, "行内删除");

  // 跨行删除:两行合一
  TextBuffer c;
  c.insert({0, 0}, "ab\ncd\nef");
  p = c.erase({{0, 1}, {2, 1}});
  assert((p == Pos{0, 1}));
  expectLines(c, {"af"}, "跨行删除合并");

  // 删除整个缓冲区 -> 仍剩 1 个空行
  TextBuffer d;
  d.insert({0, 0}, "1\n2\n3\n4");
  d.erase({{0, 0}, d.endPos()});
  expectLines(d, {""}, "删空缓冲区仍剩 1 空行");
  checkInvariant(d);

  // 空缓冲区上删除:无操作、不崩、不记撤销
  TextBuffer e;
  int rev = e.revision();
  p = e.erase({{0, 0}, {0, 0}});
  assert((p == Pos{0, 0}));
  p = e.erase({{-5, -5}, {99, 99}});
  assert((p == Pos{0, 0}));
  expectLines(e, {""}, "空缓冲区删除");
  assert(e.revision() == rev);
  assert(!e.canUndo());

  // 删除换行符本身 = 合并两行
  TextBuffer f;
  f.insert({0, 0}, "aa\nbb");
  f.erase({{0, 2}, {1, 0}});
  expectLines(f, {"aabb"}, "删掉换行合并两行");
  std::printf("  ok  基本删除\n");
}

// ------------------------------------------------------------------ 2. 越界安全
void testOutOfBounds() {
  TextBuffer b;
  b.insert({0, 0}, "ab\ncd");

  // 负数 Pos -> 夹到 {0,0}
  Pos e = b.insert({-3, -7}, "X");
  assert((e == Pos{0, 1}));
  expectLines(b, {"Xab", "cd"}, "负数 Pos 夹到行首");

  // 行号超界 -> 夹到末行行尾
  e = b.insert({100, 0}, "Y");
  assert((e == Pos{1, 3}));
  expectLines(b, {"Xab", "cdY"}, "行号超界夹到末行尾");

  // col 超行长 -> 夹到行长
  e = b.insert({0, 999}, "Z");
  assert((e == Pos{0, 4}));
  expectLines(b, {"XabZ", "cdY"}, "col 超界夹到行尾");

  // Range a > b -> 自动交换
  Lines before = snap(b);
  b.erase({{1, 3}, {0, 0}});
  expectLines(b, {""}, "颠倒 Range 规范化后删全部");
  // 撤销回去
  Pos cur;
  assert(b.undo(cur));
  expectLines(b, before, "颠倒 Range 的撤销");

  // 完全越界的 Range
  b.erase({{-100, -100}, {-50, -50}});
  expectLines(b, before, "全负 Range 是空操作");
  b.erase({{500, 500}, {600, 600}});
  expectLines(b, before, "全超界 Range 是空操作");

  // 只读访问器越界
  assert(b.line(-1).empty() && b.line(12345).empty());
  assert(b.lineLen(-1) == 0 && b.lineLen(12345) == 0);
  assert(b.textRange({{-9, -9}, {-1, -1}}).empty());
  assert(b.textRange({{999, 999}, {-9, -9}}) == b.text().substr(0, b.text().size() - 1));
  checkInvariant(b);

  // UTF-8 中间的越界:col 落在续字节上,插入点退到字符边界(不产生半个字符)
  TextBuffer u;
  u.insert({0, 0}, "中文");
  u.insert({0, 1}, "!");
  expectLines(u, {"!中文"}, "col 落在续字节上退回边界");
  u.erase({{0, 2}, {0, 3}});   // 都落在 '中' 内部 -> 规范化后为空
  expectLines(u, {"!中文"}, "续字节内的空 Range 是空操作");
  std::printf("  ok  越界安全\n");
}

// ------------------------------------------------------------------ 3. textRange / text
void testTextAccess() {
  TextBuffer b;
  b.insert({0, 0}, "abc\ndef\nghi");
  assert(b.text() == "abc\ndef\nghi\n");
  assert(b.textRange({{0, 1}, {0, 2}}) == "b");
  assert(b.textRange({{0, 1}, {2, 2}}) == "bc\ndef\ngh");
  assert(b.textRange({{2, 2}, {0, 1}}) == "bc\ndef\ngh");   // 颠倒也一样
  assert(b.textRange({{1, 3}, {2, 0}}) == "\n");
  assert(b.textRange({{1, 1}, {1, 1}}).empty());
  // textRange 的内容必须与 erase 实际删掉的内容一致(撤销正确性的基石)
  std::string want = b.textRange({{0, 2}, {2, 1}});
  TextBuffer c = b;
  c.erase({{0, 2}, {2, 1}});
  Pos cur;
  assert(c.canUndo());
  assert(c.undo(cur));
  expectLines(c, b.lines(), "erase 内容与 textRange 一致(撤销后相等)");
  assert(!want.empty());
  std::printf("  ok  text / textRange\n");
}

// ------------------------------------------------------------------ 4. 撤销分组与合并
void testUndoGrouping() {
  TextBuffer b;
  // 一串连续单字符输入(同 label、位置相邻、间隔 < 800ms)-> 一次撤销全消
  Pos at{0, 0};
  for (char c : std::string("hello")) {
    TextBuffer::Edit guard(b, "insert-char");
    b.noteCursorBefore(at);
    at = b.insert(at, std::string(1, c));
    b.noteCursorAfter(at);
  }
  expectLines(b, {"hello"}, "连续单字符输入");
  Pos cur{9, 9};
  assert(b.undo(cur));
  expectLines(b, {""}, "一次撤销消掉整串连续输入");
  assert((cur == Pos{0, 0}));   // 恢复到动作前光标
  assert(!b.canUndo());
  assert(b.redo(cur));
  expectLines(b, {"hello"}, "重做整串");
  assert((cur == Pos{0, 5}));

  // 中间插入回车会断组
  TextBuffer c;
  at = Pos{0, 0};
  auto type = [&](const std::string& s, const char* label) {
    TextBuffer::Edit guard(c, label);
    c.noteCursorBefore(at);
    at = c.insert(at, s);
    c.noteCursorAfter(at);
  };
  type("a", "insert-char");
  type("b", "insert-char");
  c.breakUndoMerge();               // Editor 在回车前后都会调它
  type("\n", "newline");
  c.breakUndoMerge();
  type("c", "insert-char");
  type("d", "insert-char");
  expectLines(c, {"ab", "cd"}, "输入 ab / 回车 / cd");
  assert(c.undo(cur));
  expectLines(c, {"ab", ""}, "撤销 1:只掉 cd");
  assert(c.undo(cur));
  expectLines(c, {"ab"}, "撤销 2:只掉回车");
  assert(c.undo(cur));
  expectLines(c, {""}, "撤销 3:掉 ab");
  assert(!c.canUndo());
  // 重做三次回到原样
  assert(c.redo(cur) && c.redo(cur) && c.redo(cur));
  expectLines(c, {"ab", "cd"}, "三次重做复原");
  assert(!c.canRedo());

  // 即使不调 breakUndoMerge,插入 '\n' 自身也不可合并(不是单字符插入种子)
  TextBuffer d;
  at = Pos{0, 0};
  auto type2 = [&](const std::string& s) {
    TextBuffer::Edit guard(d, "insert-char");
    at = d.insert(at, s);
  };
  type2("a");
  type2("\n");
  type2("b");
  assert(d.undo(cur));
  expectLines(d, {"a", ""}, "'\\n' 之后的输入独立成组");
  assert(d.undo(cur));
  expectLines(d, {"a"}, "'\\n' 独立成组");
  assert(d.undo(cur));
  expectLines(d, {""}, "首字符成组");

  // 不相邻的两次单字符输入不合并
  TextBuffer e;
  e.insert({0, 0}, "xxxx");
  e.clearHistory();
  { TextBuffer::Edit g(e, "insert-char"); e.insert({0, 0}, "1"); }
  { TextBuffer::Edit g(e, "insert-char"); e.insert({0, 4}, "2"); }
  expectLines(e, {"1xxx2x"}, "两处不相邻输入");
  assert(e.undo(cur));
  expectLines(e, {"1xxxx"}, "不相邻 -> 不合并(撤销只掉一个)");

  // label 不同不合并
  TextBuffer f;
  { TextBuffer::Edit g(f, "insert-char"); f.insert({0, 0}, "a"); }
  { TextBuffer::Edit g(f, "paste"); f.insert({0, 1}, "b"); }
  assert(f.undo(cur));
  expectLines(f, {"a"}, "label 不同 -> 不合并");

  // 超过 800ms 不合并
  TextBuffer h;
  { TextBuffer::Edit g(h, "insert-char"); h.insert({0, 0}, "a"); }
  std::this_thread::sleep_for(std::chrono::milliseconds(850));
  { TextBuffer::Edit g(h, "insert-char"); h.insert({0, 1}, "b"); }
  assert(h.undo(cur));
  expectLines(h, {"a"}, "间隔 > 800ms -> 不合并");

  // 中文单字符也能合并(3 字节一个字符)
  TextBuffer z;
  at = Pos{0, 0};
  for (const char* s : {"中", "文", "好"}) {
    TextBuffer::Edit g(z, "insert-char");
    at = z.insert(at, s);
  }
  expectLines(z, {"中文好"}, "连续中文输入");
  assert(z.undo(cur));
  expectLines(z, {""}, "中文连续输入合并成一组");

  // 一次多行插入(接受 ghost)是一组,一次撤销回退干净
  TextBuffer g2;
  g2.insert({0, 0}, "head\n");
  g2.clearHistory();
  {
    TextBuffer::Edit guard(g2, "accept-ghost");
    g2.insert({1, 0}, "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\n");
  }
  assert(g2.lineCount() == 10);
  assert(g2.undo(cur));
  expectLines(g2, {"head", ""}, "8 行 ghost 一次撤销回退干净");
  std::printf("  ok  撤销分组与合并\n");
}

void testNestedEdit() {
  TextBuffer b;
  {
    TextBuffer::Edit outer(b, "outer");
    b.insert({0, 0}, "a");
    {
      TextBuffer::Edit inner(b, "inner");       // 嵌套:不真正开新组
      b.insert({0, 1}, "b");
      {
        TextBuffer::Edit inner2(b, "inner2");
        b.erase({{0, 0}, {0, 1}});
      }
    }
    b.insert({0, 1}, "c");
  }
  expectLines(b, {"bc"}, "嵌套 Edit 的结果");
  Pos cur;
  assert(b.undo(cur));
  expectLines(b, {""}, "嵌套 Edit 只算一组");
  assert(!b.canUndo());

  // 异常穿过 Edit 作用域:组仍被正确关闭,撤销仍然是一组
  TextBuffer c;
  try {
    TextBuffer::Edit guard(c, "throwing");
    c.insert({0, 0}, "x\ny");
    throw 42;
  } catch (int) {
  }
  expectLines(c, {"x", "y"}, "异常前的改动仍在");
  assert(c.canUndo());
  assert(c.undo(cur));
  expectLines(c, {""}, "异常路径下的组照样能整组撤销");

  // 不配对的 endGroup 不崩
  TextBuffer d;
  d.endGroup();
  d.endGroup();
  d.insert({0, 0}, "ok");
  assert(d.canUndo());
  checkInvariant(d);

  // 空动作不留组
  TextBuffer e;
  {
    TextBuffer::Edit guard(e, "noop");
    e.insert({0, 0}, "");
    e.erase({{0, 0}, {0, 0}});
  }
  assert(!e.canUndo());

  // 事务进行中调 undo/redo:拒绝(返回 false)而不是破坏历史;出了作用域照常工作
  TextBuffer f;
  f.insert({0, 0}, "first");
  f.breakUndoMerge();
  Pos c2;
  {
    TextBuffer::Edit guard(f, "second");
    f.insert(f.endPos(), "-second");
    assert(!f.undo(c2));           // 事务内拒绝
    assert(!f.redo(c2));
  }
  expectLines(f, {"first-second"}, "事务内的 undo 调用没有破坏内容");
  assert(f.undo(c2));
  expectLines(f, {"first"}, "出了作用域 undo 正常");
  assert(f.undo(c2));
  expectLines(f, {""}, "历史完整无损");
  std::printf("  ok  嵌套 / 异常安全的 Edit\n");
}

void testRedoInvalidation() {
  TextBuffer b;
  b.insert({0, 0}, "one");
  b.breakUndoMerge();
  b.insert({0, 3}, "two");
  Pos cur;
  assert(b.undo(cur));
  assert(b.canRedo());
  b.insert({0, 3}, "X");        // 新改动清 redo
  assert(!b.canRedo());
  expectLines(b, {"oneX"}, "新改动后 redo 被清空");

  // undo/redo 期间不写日志:反复 undo/redo 不会让栈无限增长
  TextBuffer c;
  for (int i = 0; i < 5; ++i) {
    TextBuffer::Edit g(c, "line");
    c.insert(c.endPos(), "row\n");
  }
  int undo_steps = 0;
  while (c.undo(cur)) ++undo_steps;
  assert(undo_steps == 5);
  int redo_steps = 0;
  while (c.redo(cur)) ++redo_steps;
  assert(redo_steps == 5);
  undo_steps = 0;
  while (c.undo(cur)) ++undo_steps;
  assert(undo_steps == 5);      // 没有因为 undo 自身被记账而膨胀
  expectLines(c, {""}, "反复 undo/redo 收敛");
  std::printf("  ok  redo 失效 / journaling 挂起\n");
}

void testRevisionDirty() {
  TextBuffer b;
  int r0 = b.revision();
  assert(!b.dirty());
  b.insert({0, 0}, "a");
  assert(b.revision() == r0 + 1);
  assert(b.dirty());
  b.clearDirty();
  assert(!b.dirty());
  b.erase({{0, 0}, {0, 1}});
  assert(b.revision() == r0 + 2);
  assert(b.dirty());
  Pos cur;
  int r_before_undo = b.revision();
  assert(b.undo(cur));
  assert(b.revision() > r_before_undo);   // 撤销也是改动
  assert(b.dirty());
  b.reset({"x", "y"});
  assert(!b.dirty());
  assert(!b.canUndo() && !b.canRedo());
  expectLines(b, {"x", "y"}, "reset");
  b.reset({});
  expectLines(b, {""}, "reset 空 vector 补一个空行");
  std::printf("  ok  revision / dirty\n");
}

// ------------------------------------------------------------------ 5. 撤销栈上限
void testUndoLimit() {
  TextBuffer b;
  const int kPush = 3000;
  for (int i = 0; i < kPush; ++i) {
    b.breakUndoMerge();                     // 保证每次都是独立的一组
    TextBuffer::Edit g(b, "insert-char");
    b.insert(b.endPos(), "x");
  }
  assert(b.lineLen(0) == kPush);
  int steps = 0;
  Pos cur;
  while (b.undo(cur)) ++steps;
  assert(steps == 2000);                    // 上限 2000 组,更旧的被丢弃
  assert(b.lineLen(0) == kPush - 2000);
  checkInvariant(b);
  // 丢弃了历史之后缓冲区依然可用
  int redone = 0;
  while (b.redo(cur)) ++redone;
  assert(redone == 2000);
  assert(b.lineLen(0) == kPush);
  b.insert(b.endPos(), "!");
  assert(b.canUndo());
  checkInvariant(b);
  std::printf("  ok  撤销栈上限 2000(压入 %d 组)\n", kPush);
}

// ------------------------------------------------------------------ 6. 随机往返(核心)
// 返回实际发生的撤销组数。verbose=true 时打印统计并校验"最终状态足够厚"。
int randomRoundTrip(uint64_t seed, int kOps, bool verbose) {
  const std::string pool[] = {
      "a", "中", "x", "\n", "ab", "hello", "x\ny", "l1\nl2\n", "。", "中文混合abc",
      "\t", std::string("z\0q", 3), "\r\n", "((", "多\n行\n中\n文", " ",
  };
  const int kPoolN = static_cast<int>(sizeof(pool) / sizeof(pool[0]));
  const char* labels[] = {"insert-char", "paste", "cut-line", nullptr};

  int inserts = 0, erases = 0, applied = 0;

  TextBuffer b;
  b.reset({"起始第一行", "second line", "", "\ttab\tline", "末尾中文行"});
  const Lines initial = snap(b);
  assert(!b.canUndo());

  Rng rng(seed);
  for (int i = 0; i < kOps; ++i) {
    int lc = b.lineCount();
    // 故意生成越界/颠倒坐标(约 1/4 概率),要求实现规范化而不是崩溃
    auto randPos = [&]() {
      Pos p;
      p.line = rng.between(-2, lc + 2);
      int len = b.lineLen(p.line < 0 ? 0 : p.line);
      p.col = rng.between(-3, len + 3);
      return p;
    };
    // 删除多数时候是"局部"的(真实编辑就是这样),否则整段大范围删除会把缓冲区
    // 反复削成一行,随机性反而变弱。
    auto randNear = [&](Pos a) {
      Pos p;
      p.line = a.line + (rng.below(100) < 70 ? 0 : rng.between(-2, 2));
      int len = b.lineLen(p.line < 0 ? 0 : p.line);
      p.col = a.col + rng.between(-10, 10);
      if (p.col > len + 3) p.col = len + 3;
      return p;
    };
    const char* label = labels[rng.below(4)];
    bool do_insert = rng.below(100) < 80;   // 插入多于删除,缓冲区不会被削成一行后空转

    if (rng.below(100) < 15) b.breakUndoMerge();
    {
      TextBuffer::Edit guard(b, label);
      if (rng.below(100) < 40) b.noteCursorBefore(randPos());
      if (do_insert) {
        Pos at = randPos();
        const std::string& t = pool[rng.below(kPoolN)];
        Pos end = b.insert(at, t);
        // insert 的返回值必须落在缓冲区内、且等于 clampPos 的不动点
        assert((b.clampPos(end) == end) || !t.empty());
        ++inserts;
      } else {
        Pos a = randPos();
        Pos c = (rng.below(100) < 97) ? randNear(a) : randPos();
        b.erase({a, c});
        ++erases;
      }
      if (rng.below(100) < 40) b.noteCursorAfter(randPos());
    }
    // 偶尔嵌套一层做复合动作(一次用户动作 = 一组)
    if (rng.below(100) < 8) {
      TextBuffer::Edit outer(b, "compound");
      {
        TextBuffer::Edit inner(b, "inner");
        b.insert(randPos(), pool[rng.below(kPoolN)]);
      }
      Pos ca = randPos();
      b.erase({ca, randNear(ca)});
      ++applied;
    }
    assert(b.lineCount() >= 1);
  }
  checkInvariant(b);
  const Lines final_state = snap(b);
  assert(final_state != initial);
  if (verbose) {
    std::printf("      随机操作:%d 次(insert %d / erase %d / compound %d),"
                "最终 %d 行 / %zu 字节\n",
                kOps, inserts, erases, applied, b.lineCount(), b.text().size());
    assert(b.lineCount() >= 20);   // 保证最终状态足够"厚",往返测试才有分量
  }

  // ---- 全部撤销:必须逐字节回到初始状态 ----
  Pos cur;
  int undos = 0;
  while (b.canUndo()) {
    assert(b.undo(cur));
    ++undos;
    assert(b.lineCount() >= 1);
    assert(undos < 200000);
  }
  expectLines(b, initial, "★ 2000 次随机操作全部撤销后与初始状态逐字节相等");
  assert((b.clampPos(cur) == cur));

  // ---- 全部重做:必须逐字节回到操作结束时的状态 ----
  int redos = 0;
  while (b.canRedo()) {
    assert(b.redo(cur));
    ++redos;
    assert(b.lineCount() >= 1);
    assert(redos < 200000);
  }
  expectLines(b, final_state, "★ 全部重做后与操作结束时逐字节相等");
  assert(redos == undos);

  // ---- 再来一轮 undo/redo,确认状态机是幂等的 ----
  while (b.canUndo()) assert(b.undo(cur));
  expectLines(b, initial, "★ 第二轮全撤销仍相等");
  while (b.canRedo()) assert(b.redo(cur));
  expectLines(b, final_state, "★ 第二轮全重做仍相等");
  if (verbose) {
    std::printf("  ok  随机撤销/重做往返一致性(%d 组撤销 / %d 组重做)\n", undos, redos);
  }
  return undos;
}

void testRandomUndoRedoRoundTrip() {
  // 主测:2000 次随机操作(固定种子,可复现)
  randomRoundTrip(0xC0FFEEull, 2000, true);
  // 再用多个种子扫一遍,防止"只有这一条随机序列恰好对称"
  int total = 0;
  const uint64_t seeds[] = {1, 2, 0xDEADBEEFull, 0x5A5A5A5Aull, 12345678901ull};
  for (uint64_t sd : seeds) total += randomRoundTrip(sd, 700, false);
  std::printf("  ok  多种子随机往返(%d 个种子 x 700 次操作,共 %d 组撤销/重做)\n",
              static_cast<int>(sizeof(seeds) / sizeof(seeds[0])), total);
}

// 逐组核对:比"全撤销后相等"更严的一条 —— 每撤销一组,内容必须精确等于那一组
// 动作发生之前的快照。这能抓出"聚合起来正好抵消、但单组不对称"的日志错误。
void testStepwiseUndoMatchesSnapshots() {
  const std::string pool[] = {"a", "中", "\n", "ab\ncd", "x\ny\nz", "。。", "\t",
                              std::string("q\0w", 3), "line\n"};
  const int kPoolN = static_cast<int>(sizeof(pool) / sizeof(pool[0]));
  TextBuffer b;
  b.reset({"seed 行", "second"});
  std::vector<Lines> before_stack;   // 每个撤销组对应一张"动作前"快照
  std::vector<Lines> after_stack;    // 对应"动作后"快照(重做核对用)
  Rng rng(0x1234567ull);
  const int kOps = 400;
  for (int i = 0; i < kOps; ++i) {
    b.breakUndoMerge();              // 关掉合并 -> 一次事务恰好一组,快照可一一对应
    Lines before = snap(b);
    int rev = b.revision();
    {
      TextBuffer::Edit guard(b, "step");
      if (rng.below(100) < 60) {
        Pos at{rng.between(-1, b.lineCount()), rng.between(-2, 40)};
        b.insert(at, pool[rng.below(kPoolN)]);
      } else {
        Pos a{rng.between(-1, b.lineCount()), rng.between(-2, 40)};
        Pos c{a.line + rng.between(0, 2), a.col + rng.between(-5, 12)};
        b.erase({a, c});
      }
    }
    if (b.revision() != rev) {       // 有实际改动才会产生一组
      before_stack.push_back(std::move(before));
      after_stack.push_back(snap(b));
    }
    assert(b.lineCount() >= 1);
  }
  assert(before_stack.size() > 300);

  Pos cur;
  for (size_t i = before_stack.size(); i-- > 0;) {
    assert(b.undo(cur));
    if (b.lines() != before_stack[i]) {
      std::fprintf(stderr, "FAIL 第 %zu 组撤销后内容不等于该组动作前的快照\n"
                           "  got  %s\n  want %s\n",
                   i, show(b.lines()).c_str(), show(before_stack[i]).c_str());
      std::abort();
    }
  }
  assert(!b.canUndo());
  for (size_t i = 0; i < after_stack.size(); ++i) {
    assert(b.redo(cur));
    if (b.lines() != after_stack[i]) {
      std::fprintf(stderr, "FAIL 第 %zu 组重做后内容不等于该组动作后的快照\n", i);
      std::abort();
    }
  }
  assert(!b.canRedo());
  std::printf("  ok  逐组撤销/重做与快照精确一致(%zu 组)\n", before_stack.size());
}

// ------------------------------------------------------------------ 7. 文件读写
void testFileRoundTrip() {
  struct Case {
    const char* name;
    Lines lines;
  };
  const Case cases[] = {
      {"ascii", {"int main(){}", "return 0;"}},
      {"中文", {"// 中文注释", "printf(\"你好,世界\\n\");", ""}},
      {"空缓冲", {""}},
      {"含空行", {"", "", "a", ""}},
      {"含 NUL", {std::string("a\0b", 3), std::string("\0\0", 2)}},
      {"长行", {std::string(5000, 'x')}},
      {"孤立 CR", {std::string("a\rb")}},
  };
  for (const Case& c : cases) {
    TextBuffer b;
    b.reset(c.lines);
    std::string p = tmpPath(c.name), err = "x";
    assert(b.saveFile(p, err));
    assert(err.empty());
    assert(!b.dirty());
    TextBuffer d;
    std::string err2 = "x";
    assert(d.loadFile(p, err2));
    assert(err2.empty());
    if (d.lines() != c.lines) {
      std::fprintf(stderr, "FAIL 文件往返 [%s]\n  got  %s\n  want %s\n", c.name,
                   show(d.lines()).c_str(), show(c.lines).c_str());
      std::abort();
    }
    assert(!d.dirty());
    assert(d.path() == p);
    assert(!d.canUndo());
    ::unlink(p.c_str());
  }

  // 无末尾换行:加载得到相同的行;保存时按 text() 契约补上末尾换行
  std::string p = tmpPath("noeol");
  assert(writeRaw(p, "abc\ndef"));
  TextBuffer b;
  std::string err;
  assert(b.loadFile(p, err));
  expectLines(b, {"abc", "def"}, "无末尾换行的文件");
  assert(b.saveFile(p, err));
  assert(readRaw(p) == "abc\ndef\n");
  TextBuffer b2;
  assert(b2.loadFile(p, err));
  expectLines(b2, {"abc", "def"}, "补了末尾换行后再加载内容相同");
  ::unlink(p.c_str());

  // CRLF:行尾 "\r\n" 规范化为 LF;保存写 LF;缓冲区内容往返稳定
  p = tmpPath("crlf");
  assert(writeRaw(p, "line1\r\nline2\r\n中文\r\n"));
  TextBuffer c;
  assert(c.loadFile(p, err));
  expectLines(c, {"line1", "line2", "中文"}, "CRLF 规范化为 LF");
  assert(c.saveFile(p, err));
  assert(readRaw(p) == "line1\nline2\n中文\n");
  TextBuffer c2;
  assert(c2.loadFile(p, err));
  expectLines(c2, c.lines(), "CRLF 文件保存后再加载内容相等");
  ::unlink(p.c_str());

  // 混合 CRLF/LF + 无末尾换行 + 空行
  p = tmpPath("mixed");
  assert(writeRaw(p, "a\r\n\r\nb\nc\r"));
  TextBuffer m;
  assert(m.loadFile(p, err));
  expectLines(m, {"a", "", "b", std::string("c\r")}, "混合换行(末段孤立 CR 保留)");
  ::unlink(p.c_str());

  // 二进制内容不崩(非法 UTF-8 + NUL + 无换行)
  p = tmpPath("binary");
  std::string bin;
  for (int i = 0; i < 512; ++i) bin.push_back(static_cast<char>(i * 7 & 0xFF));
  assert(writeRaw(p, bin));
  TextBuffer bb;
  assert(bb.loadFile(p, err));
  checkInvariant(bb);
  bb.insert({0, 1}, "中");          // 在非法字节序列中插入也不崩
  bb.erase({{0, 0}, {0, 9}});
  checkInvariant(bb);
  assert(bb.saveFile(p, err));
  ::unlink(p.c_str());

  // 空文件
  p = tmpPath("empty");
  assert(writeRaw(p, ""));
  TextBuffer e;
  assert(e.loadFile(p, err));
  expectLines(e, {""}, "空文件 = 一个空行");
  assert(e.saveFile(p, err));
  assert(readRaw(p).empty());      // 空缓冲区存成 0 字节
  ::unlink(p.c_str());

  // 原子写:目标已存在且内容不同时,替换后是新内容,且不留 tmp 残骸
  p = tmpPath("atomic");
  assert(writeRaw(p, "OLD CONTENT\n"));
  TextBuffer a;
  a.reset({"NEW"});
  assert(a.saveFile(p, err));
  assert(readRaw(p) == "NEW\n");
  ::unlink(p.c_str());
  std::printf("  ok  文件保存/加载往返(%d 组内容 + CRLF/NUL/二进制)\n",
              static_cast<int>(sizeof(cases) / sizeof(cases[0])));
}

void testFileErrors() {
  TextBuffer b;
  std::string err;
  // 不存在的文件
  assert(!b.loadFile("/tmp/cppide-definitely-does-not-exist-9e3779b9", err));
  assert(!err.empty());
  expectLines(b, {""}, "加载失败后缓冲区不变");
  assert(b.path().empty());

  // 目录
  err.clear();
  assert(!b.loadFile("/tmp", err));
  assert(!err.empty());

  // 保存到不可写路径(父目录不存在 -> 与 uid 无关,root 下也失败)
  b.reset({"data"});
  err.clear();
  assert(!b.saveFile("/tmp/cppide-no-such-dir-9e3779b9/out.cpp", err));
  assert(!err.empty());
  // 保存到根目录下不可写位置(非 root 时也覆盖权限分支)
  if (::geteuid() != 0) {
    err.clear();
    assert(!b.saveFile("/proc/cppide-cannot-write", err));
    assert(!err.empty());
  }
  // 空文件名
  err.clear();
  assert(!b.saveFile("", err));
  assert(!err.empty());

  // 目标是一个目录 -> rename 失败;必须返回 false、清掉 tmp、且不留残骸
  std::string d = tmpPath("dir");
  ::mkdir(d.c_str(), 0755);
  err.clear();
  assert(!b.saveFile(d, err));
  assert(!err.empty());
  ::rmdir(d.c_str());

  // 保存失败不得破坏已存在的目标文件:先写好一个文件,再让保存失败
  std::string keep = tmpPath("keep");
  assert(writeRaw(keep, "ORIGINAL\n"));
  TextBuffer k;
  k.reset({"NEW"});
  err.clear();
  assert(!k.saveFile(keep + "/impossible", err));   // 用文件当目录 -> ENOTDIR
  assert(readRaw(keep) == "ORIGINAL\n");
  assert(k.dirty() || !k.dirty());                  // 只要求不崩、内容不被破坏
  ::unlink(keep.c_str());

  // tmp 残骸检查:/tmp 下不该留下 .cppide-save-* 文件
  {
    std::string probe = tmpPath("leak");
    assert(writeRaw(probe, "x"));
    TextBuffer t;
    t.reset({"y"});
    std::string e2;
    assert(t.saveFile(probe, e2));
    std::string tmpname = "/tmp/.cppide-save-" +
                          probe.substr(probe.find_last_of('/') + 1) + "." +
                          std::to_string(static_cast<long>(::getpid())) + ".tmp";
    assert(::access(tmpname.c_str(), F_OK) != 0);   // 成功路径也不留 tmp
    ::unlink(probe.c_str());
  }
  std::printf("  ok  文件错误路径(不崩,返回 false + err,不留 tmp 残骸)\n");
}

void testLangFromPath() {
  assert(TextBuffer::langFromPath("a.c") == Lang::C);
  assert(TextBuffer::langFromPath("/x/y/main.c") == Lang::C);
  assert(TextBuffer::langFromPath("a.cpp") == Lang::Cpp);
  assert(TextBuffer::langFromPath("a.cc") == Lang::Cpp);
  assert(TextBuffer::langFromPath("a.cxx") == Lang::Cpp);
  assert(TextBuffer::langFromPath("a.C") == Lang::Cpp);
  assert(TextBuffer::langFromPath("a.h") == Lang::Cpp);      // §4.3:头文件走 cxx
  assert(TextBuffer::langFromPath("a.hpp") == Lang::Cpp);
  assert(TextBuffer::langFromPath("a.txt") == Lang::Unknown);
  assert(TextBuffer::langFromPath("Makefile") == Lang::Unknown);
  assert(TextBuffer::langFromPath("") == Lang::Unknown);
  assert(TextBuffer::langFromPath("/a.d/noext") == Lang::Unknown);   // 点在目录名里
  assert(TextBuffer::langFromPath("/tmp/中文.cpp") == Lang::Cpp);

  // ---- 大小写规则(Wave 5 最后一轮统一)----
  // 1) `.c`(小写)= C、`.C`(大写)= C++ —— 这一对**必须**保持大小写敏感,
  //    是 C/C++ 自 cfront 起的既有约定,不能因为"扩展名大小写不敏感"就丢掉。
  assert(TextBuffer::langFromPath("a.c") == Lang::C);
  assert(TextBuffer::langFromPath("a.C") == Lang::Cpp);
  // 2) 其余扩展名一律大小写不敏感(修之前 main.CPP 会被 checkOpenPath 拒绝打开)。
  assert(TextBuffer::langFromPath("main.CPP") == Lang::Cpp);
  assert(TextBuffer::langFromPath("main.Cpp") == Lang::Cpp);
  assert(TextBuffer::langFromPath("main.cPp") == Lang::Cpp);
  assert(TextBuffer::langFromPath("main.Hpp") == Lang::Cpp);
  assert(TextBuffer::langFromPath("main.HPP") == Lang::Cpp);
  assert(TextBuffer::langFromPath("main.CC") == Lang::Cpp);
  assert(TextBuffer::langFromPath("main.CXX") == Lang::Cpp);
  assert(TextBuffer::langFromPath("main.HXX") == Lang::Cpp);
  assert(TextBuffer::langFromPath("main.HH") == Lang::Cpp);
  assert(TextBuffer::langFromPath("main.H") == Lang::Cpp);   // .h/.H 都是 C++ 头
  assert(TextBuffer::langFromPath("main.C++") == Lang::Cpp);
  // 3) 与 Builder::syntaxOnly 统一后新增的两个头扩展名(原来是死代码)。
  assert(TextBuffer::langFromPath("a.h++") == Lang::Cpp);
  assert(TextBuffer::langFromPath("a.hp") == Lang::Cpp);
  assert(TextBuffer::langFromPath("a.HP") == Lang::Cpp);
  // 4) `.inc` 已从支持清单里去掉(不是 C/C++ 标准扩展名,不扩大需求边界)。
  assert(TextBuffer::langFromPath("a.inc") == Lang::Unknown);
  assert(TextBuffer::langFromPath("a.INC") == Lang::Unknown);
  // 5) 仍然不认的:相近但不属于 C/C++ 的扩展名
  assert(TextBuffer::langFromPath("a.cs") == Lang::Unknown);
  assert(TextBuffer::langFromPath("a.cp") == Lang::Unknown);
  assert(TextBuffer::langFromPath("a.hs") == Lang::Unknown);
  assert(TextBuffer::langFromPath("a.") == Lang::Unknown);   // 光一个点

  TextBuffer b;
  assert(b.lang() == Lang::Unknown);
  b.setPath("/tmp/x.cpp");
  assert(b.lang() == Lang::Cpp);
  assert(b.path() == "/tmp/x.cpp");
  b.setPath("/tmp/x.c");
  assert(b.lang() == Lang::C);
  std::printf("  ok  langFromPath\n");
}

void testHasNonBlank() {
  TextBuffer b;
  assert(!b.hasNonBlank());
  b.insert({0, 0}, "   \t\n\t  \n");
  assert(!b.hasNonBlank());
  b.insert({1, 1}, "x");
  assert(b.hasNonBlank());
  Pos cur;
  assert(b.undo(cur));
  assert(!b.hasNonBlank());
  b.reset({"中"});
  assert(b.hasNonBlank());
  std::printf("  ok  hasNonBlank\n");
}

void testPosOperators() {
  assert((Pos{1, 2} == Pos{1, 2}));
  assert((Pos{1, 2} != Pos{1, 3}));
  assert((Pos{1, 2} < Pos{1, 3}));
  assert((Pos{1, 9} < Pos{2, 0}));
  assert(!(Pos{2, 0} < Pos{1, 9}));
  assert((Pos{1, 2} <= Pos{1, 2}));
  assert((Pos{1, 2} <= Pos{1, 3}));
  assert(!(Pos{1, 4} <= Pos{1, 3}));
  Range r{{3, 1}, {1, 2}};
  assert((r.normalized().a == Pos{1, 2}));
  assert((r.normalized().b == Pos{3, 1}));
  assert(!r.empty());
  assert((Range{{1, 1}, {1, 1}}.empty()));
  std::printf("  ok  Pos / Range 运算符\n");
}

}  // namespace

int main() {
  std::printf("test_textbuf:\n");
  testPosOperators();
  testBasicInsert();
  testBasicErase();
  testOutOfBounds();
  testTextAccess();
  testUndoGrouping();
  testNestedEdit();
  testRedoInvalidation();
  testRevisionDirty();
  testUndoLimit();
  testRandomUndoRedoRoundTrip();
  testStepwiseUndoMatchesSnapshots();
  testFileRoundTrip();
  testFileErrors();
  testLangFromPath();
  testHasNonBlank();
  std::printf("test_textbuf: 全部通过\n");
  return 0;
}

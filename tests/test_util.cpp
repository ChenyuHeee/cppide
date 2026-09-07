// test_util.cpp —— util.{h,cpp} 的单元测试。纯 assert,main 返回 0 即通过。
//
// 覆盖重点(Wave 1 报告点名的高风险区):
//   * 显示列 <-> 字节偏移的双向换算(ASCII / 中文 / 混排 / Tab / Tab+中文 / emoji)
//   * 非法 UTF-8(孤立续字节、截断序列、0xFF、超长编码、代理区)不死循环不越界
//   * wrapDisplay 的分段不变式(不越界、不切字符、不超宽、必然前进)
//
// 注意:wcwidth 需要 UTF-8 locale,main 一开始就 setlocale。
#undef NDEBUG
#include <cassert>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "../src/util.h"

using namespace util;

static int g_checks = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                            \
    assert(cond);                                                                \
  } while (0)

#define CHECK_EQ_I(a, b)                                                         \
  do {                                                                           \
    ++g_checks;                                                                  \
    long long va_ = (long long)(a), vb_ = (long long)(b);                        \
    if (va_ != vb_) {                                                            \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,    \
                   __LINE__, #a, #b, va_, vb_);                                  \
    }                                                                            \
    assert(va_ == vb_);                                                          \
  } while (0)

#define CHECK_EQ_S(a, b)                                                         \
  do {                                                                           \
    ++g_checks;                                                                  \
    std::string va_ = (a), vb_ = (b);                                            \
    if (va_ != vb_) {                                                            \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__,\
                   __LINE__, #a, #b, va_.c_str(), vb_.c_str());                  \
    }                                                                            \
    assert(va_ == vb_);                                                          \
  } while (0)

// ------------------------------------------------------------------ 辅助

// s 的全部合法字符边界(非法字节各自成一个“字符”)。
static std::set<int> boundaries(const std::string& s) {
  std::set<int> b;
  size_t i = 0;
  b.insert(0);
  while (i < s.size()) {
    size_t nxt = utf8Next(s, i);
    CHECK(nxt > i);                       // 绝不原地踏步 —— 死循环的护栏
    CHECK(nxt <= s.size());
    i = nxt;
    b.insert((int)i);
  }
  return b;
}

// 显示列 <-> 字节列 的双向不变式。要求 s 里没有零宽字符(否则列不唯一)。
static void checkColBijection(const std::string& s, int tw) {
  const std::set<int> bs = boundaries(s);
  const int total = displayWidth(s, tw, 0);
  CHECK(total >= 0);

  // 方向一:每个字符边界 -> 显示列 -> 回到同一个字节边界。
  for (int b : bs) {
    int c = byteToDisplayCol(s, b, tw);
    CHECK(c >= 0 && c <= total);
    CHECK_EQ_I(displayColToByte(s, c, tw), b);
  }
  // 方向二:每个显示列 -> 字节边界,且该边界的列 <= 该显示列 < 下一个边界的列。
  for (int c = 0; c <= total + 3; ++c) {
    int b = displayColToByte(s, c, tw);
    CHECK(bs.count(b) == 1);              // 永远落在 UTF-8 字符边界上
    if (c >= total) {
      CHECK_EQ_I(b, (int)s.size());
    } else {
      int lo = byteToDisplayCol(s, b, tw);
      int hi = byteToDisplayCol(s, (int)utf8Next(s, (size_t)b), tw);
      CHECK(lo <= c && c < hi);           // 吸附到该字符的起始字节
    }
  }
  // 负列 / 超大列 的钳位。
  CHECK_EQ_I(displayColToByte(s, -1, tw), 0);
  CHECK_EQ_I(displayColToByte(s, -1000000, tw), 0);
  CHECK_EQ_I(displayColToByte(s, total + 1000, tw), (int)s.size());
  CHECK_EQ_I(byteToDisplayCol(s, -5, tw), 0);
  CHECK_EQ_I(byteToDisplayCol(s, (int)s.size() + 99, tw), total);
}

// wrapDisplay 的分段不变式。
static void checkWrapInvariants(const std::string& s, int width, int tw) {
  const std::set<int> bs = boundaries(s);
  std::vector<WrapSeg> segs = wrapDisplay(s, width, tw);
  CHECK(!segs.empty());                   // 空串也要有一段
  int prev_end = 0;
  for (size_t k = 0; k < segs.size(); ++k) {
    const WrapSeg& sg = segs[k];
    CHECK(sg.len >= 0);
    CHECK(sg.start >= prev_end);          // 单调、不重叠
    CHECK(sg.start + sg.len <= (int)s.size());
    CHECK(bs.count(sg.start) == 1);       // 不切断 UTF-8 字符
    CHECK(bs.count(sg.start + sg.len) == 1);
    std::string piece = s.substr((size_t)sg.start, (size_t)sg.len);
    int w = displayWidth(piece, tw, 0);
    if (w > width) {
      // 唯一允许超宽的情形:该段只装得下一个字符,而这个字符本身就比 width 宽。
      CHECK_EQ_I((int)utf8Next(piece, 0), sg.len);
    }
    // 段与段之间只允许丢掉断点上的空白 / 换行,绝不能吞掉正文字节。
    for (int b = prev_end; b < sg.start; ++b) {
      char c = s[(size_t)b];
      CHECK(c == ' ' || c == '\t' || c == '\n');
    }
    prev_end = sg.start + sg.len;
  }
  for (int b = prev_end; b < (int)s.size(); ++b) {
    char c = s[(size_t)b];
    CHECK(c == ' ' || c == '\t' || c == '\n');
  }
}

// 随机二进制串:编辑器会打开任意文件,这些函数必须永不崩、永不死循环。
// 只查“弱不变式”(随机串里可能出现零宽字符,列与字节不再一一对应)。
static void checkBinarySafe(const std::string& s, int tw) {
  const std::set<int> bs = boundaries(s);
  const int total = displayWidth(s, tw, 0);
  CHECK(total >= 0);
  CHECK(total <= (int)s.size() * (tw > 2 ? tw : 2));

  int prev_b = -1;
  for (int c = 0; c <= total + 2; ++c) {
    int b = displayColToByte(s, c, tw);
    CHECK(bs.count(b) == 1);              // 永远落在字符边界上
    CHECK(b >= prev_b);                   // 单调不回退
    CHECK(b <= (int)s.size());
    prev_b = b;
  }
  int prev_c = -1;
  for (int b : bs) {
    int c = byteToDisplayCol(s, b, tw);
    CHECK(c >= 0 && c <= total);
    CHECK(c >= prev_c);
    prev_c = c;
  }
  // 非边界字节也不能让它越界或返回负数
  for (int b = 0; b <= (int)s.size() + 2; ++b) {
    int c = byteToDisplayCol(s, b, tw);
    CHECK(c >= 0 && c <= total);
  }
  for (int w = 1; w <= 6; ++w) checkWrapInvariants(s, w, tw);
  CHECK(utf8Length(s) <= s.size());
  (void)utf8Valid(s);
  CHECK_EQ_S(fromWide(toWide(s)), fromWide(toWide(fromWide(toWide(s)))));   // 幂等
}

static void testBinaryFuzz() {
  uint64_t seed = 0x12345678ABCDEF01ULL;    // 固定种子:失败可复现
  auto rnd = [&seed]() {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return (uint32_t)(seed >> 32);
  };
  static const char kSpice[] = {'\t', '\n', ' ', 'a', '\x80', '\xFF', '\xC3',
                                '\xE4', '\xB8', '\xAD', '\xF0', '\x9F'};
  for (int iter = 0; iter < 600; ++iter) {
    size_t n = rnd() % 24;
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      uint32_t r = rnd();
      if (r % 3 == 0) s += kSpice[r / 3 % sizeof(kSpice)];
      else s += (char)(r % 256);
    }
    checkBinarySafe(s, 1 + (int)(rnd() % 8));
  }
}

// ------------------------------------------------------------------ 时间

static void testTime() {
  int64_t a = nowMs();
  for (int i = 0; i < 2000; ++i) {
    int64_t b = nowMs();
    CHECK(b >= a);                        // steady_clock 单调不回退
    a = b;
  }
  int64_t t0 = nowMs();
  usleep(20000);
  int64_t t1 = nowMs();
  CHECK(t1 - t0 >= 10);                   // 睡了 20ms,至少涨 10ms
  CHECK(t1 - t0 < 5000);

  int64_t e = nowEpochMs();
  CHECK(e > 1600000000000LL);             // 2020-09 之后
  CHECK(e < 4000000000000LL);

  std::string hms = formatTimeHMS(e);
  CHECK_EQ_I((int)hms.size(), 8);
  CHECK(hms[2] == ':' && hms[5] == ':');
  for (int i : {0, 1, 3, 4, 6, 7}) CHECK(hms[(size_t)i] >= '0' && hms[(size_t)i] <= '9');

  CHECK_EQ_S(formatDuration(0), "0ms");
  CHECK_EQ_S(formatDuration(12), "12ms");
  CHECK_EQ_S(formatDuration(999), "999ms");
  CHECK_EQ_S(formatDuration(1400), "1.4s");
  CHECK_EQ_S(formatDuration(1000), "1.0s");
  CHECK_EQ_S(formatDuration(-7), "0ms");
  CHECK_EQ_S(formatDuration(123000), "2m03s");
}

// ------------------------------------------------------------------ 字符串

static void testStrings() {
  CHECK_EQ_S(trim("  a b  "), "a b");
  CHECK_EQ_S(trim("\t\n x \r\n"), "x");
  CHECK_EQ_S(trim(""), "");
  CHECK_EQ_S(trim("   "), "");
  CHECK_EQ_S(trimLeft("  a  "), "a  ");
  CHECK_EQ_S(trimRight("  a  "), "  a");
  CHECK_EQ_S(trim("中文"), "中文");

  CHECK(startsWith("hello", "he"));
  CHECK(startsWith("hello", ""));
  CHECK(startsWith("hello", "hello"));
  CHECK(!startsWith("he", "hello"));
  CHECK(endsWith("hello", "lo"));
  CHECK(endsWith("hello", ""));
  CHECK(!endsWith("lo", "hello"));

  CHECK_EQ_S(toLower("AbC_09"), "abc_09");
  CHECK_EQ_S(toLower("中文ABC"), "中文abc");   // 多字节序列的字节不被破坏
  CHECK(utf8Valid(toLower("中文ABC")));

  CHECK(isBlank(""));
  CHECK(isBlank(" \t\r\n"));
  CHECK(!isBlank(" a "));

  // splitLines:CRLF、末尾换行、空串
  {
    std::vector<std::string> v = splitLines("a\nb\nc");
    CHECK_EQ_I(v.size(), 3);
    CHECK_EQ_S(v[2], "c");
    v = splitLines("a\r\nb\r\n");
    CHECK_EQ_I(v.size(), 2);
    CHECK_EQ_S(v[0], "a");
    CHECK_EQ_S(v[1], "b");
    v = splitLines("a\n\n");
    CHECK_EQ_I(v.size(), 2);
    CHECK_EQ_S(v[1], "");
    v = splitLines("");
    CHECK_EQ_I(v.size(), 1);              // 空文本 = 1 个空行
    CHECK_EQ_S(v[0], "");
    v = splitLines("\n");
    CHECK_EQ_I(v.size(), 1);
    CHECK_EQ_S(v[0], "");
    v = splitLines("\r\n\r\n");
    CHECK_EQ_I(v.size(), 2);
    CHECK_EQ_S(v[0], "");
  }

  {
    std::vector<std::string> v = split("a,b,,c", ',');
    CHECK_EQ_I(v.size(), 4);
    CHECK_EQ_S(v[2], "");
    v = split("", ',');
    CHECK_EQ_I(v.size(), 1);
    CHECK_EQ_S(v[0], "");
    v = split("a,", ',');
    CHECK_EQ_I(v.size(), 2);
    CHECK_EQ_S(v[1], "");
    CHECK_EQ_S(join(split("a,b,c", ','), ","), "a,b,c");
  }
  CHECK_EQ_S(join({}, ","), "");
  CHECK_EQ_S(join({"x"}, ", "), "x");
  CHECK_EQ_S(join({"x", "y"}, ", "), "x, y");

  CHECK_EQ_S(replaceAll("aaa", "a", "b"), "bbb");
  CHECK_EQ_S(replaceAll("aaa", "aa", "a"), "aa");
  CHECK_EQ_S(replaceAll("abc", "", "X"), "abc");    // 空 from 不死循环
  CHECK_EQ_S(replaceAll("a.b", ".", "\\."), "a\\.b");
  CHECK_EQ_S(replaceAll("", "a", "b"), "");

  // clipBytes:不切断 UTF-8,总长不超 max_bytes
  CHECK_EQ_S(clipBytes("hello", 10), "hello");
  CHECK_EQ_S(clipBytes("hello", 5), "hello");
  {
    std::string c = clipBytes("hello world", 8);
    CHECK(c.size() <= 8);
    CHECK(endsWith(c, "\xE2\x80\xA6"));
    CHECK_EQ_S(c, "hello\xE2\x80\xA6");
    c = clipBytes("中文测试", 8);                    // 12 字节 -> 预算 5 -> 只放 1 个汉字
    CHECK(c.size() <= 8);
    CHECK(utf8Valid(c));
    CHECK_EQ_S(c, "中\xE2\x80\xA6");
    c = clipBytes("中文", 2);                        // 预算不足以放省略号
    CHECK(c.size() <= 2);
    CHECK_EQ_S(c, "");
    c = clipBytes("abc", 0);
    CHECK_EQ_S(c, "");
    c = clipBytes(std::string("\xFF\xFF\xFF\xFF\xFF", 5), 4);   // 二进制垃圾也不崩
    CHECK(c.size() <= 4);
  }

  CHECK_EQ_I(findNoCase("Hello World", "world"), 6);
  CHECK_EQ_I(findNoCase("Hello", "HELLO"), 0);
  CHECK(findNoCase("Hello", "xyz") == std::string::npos);
  CHECK_EQ_I(findNoCase("aAaA", "aa", 1), 1);
  CHECK_EQ_I(findNoCase("abc", ""), 0);
  CHECK(findNoCase("abc", "c", 5) == std::string::npos);
  CHECK_EQ_I(findNoCase("中文abc", "ABC"), 6);

  CHECK(hash64("a") != hash64("b"));
  CHECK_EQ_I(hash64("abc"), hash64("abc"));
  CHECK_EQ_I(hash64(""), 1469598103934665603ULL);   // FNV-1a offset basis
  CHECK_EQ_S(toHex(0), "0000000000000000");
  CHECK_EQ_S(toHex(0xdeadbeefULL), "00000000deadbeef");
  CHECK_EQ_S(toHex(0xFFFFFFFFFFFFFFFFULL), "ffffffffffffffff");
  CHECK_EQ_I(toHex(hash64("/tmp/x.cpp")).size(), 16);
}

// ------------------------------------------------------------------ 路径

static void testPaths() {
  CHECK_EQ_S(util::basename("/a/b/c.cpp"), "c.cpp");
  CHECK_EQ_S(util::basename("c.cpp"), "c.cpp");
  CHECK_EQ_S(util::basename("/a/b/"), "b");
  CHECK_EQ_S(util::basename("/"), "/");
  CHECK_EQ_S(util::basename(""), "");

  CHECK_EQ_S(util::dirname("/a/b/c.cpp"), "/a/b");
  CHECK_EQ_S(util::dirname("c.cpp"), ".");
  CHECK_EQ_S(util::dirname("/c.cpp"), "/");
  CHECK_EQ_S(util::dirname("/"), "/");
  CHECK_EQ_S(util::dirname(""), ".");
  CHECK_EQ_S(util::dirname("a/b/"), "a");

  CHECK_EQ_S(util::extension("a.cpp"), ".cpp");
  CHECK_EQ_S(util::extension("a.tar.gz"), ".gz");
  CHECK_EQ_S(util::extension("/x/.bashrc"), "");        // 隐藏文件不算扩展名
  CHECK_EQ_S(util::extension("Makefile"), "");
  CHECK_EQ_S(util::extension("/a.d/b"), "");            // 点在目录名里不算
  CHECK_EQ_S(util::extension("a."), ".");

  CHECK_EQ_S(util::stem("/a/b/c.cpp"), "c");
  CHECK_EQ_S(util::stem("a.tar.gz"), "a.tar");
  CHECK_EQ_S(util::stem("Makefile"), "Makefile");
  CHECK_EQ_S(util::stem("/x/.bashrc"), ".bashrc");

  CHECK_EQ_S(util::joinPath("/a", "b"), "/a/b");
  CHECK_EQ_S(util::joinPath("/a/", "b"), "/a/b");
  CHECK_EQ_S(util::joinPath("/a//", "b"), "/a/b");
  CHECK_EQ_S(util::joinPath("", "b"), "b");
  CHECK_EQ_S(util::joinPath("/a", ""), "/a");
  CHECK_EQ_S(util::joinPath("/a", "/abs"), "/abs");
  CHECK_EQ_S(util::joinPath("/", "b"), "/b");

  // ~ 展开
  const char* old_home = std::getenv("HOME");
  std::string saved_home = old_home ? old_home : "";
  setenv("HOME", "/tmp/fakehome", 1);
  CHECK_EQ_S(util::homeDir(), "/tmp/fakehome");
  CHECK_EQ_S(util::expandUser("~/x/y"), "/tmp/fakehome/x/y");
  CHECK_EQ_S(util::expandUser("~"), "/tmp/fakehome");
  CHECK_EQ_S(util::expandUser("~root/x"), "~root/x");   // ~user 不支持,原样
  CHECK_EQ_S(util::expandUser("/abs/x"), "/abs/x");
  CHECK_EQ_S(util::expandUser(""), "");
  CHECK_EQ_S(util::expandUser("a~b"), "a~b");
  setenv("HOME", "/tmp/fakehome/", 1);
  CHECK_EQ_S(util::expandUser("~/x"), "/tmp/fakehome/x");   // 结尾斜杠不重复
  if (old_home) setenv("HOME", saved_home.c_str(), 1);
  else unsetenv("HOME");

  const char* old_tmp = std::getenv("TMPDIR");
  std::string saved_tmp = old_tmp ? old_tmp : "";
  setenv("TMPDIR", "/tmp/zz/", 1);
  CHECK_EQ_S(util::tempDir(), "/tmp/zz");
  unsetenv("TMPDIR");
  CHECK_EQ_S(util::tempDir(), "/tmp");
  if (old_tmp) setenv("TMPDIR", saved_tmp.c_str(), 1);

  CHECK_EQ_S(util::envOr("CPPIDE_NO_SUCH_VAR_XYZ", "def"), "def");
  setenv("CPPIDE_TEST_VAR", "v1", 1);
  CHECK_EQ_S(util::envOr("CPPIDE_TEST_VAR", "def"), "v1");
  setenv("CPPIDE_TEST_VAR", "", 1);
  CHECK_EQ_S(util::envOr("CPPIDE_TEST_VAR", "def"), "def");   // 空值当没设
  unsetenv("CPPIDE_TEST_VAR");
  CHECK_EQ_S(util::envOr(nullptr, "def"), "def");
}

// ------------------------------------------------------------------ 文件

static void testFiles() {
  char base[256];
  std::snprintf(base, sizeof(base), "/tmp/cppide_util_test_%d", (int)getpid());
  std::string dir = base;
  std::string err;

  CHECK(makeDirs(dir + "/a/b/c", err));
  CHECK(err.empty());
  CHECK(isDirectory(dir + "/a/b/c"));
  CHECK(makeDirs(dir + "/a/b/c", err));           // 幂等
  CHECK(fileExists(dir));
  CHECK(!fileExists(dir + "/nope"));
  CHECK(!isDirectory(dir + "/nope"));
  CHECK_EQ_I(fileSize(dir + "/nope"), -1);
  CHECK_EQ_I(fileMtimeMs(dir + "/nope"), -1);

  // 原子写 + 读回
  std::string p = dir + "/f.txt";
  std::string data = "第一行\nsecond\n\t制表\n";
  CHECK(writeFileAtomic(p, data, err));
  CHECK(err.empty());
  std::string got;
  CHECK(readFile(p, got, err));
  CHECK_EQ_S(got, data);
  CHECK_EQ_I(fileSize(p), (int64_t)data.size());
  CHECK(fileMtimeMs(p) > 0);
  CHECK(!fileExists(p + ".cppide.tmp"));          // 临时文件不留痕

  // 覆盖写(含 NUL 的二进制)
  std::string bin("a\0b\xFF\xFE\x80", 6);
  CHECK(writeFileAtomic(p, bin, err));
  CHECK(readFile(p, got, err));
  CHECK_EQ_I(got.size(), 6);
  CHECK(got == bin);

  // 空内容
  CHECK(writeFileAtomic(p, "", err));
  CHECK(readFile(p, got, err));
  CHECK_EQ_S(got, "");
  CHECK_EQ_I(fileSize(p), 0);

  // 大内容(跨多次 write/read)
  std::string big(300000, 'x');
  big += "中文尾巴";
  CHECK(writeFileAtomic(p, big, err));
  CHECK(readFile(p, got, err));
  CHECK(got == big);

  // 失败路径:err 必须是非空说明,out 必须清空
  got = "dirty";
  CHECK(!readFile(dir + "/no/such/file", got, err));
  CHECK(!err.empty());
  CHECK_EQ_S(got, "");
  CHECK(!readFile(dir, got, err));                // 目录不是文件
  CHECK(!err.empty());
  CHECK(!readFile("", got, err));
  CHECK(!err.empty());
  CHECK(!writeFileAtomic(dir + "/no/such/dir/f", "x", err));
  CHECK(!err.empty());
  CHECK(!writeFileAtomic("", "x", err));
  CHECK(!makeDirs("", err));
  CHECK(!makeDirs(p + "/sub", err));              // p 是文件,不能当目录用
  CHECK(!err.empty());

  char cmd[512];
  std::snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir.c_str());
  int rc = std::system(cmd);
  (void)rc;
}

// ------------------------------------------------------------------ UTF-8

static void testUtf8() {
  CHECK(utf8IsContinuation(0x80));
  CHECK(utf8IsContinuation(0xBF));
  CHECK(!utf8IsContinuation(0x41));
  CHECK(!utf8IsContinuation(0xC3));
  CHECK(!utf8IsContinuation(0xF0));

  // 合法:1/2/3/4 字节
  struct { const char* s; uint32_t cp; size_t len; } ok[] = {
      {"A", 0x41, 1},
      {"\xC3\xA9", 0xE9, 2},               // é
      {"\xE4\xB8\xAD", 0x4E2D, 3},         // 中
      {"\xF0\x9F\x98\x80", 0x1F600, 4},    // 😀
      {"\xF4\x8F\xBF\xBF", 0x10FFFF, 4},   // 最大合法码点
      {"\xE0\xA0\x80", 0x800, 3},          // 3 字节最小值
      {"\xC2\x80", 0x80, 2},               // 2 字节最小值
  };
  for (auto& t : ok) {
    std::string s = t.s;
    uint32_t cp = 0;
    CHECK_EQ_I(utf8Decode(s, 0, cp), t.len);
    CHECK_EQ_I(cp, t.cp);
    CHECK(utf8Valid(s));
    CHECK_EQ_I(utf8Length(s), 1);
    CHECK_EQ_S(codepointToUtf8(t.cp), s);
    CHECK_EQ_I(utf8Next(s, 0), t.len);
    CHECK_EQ_I(utf8Prev(s, t.len), 0);
  }

  // 非法序列:每个坏字节吃 1 字节 + U+FFFD,长度 = 字节数
  struct { std::string s; size_t chars; const char* why; } bad[] = {
      {std::string("\x80", 1), 1, "孤立续字节"},
      {std::string("\xBF", 1), 1, "孤立续字节"},
      {std::string("\xFF", 1), 1, "0xFF 永不合法"},
      {std::string("\xFE", 1), 1, "0xFE 永不合法"},
      {std::string("\xF5\x80\x80\x80", 4), 4, "> U+10FFFF"},
      {std::string("\xE4\xB8", 2), 2, "截断的 3 字节"},
      {std::string("\xF0\x9F\x98", 3), 3, "截断的 4 字节"},
      {std::string("\xC3", 1), 1, "截断的 2 字节"},
      {std::string("\xC0\xAF", 2), 2, "超长编码的 '/'"},
      {std::string("\xC1\xBF", 2), 2, "超长编码"},
      {std::string("\xE0\x80\xAF", 3), 3, "3 字节超长"},
      {std::string("\xF0\x82\x82\xAC", 4), 4, "4 字节超长的 €"},
      {std::string("\xED\xA0\x80", 3), 3, "代理区 U+D800"},
      {std::string("\xED\xBF\xBF", 3), 3, "代理区 U+DFFF"},
      {std::string("\xED\xA0\x80\xED\xB0\x80", 6), 6, "CESU-8 代理对"},
      {std::string("\x00\x41", 2), 2, "内嵌 NUL 是合法的 U+0000"},
  };
  for (auto& t : bad) {
    const bool nul_case = (t.why[0] == 'N' || std::strcmp(t.why, "内嵌 NUL 是合法的 U+0000") == 0);
    CHECK_EQ_I(utf8Length(t.s), t.chars);
    if (!nul_case) {
      CHECK(!utf8Valid(t.s));
      uint32_t cp = 0;
      CHECK_EQ_I(utf8Decode(t.s, 0, cp), 1);       // 永远前进,永远只吃 1 字节
      CHECK_EQ_I(cp, 0xFFFD);
    }
    // 逐字节遍历不越界、不死循环
    size_t i = 0, guard = 0;
    while (i < t.s.size()) {
      size_t nxt = utf8Next(t.s, i);
      CHECK(nxt > i);
      i = nxt;
      ++guard;
      CHECK(guard <= t.s.size() + 1);
    }
    // 反向遍历同样必然前进
    i = t.s.size();
    guard = 0;
    while (i > 0) {
      size_t prv = utf8Prev(t.s, i);
      CHECK(prv < i);
      i = prv;
      ++guard;
      CHECK(guard <= t.s.size() + 1);
    }
    // 宽度计算不崩,且非负
    CHECK(displayWidth(t.s, 4, 0) >= 0);
    checkWrapInvariants(t.s, 3, 4);
  }

  // 混排 + 非法字节夹在中间
  {
    std::string s = std::string("a\xE4\xB8\xAD\xFF" "b\xF0\x9F\x98\x80", 10);
    CHECK_EQ_I(utf8Length(s), 5);          // a 中 <FF> b 😀
    CHECK(!utf8Valid(s));
    std::vector<uint32_t> cps = utf8ToCodepoints(s);
    CHECK_EQ_I(cps.size(), 5);
    CHECK_EQ_I(cps[0], 'a');
    CHECK_EQ_I(cps[1], 0x4E2D);
    CHECK_EQ_I(cps[2], 0xFFFD);
    CHECK_EQ_I(cps[3], 'b');
    CHECK_EQ_I(cps[4], 0x1F600);
    CHECK_EQ_I(utf8Prev(s, s.size()), 6);  // 😀 的起点
    CHECK_EQ_I(utf8Prev(s, 5), 4);         // 坏字节自成一个字符
    CHECK_EQ_I(utf8Prev(s, 4), 1);         // 中 的起点
  }

  // 越界 / 边界参数
  {
    std::string s = "abc";
    CHECK_EQ_I(utf8Next(s, 3), 3);
    CHECK_EQ_I(utf8Next(s, 99), 3);
    CHECK_EQ_I(utf8Prev(s, 0), 0);
    CHECK_EQ_I(utf8Prev(s, 99), 2);
    uint32_t cp = 7;
    CHECK_EQ_I(utf8Decode(s, 99, cp), 1);  // 越界也返回 >=1,不越界读
    CHECK_EQ_I(cp, 0);
    CHECK_EQ_I(utf8Length(""), 0);
    CHECK(utf8Valid(""));
    CHECK_EQ_I(displayWidth("", 4, 0), 0);
  }

  // utf8Prev 落在多字节字符中间时退到上一字节(不越界)
  {
    std::string s = "中";                  // 3 字节
    CHECK_EQ_I(utf8Prev(s, 3), 0);
    CHECK_EQ_I(utf8Prev(s, 2), 1);         // 2 不是字符边界 -> 退 1 字节
    CHECK_EQ_I(utf8Prev(s, 1), 0);
  }

  // codepointToUtf8 对非法码点给 U+FFFD
  CHECK_EQ_S(codepointToUtf8(0xD800), "\xEF\xBF\xBD");
  CHECK_EQ_S(codepointToUtf8(0x110000), "\xEF\xBF\xBD");
  CHECK_EQ_S(codepointToUtf8(0xFFFFFFFF), "\xEF\xBF\xBD");
  CHECK_EQ_S(codepointToUtf8(0), std::string("\0", 1));

  // 宽字符互转
  {
    std::string s = "a中b😀é";
    std::wstring w = toWide(s);
    CHECK_EQ_I(w.size(), 5u * (sizeof(wchar_t) >= 4 ? 1 : 1) + (sizeof(wchar_t) >= 4 ? 0 : 1));
    CHECK_EQ_S(fromWide(w), s);
    CHECK_EQ_S(fromWide(toWide("")), "");
    // 非法字节转 wide 再转回来 = U+FFFD 替换
    std::string badly = std::string("a\xFF" "b", 3);
    CHECK_EQ_S(fromWide(toWide(badly)), "a\xEF\xBF\xBD" "b");
  }
}

// ------------------------------------------------------------------ 显示宽度

static void testWidth() {
  // 单字符宽度
  CHECK_EQ_I(charDisplayWidth('a'), 1);
  CHECK_EQ_I(charDisplayWidth(' '), 1);
  CHECK_EQ_I(charDisplayWidth(0x4E2D), 2);        // 中
  CHECK_EQ_I(charDisplayWidth(0xFF21), 2);        // 全角 A
  CHECK_EQ_I(charDisplayWidth(0x1F600), 2);       // 😀
  CHECK_EQ_I(charDisplayWidth(0xE9), 1);          // é
  CHECK_EQ_I(charDisplayWidth(0x0301), 0);        // 组合音标 = 0 列
  CHECK_EQ_I(charDisplayWidth(0x200B), 0);        // 零宽空格
  CHECK_EQ_I(charDisplayWidth('\t'), 1);          // 控制字符本身算 1(制表位是串级语义)
  CHECK_EQ_I(charDisplayWidth('\n'), 1);
  CHECK_EQ_I(charDisplayWidth(0x01), 1);
  CHECK_EQ_I(charDisplayWidth(0x7F), 1);          // DEL
  CHECK_EQ_I(charDisplayWidth(0xFFFD), 1);        // 替换符 = 非法字节的占位
  CHECK_EQ_I(charDisplayWidth(0xD800), 1);        // 孤立代理:占位 1
  CHECK_EQ_I(charDisplayWidth(0x110000), 1);      // 超出 Unicode:占位 1
  CHECK_EQ_I(charDisplayWidth(0xFFFFFFFF), 1);
  for (uint32_t cp = 0; cp < 0x2000; ++cp) CHECK(charDisplayWidth(cp) >= 0);   // 永不为负

  // 纯 ASCII
  CHECK_EQ_I(displayWidth("hello", 4, 0), 5);
  // 纯中文
  CHECK_EQ_I(displayWidth("中文测试", 4, 0), 8);
  // 中英混排
  CHECK_EQ_I(displayWidth("a中b文c", 4, 0), 1 + 2 + 1 + 2 + 1);
  // emoji
  CHECK_EQ_I(displayWidth("😀😀", 4, 0), 4);

  // Tab 展开到制表位(不是固定宽度)
  CHECK_EQ_I(displayWidth("\t", 4, 0), 4);
  CHECK_EQ_I(displayWidth("a\t", 4, 0), 4);       // 1 -> 4
  CHECK_EQ_I(displayWidth("abc\t", 4, 0), 4);     // 3 -> 4
  CHECK_EQ_I(displayWidth("abcd\t", 4, 0), 8);    // 4 -> 8(整格上也要跳整格)
  CHECK_EQ_I(displayWidth("\t\t", 4, 0), 8);
  CHECK_EQ_I(displayWidth("a\tb", 4, 0), 5);
  CHECK_EQ_I(displayWidth("\t", 8, 0), 8);
  CHECK_EQ_I(displayWidth("ab\tc", 3, 0), 4);     // tw=3: 2 -> 3,再 +1
  CHECK_EQ_I(displayWidth("a\t", 1, 0), 2);       // tw=1: 每个 Tab 恰好 1 列
  CHECK_EQ_I(displayWidth("a\t", 0, 0), 2);       // tw<1 按 1 处理
  // Tab 与中文混合
  CHECK_EQ_I(displayWidth("中\t", 4, 0), 4);      // 2 -> 4
  CHECK_EQ_I(displayWidth("中\t中", 4, 0), 6);
  CHECK_EQ_I(displayWidth("中中\t", 4, 0), 8);    // 4 -> 8
  CHECK_EQ_I(displayWidth("中\t", 8, 0), 8);
  // start_col 影响制表位对齐,返回的是“本串占的宽度”
  CHECK_EQ_I(displayWidth("\t", 4, 1), 3);
  CHECK_EQ_I(displayWidth("\t", 4, 3), 1);
  CHECK_EQ_I(displayWidth("\t", 4, 4), 4);
  CHECK_EQ_I(displayWidth("abc", 4, 7), 3);
  CHECK_EQ_I(displayWidth("", 4, 5), 0);

  // ---- 双向换算:每个字符边界与每个显示列都要对得上
  const int tws[] = {1, 2, 3, 4, 8};
  const char* cases[] = {
      "",
      "hello world",
      "中文测试",
      "a中b文c",
      "\t",
      "\t\t\t",
      "a\tb\tc",
      "中\t中\t",
      "\t中\ta",
      "abcd\t\t中",
      "😀a😀",
      "\xF0\x9F\x98\x80\t\xE4\xB8\xAD",
      "   leading and trailing   ",
  };
  for (int tw : tws)
    for (const char* c : cases) checkColBijection(c, tw);

  // 显式检查“落在宽字符/Tab 中间”的吸附规则
  {
    std::string s = "中文";                       // 列 0..1 = 中,2..3 = 文
    CHECK_EQ_I(displayColToByte(s, 0, 4), 0);
    CHECK_EQ_I(displayColToByte(s, 1, 4), 0);     // 宽字符中间 -> 吸附到起始字节
    CHECK_EQ_I(displayColToByte(s, 2, 4), 3);
    CHECK_EQ_I(displayColToByte(s, 3, 4), 3);
    CHECK_EQ_I(displayColToByte(s, 4, 4), 6);     // 行尾
    CHECK_EQ_I(displayColToByte(s, 9, 4), 6);
    CHECK_EQ_I(byteToDisplayCol(s, 0, 4), 0);
    CHECK_EQ_I(byteToDisplayCol(s, 3, 4), 2);
    CHECK_EQ_I(byteToDisplayCol(s, 6, 4), 4);
    CHECK_EQ_I(byteToDisplayCol(s, 1, 4), 0);     // 字节落在字符中间 -> 向左吸附
    CHECK_EQ_I(byteToDisplayCol(s, 2, 4), 0);
    CHECK_EQ_I(byteToDisplayCol(s, 4, 4), 2);
  }
  {
    std::string s = "\tx";                        // 列 0..3 = Tab,4 = x
    for (int c = 0; c <= 3; ++c) CHECK_EQ_I(displayColToByte(s, c, 4), 0);
    CHECK_EQ_I(displayColToByte(s, 4, 4), 1);
    CHECK_EQ_I(displayColToByte(s, 5, 4), 2);
    CHECK_EQ_I(byteToDisplayCol(s, 1, 4), 4);
  }
  {
    std::string s = "a\t中\tz";
    // a=0, Tab: 1->4, 中: 4..5, Tab: 6->8, z: 8
    CHECK_EQ_I(byteToDisplayCol(s, 0, 4), 0);
    CHECK_EQ_I(byteToDisplayCol(s, 1, 4), 1);
    CHECK_EQ_I(byteToDisplayCol(s, 2, 4), 4);
    CHECK_EQ_I(byteToDisplayCol(s, 5, 4), 6);
    CHECK_EQ_I(byteToDisplayCol(s, 6, 4), 8);
    CHECK_EQ_I(byteToDisplayCol(s, 7, 4), 9);
    CHECK_EQ_I(displayColToByte(s, 7, 4), 5);     // Tab 中间 -> Tab 起点
    CHECK_EQ_I(displayColToByte(s, 5, 4), 2);     // 中 的第二列 -> 中 的起点
  }

  // 零宽字符:列不再一一对应,但不能崩、也不能越界
  {
    std::string s = "e\xCC\x81zh";                // e + U+0301 + zh
    CHECK_EQ_I(displayWidth(s, 4, 0), 3);
    CHECK_EQ_I(displayColToByte(s, 0, 4), 0);
    CHECK_EQ_I(displayColToByte(s, 1, 4), 3);     // 组合符归属前一个字符
    CHECK_EQ_I(byteToDisplayCol(s, 3, 4), 1);
    CHECK_EQ_I(byteToDisplayCol(s, (int)s.size(), 4), 3);
  }

  // 空行 / 只有 Tab 的行
  CHECK_EQ_I(displayColToByte("", 0, 4), 0);
  CHECK_EQ_I(displayColToByte("", 7, 4), 0);
  CHECK_EQ_I(byteToDisplayCol("", 0, 4), 0);
  {
    std::string s = "\t\t\t";
    CHECK_EQ_I(displayWidth(s, 4, 0), 12);
    CHECK_EQ_I(displayColToByte(s, 0, 4), 0);
    CHECK_EQ_I(displayColToByte(s, 3, 4), 0);
    CHECK_EQ_I(displayColToByte(s, 4, 4), 1);
    CHECK_EQ_I(displayColToByte(s, 11, 4), 2);
    CHECK_EQ_I(displayColToByte(s, 12, 4), 3);
    CHECK_EQ_I(byteToDisplayCol(s, 3, 4), 12);
  }

  // 极长行(不做 O(n^2) 全遍历,抽样)
  {
    std::string s;
    s.reserve(400000);
    for (int i = 0; i < 100000; ++i) s += "a中\t";   // 每 3 字符一组
    int total = displayWidth(s, 4, 0);
    CHECK(total > 0);
    CHECK_EQ_I(total, 100000 * 4);                   // a(1) 中(2->3) Tab(->4)
    CHECK_EQ_I(byteToDisplayCol(s, (int)s.size(), 4), total);
    CHECK_EQ_I(displayColToByte(s, total, 4), (int)s.size());
    CHECK_EQ_I(displayColToByte(s, 0, 4), 0);
    CHECK_EQ_I(displayColToByte(s, 4, 4), 5);        // 第二组的起点
    CHECK_EQ_I(byteToDisplayCol(s, 5, 4), 4);
    std::vector<WrapSeg> segs = wrapDisplay(s, 80, 4);
    CHECK(segs.size() > 1000);
    CHECK_EQ_I(segs.front().start, 0);
  }
}

// ------------------------------------------------------------------ 折行

static void testWrap() {
  // 空串 -> 一段空段
  {
    std::vector<WrapSeg> v = wrapDisplay("", 10, 4);
    CHECK_EQ_I(v.size(), 1);
    CHECK_EQ_I(v[0].start, 0);
    CHECK_EQ_I(v[0].len, 0);
  }
  // 放得下 -> 一段
  {
    std::vector<WrapSeg> v = wrapDisplay("hello", 10, 4);
    CHECK_EQ_I(v.size(), 1);
    CHECK_EQ_I(v[0].len, 5);
  }
  // 在空白处断词,断点上的空白被吃掉
  {
    std::string s = "aaa bbb ccc";
    std::vector<WrapSeg> v = wrapDisplay(s, 7, 4);
    CHECK_EQ_I(v.size(), 2);
    CHECK_EQ_S(s.substr((size_t)v[0].start, (size_t)v[0].len), "aaa bbb");
    CHECK_EQ_S(s.substr((size_t)v[1].start, (size_t)v[1].len), "ccc");
    v = wrapDisplay(s, 3, 4);
    CHECK_EQ_I(v.size(), 3);
    CHECK_EQ_S(s.substr((size_t)v[1].start, (size_t)v[1].len), "bbb");
  }
  // 单词过长 -> 硬断
  {
    std::string s = "abcdefghij";
    std::vector<WrapSeg> v = wrapDisplay(s, 4, 4);
    CHECK_EQ_I(v.size(), 3);
    CHECK_EQ_S(s.substr((size_t)v[0].start, (size_t)v[0].len), "abcd");
    CHECK_EQ_S(s.substr((size_t)v[2].start, (size_t)v[2].len), "ij");
  }
  // 中文:没有空白,只能硬断,且绝不切断字符
  {
    std::string s = "中文测试折行";                 // 6 字 = 12 列
    std::vector<WrapSeg> v = wrapDisplay(s, 5, 4);   // 每段最多 2 个字(4 列)
    CHECK_EQ_I(v.size(), 3);
    for (auto& sg : v) CHECK_EQ_I(sg.len, 6);
    CHECK_EQ_S(s.substr((size_t)v[0].start, (size_t)v[0].len), "中文");
  }
  // 宽字符本身比 width 宽:允许超宽,但必须只含这一个字符,且必须前进
  {
    std::string s = "中中";
    std::vector<WrapSeg> v = wrapDisplay(s, 1, 4);
    CHECK_EQ_I(v.size(), 2);
    CHECK_EQ_I(v[0].len, 3);
    CHECK_EQ_I(v[1].start, 3);
    v = wrapDisplay(s, 0, 4);                        // width<1 按 1
    CHECK_EQ_I(v.size(), 2);
    v = wrapDisplay(s, -5, 4);
    CHECK_EQ_I(v.size(), 2);
  }
  // 换行符是硬断点,'\n' 本身不属于任何段
  {
    std::string s = "ab\ncd";
    std::vector<WrapSeg> v = wrapDisplay(s, 10, 4);
    CHECK_EQ_I(v.size(), 2);
    CHECK_EQ_S(s.substr((size_t)v[0].start, (size_t)v[0].len), "ab");
    CHECK_EQ_S(s.substr((size_t)v[1].start, (size_t)v[1].len), "cd");
    v = wrapDisplay("ab\n", 10, 4);                  // 末尾换行不产生空尾段
    CHECK_EQ_I(v.size(), 1);
    v = wrapDisplay("\n", 10, 4);
    CHECK_EQ_I(v.size(), 1);
    CHECK_EQ_I(v[0].len, 0);
    v = wrapDisplay("\n\nx", 10, 4);
    CHECK_EQ_I(v.size(), 3);
    CHECK_EQ_I(v[0].len, 0);
    CHECK_EQ_I(v[1].len, 0);
  }
  // Tab 参与折行:Tab 按制表位吃列
  {
    std::string s = "\tabcdefgh";
    std::vector<WrapSeg> v = wrapDisplay(s, 8, 4);   // Tab 吃 4 列,剩 4 列给 abcd
    CHECK_EQ_S(s.substr((size_t)v[0].start, (size_t)v[0].len), "\tabcd");
    CHECK_EQ_S(s.substr((size_t)v[1].start, (size_t)v[1].len), "efgh");
  }
  // 全是空白的串:不能死循环
  {
    std::vector<WrapSeg> v = wrapDisplay("        ", 3, 4);
    CHECK(!v.empty());
    v = wrapDisplay("\t\t\t\t", 3, 4);
    CHECK(!v.empty());
  }

  // 不变式:各种宽度 x 各种串
  const char* cases[] = {
      "",
      " ",
      "a",
      "hello world foo bar baz",
      "中文测试折行中文测试折行",
      "混排 mixed 中文 text 一二三 456",
      "\t\ta\tbb\tccc\t",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "line1\nline2\n\nline4",
      "😀😀😀 emoji 折行 😀",
      "   ",
      "a  b",
      "trailing   ",
      "\xFF\xFE 坏字节 \x80 也要能折",
      "e\xCC\x81 zero-width \xE2\x80\x8B end",
  };
  for (int w = 1; w <= 12; ++w)
    for (int tw = 1; tw <= 8; tw += 3)
      for (const char* c : cases) checkWrapInvariants(c, w, tw);
}

// wcwidth 不可用("C" locale)时的兜底表 —— 布局不能崩。
static void testWidthFallbackInCLocale() {
  const char* saved = setlocale(LC_CTYPE, nullptr);
  std::string keep = saved ? saved : "";
  setlocale(LC_CTYPE, "C");
  CHECK(wcwidth((wchar_t)0x4E2D) < 0);            // 前提:C locale 下确实不认识
  CHECK_EQ_I(charDisplayWidth(0x4E2D), 2);        // 兜底表仍然给 2
  CHECK_EQ_I(charDisplayWidth(0xFF21), 2);
  CHECK_EQ_I(charDisplayWidth(0x1F600), 2);
  CHECK_EQ_I(charDisplayWidth(0x0301), 0);
  CHECK_EQ_I(charDisplayWidth(0xE9), 1);
  CHECK_EQ_I(displayWidth("中文", 4, 0), 4);
  if (!keep.empty()) setlocale(LC_CTYPE, keep.c_str());
}

int main() {
  if (!setlocale(LC_ALL, "")) setlocale(LC_ALL, "C.UTF-8");
  if (wcwidth((wchar_t)0x4E2D) != 2) {
    // 环境没有可用的 UTF-8 locale:换一个再试,否则后面靠内置兜底表也能过。
    if (!setlocale(LC_ALL, "C.UTF-8")) setlocale(LC_ALL, "C.utf8");
  }

  testTime();
  testStrings();
  testPaths();
  testFiles();
  testUtf8();
  testWidth();
  testWrap();
  testBinaryFuzz();
  testWidthFallbackInCLocale();

  std::printf("test_util: OK (%d checks)\n", g_checks);
  return 0;
}

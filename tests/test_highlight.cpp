// test_highlight.cpp —— Highlighter / HighlightCache 单测(纯 assert,main 返回 0 = 通过)
//
// 编译运行(Wave 2 独立跑,不需要 textbuf.cpp):
//   g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined
//       -o /tmp/test_highlight tests/test_highlight.cpp src/highlight.cpp && /tmp/test_highlight
//   (也可直接跑 bash .flower/scripts/wave2-highlight-verify.sh)
//
// ★ 为什么这里能测 HighlightCache:
//   HighlightCache::entryState/spansFor 收 `const TextBuffer&`,无法用别的类型伪造。
//   本文件因此就地给出 TextBuffer 的 5 个成员的**最小实现**,并全部标记
//   __attribute__((weak))。Wave 2 里 textbuf.cpp 还不存在 → 用这份假实现;
//   Wave 3 之后 `make tests` 会把 textbuf.o 一起链进来 → 强符号覆盖弱符号,
//   既不会重复定义报错,也自动改用真实现(本测试只用 reset/lineCount/line,
//   都是 architecture.md 冻结的公开语义)。已实测两种链接方式都通过。

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/highlight.h"

// ===================== TextBuffer 最小弱实现(见文件头说明) =====================
__attribute__((weak)) TextBuffer::TextBuffer() { lines_.push_back(std::string()); }

__attribute__((weak)) void TextBuffer::reset(std::vector<std::string> lines) {
  if (lines.empty()) lines.push_back(std::string());
  lines_ = std::move(lines);
  ++revision_;
  dirty_ = false;
}

__attribute__((weak)) int TextBuffer::lineCount() const {
  return static_cast<int>(lines_.size());
}

__attribute__((weak)) const std::string& TextBuffer::line(int i) const {
  static const std::string kEmpty;
  if (i < 0 || i >= static_cast<int>(lines_.size())) return kEmpty;
  return lines_[static_cast<size_t>(i)];
}

__attribute__((weak)) int TextBuffer::lineLen(int i) const {
  return static_cast<int>(line(i).size());
}

// ================================ 测试脚手架 ================================

static long g_checks = 0;
static int g_cases = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                        \
    assert(cond);                                                            \
  } while (0)

static void caseBegin(const char* name) {
  ++g_cases;
  (void)name;
}

// spans 的核心不变式:升序、无缝、无重叠、完整覆盖 [0, n)、start/len 在界内。
static void checkCover(const std::string& line, const std::vector<Span>& sp) {
  const int n = static_cast<int>(line.size());
  int cur = 0;
  for (const Span& s : sp) {
    CHECK(s.len > 0);
    CHECK(s.start == cur);          // 紧接上一段:既无空隙也无重叠
    CHECK(s.start >= 0 && s.start <= n);
    CHECK(s.len <= n - s.start);    // 不越界
    CHECK(static_cast<unsigned>(s.tok) < static_cast<unsigned>(kTokCount));
    CHECK(std::strlen(tokName(s.tok)) > 0);
    cur = s.start + s.len;
  }
  CHECK(cur == n);                  // 拼起来等于原行
  if (n == 0) CHECK(sp.empty());
}

static bool isUtf8Cont(unsigned char c) { return (c & 0xC0u) == 0x80u; }

// 合法 UTF-8 行:任何分段点都不能落在多字节字符中间。
static bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t need = 0;
    if (c < 0x80u) need = 0;
    else if ((c & 0xE0u) == 0xC0u) need = 1;
    else if ((c & 0xF0u) == 0xE0u) need = 2;
    else if ((c & 0xF8u) == 0xF0u) need = 3;
    else return false;
    if (need > 0 && i + need >= s.size()) return false;   // 截断的多字节序列
    for (size_t k = 1; k <= need; ++k)
      if (!isUtf8Cont(static_cast<unsigned char>(s[i + k]))) return false;
    i += need + 1;
  }
  return true;
}

static void checkUtf8Boundaries(const std::string& line, const std::vector<Span>& sp) {
  if (!validUtf8(line)) return;  // 故意非法的输入不要求保持字符边界
  for (const Span& s : sp) {
    if (s.start > 0)
      CHECK(!isUtf8Cont(static_cast<unsigned char>(line[static_cast<size_t>(s.start)])));
  }
}

struct Scan {
  std::string line;
  std::vector<Span> sp;
  uint8_t out = ST_NONE;

  // 覆盖 [off, off+len) 的那一段的 tok(找不到则 Normal 并断言失败)
  Tok tokAt(int off) const {
    for (const Span& s : sp)
      if (off >= s.start && off < s.start + s.len) return s.tok;
    CHECK(false);
    return Tok::Normal;
  }
  // 精确存在一段其文本恰为 text 且 tok 为 t
  // 注意:相邻同 Tok 的段会被合并,所以 Tok::Normal 的词(和它两侧的空白同色)
  // 一般查不到“恰好等于该词”的段 —— 那种情况用 tokOfText。
  bool hasExact(const std::string& text, Tok t) const {
    for (const Span& s : sp)
      if (s.tok == t && s.len == static_cast<int>(text.size()) &&
          line.compare(static_cast<size_t>(s.start), text.size(), text) == 0)
        return true;
    return false;
  }
  // text 首次出现处的 tok
  Tok tokOfText(const std::string& text) const {
    const size_t p = line.find(text);
    CHECK(p != std::string::npos);
    return tokAt(static_cast<int>(p));
  }
  bool hasTok(Tok t) const {
    for (const Span& s : sp)
      if (s.tok == t) return true;
    return false;
  }
  int count(Tok t) const {
    int c = 0;
    for (const Span& s : sp)
      if (s.tok == t) ++c;
    return c;
  }
};

static Scan scan(const Highlighter& h, const std::string& line, uint8_t in = ST_NONE) {
  Scan r;
  r.line = line;
  r.out = h.scanLine(line, in, r.sp);
  checkCover(r.line, r.sp);
  checkUtf8Boundaries(r.line, r.sp);
  // 纯函数性:重扫一次必须逐字节相同
  std::vector<Span> again;
  const uint8_t out2 = h.scanLine(line, in, again);
  CHECK(out2 == r.out);
  CHECK(again.size() == r.sp.size());
  for (size_t k = 0; k < again.size() && k < r.sp.size(); ++k) {
    CHECK(again[k].start == r.sp[k].start);
    CHECK(again[k].len == r.sp[k].len);
    CHECK(again[k].tok == r.sp[k].tok);
  }
  return r;
}

// ================================ 各组测试 ================================

static void testTokNames() {
  caseBegin("tokName");
  CHECK(kTokCount == 11);
  CHECK(std::string(tokName(Tok::Normal)) == "Normal");
  CHECK(std::string(tokName(Tok::TodoInComment)) == "TodoInComment");
  CHECK(std::string(tokName(Tok::Func)) == "Func");
  for (int i = 0; i < kTokCount; ++i)
    CHECK(std::strlen(tokName(static_cast<Tok>(i))) > 0);
}

// 一批真实竞赛风格代码(含中文注释/中文字符串)—— 逐行断言覆盖不变式
static const char* kCorpus[] = {
    "#include <bits/stdc++.h>",
    "#include \"local.h\"",
    "#define REP(i, n) for (int i = 0; i < (n); ++i)",
    "using namespace std;",
    "typedef long long ll;",
    "using pii = pair<int, int>;",
    "const int MAXN = 2e5 + 7;   // 上限,注意开够",
    "ll a[MAXN], dp[MAXN][2];",
    "",
    "/* 多行注释开始:这里写题解思路",
    "   状态转移:dp[i][j] = max(dp[i-1][j], ...)   TODO 边界还没验",
    "   FIXME 卡常?XXX 先交一发 */",
    "struct Node {",
    "  int l, r;",
    "  bool operator<(const Node& o) const { return l < o.l; }",
    "};",
    "",
    "template <class T>",
    "inline T gcd2(T x, T y) { return y ? gcd2(y, x % y) : x; }",
    "",
    "int main() {",
    "  ios::sync_with_stdio(false);",
    "  cin.tie(nullptr);",
    "  int n = 0, m = 0;",
    "  if (!(cin >> n >> m)) return 0;",
    "  vector<vector<int>> g(n + 1);",
    "  unordered_map<string, int> id;",
    "  string s = \"你好,世界\";           // 中文字符串",
    "  printf(\"%lld\\n\", (ll)1e18 + 3);",
    "  double eps = 1e-9, pi = 3.1415926535;",
    "  ll big = 1'000'000'007LL, mask = 0xdeadBEEFull, bits = 0b1011'0010;",
    "  char c = 'x', nl = '\\n', q = '\\'', hex = '\\x41';",
    "  for (int i = 0; i < n; ++i) { g[i].push_back(i ^ 1); }",
    "  auto cmp = [&](const pii& a, const pii& b) { return a.second > b.second; };",
    "  sort(g.begin(), g.end(), cmp);   // 按第二关键字降序 —— 中文注释里也有 TODO",
    "  cout << (n & 1 ? \"odd\" : \"even\") << '\\n';",
    "  // 收尾:别忘了 \\t 转义与 /* 这种不算注释的东西",
    "  return 0;",
    "}",
    "static_assert(sizeof(ll) == 8, \"需要 64 位\");",
    "R\"(raw 单行 (嵌套) 也要能过)\";",
    "\tint\tx\t=\t1;",
};

static void testCorpusInvariant() {
  caseBegin("corpus/coverage-invariant");
  for (Lang lg : {Lang::Cpp, Lang::C, Lang::Unknown}) {
    Highlighter h(lg);
    uint8_t st = ST_NONE;
    for (const char* raw : kCorpus) {
      const std::string line(raw);
      std::vector<Span> sp;
      const uint8_t next = h.scanLine(line, st, sp);
      checkCover(line, sp);
      checkUtf8Boundaries(line, sp);
      CHECK((next & ~static_cast<uint8_t>(ST_BLOCKCOMMENT | ST_RAWSTRING | ST_CONTLINE)) == 0);
      st = next;
    }
  }
  // 语料里的关键着色抽查(C++)
  Highlighter h(Lang::Cpp);
  Scan s = scan(h, kCorpus[0]);
  CHECK(s.tokAt(0) == Tok::Preproc);
  CHECK(s.hasExact("<bits/stdc++.h>", Tok::String));
  s = scan(h, kCorpus[1]);
  CHECK(s.hasExact("\"local.h\"", Tok::String));
  s = scan(h, "  int n = 0, m = 0;");
  CHECK(s.hasExact("int", Tok::Type));
  CHECK(s.tokOfText("n =") == Tok::Normal);
  CHECK(s.hasExact("0", Tok::Number));
  s = scan(h, "  cin.tie(nullptr);");
  CHECK(s.hasExact("tie", Tok::Func));
  CHECK(s.hasExact("nullptr", Tok::Keyword));
  CHECK(s.tokOfText("cin") == Tok::Normal);
  s = scan(h, "  vector<vector<int>> g(n + 1);");
  CHECK(s.hasExact("vector", Tok::Type));   // STL 名进类型表
  CHECK(s.hasExact("g", Tok::Func));        // 标识符紧跟 '(' → Func
  CHECK(s.hasExact("<", Tok::Operator));    // 非 #include 行的 '<' 不是字符串
  CHECK(!s.hasTok(Tok::String));
}

static void testPreproc() {
  caseBegin("preproc");
  Highlighter h(Lang::Cpp);
  Scan s = scan(h, "#include <vector>");
  CHECK(s.hasExact("#include", Tok::Preproc));
  CHECK(s.hasExact("<vector>", Tok::String));
  CHECK(s.out == ST_NONE);

  s = scan(h, "   #  define FOO 1");
  CHECK(s.tokAt(0) == Tok::Normal);        // 行首空白
  CHECK(s.tokAt(3) == Tok::Preproc);       // '#'
  CHECK(s.hasExact("define", Tok::Preproc));
  CHECK(s.hasExact("1", Tok::Number));

  s = scan(h, "#include <no-close");
  CHECK(s.hasExact("<no-close", Tok::String));  // 未闭合 <> 染到行尾
  s = scan(h, "#");
  CHECK(s.sp.size() == 1 && s.sp[0].tok == Tok::Preproc && s.sp[0].len == 1);
  s = scan(h, "int x = 1; # not a directive");
  CHECK(s.tokAt(11) == Tok::Operator);     // '#' 不在行首 → 不是指令
  CHECK(!s.hasTok(Tok::Preproc));

  // #define 续行:置 ST_CONTLINE,续行按常规代码扫描
  s = scan(h, "#define MAX(a, b) \\");
  CHECK(s.out == ST_CONTLINE);
  CHECK(s.hasExact("#define", Tok::Preproc));
  Scan t = scan(h, "  ((a) > (b) ? (a) : (b))", s.out);
  CHECK(t.out == ST_NONE);
  CHECK(!t.hasTok(Tok::Comment));
  Scan u = scan(h, "  int keep = 1; \\", ST_CONTLINE);
  CHECK(u.out == ST_CONTLINE);             // 连续多行续行
  CHECK(u.hasExact("int", Tok::Type));
}

static void testLineComment() {
  caseBegin("line-comment");
  Highlighter h(Lang::Cpp);
  Scan s = scan(h, "int a; // 说明 TODO: 改这里");
  CHECK(s.hasExact("TODO", Tok::TodoInComment));
  CHECK(s.tokAt(7) == Tok::Comment);
  CHECK(s.out == ST_NONE);

  // 续行的行注释:下一行整行仍是注释
  s = scan(h, "// 注释续行 \\");
  CHECK(s.out != ST_NONE);
  CHECK(s.count(Tok::Comment) == 1);
  Scan t = scan(h, "still comment TODO here", s.out);
  CHECK(t.hasExact("TODO", Tok::TodoInComment));
  CHECK(t.tokAt(0) == Tok::Comment);
  CHECK(t.out == ST_NONE);
  // 三行链:第二行也以 \ 结尾
  Scan t2 = scan(h, "second line \\", s.out);
  CHECK(t2.out == s.out);
  CHECK(t2.tokAt(0) == Tok::Comment);
  Scan t3 = scan(h, "int notComment = 1;", t2.out);
  CHECK(t3.tokAt(0) == Tok::Comment);
  Scan t4 = scan(h, "int realCode = 1;", t3.out);
  CHECK(t4.hasExact("int", Tok::Type));
  CHECK(!t4.hasTok(Tok::Comment));

  // TODO 必须按词边界
  s = scan(h, "// TODOS TODO XTODO FIXME XXX xxx");
  CHECK(s.hasExact("TODO", Tok::TodoInComment));
  CHECK(s.hasExact("FIXME", Tok::TodoInComment));
  CHECK(s.hasExact("XXX", Tok::TodoInComment));
  CHECK(s.count(Tok::TodoInComment) == 3);  // TODOS / XTODO / xxx 不算
}

static void testBlockComment() {
  caseBegin("block-comment");
  Highlighter h(Lang::Cpp);
  // 跨 3 行
  Scan a = scan(h, "int x = 1; /* 开始");
  CHECK(a.out == ST_BLOCKCOMMENT);
  CHECK(a.hasExact("int", Tok::Type));
  CHECK(a.tokAt(11) == Tok::Comment);
  Scan b = scan(h, "   中间一行 TODO 还没做 int y;", a.out);
  CHECK(b.out == ST_BLOCKCOMMENT);
  CHECK(b.tokAt(0) == Tok::Comment);
  CHECK(b.hasExact("TODO", Tok::TodoInComment));
  CHECK(!b.hasTok(Tok::Type));            // 注释里的 int 不是类型
  Scan c = scan(h, "结束 */ int z = 2;", b.out);
  CHECK(c.out == ST_NONE);
  CHECK(c.hasExact("int", Tok::Type));
  CHECK(c.tokOfText("z") == Tok::Normal);
  CHECK(c.tokAt(0) == Tok::Comment);

  // 块注释里以 \ 结尾不产生续行状态,仍是块注释
  Scan d = scan(h, "/* aaa \\");
  CHECK(d.out == ST_BLOCKCOMMENT);
  // 单行闭合
  Scan e = scan(h, "a /* x */ b;");
  CHECK(e.out == ST_NONE);
  CHECK(e.hasExact("/* x */", Tok::Comment));
  // 多个块注释
  Scan f = scan(h, "/*a*/ int /*b*/ y;");
  CHECK(f.out == ST_NONE);
  CHECK(f.count(Tok::Comment) == 2);
  CHECK(f.hasExact("int", Tok::Type));
  // 块注释里的 // 与 " 都不特殊
  Scan g = scan(h, "/* // \" 未闭合", ST_NONE);
  CHECK(g.out == ST_BLOCKCOMMENT);
  CHECK(g.count(Tok::Comment) == 1);
  // 注释延续行里的 */ 之后是代码
  Scan i2 = scan(h, "*/", ST_BLOCKCOMMENT);
  CHECK(i2.out == ST_NONE);
  CHECK(i2.sp.size() == 1 && i2.sp[0].tok == Tok::Comment);
}

static void testStringVsComment() {
  caseBegin("string-vs-comment");
  Highlighter h(Lang::Cpp);
  // "/*" 不开注释
  Scan s = scan(h, "const char* p = \"/*\";");
  CHECK(s.out == ST_NONE);
  CHECK(s.hasExact("\"/*\"", Tok::String));
  CHECK(!s.hasTok(Tok::Comment));
  // "//" 不开注释
  s = scan(h, "puts(\"// not a comment\"); x = 1;");
  CHECK(!s.hasTok(Tok::Comment));
  CHECK(s.hasExact("\"// not a comment\"", Tok::String));
  CHECK(s.hasExact("puts", Tok::Func));
  // "*/" 在字符串里不闭合注释(而块注释里的 " 不算字符串)
  s = scan(h, "s = \"a\\\"b\";");             // s = "a\"b";
  CHECK(s.hasExact("\"a\\\"b\"", Tok::String));
  CHECK(s.out == ST_NONE);
  // 转义的反斜杠不吃掉结束引号
  s = scan(h, "s = \"a\\\\\";");              // s = "a\\";
  CHECK(s.hasExact("\"a\\\\\"", Tok::String));
  // 未闭合字符串:染到行尾,不泄漏状态
  s = scan(h, "s = \"abc");
  CHECK(s.out == ST_NONE);
  CHECK(s.hasExact("\"abc", Tok::String));
  // 只有一个引号
  s = scan(h, "\"");
  CHECK(s.sp.size() == 1 && s.sp[0].tok == Tok::String && s.sp[0].len == 1);
  CHECK(s.out == ST_NONE);
  // 中文字符串:整段一个 String,多字节不被切开
  s = scan(h, "string t = \"中文，逗号\";");
  CHECK(s.hasExact("\"中文，逗号\"", Tok::String));
  CHECK(s.hasExact("string", Tok::Type));
}

static void testRawString() {
  caseBegin("raw-string");
  Highlighter h(Lang::Cpp);
  Scan s = scan(h, "auto j = R\"({\"k\": 1})\";");
  CHECK(s.out == ST_NONE);
  CHECK(s.hasExact("R\"({\"k\": 1})\"", Tok::String));
  s = scan(h, "auto j = R\"json({\"k\": 1})json\";");
  CHECK(s.out == ST_NONE);
  CHECK(s.hasExact("R\"json({\"k\": 1})json\"", Tok::String));
  // 跨行(近似):置 ST_RAWSTRING,后续整行 String 直到出现 )"
  s = scan(h, "auto t = R\"(第一行");
  CHECK(s.out == ST_RAWSTRING);
  Scan b = scan(h, "中间任意 // /* \" 内容", s.out);
  CHECK(b.out == ST_RAWSTRING);
  CHECK(b.sp.size() == 1 && b.sp[0].tok == Tok::String);
  Scan c = scan(h, "末行)\"; int z = 0;", b.out);
  CHECK(c.out == ST_NONE);
  CHECK(c.hasExact("int", Tok::Type));
  CHECK(c.tokAt(0) == Tok::String);
  // u8R / LR 前缀
  s = scan(h, "auto a = u8R\"(x)\", b = LR\"(y)\";");
  CHECK(s.hasExact("u8R\"(x)\"", Tok::String));
  CHECK(s.hasExact("LR\"(y)\"", Tok::String));
  // 不是原始字符串的 R:普通标识符 + 普通字符串
  s = scan(h, "int R = 1; foo(RR\"x\");");
  CHECK(s.tokOfText("R =") == Tok::Normal);
  CHECK(s.hasExact("\"x\"", Tok::String));
}

static void testNumbers() {
  caseBegin("numbers");
  Highlighter h(Lang::Cpp);
  struct { const char* expr; const char* num; } cs[] = {
      {"x = 0x1f;", "0x1f"},
      {"x = 0X1F;", "0X1F"},
      {"x = 0b1010;", "0b1010"},
      {"x = 1'000'000;", "1'000'000"},
      {"x = 1e-9;", "1e-9"},
      {"x = 1E+10;", "1E+10"},
      {"x = 3.14f;", "3.14f"},
      {"x = 1ULL;", "1ULL"},
      {"x = 0xdeadBEEFull;", "0xdeadBEEFull"},
      {"x = .5;", ".5"},
      {"x = 2.;", "2."},
      {"x = 0;", "0"},
      {"x = 007;", "007"},
      {"x = 0b1011'0010;", "0b1011'0010"},
      {"x = 0x1'2p3;", "0x1'2p3"},
      {"x = 1.5e10L;", "1.5e10L"},
      {"x = 100_km;", "100_km"},
  };
  for (const auto& c : cs) {
    Scan s = scan(h, c.expr);
    CHECK(s.hasExact(c.num, Tok::Number));
    CHECK(s.count(Tok::Number) == 1);
    CHECK(!s.hasTok(Tok::Char));   // 数字分隔符 ' 绝不能被当成字符字面量
    CHECK(s.out == ST_NONE);
  }
  // 数字后面紧跟标识符边界
  Scan s = scan(h, "for (int i = 0; i < 10; i += 2) sum += a[i];");
  CHECK(s.count(Tok::Number) == 3);
  CHECK(s.hasExact("for", Tok::Keyword));
  // 'e' 后面不是数字就不算指数
  s = scan(h, "x = 1else;");
  CHECK(s.hasExact("1else", Tok::Number));  // 已知近似:后缀一律吞进数字
}

static void testCharLiteral() {
  caseBegin("char-literal");
  Highlighter h(Lang::Cpp);
  struct { const char* expr; const char* lit; } cs[] = {
      {"c = 'a';", "'a'"},
      {"c = '\\n';", "'\\n'"},
      {"c = '\\'';", "'\\''"},
      {"c = '\\x41';", "'\\x41'"},
      {"c = '\\\\';", "'\\\\'"},
      {"c = '0';", "'0'"},
  };
  for (const auto& c : cs) {
    Scan s = scan(h, c.expr);
    CHECK(s.hasExact(c.lit, Tok::Char));
    CHECK(s.count(Tok::Char) == 1);
    CHECK(s.out == ST_NONE);
  }
  // 数字里的 ' 被数字扫描器消化,不会开一个字符字面量
  Scan s = scan(h, "ll a = 1'000, b = 'x';");
  CHECK(s.hasExact("1'000", Tok::Number));
  CHECK(s.hasExact("'x'", Tok::Char));
  CHECK(s.count(Tok::Char) == 1);
  // 未闭合字符字面量:染到行尾但不泄漏状态
  s = scan(h, "c = 'a");
  CHECK(s.out == ST_NONE);
  CHECK(s.hasExact("'a", Tok::Char));
  s = scan(h, "'");
  CHECK(s.sp.size() == 1 && s.sp[0].tok == Tok::Char && s.sp[0].len == 1);
}

static void testKeywordTables() {
  caseBegin("keyword-tables");
  // C++ 有、C 没有
  for (const char* w : {"class", "template", "namespace", "new", "delete",
                        "nullptr", "true", "false", "try", "catch", "throw",
                        "public", "private", "virtual", "using", "operator",
                        "constexpr", "decltype", "typename", "this"}) {
    CHECK(Highlighter::isKeyword(w, Lang::Cpp));
    CHECK(!Highlighter::isKeyword(w, Lang::C));
  }
  // 两种语言共有
  for (const char* w : {"if", "else", "for", "while", "return", "struct",
                        "switch", "case", "sizeof", "static", "const",
                        "typedef", "enum", "union", "goto", "extern"}) {
    CHECK(Highlighter::isKeyword(w, Lang::Cpp));
    CHECK(Highlighter::isKeyword(w, Lang::C));
  }
  // C 独有
  for (const char* w : {"_Bool", "_Atomic", "_Generic", "_Static_assert"}) {
    CHECK(Highlighter::isKeyword(w, Lang::C));
  }
  // 类型表
  for (const char* w : {"int", "char", "double", "size_t", "int64_t", "void", "FILE"}) {
    CHECK(Highlighter::isTypeWord(w, Lang::Cpp));
    CHECK(Highlighter::isTypeWord(w, Lang::C));
  }
  for (const char* w : {"string", "vector", "map", "set", "pair", "queue",
                        "priority_queue", "ll", "unordered_map"}) {
    CHECK(Highlighter::isTypeWord(w, Lang::Cpp));
    CHECK(!Highlighter::isTypeWord(w, Lang::C));   // C 下不是类型
  }
  CHECK(!Highlighter::isKeyword("myvar", Lang::Cpp));
  CHECK(!Highlighter::isTypeWord("myvar", Lang::Cpp));
  CHECK(!Highlighter::isKeyword("", Lang::Cpp));
  CHECK(!Highlighter::isTypeWord("", Lang::C));
  // Lang::Unknown 按 C++ 处理
  CHECK(Highlighter::isKeyword("class", Lang::Unknown));
  CHECK(Highlighter::isTypeWord("vector", Lang::Unknown));

  // 语言模式在 scanLine 里生效
  const std::string src = "class Foo { vector<int> v; };";
  Highlighter cpp(Lang::Cpp), c(Lang::C);
  Scan a = scan(cpp, src);
  CHECK(a.hasExact("class", Tok::Keyword));
  CHECK(a.hasExact("vector", Tok::Type));
  Scan b = scan(c, src);
  CHECK(b.tokOfText("class") == Tok::Normal);
  CHECK(b.tokOfText("vector") == Tok::Normal);
  // setLang / lang
  Highlighter m(Lang::C);
  CHECK(m.lang() == Lang::C);
  m.setLang(Lang::Cpp);
  CHECK(m.lang() == Lang::Cpp);
  CHECK(scan(m, src).hasExact("class", Tok::Keyword));
}

static void testEdgeCases() {
  caseBegin("edge-cases");
  Highlighter h(Lang::Cpp);
  // 空行:0 个 span
  for (uint8_t in : {static_cast<uint8_t>(ST_NONE),
                     static_cast<uint8_t>(ST_BLOCKCOMMENT),
                     static_cast<uint8_t>(ST_RAWSTRING),
                     static_cast<uint8_t>(ST_CONTLINE),
                     static_cast<uint8_t>(ST_CONTLINE | ST_BLOCKCOMMENT),
                     static_cast<uint8_t>(0xFF)}) {
    Scan s = scan(h, "", in);
    CHECK(s.sp.empty());
  }
  // 单字符极端输入
  const char* singles[] = {"/", "*", "\\", "\"", "'", "#", "?", ".", "<", ">",
                           "&", "|", "^", "~", "!", "%", ":", ";", ",", "(",
                           ")", "[", "]", "{", "}", "=", "+", "-", " ", "\t"};
  for (const char* one : singles) {
    Scan s = scan(h, one);
    CHECK(s.sp.size() == 1);
    CHECK(s.sp[0].start == 0 && s.sp[0].len == 1);
  }
  CHECK(scan(h, "/").sp[0].tok == Tok::Operator);
  CHECK(scan(h, "\\").out == ST_CONTLINE);
  // 只有 "/*" / "*/"
  CHECK(scan(h, "/*").out == ST_BLOCKCOMMENT);
  CHECK(scan(h, "//").out == ST_NONE);
  CHECK(scan(h, "*/").out == ST_NONE);
  // 未知状态位被忽略
  Scan s = scan(h, "int x;", 0xF8);
  CHECK(s.hasExact("int", Tok::Type));

  // 非法 UTF-8 字节:不崩、覆盖完整
  const std::string bad1 = std::string("int x = 1; // ") + "\xff\xfe\x80\xc3";
  scan(h, bad1);
  const std::string bad2 = std::string("\x80\x80 int ") + "\xc3" + " y;";
  scan(h, bad2);
  std::string bad3;
  for (int i = 1; i < 256; ++i) bad3.push_back(static_cast<char>(i));
  scan(h, bad3);
  scan(h, bad3, ST_BLOCKCOMMENT);
  scan(h, bad3, ST_RAWSTRING);
  scan(h, std::string(1, '\0') + "int x;");   // 内嵌 NUL
  // 中文:注释、字符串、标识符里都不能切断多字节字符
  Scan z = scan(h, "int 变量名 = 1; // 中文注释里的 TODO 项");
  CHECK(z.tokOfText("变量名") == Tok::Normal);
  CHECK(z.hasExact("TODO", Tok::TodoInComment));
  z = scan(h, "// 全中文注释,没有 ASCII");
  CHECK(z.sp.size() == 1 && z.sp[0].tok == Tok::Comment);
  z = scan(h, "中间任意内容", ST_BLOCKCOMMENT);
  CHECK(z.sp.size() == 1 && z.sp[0].tok == Tok::Comment);

  // 极长行:10 万字符
  {
    std::string big;
    big.reserve(100000);
    while (big.size() < 100000) big += "a1 + 你好 \"s\" /*c*/ 'x' 0x1f ";
    big.resize(100000);
    Scan L = scan(h, big);
    CHECK(static_cast<int>(L.sp.size()) > 100);
    std::string spaces(100000, ' ');
    Scan sp2 = scan(h, spaces);
    CHECK(sp2.sp.size() == 1 && sp2.sp[0].len == 100000);
    Scan cm = scan(h, std::string("/* ") + std::string(100000, 'x'));
    CHECK(cm.out == ST_BLOCKCOMMENT);
    Scan q = scan(h, std::string("\"") + std::string(100000, 'q'));
    CHECK(q.sp.size() == 1 && q.sp[0].tok == Tok::String);
  }
}

// ============================== HighlightCache ==============================

static void checkAllLines(TextBuffer& b, const Highlighter& h, HighlightCache& hc) {
  std::vector<Span> out;
  for (int i = 0; i < b.lineCount(); ++i) {
    hc.spansFor(b, h, i, out);
    checkCover(b.line(i), out);
  }
}

// 与缓存无关的“从头老实扫一遍”参考实现
static uint8_t refEntry(const TextBuffer& b, const Highlighter& h, int line) {
  uint8_t st = ST_NONE;
  std::vector<Span> tmp;
  for (int i = 0; i < line; ++i) st = h.scanLine(b.line(i), st, tmp);
  return st;
}

static void testCache() {
  caseBegin("HighlightCache");
  Highlighter h(Lang::Cpp);
  HighlightCache hc;
  TextBuffer b;

  // 空缓存 / 越界
  hc.resize(0);
  CHECK(hc.entryState(b, h, 0) == ST_NONE);
  CHECK(hc.entryState(b, h, -5) == ST_NONE);
  CHECK(hc.entryState(b, h, 999) == ST_NONE);
  std::vector<Span> out;
  hc.spansFor(b, h, -1, out);
  CHECK(out.empty());
  hc.spansFor(b, h, 999, out);
  CHECK(out.empty());

  // 200 行:第 100 行开块注释,第 105 行闭合
  std::vector<std::string> lines;
  for (int i = 0; i < 200; ++i) lines.push_back("int v" + std::to_string(i) + " = " +
                                               std::to_string(i) + ";");
  lines[100] = "/* 块注释从这里开始";
  lines[105] = "结束 */";
  b.reset(lines);
  hc.clear();
  CHECK(hc.validUpTo() == 0);
  hc.resize(b.lineCount());

  CHECK(hc.entryState(b, h, 0) == ST_NONE);
  CHECK(hc.entryState(b, h, 101) == ST_BLOCKCOMMENT);
  CHECK(hc.entryState(b, h, 105) == ST_BLOCKCOMMENT);
  CHECK(hc.entryState(b, h, 106) == ST_NONE);
  CHECK(hc.validUpTo() >= 107);
  // 惰性:只扫到需要的地方(尚未碰过 199 行)
  CHECK(hc.validUpTo() <= 200);
  for (int i = 0; i < 200; ++i) CHECK(hc.entryState(b, h, i) == refEntry(b, h, i));
  CHECK(hc.validUpTo() == 200);
  checkAllLines(b, h, hc);

  // ★ 收敛提前退出:改第 0 行(行数不变)→ invalidateFrom(0)
  //   重扫第 0 行得到的状态与旧缓存相同 → 立刻把有效区推到底,不逐行重扫。
  lines[0] = "int v0 = 42;   // 只改了这一行";
  b.reset(lines);
  hc.resize(b.lineCount());          // 行数没变
  hc.invalidateFrom(0);
  CHECK(hc.validUpTo() <= 1);
  CHECK(hc.entryState(b, h, 1) == ST_NONE);
  CHECK(hc.validUpTo() == 200);      // 收敛后一次性认可后续全部缓存
  CHECK(hc.entryState(b, h, 101) == ST_BLOCKCOMMENT);
  for (int i = 0; i < 200; ++i) CHECK(hc.entryState(b, h, i) == refEntry(b, h, i));

  // ★ 真正会传播的改动:第 0 行敲一个 "/*",全文都进注释
  lines[0] = "/* 从头开始的注释";
  b.reset(lines);
  hc.resize(b.lineCount());
  hc.invalidateFrom(0);
  CHECK(hc.entryState(b, h, 1) == ST_BLOCKCOMMENT);
  CHECK(hc.entryState(b, h, 50) == ST_BLOCKCOMMENT);
  CHECK(hc.entryState(b, h, 101) == ST_BLOCKCOMMENT);
  CHECK(hc.entryState(b, h, 106) == ST_NONE);   // 105 行的 */ 仍然闭合
  for (int i = 0; i < 200; ++i) CHECK(hc.entryState(b, h, i) == refEntry(b, h, i));

  // ★ 行数变化(插行会让旧值与行号错位)必须整段作废,不能误判收敛
  lines[0] = "int v0 = 0;";
  {
    std::vector<std::string> ins = lines;
    ins.insert(ins.begin(), "/* 插在最前面");
    b.reset(ins);
    hc.resize(b.lineCount());
    hc.invalidateFrom(0);
    CHECK(b.lineCount() == 201);
    CHECK(hc.entryState(b, h, 1) == ST_BLOCKCOMMENT);
    CHECK(hc.entryState(b, h, 50) == ST_BLOCKCOMMENT);
    CHECK(hc.entryState(b, h, 106) == ST_BLOCKCOMMENT);  // 老的 */ 现在在 106
    CHECK(hc.entryState(b, h, 107) == ST_NONE);
    for (int i = 0; i < 201; ++i) CHECK(hc.entryState(b, h, i) == refEntry(b, h, i));
    checkAllLines(b, h, hc);
  }
  // 删行(缩小)
  {
    std::vector<std::string> del(lines.begin(), lines.begin() + 3);
    del[0] = "/* a";
    b.reset(del);
    hc.resize(b.lineCount());
    hc.invalidateFrom(0);
    CHECK(b.lineCount() == 3);
    CHECK(hc.entryState(b, h, 2) == ST_BLOCKCOMMENT);
    CHECK(hc.validUpTo() <= 3);
    CHECK(hc.entryState(b, h, 99) == ST_BLOCKCOMMENT);  // 越界夹到末行
    checkAllLines(b, h, hc);
  }
  // 未闭合原始字符串跨越缓存
  {
    b.reset({"auto s = R\"(a", "b", "c)\";", "int z = 1;"});
    hc.clear();
    hc.resize(b.lineCount());
    CHECK(hc.entryState(b, h, 1) == ST_RAWSTRING);
    CHECK(hc.entryState(b, h, 2) == ST_RAWSTRING);
    CHECK(hc.entryState(b, h, 3) == ST_NONE);
    hc.spansFor(b, h, 3, out);
    checkCover(b.line(3), out);
    CHECK(!out.empty() && out[0].tok == Tok::Type);
  }
  // 反复失效 + 随机访问,结果必须始终等于参考实现
  {
    std::vector<std::string> mix = {
        "#include <bits/stdc++.h>", "/* c1", "still", "*/ int a;", "// x \\",
        "cont", "auto r = R\"(q", "raw)\";", "int b = 1'0;", "char c = '\\'';",
        "", "\t", "/* c2", "TODO", "*/"};
    b.reset(mix);
    hc.clear();
    hc.resize(b.lineCount());
    unsigned seed = 12345u;
    for (int iter = 0; iter < 300; ++iter) {
      seed = seed * 1103515245u + 12345u;
      const int from = static_cast<int>((seed >> 8) % static_cast<unsigned>(mix.size()));
      seed = seed * 1103515245u + 12345u;
      const int probe = static_cast<int>((seed >> 8) % static_cast<unsigned>(mix.size()));
      hc.invalidateFrom(from);
      CHECK(hc.entryState(b, h, probe) == refEntry(b, h, probe));
      hc.spansFor(b, h, probe, out);
      checkCover(b.line(probe), out);
    }
    checkAllLines(b, h, hc);
  }
}

int main() {
  testTokNames();
  testCorpusInvariant();
  testPreproc();
  testLineComment();
  testBlockComment();
  testStringVsComment();
  testRawString();
  testNumbers();
  testCharLiteral();
  testKeywordTables();
  testEdgeCases();
  testCache();
  std::printf("test_highlight: OK  (%d 组, %ld 处断言)\n", g_cases, g_checks);
  return 0;
}

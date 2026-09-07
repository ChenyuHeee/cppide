// tests/test_diag.cpp —— src/build.cpp 的行为测试(architecture.md §4.3 / §4.4)
//
// 两层验收:
//
//  A. 纯解析层(不 fork,不需要终端):样本**全部是现场用真实 g++/gcc 编译故意
//     写错的小程序采集来的 stderr**,由 .flower/scripts/gen-diag-samples.py 回填。
//     覆盖:多行错误、warning、note、fatal error、In file included from、
//     "^~~~" 指示行、模板展开的超长错误、路径含 ':' 和空格、中文 locale 输出、
//     空 stderr、纯垃圾文本(含 NUL)、极长单行、病态候选串(防平方级退化)。
//
//  B. 真编译层:用 Builder::compileSpec() + runProcess() **真的**去编译一个故意
//     写错的 .cpp,断言解析出的行号与源码里的错误行号一致 —— 这是"编译错误能跳到
//     对应行号"这条需求的直接证据,而不是靠固化样本自证。同一节里还真编真跑了一个
//     正确的程序(验 runSpec + stdin)、真做了一次头文件语法检查。
//
// 本文件自己绝不允许挂死:main() 一进来就 alarm(180)。
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "build.h"
#include "proc.h"
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
      fprintf(stderr, "\n*** FAIL [%s] %s:%d\n    实得 |%s|\n    期望 |%s|\n",       \
              g_case, __FILE__, __LINE__, aa.substr(0, 400).c_str(),                 \
              bb.substr(0, 400).c_str());                                            \
      fflush(stderr);                                                                \
      abort();                                                                       \
    }                                                                                \
  } while (0)

#define CHECK_EQ_I(a, b)                                                       \
  do {                                                                         \
    ++g_checks;                                                                \
    long aa = (long)(a), bb = (long)(b);                                       \
    if (aa != bb) {                                                            \
      fprintf(stderr, "\n*** FAIL [%s] %s:%d: %ld != %ld\n", g_case, __FILE__, \
              __LINE__, aa, bb);                                               \
      fflush(stderr);                                                          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static void begin(const char* name) {
  g_case = name;
  ++g_cases;
  printf("  ok  %s\n", name);
  fflush(stdout);
}

static int countSev(const std::vector<Diag>& d, DiagSev s) {
  int n = 0;
  for (const Diag& x : d) {
    if (x.sev == s) ++n;
  }
  return n;
}

// 断言第 i 条诊断的全部字段。
static void expectDiag(const std::vector<Diag>& d, std::size_t i, const char* file,
                       int line, int col, DiagSev sev, const char* msg,
                       int out_line) {
  if (i >= d.size()) {
    fprintf(stderr, "\n*** FAIL [%s]: 只有 %zu 条诊断,取不到第 %zu 条\n", g_case,
            d.size(), i);
    abort();
  }
  const Diag& x = d[i];
  CHECK_EQ_S(x.file, file);
  CHECK_EQ_I(x.line, line);
  CHECK_EQ_I(x.col, col);
  CHECK(x.sev == sev);
  CHECK_EQ_S(x.message, msg);
  CHECK_EQ_I(x.out_line, out_line);
}

// 一份可用的默认配置(Config 的默认值就是 §7 规定的)。
static Config baseConfig() {
  Config c;
  return c;
}

// ==== 以下样本由 .flower/scripts/gen-diag-samples.py 现场编译真实的 g++/gcc 采集 ====
// 不要手改:重新生成用 `python3 .flower/scripts/gen-diag-samples.py`。
// 采集环境:g++ (Debian) 14.2.0 / LC_ALL=C。

// 多个 error + 多个 warning(最常见的一屏)
static const char kMultiError[] = R"DIAG(a.cpp: In function 'int main()':
a.cpp:3:11: error: expected primary-expression before ';' token
    3 |   int x = ;
      |           ^
a.cpp:4:3: error: 'undeclared_fn' was not declared in this scope
    4 |   undeclared_fn(1);
      |   ^~~~~~~~~~~~~
a.cpp:6:10: error: 'z' was not declared in this scope
    6 |   return z;
      |          ^
a.cpp:3:7: warning: unused variable 'x' [-Wunused-variable]
    3 |   int x = ;
      |       ^
a.cpp:5:7: warning: unused variable 'y' [-Wunused-variable]
    5 |   int y;
      |       ^
)DIAG";

// In file included from(错误在被包含的头里)
static const char kIncludedFrom[] = R"DIAG(In file included from b.cpp:1:
inc.h: In function 'void f(S)':
inc.h:3:17: error: 'struct S' has no member named 'nonexistent'
    3 | void f(S s) { s.nonexistent = 1; }
      |                 ^~~~~~~~~~~
)DIAG";

// fatal error: 找不到头文件 + compilation terminated.
static const char kFatalError[] = R"DIAG(c.cpp:1:10: fatal error: no_such_header_here.h: No such file or directory
    1 | #include "no_such_header_here.h"
      |          ^~~~~~~~~~~~~~~~~~~~~~~
compilation terminated.
)DIAG";

// error + note: declared here
static const char kWithNote[] = R"DIAG(d.cpp: In function 'int main()':
d.cpp:2:15: error: too few arguments to function 'void g(int, int)'
    2 | int main() { g(1); return 0; }
      |              ~^~~
d.cpp:1:6: note: declared here
    1 | void g(int a, int b) { (void)a; (void)b; }
      |      ^
)DIAG";

// 两级包含链(from 续行)+ static_assert + note
static const char kNestedInclude[] = R"DIAG(In file included from lvl1.h:2,
                 from e.cpp:1:
lvl2.h:2:27: error: static assertion failed: bad int size
    2 | static_assert(sizeof(int) == 99, "bad int size");
      |               ~~~~~~~~~~~~^~~~~
lvl2.h:2:27: note: the comparison reduces to '(4 == 99)'
)DIAG";

// 模板展开的超长错误(required from / 一堆 candidate note)
static const char kTemplateBlowup[] = R"DIAG(In file included from /usr/include/c++/14/algorithm:61,
                 from f.cpp:4:
/usr/include/c++/14/bits/stl_algo.h: In instantiation of 'void std::__sort(_RandomAccessIterator, _RandomAccessIterator, _Compare) [with _RandomAccessIterator = _Rb_tree_iterator<pair<const __cxx11::basic_string<char>, vector<int> > >; _Compare = __gnu_cxx::__ops::_Iter_less_iter]':
/usr/include/c++/14/bits/stl_algo.h:4772:18:   required from 'void std::sort(_RAIter, _RAIter) [with _RAIter = _Rb_tree_iterator<pair<const __cxx11::basic_string<char>, vector<int> > >]'
 4772 |       std::__sort(__first, __last, __gnu_cxx::__ops::__iter_less_iter());
      |       ~~~~~~~~~~~^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
f.cpp:7:12:   required from here
    7 |   std::sort(m.begin(), m.end());
      |   ~~~~~~~~~^~~~~~~~~~~~~~~~~~~~
/usr/include/c++/14/bits/stl_algo.h:1906:50: error: no match for 'operator-' (operand types are 'std::_Rb_tree_iterator<std::pair<const std::__cxx11::basic_string<char>, std::vector<int> > >' and 'std::_Rb_tree_iterator<std::pair<const std::__cxx11::basic_string<char>, std::vector<int> > >')
 1906 |                                 std::__lg(__last - __first) * 2,
      |                                           ~~~~~~~^~~~~~~~~
In file included from /usr/include/c++/14/bits/stl_algobase.h:67,
                 from /usr/include/c++/14/vector:62,
                 from f.cpp:1:
/usr/include/c++/14/bits/stl_iterator.h:618:5: note: candidate: 'template<class _IteratorL, class _IteratorR> constexpr decltype ((__y.base() - __x.base())) std::operator-(const reverse_iterator<_Iterator>&, const reverse_iterator<_IteratorR>&)'
  618 |     operator-(const reverse_iterator<_IteratorL>& __x,
      |     ^~~~~~~~
/usr/include/c++/14/bits/stl_iterator.h:618:5: note:   template argument deduction/substitution failed:
/usr/include/c++/14/bits/stl_algo.h:1906:50: note:   'std::_Rb_tree_iterator<std::pair<const std::__cxx11::basic_string<char>, std::vector<int> > >' is not derived from 'const std::reverse_iterator<_Iterator>'
 1906 |                                 std::__lg(__last - __first) * 2,
      |                                           ~~~~~~~^~~~~~~~~
/usr/include/c++/14/bits/stl_iterator.h:1790:5: note: candidate: 'template<class _IteratorL, class _IteratorR> constexpr decltype ((__x.base() - __y.base())) std::operator-(const move_iterator<_IteratorL>&, const move_iterator<_IteratorR>&)'
 1790 |     operator-(const move_iterator<_IteratorL>& __x,
      |     ^~~~~~~~
/usr/include/c++/14/bits/stl_iterator.h:1790:5: note:   template argument deduction/substitution failed:
/usr/include/c++/14/bits/stl_algo.h:1906:50: note:   'std::_Rb_tree_iterator<std::pair<const std::__cxx11::basic_string<char>, std::vector<int> > >' is not derived from 'const std::move_iterator<_IteratorL>'
 1906 |                                 std::__lg(__last - __first) * 2,
      |                                           ~~~~~~~^~~~~~~~~
)DIAG";

// 路径含 ':' 和空格 —— 行号解析的头号陷阱
static const char kWeirdPath[] = R"DIAG(dir:with space/my file:2.cpp: In function 'int main()':
dir:with space/my file:2.cpp:3:11: error: expected primary-expression before ';' token
    3 |   int x = ;
      |           ^
dir:with space/my file:2.cpp:4:3: error: 'undeclared_fn' was not declared in this scope
    4 |   undeclared_fn(1);
      |   ^~~~~~~~~~~~~
dir:with space/my file:2.cpp:6:10: error: 'z' was not declared in this scope
    6 |   return z;
      |          ^
dir:with space/my file:2.cpp:3:7: warning: unused variable 'x' [-Wunused-variable]
    3 |   int x = ;
      |       ^
dir:with space/my file:2.cpp:5:7: warning: unused variable 'y' [-Wunused-variable]
    5 |   int y;
      |       ^
)DIAG";

// C 编译器(gcc)的输出:error/note/warning 混排
static const char kCLang[] = R"DIAG(h.c: In function 'main':
h.c:3:11: error: initialization of 'int' from 'char *' makes integer from pointer without a cast [-Wint-conversion]
    3 |   int a = "str";
      |           ^~~~~
h.c:4:18: error: 'undefined_thing' undeclared (first use in this function)
    4 |   printf("%d\n", undefined_thing);
      |                  ^~~~~~~~~~~~~~~
h.c:4:18: note: each undeclared identifier is reported only once for each function it appears in
h.c:5:11: error: expected ';' before '}' token
    5 |   return 0
      |           ^
      |           ;
    6 | }
      | ~          
h.c:3:7: warning: unused variable 'a' [-Wunused-variable]
    3 |   int a = "str";
      |       ^
)DIAG";

// 链接期错误(undefined reference + collect2)
static const char kLinkError[] = R"DIAG(/usr/bin/ld: /tmp/ccWvExsx.o: in function `main':
i.cpp:(.text+0x8): undefined reference to `missing_symbol()'
collect2: error: ld returned 1 exit status
)DIAG";

// 驱动层错误:没有行号的 'g++: fatal error: ...'
static const char kDriverError[] = R"DIAG(g++: error: unrecognized command-line option '--nonexistent-option'
g++: fatal error: no input files
compilation terminated.
)DIAG";

// 直接 -fsyntax-only 一个头文件(会带那条无法关掉的假警告)
static const char kHeaderDirect[] = R"DIAG(bad.h:1:9: warning: #pragma once in main file
    1 | #pragma once
      |         ^~~~
bad.h: In function 'int f()':
bad.h:2:18: error: 'notdefined' was not declared in this scope
    2 | int f() { return notdefined; }
      |                  ^~~~~~~~~~
)DIAG";


// 中文 locale 下 gcc 的输出。**手工构造**:本机只有 C / C.utf8 两个 locale
// (locale -a 已确认),装不出真的 zh_CN.UTF-8,但格式照 gcc 的 zh_CN 消息目录写。
// 它的存在只为了证明一件事:一旦忘了给编译子进程加 LC_ALL=C,诊断就**一条也解析不出来**
// —— 用户会看到"编译失败,0 个错误"。这就是 build.cpp 里那条 env_extra 的全部理由。
static const char kChineseLocale[] =
    R"DIAG(a.cpp: 在函数‘int main()’中:
a.cpp:3:11: 错误:expected primary-expression before ‘;’ token
    3 |   int x = ;
      |           ^
a.cpp:3:7: 警告:未使用的变量‘x’ [-Wunused-variable]
    3 |   int x = ;
      |       ^
)DIAG";

// 英文严重度 + 中文正文(用户代码里有中文字符串/标识符时的真实形态)。
static const char kChineseMessage[] =
    R"DIAG(测试/主程序.cpp:7:23: error: 未终止的字符串常量 '你好,世界'
    7 |   const char* s = "你好
      |                       ^
测试/主程序.cpp:9:3: warning: 变量 '计数' 未使用 [-Wunused-variable]
)DIAG";

// ================================================================== A. 纯解析层
static void case_multi_error() {
  begin("多行错误 + 多个警告(真实 g++ 输出)");
  std::vector<Diag> d = Builder::parseDiagnostics(kMultiError);
  CHECK_EQ_I(d.size(), 5);
  CHECK_EQ_I(countSev(d, DiagSev::Error), 3);
  CHECK_EQ_I(countSev(d, DiagSev::Warning), 2);
  // 源码里的错误分别在第 3、4、6 行 —— 这是"能跳到对应行号"的核心断言
  expectDiag(d, 0, "a.cpp", 3, 11, DiagSev::Error,
             "expected primary-expression before ';' token", 1);
  expectDiag(d, 1, "a.cpp", 4, 3, DiagSev::Error,
             "'undeclared_fn' was not declared in this scope", 4);
  expectDiag(d, 2, "a.cpp", 6, 10, DiagSev::Error,
             "'z' was not declared in this scope", 7);
  expectDiag(d, 3, "a.cpp", 3, 7, DiagSev::Warning,
             "unused variable 'x' [-Wunused-variable]", 10);
  expectDiag(d, 4, "a.cpp", 5, 7, DiagSev::Warning,
             "unused variable 'y' [-Wunused-variable]", 13);
  // "a.cpp: In function 'int main()':" 这类上下文行不是诊断
  std::vector<std::string> lines = util::splitLines(kMultiError);
  CHECK(lines.size() > 13);
  CHECK(lines[0].find("In function") != std::string::npos);
  // out_line 必须能反查回原始行(面板双向定位的前提)
  for (const Diag& x : d) {
    CHECK(x.out_line >= 0 && static_cast<std::size_t>(x.out_line) < lines.size());
    CHECK(lines[static_cast<std::size_t>(x.out_line)].find(x.message) !=
          std::string::npos);
  }
}

static void case_included_from() {
  begin("In file included from(单级)");
  std::vector<Diag> d = Builder::parseDiagnostics(kIncludedFrom);
  CHECK_EQ_I(d.size(), 1);   // "In file included from b.cpp:1:" 不计入
  expectDiag(d, 0, "inc.h", 3, 17, DiagSev::Error,
             "'struct S' has no member named 'nonexistent'", 2);
  CHECK(std::string(kIncludedFrom).find("In file included from") == 0);
}

static void case_nested_included_from() {
  begin("In file included from(两级 + from 续行)");
  std::vector<Diag> d = Builder::parseDiagnostics(kNestedInclude);
  // 头两行是 "In file included from lvl1.h:2," 与 "                 from e.cpp:1:"
  // 两者都带 ":<数字>",但都不是诊断
  CHECK_EQ_I(d.size(), 2);
  CHECK_EQ_I(countSev(d, DiagSev::Error), 1);
  CHECK_EQ_I(countSev(d, DiagSev::Note), 1);
  expectDiag(d, 0, "lvl2.h", 2, 27, DiagSev::Error,
             "static assertion failed: bad int size", 2);
  expectDiag(d, 1, "lvl2.h", 2, 27, DiagSev::Note,
             "the comparison reduces to '(4 == 99)'", 5);
}

static void case_fatal_error() {
  begin("fatal error + compilation terminated.");
  std::vector<Diag> d = Builder::parseDiagnostics(kFatalError);
  CHECK_EQ_I(d.size(), 1);
  // "fatal error" 必须归到 Error(而不是被 "error" 抢先切错位置)
  expectDiag(d, 0, "c.cpp", 1, 10, DiagSev::Error,
             "no_such_header_here.h: No such file or directory", 0);
  // "compilation terminated." 没有严重度关键字 => 不是诊断
  CHECK(std::string(kFatalError).find("compilation terminated.") !=
        std::string::npos);
}

static void case_note() {
  begin("note 不计入 errors/warnings");
  std::vector<Diag> d = Builder::parseDiagnostics(kWithNote);
  CHECK_EQ_I(d.size(), 2);
  CHECK_EQ_I(countSev(d, DiagSev::Error), 1);
  CHECK_EQ_I(countSev(d, DiagSev::Warning), 0);
  CHECK_EQ_I(countSev(d, DiagSev::Note), 1);
  expectDiag(d, 0, "d.cpp", 2, 15, DiagSev::Error,
             "too few arguments to function 'void g(int, int)'", 1);
  expectDiag(d, 1, "d.cpp", 1, 6, DiagSev::Note, "declared here", 4);
}

static void case_caret_lines() {
  begin("^~~~ 指示行与源码回显行不计入 diags");
  // 只喂指示行 / 回显行
  const char* only_noise =
      "    3 |   int x = ;\n"
      "      |           ^\n"
      "  123 |     std::sort(a, b);\n"
      "      |     ~~~~~~~~~^~~~~~\n"
      "   ^~~~~~~~~~~\n"
      "        ^\n"
      "~~~~^~~~\n"
      "      |           ;\n"
      "    6 | }\n"
      "      | ~          \n";
  CHECK_EQ_I(Builder::parseDiagnostics(only_noise).size(), 0);
  // 回显的源码里出现 "error:" 也不能被当成诊断(这是最容易踩的假阳性)
  const char* echo_with_keyword =
      "x.cpp:9:3: error: 'q' was not declared in this scope\n"
      "    9 |   // error: this comment is not a diagnostic\n"
      "      |      ^\n"
      "   42 |   warning: neither is this one\n"
      "      |   ^~~~~~~\n";
  std::vector<Diag> d = Builder::parseDiagnostics(echo_with_keyword);
  CHECK_EQ_I(d.size(), 1);
  expectDiag(d, 0, "x.cpp", 9, 3, DiagSev::Error,
             "'q' was not declared in this scope", 0);
}

static void case_template_blowup() {
  begin("模板展开的超长错误");
  std::vector<Diag> d = Builder::parseDiagnostics(kTemplateBlowup);
  // 真实输出:1 个 error + 6 个 note;"required from" / "In instantiation of"
  // 这些上下文行没有严重度关键字,不计入
  CHECK_EQ_I(countSev(d, DiagSev::Error), 1);
  CHECK_EQ_I(countSev(d, DiagSev::Warning), 0);
  CHECK_EQ_I(d.size(), 7);
  expectDiag(d, 0, "/usr/include/c++/14/bits/stl_algo.h", 1906, 50, DiagSev::Error,
             "no match for 'operator-' (operand types are "
             "'std::_Rb_tree_iterator<std::pair<const "
             "std::__cxx11::basic_string<char>, std::vector<int> > >' and "
             "'std::_Rb_tree_iterator<std::pair<const "
             "std::__cxx11::basic_string<char>, std::vector<int> > >')",
             9);
  // 消息不能被截断
  CHECK(d[0].message.size() > 200);
  // 后续全是 note,且行号列号都解析出来了
  for (std::size_t i = 1; i < d.size(); ++i) {
    CHECK(d[i].sev == DiagSev::Note);
    CHECK(d[i].line > 0);
    CHECK(d[i].col > 0);
    CHECK(!d[i].file.empty());
  }
  // 带 ':' 的路径片段("c++/14")不能把 file 切断
  CHECK(d[0].file.find("c++/14") != std::string::npos);
  // "f.cpp:7:12:   required from here" 有 file:line:col 但没有严重度 => 不是诊断
  CHECK(std::string(kTemplateBlowup).find("required from here") !=
        std::string::npos);
  for (const Diag& x : d) {
    CHECK(x.message.find("required from") == std::string::npos);
  }
}

static void case_weird_path() {
  begin("路径含 ':' 与空格(行号解析的头号陷阱)");
  std::vector<Diag> d = Builder::parseDiagnostics(kWeirdPath);
  CHECK_EQ_I(d.size(), 5);
  CHECK_EQ_I(countSev(d, DiagSev::Error), 3);
  CHECK_EQ_I(countSev(d, DiagSev::Warning), 2);
  // 文件名里那个 ":2" 绝不能被当成行号
  expectDiag(d, 0, "dir:with space/my file:2.cpp", 3, 11, DiagSev::Error,
             "expected primary-expression before ';' token", 1);
  expectDiag(d, 1, "dir:with space/my file:2.cpp", 4, 3, DiagSev::Error,
             "'undeclared_fn' was not declared in this scope", 4);
  expectDiag(d, 2, "dir:with space/my file:2.cpp", 6, 10, DiagSev::Error,
             "'z' was not declared in this scope", 7);
  expectDiag(d, 3, "dir:with space/my file:2.cpp", 3, 7, DiagSev::Warning,
             "unused variable 'x' [-Wunused-variable]", 10);
  expectDiag(d, 4, "dir:with space/my file:2.cpp", 5, 7, DiagSev::Warning,
             "unused variable 'y' [-Wunused-variable]", 13);
  for (const Diag& x : d) {
    CHECK_EQ_S(util::basename(x.file), "my file:2.cpp");   // ★ util:: 前缀!
  }
  // 更狠的手工构造:多段 ":<数字>" 混在路径里
  const char* nasty =
      "/tmp/a:1/b:2/c:3.cpp:12:7: error: boom\n"
      "/x:9:8:7:6.cc:100:2: warning: hmm\n"
      "C:/win/style/path.cpp:5:1: note: 也可能来自 Windows 风格路径\n";
  std::vector<Diag> n = Builder::parseDiagnostics(nasty);
  CHECK_EQ_I(n.size(), 3);
  expectDiag(n, 0, "/tmp/a:1/b:2/c:3.cpp", 12, 7, DiagSev::Error, "boom", 0);
  // "/x:9:8:7:6.cc:..." —— ":9" 后面紧跟 ':' 是合法收口,所以这里会被当成行号 9、
  // 列号 8。这是"从左往右取第一个合法 ':<数字>'"规则的必然结果,记录为已知行为。
  CHECK_EQ_I(n[1].line, 9);
  CHECK_EQ_I(n[1].col, 8);
  CHECK(n[1].sev == DiagSev::Warning);
  expectDiag(n, 2, "C:/win/style/path.cpp", 5, 1, DiagSev::Note,
             "也可能来自 Windows 风格路径", 2);
}

static void case_c_compiler() {
  begin("C 编译器(gcc)的混排输出");
  std::vector<Diag> d = Builder::parseDiagnostics(kCLang);
  CHECK_EQ_I(d.size(), 5);
  CHECK_EQ_I(countSev(d, DiagSev::Error), 3);
  CHECK_EQ_I(countSev(d, DiagSev::Warning), 1);
  CHECK_EQ_I(countSev(d, DiagSev::Note), 1);
  expectDiag(d, 0, "h.c", 3, 11, DiagSev::Error,
             "initialization of 'int' from 'char *' makes integer from pointer "
             "without a cast [-Wint-conversion]",
             1);
  expectDiag(d, 3, "h.c", 5, 11, DiagSev::Error, "expected ';' before '}' token", 8);
  expectDiag(d, 4, "h.c", 3, 7, DiagSev::Warning,
             "unused variable 'a' [-Wunused-variable]", 14);
}

static void case_no_line_number() {
  begin("没有行号的诊断(驱动层 / 链接期)");
  std::vector<Diag> d = Builder::parseDiagnostics(kDriverError);
  CHECK_EQ_I(d.size(), 2);
  CHECK_EQ_I(countSev(d, DiagSev::Error), 2);
  expectDiag(d, 0, "g++", 0, 0, DiagSev::Error,
             "unrecognized command-line option '--nonexistent-option'", 0);
  expectDiag(d, 1, "g++", 0, 0, DiagSev::Error, "no input files", 1);

  std::vector<Diag> l = Builder::parseDiagnostics(kLinkError);
  // "/usr/bin/ld: xxx.o: in function `main':" 与 "i.cpp:(.text+0x8): undefined
  // reference to ..." 都没有严重度关键字;只有 collect2 那行是诊断。
  CHECK_EQ_I(l.size(), 1);
  expectDiag(l, 0, "collect2", 0, 0, DiagSev::Error, "ld returned 1 exit status", 2);
}

static void case_header_direct() {
  begin("直接 -fsyntax-only 头文件(含无法关闭的假警告)");
  std::vector<Diag> d = Builder::parseDiagnostics(kHeaderDirect);
  CHECK_EQ_I(d.size(), 2);
  expectDiag(d, 0, "bad.h", 1, 9, DiagSev::Warning, "#pragma once in main file", 0);
  expectDiag(d, 1, "bad.h", 2, 18, DiagSev::Error,
             "'notdefined' was not declared in this scope", 4);
  // 正因为有这条假警告,compileSpec 对头文件走 stdin TU(见 case_compile_spec_header)
}

static void case_chinese() {
  begin("中文 locale 输出 / 中文正文");
  // 中文严重度关键字一条都认不出来 —— 这正是 LC_ALL=C 存在的理由
  std::vector<Diag> d = Builder::parseDiagnostics(kChineseLocale);
  CHECK_EQ_I(d.size(), 0);
  // 英文关键字 + 中文正文:必须解析出来,且正文字节完全保留
  std::vector<Diag> m = Builder::parseDiagnostics(kChineseMessage);
  CHECK_EQ_I(m.size(), 2);
  expectDiag(m, 0, "测试/主程序.cpp", 7, 23, DiagSev::Error,
             "未终止的字符串常量 '你好,世界'", 0);
  expectDiag(m, 1, "测试/主程序.cpp", 9, 3, DiagSev::Warning,
             "变量 '计数' 未使用 [-Wunused-variable]", 3);
  CHECK_EQ_I(countSev(m, DiagSev::Error), 1);
  CHECK_EQ_I(countSev(m, DiagSev::Warning), 1);
}

static void case_degenerate_input() {
  begin("空 stderr / 纯垃圾 / 极长单行 / 病态候选串");
  // 空
  CHECK_EQ_I(Builder::parseDiagnostics("").size(), 0);
  CHECK_EQ_I(Builder::parseDiagnostics("\n").size(), 0);
  CHECK_EQ_I(Builder::parseDiagnostics("\n\n\n\n").size(), 0);
  CHECK_EQ_I(Builder::parseDiagnostics("   \t  \n \n").size(), 0);
  CHECK_EQ_I(Builder::parseDiagnostics("\r\n\r\n").size(), 0);
  // 纯垃圾(含 NUL、含所有非零字节)
  {
    std::string junk;
    for (int i = 1; i < 256; ++i) junk += static_cast<char>(i);
    junk += '\0';
    junk += "::::::\n:::1:1:1\n";
    junk += ": : : \n";
    junk += "\x01\x02\x03 error\n";        // "error" 后面没有 ':' => 不算
    junk += std::string(1, '\0') + "error:\n";
    std::vector<Diag> d = Builder::parseDiagnostics(junk);
    for (const Diag& x : d) {
      CHECK(x.line >= 0 && x.col >= 0 && x.out_line >= 0);
    }
  }
  // 伪随机字节流:只要求不崩、字段自洽
  {
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    std::string s;
    for (int i = 0; i < 40000; ++i) {
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      char c = static_cast<char>((seed >> 33) & 0x7F);
      if (c == 0) c = ' ';
      s += c;
    }
    std::vector<Diag> d = Builder::parseDiagnostics(s);
    for (const Diag& x : d) CHECK(x.line >= 0 && x.out_line >= 0);
  }
  // 极长单行:1MB 的消息
  {
    std::string s = "big.cpp:5:3: error: ";
    s += std::string(1u << 20, 'x');
    std::vector<Diag> d = Builder::parseDiagnostics(s);
    CHECK_EQ_I(d.size(), 1);
    CHECK_EQ_S(d[0].file, "big.cpp");
    CHECK_EQ_I(d[0].line, 5);
    CHECK_EQ_I(d[0].col, 3);
    CHECK_EQ_I(d[0].message.size(), 1u << 20);
  }
  // 极长单行 + 没有严重度:必须快速否掉
  {
    std::string s(4u << 20, 'y');
    CHECK_EQ_I(Builder::parseDiagnostics(s).size(), 0);
  }
  // 病态候选串:10 万个"看着像行号但收不了口"的 ":<数字>x"。
  // 若实现对每个候选都重新全行搜一遍严重度,这里就会退化成平方级并卡死。
  {
    std::string s;
    s.reserve(400000);
    for (int i = 0; i < 100000; ++i) s += ":7q";
    s += " error: pathological";
    int64_t t0 = util::nowMs();
    std::vector<Diag> d = Builder::parseDiagnostics(s);
    int64_t dt = util::nowMs() - t0;
    printf("      病态候选串(30 万字符)解析耗时 %lld ms\n", (long long)dt);
    CHECK(dt < 3000);                 // 平方级实现会远远超过
    CHECK_EQ_I(d.size(), 1);
    CHECK_EQ_I(d[0].line, 0);         // 一个候选都没收口 => 无行号
    CHECK_EQ_S(d[0].message, "pathological");
  }
  // 大量行:5 万行诊断
  {
    std::string s;
    for (int i = 1; i <= 50000; ++i) {
      s += "m.cpp:" + std::to_string(i) + ":1: warning: w\n";
    }
    int64_t t0 = util::nowMs();
    std::vector<Diag> d = Builder::parseDiagnostics(s);
    int64_t dt = util::nowMs() - t0;
    printf("      5 万行诊断解析耗时 %lld ms\n", (long long)dt);
    CHECK_EQ_I(d.size(), 50000);
    CHECK_EQ_I(d[0].line, 1);
    CHECK_EQ_I(d[49999].line, 50000);
    CHECK_EQ_I(d[49999].out_line, 49999);
    CHECK(dt < 5000);
  }
  // 边界形态
  {
    std::vector<Diag> d = Builder::parseDiagnostics("a.cpp:7: error: 没有列号\n");
    CHECK_EQ_I(d.size(), 1);
    CHECK_EQ_I(d[0].line, 7);
    CHECK_EQ_I(d[0].col, 0);
    CHECK_EQ_S(d[0].file, "a.cpp");
  }
  {
    // 行号后面直接是空格(某些编译器风格)
    std::vector<Diag> d = Builder::parseDiagnostics("a.cpp:7 error: x\n");
    CHECK_EQ_I(d.size(), 1);
    CHECK_EQ_I(d[0].line, 7);
    CHECK_EQ_S(d[0].file, "a.cpp");
  }
  {
    // 只有严重度,没有文件
    std::vector<Diag> d = Builder::parseDiagnostics("error: no file at all\n");
    CHECK_EQ_I(d.size(), 1);
    CHECK_EQ_S(d[0].file, "");
    CHECK_EQ_I(d[0].line, 0);
    CHECK_EQ_S(d[0].message, "no file at all");
  }
  {
    // 严重度后面没有正文
    std::vector<Diag> d = Builder::parseDiagnostics("a.cpp:1:1: error:\n");
    CHECK_EQ_I(d.size(), 1);
    CHECK_EQ_S(d[0].message, "");
  }
  {
    // 天文数字行号不能溢出(实现里夹在 1e8)
    std::string s = "a.cpp:";
    s += std::string(60, '9');
    s += ":1: error: overflow\n";
    std::vector<Diag> d = Builder::parseDiagnostics(s);
    CHECK_EQ_I(d.size(), 1);
    CHECK(d[0].line > 0);
    CHECK(d[0].line <= 1000000000);
  }
  // diagSevZh 全覆盖
  CHECK_EQ_S(diagSevZh(DiagSev::Error), "错误");
  CHECK_EQ_S(diagSevZh(DiagSev::Warning), "警告");
  CHECK_EQ_S(diagSevZh(DiagSev::Note), "注");
  CHECK_EQ_S(diagSevZh(DiagSev::Info), "信息");
}

// ================================================================== 装配层
static void case_compile_spec_cpp() {
  begin("compileSpec:C++ 源文件");
  Config cfg = baseConfig();
  Builder b(cfg);
  ProcSpec s = b.compileSpec("/tmp/a b/x y.cpp", Lang::Cpp, "/tmp/out dir/bin");
  CHECK_EQ_S(s.prog, cfg.cxx);
  // flags 逐个进 argv,最后是 -o out src;**没有任何 shell quoting**
  CHECK_EQ_I(s.args.size(), cfg.cxxflags.size() + 3);
  for (std::size_t i = 0; i < cfg.cxxflags.size(); ++i) {
    CHECK_EQ_S(s.args[i], cfg.cxxflags[i]);
  }
  CHECK_EQ_S(s.args[cfg.cxxflags.size() + 0], "-o");
  CHECK_EQ_S(s.args[cfg.cxxflags.size() + 1], "/tmp/out dir/bin");
  CHECK_EQ_S(s.args[cfg.cxxflags.size() + 2], "/tmp/a b/x y.cpp");
  // 含空格的路径原样传递,一个引号一个反斜杠都不许加
  for (const std::string& a : s.args) {
    CHECK(a.find('\\') == std::string::npos);
    CHECK(a.find('\'') == std::string::npos);
    CHECK(a.find('"') == std::string::npos);
  }
  // ★ LC_ALL=C 必须在
  CHECK_EQ_I(s.env_extra.size(), 1);
  CHECK_EQ_S(s.env_extra[0], "LC_ALL=C");
  CHECK_EQ_I(s.timeout_ms, cfg.compile_timeout_ms);
  CHECK(s.stdin_data.empty());
  CHECK(s.max_stderr >= (1u << 20));   // 模板报错很大
  // commandLineForDisplay 只用于回显,不参与执行
  CHECK(commandLineForDisplay(s).find("x y.cpp") != std::string::npos);
}

static void case_compile_spec_c() {
  begin("compileSpec:C 源文件走 cc + cflags");
  Config cfg = baseConfig();
  cfg.cc = "my-cc";
  cfg.cxx = "my-cxx";
  cfg.cflags = {"-std=c99", "-Wall"};
  cfg.cxxflags = {"-std=c++20"};
  cfg.compile_timeout_ms = 12345;
  Builder b(cfg);
  ProcSpec s = b.compileSpec("x.c", Lang::C, "/tmp/x.bin");
  CHECK_EQ_S(s.prog, "my-cc");
  CHECK_EQ_I(s.args.size(), 5);
  CHECK_EQ_S(s.args[0], "-std=c99");
  CHECK_EQ_S(s.args[1], "-Wall");
  CHECK_EQ_S(s.args[2], "-o");
  CHECK_EQ_S(s.args[3], "/tmp/x.bin");
  CHECK_EQ_S(s.args[4], "x.c");
  CHECK_EQ_I(s.timeout_ms, 12345);
  // Lang::Unknown 走 cxx(保守:C++ 前端能吃下 C 的绝大多数写法)
  ProcSpec u = b.compileSpec("x.txt", Lang::Unknown, "/tmp/x.bin");
  CHECK_EQ_S(u.prog, "my-cxx");
  CHECK_EQ_S(u.args[0], "-std=c++20");
}

static void case_compile_spec_header() {
  begin("compileSpec:头文件 -fsyntax-only,不产出二进制");
  Config cfg = baseConfig();
  Builder b(cfg);
  CHECK(Builder::syntaxOnly("a.h"));
  CHECK(Builder::syntaxOnly("a.hpp"));
  CHECK(Builder::syntaxOnly("a.hh"));
  CHECK(Builder::syntaxOnly("a.hxx"));
  CHECK(Builder::syntaxOnly("/x/y/A.H"));      // 扩展名大小写不敏感
  CHECK(!Builder::syntaxOnly("a.cpp"));
  CHECK(!Builder::syntaxOnly("a.c"));
  CHECK(!Builder::syntaxOnly("a"));
  CHECK(!Builder::syntaxOnly(""));
  CHECK(!Builder::syntaxOnly("h"));
  CHECK(!Builder::syntaxOnly("/dir.h/file.cpp"));
  // ---- Wave 5 最后一轮:syntaxOnly 与 langFromPath 用同一份清单 ----
  // 原来这里认 .h++/.hp/.inc 而 langFromPath 不认,main.cpp 又用 langFromPath
  // 当开文件门槛 -> 那三个分支是死代码。现在两处统一:
  CHECK(Builder::syntaxOnly("a.h++"));         // 已进入 langFromPath 的清单
  CHECK(Builder::syntaxOnly("a.hp"));
  CHECK(Builder::syntaxOnly("a.HPP"));
  CHECK(Builder::syntaxOnly("a.Hxx"));
  CHECK(!Builder::syntaxOnly("a.inc"));        // .inc 已被移出支持清单
  CHECK(!Builder::syntaxOnly("a.txt"));        // 不被 langFromPath 认的一律 false
  CHECK(!Builder::syntaxOnly("a.hs"));         // 以 h 开头但不是 C/C++ 头
  CHECK(!Builder::syntaxOnly("a.CPP"));        // 大写源文件仍然是源文件
  CHECK(!Builder::syntaxOnly("a.C"));
  // 不变式:syntaxOnly 为真 => langFromPath 一定认得(否则 main.cpp 打不开它)
  const char* exts[] = {".h", ".hpp", ".hh", ".hxx", ".h++", ".hp",
                        ".H", ".HPP", ".Hp", ".c", ".C", ".cpp", ".inc", ".txt"};
  for (const char* e : exts) {
    const std::string p = std::string("/tmp/x") + e;
    if (Builder::syntaxOnly(p)) CHECK(TextBuffer::langFromPath(p) != Lang::Unknown);
  }

  ProcSpec s = b.compileSpec("/tmp/some dir/thing.hpp", Lang::Cpp, "/tmp/ignored");
  CHECK_EQ_S(s.prog, cfg.cxx);
  // 不能出现 -o(不产出二进制)
  for (const std::string& a : s.args) CHECK(a != "-o");
  for (const std::string& a : s.args) CHECK(a.find("/tmp/ignored") == std::string::npos);
  bool has_syn = false;
  for (const std::string& a : s.args) has_syn = has_syn || a == "-fsyntax-only";
  CHECK(has_syn);
  // 走 stdin TU:见 build.cpp 头注释 4)——直接编头文件会带一条关不掉的假警告
  CHECK_EQ_S(s.args.back(), "-");
  CHECK_EQ_S(s.stdin_data, "#include \"/tmp/some dir/thing.hpp\"\n");
  CHECK_EQ_I(s.timeout_ms, cfg.compile_timeout_ms);
  CHECK_EQ_S(s.env_extra[0], "LC_ALL=C");
  // 相对路径要补成绝对路径,否则子进程 cwd 不同就找不到文件
  ProcSpec r = b.compileSpec("rel.h", Lang::Cpp, "");
  CHECK(r.stdin_data.find("#include \"/") == 0);
  CHECK(r.stdin_data.find("rel.h") != std::string::npos);
  // 文件名里带 '"' 时这招不成立,退回直接编该文件
  ProcSpec q = b.compileSpec("/tmp/we\"ird.h", Lang::Cpp, "");
  CHECK(q.stdin_data.empty());
  CHECK_EQ_S(q.args.back(), "/tmp/we\"ird.h");
  // 就算调用方硬说这是 C,头文件也走 cxx(§4.3 的表)
  ProcSpec c = b.compileSpec("x.h", Lang::C, "");
  CHECK_EQ_S(c.prog, cfg.cxx);
}

static void case_run_spec() {
  begin("runSpec");
  Config cfg = baseConfig();
  cfg.run_timeout_ms = 777;
  cfg.run_output_limit = 4096;
  Builder b(cfg);
  ProcSpec s = b.runSpec("/tmp/dir with space/prog", "1 2 3\n");
  CHECK_EQ_S(s.prog, "/tmp/dir with space/prog");
  CHECK_EQ_I(s.args.size(), 0);
  CHECK_EQ_S(s.stdin_data, "1 2 3\n");
  CHECK_EQ_I(s.timeout_ms, 777);
  CHECK_EQ_I(s.max_stdout, 4096);
  CHECK_EQ_I(s.max_stderr, 4096);
  CHECK(s.hard_kill_bytes >= s.max_stdout);
  CHECK(s.env_extra.empty());        // 用户程序的环境不该被我们改
  // 不含 '/' 的名字必须补 "./",否则 execvp 会去 PATH 里找同名系统命令
  ProcSpec t = b.runSpec("prog", "");
  CHECK_EQ_S(t.prog, "./prog");
  ProcSpec u = b.runSpec("./prog", "");
  CHECK_EQ_S(u.prog, "./prog");
  // 空 stdin 合法(立即 EOF)
  CHECK(b.runSpec("/tmp/x", "").stdin_data.empty());
}

static void case_temp_binary_path() {
  begin("tempBinaryPath:$TMPDIR + hash,不写死 /tmp");
  const std::string p1 = Builder::tempBinaryPath("/home/u/proj/main.cpp");
  const std::string p2 = Builder::tempBinaryPath("/home/u/other/main.cpp");
  const std::string p3 = Builder::tempBinaryPath("/home/u/proj/main.cpp");
  CHECK_EQ_S(p1, p3);                       // 同一源文件必须稳定
  CHECK(p1 != p2);                          // 同名不同目录不能撞车
  CHECK(p1.find(util::tempDir() + "/cppide-") == 0);
  CHECK(p1.find("main") != std::string::npos);
  CHECK(util::basename(p1).find(' ') == std::string::npos);
  // macOS 的 TMPDIR 是带随机后缀的目录,不能写死 /tmp
  const std::string saved = util::envOr("TMPDIR", "");
  CHECK(setenv("TMPDIR", "/var/folders/xy/T", 1) == 0);
  const std::string p4 = Builder::tempBinaryPath("/home/u/proj/main.cpp");
  CHECK(p4.find("/var/folders/xy/T/cppide-") == 0);
  CHECK(setenv("TMPDIR", "/var/folders/xy/T/", 1) == 0);   // 结尾带斜杠
  const std::string p5 = Builder::tempBinaryPath("/home/u/proj/main.cpp");
  CHECK(p5.find("//") == std::string::npos);
  CHECK_EQ_S(p5, p4);
  if (saved.empty()) {
    CHECK(unsetenv("TMPDIR") == 0);
  } else {
    CHECK(setenv("TMPDIR", saved.c_str(), 1) == 0);
  }
  // 奇形怪状的源文件名不能污染 $TMPDIR
  const std::string weird =
      Builder::tempBinaryPath("/tmp/../etc/  a/b:c d;e|f`g$(h).cpp");
  CHECK(weird.find(util::tempDir() + "/cppide-") == 0);
  CHECK(util::basename(weird).find('/') == std::string::npos);
  const char* forbidden = " ;|`$():*?<>\"'\\&\n";
  for (const char* c = forbidden; *c; ++c) {
    CHECK(util::basename(weird).find(*c) == std::string::npos);
  }
  // 空名 / 纯扩展名不能造出隐藏文件或裸目录
  const std::string e1 = Builder::tempBinaryPath("");
  const std::string e2 = Builder::tempBinaryPath("/tmp/.hidden");
  CHECK(util::basename(e1).find("cppide-") == 0);
  CHECK(util::basename(e2).find("cppide-") == 0);
  // 相对路径与它的绝对形式必须映射到同一个 hash
  char cwdbuf[4096];
  CHECK(getcwd(cwdbuf, sizeof cwdbuf) != nullptr);
  const std::string rel = Builder::tempBinaryPath("rel/main.cpp");
  const std::string abs =
      Builder::tempBinaryPath(std::string(cwdbuf) + "/rel/main.cpp");
  CHECK_EQ_S(rel, abs);
}

static void case_binary_stale() {
  begin("binaryStale:mtime 比较");
  char tmpl[] = "/tmp/cppide-stale-XXXXXX";
  CHECK(mkdtemp(tmpl) != nullptr);
  const std::string dir = tmpl;
  const std::string src = dir + "/a.cpp";
  const std::string bin = dir + "/a.bin";
  std::string err;
  CHECK(util::writeFileAtomic(src, "int main(){}\n", err));
  // 二进制不存在 => stale
  CHECK(Builder::binaryStale(src, bin));
  CHECK(Builder::binaryStale(src, ""));
  CHECK(Builder::binaryStale(src, dir + "/nope/deep/x"));
  // 二进制比源文件新 => 不 stale
  usleep(20000);
  CHECK(util::writeFileAtomic(bin, "fake\n", err));
  CHECK(!Builder::binaryStale(src, bin));
  // 源文件被改写 => stale
  usleep(20000);
  CHECK(util::writeFileAtomic(src, "int main(){return 1;}\n", err));
  CHECK(Builder::binaryStale(src, bin));
  // 源文件不存在也保守当 stale(总比"不重编"安全)
  CHECK(Builder::binaryStale(dir + "/gone.cpp", bin));
  CHECK(Builder::binaryStale("", bin));
  unlink(src.c_str());
  unlink(bin.c_str());
  rmdir(dir.c_str());
}

static void case_analyze() {
  begin("analyze:ProcResult -> CompileOutcome");
  // 成功
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::Ok;
    pr.exit_code = 0;
    pr.wall_ms = 320;
    CompileOutcome o = Builder::analyze(pr, "/tmp/bin", false);
    CHECK(o.ok);
    CHECK_EQ_I(o.errors, 0);
    CHECK_EQ_I(o.warnings, 0);
    CHECK_EQ_S(o.binary_path, "/tmp/bin");
    CHECK(!o.syntax_only);
    CHECK_EQ_S(o.summaryZh(), "编译成功 · 用时 320ms");
  }
  // 成功但有警告
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::Ok;
    pr.exit_code = 0;
    pr.wall_ms = 90;
    pr.stderr_text =
        "a.cpp:3:7: warning: unused variable 'x' [-Wunused-variable]\n"
        "    3 |   int x;\n"
        "      |       ^\n";
    CompileOutcome o = Builder::analyze(pr, "/tmp/bin", false);
    CHECK(o.ok);
    CHECK_EQ_I(o.warnings, 1);
    CHECK_EQ_I(o.diags.size(), 1);
    CHECK_EQ_S(o.summaryZh(), "编译成功 · 1 警告 · 用时 90ms");
  }
  // 失败:2 错误 1 警告(§4.3 的例子)
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::Ok;
    pr.exit_code = 1;
    pr.wall_ms = 1400;
    pr.stderr_text =
        "a.cpp:1:1: error: e1\n"
        "a.cpp:2:1: error: e2\n"
        "a.cpp:3:1: warning: w1\n"
        "a.cpp:4:1: note: n1\n";
    CompileOutcome o = Builder::analyze(pr, "/tmp/bin", false);
    CHECK(!o.ok);
    CHECK_EQ_I(o.errors, 2);
    CHECK_EQ_I(o.warnings, 1);
    CHECK_EQ_I(o.diags.size(), 4);
    CHECK_EQ_S(o.summaryZh(), "2 错误 1 警告 · 用时 1.4s");
  }
  // 退出码 0 但有 error 级诊断(-Werror 之类的怪场景)=> 不算成功
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::Ok;
    pr.exit_code = 0;
    pr.stderr_text = "a.cpp:1:1: error: boom\n";
    CompileOutcome o = Builder::analyze(pr, "/tmp/bin", false);
    CHECK(!o.ok);
    CHECK_EQ_I(o.errors, 1);
  }
  // 退出码非 0 但一条诊断都没解析出来 => 也不能报"成功",摘要要说得清
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::Ok;
    pr.exit_code = 4;
    pr.wall_ms = 30;
    pr.stderr_text = "some unparseable compiler noise\n";
    CompileOutcome o = Builder::analyze(pr, "/tmp/bin", false);
    CHECK(!o.ok);
    CHECK_EQ_I(o.errors, 0);
    CHECK_EQ_S(o.summaryZh(), "编译失败(退出码 4) · 用时 30ms");
  }
  // syntax_only:binary_path 必须为空
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::Ok;
    pr.exit_code = 0;
    pr.wall_ms = 55;
    CompileOutcome o = Builder::analyze(pr, "/tmp/bin", true);
    CHECK(o.ok);
    CHECK(o.syntax_only);
    CHECK_EQ_S(o.binary_path, "");
    CHECK_EQ_S(o.summaryZh(), "语法检查通过 · 用时 55ms");
  }
  // 编译器都没起来
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::SpawnFailed;
    pr.spawn_error = "找不到命令:g++-99";
    CompileOutcome o = Builder::analyze(pr, "/tmp/bin", false);
    CHECK(!o.ok);
    CHECK_EQ_S(o.summaryZh(), "无法启动编译器:找不到命令:g++-99");
  }
  // 超时
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::Timeout;
    pr.wall_ms = 30000;
    CompileOutcome o = Builder::analyze(pr, "/tmp/bin", false);
    CHECK(!o.ok);
    CHECK_EQ_S(o.summaryZh(), "编译超时,已终止 · 用时 30.0s");
  }
  // 被信号打死
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::Killed;
    pr.signaled = true;
    pr.signal = 9;
    pr.wall_ms = 12;
    CompileOutcome o = Builder::analyze(pr, "/tmp/bin", false);
    CHECK(!o.ok);
    CHECK_EQ_S(o.summaryZh(), "编译被终止 · 用时 12ms");
  }
  // 默认构造的 CompileOutcome 也不能崩
  {
    CompileOutcome o;
    CHECK(!o.ok);
    CHECK(!o.summaryZh().empty());
  }
  // proc 原样带回(面板要显示完整输出)
  {
    ProcResult pr;
    pr.outcome = ProcOutcome::Ok;
    pr.exit_code = 0;
    pr.stdout_text = "OUT";
    pr.stderr_text = "";
    CompileOutcome o = Builder::analyze(pr, "", false);
    CHECK_EQ_S(o.proc.stdout_text, "OUT");
  }
}

// ================================================================== B. 真编译层
// 这一节是"编译错误能跳到对应行号"的直接证据:真的 fork 一个编译器出来。
static std::string g_dir;

static void writeSrc(const std::string& path, const std::string& body) {
  std::string err;
  if (!util::writeFileAtomic(path, body, err)) {
    fprintf(stderr, "\n*** 写不了 %s:%s\n", path.c_str(), err.c_str());
    abort();
  }
}

static void case_real_compile_line_numbers() {
  begin("★ 真编译:诊断行号必须与源码里的错误行号一致");
  // 故意写错。错误刻意分布在 3、5、7 三行,行 1/2/4/6/8 是好的。
  const std::string src = g_dir + "/broken one.cpp";   // 路径含空格,顺手一起验
  writeSrc(src,
           "#include <string>\n"                       // 1
           "int main() {\n"                            // 2
           "  int a = undefined_symbol_here;\n"        // 3 error
           "  (void)a;\n"                              // 4
           "  std::string s = 5;\n"                    // 5 error
           "  (void)s;\n"                              // 6
           "  return not_declared_either;\n"           // 7 error
           "}\n");                                     // 8
  Config cfg = baseConfig();
  Builder b(cfg);
  const std::string out = Builder::tempBinaryPath(src);
  unlink(out.c_str());
  ProcSpec spec = b.compileSpec(src, Lang::Cpp, out);
  ProcResult pr = runProcess(spec);
  if (pr.outcome == ProcOutcome::SpawnFailed) {
    fprintf(stderr, "\n*** 起不动编译器 %s:%s\n", spec.prog.c_str(),
            pr.spawn_error.c_str());
    abort();
  }
  CompileOutcome o = Builder::analyze(pr, out, false);
  printf("      %s -> %s\n", commandLineForDisplay(spec).c_str(),
         o.summaryZh().c_str());
  CHECK(!o.ok);
  CHECK(o.errors >= 3);
  CHECK(!o.diags.empty());
  // 收集所有落在我们源文件里的 error 行号
  std::set<int> err_lines;
  for (const Diag& d : o.diags) {
    if (d.sev != DiagSev::Error) continue;
    // 只有 basename 与当前缓冲区一致的诊断可跳(§4.3 最后一条)
    if (util::basename(d.file) != util::basename(src)) continue;
    CHECK_EQ_S(d.file, src);       // 编译器回显的就是我们传进去的那个路径
    CHECK(d.line > 0);
    CHECK(d.col > 0);
    CHECK(!d.message.empty());
    err_lines.insert(d.line);
  }
  std::string got;
  for (int l : err_lines) got += " " + std::to_string(l);
  printf("      源文件里的 error 行号:%s(期望 3 5 7)\n", got.c_str());
  CHECK(err_lines.count(3) == 1);
  CHECK(err_lines.count(5) == 1);
  CHECK(err_lines.count(7) == 1);
  CHECK_EQ_I(err_lines.size(), 3);   // 不许多也不许少
  // out_line 必须能在 stderr 里回指到那一行(面板双向定位)
  std::vector<std::string> lines = util::splitLines(pr.stderr_text);
  for (const Diag& d : o.diags) {
    CHECK(d.out_line >= 0);
    CHECK(static_cast<std::size_t>(d.out_line) < lines.size());
    CHECK(lines[static_cast<std::size_t>(d.out_line)].find(d.message) !=
          std::string::npos);
  }
  // 编译失败 => 不该有二进制
  CHECK(!util::fileExists(out));
  CHECK(Builder::binaryStale(src, out));
  // 单错误的最小样本:只有第 4 行错
  const std::string src2 = g_dir + "/one_error.cpp";
  writeSrc(src2, "int main() {\n  int x = 1;\n  (void)x;\n  return oops;\n}\n");
  const std::string out2 = Builder::tempBinaryPath(src2);
  ProcResult pr2 = runProcess(b.compileSpec(src2, Lang::Cpp, out2));
  CompileOutcome o2 = Builder::analyze(pr2, out2, false);
  CHECK(!o2.ok);
  CHECK_EQ_I(o2.errors, 1);
  CHECK_EQ_I(o2.diags[0].line, 4);
  CHECK_EQ_S(o2.diags[0].file, src2);
  printf("      单错误样本:%s:%d:%d %s\n", util::basename(o2.diags[0].file).c_str(),
         o2.diags[0].line, o2.diags[0].col, o2.diags[0].message.c_str());
}

static void case_real_compile_ok_and_run() {
  begin("★ 真编译 + 真运行:成功路径与 runSpec");
  const std::string src = g_dir + "/hello.cpp";
  writeSrc(src,
           "#include <cstdio>\n"
           "int main() {\n"
           "  int a = 0, b = 0;\n"
           "  if (scanf(\"%d %d\", &a, &b) != 2) return 3;\n"
           "  printf(\"%d\\n\", a + b);\n"
           "  fprintf(stderr, \"done\\n\");\n"
           "  return 0;\n"
           "}\n");
  Config cfg = baseConfig();
  Builder b(cfg);
  const std::string out = Builder::tempBinaryPath(src);
  unlink(out.c_str());
  CHECK(Builder::binaryStale(src, out));
  ProcResult pr = runProcess(b.compileSpec(src, Lang::Cpp, out));
  CompileOutcome o = Builder::analyze(pr, out, false);
  if (!o.ok) {
    fprintf(stderr, "\n*** 本该编译成功:%s\n%s\n", o.summaryZh().c_str(),
            pr.stderr_text.c_str());
    abort();
  }
  CHECK_EQ_I(o.errors, 0);
  CHECK_EQ_I(o.warnings, 0);
  CHECK_EQ_I(o.diags.size(), 0);
  CHECK_EQ_S(o.binary_path, out);
  CHECK(util::fileExists(out));
  CHECK(!Builder::binaryStale(src, out));      // 刚编出来,不 stale
  CHECK(o.summaryZh().find("编译成功") == 0);
  // 真跑一遍:stdin 来自【输入】面板(§4.4)
  ProcSpec rs = b.runSpec(out, "40 2\n");
  ProcResult rr = runProcess(rs);
  CHECK(rr.ok());
  CHECK_EQ_S(rr.stdout_text, "42\n");
  CHECK_EQ_S(rr.stderr_text, "done\n");
  printf("      运行结果:%s · %s", rr.summary().c_str(), rr.stdout_text.c_str());
  // 源文件更新 => stale => 需要重编(§4.4 的 Ctrl-R 自动编译判定)
  usleep(20000);
  writeSrc(src, std::string("// touched\n") +
                    "#include <cstdio>\nint main(){ printf(\"x\\n\"); }\n");
  CHECK(Builder::binaryStale(src, out));
  unlink(out.c_str());
}

static void case_real_header_syntax_check() {
  begin("★ 真编译:头文件只做语法检查,不产出二进制、不带假警告");
  const std::string hdr = g_dir + "/bad header.hpp";
  writeSrc(hdr,
           "#pragma once\n"                 // 1
           "struct T { int v; };\n"         // 2
           "inline int f(T t) {\n"          // 3
           "  return t.no_such_field;\n"    // 4 error
           "}\n");                          // 5
  Config cfg = baseConfig();
  Builder b(cfg);
  CHECK(Builder::syntaxOnly(hdr));
  ProcSpec spec = b.compileSpec(hdr, Lang::Cpp, "");
  ProcResult pr = runProcess(spec);
  CompileOutcome o = Builder::analyze(pr, "", true);
  printf("      %s\n", o.summaryZh().c_str());
  CHECK(!o.ok);
  CHECK(o.syntax_only);
  CHECK_EQ_S(o.binary_path, "");
  CHECK(o.errors >= 1);
  // ★ 不能出现那条无法关闭的假警告(证明 stdin TU 这招有效)
  for (const Diag& d : o.diags) {
    CHECK(d.message.find("#pragma once in main file") == std::string::npos);
  }
  CHECK_EQ_I(o.warnings, 0);
  // 错误的文件名/行号必须指向真实的头文件,否则跳不过去
  bool found = false;
  for (const Diag& d : o.diags) {
    if (d.sev == DiagSev::Error && util::basename(d.file) == "bad header.hpp") {
      CHECK_EQ_I(d.line, 4);
      found = true;
    }
  }
  CHECK(found);
  // 没有 .gch、没有 a.out 之类的副产品
  CHECK(!util::fileExists(hdr + ".gch"));
  CHECK(!util::fileExists(g_dir + "/a.out"));

  // 正确的头文件:语法检查干净通过
  const std::string ok_hdr = g_dir + "/good.h";
  writeSrc(ok_hdr, "#pragma once\ninline int g() { return 1; }\n");
  ProcResult pr2 = runProcess(b.compileSpec(ok_hdr, Lang::Cpp, ""));
  CompileOutcome o2 = Builder::analyze(pr2, "", true);
  if (!o2.ok) {
    fprintf(stderr, "\n*** 干净头文件竟然没通过:\n%s\n", pr2.stderr_text.c_str());
    abort();
  }
  CHECK_EQ_I(o2.diags.size(), 0);
  CHECK_EQ_S(o2.summaryZh().substr(0, 18), "语法检查通过");
  CHECK(!util::fileExists(ok_hdr + ".gch"));
}

static void case_real_compiler_missing() {
  begin("★ 真编译:编译器不存在时不崩、有中文说明");
  Config cfg = baseConfig();
  cfg.cxx = "cppide-no-such-compiler-xyz";
  Builder b(cfg);
  const std::string src = g_dir + "/hello2.cpp";
  writeSrc(src, "int main(){}\n");
  ProcSpec spec = b.compileSpec(src, Lang::Cpp, g_dir + "/hello2.bin");
  ProcResult pr = runProcess(spec);
  CHECK(pr.outcome == ProcOutcome::SpawnFailed);
  CompileOutcome o = Builder::analyze(pr, g_dir + "/hello2.bin", false);
  CHECK(!o.ok);
  CHECK(o.summaryZh().find("无法启动编译器") == 0);
  printf("      %s\n", o.summaryZh().c_str());
}

// ------------------------------------------------------------------ main
static void onAlarm(int) {
  const char* m = "\n*** test_diag 超时(180s),现场卡在某个用例上\n";
  ssize_t n = write(2, m, strlen(m));
  (void)n;
  _exit(0x7E);
}

int main() {
  signal(SIGALRM, onAlarm);
  alarm(180);
  printf("test_diag:\n");

  // A. 纯解析
  case_multi_error();
  case_included_from();
  case_nested_included_from();
  case_fatal_error();
  case_note();
  case_caret_lines();
  case_template_blowup();
  case_weird_path();
  case_c_compiler();
  case_no_line_number();
  case_header_direct();
  case_chinese();
  case_degenerate_input();

  // 装配
  case_compile_spec_cpp();
  case_compile_spec_c();
  case_compile_spec_header();
  case_run_spec();
  case_temp_binary_path();
  case_binary_stale();
  case_analyze();

  // B. 真编译
  char tmpl[] = "/tmp/cppide-diag-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) {
    perror("mkdtemp");
    return 1;
  }
  g_dir = tmpl;
  case_real_compile_line_numbers();
  case_real_compile_ok_and_run();
  case_real_header_syntax_check();
  case_real_compiler_missing();
  // 清场(留着也无害,但测试不该往 /tmp 里堆垃圾)
  {
    const char* names[] = {"broken one.cpp", "one_error.cpp", "hello.cpp",
                           "hello2.cpp",     "bad header.hpp", "good.h"};
    for (const char* n : names) unlink((g_dir + "/" + n).c_str());
    unlink(Builder::tempBinaryPath(g_dir + "/one_error.cpp").c_str());
    unlink(Builder::tempBinaryPath(g_dir + "/hello.cpp").c_str());
    unlink(Builder::tempBinaryPath(g_dir + "/broken one.cpp").c_str());
    rmdir(g_dir.c_str());
  }

  printf("test_diag: 全部通过(%d 个用例 / %d 个断言)\n", g_cases, g_checks);
  return 0;
}

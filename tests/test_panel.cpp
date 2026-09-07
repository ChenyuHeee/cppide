// test_panel.cpp —— Panel(输出面板)与 StdinBuffer 单测(纯 assert,main 返回 0 = 全过)
//
// 最重要的三条:
//   * testStreamingChunkEquivalence():同一段含中文/emoji/CRLF/超长行的文本,
//     按 1/2/3/7/随机 字节投喂 vs 一次性投喂,最终面板内容必须**逐字段相同**。
//     AI 流式 delta 一定会把多字节字符切两半,这条不过就等于中文输出会花屏。
//   * testWrapInvariants():width 1..12 与 80 下,每个视觉段的显示宽度 <= width
//     (唯一例外:单个字符本身就比 width 宽),拼回去等于原行(只允许丢断点空白),
//     且**没有任何段的边界落在 UTF-8 多字节字符中间**。
//   * testFlood100MB():灌 100MB 运行输出,行数被 max_lines 限住、最旧被丢、
//     最新内容在、byteSize 有硬上界(内存不会爆)。
//
// 本文件链接 src/panel.cpp src/textbuf.cpp src/util.cpp。不碰终端、不 include curses。
// util 的宽度函数依赖 setlocale(LC_ALL,"") —— main 里自己设。

#include "../src/panel.h"

#include <cassert>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <wchar.h>

#include "../src/textbuf.h"
#include "../src/util.h"

namespace {

int g_checks = 0;
int g_cases = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ_I(a, b)                                                       \
  do {                                                                         \
    ++g_checks;                                                                \
    const long long va_ = (long long)(a), vb_ = (long long)(b);                \
    if (va_ != vb_) {                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,  \
                   __LINE__, #a, #b, va_, vb_);                                \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

#define CHECK_EQ_S(a, b)                                                       \
  do {                                                                         \
    ++g_checks;                                                                \
    const std::string va_ = (a), vb_ = (b);                                    \
    if (va_ != vb_) {                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s\n  got:  [%s]\n  want: [%s]\n",\
                   __FILE__, __LINE__, #a, #b, va_.c_str(), vb_.c_str());      \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

// panel.cpp 里的单行字节上限(超长无换行输出会在 UTF-8 边界硬分行)。
constexpr size_t kMaxLineBytes = 4096;
constexpr int kTabWidth = 4;   // Panel::tab_width_ 的默认值

bool isCont(char c) { return util::utf8IsContinuation(static_cast<unsigned char>(c)); }

std::string segText(const Panel& p, const VisualLine& v) {
  const std::string& t = p.lineAt(v.logical).text;
  CHECK(v.start >= 0 && v.len >= 0);
  CHECK(static_cast<size_t>(v.start) + static_cast<size_t>(v.len) <= t.size());
  return t.substr(static_cast<size_t>(v.start), static_cast<size_t>(v.len));
}

bool sameContent(const Panel& a, const Panel& b) {
  if (a.lineCount() != b.lineCount()) return false;
  for (int i = 0; i < a.lineCount(); ++i) {
    const PanelLine& x = a.lineAt(i);
    const PanelLine& y = b.lineAt(i);
    if (x.text != y.text || x.diag_index != y.diag_index ||
        x.is_stderr != y.is_stderr || x.is_meta != y.is_meta) {
      return false;
    }
  }
  return true;
}

std::string dumpFirstDiff(const Panel& a, const Panel& b) {
  char buf[512];
  if (a.lineCount() != b.lineCount()) {
    std::snprintf(buf, sizeof buf, "lineCount %d vs %d", a.lineCount(), b.lineCount());
    return buf;
  }
  for (int i = 0; i < a.lineCount(); ++i) {
    if (a.lineAt(i).text != b.lineAt(i).text) {
      std::snprintf(buf, sizeof buf, "line %d: [%s] vs [%s]", i,
                    util::clipBytes(a.lineAt(i).text, 80).c_str(),
                    util::clipBytes(b.lineAt(i).text, 80).c_str());
      return buf;
    }
  }
  return "(meta differs)";
}

// 折行不变式:对面板当前 layout 全量检查。
void checkWrapInvariants(const Panel& p, int width, const char* what) {
  const std::vector<VisualLine>& L = p.layout();
  int prev_logical = -1;
  size_t expect_start = 0;
  for (size_t k = 0; k < L.size(); ++k) {
    const VisualLine& v = L[k];
    const std::string& text = p.lineAt(v.logical).text;
    CHECK(text.find('\n') == std::string::npos);        // 面板逻辑行内绝不含 '\n'
    if (v.logical != prev_logical) {
      CHECK_EQ_I(v.logical, prev_logical + 1);          // 逻辑行不跳号、不回退
      CHECK_EQ_I(v.start, 0);                           // 每逻辑行的首段从 0 开始
      prev_logical = v.logical;
      expect_start = 0;
    }
    const std::string seg = segText(p, v);

    // (1) 显示宽度不超 width —— 唯一例外:单个字符本身就比 width 宽。
    const int w = util::displayWidth(seg, kTabWidth, 0);
    const bool single_char = (util::utf8Length(seg) <= 1);
    if (w > width && !single_char) {
      std::fprintf(stderr, "FAIL wrap %s: width=%d seg width=%d [%s]\n", what,
                   width, w, seg.c_str());
      std::abort();
    }
    ++g_checks;

    // (2) 段边界必须落在 UTF-8 字符起点上 —— 绝不能把多字节字符切开。
    if (static_cast<size_t>(v.start) < text.size()) CHECK(!isCont(text[v.start]));
    const size_t end = static_cast<size_t>(v.start) + static_cast<size_t>(v.len);
    if (end < text.size()) CHECK(!isCont(text[end]));
    CHECK(util::utf8Valid(seg) == util::utf8Valid(text));   // 没有切出新的非法序列

    // (3) 拼回去:段之间只允许丢空白,且不许重叠/回退。
    CHECK(static_cast<size_t>(v.start) >= expect_start);
    for (size_t g = expect_start; g < static_cast<size_t>(v.start); ++g) {
      CHECK(text[g] == ' ' || text[g] == '\t');
    }
    expect_start = end;

    // (4) 段尾若还有内容,后面必须紧跟同逻辑行的下一段(否则就是丢内容了)。
    const bool last_of_logical =
        (k + 1 == L.size()) || (L[k + 1].logical != v.logical);
    if (last_of_logical) {
      for (size_t g = end; g < text.size(); ++g) {
        CHECK(text[g] == ' ' || text[g] == '\t');
      }
    }
  }
  if (!L.empty()) CHECK_EQ_I(prev_logical, p.lineCount() - 1);
}

void feedChunks(Panel& p, const std::string& s, size_t chunk) {
  for (size_t i = 0; i < s.size(); i += chunk) {
    p.appendStreaming(s.substr(i, std::min(chunk, s.size() - i)));
  }
  p.endStreaming();
}

long peakRssKb() {
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
#ifdef __APPLE__
  return static_cast<long>(ru.ru_maxrss / 1024);   // macOS 是字节
#else
  return static_cast<long>(ru.ru_maxrss);          // Linux 是 KB
#endif
}

// ------------------------------------------------------------------ 1. 标题
void testTitles() {
  ++g_cases;
  CHECK_EQ_S(panelTitleZh(PanelId::Compile), "编译");
  CHECK_EQ_S(panelTitleZh(PanelId::Run), "运行");
  CHECK_EQ_S(panelTitleZh(PanelId::Ai), "AI");
  CHECK_EQ_S(panelTitleZh(PanelId::Input), "输入");
  CHECK_EQ_S(panelTitleZh(static_cast<PanelId>(99)), "?");     // 越界不崩
  CHECK_EQ_I(kPanelCount, 4);

  Panel c(PanelId::Compile);
  CHECK(c.id() == PanelId::Compile);
  CHECK_EQ_S(c.titleZh(), "编译");
  Panel i(PanelId::Input);
  CHECK_EQ_S(i.titleZh(), "输入");
}

// ------------------------------------------------- 2. 空面板 / 越界安全
void testEmptyPanel() {
  ++g_cases;
  Panel p(PanelId::Ai);
  CHECK_EQ_I(p.lineCount(), 0);
  CHECK_EQ_I(static_cast<int>(p.byteSize()), 0);
  CHECK_EQ_I(p.visualLineCount(), 0);
  CHECK_EQ_I(p.scroll(), 0);
  CHECK(p.autoScroll());
  CHECK_EQ_I(p.cursorLine(), 0);
  CHECK_EQ_S(p.lineAt(0).text, "");
  CHECK_EQ_S(p.lineAt(-1).text, "");
  CHECK_EQ_S(p.lineAt(12345).text, "");
  CHECK_EQ_I(p.lineAt(0).diag_index, -1);
  CHECK(p.lines().empty());

  // 空面板上滚动 / 移光标 / 查映射 —— 一律不崩,坐标合法
  p.setViewSize(40, 5);
  p.scrollBy(5);   CHECK_EQ_I(p.scroll(), 0);
  p.scrollBy(-5);  CHECK_EQ_I(p.scroll(), 0);
  p.scrollTo(999); CHECK_EQ_I(p.scroll(), 0);
  p.scrollToEnd(); CHECK_EQ_I(p.scroll(), 0);
  p.moveCursor(3); CHECK_EQ_I(p.cursorLine(), 0);
  p.moveCursor(-9);CHECK_EQ_I(p.cursorLine(), 0);
  p.setCursorLine(7); CHECK_EQ_I(p.cursorLine(), 0);
  CHECK_EQ_I(p.logicalToVisual(0), 0);
  CHECK_EQ_I(p.logicalToVisual(-3), 0);
  CHECK_EQ_I(p.logicalToVisual(999), 0);
  CHECK_EQ_I(p.visualToLogical(0), 0);
  CHECK_EQ_I(p.visualToLogical(-1), 0);
  CHECK_EQ_I(p.visualToLogical(999), 0);
  CHECK(p.layout().empty());

  // clear() 之后回到同样干净的状态
  p.append("一些内容\n第二行");
  CHECK_EQ_I(p.lineCount(), 2);
  p.clear();
  CHECK_EQ_I(p.lineCount(), 0);
  CHECK_EQ_I(p.visualLineCount(), 0);
  CHECK_EQ_I(p.scroll(), 0);
  CHECK(p.autoScroll());
  CHECK(!p.unread());
}

// ------------------------------------------------- 3. append / appendLine
void testAppendBasics() {
  ++g_cases;
  Panel p(PanelId::Run);
  p.setViewSize(80, 6);

  p.append("");                       // 空串不产生行
  CHECK_EQ_I(p.lineCount(), 0);

  p.append("hello");
  CHECK_EQ_I(p.lineCount(), 1);
  CHECK_EQ_S(p.lineAt(0).text, "hello");
  CHECK(!p.lineAt(0).is_stderr);
  CHECK(!p.lineAt(0).is_meta);
  CHECK_EQ_I(p.lineAt(0).diag_index, -1);

  p.append("a\nb\nc\n");              // 末尾换行不产生空尾行
  CHECK_EQ_I(p.lineCount(), 4);
  CHECK_EQ_S(p.lineAt(3).text, "c");

  p.append("crlf\r\nsecond\r\n");     // CRLF 的 '\r' 被吃掉
  CHECK_EQ_I(p.lineCount(), 6);
  CHECK_EQ_S(p.lineAt(4).text, "crlf");
  CHECK_EQ_S(p.lineAt(5).text, "second");

  p.append("err", true, 7);           // stderr + diag_index
  CHECK(p.lineAt(6).is_stderr);
  CHECK_EQ_I(p.lineAt(6).diag_index, 7);

  p.append("\n\n");                   // 两个空行
  CHECK_EQ_I(p.lineCount(), 9);
  CHECK_EQ_S(p.lineAt(7).text, "");
  CHECK_EQ_S(p.lineAt(8).text, "");

  PanelLine meta;
  meta.text = "g++ -O2 main.cpp";
  meta.is_meta = true;
  p.appendLine(meta);
  CHECK(p.lineAt(9).is_meta);
  CHECK_EQ_S(p.lineAt(9).text, "g++ -O2 main.cpp");

  // appendLine 里内嵌 '\n' 也要拆开(否则折行不变式会被破坏)
  PanelLine multi;
  multi.text = "第一\n第二";
  multi.is_stderr = true;
  multi.diag_index = 3;
  p.appendLine(multi);
  CHECK_EQ_I(p.lineCount(), 12);
  CHECK_EQ_S(p.lineAt(10).text, "第一");
  CHECK_EQ_S(p.lineAt(11).text, "第二");
  CHECK(p.lineAt(11).is_stderr);
  CHECK_EQ_I(p.lineAt(11).diag_index, 3);

  // byteSize:各行字节 + 每行一个换行
  size_t want = 0;
  for (const PanelLine& ln : p.lines()) want += ln.text.size() + 1;
  CHECK_EQ_I(static_cast<int>(p.byteSize()), static_cast<int>(want));

  // lines() 与 lineAt() 一致
  CHECK_EQ_I(static_cast<int>(p.lines().size()), p.lineCount());
  for (int i = 0; i < p.lineCount(); ++i) {
    CHECK(p.lines()[static_cast<size_t>(i)].text == p.lineAt(i).text);
  }
  checkWrapInvariants(p, 80, "append-basics");
}

// ------------------------------------------------- 4. 折行不变式
void testWrapInvariants() {
  ++g_cases;
  const std::vector<std::string> lines = {
      "",
      " ",
      "   \t  ",
      "hello world",
      "短",
      "中文与 English 混排,还有 emoji 🌟🎉 和标点。",
      "tab\there\tand\ttab\t",
      "\t\t缩进很深的中文行,前面全是制表符",
      "supercalifragilisticexpialidociousandthensomemoreverylongword",   // 超长无空格
      "很长的没有空格的中文串很长的没有空格的中文串很长的没有空格的中文串很长的没有空格的中文串",
      "a🌟b🎉c",                                    // 窄宽混排
      "ascii tail with trailing spaces      ",
      "混排aaa中文bbb🌟ccc\tddd eee",
      "e\xCC\x81 combining accent 组合字符",        // 组合字符(宽度 0)
  };

  std::vector<int> widths;
  for (int w = 1; w <= 12; ++w) widths.push_back(w);
  widths.push_back(20);
  widths.push_back(80);

  for (int w : widths) {
    // 全部行放在同一个面板里(顺便验证多逻辑行的连续覆盖)
    Panel p(PanelId::Ai);
    for (const std::string& s : lines) p.append(s.empty() ? std::string("") : s);
    // 空串 append 不产生行,所以单独用 appendLine 补一条真正的空行
    p.appendLine(PanelLine{});
    p.setViewSize(w, 5);
    char what[64];
    std::snprintf(what, sizeof what, "width=%d", w);
    checkWrapInvariants(p, w, what);
    CHECK(p.visualLineCount() >= p.lineCount());     // 每逻辑行至少一段
    CHECK_EQ_I(p.visualToLogical(0), 0);

    // 逐行:段拼起来后,只允许比原行少掉空白字符
    for (int i = 0; i < p.lineCount(); ++i) {
      std::string joined;
      const std::vector<VisualLine>& L = p.layout();
      for (const VisualLine& v : L) {
        if (v.logical == i) joined += segText(p, v);
      }
      std::string a = p.lineAt(i).text, b = joined;
      // 去掉所有空白后必须逐字节相等
      std::string na, nb;
      for (char c : a) if (c != ' ' && c != '\t') na += c;
      for (char c : b) if (c != ' ' && c != '\t') nb += c;
      CHECK_EQ_S(nb, na);
    }
  }

  // 宽度 <= 0:没法折行,退化为“每逻辑行一段、覆盖整行”(不能按宽度 1 折 ——
  // 那会把 20MB 内容炸成两千万个视觉行)
  Panel z(PanelId::Ai);
  z.append("很长的中文行很长的中文行很长的中文行");
  z.setViewSize(0, 3);
  CHECK_EQ_I(z.visualLineCount(), 1);
  CHECK_EQ_I(z.layout()[0].len, static_cast<int>(z.lineAt(0).text.size()));
  z.setViewSize(-5, 3);
  CHECK_EQ_I(z.visualLineCount(), 1);
}

// ---------------------------------- 5. 流式分片鲁棒性(关键测试)
void testStreamingChunkEquivalence() {
  ++g_cases;
  std::string big_run;                            // 触发单行硬分行的超长无换行段
  for (int i = 0; i < 2000; ++i) big_run += "中";  // 6000 字节
  std::string emoji_run;
  for (int i = 0; i < 500; ++i) emoji_run += "🌟"; // 2000 字节,4 字节字符

  const std::string text =
      "AI 回答:\n"
      "第一行中文内容,带 emoji 🌟🎉 与制表符\t结尾\n"
      "second ascii line\r\n"
      "\n"
      "  带前导空白的行  \n"
      + big_run + "\n" + emoji_run + "\n" +
      "e\xCC\x81 组合字符行\n"
      "最后一行没有换行结尾,中文收尾";

  Panel bulk(PanelId::Ai);
  bulk.setViewSize(40, 8);
  bulk.appendStreaming(text);
  bulk.endStreaming();
  CHECK(bulk.lineCount() > 0);
  checkWrapInvariants(bulk, 40, "stream-bulk");

  const size_t chunks[] = {1, 2, 3, 4, 5, 7, 13, 64, 4095, 4096, 4097};
  for (size_t c : chunks) {
    Panel p(PanelId::Ai);
    p.setViewSize(40, 8);
    feedChunks(p, text, c);
    if (!sameContent(bulk, p)) {
      std::fprintf(stderr, "FAIL chunk=%zu: %s\n", c, dumpFirstDiff(bulk, p).c_str());
      std::abort();
    }
    ++g_checks;
    // 折行也必须完全一致
    CHECK_EQ_I(p.visualLineCount(), bulk.visualLineCount());
    for (size_t k = 0; k < p.layout().size(); ++k) {
      CHECK(p.layout()[k].logical == bulk.layout()[k].logical);
      CHECK(p.layout()[k].start == bulk.layout()[k].start);
      CHECK(p.layout()[k].len == bulk.layout()[k].len);
    }
    checkWrapInvariants(p, 40, "stream-chunked");
  }

  // 随机分片(确定性 PRNG),再比一次
  for (int trial = 0; trial < 20; ++trial) {
    unsigned s = 12345u + static_cast<unsigned>(trial) * 7919u;
    Panel p(PanelId::Ai);
    p.setViewSize(40, 8);
    size_t i = 0;
    while (i < text.size()) {
      s = s * 1103515245u + 12345u;
      const size_t n = 1 + (s >> 16) % 11;
      p.appendStreaming(text.substr(i, std::min(n, text.size() - i)));
      i += n;
    }
    p.endStreaming();
    if (!sameContent(bulk, p)) {
      std::fprintf(stderr, "FAIL random trial=%d: %s\n", trial,
                   dumpFirstDiff(bulk, p).c_str());
      std::abort();
    }
    ++g_checks;
  }

  // 每一行都必须是合法 UTF-8(1 字节投喂结束后不能残留半个字符)
  for (int i = 0; i < bulk.lineCount(); ++i) {
    CHECK(util::utf8Valid(bulk.lineAt(i).text));
  }
}

// -------------------- 6. 分片切在多字节中间的“中途状态”也不能崩
void testStreamingMidCharSafety() {
  ++g_cases;
  const std::string zh = "中";        // E4 B8 AD
  Panel p(PanelId::Ai);
  p.setViewSize(3, 2);
  p.appendStreaming(zh.substr(0, 1));            // 只有 lead byte
  // 残缺期间的所有只读操作都必须安全(wrapDisplay 把非法字节当 1 字节 U+FFFD)
  CHECK(p.visualLineCount() >= 1);
  CHECK(p.scroll() >= 0);
  CHECK(p.layout().size() >= 1);
  CHECK_EQ_I(p.visualToLogical(0), 0);
  CHECK_EQ_S(p.lineAt(0).text, zh.substr(0, 1));
  p.appendStreaming(zh.substr(1, 1));
  CHECK(p.visualLineCount() >= 1);
  p.appendStreaming(zh.substr(2, 1));
  p.endStreaming();
  CHECK_EQ_S(p.lineAt(0).text, zh);              // 拼回完整字符
  CHECK(util::utf8Valid(p.lineAt(0).text));
  checkWrapInvariants(p, 3, "midchar");

  // 分片边界正好落在硬分行点附近的多字节字符上
  std::string s;
  for (int i = 0; i < 1500; ++i) s += "漢";      // 4500 字节 > kMaxLineBytes
  Panel a(PanelId::Ai), b(PanelId::Ai);
  a.setViewSize(20, 4);
  b.setViewSize(20, 4);
  a.appendStreaming(s);
  a.endStreaming();
  feedChunks(b, s, 1);
  CHECK(sameContent(a, b));
  CHECK_EQ_I(a.lineCount(), 2);                  // 4500 字节被硬分成两行
  CHECK_EQ_S(a.lineAt(0).text + a.lineAt(1).text, s);
  CHECK(util::utf8Valid(a.lineAt(0).text));
  CHECK(util::utf8Valid(a.lineAt(1).text));
  CHECK(a.lineAt(0).text.size() <= kMaxLineBytes);
}

// ---------------------------------- 7. 流式的换行 / 收尾语义
void testStreamingLineSemantics() {
  ++g_cases;
  Panel p(PanelId::Ai);
  p.setViewSize(40, 4);

  p.appendStreaming("");                       // 空 delta:什么都不发生
  CHECK_EQ_I(p.lineCount(), 0);

  p.appendStreaming("ab");
  CHECK_EQ_I(p.lineCount(), 1);
  p.appendStreaming("cd");                     // 续写同一行
  CHECK_EQ_I(p.lineCount(), 1);
  CHECK_EQ_S(p.lineAt(0).text, "abcd");
  p.appendStreaming("\nxy");                   // 遇 '\n' 才换行
  CHECK_EQ_I(p.lineCount(), 2);
  CHECK_EQ_S(p.lineAt(1).text, "xy");
  p.appendStreaming("\r\nz");                  // CRLF 的 '\r' 不留
  CHECK_EQ_S(p.lineAt(1).text, "xy");
  CHECK_EQ_S(p.lineAt(2).text, "z");
  p.endStreaming();
  CHECK_EQ_I(p.lineCount(), 3);

  // endStreaming 之后的 appendStreaming 另起一行,不再续写
  p.appendStreaming("new");
  CHECK_EQ_I(p.lineCount(), 4);
  CHECK_EQ_S(p.lineAt(3).text, "new");
  p.endStreaming();

  // append() 也会打断流式续写
  Panel q(PanelId::Ai);
  q.appendStreaming("part");
  q.append("whole");
  q.appendStreaming("after");
  CHECK_EQ_I(q.lineCount(), 3);
  CHECK_EQ_S(q.lineAt(0).text, "part");
  CHECK_EQ_S(q.lineAt(1).text, "whole");
  CHECK_EQ_S(q.lineAt(2).text, "after");

  // 末尾换行开出来的空尾行,endStreaming 收掉(与 append 的语义一致)
  Panel r(PanelId::Ai);
  r.appendStreaming("a\nb\n");
  CHECK_EQ_I(r.lineCount(), 3);                // 流式期间末尾那行还“开着”
  r.endStreaming();
  CHECK_EQ_I(r.lineCount(), 2);
  CHECK_EQ_S(r.lineAt(1).text, "b");
  // 有意的空行(连续两个换行)要保住
  Panel r2(PanelId::Ai);
  r2.appendStreaming("a\n\n");
  r2.endStreaming();
  CHECK_EQ_I(r2.lineCount(), 2);
  CHECK_EQ_S(r2.lineAt(1).text, "");
  // 只喂了一个换行
  Panel r3(PanelId::Ai);
  r3.appendStreaming("\n");
  r3.endStreaming();
  CHECK_EQ_I(r3.lineCount(), 1);
  CHECK_EQ_S(r3.lineAt(0).text, "");
  // endStreaming 空调用不崩
  Panel r4(PanelId::Ai);
  r4.endStreaming();
  r4.endStreaming();
  CHECK_EQ_I(r4.lineCount(), 0);

  // 流式续行继承颜色/严重度(stderr 流不会中途变白)
  Panel e(PanelId::Run);
  PanelLine seed;
  seed.text = "";
  seed.is_stderr = true;
  e.appendLine(seed);
  e.appendStreaming("x\ny\nz");                 // appendLine 关掉了流式 -> 另起一行
  CHECK(!e.lineAt(1).is_stderr);
  Panel e2(PanelId::Run);
  e2.appendStreaming("first\nsecond");
  CHECK(!e2.lineAt(1).is_stderr);
}

// ---------------------------------- 8. 环形裁剪(小容量,语义)
void testRingTrimSmall() {
  ++g_cases;
  Panel p(PanelId::Run);
  p.setViewSize(80, 5);
  p.setMaxLines(10);
  for (int i = 0; i < 1000; ++i) p.append("line " + std::to_string(i));
  CHECK(p.lineCount() <= 10);
  CHECK(p.lineCount() >= 1);
  CHECK_EQ_S(p.lineAt(p.lineCount() - 1).text, "line 999");     // 最新在
  const int first = std::atoi(p.lineAt(0).text.c_str() + 5);
  CHECK(first >= 1000 - 10);                                    // 最旧被丢
  checkWrapInvariants(p, 80, "ring-small");

  // setMaxLines(1):只留最新一行
  p.setMaxLines(1);
  CHECK_EQ_I(p.lineCount(), 1);
  CHECK_EQ_S(p.lineAt(0).text, "line 999");
  p.append("tail");
  CHECK_EQ_I(p.lineCount(), 1);
  CHECK_EQ_S(p.lineAt(0).text, "tail");

  // 非法上限被夹到 1
  p.setMaxLines(0);
  CHECK_EQ_I(p.lineCount(), 1);
  p.setMaxLines(-7);
  CHECK_EQ_I(p.lineCount(), 1);
  p.append("x\ny\nz");
  CHECK_EQ_I(p.lineCount(), 1);
  CHECK_EQ_S(p.lineAt(0).text, "z");

  // 放大上限不会凭空造行
  p.setMaxLines(100);
  CHECK_EQ_I(p.lineCount(), 1);

  // 裁剪时光标/滚动坐标跟着上移,不越界
  Panel q(PanelId::Compile);
  q.setViewSize(80, 3);
  q.setMaxLines(20);
  for (int i = 0; i < 20; ++i) q.append("l" + std::to_string(i));
  q.setCursorLine(19);
  q.scrollTo(0);
  for (int i = 20; i < 60; ++i) {
    q.append("l" + std::to_string(i));
    CHECK(q.cursorLine() >= 0 && q.cursorLine() < q.lineCount());
    CHECK(q.scroll() >= 0 && q.scroll() <= std::max(0, q.visualLineCount() - 3));
  }

  // 流式灌入也受裁剪约束
  Panel s(PanelId::Ai);
  s.setViewSize(40, 4);
  s.setMaxLines(5);
  for (int i = 0; i < 500; ++i) s.appendStreaming("stream " + std::to_string(i) + "\n");
  s.endStreaming();
  CHECK(s.lineCount() <= 5);
  CHECK_EQ_S(s.lineAt(s.lineCount() - 1).text, "stream 499");
}

// ---------------------------------- 9. 超长无换行行的硬分行
void testLongLineSplit() {
  ++g_cases;
  std::string big;
  for (int i = 0; i < 4096; ++i) big += "漢";     // 12288 字节,无空格无换行
  Panel p(PanelId::Run);
  p.setViewSize(80, 6);
  p.append(big);
  CHECK(p.lineCount() >= 3);
  std::string joined;
  for (int i = 0; i < p.lineCount(); ++i) {
    CHECK(p.lineAt(i).text.size() <= kMaxLineBytes);
    CHECK(util::utf8Valid(p.lineAt(i).text));     // 分行点在 UTF-8 边界上
    joined += p.lineAt(i).text;
  }
  CHECK_EQ_S(joined, big);
  checkWrapInvariants(p, 80, "long-line");

  // 元信息在硬分行的各段上保留(编译面板跳行仍可用)
  Panel c(PanelId::Compile);
  c.setViewSize(80, 6);
  c.append(big, true, 5);
  for (int i = 0; i < c.lineCount(); ++i) {
    CHECK(c.lineAt(i).is_stderr);
    CHECK_EQ_I(c.lineAt(i).diag_index, 5);
  }

  // 4 字节字符 + 恰好落在上限上的边界
  std::string emo;
  for (int i = 0; i < 2000; ++i) emo += "🌟";     // 8000 字节
  Panel e(PanelId::Ai);
  e.setViewSize(20, 4);
  e.appendStreaming(emo);
  e.endStreaming();
  std::string j2;
  for (int i = 0; i < e.lineCount(); ++i) {
    CHECK(e.lineAt(i).text.size() <= kMaxLineBytes);
    CHECK(util::utf8Valid(e.lineAt(i).text));
    j2 += e.lineAt(i).text;
  }
  CHECK_EQ_S(j2, emo);
}

// ---------------------------------- 10. 灌 100MB 运行输出
void testFlood100MB() {
  ++g_cases;
  Panel p(PanelId::Run);
  p.setViewSize(80, 10);

  const long rss_before = peakRssKb();
  const int64_t t0 = util::nowMs();
  const size_t kChunkTarget = 1u << 20;          // 1MB 一块,共 100 块
  long long total_lines = 0;
  size_t fed = 0;
  std::string chunk;
  chunk.reserve(kChunkTarget + 128);
  for (int c = 0; c < 100; ++c) {
    chunk.clear();
    while (chunk.size() < kChunkTarget) {
      chunk += "行 ";
      chunk += std::to_string(total_lines);
      chunk += " 输出内容 output payload 0123456789 abcdefghij\n";
      ++total_lines;
    }
    fed += chunk.size();
    p.append(chunk, (c % 2) == 1);
  }
  const int64_t ms = util::nowMs() - t0;
  const long rss_after = peakRssKb();

  CHECK(fed >= 100u * 1024u * 1024u);            // 真的灌了 >= 100MB
  CHECK(p.lineCount() <= 5000);                  // 行数被默认上限限住
  CHECK(p.lineCount() > 4000);
  // 内存硬上界:max_lines * (单行上限 + 1)
  CHECK(p.byteSize() <= 5000u * (kMaxLineBytes + 1));
  CHECK(p.byteSize() < 2u * 1024u * 1024u);      // 实际内容远小于此
  // 最新内容在
  const std::string want_last = "行 " + std::to_string(total_lines - 1) +
                                " 输出内容 output payload 0123456789 abcdefghij";
  CHECK_EQ_S(p.lineAt(p.lineCount() - 1).text, want_last);
  // 最旧被丢
  CHECK(p.lineAt(0).text.find("行 0 ") == std::string::npos);
  const long long first_no = std::atoll(p.lineAt(0).text.c_str() + std::strlen("行 "));
  CHECK(first_no >= total_lines - 5000);
  // 灌完之后布局/滚动依然自洽
  CHECK(p.autoScroll());
  CHECK_EQ_I(p.scroll(), std::max(0, p.visualLineCount() - 10));
  checkWrapInvariants(p, 80, "flood");
  CHECK(p.lineAt(p.lineCount() - 1).is_stderr);  // 最后一块是 stderr

  // 无换行的巨型输出(进度条 / 二进制)也不能把内存吃爆
  Panel q(PanelId::Run);
  q.setViewSize(80, 10);
  q.setMaxLines(200);
  std::string noeol(8u << 20, 'x');              // 8MB 一行,无换行
  const int64_t t1 = util::nowMs();
  q.append(noeol);
  const int64_t ms2 = util::nowMs() - t1;
  CHECK(q.lineCount() <= 200);
  CHECK(q.byteSize() <= 200u * (kMaxLineBytes + 1));
  CHECK_EQ_I(static_cast<int>(q.lineAt(0).text.size()), static_cast<int>(kMaxLineBytes));
  const long rss_end = peakRssKb();

  std::printf("  [flood] 灌入 %.1f MB / %lld 行,耗时 %lldms;8MB 无换行行耗时 %lldms\n",
              static_cast<double>(fed) / (1024.0 * 1024.0), total_lines,
              static_cast<long long>(ms), static_cast<long long>(ms2));
  std::printf("  [flood] 保留 %d 行 / %zu 字节;峰值 RSS %ldKB -> %ldKB -> %ldKB\n",
              p.lineCount(), p.byteSize(), rss_before, rss_after, rss_end);
  // 峰值内存不该跟灌入量同阶(100MB 全留下来的话这里必炸)。
  // ASan 有 redzone 开销,门槛放宽到 1GB,但仍能抓住“把 100MB 全存下来”。
  CHECK(rss_end < 1024L * 1024L);
}

// ---------------------------------- 11. 滚动边界
void testScrollEdges() {
  ++g_cases;
  Panel p(PanelId::Run);
  p.setViewSize(80, 5);
  for (int i = 0; i < 10; ++i) p.append("l" + std::to_string(i));
  CHECK_EQ_I(p.visualLineCount(), 10);
  CHECK(p.autoScroll());
  CHECK_EQ_I(p.scroll(), 5);                 // 贴底:10 - 5

  p.scrollTo(0);
  CHECK_EQ_I(p.scroll(), 0);
  CHECK(!p.autoScroll());                    // 离开底部就不再自动跟随
  p.append("l10");
  CHECK_EQ_I(p.scroll(), 0);                 // 不贴底时新内容不抢视口
  p.scrollToEnd();
  CHECK(p.autoScroll());
  CHECK_EQ_I(p.scroll(), 6);                 // 11 - 5

  p.scrollBy(-3);
  CHECK_EQ_I(p.scroll(), 3);
  CHECK(!p.autoScroll());
  p.scrollBy(-100);
  CHECK_EQ_I(p.scroll(), 0);
  p.scrollBy(100);
  CHECK_EQ_I(p.scroll(), 6);
  CHECK(p.autoScroll());                     // 滚回底部重新贴底
  p.scrollTo(-5);
  CHECK_EQ_I(p.scroll(), 0);
  p.scrollTo(99999);
  CHECK_EQ_I(p.scroll(), 6);

  p.setAutoScroll(false);
  CHECK(!p.autoScroll());
  p.scrollTo(2);
  p.setAutoScroll(true);
  CHECK_EQ_I(p.scroll(), 6);

  // 高度 1:每次只看一行
  p.setViewSize(80, 1);
  CHECK_EQ_I(p.scroll(), 10);
  p.scrollTo(0);
  CHECK_EQ_I(p.scroll(), 0);
  p.scrollBy(1);
  CHECK_EQ_I(p.scroll(), 1);

  // 高度 0(面板折叠):不崩,坐标合法
  p.setViewSize(80, 0);
  CHECK(p.scroll() >= 0);
  CHECK(p.scroll() <= p.visualLineCount());
  p.scrollBy(5);
  CHECK(p.scroll() >= 0);
  p.scrollToEnd();
  CHECK(p.scroll() >= 0);

  // 内容比视口少:顶部恒为 0
  Panel s(PanelId::Ai);
  s.setViewSize(80, 10);
  s.append("only one line");
  CHECK_EQ_I(s.visualLineCount(), 1);
  CHECK_EQ_I(s.scroll(), 0);
  s.scrollBy(7);
  CHECK_EQ_I(s.scroll(), 0);
  CHECK(s.autoScroll());
  s.scrollBy(-7);
  CHECK_EQ_I(s.scroll(), 0);

  // 折行后按视觉行滚动
  Panel w(PanelId::Ai);
  w.setViewSize(10, 3);
  for (int i = 0; i < 5; ++i) w.append("这是一行比较长的中文内容需要折行");
  CHECK(w.visualLineCount() > 5);
  const int top_max = w.visualLineCount() - 3;
  CHECK_EQ_I(w.scroll(), top_max);
  w.scrollTo(top_max / 2);
  CHECK_EQ_I(w.scroll(), top_max / 2);
  CHECK(w.visualToLogical(w.scroll()) >= 0);
  CHECK(w.visualToLogical(w.scroll()) < w.lineCount());
}

// ---------------------------------- 12. 未读标记 / 错误数
void testUnreadAndErrorCount() {
  ++g_cases;
  Panel a(PanelId::Compile);
  Panel b(PanelId::Run);
  CHECK(!a.unread());
  CHECK(!b.unread());
  CHECK_EQ_I(a.errorCount(), 0);

  a.append("main.cpp:1:1: error: boom", true, 0);
  CHECK(a.unread());                         // 有新输出 -> 标签上该显示 '*'
  CHECK(!b.unread());                        // 两个面板互不影响
  a.setUnread(false);
  CHECK(!a.unread());
  a.appendLine(PanelLine{});
  CHECK(a.unread());
  a.setUnread(false);
  a.appendStreaming("delta");
  CHECK(a.unread());
  a.setUnread(true);
  CHECK(a.unread());

  a.setErrorCount(2);
  CHECK_EQ_I(a.errorCount(), 2);
  CHECK_EQ_I(b.errorCount(), 0);
  b.setErrorCount(9);
  CHECK_EQ_I(a.errorCount(), 2);
  CHECK_EQ_I(b.errorCount(), 9);
  a.setErrorCount(-3);
  CHECK_EQ_I(a.errorCount(), 0);             // 负数夹到 0

  a.setErrorCount(4);
  a.setUnread(true);
  a.clear();
  CHECK(!a.unread());                        // clear 一并清标签状态
  CHECK_EQ_I(a.errorCount(), 0);
  CHECK_EQ_I(b.errorCount(), 9);             // 别人的不受影响

  // 新建的面板必须拿到干净状态(反复 new/delete 也不串味)
  for (int i = 0; i < 16; ++i) {
    Panel* p = new Panel(PanelId::Ai);
    CHECK(!p->unread());
    CHECK_EQ_I(p->errorCount(), 0);
    p->setUnread(true);
    p->setErrorCount(7);
    delete p;
  }

  // app.h 里是 std::vector<Panel> panels_:emplace_back 触发重分配会**移动**
  // 元素。标签状态必须活过移动,否则标签上的 '*' 会静默消失。
  std::vector<Panel> v;
  v.emplace_back(PanelId::Compile);
  v[0].setUnread(true);
  v[0].setErrorCount(3);
  v[0].append("some output");
  const int lines_before = v[0].lineCount();
  for (int i = 0; i < 8; ++i) v.emplace_back(PanelId::Input);   // 逼它重分配
  CHECK(v[0].unread());
  CHECK_EQ_I(v[0].errorCount(), 3);
  CHECK_EQ_I(v[0].lineCount(), lines_before);
  CHECK(v[0].id() == PanelId::Compile);

  // 标签状态是**每实例**的:同一个 PanelId 的两个同时存活实例互不干扰。
  // (曾经的静态表兜底在这里会失败 —— 头文件补上 unread_/error_count_ 后天然成立。)
  Panel s1(PanelId::Run), s2(PanelId::Run);
  s1.setUnread(true);
  s1.setErrorCount(5);
  CHECK(s1.unread());
  CHECK(!s2.unread());
  CHECK_EQ_I(s1.errorCount(), 5);
  CHECK_EQ_I(s2.errorCount(), 0);
  s2.append("x");                            // s2 变脏不该动 s1 的计数
  s1.setUnread(false);
  CHECK(s2.unread());
  CHECK(!s1.unread());
  CHECK_EQ_I(s1.errorCount(), 5);
  s2.clear();
  CHECK_EQ_I(s1.errorCount(), 5);            // s2.clear() 不该清 s1
  CHECK(!s2.unread());

  // 拷贝构造/拷贝赋值也应带走标签状态(隐式生成的,成员化后自动正确)。
  Panel c1 = s1;
  CHECK_EQ_I(c1.errorCount(), 5);
  c1.setErrorCount(1);
  CHECK_EQ_I(s1.errorCount(), 5);
}

// ---------------------------------- 13. diag_index 携带与双向定位
void testDiagIndex() {
  ++g_cases;
  Panel p(PanelId::Compile);
  p.setViewSize(40, 6);

  PanelLine cmd;
  cmd.text = "g++ -std=c++17 -O2 main.cpp -o /tmp/a.out";
  cmd.is_meta = true;
  p.appendLine(cmd);
  p.append("main.cpp:7:15: error: 'x' was not declared in this scope 这条很长需要折行", true, 0);
  p.append("    7 |   x = 1;", true, -1);
  p.append("      |   ^", true, -1);
  p.append("main.cpp:9:3: warning: unused variable 'y'", true, 1);
  p.append("编译失败:2 错误 1 警告", false, -1);

  CHECK_EQ_I(p.lineCount(), 6);
  // 正向:面板行 -> 诊断下标(Enter 跳转就是读这个)
  CHECK_EQ_I(p.lineAt(0).diag_index, -1);
  CHECK(p.lineAt(0).is_meta);
  CHECK_EQ_I(p.lineAt(1).diag_index, 0);
  CHECK_EQ_I(p.lineAt(2).diag_index, -1);
  CHECK_EQ_I(p.lineAt(3).diag_index, -1);
  CHECK_EQ_I(p.lineAt(4).diag_index, 1);
  CHECK_EQ_I(p.lineAt(5).diag_index, -1);

  // 反向:诊断下标 -> 面板行(Ctrl-N/Ctrl-P 遍历 + 高亮当前诊断行)
  auto lineOfDiag = [&p](int d) {
    for (int i = 0; i < p.lineCount(); ++i) {
      if (p.lineAt(i).diag_index == d) return i;
    }
    return -1;
  };
  CHECK_EQ_I(lineOfDiag(0), 1);
  CHECK_EQ_I(lineOfDiag(1), 4);
  CHECK_EQ_I(lineOfDiag(2), -1);

  // 光标落到诊断行 + 视口跟随(聚焦面板按 Enter 的前提)
  p.setCursorLine(lineOfDiag(1));
  CHECK_EQ_I(p.cursorLine(), 4);
  CHECK_EQ_I(p.lineAt(p.cursorLine()).diag_index, 1);
  const int v = p.logicalToVisual(4);
  CHECK(v >= 0 && v < p.visualLineCount());
  CHECK_EQ_I(p.layout()[static_cast<size_t>(v)].logical, 4);
  CHECK_EQ_I(p.layout()[static_cast<size_t>(v)].start, 0);     // 首个视觉行
  CHECK(p.scroll() <= v);
  CHECK(v < p.scroll() + 6);                                   // 光标行在视口内

  // 折行后仍然对得上:诊断行折成多段,每段都指回同一逻辑行
  int segs_of_1 = 0;
  for (const VisualLine& vl : p.layout()) {
    if (vl.logical == 1) ++segs_of_1;
  }
  CHECK(segs_of_1 > 1);                                        // 那条 error 确实折了
  for (const VisualLine& vl : p.layout()) {
    if (vl.logical == 1) CHECK_EQ_I(p.lineAt(vl.logical).diag_index, 0);
  }

  // 光标上下移动不越界,且总能读回正确的 diag_index
  p.setCursorLine(0);
  for (int i = 0; i < 20; ++i) {
    p.moveCursor(1);
    CHECK(p.cursorLine() >= 0 && p.cursorLine() < p.lineCount());
  }
  CHECK_EQ_I(p.cursorLine(), 5);
  for (int i = 0; i < 20; ++i) {
    p.moveCursor(-1);
    CHECK(p.cursorLine() >= 0);
  }
  CHECK_EQ_I(p.cursorLine(), 0);
  p.setCursorLine(999);
  CHECK_EQ_I(p.cursorLine(), 5);
  p.setCursorLine(-4);
  CHECK_EQ_I(p.cursorLine(), 0);

  // 裁剪后剩下的行仍带着正确的 diag_index
  p.setMaxLines(2);
  CHECK_EQ_I(p.lineCount(), 2);
  CHECK_EQ_I(p.lineAt(0).diag_index, 1);
  CHECK_EQ_I(p.lineAt(1).diag_index, -1);
  CHECK(p.cursorLine() >= 0 && p.cursorLine() < 2);
}

// ---------------------------------- 14. 宽度 80 -> 20 -> 80
void testResizeRewrap() {
  ++g_cases;
  Panel p(PanelId::Ai);
  p.setViewSize(80, 5);
  // 每行在 80 列下都放得下(视觉行号 == 逻辑行号),在 20 列下都要折成多段
  for (int i = 0; i < 40; ++i) {
    p.append("第 " + std::to_string(i) + " 行:中文与 ascii 混排的一行输出内容");
  }
  const int n80 = p.visualLineCount();
  CHECK_EQ_I(n80, 40);                        // 80 列下不折
  checkWrapInvariants(p, 80, "resize-80");

  // 不贴底时:缩放要把用户原来看的那个逻辑行留在顶部
  p.scrollTo(10);
  CHECK_EQ_I(p.scroll(), 10);
  CHECK(!p.autoScroll());
  CHECK_EQ_I(p.visualToLogical(10), 10);

  p.setViewSize(20, 5);
  const int n20 = p.visualLineCount();
  CHECK(n20 > n80 * 2);                       // 20 列下每行折成 3 段以上
  checkWrapInvariants(p, 20, "resize-20");
  CHECK_EQ_I(p.visualToLogical(p.scroll()), 10);          // 锚点逻辑行没跑
  CHECK_EQ_I(p.scroll(), p.logicalToVisual(10));
  CHECK(p.scroll() >= 0 && p.scroll() <= std::max(0, n20 - 5));

  p.setViewSize(80, 5);
  CHECK_EQ_I(p.visualLineCount(), n80);       // 折行结果回到原样
  CHECK_EQ_I(p.scroll(), 10);                 // 滚动位置也回到原样
  CHECK_EQ_I(p.visualToLogical(p.scroll()), 10);
  checkWrapInvariants(p, 80, "resize-80-again");

  // 贴底的面板:缩放后依然贴底
  p.scrollToEnd();
  CHECK(p.autoScroll());
  p.setViewSize(20, 5);
  CHECK(p.autoScroll());
  CHECK_EQ_I(p.scroll(), std::max(0, p.visualLineCount() - 5));
  p.setViewSize(80, 5);
  CHECK(p.autoScroll());
  CHECK_EQ_I(p.scroll(), n80 - 5);

  // 只改高度不需要重算折行,滚动位置合法
  p.scrollTo(3);
  p.setViewSize(80, 20);
  CHECK_EQ_I(p.visualLineCount(), n80);
  CHECK(p.scroll() >= 0 && p.scroll() <= n80 - 20);
  p.setViewSize(80, 3);
  CHECK(p.scroll() >= 0 && p.scroll() <= n80 - 3);

  // 极端缩放序列:反复横跳不崩、坐标始终合法
  const int seq[] = {1, 2, 3, 5, 8, 13, 21, 34, 80, 7, 200, 0, 40, 80};
  for (int w : seq) {
    p.setViewSize(w, (w % 4) + 1);
    const int h = (w % 4) + 1;
    CHECK(p.scroll() >= 0);
    CHECK(p.scroll() <= std::max(0, p.visualLineCount() - h));
    CHECK(p.visualToLogical(p.scroll()) >= 0);
    CHECK(p.visualToLogical(p.scroll()) < p.lineCount());
    if (w > 0) {
      char what[48];
      std::snprintf(what, sizeof what, "resize-seq-%d", w);
      checkWrapInvariants(p, w, what);
    }
  }
  CHECK_EQ_I(p.viewWidth(), 80);
  CHECK_EQ_I(p.viewHeight(), 1);
}

// ---------------------------------- 15. 逻辑<->视觉映射自洽
void testLogicalVisualMapping() {
  ++g_cases;
  Panel p(PanelId::Ai);
  p.setViewSize(7, 4);
  p.append("短");
  p.append("");
  p.appendLine(PanelLine{});
  p.append("这是一行需要折很多次的中文内容内容内容");
  p.append("mixed 中文 and ascii words here");
  p.append("\t制表符开头");

  const std::vector<VisualLine>& L = p.layout();
  CHECK(!L.empty());
  for (int i = 0; i < p.lineCount(); ++i) {
    const int v = p.logicalToVisual(i);
    CHECK(v >= 0 && v < static_cast<int>(L.size()));
    CHECK_EQ_I(L[static_cast<size_t>(v)].logical, i);
    CHECK_EQ_I(L[static_cast<size_t>(v)].start, 0);
    CHECK_EQ_I(p.visualToLogical(v), i);
  }
  for (int v = 0; v < static_cast<int>(L.size()); ++v) {
    const int lg = p.visualToLogical(v);
    CHECK(lg >= 0 && lg < p.lineCount());
    CHECK(p.logicalToVisual(lg) <= v);
  }
  // 越界输入被夹住
  CHECK_EQ_I(p.logicalToVisual(-99), 0);
  CHECK_EQ_I(p.logicalToVisual(9999), p.logicalToVisual(p.lineCount() - 1));
  CHECK_EQ_I(p.visualToLogical(-99), 0);
  CHECK_EQ_I(p.visualToLogical(9999), p.lineCount() - 1);
}

// ---------------------------------- 16. StdinBuffer:取全文喂子进程 stdin
void testStdinBufferData() {
  ++g_cases;
  StdinBuffer sb;
  // 空输入 -> 空串(不能凭空给子进程多喂一个 "\n")
  CHECK_EQ_S(sb.data(), "");
  CHECK_EQ_I(sb.buffer().lineCount(), 1);
  CHECK_EQ_I(sb.cursor().line, 0);
  CHECK_EQ_I(sb.cursor().col, 0);
  CHECK_EQ_I(sb.scroll(), 0);

  // 多行含中文:取出的全文与写入一致
  const std::string in = "3\n第一行 中文输入\n第二行\t带制表符\n🌟 emoji 行\n";
  sb.setData(in);
  CHECK_EQ_S(sb.data(), in);
  CHECK_EQ_I(sb.buffer().lineCount(), 4);
  CHECK_EQ_S(sb.buffer().line(1), "第一行 中文输入");

  // 末尾没有换行时补一个
  sb.setData("5 7\n8 9");
  CHECK_EQ_S(sb.data(), "5 7\n8 9\n");
  sb.setData("单行没有换行");
  CHECK_EQ_S(sb.data(), "单行没有换行\n");

  // 中间的空行要保住
  sb.setData("a\n\nb\n");
  CHECK_EQ_S(sb.data(), "a\n\nb\n");
  // 末尾的空行 = 末尾换行
  sb.setData("a\n\n");
  CHECK_EQ_S(sb.data(), "a\n");
  // 空串 / 单个换行都算“空输入”
  sb.setData("");
  CHECK_EQ_S(sb.data(), "");
  sb.setData("\n");
  CHECK_EQ_S(sb.data(), "");

  // CRLF 输入被规整成 LF(子进程按行读更符合预期)
  sb.setData("a\r\nb\r\n");
  CHECK_EQ_S(sb.data(), "a\nb\n");

  // clear()
  sb.setData("x\ny\n");
  sb.clear();
  CHECK_EQ_S(sb.data(), "");
  CHECK_EQ_I(sb.buffer().lineCount(), 1);
  CHECK_EQ_I(sb.cursor().line, 0);

  // loadFile:成功 / 失败
  const std::string path = std::string(util::tempDir()) + "/cppide-test-panel-stdin.txt";
  std::string err = "x";
  CHECK(util::writeFileAtomic(path, "10 20\n中文一行\n", err));
  CHECK(sb.loadFile(path, err));
  CHECK_EQ_S(err, "");
  CHECK_EQ_S(sb.data(), "10 20\n中文一行\n");
  CHECK_EQ_I(sb.cursor().line, 0);
  CHECK(!sb.loadFile(path + ".nope", err));
  CHECK(!err.empty());                       // 中文错误说明
  CHECK_EQ_S(sb.data(), "10 20\n中文一行\n");   // 失败不动内容
  std::remove(path.c_str());

  // 视口 / 滚动
  std::string many;
  for (int i = 0; i < 10; ++i) many += "line " + std::to_string(i) + "\n";
  sb.setData(many);
  sb.setViewSize(20, 3);
  CHECK_EQ_I(sb.scroll(), 0);
  sb.setCursor(Pos{9, 0});
  CHECK_EQ_I(sb.cursor().line, 9);
  CHECK_EQ_I(sb.scroll(), 7);                // 光标行进视口
  sb.setScroll(100);
  CHECK_EQ_I(sb.scroll(), 7);                // 夹到 lineCount - h
  sb.setScroll(-5);
  CHECK_EQ_I(sb.scroll(), 0);
  sb.setCursor(Pos{999, 999});               // 越界光标被夹住
  CHECK_EQ_I(sb.cursor().line, 9);
  CHECK_EQ_I(sb.cursor().col, sb.buffer().lineLen(9));
  sb.setViewSize(20, 0);                     // 高度 0 不崩
  CHECK_EQ_I(sb.scroll(), 0);
}

// ---------------------------------- 17. StdinBuffer:编辑与撤销
void testStdinBufferEditing() {
  ++g_cases;
  StdinBuffer sb;
  sb.setViewSize(40, 4);

  sb.insertText("你好");
  CHECK_EQ_S(sb.data(), "你好\n");
  CHECK_EQ_I(sb.cursor().col, 6);            // Pos::col 是字节偏移
  sb.insertNewline();
  CHECK_EQ_I(sb.cursor().line, 1);
  CHECK_EQ_I(sb.cursor().col, 0);
  sb.insertText("world");
  CHECK_EQ_S(sb.data(), "你好\nworld\n");
  sb.insertText("");                         // 空插入不动
  CHECK_EQ_S(sb.data(), "你好\nworld\n");
  // 一次 insertText 里带换行也要正确拆行
  sb.insertText("\nA\nB");
  CHECK_EQ_S(sb.data(), "你好\nworld\nA\nB\n");
  CHECK_EQ_I(sb.buffer().lineCount(), 4);

  // 移动:多字节字符按字符走,不落在字符中间
  sb.setCursor(Pos{0, 0});
  sb.moveEnd();
  CHECK_EQ_I(sb.cursor().col, 6);
  sb.moveLeft();
  CHECK_EQ_I(sb.cursor().col, 3);            // 跨过一个 3 字节汉字
  sb.moveLeft();
  CHECK_EQ_I(sb.cursor().col, 0);
  sb.moveLeft();                             // 缓冲区开头再左移:原地不动
  CHECK_EQ_I(sb.cursor().line, 0);
  CHECK_EQ_I(sb.cursor().col, 0);
  sb.moveRight();
  CHECK_EQ_I(sb.cursor().col, 3);
  sb.moveHome();
  CHECK_EQ_I(sb.cursor().col, 0);
  sb.moveUp();                               // 第一行上移:停在行首
  CHECK_EQ_I(sb.cursor().line, 0);
  sb.moveDown();
  CHECK_EQ_I(sb.cursor().line, 1);
  sb.moveEnd();
  CHECK_EQ_I(sb.cursor().col, 5);            // "world"
  sb.moveUp();                               // 列被夹到 UTF-8 字符边界(5 -> 3)
  CHECK_EQ_I(sb.cursor().line, 0);
  CHECK_EQ_I(sb.cursor().col, 3);
  sb.setCursor(Pos{3, 1});
  sb.moveDown();                             // 最后一行下移:停在行尾
  CHECK_EQ_I(sb.cursor().line, 3);
  CHECK_EQ_I(sb.cursor().col, 1);
  for (int i = 0; i < 30; ++i) sb.moveRight();
  CHECK_EQ_I(sb.cursor().line, 3);
  CHECK_EQ_I(sb.cursor().col, 1);
  for (int i = 0; i < 60; ++i) sb.moveLeft();
  CHECK_EQ_I(sb.cursor().line, 0);
  CHECK_EQ_I(sb.cursor().col, 0);

  // backspace:删整个多字节字符
  sb.setCursor(Pos{0, 6});
  sb.backspace();
  CHECK_EQ_S(sb.buffer().line(0), "你");
  CHECK_EQ_I(sb.cursor().col, 3);
  sb.backspace();
  CHECK_EQ_S(sb.buffer().line(0), "");
  sb.backspace();                            // 0,0 处 backspace:什么都不做
  CHECK_EQ_I(sb.buffer().lineCount(), 4);
  // 行首 backspace 合并到上一行
  sb.setCursor(Pos{1, 0});
  sb.backspace();
  CHECK_EQ_I(sb.buffer().lineCount(), 3);
  CHECK_EQ_S(sb.buffer().line(0), "world");
  CHECK_EQ_I(sb.cursor().line, 0);
  CHECK_EQ_I(sb.cursor().col, 0);

  // del:行内删一个字符 / 行尾删合并 / 末尾无事可做
  sb.setData("汉字abc\nnext\n");
  sb.setCursor(Pos{0, 0});
  sb.del();
  CHECK_EQ_S(sb.buffer().line(0), "字abc");
  sb.moveEnd();
  sb.del();                                  // 行尾:与下一行合并
  CHECK_EQ_I(sb.buffer().lineCount(), 1);
  CHECK_EQ_S(sb.buffer().line(0), "字abcnext");
  sb.moveEnd();
  sb.del();                                  // 缓冲区末尾:无事可做
  CHECK_EQ_S(sb.buffer().line(0), "字abcnext");
  CHECK_EQ_I(sb.buffer().lineCount(), 1);

  // 撤销 / 重做:全撤销必须逐字节回到初始状态
  StdinBuffer u;
  u.setViewSize(40, 4);
  CHECK(!u.undo());                           // 没历史时返回 false
  CHECK(!u.redo());
  u.insertText("第一段");
  u.insertNewline();
  u.insertText("second");
  u.insertNewline();
  u.insertText("三");
  u.backspace();
  u.insertText("四五六");
  const std::string final_text = u.data();
  CHECK_EQ_S(final_text, "第一段\nsecond\n四五六\n");

  int guard = 0;
  while (u.undo()) {
    CHECK(++guard < 100);
  }
  CHECK_EQ_S(u.data(), "");                   // 回到空
  CHECK_EQ_I(u.cursor().line, 0);
  guard = 0;
  while (u.redo()) {
    CHECK(++guard < 100);
  }
  CHECK_EQ_S(u.data(), final_text);           // 全重做回到终态
  CHECK(u.cursor().line >= 0);
  CHECK(u.cursor().line < u.buffer().lineCount());

  // buffer() 拿到的就是同一个 TextBuffer(撤销栈共享)
  StdinBuffer d;
  d.insertText("abc");
  CHECK(d.buffer().canUndo());
  CHECK(d.buffer().dirty());
  CHECK_EQ_I(d.buffer().lineCount(), 1);
  d.buffer().reset({"x", "y"});               // 直接操作底层缓冲也不能把光标弄坏
  CHECK_EQ_S(d.data(), "x\ny\n");
  d.setCursor(Pos{99, 99});
  CHECK_EQ_I(d.cursor().line, 1);

  // 中文往复:插入 -> 撤销 -> 重做,全文逐字节一致
  StdinBuffer z;
  z.setData("初始内容\n");
  z.setCursor(Pos{0, 12});
  z.insertText("追加的中文");
  CHECK_EQ_S(z.data(), "初始内容追加的中文\n");
  CHECK(z.undo());
  CHECK_EQ_S(z.data(), "初始内容\n");
  CHECK(z.redo());
  CHECK_EQ_S(z.data(), "初始内容追加的中文\n");
}

}  // namespace

int main() {
  if (!std::setlocale(LC_ALL, "")) std::setlocale(LC_ALL, "C.UTF-8");
  if (wcwidth(static_cast<wchar_t>(0x4E2D)) != 2) {
    if (!std::setlocale(LC_ALL, "C.UTF-8")) std::setlocale(LC_ALL, "C.utf8");
  }

  testTitles();
  testEmptyPanel();
  testAppendBasics();
  testWrapInvariants();
  testStreamingChunkEquivalence();
  testStreamingMidCharSafety();
  testStreamingLineSemantics();
  testRingTrimSmall();
  testLongLineSplit();
  testFlood100MB();
  testScrollEdges();
  testUnreadAndErrorCount();
  testDiagIndex();
  testResizeRewrap();
  testLogicalVisualMapping();
  testStdinBufferData();
  testStdinBufferEditing();

  std::printf("test_panel: OK (%d cases, %d checks)\n", g_cases, g_checks);
  return 0;
}

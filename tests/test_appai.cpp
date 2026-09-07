// test_appai.cpp —— Wave 5 裁决①:AiDone 的语义是**替换**,不是追加。
//
// 钉住的行为(这是"AI 内容显示两遍"这类 bug 唯一的产地):
//   * AiDelta 只累积进 App::ai_stream_,**永不落地** —— 既不进 ghost 也不进面板;
//   * AiDone.text 是完整最终文本,onAiDone 只认它一个来源;
//   * 因此:一段 Delta 序列 + Done 之后,内容在 ghost / 【AI】面板里
//     **恰好出现一次**,不重复、不拼接、不截断。
//
// 怎么在没有 tty 的环境里测 App:
//   `Ui::Ui()` 是 `= default`(ui.cpp:452),curses 只在 `Ui::init()` 里碰;
//   `Ui::~Ui()` -> `shutdown()` 在没 init 过时只做幂等的 endwinOnce()(内部
//   g_screen_live == 0,什么都不做)。所以**构造 App 但不调 App::init()** 是安全的:
//   全程不进 ncurses,可以直接调事件处理函数并检查 ed_ / panels_ 的真实状态。
//   代价:panels_ 没有 setViewSize,所以只断言逻辑行内容,不断言折行结果。
//
// 为拿到 App 的私有区,本文件在 include 项目头之前 `#define private public`
// (与 tests/test_ai.cpp 同一套做法;标准库头全部在此之前 include 完)。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
#include <unistd.h>

// ★ 只为测试打开私有区。
#define private public
#include "ai.h"
#include "app.h"
#include "config.h"
#include "editor.h"
#include "panel.h"
#include "util.h"
#undef private

static int g_checks = 0;
static int g_cases = 0;

static void failAt(const char* file, int line, const char* expr, const std::string& note) {
  std::fflush(stdout);
  std::fprintf(stderr, "\nFAIL %s:%d  CHECK(%s)  %s\n", file, line, expr, note.c_str());
  std::fflush(stderr);
  std::abort();
}

#define CHECK(cond)                                                   \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) failAt(__FILE__, __LINE__, #cond, std::string());     \
  } while (0)

#define CHECK_M(cond, note)                                           \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) failAt(__FILE__, __LINE__, #cond, std::string(note)); \
  } while (0)

static void caseBegin(const char* name) {
  ++g_cases;
  std::printf("  [%d] %s\n", g_cases, name);
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 脚手架
// ---------------------------------------------------------------------------

// api_key 恒为空:AiService 不起 worker 线程,也不可能发出任何真实请求。
// (本测试全程只手工投递 AppEvent,不碰网络。)
static Config emptyKeyConfig() {
  Config c;
  c.api_key.clear();
  c.base_url = "http://127.0.0.1:1";
  c.model = "test-model";
  c.tick_ms = 60;
  return c;
}

// 数一下 needle 在 hay 里出现几次(重叠不计)。
static int countOccurrences(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return 0;
  int n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos;
       p = hay.find(needle, p + needle.size())) {
    ++n;
  }
  return n;
}

// 把一个面板的所有逻辑行拼成一整块文本。
static std::string panelText(const Panel& p) {
  std::string s;
  for (const PanelLine& ln : p.lines()) {
    s += ln.text;
    s.push_back('\n');
  }
  return s;
}

// 手工造一条事件。
static AppEvent mkEvent(EvKind kind, uint64_t gen, AiSink sink, std::string text) {
  AppEvent ev;
  ev.kind = kind;
  ev.gen = gen;
  ev.sink = sink;
  ev.text = std::move(text);
  return ev;
}

// 去掉每行的 `//` 注释:所有源码断言只看真正的代码,不被注释里的词误伤。
// (本工程注释里大量出现 appendStreaming / ai_stream_ 这类词来解释"为什么不这么做"。)
static std::string stripLineComments(const std::string& s) {
  std::string out;
  for (const std::string& line : util::splitLines(s)) {
    const size_t p = line.find("//");
    out += (p == std::string::npos) ? line : line.substr(0, p);
    out.push_back('\n');
  }
  return out;
}

// 把 full 切成 n 段(模拟 SSE delta 的任意切点,含把 UTF-8 字符切两半的情形)。
static std::vector<std::string> chopInto(const std::string& full, size_t n) {
  std::vector<std::string> out;
  if (n == 0) return out;
  const size_t step = full.size() / n ? full.size() / n : 1;
  for (size_t i = 0; i < full.size(); i += step) out.push_back(full.substr(i, step));
  if (out.empty()) out.push_back(std::string());
  return out;
}

// ---------------------------------------------------------------------------
// 1. 写代码模式(GhostText):Delta 序列 + Done -> ghost 内容恰好一份
// ---------------------------------------------------------------------------
static void case_ghost_done_replaces() {
  caseBegin("AiDone(GhostText)= 替换:ghost 内容只出现一次");

  const std::string kFinal = "  for (int i = 0; i < n; ++i) sum += a[i];";

  for (size_t nchunks : {1u, 2u, 3u, 7u, 40u}) {
    App app(emptyKeyConfig(), std::string());
    const uint64_t gen = app.ai_.generation();

    app.onAiStarted(mkEvent(EvKind::AiStarted, gen, AiSink::GhostText, std::string()));
    CHECK(app.ai_stream_.empty());

    // 流式:每一段都投一次 AiDelta。
    const std::vector<std::string> chunks = chopInto(kFinal, nchunks);
    std::string rebuilt;
    for (const std::string& c : chunks) {
      app.onAiDelta(mkEvent(EvKind::AiDelta, gen, AiSink::GhostText, c));
      rebuilt += c;
    }
    CHECK_M(rebuilt == kFinal, "chopInto 自身不该丢字节");
    CHECK_M(app.ai_stream_ == kFinal, "delta 应当被完整累积");

    // ★ 关键断言 1:AiDone **之前**,ghost 里一个字都没有 —— delta 不落地。
    CHECK_M(app.ed_.ghost().text.empty(), "AiDelta 竟然把文本落进了 ghost");
    CHECK_M(!app.ed_.ghost().complete, "AiDone 之前 ghost 不能是 complete");
    // 同一轮里【AI】面板也不该被 delta 污染。
    CHECK_M(panelText(app.panel(PanelId::Ai)).find(kFinal) == std::string::npos,
            "AiDelta 竟然把文本落进了【AI】面板");

    // 收尾:Done 带完整最终文本。
    app.onAiDone(mkEvent(EvKind::AiDone, gen, AiSink::GhostText, kFinal));

    // ★ 关键断言 2:ghost 恰好等于最终文本(不是 2 份,也不是 1.5 份)。
    CHECK_M(app.ed_.ghost().text == kFinal, "ghost 内容与最终文本不相等(重复或截断)");
    CHECK_M(countOccurrences(app.ed_.ghost().text, kFinal) == 1,
            "最终文本在 ghost 里出现了不止一次");
    CHECK(app.ed_.ghost().complete);
    CHECK(app.ed_.ghost().sink == AiSink::GhostText);
    CHECK(app.ed_.ghost().gen == gen);
    // ai_stream_ 收尾即清空,不会漏到下一轮。
    CHECK(app.ai_stream_.empty());

    // ★ 关键断言 3:GhostText 一轮里【AI】面板不追加这段代码(§6)。
    CHECK_M(countOccurrences(panelText(app.panel(PanelId::Ai)), kFinal) == 0,
            "写代码模式的补全竟然也进了【AI】面板");
  }
}

// ---------------------------------------------------------------------------
// 2. 练习模式(PanelOnly):Delta 序列 + Done -> 面板里内容恰好一份
// ---------------------------------------------------------------------------
static void case_panel_done_replaces() {
  caseBegin("AiDone(PanelOnly)= 替换:【AI】面板内容只出现一次");

  // 含中文与换行:既检查"不重复",也顺手检查多行拆分没把内容吃掉。
  const std::string kFinal =
      "思路:先排序再双指针。\n"
      "复杂度:O(n log n)。\n"
      "卡点:注意 n == 0 的边界。\n\n"
      "【推理过程】\n"
      "枚举右端点时左端点单调不减,所以可以摊还成 O(n)。";

  for (size_t nchunks : {1u, 2u, 5u, 13u, 100u}) {
    App app(emptyKeyConfig(), std::string());
    const uint64_t gen = app.ai_.generation();
    Panel& ai_panel = app.panel(PanelId::Ai);
    const int lines_before = ai_panel.lineCount();

    app.onAiStarted(mkEvent(EvKind::AiStarted, gen, AiSink::PanelOnly, std::string()));
    for (const std::string& c : chopInto(kFinal, nchunks)) {
      app.onAiDelta(mkEvent(EvKind::AiDelta, gen, AiSink::PanelOnly, c));
    }
    CHECK(app.ai_stream_ == kFinal);

    // ★ Done 之前面板一行都没多(delta 不落地)。
    CHECK_M(ai_panel.lineCount() == lines_before,
            "AiDelta 往【AI】面板写了内容,收尾时会显示两遍");

    app.onAiDone(mkEvent(EvKind::AiDone, gen, AiSink::PanelOnly, kFinal));

    const std::string txt = panelText(ai_panel);
    // ★ 每一行都恰好出现一次。
    for (const std::string& line : util::splitLines(kFinal)) {
      if (util::trim(line).empty()) continue;
      const int n = countOccurrences(txt, line);
      CHECK_M(n == 1, "【AI】面板里 \"" + line + "\" 出现了 " + std::to_string(n) + " 次");
    }
    // 整段也只出现一次(把换行也算进去)。
    CHECK_M(countOccurrences(txt, kFinal) == 1, "整段回复在面板里不是恰好一份");
    // ★ 练习模式绝不碰缓冲区、绝不产生 ghost(§6 的核心保证)。
    CHECK_M(app.ed_.ghost().text.empty(), "练习模式竟然产生了 ghost");
    CHECK_M(!app.ed_.buffer().hasNonBlank(), "练习模式竟然改了缓冲区");
    CHECK(app.ai_stream_.empty());
  }
}

// ---------------------------------------------------------------------------
// 3. Done.text 比 delta 累积**短**时也必须以 Done 为准(替换语义的分水岭)
//    旧实现取"较长者",于是写代码模式的后处理(去掉重复前缀 / 截到
//    ghost_max_lines)会被 delta 原文顶掉,ghost 里出现未后处理的重复代码。
// ---------------------------------------------------------------------------
static void case_done_shorter_than_stream_wins() {
  caseBegin("Done.text 比 delta 累积短时:仍然以 Done 为准(不取较长者)");

  // 模型把光标所在行的前缀重复了一遍,ai.cpp 的 postprocessCompletion 已去重;
  // delta 原文(长)与最终文本(短)不一致,UI 必须显示后者。
  const std::string kRawStream = "  sum += a[i];  sum += a[i];  sum += a[i];";
  const std::string kFinal = "  sum += a[i];";
  CHECK(kFinal.size() < kRawStream.size());

  {
    App app(emptyKeyConfig(), std::string());
    const uint64_t gen = app.ai_.generation();
    app.onAiStarted(mkEvent(EvKind::AiStarted, gen, AiSink::GhostText, std::string()));
    for (const std::string& c : chopInto(kRawStream, 6)) {
      app.onAiDelta(mkEvent(EvKind::AiDelta, gen, AiSink::GhostText, c));
    }
    app.onAiDone(mkEvent(EvKind::AiDone, gen, AiSink::GhostText, kFinal));

    CHECK_M(app.ed_.ghost().text == kFinal,
            "ghost 取了 delta 累积而不是 Done.text(替换语义被破坏)");
    CHECK_M(countOccurrences(app.ed_.ghost().text, "sum += a[i];") == 1,
            "去重后的补全里 \"sum += a[i];\" 竟然出现了多次");
  }
  {
    App app(emptyKeyConfig(), std::string());
    const uint64_t gen = app.ai_.generation();
    app.onAiStarted(mkEvent(EvKind::AiStarted, gen, AiSink::PanelOnly, std::string()));
    for (const std::string& c : chopInto(kRawStream, 6)) {
      app.onAiDelta(mkEvent(EvKind::AiDelta, gen, AiSink::PanelOnly, c));
    }
    app.onAiDone(mkEvent(EvKind::AiDone, gen, AiSink::PanelOnly, kFinal));
    const std::string txt = panelText(app.panel(PanelId::Ai));
    CHECK_M(countOccurrences(txt, "sum += a[i];") == 1,
            "面板里出现了 delta 原文(应当只有 Done 的后处理结果)");
  }
}

// ---------------------------------------------------------------------------
// 4. 连续两轮:第二轮不会带上第一轮的残余(ai_stream_ 已清空)
// ---------------------------------------------------------------------------
static void case_two_rounds_no_leak() {
  caseBegin("连续两轮回复互不污染(ai_stream_ 收尾即清空)");

  App app(emptyKeyConfig(), std::string());
  const std::string a = "第一轮:用前缀和。";
  const std::string b = "第二轮:改用差分数组。";

  const uint64_t gen = app.ai_.generation();
  app.onAiStarted(mkEvent(EvKind::AiStarted, gen, AiSink::PanelOnly, std::string()));
  app.onAiDelta(mkEvent(EvKind::AiDelta, gen, AiSink::PanelOnly, a));
  app.onAiDone(mkEvent(EvKind::AiDone, gen, AiSink::PanelOnly, a));

  app.onAiStarted(mkEvent(EvKind::AiStarted, gen, AiSink::PanelOnly, std::string()));
  app.onAiDelta(mkEvent(EvKind::AiDelta, gen, AiSink::PanelOnly, b));
  app.onAiDone(mkEvent(EvKind::AiDone, gen, AiSink::PanelOnly, b));

  const std::string txt = panelText(app.panel(PanelId::Ai));
  CHECK_M(countOccurrences(txt, a) == 1, "第一轮内容出现了不止一次");
  CHECK_M(countOccurrences(txt, b) == 1, "第二轮内容出现了不止一次");
  // 顺序也要对。
  CHECK(txt.find(a) < txt.find(b));

  // 两轮 ghost:第二轮把第一轮整个换掉,不是拼接。
  App g(emptyKeyConfig(), std::string());
  const uint64_t g2 = g.ai_.generation();
  g.onAiStarted(mkEvent(EvKind::AiStarted, g2, AiSink::GhostText, std::string()));
  g.onAiDelta(mkEvent(EvKind::AiDelta, g2, AiSink::GhostText, "int x = 1;"));
  g.onAiDone(mkEvent(EvKind::AiDone, g2, AiSink::GhostText, "int x = 1;"));
  CHECK(g.ed_.ghost().text == "int x = 1;");
  g.onAiStarted(mkEvent(EvKind::AiStarted, g2, AiSink::GhostText, std::string()));
  g.onAiDelta(mkEvent(EvKind::AiDelta, g2, AiSink::GhostText, "long y = 2;"));
  g.onAiDone(mkEvent(EvKind::AiDone, g2, AiSink::GhostText, "long y = 2;"));
  CHECK_M(g.ed_.ghost().text == "long y = 2;", "第二轮 ghost 与第一轮拼接了");
  CHECK(g.ed_.ghost().text.find("int x = 1;") == std::string::npos);
}

// ---------------------------------------------------------------------------
// 5. 源码级断言:onAiDelta 不许调任何落地函数;onAiDone 不许再取 ai_stream_
//    (行为断言会被"改回兜底"绕过 —— 这条 grep 断言把它钉死。)
// ---------------------------------------------------------------------------
static void case_source_replace_semantics() {
  caseBegin("源码断言:AiDone 只认 ev.text,delta 不落地");

  const char* cands[] = {nullptr, "src/app.cpp", "../src/app.cpp", "/work/src/app.cpp"};
  cands[0] = std::getenv("CPPIDE_APP_CPP");
  std::string src, used;
  for (const char* p : cands) {
    if (p == nullptr) continue;
    std::ifstream f(p, std::ios::binary);
    if (!f) continue;
    std::ostringstream os;
    os << f.rdbuf();
    src = os.str();
    used = p;
    break;
  }
  CHECK_M(!src.empty(), "找不到 src/app.cpp;设 CPPIDE_APP_CPP=<路径>");
  std::printf("      (读取 %s,%zu 字节)\n", used.c_str(), src.size());

  // 切出 onAiDelta 的函数体。
  const size_t d0 = src.find("void App::onAiDelta(");
  CHECK(d0 != std::string::npos);
  const size_t d1 = src.find("\n}", d0);
  CHECK(d1 != std::string::npos);
  const std::string delta_body = stripLineComments(src.substr(d0, d1 - d0));
  // delta 里唯一允许出现的写操作是 `ai_stream_ +=`。
  CHECK_M(delta_body.find("setGhost") == std::string::npos,
          "onAiDelta 调了 setGhost(delta 落地 = 内容会显示两遍)");
  CHECK_M(delta_body.find("appendGhostDelta") == std::string::npos,
          "onAiDelta 调了 appendGhostDelta");
  CHECK_M(delta_body.find("appendStreaming") == std::string::npos,
          "onAiDelta 往面板 appendStreaming(收尾再 append 全文就会重复)");
  CHECK_M(delta_body.find("appendAi") == std::string::npos, "onAiDelta 调了 appendAi");
  CHECK(delta_body.find("ai_stream_ +=") != std::string::npos);

  // 切出 onAiDone 的函数体。
  const size_t n0 = src.find("void App::onAiDone(");
  CHECK(n0 != std::string::npos);
  const size_t n1 = src.find("\n}", n0);
  CHECK(n1 != std::string::npos);
  const std::string done_body = stripLineComments(src.substr(n0, n1 - n0));
  // ★ Done 里 ai_stream_ 只允许被 clear(),不允许参与"取哪个文本"的决策。
  CHECK_M(done_body.find("ai_stream_.size()") == std::string::npos,
          "onAiDone 又开始比 ai_stream_ 的长度了(替换语义被改回兜底)");
  CHECK_M(done_body.find("+= ai_stream_") == std::string::npos,
          "onAiDone 把 ai_stream_ 拼进了最终文本");
  CHECK_M(done_body.find(": ai_stream_") == std::string::npos,
          "onAiDone 用三目运算在 ev.text / ai_stream_ 之间挑(应当只认 ev.text)");
  CHECK(done_body.find("const std::string& full = ev.text;") != std::string::npos);
  // ai_stream_ 在 Done 里只出现在 clear() 上。
  {
    int total = countOccurrences(done_body, "ai_stream_");
    int cleared = countOccurrences(done_body, "ai_stream_.clear()");
    CHECK_M(total == cleared, "onAiDone 里 ai_stream_ 出现在了 clear() 之外的地方");
  }
}

int main() {
  // 看门狗:整份测试不碰网络也不碰 tty,5 秒都用不到。
  ::alarm(60);
  std::printf("test_appai:\n");
  case_ghost_done_replaces();
  case_panel_done_replaces();
  case_done_shorter_than_stream_wins();
  case_two_rounds_no_leak();
  case_source_replace_semantics();
  std::printf("test_appai: 全部通过(%d 个用例 / %d 个断言)\n", g_cases, g_checks);
  return 0;
}

// test_sse.cpp —— SseParser 分片鲁棒性(纯 assert,无框架)
//
// 核心不变式:**同一段字节流,不管被怎么切,产出的 payload / delta 序列必须完全相同**。
// 三种投喂方式:一次性、每 1 字节、随机切分(固定种子,可复现)。
// 真实的 chunked SSE 会在任意字节处切断 —— 包括 UTF-8 多字节中间、
// "data:" 关键字中间、"\r\n" 的两个字节之间。
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "../src/aihttp.h"

static int g_checks = 0;
static int g_cases = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    ++g_checks;                                                          \
    if (!(cond)) {                                                       \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      assert(false && #cond);                                            \
      return 1;                                                          \
    }                                                                    \
  } while (0)

// ---------------------------------------------------------------- 投喂器

using Chunks = std::vector<std::string>;

static Chunks splitOneShot(const std::string& s) { return Chunks{s}; }

static Chunks splitPerByte(const std::string& s) {
  Chunks out;
  out.reserve(s.size());
  for (char c : s) out.push_back(std::string(1, c));
  return out;
}

static Chunks splitRandom(const std::string& s, uint32_t seed) {
  std::mt19937 rng(seed);
  Chunks out;
  size_t i = 0;
  while (i < s.size()) {
    std::uniform_int_distribution<size_t> d(1, 7);
    size_t n = std::min(d(rng), s.size() - i);
    out.push_back(s.substr(i, n));
    i += n;
  }
  return out;
}

// 收集 SseParser 吐出的原始 payload 序列。
static std::vector<std::string> rawPayloads(const Chunks& chunks) {
  SseParser p;
  std::vector<std::string> out;
  auto cb = [&](const std::string& s) { out.push_back(s); };
  for (const std::string& c : chunks) p.feed(c.data(), c.size(), cb);
  p.finish(cb);
  return out;
}

// 收集"语义事件"序列:C:<content> / R:<reasoning> / DONE / BAD
static std::vector<std::string> events(const Chunks& chunks) {
  SseParser p;
  std::vector<std::string> out;
  auto cb = [&](const std::string& payload) {
    std::string c, r;
    bool done = false;
    if (!sseChunkToDelta(payload, c, r, done)) {
      out.push_back("BAD");
      return;
    }
    if (done) { out.push_back("DONE"); return; }
    if (!c.empty()) out.push_back("C:" + c);
    if (!r.empty()) out.push_back("R:" + r);
  };
  for (const std::string& ch : chunks) p.feed(ch.data(), ch.size(), cb);
  p.finish(cb);
  return out;
}

static void dumpVec(const char* tag, const std::vector<std::string>& v) {
  std::fprintf(stderr, "  %s (%zu):", tag, v.size());
  for (const std::string& s : v) std::fprintf(stderr, " [%s]", s.c_str());
  std::fprintf(stderr, "\n");
}

// 三种切分方式必须给出完全相同的结果。
static bool sameUnderAllSplits(const std::string& stream, const char* name) {
  ++g_cases;
  const std::vector<std::string> a = rawPayloads(splitOneShot(stream));
  const std::vector<std::string> b = rawPayloads(splitPerByte(stream));
  const std::vector<std::string> c = rawPayloads(splitRandom(stream, 0xC0FFEEu));
  const std::vector<std::string> d = rawPayloads(splitRandom(stream, 12345u));
  bool ok = (a == b) && (a == c) && (a == d);
  const std::vector<std::string> ea = events(splitOneShot(stream));
  const std::vector<std::string> eb = events(splitPerByte(stream));
  const std::vector<std::string> ec = events(splitRandom(stream, 0xC0FFEEu));
  ok = ok && (ea == eb) && (ea == ec);
  if (!ok) {
    std::fprintf(stderr, "FAIL split-invariance: %s\n", name);
    dumpVec("oneshot", a);
    dumpVec("perbyte", b);
    dumpVec("rand", c);
    dumpVec("ev-oneshot", ea);
    dumpVec("ev-perbyte", eb);
  }
  ++g_checks;
  return ok;
}

static std::string frame(const std::string& content) {
  return "data: {\"choices\":[{\"delta\":{\"content\":\"" + content +
         "\"}}]}\n\n";
}

int main() {
  // ---- 1. 最小可用流:一帧 + [DONE] ----
  {
    const std::string s = frame("hi") + "data: [DONE]\n\n";
    CHECK(sameUnderAllSplits(s, "basic"));
    const std::vector<std::string> e = events(splitPerByte(s));
    CHECK(e.size() == 2);
    CHECK(e[0] == "C:hi");
    CHECK(e[1] == "DONE");
  }

  // ---- 2. 多帧拼接成完整文本 ----
  {
    std::string s;
    const char* parts[] = {"Hello", ", ", "world", "!"};
    for (const char* p : parts) s += frame(p);
    s += "data: [DONE]\n\n";
    CHECK(sameUnderAllSplits(s, "multi"));
    std::string joined;
    for (const std::string& ev : events(splitRandom(s, 7u)))
      if (ev.rfind("C:", 0) == 0) joined += ev.substr(2);
    CHECK(joined == "Hello, world!");
  }

  // ---- 3. \r\n 行尾(SSE 规范允许) ----
  {
    const std::string s =
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\"}}]}\r\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"b\"}}]}\r\n\r\n"
        "data: [DONE]\r\n\r\n";
    CHECK(sameUnderAllSplits(s, "crlf"));
    const std::vector<std::string> e = events(splitPerByte(s));
    CHECK(e.size() == 3);
    CHECK(e[0] == "C:a" && e[1] == "C:b" && e[2] == "DONE");
  }

  // ---- 3b. 混用 \n\r\n 与 \r\n\n 这类畸形终止符 ----
  {
    const std::string s =
        "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\r\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"y\"}}]}\r\n\n";
    CHECK(sameUnderAllSplits(s, "mixed-eol"));
    const std::vector<std::string> e = events(splitPerByte(s));
    CHECK(e.size() == 2 && e[0] == "C:x" && e[1] == "C:y");
  }

  // ---- 4. event: / id: / retry: / 注释行 / 无 data: 前缀的垃圾 ----
  {
    const std::string s =
        ": this is a comment ping\n\n"
        "event: message\n"
        "id: 42\n"
        "retry: 3000\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
        "garbage line without colon\n\n"
        ":\n\n"
        "event: done\n\n"
        "data: [DONE]\n\n";
    CHECK(sameUnderAllSplits(s, "meta-fields"));
    const std::vector<std::string> e = events(splitPerByte(s));
    CHECK(e.size() == 2);
    CHECK(e[0] == "C:ok");
    CHECK(e[1] == "DONE");
  }

  // ---- 5. UTF-8 中文 + emoji,必然被 1 字节切分切在多字节中间 ----
  {
    const std::string s = frame("你好") + frame("世界") +
                          frame("🚀") + frame("汉字🌟混排") +
                          "data: [DONE]\n\n";
    CHECK(sameUnderAllSplits(s, "utf8"));
    std::string joined;
    for (const std::string& ev : events(splitPerByte(s)))
      if (ev.rfind("C:", 0) == 0) joined += ev.substr(2);
    CHECK(joined == "你好世界🚀汉字🌟混排");
    // 逐字节切分下,拼出来的字节必须与源串完全一致(没有半个字符被丢)。
    CHECK(events(splitPerByte(s)) == events(splitOneShot(s)));
  }

  // ---- 6. "data:" 关键字本身被切断 ----
  {
    const std::string s = frame("A") + frame("B");
    // 手工在 "da|ta:" 处切
    Chunks c;
    c.push_back(s.substr(0, 2));
    c.push_back(s.substr(2, 3));
    c.push_back(s.substr(5));
    CHECK(rawPayloads(c) == rawPayloads(splitOneShot(s)));
    CHECK(events(c).size() == 2);
    ++g_cases;
  }

  // ---- 7. 空行、连续空行、流首尾的空行 ----
  {
    const std::string s = "\n\n\r\n\n" + frame("z") + "\n\n\n\n" +
                          "data: [DONE]\n\n\n";
    CHECK(sameUnderAllSplits(s, "blank-lines"));
    const std::vector<std::string> e = events(splitPerByte(s));
    CHECK(e.size() == 2 && e[0] == "C:z" && e[1] == "DONE");
  }

  // ---- 8. 超长单帧(1MiB content) ----
  {
    const std::string big(1024 * 1024, 'x');
    const std::string s = frame(big) + "data: [DONE]\n\n";
    ++g_cases;
    // 逐字节切 1MiB 太慢,这里用随机切分 + 一次性对比。
    const std::vector<std::string> a = rawPayloads(splitOneShot(s));
    const std::vector<std::string> b = rawPayloads(splitRandom(s, 999u));
    CHECK(a == b);
    const std::vector<std::string> e = events(splitRandom(s, 999u));
    CHECK(e.size() == 2);
    CHECK(e[0].size() == 2 + big.size());
    CHECK(e[0] == "C:" + big);
  }

  // ---- 9. 非法 JSON 帧:跳过不崩,且不影响后续帧 ----
  {
    const std::string s =
        "data: {not json at all\n\n"
        "data: \n\n"
        "data: [\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"good\"}}]}\n\n"
        "data: {\"choices\":[]}\n\n"
        "data: {\"choices\":[{\"delta\":{}}]}\n\n"
        "data: null\n\n"
        "data: 12345\n\n"
        "data: [DONE]\n\n";
    CHECK(sameUnderAllSplits(s, "bad-json"));
    const std::vector<std::string> e = events(splitPerByte(s));
    // "good" 一定出现,DONE 一定出现,坏帧只产生 BAD,不产生内容。
    bool saw_good = false, saw_done = false;
    for (const std::string& ev : e) {
      if (ev == "C:good") saw_good = true;
      if (ev == "DONE") saw_done = true;
      CHECK(ev == "BAD" || ev == "C:good" || ev == "DONE");
    }
    CHECK(saw_good);
    CHECK(saw_done);
  }

  // ---- 10. reasoning_content 单独走 R: 通道 ----
  {
    const std::string s =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"想一想\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"答案\"}}]}\n\n"
        "data: [DONE]\n\n";
    CHECK(sameUnderAllSplits(s, "reasoning"));
    const std::vector<std::string> e = events(splitPerByte(s));
    CHECK(e.size() == 3);
    CHECK(e[0] == "R:想一想");
    CHECK(e[1] == "C:答案");
    CHECK(e[2] == "DONE");
  }

  // ---- 11. 多条 data: 行属于同一帧(SSE 规范:用 \n 拼接) ----
  {
    const std::string s = "data: line1\ndata: line2\n\n";
    CHECK(sameUnderAllSplits(s, "multi-data-lines"));
    const std::vector<std::string> p = rawPayloads(splitPerByte(s));
    CHECK(p.size() == 1);
    CHECK(p[0] == "line1\nline2");
  }

  // ---- 12. "data:" 后没有空格 / 有多个空格 ----
  {
    const std::string s = "data:{\"choices\":[{\"delta\":{\"content\":\"n\"}}]}\n\n"
                          "data:  two-spaces\n\n";
    CHECK(sameUnderAllSplits(s, "spacing"));
    const std::vector<std::string> p = rawPayloads(splitOneShot(s));
    CHECK(p.size() == 2);
    CHECK(p[0] == "{\"choices\":[{\"delta\":{\"content\":\"n\"}}]}");
    CHECK(p[1] == " two-spaces");  // 规范:只去掉一个空格
  }

  // ---- 13. finish():最后一帧没有以空行结尾(服务端直接断连) ----
  {
    const std::string s = frame("done-part") +
                          "data: {\"choices\":[{\"delta\":{\"content\":\"tail\"}}]}";
    CHECK(sameUnderAllSplits(s, "no-trailing-blank"));
    const std::vector<std::string> e = events(splitPerByte(s));
    CHECK(e.size() == 2);
    CHECK(e[0] == "C:done-part");
    CHECK(e[1] == "C:tail");
  }

  // ---- 14. 半帧永不早发:只喂前半段,不应产出任何 payload ----
  {
    ++g_cases;
    SseParser p;
    int n = 0;
    const std::string half = "data: {\"choices\":[{\"delta\":{\"content\":\"x";
    p.feed(half.data(), half.size(), [&](const std::string&) { ++n; });
    CHECK(n == 0);
    const std::string rest = "\"}}]}\n\n";
    p.feed(rest.data(), rest.size(), [&](const std::string&) { ++n; });
    CHECK(n == 1);
  }

  // ---- 15. reset() 清掉半帧 ----
  {
    ++g_cases;
    SseParser p;
    int n = 0;
    const std::string half = "data: abc";
    p.feed(half.data(), half.size(), [&](const std::string&) { ++n; });
    p.reset();
    p.finish([&](const std::string&) { ++n; });
    CHECK(n == 0);
  }

  // ---- 16. 空回调 / 零长度 feed 不崩 ----
  {
    ++g_cases;
    SseParser p;
    p.feed(nullptr, 0, [](const std::string&) {});
    p.feed("", 0, [](const std::string&) {});
    const std::string s = frame("q");
    p.feed(s.data(), s.size(), nullptr);  // cb 为空:必须不崩(内部判空)
    p.reset();
    p.finish(nullptr);
    CHECK(true);
  }

  // ---- 17. 真实 DeepSeek 风格流(带 role 首帧 + finish_reason 末帧 + usage) ----
  {
    const std::string s =
        "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"role\":"
        "\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"int \"},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"main() {}\"},\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":9,"
        "\"completion_tokens\":5}}\n\n"
        "data: [DONE]\n\n";
    CHECK(sameUnderAllSplits(s, "deepseek-shape"));
    std::string joined;
    for (const std::string& ev : events(splitPerByte(s)))
      if (ev.rfind("C:", 0) == 0) joined += ev.substr(2);
    CHECK(joined == "int main() {}");
    // role-only 首帧与 finish_reason 末帧是合法帧,不该被算成 BAD。
    for (const std::string& ev : events(splitOneShot(s))) CHECK(ev != "BAD");
  }

  // ---- 18. 内容里带转义:引号、换行、反斜杠、\uXXXX ----
  {
    const std::string s =
        "data: {\"choices\":[{\"delta\":{\"content\":\"a\\\"b\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"l1\\nl2\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"c:\\\\path\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\u4f60\\u597d\"}}]}\n\n";
    CHECK(sameUnderAllSplits(s, "escapes"));
    const std::vector<std::string> e = events(splitPerByte(s));
    CHECK(e.size() == 4);
    CHECK(e[0] == "C:a\"b");
    CHECK(e[1] == "C:l1\nl2");
    CHECK(e[2] == "C:c:\\path");
    CHECK(e[3] == "C:你好");
  }

  // ---- 19. 帧内 payload 本身含 "\n\n"(经 JSON 转义后是字面 \n,不会误切帧) ----
  {
    const std::string s =
        "data: {\"choices\":[{\"delta\":{\"content\":\"x\\n\\ny\"}}]}\n\n";
    CHECK(sameUnderAllSplits(s, "escaped-blank-in-content"));
    const std::vector<std::string> e = events(splitPerByte(s));
    CHECK(e.size() == 1);
    CHECK(e[0] == "C:x\n\ny");
  }

  // ---- 20. 二进制垃圾 / 无效 UTF-8 字节:不崩 ----
  {
    std::string s = "data: \xff\xfe\x80 bad bytes\n\n";
    s += frame("after");
    CHECK(sameUnderAllSplits(s, "invalid-utf8"));
    bool saw_after = false;
    for (const std::string& ev : events(splitPerByte(s)))
      if (ev == "C:after") saw_after = true;
    CHECK(saw_after);
  }

  // ---- 21. 大量小帧(1000 帧)在随机切分下顺序与内容都不乱 ----
  {
    std::string s;
    for (int i = 0; i < 1000; ++i) s += frame(std::to_string(i % 10));
    s += "data: [DONE]\n\n";
    ++g_cases;
    const std::vector<std::string> a = events(splitOneShot(s));
    const std::vector<std::string> b = events(splitRandom(s, 4242u));
    CHECK(a == b);
    CHECK(a.size() == 1001);
  }

  std::fprintf(stderr, "test_sse: OK  cases=%d checks=%d\n", g_cases, g_checks);
  std::printf("test_sse: all passed (%d cases, %d checks)\n", g_cases, g_checks);
  return 0;
}

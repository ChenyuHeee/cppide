// test_aihttp.cpp —— ChatRequest::toJson / 错误映射 / 真实网络错误路径 / 并发
//
// 三条重点(需求.md 硬性验收):
//   1. **代码里不出现硬编码 key 或写死的模型名**:toJson 的 model 必须来自入参;
//      model 为空时 send() 报错而不是偷偷替换默认值。
//   2. **api_key 绝不出现在任何返回的字符串里**:用一个独特的假 key 走遍所有
//      错误路径,断言 error/text/reasoning 里都搜不到它;toJson() 里也不能有。
//   3. **网络失败/超时/API 报错时不崩、不卡死**:真实跑 DNS 失败、连接被拒、
//      1ms 超时、立即取消、真实 401,并断言 send() 在有限时间内返回。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "../src/aihttp.h"
#include "../src/json.h"

static int g_checks = 0;
static int g_cases = 0;
static int g_skipped = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      assert(false && #cond);                                              \
      std::_Exit(1);                                                       \
    }                                                                      \
  } while (0)

#define CASE(name)                                        \
  ++g_cases;                                              \
  std::fprintf(stderr, "-- case %d: %s\n", g_cases, name)

// 一个**独特的假 key**:绝不是任何真实 key,只用来做泄漏断言。
static const char* kFakeKey = "FAKEKEY_DO_NOT_USE_abc123";
// 模型名也用假的,证明它来自入参而不是代码里写死的。
static const char* kFakeModel = "test-model-XYZ-9";

static bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// 一次响应出门后必须过的安检:任何字段都不能含 key。
static void assertNoKeyLeak(const ChatResponse& r, const char* where) {
  ++g_checks;
  if (contains(r.error, kFakeKey) || contains(r.text, kFakeKey) ||
      contains(r.reasoning, kFakeKey)) {
    std::fprintf(stderr, "FAIL key leak at %s: error=[%s] text=[%s]\n", where,
                 r.error.c_str(), r.text.c_str());
    assert(false && "api_key leaked");
    std::_Exit(1);
  }
}

static ChatRequest baseReq(const std::string& url, bool stream) {
  ChatRequest q;
  q.url = url;
  q.api_key = kFakeKey;
  q.model = kFakeModel;
  q.messages.push_back({"system", "你是 C++ 助手"});
  q.messages.push_back({"user", "写个 hello"});
  q.stream = stream;
  q.connect_timeout_ms = 2000;
  q.total_timeout_ms = 6000;
  return q;
}

static ChatDeltaFn nullDelta() {
  return [](const std::string&) { return true; };
}
static ChatCancelFn neverCancel() {
  return []() { return false; };
}

// ------------------------------------------------------------ 迷你 HTTP 服务器
//
// 只服务固定次数的连接,把预置的原始响应字节写回去(可分片 + 延迟),
// 用来端到端验 send():SSE 流式、非流式、4xx JSON 错误体、传输中取消。

struct MiniServer {
  int fd = -1;
  int port = 0;
  std::thread th;
  std::atomic<bool> stop_flag{false};
  std::string response;       // 原始响应字节
  size_t chunk = 0;           // 0 = 一次写完;否则按 chunk 字节分片
  int delay_ms = 0;           // 每片之间的延迟
  int conns = 1;              // 服务几个连接
  std::mutex mu;
  std::string last_request;   // 收到的请求原文(含 header)

  bool start() {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;  // 让内核挑端口
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
    if (::listen(fd, 8) != 0) return false;
    sockaddr_in got{};
    socklen_t gl = sizeof(got);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &gl) != 0) return false;
    port = ntohs(got.sin_port);
    th = std::thread([this]() { loop(); });
    return true;
  }

  void loop() {
    for (int i = 0; i < conns && !stop_flag.load(); ++i) {
      pollfd pf{fd, POLLIN, 0};
      int pr = ::poll(&pf, 1, 10000);  // 10s 兜底:绝不无限阻塞
      if (pr <= 0) return;
      int c = ::accept(fd, nullptr, nullptr);
      if (c < 0) return;
      serve(c);
      ::close(c);
    }
  }

  void serve(int c) {
    // 读请求(header + body)。只等 2s,读到 header 结束就够了。
    std::string req;
    for (int i = 0; i < 40; ++i) {
      pollfd pf{c, POLLIN, 0};
      if (::poll(&pf, 1, 50) <= 0) {
        if (req.find("\r\n\r\n") != std::string::npos) break;
        continue;
      }
      char buf[4096];
      ssize_t n = ::recv(c, buf, sizeof(buf), 0);
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
      if (req.find("\r\n\r\n") != std::string::npos && i > 0) break;
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      last_request = req;
    }
    // 写响应
    const size_t step = chunk == 0 ? response.size() : chunk;
    size_t off = 0;
    while (off < response.size() && !stop_flag.load()) {
      size_t n = std::min(step, response.size() - off);
      ssize_t w = ::send(c, response.data() + off, n, 0);
      if (w <= 0) return;  // 客户端已断开(取消场景),直接收工
      off += static_cast<size_t>(w);
      if (delay_ms > 0 && off < response.size())
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
  }

  void stop() {
    stop_flag.store(true);
    if (th.joinable()) th.join();
    if (fd >= 0) { ::close(fd); fd = -1; }
  }

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  }
  std::string request() {
    std::lock_guard<std::mutex> lk(mu);
    return last_request;
  }
};

static std::string httpResp(const char* status, const char* ctype,
                            const std::string& body) {
  std::string r = std::string("HTTP/1.1 ") + status + "\r\n";
  r += std::string("Content-Type: ") + ctype + "\r\n";
  r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  r += "Connection: close\r\n\r\n";
  r += body;
  return r;
}

int main() {
  // globalInit 必须在任何线程启动之前调用一次。
  HttpChat::globalInit();

  // =========================================================== 1. toJson
  {
    CASE("ChatRequest::toJson 合法 JSON + 往返 + 转义 + model 来自入参");
    ChatRequest q = baseReq("http://example.invalid/x", true);
    q.messages.clear();
    q.messages.push_back({"system", "第一行\n第二行\t制表 \"引号\" 反斜杠\\ 结束"});
    q.messages.push_back({"user", "中文 emoji 🚀 与控制字符\x01 混排"});
    q.temperature = 0.35;
    q.max_tokens = 128;
    q.stop.push_back("\n\n");
    q.stop.push_back("```");
    const std::string js = q.toJson();

    std::string err;
    mj::Value v = mj::Value::parse(js, err);
    CHECK(err.empty());
    CHECK(v.isObject());
    // model 必须是入参那个假模型名,不是写死的 deepseek-chat 之类。
    CHECK(v["model"].asString() == kFakeModel);
    CHECK(!contains(js, "deepseek-chat"));
    CHECK(v["stream"].asBool() == true);
    CHECK(v["max_tokens"].asInt() == 128);
    CHECK(v["temperature"].asNumber() > 0.34 && v["temperature"].asNumber() < 0.36);
    CHECK(v["messages"].isArray());
    CHECK(v["messages"].size() == 2);
    CHECK(v["messages"][0]["role"].asString() == "system");
    // 往返后内容必须逐字节相同(转义正确)。
    CHECK(v["messages"][0]["content"].asString() == q.messages[0].content);
    CHECK(v["messages"][1]["content"].asString() == q.messages[1].content);
    CHECK(v["stop"].isArray() && v["stop"].size() == 2);
    CHECK(v["stop"][0].asString() == "\n\n");
    // **key 不进 body**(走 Authorization 头)。
    CHECK(!contains(js, kFakeKey));
    CHECK(!contains(js, "api_key"));
    CHECK(!contains(js, "Authorization"));
    // 换行/引号必须是转义形式,不是裸字符。
    CHECK(!contains(js, "第一行\n"));
    CHECK(contains(js, "\\n"));

    // model 换一个 → 输出跟着换(再次证明不是写死的)。
    q.model = "another/model:v2";
    mj::Value v2 = mj::Value::parse(q.toJson(), err);
    CHECK(err.empty());
    CHECK(v2["model"].asString() == "another/model:v2");

    // 非流式 + 无 stop + max_tokens<=0
    ChatRequest q3 = baseReq("http://x.invalid", false);
    q3.max_tokens = 0;
    mj::Value v3 = mj::Value::parse(q3.toJson(), err);
    CHECK(err.empty());
    CHECK(v3["stream"].asBool() == false);
    CHECK(!v3.has("stop"));
    CHECK(!v3.has("max_tokens"));

    // 空 messages 也要生成合法 JSON。
    ChatRequest q4 = baseReq("http://x.invalid", true);
    q4.messages.clear();
    mj::Value v4 = mj::Value::parse(q4.toJson(), err);
    CHECK(err.empty());
    CHECK(v4["messages"].isArray() && v4["messages"].size() == 0);
  }

  // =========================================================== 2. 纯函数解析
  {
    CASE("sseChunkToDelta / chatBodyToText / 错误文案映射");
    std::string c, r;
    bool done = false;
    CHECK(sseChunkToDelta("[DONE]", c, r, done) && done);
    CHECK(sseChunkToDelta(" [DONE] ", c, r, done) && done);
    CHECK(sseChunkToDelta("{\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}", c, r,
                          done));
    CHECK(c == "hi" && !done && r.empty());
    CHECK(sseChunkToDelta(
        "{\"choices\":[{\"delta\":{\"reasoning_content\":\"思\"}}]}", c, r, done));
    CHECK(r == "思" && c.empty());
    CHECK(!sseChunkToDelta("{oops", c, r, done));
    CHECK(!sseChunkToDelta("", c, r, done));

    // 非流式回退:choices[0].message.content + usage
    std::string text, reason;
    int pt = -1, ct = -1;
    const std::string body =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"int main(){}\",\"reasoning_content\":\"先想\"}}],"
        "\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":7}}";
    CHECK(chatBodyToText(body, text, reason, pt, ct));
    CHECK(text == "int main(){}");
    CHECK(reason == "先想");
    CHECK(pt == 11 && ct == 7);
    CHECK(!chatBodyToText("{\"error\":{\"message\":\"bad\"}}", text, reason, pt, ct));
    CHECK(!chatBodyToText("not json", text, reason, pt, ct));
    CHECK(!chatBodyToText("", text, reason, pt, ct));

    // HTTP 状态 → 中文文案(§5.6)
    CHECK(contains(chatErrorZh(401, CURLE_OK, ""), "API key 无效或已过期"));
    CHECK(contains(chatErrorZh(402, CURLE_OK, ""), "余额不足或请求过于频繁"));
    CHECK(contains(chatErrorZh(429, CURLE_OK, ""), "余额不足或请求过于频繁"));
    CHECK(contains(chatErrorZh(500, CURLE_OK, ""), "服务端错误(HTTP 500)"));
    CHECK(contains(chatErrorZh(503, CURLE_OK, ""), "服务端错误(HTTP 503)"));
    CHECK(contains(chatErrorZh(404, CURLE_OK, ""), "HTTP 404"));
    // error.message 会被附上
    CHECK(contains(chatErrorZh(401, CURLE_OK, "{\"error\":{\"message\":\"no auth\"}}"),
                   "no auth"));
    // 解析不出来 → 贴 body 片段
    CHECK(contains(chatErrorZh(400, CURLE_OK, "<html>oops</html>"), "oops"));

    // curl 错误码 → 中文
    CHECK(curlErrorZh(CURLE_OPERATION_TIMEDOUT) == "请求超时");
    CHECK(curlErrorZh(CURLE_COULDNT_RESOLVE_HOST) ==
          "无法解析域名,检查网络或 base_url");
    CHECK(curlErrorZh(CURLE_SSL_CONNECT_ERROR) == "TLS 连接失败");
    CHECK(curlErrorZh(CURLE_ABORTED_BY_CALLBACK) == "已取消");
    CHECK(curlErrorZh(CURLE_OK).empty());
    CHECK(!curlErrorZh(9999).empty());  // 未知码也要有话说,不能空
    // curl 层失败时 HTTP 状态无意义,应走 curl 文案
    CHECK(chatErrorZh(0, CURLE_COULDNT_RESOLVE_HOST, "") ==
          "无法解析域名,检查网络或 base_url");
  }

  // =========================================================== 3. 参数校验
  {
    CASE("参数缺失:model 为空必须报错,绝不替换成写死的默认模型");
    HttpChat h;
    ChatRequest q = baseReq("http://127.0.0.1:1/x", true);
    q.model.clear();
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    CHECK(!r.ok);
    CHECK(contains(r.error, "模型"));
    CHECK(!contains(r.error, "deepseek"));
    assertNoKeyLeak(r, "empty-model");

    ChatRequest q2 = baseReq("", true);
    ChatResponse r2 = h.send(q2, nullDelta(), neverCancel());
    CHECK(!r2.ok && contains(r2.error, "base_url"));
    assertNoKeyLeak(r2, "empty-url");

    ChatRequest q3 = baseReq("http://127.0.0.1:1/x", true);
    q3.api_key.clear();
    ChatResponse r3 = h.send(q3, nullDelta(), neverCancel());
    CHECK(!r3.ok && contains(r3.error, "API key"));
  }

  // =========================================================== 4. (e) 立即取消
  {
    CASE("(e) should_cancel 立刻返回 true → canceled 且不算错误");
    HttpChat h;
    ChatRequest q = baseReq("https://api.deepseek.com/v1/chat/completions", true);
    q.total_timeout_ms = 6000;
    int polls = 0;
    ChatCancelFn always = [&polls]() { ++polls; return true; };
    const int64_t t0 = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    ChatResponse r = h.send(q, nullDelta(), always);
    const int64_t dt = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) - t0;
    CHECK(r.canceled == true);
    CHECK(r.ok == false);
    CHECK(r.error.empty());          // 取消不是错误
    CHECK(r.timed_out == false);
    CHECK(polls >= 1);
    CHECK(dt < 2000);                // 必须"立刻"作废,不等网络
    assertNoKeyLeak(r, "cancel-immediate");
    std::fprintf(stderr, "   canceled in %lldms, polls=%d\n",
                 static_cast<long long>(dt), polls);
  }

  // =========================================================== 5. (a) DNS 失败
  {
    CASE("(a) 不存在的域名 → COULDNT_RESOLVE_HOST 中文提示");
    HttpChat h;
    ChatRequest q = baseReq(
        "https://this-host-does-not-exist-cppide-test.invalid/v1/chat/completions",
        true);
    q.connect_timeout_ms = 4000;
    q.total_timeout_ms = 8000;
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    CHECK(!r.ok);
    CHECK(!r.canceled);
    CHECK(!r.error.empty());
    // 解析失败;某些环境会被 DNS 劫持成连接失败,两者都接受,但必须是中文提示。
    CHECK(contains(r.error, "无法解析域名") || contains(r.error, "无法连接服务器") ||
          contains(r.error, "请求超时"));
    assertNoKeyLeak(r, "dns-fail");
    std::fprintf(stderr, "   error=[%s] elapsed=%lldms\n", r.error.c_str(),
                 static_cast<long long>(r.elapsed_ms));
  }

  // =========================================================== 6. (b) 连接被拒
  {
    CASE("(b) http://127.0.0.1:1 → 连接失败,不崩");
    HttpChat h;
    ChatRequest q = baseReq("http://127.0.0.1:1/v1/chat/completions", true);
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    CHECK(!r.ok);
    CHECK(!r.canceled);
    CHECK(!r.error.empty());
    CHECK(contains(r.error, "无法连接服务器") || contains(r.error, "请求超时"));
    CHECK(r.elapsed_ms < 8000);
    assertNoKeyLeak(r, "conn-refused");
    std::fprintf(stderr, "   error=[%s] elapsed=%lldms\n", r.error.c_str(),
                 static_cast<long long>(r.elapsed_ms));
  }

  // =========================================================== 7. (d) 1ms 超时
  {
    // d1:本地慢服务器(127.0.0.1,不涉及 DNS)→ 超时路径完全确定。
    CASE("(d1) total_timeout_ms=1 打本地慢服务器 → 请求超时,毫秒级返回");
    MiniServer s;
    std::string sse = "data: {\"choices\":[{\"delta\":{\"content\":\"slow\"}}]}\n\n";
    s.response = httpResp("200 OK", "text/event-stream", sse);
    s.chunk = 8;
    s.delay_ms = 300;  // 慢到 1ms 超时必然命中
    CHECK(s.start());
    HttpChat h;
    ChatRequest q = baseReq(s.url(), true);
    q.total_timeout_ms = 1;
    q.connect_timeout_ms = 1;
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    s.stop();
    CHECK(!r.ok);
    CHECK(r.timed_out == true);
    CHECK(r.error == "请求超时");
    CHECK(!r.canceled);
    CHECK(r.elapsed_ms < 2000);
    assertNoKeyLeak(r, "timeout-1ms-local");
    std::fprintf(stderr, "   elapsed=%lldms\n",
                 static_cast<long long>(r.elapsed_ms));
  }
  {
    // d2:真实域名。注意上限放到 30s 而不是 3s ——
    // libcurl 的 Curl_resolver_kill 会 **join 解析线程**,所以 CURLOPT_TIMEOUT_MS
    // 无法打断一次正在进行的 getaddrinfo:send() 的实际下限是系统 DNS 的耗时
    // (冷缓存实测 ~3.2s,热缓存 ~2ms)。关键性质仍然成立:send() **一定会返回**,
    // 且它跑在 AiService 的 worker 线程上,UI 线程不受影响。
    CASE("(d2) total_timeout_ms=1 打真实域名 → 超时/网络错误,有界返回");
    HttpChat h;
    ChatRequest q = baseReq("https://api.deepseek.com/v1/chat/completions", true);
    q.total_timeout_ms = 1;
    q.connect_timeout_ms = 1;
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    CHECK(!r.ok);
    CHECK(!r.error.empty());     // 必须有可见的中文提示
    CHECK(!r.canceled);
    CHECK(r.elapsed_ms < 30000);  // 有界(受系统 DNS 解析耗时下限约束)
    if (r.timed_out) CHECK(r.error == "请求超时");
    assertNoKeyLeak(r, "timeout-1ms-real");
    std::fprintf(stderr, "   timed_out=%d error=[%s] elapsed=%lldms\n",
                 static_cast<int>(r.timed_out), r.error.c_str(),
                 static_cast<long long>(r.elapsed_ms));
  }

  // =========================================================== 8. 本地 SSE 流
  {
    CASE("本地 mini server:SSE 流式 → delta 顺序、拼接、reasoning 不进 on_delta");
    MiniServer s;
    std::string sse;
    sse += "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\",\"content\":\"\"}}]}\n\n";
    sse += "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"我想想\"}}]}\n\n";
    sse += "data: {\"choices\":[{\"delta\":{\"content\":\"int \"}}]}\n\n";
    sse += "data: {\"choices\":[{\"delta\":{\"content\":\"main() { 你好🚀 }\"}}]}\n\n";
    sse += "data: [DONE]\n\n";
    s.response = httpResp("200 OK", "text/event-stream", sse);
    s.chunk = 3;      // 每 3 字节一片:切在 UTF-8 中间、切在 "data:" 中间
    s.delay_ms = 0;
    CHECK(s.start());

    HttpChat h;
    ChatRequest q = baseReq(s.url(), true);
    std::vector<std::string> deltas;
    ChatResponse r = h.send(q,
                            [&](const std::string& d) {
                              deltas.push_back(d);
                              return true;
                            },
                            neverCancel());
    s.stop();
    CHECK(r.ok);
    CHECK(r.http_status == 200);
    CHECK(r.text == "int main() { 你好🚀 }");
    CHECK(deltas.size() == 2);
    CHECK(deltas[0] == "int ");
    // reasoning 单独交出,**不**经由 on_delta(写代码模式因此不可能插入思维链)
    CHECK(r.reasoning == "我想想");
    for (const std::string& d : deltas) CHECK(!contains(d, "我想想"));
    CHECK(!r.canceled && !r.timed_out);
    assertNoKeyLeak(r, "sse-ok");

    // 服务端收到的请求:key 在 Authorization 头里,body 里没有 key
    const std::string got = s.request();
    CHECK(contains(got, "Authorization: Bearer "));
    CHECK(contains(got, kFakeKey));                  // 头里有
    CHECK(contains(got, "Accept: text/event-stream"));
    CHECK(contains(got, "Content-Type: application/json"));
    const size_t bpos = got.find("\r\n\r\n");
    CHECK(bpos != std::string::npos);
    const std::string sent_body = got.substr(bpos + 4);
    CHECK(!sent_body.empty());
    CHECK(!contains(sent_body, kFakeKey));           // body 里没有
    CHECK(contains(sent_body, kFakeModel));          // model 来自入参
  }

  // =========================================================== 9. 本地非流式
  {
    CASE("本地 mini server:非流式 → choices[0].message.content + usage");
    MiniServer s;
    s.response = httpResp(
        "200 OK", "application/json",
        "{\"choices\":[{\"message\":{\"content\":\"非流式回复\","
        "\"reasoning_content\":\"链\"}}],"
        "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4}}");
    CHECK(s.start());
    HttpChat h;
    ChatRequest q = baseReq(s.url(), false);
    int calls = 0;
    ChatResponse r = h.send(q,
                            [&](const std::string& d) {
                              ++calls;
                              return d == "非流式回复";
                            },
                            neverCancel());
    s.stop();
    CHECK(r.ok);
    CHECK(r.text == "非流式回复");
    CHECK(r.reasoning == "链");
    CHECK(r.prompt_tokens == 3 && r.completion_tokens == 4);
    CHECK(calls == 1);  // 非流式只在末尾回调一次
    CHECK(contains(s.request(), "Accept: application/json"));
    assertNoKeyLeak(r, "nonstream-ok");
  }

  // =========================================================== 10. 4xx JSON 体
  {
    CASE("stream=true 但服务端回 401 + 普通 JSON 错误体 → 走错误路径");
    MiniServer s;
    s.response = httpResp("401 Unauthorized", "application/json",
                          "{\"error\":{\"message\":\"Authentication Fails\","
                          "\"type\":\"authentication_error\"}}");
    CHECK(s.start());
    HttpChat h;
    ChatRequest q = baseReq(s.url(), true);  // 请求的是 SSE,body 却是 JSON
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    s.stop();
    CHECK(!r.ok);
    CHECK(r.http_status == 401);
    CHECK(contains(r.error, "API key 无效或已过期"));
    CHECK(contains(r.error, "Authentication Fails"));  // 附上服务端原因
    CHECK(!r.canceled);
    assertNoKeyLeak(r, "local-401");
  }
  {
    CASE("500 + 非 JSON 体 → 服务端错误 + body 前 200 字节(不含 key)");
    MiniServer s;
    std::string junk = "<html><body>gateway exploded ";
    junk += std::string(600, 'Z');
    junk += "</body></html>";
    s.response = httpResp("502 Bad Gateway", "text/html", junk);
    CHECK(s.start());
    HttpChat h;
    ChatRequest q = baseReq(s.url(), true);
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    s.stop();
    CHECK(!r.ok);
    CHECK(r.http_status == 502);
    CHECK(contains(r.error, "服务端错误(HTTP 502)"));
    CHECK(contains(r.error, "gateway exploded"));
    CHECK(r.error.size() < 400);  // 只贴前 200 字节,不把整页糊上来
    CHECK(!contains(r.error, "\n"));  // 单行,能塞进状态栏
    assertNoKeyLeak(r, "local-502");
  }
  {
    CASE("200 但 body 是无法解析的垃圾 → 报协议不匹配 + 片段,不崩");
    MiniServer s;
    s.response = httpResp("200 OK", "text/plain", "totally not our protocol");
    CHECK(s.start());
    HttpChat h;
    ChatRequest q = baseReq(s.url(), true);
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    s.stop();
    CHECK(!r.ok);
    CHECK(contains(r.error, "无法解析") || contains(r.error, "协议"));
    CHECK(contains(r.error, "totally not our protocol"));
    assertNoKeyLeak(r, "garbage-200");
  }
  {
    CASE("200 空 body → 明确报空响应,不崩");
    MiniServer s;
    s.response = httpResp("200 OK", "text/event-stream", "");
    CHECK(s.start());
    HttpChat h;
    ChatRequest q = baseReq(s.url(), true);
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    s.stop();
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    assertNoKeyLeak(r, "empty-200");
  }

  // =========================================================== 11. 传输中取消
  {
    CASE("传输中 should_cancel 转 true → 立即中止,canceled 且 error 为空");
    MiniServer s;
    std::string sse;
    for (int i = 0; i < 200; ++i)
      sse += "data: {\"choices\":[{\"delta\":{\"content\":\"" +
             std::to_string(i) + "\"}}]}\n\n";
    // 不给 Content-Length:分片慢慢吐,模拟真实流
    s.response = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                             "Connection: close\r\n\r\n") + sse;
    s.chunk = 40;
    s.delay_ms = 20;
    CHECK(s.start());

    HttpChat h;
    ChatRequest q = baseReq(s.url(), true);
    q.total_timeout_ms = 8000;
    std::atomic<bool> want_cancel{false};
    int n = 0;
    ChatResponse r = h.send(q,
                            [&](const std::string&) {
                              if (++n >= 3) want_cancel.store(true);
                              return true;
                            },
                            [&]() { return want_cancel.load(); });
    s.stop();
    CHECK(r.canceled == true);
    CHECK(r.ok == false);
    CHECK(r.error.empty());  // 取消不是错误
    CHECK(n >= 3);
    CHECK(r.elapsed_ms < 8000);
    assertNoKeyLeak(r, "cancel-midstream");
    std::fprintf(stderr, "   canceled after %d deltas, %lldms\n", n,
                 static_cast<long long>(r.elapsed_ms));
  }
  {
    CASE("on_delta 返回 false → 中止传输,同样按取消处理");
    MiniServer s;
    std::string sse;
    for (int i = 0; i < 100; ++i)
      sse += "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
    s.response = httpResp("200 OK", "text/event-stream", sse);
    s.chunk = 60;
    s.delay_ms = 5;
    CHECK(s.start());
    HttpChat h;
    ChatRequest q = baseReq(s.url(), true);
    int n = 0;
    ChatResponse r = h.send(q,
                            [&](const std::string&) {
                              ++n;
                              return n < 2;  // 第二次就要求中止
                            },
                            neverCancel());
    s.stop();
    CHECK(!r.ok);
    CHECK(r.canceled);
    CHECK(r.error.empty());
    CHECK(n == 2);
    assertNoKeyLeak(r, "delta-false");
  }

  // =========================================================== 12. 句柄复用
  {
    CASE("同一个 HttpChat 连续多次 send:半帧不跨请求残留");
    MiniServer s1, s2;
    s1.response = httpResp("200 OK", "text/event-stream",
                           "data: {\"choices\":[{\"delta\":{\"content\":\"AAA\"}}]}");
    // 注意:上面这帧**没有**结尾空行,靠 finish() 吐出;
    // 若 carry 跨请求残留,第二次请求就会串味。
    s2.response = httpResp("200 OK", "text/event-stream",
                           "data: {\"choices\":[{\"delta\":{\"content\":\"BBB\"}}]}\n\n"
                           "data: [DONE]\n\n");
    CHECK(s1.start());
    CHECK(s2.start());
    HttpChat h;
    ChatRequest q1 = baseReq(s1.url(), true);
    ChatResponse r1 = h.send(q1, nullDelta(), neverCancel());
    ChatRequest q2 = baseReq(s2.url(), true);
    ChatResponse r2 = h.send(q2, nullDelta(), neverCancel());
    s1.stop();
    s2.stop();
    CHECK(r1.ok && r1.text == "AAA");
    CHECK(r2.ok && r2.text == "BBB");  // 不含 AAA
    CHECK(!contains(r2.text, "AAA"));
    assertNoKeyLeak(r1, "reuse-1");
    assertNoKeyLeak(r2, "reuse-2");
  }

  // =========================================================== 12b. key 回显
  {
    CASE("服务端把 key 原样回显在错误体里 → 必须被抹掉再交给上层");
    MiniServer s;
    std::string body = "{\"error\":{\"message\":\"bad key ";
    body += kFakeKey;                                  // 完整回显
    body += " and partial ";
    body += std::string(kFakeKey).substr(5, 12);       // 截断回显
    body += "\"}}";
    s.response = httpResp("401 Unauthorized", "application/json", body);
    CHECK(s.start());
    HttpChat h;
    ChatRequest q = baseReq(s.url(), true);
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    s.stop();
    CHECK(!r.ok && r.http_status == 401);
    CHECK(contains(r.error, "API key 无效或已过期"));
    CHECK(!contains(r.error, kFakeKey));
    // key 的任意 8 字节连续片段都不该出现
    const std::string k(kFakeKey);
    for (size_t i = 0; i + 8 <= k.size(); ++i)
      CHECK(!contains(r.error, k.substr(i, 8)));
    CHECK(contains(r.error, "REDACTED"));
    assertNoKeyLeak(r, "key-echo");
  }

  // =========================================================== 13. (c) 真实 401
  {
    CASE("(c) 假 key 打真实 api.deepseek.com → 期望 401 映射成中文且不含 key");
    HttpChat h;
    ChatRequest q = baseReq("https://api.deepseek.com/v1/chat/completions", true);
    q.connect_timeout_ms = 5000;
    q.total_timeout_ms = 15000;
    ChatResponse r = h.send(q, nullDelta(), neverCancel());
    assertNoKeyLeak(r, "real-401");
    std::fprintf(stderr, "   status=%ld ok=%d error=[%s]\n", r.http_status,
                 static_cast<int>(r.ok), r.error.c_str());
    if (r.http_status == 401) {
      CHECK(!r.ok);
      CHECK(contains(r.error, "API key 无效或已过期"));
      CHECK(!contains(r.error, kFakeKey));
      CHECK(!r.canceled);
      std::fprintf(stderr, "   [real 401 verified]\n");
    } else if (r.http_status == 402 || r.http_status == 429) {
      CHECK(contains(r.error, "余额不足或请求过于频繁"));
    } else if (!r.ok && !r.error.empty()) {
      ++g_skipped;
      std::fprintf(stderr,
                   "   SKIP: 网络不可用(status=%ld error=%s),已验证不崩且无 key 泄漏\n",
                   r.http_status, r.error.c_str());
    } else {
      // 拿假 key 竟然成功?那就是环境有代理在伪造响应,只断言不含 key。
      ++g_skipped;
      std::fprintf(stderr, "   SKIP: 意外成功(status=%ld)\n", r.http_status);
    }
  }

  // =========================================================== 14. 并发
  {
    CASE("4 线程 × 各自的 HttpChat 实例 × 20 次错误请求 → 不崩不泄漏");
    std::atomic<int> done_count{0};
    std::atomic<int> nonempty_err{0};
    std::atomic<int> crashed{0};
    auto worker = [&](int tid) {
      HttpChat h;  // 每线程一个实例(契约)
      for (int i = 0; i < 20; ++i) {
        ChatRequest q = baseReq("http://127.0.0.1:1/v1/chat/completions", true);
        q.connect_timeout_ms = 300;
        q.total_timeout_ms = 1000;
        q.messages[1].content = "线程 " + std::to_string(tid) + " 第 " +
                                std::to_string(i) + " 次 🚀";
        bool cancel_this = (i % 5 == 4);
        ChatResponse r = h.send(
            q, [](const std::string&) { return true; },
            [cancel_this]() { return cancel_this; });
        if (contains(r.error, kFakeKey) || contains(r.text, kFakeKey))
          crashed.fetch_add(1);
        if (cancel_this) {
          if (!r.canceled || !r.error.empty()) crashed.fetch_add(1);
        } else {
          if (r.ok) crashed.fetch_add(1);
          if (!r.error.empty()) nonempty_err.fetch_add(1);
        }
        done_count.fetch_add(1);
      }
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back(worker, t);
    for (std::thread& t : ts) t.join();
    CHECK(done_count.load() == 80);
    CHECK(crashed.load() == 0);
    CHECK(nonempty_err.load() == 64);  // 16 次是取消(error 为空)
    std::fprintf(stderr, "   80 requests across 4 threads, all accounted\n");
  }

  HttpChat::globalCleanup();
  std::fprintf(stderr, "test_aihttp: OK cases=%d checks=%d skipped=%d\n", g_cases,
               g_checks, g_skipped);
  std::printf("test_aihttp: all passed (%d cases, %d checks, %d skipped)\n", g_cases,
              g_checks, g_skipped);
  return 0;
}

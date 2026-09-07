// test_ai.cpp —— AiService(src/ai.cpp)验收测试。纯 assert 风格,main 返回 0 = 通过。
//
// 设计要点:
//  * CHECK 宏**不依赖 assert**,所以整份测试在 -DNDEBUG 下同样有效
//    (验收项 1「§6 反证」明确要求 NDEBUG 下也通过)。
//  * 为了直接断言私有的 `AiService::sinkFor` / `buildRequest` / `buildContext` /
//    `th_`(worker 线程是否存在),本文件在 include 项目头之前做 `#define private public`。
//    所有标准库头都在这条 define **之前**先 include 完,避免宏污染 libstdc++。
//    这只影响本 TU 的访问控制,不改变任何布局(无虚函数、成员声明顺序不变)。
//  * 全程 alarm() 看门狗 + 每个网络用例都有毫秒级上限,**不可能永久挂住**。
//  * 需要真实网络的用例(假 key 打 api.deepseek.com)在网络不可用时自动跳过并打印说明。

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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// ★ 只为测试打开私有区(见文件头说明)。标准库头已在上面全部 include 完毕。
#define private public
#include "ai.h"
#include "aihttp.h"
#include "config.h"
#include "textbuf.h"
#include "util.h"
#undef private

// ---------------------------------------------------------------------------
// 测试脚手架
// ---------------------------------------------------------------------------

static int g_checks = 0;
static int g_cases = 0;
static int g_skipped = 0;

static void failAt(const char* file, int line, const char* expr, const std::string& note) {
  std::fflush(stdout);
  std::fprintf(stderr, "\nFAIL %s:%d  CHECK(%s)  %s\n", file, line, expr, note.c_str());
  std::fflush(stderr);
  std::abort();   // 不用 assert:NDEBUG 下也必须真的失败
}

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) failAt(__FILE__, __LINE__, #cond, std::string());      \
  } while (0)

#define CHECK_M(cond, note)                                            \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) failAt(__FILE__, __LINE__, #cond, std::string(note));  \
  } while (0)

static void caseBegin(const char* name) {
  ++g_cases;
  std::printf("  [%2d] %s\n", g_cases, name);
  std::fflush(stdout);
}

static void sleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static int64_t nowMs() { return util::nowMs(); }

// /proc/self/status 的 Threads:(Linux)。取不到返回 -1(macOS 上跳过该断言)。
static int threadCount() {
  std::ifstream f("/proc/self/status");
  if (!f) return -1;
  std::string line;
  while (std::getline(f, line)) {
    if (util::startsWith(line, "Threads:")) {
      return std::atoi(line.c_str() + 8);
    }
  }
  return -1;
}

// 等到线程数变成 want。必须轮询:pthread_join 返回之后,内核 task 从 /proc 里
// 消失还有一个极短的窗口(实测 20 轮里会命中 1~2 次),直接读会假阳性。
static bool waitThreadCount(int want, int timeout_ms) {
  if (want < 0) return true;
  const int64_t deadline = util::nowMs() + timeout_ms;
  for (;;) {
    const int n = threadCount();
    if (n < 0 || n == want) return true;
    if (util::nowMs() >= deadline) {
      std::printf("       (线程数 %d != 期望 %d)\n", n, want);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// ---------------------------------------------------------------------------
// 本地 mini HTTP 服务器(不联网也能测流式/背压/4xx/超时/在飞)
// ---------------------------------------------------------------------------

struct MiniServer {
  int lfd = -1;
  int port = 0;
  std::atomic<bool> stop{false};
  std::atomic<int> hits{0};
  std::string resp;             // 完整响应字节
  size_t chunk = 0;             // 0 = 一次性写完
  int chunk_delay_ms = 0;
  bool hang = false;            // 只接受连接,永不回字节(制造"在飞"/超时)
  std::thread th;
  std::mutex mu;
  std::string last_req;         // 最后一次收到的请求原文(含请求头)
  std::vector<int> held;        // hang 模式下扣住的连接

  bool start() {
    lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return false;
    int on = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
    if (::listen(lfd, 16) != 0) return false;
    sockaddr_in got{};
    socklen_t gl = sizeof(got);
    if (::getsockname(lfd, reinterpret_cast<sockaddr*>(&got), &gl) != 0) return false;
    port = ::ntohs(got.sin_port);
    th = std::thread([this] { loop(); });
    return true;
  }

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port);
  }

  std::string lastRequest() {
    std::lock_guard<std::mutex> lk(mu);
    return last_req;
  }

  void loop() {
    while (!stop.load()) {
      pollfd pf{lfd, POLLIN, 0};
      int r = ::poll(&pf, 1, 50);
      if (r <= 0) continue;
      int c = ::accept(lfd, nullptr, nullptr);
      if (c < 0) continue;
      hits.fetch_add(1);
      timeval tv{0, 300 * 1000};
      ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      std::string req;
      char buf[4096];
      const int64_t deadline = nowMs() + 800;
      while (nowMs() < deadline) {
        ssize_t n = ::recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
        if (req.find("\r\n\r\n") != std::string::npos) {
          // 头收全了;若有 body 就按 Content-Length 收完
          size_t hend = req.find("\r\n\r\n") + 4;
          size_t cl = 0;
          size_t p = util::findNoCase(req, "content-length:");
          if (p != std::string::npos) cl = static_cast<size_t>(std::atol(req.c_str() + p + 15));
          if (req.size() >= hend + cl) break;
        }
      }
      {
        std::lock_guard<std::mutex> lk(mu);
        last_req = req;
      }
      if (hang) {
        std::lock_guard<std::mutex> lk(mu);
        held.push_back(c);       // 扣住不回,连接保持打开
        continue;
      }
      writeAll(c);
      ::shutdown(c, SHUT_RDWR);
      ::close(c);
    }
  }

  void writeAll(int c) {
    const size_t step = (chunk == 0) ? resp.size() : chunk;
    size_t off = 0;
    while (off < resp.size() && !stop.load()) {
      const size_t n = std::min(step, resp.size() - off);
      ssize_t w = ::send(c, resp.data() + off, n, MSG_NOSIGNAL);
      if (w <= 0) return;
      off += static_cast<size_t>(w);
      if (chunk_delay_ms > 0 && off < resp.size()) sleepMs(chunk_delay_ms);
    }
  }

  void shutdownServer() {
    stop.store(true);
    if (th.joinable()) th.join();
    {
      std::lock_guard<std::mutex> lk(mu);
      for (int c : held) ::close(c);
      held.clear();
    }
    if (lfd >= 0) ::close(lfd);
    lfd = -1;
  }

  ~MiniServer() { shutdownServer(); }
};

static std::string httpBody200(const std::string& body) {
  std::ostringstream os;
  os << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
     << body.size() << "\r\nConnection: close\r\n\r\n" << body;
  return os.str();
}

static std::string httpStatusJson(int status, const std::string& reason,
                                  const std::string& body) {
  std::ostringstream os;
  os << "HTTP/1.1 " << status << " " << reason
     << "\r\nContent-Type: application/json\r\nContent-Length: " << body.size()
     << "\r\nConnection: close\r\n\r\n" << body;
  return os.str();
}

// 用 content 片段构造一条 SSE 响应(可选 reasoning_content 片段)。
static std::string sseResponse(const std::vector<std::string>& contents,
                               const std::vector<std::string>& reasonings = {}) {
  std::ostringstream os;
  os << "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
  for (const std::string& r : reasonings) {
    os << "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"" << r << "\"}}]}\n\n";
  }
  for (const std::string& c : contents) {
    os << "data: {\"choices\":[{\"delta\":{\"content\":\"" << c << "\"}}]}\n\n";
  }
  os << "data: [DONE]\n\n";
  return os.str();
}

// ---------------------------------------------------------------------------
// 配置 / 缓冲区 / 事件 helper
// ---------------------------------------------------------------------------

static const char* kFakeKey = "sk-FAKEKEY-cppide-test-0123456789abcdef";

static Config cfgFor(const std::string& base_url, const std::string& key = kFakeKey) {
  Config c;
  c.api_key = key;
  c.base_url = base_url;
  c.chat_path = "/v1/chat/completions";
  c.model = "test-model-XYZ-9";
  c.stream = true;
  c.ghost_delay_ms = 80;
  c.ghost_min_interval_ms = 2000;
  c.ghost_max_lines = 8;
  c.ai_connect_timeout_ms = 1000;
  c.ai_timeout_ms = 4000;
  c.ai_max_context_lines = 400;
  return c;
}

static void fillBuf(TextBuffer& b, const std::vector<std::string>& lines) {
  b.reset(lines);
}

static TextBuffer sampleBuf() {
  TextBuffer b;
  fillBuf(b, {"#include <cstdio>", "int main() {", "  int n = 0;", "  ret", "}"});
  return b;
}

static void drainInto(Mailbox<AppEvent>& mb, std::vector<AppEvent>& out) {
  AppEvent e;
  while (mb.tryPop(e)) out.push_back(std::move(e));
}

// 轮询等到出现某种事件(或超时)。永不阻塞在条件变量上。
static bool waitForKind(Mailbox<AppEvent>& mb, std::vector<AppEvent>& out, EvKind k,
                        int timeout_ms) {
  const int64_t deadline = nowMs() + timeout_ms;
  for (;;) {
    drainInto(mb, out);
    for (const AppEvent& e : out) {
      if (e.kind == k) return true;
    }
    if (nowMs() >= deadline) return false;
    sleepMs(2);
  }
}

static bool waitInFlight(const AiService& svc, bool want, int timeout_ms) {
  const int64_t deadline = nowMs() + timeout_ms;
  while (nowMs() < deadline) {
    if (svc.inFlight() == want) return true;
    sleepMs(1);
  }
  return svc.inFlight() == want;
}

static int countKind(const std::vector<AppEvent>& evs, EvKind k) {
  int n = 0;
  for (const AppEvent& e : evs) {
    if (e.kind == k) ++n;
  }
  return n;
}

static const AppEvent* findKind(const std::vector<AppEvent>& evs, EvKind k) {
  for (const AppEvent& e : evs) {
    if (e.kind == k) return &e;
  }
  return nullptr;
}

static bool hasNonAscii(const std::string& s) {
  for (unsigned char c : s) {
    if (c >= 0x80) return true;
  }
  return false;
}

// key 的任意 8 字节片段都不许出现(与 aihttp 的 scrub 口径一致)。
static bool leaksKey(const std::string& s, const std::string& key) {
  if (key.size() < 8) return false;
  for (size_t w = 0; w + 8 <= key.size(); ++w) {
    if (s.find(key.substr(w, 8)) != std::string::npos) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// 1. §6 反证:sink 决策点 + 练习模式的 sink 恒为 PanelOnly(NDEBUG 下也必须过)
// ---------------------------------------------------------------------------

static void case_sink_decision_point() {
  caseBegin("§6(a) sinkFor 是唯一决策点,练习模式恒 PanelOnly");
  CHECK(AiService::sinkFor(AiMode::Practice) == AiSink::PanelOnly);
  CHECK(AiService::sinkFor(AiMode::Code) == AiSink::GhostText);

  Mailbox<AppEvent> out;
  Config c = cfgFor("http://127.0.0.1:1", "");   // key 空 -> 不起线程,只用纯函数
  AiService svc(c, &out);
  TextBuffer b = sampleBuf();
  const Pos cur{3, 5};

  // 练习模式:问题为空 / 非空 / 各种上下文,sink 必须恒为 PanelOnly。
  const std::vector<std::string> qs = {"", "怎么优化?", std::string(500, 'x')};
  for (const std::string& q : qs) {
    const AiRequest r = svc.buildRequest(b, cur, q, AiMode::Practice);
    CHECK(r.sink == AiSink::PanelOnly);
    CHECK(r.mode == AiMode::Practice);
    CHECK(r.sink != AiSink::GhostText);
    const AiRequest rc = svc.buildRequest(b, cur, q, AiMode::Code);
    CHECK(rc.sink == AiSink::GhostText);
    CHECK(rc.mode == AiMode::Code);
  }
  // 切模式后再建请求,sink 依然只由 mode 决定(没有别处的状态能改它)。
  svc.setMode(AiMode::Practice);
  CHECK(svc.buildRequest(b, cur, "", svc.mode()).sink == AiSink::PanelOnly);
  svc.setMode(AiMode::Code);
  CHECK(svc.buildRequest(b, cur, "", svc.mode()).sink == AiSink::GhostText);
  // 练习模式的 system 一定带那句不可覆盖的硬后缀。
  const AiRequest rp = svc.buildRequest(b, cur, "", AiMode::Practice);
  CHECK(rp.system_prompt.find(aiprompt::practiceHardSuffix()) != std::string::npos);
  // 配置覆盖 system 后,硬后缀仍然在。
  Config c2 = c;
  c2.prompt_practice = "自定义教练 prompt";
  AiService svc2(c2, &out);
  const AiRequest rp2 = svc2.buildRequest(b, cur, "", AiMode::Practice);
  CHECK(rp2.system_prompt.find("自定义教练 prompt") != std::string::npos);
  CHECK(rp2.system_prompt.find(aiprompt::practiceHardSuffix()) != std::string::npos);
  CHECK(rp2.sink == AiSink::PanelOnly);
}

// 遍历 ai.cpp 源码:它不含任何写 TextBuffer 的调用。
// 这是"练习模式不会向缓冲区插入任何代码"的**结构性**证据。
static void case_source_has_no_buffer_writes() {
  caseBegin("§6(b) ai.cpp 源码里没有任何缓冲区写操作(grep 断言)");
  const char* cands[] = {nullptr, "src/ai.cpp", "../src/ai.cpp", "/work/src/ai.cpp"};
  cands[0] = std::getenv("CPPIDE_AI_CPP");
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
  CHECK_M(!src.empty(), "找不到 src/ai.cpp;设 CPPIDE_AI_CPP=<路径> 或从仓库根目录运行");
  std::printf("       (读取 %s,%zu 字节)\n", used.c_str(), src.size());

  // (1) 全文没有 TextBuffer 的两个写原语调用,也没有任何 insert(/erase( 形式的调用。
  CHECK_M(src.find("insert(") == std::string::npos, "ai.cpp 出现了 insert( 调用");
  CHECK_M(src.find("erase(") == std::string::npos, "ai.cpp 出现了 erase( 调用");
  CHECK(src.find(".insert") == std::string::npos);
  CHECK(src.find("->insert") == std::string::npos);
  CHECK(src.find(".erase") == std::string::npos);
  CHECK(src.find("->erase") == std::string::npos);
  // 其它可写路径也不许出现。
  CHECK(src.find("TextBuffer::Edit") == std::string::npos);
  CHECK(src.find("beginGroup") == std::string::npos);
  CHECK(src.find("reset(") == std::string::npos);
  CHECK(src.find("loadFile") == std::string::npos);
  CHECK(src.find("saveFile") == std::string::npos);
  CHECK(src.find("setPath") == std::string::npos);
  CHECK(src.find("undo(") == std::string::npos);
  CHECK(src.find("redo(") == std::string::npos);
  // Editor 也不该被 ai.cpp 认识(ghost 的落地在 Editor,不在这里)。
  CHECK(src.find("editor.h") == std::string::npos);
  CHECK(src.find("Editor") == std::string::npos);
  CHECK(src.find("acceptGhost") == std::string::npos);

  // (2) 每一行提到 TextBuffer 的地方,要么是注释,要么写的是 const TextBuffer&。
  const std::vector<std::string> lines = util::splitLines(src);
  int mention = 0, const_ref = 0;
  for (const std::string& raw : lines) {
    if (raw.find("TextBuffer") == std::string::npos) continue;
    ++mention;
    const std::string t = util::trimLeft(raw);
    const bool comment = util::startsWith(t, "//") || util::startsWith(t, "*");
    const bool cref = raw.find("const TextBuffer&") != std::string::npos;
    if (cref) ++const_ref;
    CHECK_M(comment || cref, "ai.cpp 里出现了非 const 的 TextBuffer 引用:" + raw);
  }
  CHECK(mention > 0);
  CHECK(const_ref >= 3);   // buildContext / buildRequest / maybeAutoTrigger / askNow
  // (3) sink 只可能来自 sinkFor():ai.cpp 里连 AiSink 的枚举字面量都不出现,
  //     所以不存在"某处自己决定了一个 GhostText"的可能。
  CHECK_M(src.find("AiSink::GhostText") == std::string::npos,
          "ai.cpp 里出现了 AiSink::GhostText 字面量(sink 决策散落了)");
  CHECK(src.find("AiSink::PanelOnly") == std::string::npos);
  CHECK(src.find("sinkFor(") != std::string::npos);
  // 每一处 sink 赋值都只能是 sinkFor(...) 或从请求里透传。
  for (const std::string& raw : lines) {
    const std::string t = util::trimLeft(raw);
    if (util::startsWith(t, "//")) continue;      // 注释里的 "sink == ..." 不算赋值
    const size_t at = raw.find("sink =");
    if (at == std::string::npos) continue;
    if (raw.size() > at + 6 && raw[at + 6] == '=') continue;   // "sink =="
    const bool ok = raw.find("sinkFor(") != std::string::npos ||
                    raw.find("= sink;") != std::string::npos;
    CHECK_M(ok, "ai.cpp 里有一处 sink 赋值不是来自 sinkFor:" + raw);
  }
}

// ---------------------------------------------------------------------------
// 2. key 为空:不起线程、未配置、不崩
// ---------------------------------------------------------------------------

static void case_disabled_no_worker_thread() {
  caseBegin("key 为空:worker 线程未创建 + \"未配置\" 提示 + 不崩");
  const int base = threadCount();
  Mailbox<AppEvent> out;
  Config c = cfgFor("http://127.0.0.1:1", "");
  {
    AiService svc(c, &out);
    CHECK(svc.enabled() == false);
    CHECK(std::string(svc.stateZh()) == "未配置");
    CHECK(svc.state() == AiState::Disabled);
    CHECK(svc.inFlight() == false);
    // ★ 线程根本不存在(两种可观测方式)
    CHECK_M(svc.th_.joinable() == false, "key 为空却创建了 worker 线程");
    if (base >= 0) {
      CHECK_M(threadCount() == base, "key 为空却多出了线程");
    }
    TextBuffer b = sampleBuf();
    // askNow:不崩,且给出"未配置"提示
    svc.askNow(b, Pos{3, 5}, "");
    std::vector<AppEvent> evs;
    drainInto(out, evs);
    CHECK(evs.size() == 1);
    CHECK(evs[0].kind == EvKind::AiError);
    CHECK(evs[0].text.find("未配置") != std::string::npos);
    CHECK(evs[0].gen == svc.generation());     // 不会被 UI 的 gen 过滤掉
    // 自动触发:即使各种时间条件都满足也不发请求、不崩
    svc.setMode(AiMode::Code);
    svc.noteActivity();
    sleepMs(c.ghost_delay_ms + 40);
    for (int i = 0; i < 50; ++i) svc.maybeAutoTrigger(b, Pos{3, 5});
    CHECK(svc.inFlight() == false);
    CHECK(svc.in_.size() == 0);
    evs.clear();
    drainInto(out, evs);
    CHECK(evs.empty());
    // 其它 UI 侧调用一律不崩
    svc.noteActivity();
    svc.cancelInFlight();
    CHECK(svc.toggleMode() == AiMode::Practice);
    CHECK(std::string(svc.stateZh()) == "未配置");
    // 空白缓冲区 + 未配置的组合
    TextBuffer empty;
    svc.maybeAutoTrigger(empty, Pos{0, 0});
    CHECK(svc.enabled() == false);
  }
  CHECK(waitThreadCount(base, 1000));

  // 对照组:key 非空 -> 线程确实被创建了
  Config c2 = cfgFor("http://127.0.0.1:1");
  {
    AiService svc(c2, &out);
    CHECK_M(svc.th_.joinable() == true, "key 非空却没有 worker 线程");
    if (base >= 0) CHECK_M(waitThreadCount(base + 1, 1000), "key 非空却没多出线程");
    CHECK(svc.enabled() == true);
    CHECK(std::string(svc.stateZh()) == "就绪");
  }
  CHECK_M(waitThreadCount(base, 1000), "析构后线程没有回收");
  out.drain();
}

// ---------------------------------------------------------------------------
// 3. 取消语义:generation 过滤 / 切模式 / cancelInFlight
// ---------------------------------------------------------------------------

static void case_cancel_semantics() {
  caseBegin("取消:noteActivity / setMode / cancelInFlight 都让在飞回复不落地");
  MiniServer srv;
  srv.hang = true;                        // 连上就扣住,制造稳定的"在飞"
  CHECK(srv.start());
  Config c = cfgFor(srv.url());
  c.ai_timeout_ms = 8000;

  // (a) noteActivity 之后,任何事件的 gen 都不等于当前 generation()
  {
    Mailbox<AppEvent> out;
    AiService svc(c, &out);
    svc.setMode(AiMode::Code);
    TextBuffer b = sampleBuf();
    svc.askNow(b, Pos{3, 5}, "");
    const uint64_t g0 = svc.generation();
    CHECK(waitInFlight(svc, true, 1000));
    svc.noteActivity();                   // 立刻作废
    CHECK(svc.generation() != g0);
    const int64_t t0 = nowMs();
    CHECK_M(waitInFlight(svc, false, 4000), "取消没有让在飞请求及时结束");
    const int64_t abort_ms = nowMs() - t0;
    std::printf("       curl 因取消回调中止耗时 %lld ms\n", static_cast<long long>(abort_ms));
    sleepMs(120);
    std::vector<AppEvent> evs;
    drainInto(out, evs);
    for (const AppEvent& e : evs) {
      CHECK_M(e.gen != svc.generation(), "被取消的请求还发出了当前 generation 的事件");
      CHECK(e.kind != EvKind::AiDone);
      CHECK_M(e.kind != EvKind::AiError, "取消被误报成错误");
    }
  }

  // (b) setMode 切模式:旧回复不落地,且新模式下 sink 立刻变了
  {
    Mailbox<AppEvent> out;
    AiService svc(c, &out);
    svc.setMode(AiMode::Code);
    TextBuffer b = sampleBuf();
    svc.askNow(b, Pos{3, 5}, "");
    CHECK(waitInFlight(svc, true, 1000));
    const uint64_t g0 = svc.generation();
    svc.setMode(AiMode::Practice);        // §6(3):先 gen_++
    CHECK(svc.generation() != g0);
    CHECK(svc.mode() == AiMode::Practice);
    CHECK(waitInFlight(svc, false, 4000));
    sleepMs(120);
    std::vector<AppEvent> evs;
    drainInto(out, evs);
    for (const AppEvent& e : evs) {
      CHECK_M(!(e.gen == svc.generation() && e.sink == AiSink::GhostText),
              "切到练习模式后仍有 GhostText 事件通过 gen 过滤");
    }
  }

  // (c) cancelInFlight
  {
    Mailbox<AppEvent> out;
    AiService svc(c, &out);
    svc.setMode(AiMode::Code);
    TextBuffer b = sampleBuf();
    svc.askNow(b, Pos{3, 5}, "");
    CHECK(waitInFlight(svc, true, 1000));
    const uint64_t g0 = svc.generation();
    svc.cancelInFlight();
    CHECK(svc.generation() != g0);
    CHECK_M(waitInFlight(svc, false, 4000), "cancelInFlight 没生效");
    sleepMs(120);
    std::vector<AppEvent> evs;
    drainInto(out, evs);
    CHECK(countKind(evs, EvKind::AiDone) == 0);
    for (const AppEvent& e : evs) CHECK(e.gen != svc.generation());
  }

  // (d) 请求还在队列里就被作废 -> 一个事件都不发
  {
    Mailbox<AppEvent> out;
    Config c2 = c;
    AiService svc(c2, &out);
    svc.setMode(AiMode::Code);
    TextBuffer b = sampleBuf();
    svc.askNow(b, Pos{3, 5}, "");     // 第一个进 worker
    svc.askNow(b, Pos{3, 5}, "");     // 第二个排队;同时作废了第一个
    svc.noteActivity();               // 把排队中的那个也作废
    CHECK(waitInFlight(svc, false, 5000));
    sleepMs(120);
    std::vector<AppEvent> evs;
    drainInto(out, evs);
    for (const AppEvent& e : evs) CHECK(e.gen != svc.generation());
  }
  srv.shutdownServer();
}

// ---------------------------------------------------------------------------
// 4. 不卡死:worker 卡在网络上时,UI 线程侧调用全部立即返回
// ---------------------------------------------------------------------------

static int64_t g_max_ui_us = 0;

static void case_ui_never_blocks() {
  caseBegin("不卡死:worker 卡在网络上时 UI 线程调用 < 5ms");
  MiniServer srv;
  srv.hang = true;
  CHECK(srv.start());
  Config c = cfgFor(srv.url());
  c.ai_timeout_ms = 8000;             // 让它稳稳地卡住
  c.ghost_delay_ms = 0;
  c.ghost_min_interval_ms = 0;
  Mailbox<AppEvent> out;
  AiService svc(c, &out);
  svc.setMode(AiMode::Code);
  TextBuffer b;
  {
    std::vector<std::string> big;
    for (int i = 0; i < 2000; ++i) big.push_back("  for (int i = 0; i < n; ++i) sum += a[i];");
    fillBuf(b, big);
  }
  svc.askNow(b, Pos{1000, 3}, "");
  CHECK_M(waitInFlight(svc, true, 1500), "没能造出在飞状态");

  struct Sample { const char* name; int64_t us; };
  std::vector<Sample> worst;
  auto measure = [&](const char* name, const std::function<void()>& fn) {
    int64_t worst_us = 0;
    for (int i = 0; i < 40; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      fn();
      const auto t1 = std::chrono::steady_clock::now();
      const int64_t us =
          std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      worst_us = std::max(worst_us, us);
    }
    worst.push_back(Sample{name, worst_us});
    g_max_ui_us = std::max(g_max_ui_us, worst_us);
    CHECK_M(worst_us < 5000, std::string("UI 线程调用超过 5ms:") + name);
  };

  measure("noteActivity", [&] { svc.noteActivity(); });
  measure("maybeAutoTrigger", [&] { svc.maybeAutoTrigger(b, Pos{1000, 3}); });
  measure("askNow", [&] { svc.askNow(b, Pos{1000, 3}, "怎么优化"); });
  measure("stateZh", [&] { (void)svc.stateZh(); });
  measure("inFlight", [&] { (void)svc.inFlight(); });
  measure("generation", [&] { (void)svc.generation(); });
  measure("mode/setMode", [&] { (void)svc.toggleMode(); });
  measure("cancelInFlight", [&] { svc.cancelInFlight(); });
  CHECK(svc.inFlight() == true || svc.inFlight() == false);   // 只是不许挂住

  for (const Sample& s : worst) {
    std::printf("       %-16s 最坏 %lld us\n", s.name, static_cast<long long>(s.us));
  }
  srv.shutdownServer();
}

// ---------------------------------------------------------------------------
// 5. 错误路径:401 / 不可解析域名 / 超时 / 本地 4xx —— 中文提示、不含 key、不崩
// ---------------------------------------------------------------------------

static void case_error_paths_local() {
  caseBegin("错误路径(本地):401 / 超时 / 空回复 都给中文提示且不含 key");
  // (a) 本地 401
  {
    MiniServer srv;
    srv.resp = httpStatusJson(401, "Unauthorized",
                              std::string("{\"error\":{\"message\":\"Authentication Fails, "
                                          "your api key: ") + kFakeKey + " is invalid\"}}");
    CHECK(srv.start());
    Config c = cfgFor(srv.url());
    Mailbox<AppEvent> out;
    AiService svc(c, &out);
    svc.setMode(AiMode::Code);
    TextBuffer b = sampleBuf();
    svc.askNow(b, Pos{3, 5}, "");
    std::vector<AppEvent> evs;
    CHECK_M(waitForKind(out, evs, EvKind::AiError, 6000), "401 没有产出 AiError");
    const AppEvent* e = findKind(evs, EvKind::AiError);
    CHECK(e != nullptr);
    CHECK(hasNonAscii(e->text));
    CHECK_M(!leaksKey(e->text, kFakeKey), "错误消息泄漏了 api_key");
    CHECK(e->text.find("key") != std::string::npos ||
          e->text.find("无效") != std::string::npos);
    CHECK(e->gen == svc.generation());
    CHECK(std::string(svc.stateZh()) == "错误");
    CHECK(svc.state() == AiState::Error);
    std::printf("       401 -> \"%s\"\n", e->text.c_str());
    srv.shutdownServer();
  }
  // (b) 超时
  {
    MiniServer srv;
    srv.hang = true;
    CHECK(srv.start());
    Config c = cfgFor(srv.url());
    c.ai_timeout_ms = 400;
    c.ai_connect_timeout_ms = 300;
    Mailbox<AppEvent> out;
    AiService svc(c, &out);
    svc.setMode(AiMode::Code);
    TextBuffer b = sampleBuf();
    const int64_t t0 = nowMs();
    svc.askNow(b, Pos{3, 5}, "");
    std::vector<AppEvent> evs;
    CHECK_M(waitForKind(out, evs, EvKind::AiError, 5000), "超时没有产出 AiError");
    const AppEvent* e = findKind(evs, EvKind::AiError);
    CHECK(e != nullptr);
    CHECK(hasNonAscii(e->text));
    CHECK(e->text.find("超时") != std::string::npos);
    CHECK(!leaksKey(e->text, kFakeKey));
    std::printf("       timeout(%lld ms) -> \"%s\"\n",
                static_cast<long long>(nowMs() - t0), e->text.c_str());
    srv.shutdownServer();
  }
  // (c) 服务端回 200 但内容为空 -> 也要有可见提示,而不是静默
  {
    MiniServer srv;
    srv.resp = sseResponse({""});
    CHECK(srv.start());
    Config c = cfgFor(srv.url());
    Mailbox<AppEvent> out;
    AiService svc(c, &out);
    svc.setMode(AiMode::Code);
    TextBuffer b = sampleBuf();
    svc.askNow(b, Pos{3, 5}, "");
    std::vector<AppEvent> evs;
    CHECK(waitForKind(out, evs, EvKind::AiError, 6000));
    const AppEvent* e = findKind(evs, EvKind::AiError);
    CHECK(e != nullptr && hasNonAscii(e->text));
    CHECK(countKind(evs, EvKind::AiDone) == 0);
    srv.shutdownServer();
  }
  // (d) 服务端直接掐断连接(空响应)
  {
    MiniServer srv;
    srv.resp = "";
    CHECK(srv.start());
    Config c = cfgFor(srv.url());
    c.ai_timeout_ms = 2000;
    Mailbox<AppEvent> out;
    AiService svc(c, &out);
    svc.setMode(AiMode::Code);
    TextBuffer b = sampleBuf();
    svc.askNow(b, Pos{3, 5}, "");
    std::vector<AppEvent> evs;
    CHECK_M(waitForKind(out, evs, EvKind::AiError, 5000), "连接被掐断没有产出 AiError");
    const AppEvent* e = findKind(evs, EvKind::AiError);
    CHECK(e != nullptr && !e->text.empty());
    CHECK(!leaksKey(e->text, kFakeKey));
    srv.shutdownServer();
  }
}

static void case_error_paths_network() {
  caseBegin("错误路径(真实网络):假 key 打 api.deepseek.com / 不可解析域名");
  // (a) 不可解析域名(不需要外网也能得到"无法解析域名")
  {
    Config c = cfgFor("https://this-host-does-not-exist-cppide-ai.invalid");
    c.ai_timeout_ms = 8000;
    Mailbox<AppEvent> out;
    AiService svc(c, &out);
    svc.setMode(AiMode::Practice);
    TextBuffer b = sampleBuf();
    svc.askNow(b, Pos{3, 5}, "这题怎么想");
    std::vector<AppEvent> evs;
    CHECK_M(waitForKind(out, evs, EvKind::AiError, 15000), "不可解析域名没有产出 AiError");
    const AppEvent* e = findKind(evs, EvKind::AiError);
    CHECK(e != nullptr);
    CHECK(hasNonAscii(e->text));
    CHECK(!leaksKey(e->text, kFakeKey));
    for (const AppEvent& ev : evs) CHECK(ev.sink == AiSink::PanelOnly);
    std::printf("       DNS 失败 -> \"%s\"\n", e->text.c_str());
  }
  // (b) 假 key 打真实端点,期望 HTTP 401
  if (std::getenv("CPPIDE_NO_NET") != nullptr) {
    ++g_skipped;
    std::printf("       跳过真实 api.deepseek.com(CPPIDE_NO_NET 已设置)\n");
    return;
  }
  Config c = cfgFor("https://api.deepseek.com");
  c.model = "deepseek-chat";
  c.ai_timeout_ms = 15000;
  Mailbox<AppEvent> out;
  AiService svc(c, &out);
  svc.setMode(AiMode::Code);
  TextBuffer b = sampleBuf();
  svc.askNow(b, Pos{3, 5}, "");
  std::vector<AppEvent> evs;
  const bool got = waitForKind(out, evs, EvKind::AiError, 25000);
  if (!got) {
    ++g_skipped;
    std::printf("       跳过:25s 内没有拿到 AiError(网络不可用?)\n");
    CHECK(countKind(evs, EvKind::AiDone) == 0);   // 至少不能"成功"
    return;
  }
  const AppEvent* e = findKind(evs, EvKind::AiError);
  CHECK(e != nullptr);
  CHECK(hasNonAscii(e->text));
  CHECK_M(!leaksKey(e->text, kFakeKey), "401 文案泄漏了 api_key");
  CHECK(e->sink == AiSink::GhostText);   // 写代码模式的错误也照样带回 sink
  CHECK(std::string(svc.stateZh()) == "错误");
  std::printf("       真实 401 路径 -> \"%s\"\n", e->text.c_str());
  if (e->text.find("无法解析域名") != std::string::npos ||
      e->text.find("无法连接") != std::string::npos) {
    ++g_skipped;
    std::printf("       (本机没有外网,该用例退化为网络错误路径验证)\n");
  }
}

// ---------------------------------------------------------------------------
// 6. 析构安全
// ---------------------------------------------------------------------------

static void case_destruct_while_inflight() {
  caseBegin("析构安全:在飞时析构不崩不死锁 + 反复构造析构 50 次");
  MiniServer srv;
  srv.hang = true;
  CHECK(srv.start());
  Config c = cfgFor(srv.url());
  c.ai_timeout_ms = 20000;         // 故意远大于测试上限:必须靠取消而不是超时退出
  {
    Mailbox<AppEvent> out;
    AiService* svc = new AiService(c, &out);
    svc->setMode(AiMode::Code);
    TextBuffer b = sampleBuf();
    svc->askNow(b, Pos{3, 5}, "");
    CHECK(waitInFlight(*svc, true, 2000));
    const int64_t t0 = nowMs();
    delete svc;                    // 在飞时析构
    const int64_t dtor_ms = nowMs() - t0;
    std::printf("       在飞析构耗时 %lld ms\n", static_cast<long long>(dtor_ms));
    CHECK_M(dtor_ms < 4000, "在飞析构太慢(可能没有靠取消回调中止)");
  }
  // 反复构造析构:worker 线程正常起停,LSan 会盯泄漏
  const int base = threadCount();
  const int64_t t1 = nowMs();
  for (int i = 0; i < 50; ++i) {
    Mailbox<AppEvent> out;
    AiService svc(c, &out);
    CHECK(svc.th_.joinable());
    if ((i % 5) == 0) {
      TextBuffer b = sampleBuf();
      svc.setMode(AiMode::Code);
      svc.askNow(b, Pos{3, 5}, "");   // 一半的轮次带着在飞请求析构
    }
  }
  std::printf("       50 轮构造/析构耗时 %lld ms\n", static_cast<long long>(nowMs() - t1));
  CHECK_M(waitThreadCount(base, 2000), "50 轮之后有线程没回收");
  // 队列里有残留请求时析构也必须干净退出
  {
    Mailbox<AppEvent> out;
    Config c2 = cfgFor("http://127.0.0.1:1");
    AiService svc(c2, &out);
    TextBuffer b = sampleBuf();
    svc.setMode(AiMode::Code);
    for (int i = 0; i < 20; ++i) svc.askNow(b, Pos{3, 5}, "");
  }
  srv.shutdownServer();
}

// ---------------------------------------------------------------------------
// 7. 触发与限流
// ---------------------------------------------------------------------------

static void case_trigger_and_rate_limit() {
  caseBegin("触发与限流:打字期间不发;满足条件只发一个");
  MiniServer srv;
  srv.resp = sseResponse({"ok"});
  CHECK(srv.start());
  Config c = cfgFor(srv.url());
  c.ghost_delay_ms = 120;
  c.ghost_min_interval_ms = 5000;
  Mailbox<AppEvent> out;
  AiService svc(c, &out);
  TextBuffer b = sampleBuf();

  // (a) 练习模式下自动触发永不发请求
  svc.setMode(AiMode::Practice);
  svc.noteActivity();
  sleepMs(c.ghost_delay_ms + 60);
  for (int i = 0; i < 20; ++i) svc.maybeAutoTrigger(b, Pos{3, 5});
  sleepMs(80);
  CHECK_M(srv.hits.load() == 0, "练习模式竟然自动发起了补全请求");

  // (b) 写代码模式,连续打字期间不发
  svc.setMode(AiMode::Code);
  for (int i = 0; i < 15; ++i) {
    svc.noteActivity();
    svc.maybeAutoTrigger(b, Pos{3, 5});
    sleepMs(20);
  }
  CHECK_M(srv.hits.load() == 0, "打字期间就发起了请求(停顿判定失效)");

  // (c) 空白缓冲区即使停顿够久也不发
  TextBuffer blank;
  fillBuf(blank, {"", "   ", "\t"});
  sleepMs(c.ghost_delay_ms + 60);
  for (int i = 0; i < 20; ++i) svc.maybeAutoTrigger(blank, Pos{0, 0});
  sleepMs(60);
  CHECK_M(srv.hits.load() == 0, "空白缓冲区也发了请求");

  // (d) 停顿够久 -> 只发一个(连调 200 次也只有一个)
  svc.noteActivity();
  sleepMs(c.ghost_delay_ms + 60);
  for (int i = 0; i < 200; ++i) svc.maybeAutoTrigger(b, Pos{3, 5});
  std::vector<AppEvent> evs;
  CHECK(waitForKind(out, evs, EvKind::AiDone, 6000));
  sleepMs(120);
  drainInto(out, evs);
  CHECK_M(srv.hits.load() == 1, "自动触发重复发了请求:hits=" +
                                    std::to_string(srv.hits.load()));
  CHECK(countKind(evs, EvKind::AiStarted) == 1);
  CHECK(countKind(evs, EvKind::AiDone) == 1);

  // (e) 完成之后立刻再触发,受 ghost_min_interval_ms 限流
  for (int i = 0; i < 50; ++i) svc.maybeAutoTrigger(b, Pos{3, 5});
  sleepMs(150);
  CHECK_M(srv.hits.load() == 1, "ghost_min_interval_ms 限流失效");

  // (f) 手动 askNow 无视 min_interval
  svc.askNow(b, Pos{3, 5}, "");
  evs.clear();
  CHECK(waitForKind(out, evs, EvKind::AiDone, 6000));
  CHECK_M(srv.hits.load() == 2, "askNow 被 min_interval 挡住了");
  srv.shutdownServer();
}

// ---------------------------------------------------------------------------
// 8. Prompt 后处理
// ---------------------------------------------------------------------------

static void case_postprocess() {
  caseBegin("后处理:剥围栏 / 去重复前缀 / 截行 / 去尾部空行");
  using A = AiService;
  // 围栏
  CHECK(A::postprocessCompletion("```cpp\nint a = 1;\n```", "", 8) == "int a = 1;");
  CHECK(A::postprocessCompletion("```\nint a = 1;\nint b = 2;\n```\n", "", 8) ==
        "int a = 1;\nint b = 2;");
  // 围栏外的解释文字被丢掉
  CHECK(A::postprocessCompletion("这是补全:\n```cpp\nreturn 0;\n```\n希望有帮助", "", 8) ==
        "return 0;");
  // 只有开围栏没有闭围栏
  CHECK(A::postprocessCompletion("```cpp\nreturn 0;", "", 8) == "return 0;");
  // 围栏后紧跟空行
  CHECK(A::postprocessCompletion("```cpp\n\n\nreturn 0;\n\n```", "", 8) == "return 0;");
  // 行尾残留围栏
  CHECK(A::postprocessCompletion("return 0;```", "", 8) == "return 0;");
  // 无围栏时原样(只去尾部空行)
  CHECK(A::postprocessCompletion("return 0;\n\n\n", "", 8) == "return 0;");

  // 重复当前行前缀
  CHECK(A::postprocessCompletion("return 0;", "  ret", 8) == "urn 0;");
  CHECK(A::postprocessCompletion("  return 0;", "  ret", 8) == "urn 0;");
  CHECK(A::postprocessCompletion("```cpp\n  return 0;\n```", "  ret", 8) == "urn 0;");
  // 模型自己加了缩进而当前行前缀带空白
  CHECK(A::postprocessCompletion("      ret x;", "   ret", 8) == " x;");
  // 没重复就别乱剪
  CHECK(A::postprocessCompletion("urn 0;", "  ret", 8) == "urn 0;");
  CHECK(A::postprocessCompletion("int x;", "", 8) == "int x;");

  // 截断到 max_lines
  {
    std::string raw;
    for (int i = 0; i < 30; ++i) raw += "line" + std::to_string(i) + "\n";
    const std::string got = A::postprocessCompletion(raw, "", 8);
    CHECK(util::splitLines(got).size() == 8);
    CHECK(util::startsWith(got, "line0\n"));
    CHECK(util::endsWith(got, "line7"));
    // max_lines <= 0 视为不截断
    CHECK(util::splitLines(A::postprocessCompletion(raw, "", 0)).size() == 30);
  }
  // 尾部空行 / 全空白 / 空串
  CHECK(A::postprocessCompletion("a\n\n   \n\t\n", "", 8) == "a");
  CHECK(A::postprocessCompletion("\n\n   \n", "", 8).empty());
  CHECK(A::postprocessCompletion("", "abc", 8).empty());
  CHECK(A::postprocessCompletion("```\n```", "", 8).empty());
  // 组合:围栏 + 重复前缀 + 超行数 + 尾部空行
  {
    std::string raw = "```cpp\n  return solve();\nint x;\nint y;\nint z;\n\n\n```\n收工";
    const std::string got = A::postprocessCompletion(raw, "  ret", 3);
    CHECK(got == "urn solve();\nint x;\nint y;");
  }
  // 中文/UTF-8 不被截断成半个字
  CHECK(A::postprocessCompletion("// 处理边界\nreturn 0;\n", "", 8) ==
        "// 处理边界\nreturn 0;");
  CHECK(util::utf8Valid(A::postprocessCompletion("// 处理边界\n", "", 8)));
}

// ---------------------------------------------------------------------------
// 9. 上下文截断
// ---------------------------------------------------------------------------

static void case_context_truncation() {
  caseBegin("上下文截断:1000 行 + ai_max_context_lines=100");
  Mailbox<AppEvent> out;
  Config c = cfgFor("http://127.0.0.1:1", "");
  c.ai_max_context_lines = 100;
  AiService svc(c, &out);
  TextBuffer b;
  {
    std::vector<std::string> ls;
    for (int i = 0; i < 1000; ++i) ls.push_back("int v" + std::to_string(i) + " = " +
                                                std::to_string(i) + ";");
    fillBuf(b, ls);
  }
  CHECK(b.lineCount() == 1000);

  const AiRequest r = svc.buildRequest(b, Pos{500, 3}, "", AiMode::Code);
  CHECK(r.user_prompt.find(aiprompt::cursorMark()) != std::string::npos);
  CHECK(r.user_prompt.find("/* ...省略... */") != std::string::npos);
  // 光标在中间 -> 上下都有省略标记
  {
    size_t n = 0, at = 0;
    while ((at = r.user_prompt.find("/* ...省略... */", at)) != std::string::npos) {
      ++n;
      at += 4;
    }
    CHECK(n == 2);
  }
  const size_t lines = util::splitLines(r.user_prompt).size();
  CHECK_M(lines <= 104, "截断后的行数超了:" + std::to_string(lines));
  CHECK(lines >= 100);
  CHECK(r.user_prompt.size() < 8000);
  // 光标附近的行必须在,远处的不在(光标行被 <CURSOR> 切成两半)
  CHECK(r.user_prompt.find("int v499 = 499;") != std::string::npos);
  CHECK(r.user_prompt.find("int<CURSOR> v500 = 500;") != std::string::npos);
  CHECK(r.user_prompt.find("int v501 = 501;") != std::string::npos);
  CHECK(r.user_prompt.find("int v0 = 0;") == std::string::npos);
  CHECK(r.user_prompt.find("int v999 = 999;") == std::string::npos);
  CHECK(r.line_prefix == "int");

  // 光标在文件头:上面没有省略标记,下面有
  {
    const AiRequest r0 = svc.buildRequest(b, Pos{0, 0}, "", AiMode::Code);
    CHECK(!util::startsWith(r0.user_prompt, "/* ...省略... */"));
    CHECK(util::startsWith(r0.user_prompt, aiprompt::cursorMark()));
    CHECK(r0.user_prompt.find("/* ...省略... */") != std::string::npos);
    CHECK(util::splitLines(r0.user_prompt).size() <= 103);
    CHECK(r0.user_prompt.find("int v0 = 0;") != std::string::npos);
  }
  // 光标在文件尾
  {
    const AiRequest r1 = svc.buildRequest(b, b.endPos(), "", AiMode::Code);
    CHECK(util::endsWith(r1.user_prompt, aiprompt::cursorMark()));
    CHECK(r1.user_prompt.find("int v999 = 999;") != std::string::npos);
    CHECK(util::splitLines(r1.user_prompt).size() <= 103);
  }
  // 小文件:不截断、无省略标记、<CURSOR> 位置准确
  {
    TextBuffer s = sampleBuf();
    const AiRequest rs = svc.buildRequest(s, Pos{3, 5}, "", AiMode::Code);
    CHECK(rs.user_prompt.find("/* ...省略... */") == std::string::npos);
    CHECK(rs.user_prompt ==
          "#include <cstdio>\nint main() {\n  int n = 0;\n  ret<CURSOR>\n}");
    CHECK(rs.line_prefix == "  ret");
  }
  // 练习模式的 user_prompt 也带 <CURSOR> 与问题
  {
    const AiRequest rp = svc.buildRequest(b, Pos{500, 3}, "复杂度是多少", AiMode::Practice);
    CHECK(rp.user_prompt.find(aiprompt::cursorMark()) != std::string::npos);
    CHECK(rp.user_prompt.find("复杂度是多少") != std::string::npos);
    CHECK(rp.user_prompt.find("/* ...省略... */") != std::string::npos);
  }
  // buildContext 直接测:prefix 末尾即光标处
  {
    std::string pre, suf;
    AiService::buildContext(b, Pos{500, 5}, 100, pre, suf);
    CHECK(util::endsWith(pre, "int v"));
    CHECK(util::startsWith(suf, "500 = 500;"));
    // max_lines 极小也不崩
    AiService::buildContext(b, Pos{500, 5}, 1, pre, suf);
    CHECK(pre.find("int v") != std::string::npos);
    AiService::buildContext(b, Pos{500, 5}, 0, pre, suf);   // 0 = 不截断
    CHECK(util::splitLines(pre).size() == 501);
    // 越界光标被 clamp
    AiService::buildContext(b, Pos{99999, 99999}, 100, pre, suf);
    CHECK(!pre.empty());
    TextBuffer e;
    AiService::buildContext(e, Pos{0, 0}, 100, pre, suf);
    CHECK(pre.empty());
    CHECK(suf.empty());
  }
}

// ---------------------------------------------------------------------------
// 端到端:练习模式(PanelOnly + reasoning 追加)/ 写代码模式(GhostText + 后处理)
// ---------------------------------------------------------------------------

static void case_end_to_end_practice() {
  caseBegin("端到端(练习模式):每个事件 sink 恒为 PanelOnly,reasoning 被追加");
  MiniServer srv;
  srv.resp = sseResponse({"1) 想想单调队列", "2) O(n) 可行"}, {"先看数据范围"});
  CHECK(srv.start());
  Config c = cfgFor(srv.url());
  Mailbox<AppEvent> out;
  AiService svc(c, &out);
  svc.setMode(AiMode::Practice);
  TextBuffer b = sampleBuf();
  svc.askNow(b, Pos{3, 5}, "这题怎么想");
  std::vector<AppEvent> evs;
  CHECK_M(waitForKind(out, evs, EvKind::AiDone, 6000), "练习模式没有拿到 AiDone");
  sleepMs(80);
  drainInto(out, evs);
  CHECK(evs.size() >= 2);
  for (const AppEvent& e : evs) {
    CHECK_M(e.sink == AiSink::PanelOnly, "练习模式的事件出现了 GhostText sink");
    CHECK(e.gen == svc.generation());
  }
  const AppEvent* done = findKind(evs, EvKind::AiDone);
  CHECK(done != nullptr);
  CHECK(done->text.find("单调队列") != std::string::npos);
  CHECK(done->text.find("O(n) 可行") != std::string::npos);
  CHECK_M(done->text.find("先看数据范围") != std::string::npos,
          "练习模式没有追加 reasoning_content");
  CHECK(countKind(evs, EvKind::AiStarted) == 1);
  CHECK(countKind(evs, EvKind::AiError) == 0);
  // 请求体里带了硬后缀,而且没有 key
  const std::string req = srv.lastRequest();
  const size_t hdr_end = req.find("\r\n\r\n");
  CHECK(hdr_end != std::string::npos);
  const std::string body = req.substr(hdr_end + 4);
  CHECK(body.find("test-model-XYZ-9") != std::string::npos);
  CHECK_M(!leaksKey(body, kFakeKey), "请求体里出现了 api_key");
  CHECK(req.find("Authorization: Bearer") != std::string::npos);
  CHECK(std::string(svc.stateZh()) == "就绪");
  CHECK(svc.inFlight() == false);
  srv.shutdownServer();
}

static void case_end_to_end_code() {
  caseBegin("端到端(写代码模式):sink=GhostText,AiDone 是后处理结果,reasoning 被丢弃");
  MiniServer srv;
  // 模型给了围栏 + 重复了当前行前缀 + 尾部空行 + 思维链
  srv.resp = sseResponse({"```cpp\\n", "  ret", "urn 0;\\n", "\\n```\\n"}, {"思维链不该进缓冲区"});
  CHECK(srv.start());
  Config c = cfgFor(srv.url());
  Mailbox<AppEvent> out;
  AiService svc(c, &out);
  svc.setMode(AiMode::Code);
  TextBuffer b = sampleBuf();
  svc.askNow(b, Pos{3, 5}, "");
  std::vector<AppEvent> evs;
  CHECK_M(waitForKind(out, evs, EvKind::AiDone, 6000), "写代码模式没有拿到 AiDone");
  sleepMs(80);
  drainInto(out, evs);
  for (const AppEvent& e : evs) {
    CHECK(e.sink == AiSink::GhostText);
    CHECK(e.gen == svc.generation());
  }
  const AppEvent* done = findKind(evs, EvKind::AiDone);
  CHECK(done != nullptr);
  CHECK_M(done->text == "urn 0;",
          "AiDone 的文本不是后处理结果,而是:[" + done->text + "]");
  CHECK_M(done->text.find("思维链") == std::string::npos,
          "写代码模式没有丢弃 reasoning_content");
  CHECK(done->text.find("```") == std::string::npos);
  // delta 是原始增量(拼起来就是模型原文),后处理只体现在 AiDone
  std::string joined;
  for (const AppEvent& e : evs) {
    if (e.kind == EvKind::AiDelta) joined += e.text;
  }
  CHECK(joined == "```cpp\n  return 0;\n\n```\n");
  srv.shutdownServer();
}

static void case_backpressure() {
  caseBegin("背压:几百个微增量最多每 40ms 合成一个事件");
  MiniServer srv;
  std::vector<std::string> pieces;
  for (int i = 0; i < 200; ++i) pieces.push_back("x");
  srv.resp = sseResponse(pieces);
  srv.chunk = 40;              // 每 40 字节一写
  srv.chunk_delay_ms = 3;      // 拉长到 ~1s,给 40ms 窗口留出多次机会
  CHECK(srv.start());
  Config c = cfgFor(srv.url());
  c.ai_timeout_ms = 15000;
  Mailbox<AppEvent> out;
  AiService svc(c, &out);
  svc.setMode(AiMode::Code);
  TextBuffer b = sampleBuf();
  const int64_t t0 = nowMs();
  svc.askNow(b, Pos{3, 5}, "");
  std::vector<AppEvent> evs;
  CHECK(waitForKind(out, evs, EvKind::AiDone, 20000));
  sleepMs(60);
  drainInto(out, evs);
  const int64_t elapsed = nowMs() - t0;
  const int deltas = countKind(evs, EvKind::AiDelta);
  std::printf("       %lld ms 内 200 个微增量 -> %d 个 AiDelta 事件\n",
              static_cast<long long>(elapsed), deltas);
  CHECK(deltas >= 1);
  const int budget = static_cast<int>(elapsed / 40) + 3;
  CHECK_M(deltas <= budget, "delta 事件数超过 40ms 预算:" + std::to_string(deltas) +
                                " > " + std::to_string(budget));
  CHECK_M(deltas < 200, "delta 完全没有合并");
  std::string joined;
  for (const AppEvent& e : evs) {
    if (e.kind == EvKind::AiDelta) joined += e.text;
  }
  CHECK_M(joined == std::string(200, 'x'), "合并后的 delta 丢字节了");
  const AppEvent* done = findKind(evs, EvKind::AiDone);
  CHECK(done != nullptr && done->text == std::string(200, 'x'));
  srv.shutdownServer();
}

static void case_non_stream_and_misc() {
  caseBegin("非流式回退 / 模式名 / 状态文案 / 空 out_ 指针");
  MiniServer srv;
  srv.resp = httpBody200(
      "{\"choices\":[{\"message\":{\"content\":\"int s = 0;\"}}],"
      "\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":7}}");
  CHECK(srv.start());
  Config c = cfgFor(srv.url());
  c.stream = false;
  Mailbox<AppEvent> out;
  AiService svc(c, &out);
  svc.setMode(AiMode::Code);
  TextBuffer b = sampleBuf();
  svc.askNow(b, Pos{3, 5}, "");
  std::vector<AppEvent> evs;
  CHECK_M(waitForKind(out, evs, EvKind::AiDone, 6000), "非流式路径没有 AiDone");
  const AppEvent* done = findKind(evs, EvKind::AiDone);
  CHECK(done != nullptr && done->text == "int s = 0;");
  CHECK(done->sink == AiSink::GhostText);
  srv.shutdownServer();

  CHECK(std::string(aiModeZh(AiMode::Practice)) == "练习模式");
  CHECK(std::string(aiModeZh(AiMode::Code)) == "写代码模式");
  CHECK(std::string(aiprompt::cursorMark()) == "<CURSOR>");
  CHECK(hasNonAscii(aiprompt::defaultCodeSystem()));
  CHECK(hasNonAscii(aiprompt::defaultPracticeSystem()));
  CHECK(std::string(aiprompt::practiceHardSuffix()) ==
        "再次强调:不要输出任何可直接编译运行的代码。");
  // 练习模式内置 system 明确禁止输出代码
  CHECK(std::string(aiprompt::defaultPracticeSystem()).find("不要输出") !=
        std::string::npos);

  // out_ == nullptr 也不能崩(main.cpp 万一忘了传)
  {
    Config c2 = cfgFor("http://127.0.0.1:1", "");
    AiService svc2(c2, nullptr);
    TextBuffer b2 = sampleBuf();
    svc2.askNow(b2, Pos{3, 5}, "");
    svc2.maybeAutoTrigger(b2, Pos{3, 5});
    CHECK(std::string(svc2.stateZh()) == "未配置");
  }
  {
    Config c3 = cfgFor("http://127.0.0.1:1");
    AiService svc3(c3, nullptr);
    TextBuffer b3 = sampleBuf();
    svc3.setMode(AiMode::Code);
    svc3.askNow(b3, Pos{3, 5}, "");
    CHECK(waitInFlight(svc3, false, 4000));
  }
  // 模式切换与状态文案
  {
    Config c4 = cfgFor("http://127.0.0.1:1");
    Mailbox<AppEvent> o4;
    AiService svc4(c4, &o4);
    CHECK(svc4.mode() == AiMode::Practice);     // 默认是更安全的那一档
    CHECK(svc4.toggleMode() == AiMode::Code);
    CHECK(svc4.mode() == AiMode::Code);
    CHECK(svc4.toggleMode() == AiMode::Practice);
    const uint64_t g0 = svc4.generation();
    svc4.noteActivity();
    CHECK(svc4.generation() == g0 + 1);
    svc4.cancelInFlight();
    CHECK(svc4.generation() == g0 + 2);
    CHECK(std::string(svc4.stateZh()) == "就绪");
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  // 看门狗:整份测试绝不可能永久挂住(SIGALRM 默认动作 = 杀进程,退出码非 0)。
  ::alarm(240);
  ::signal(SIGPIPE, SIG_IGN);   // 服务端写半关闭的连接不许打死进程

  std::printf("test_ai: 开始\n");
  HttpChat::globalInit();       // 本测试自己负责(正式程序里由 main.cpp 调)

  const int64_t t0 = nowMs();
  case_sink_decision_point();
  case_source_has_no_buffer_writes();
  case_disabled_no_worker_thread();
  case_postprocess();
  case_context_truncation();
  case_trigger_and_rate_limit();
  case_cancel_semantics();
  case_ui_never_blocks();
  case_end_to_end_practice();
  case_end_to_end_code();
  case_backpressure();
  case_non_stream_and_misc();
  case_error_paths_local();
  case_error_paths_network();
  case_destruct_while_inflight();

  HttpChat::globalCleanup();
  std::printf("test_ai: 全部通过 —— %d cases / %d checks / %d skipped / %lld ms\n",
              g_cases, g_checks, g_skipped, static_cast<long long>(nowMs() - t0));
  std::printf("test_ai: UI 线程调用最坏耗时 %lld us\n", static_cast<long long>(g_max_ui_us));
  return 0;
}

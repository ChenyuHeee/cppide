// aihttp.cpp —— HTTP/SSE 层实现(libcurl 封装)
//
// 本文件的三条硬性约束(需求.md + architecture.md §5.6):
//   1. send() **一定会返回**:CURLOPT_TIMEOUT_MS / CONNECTTIMEOUT_MS /
//      LOW_SPEED_LIMIT+TIME 三重保险,任何一条命中都会让 curl_easy_perform 收敛。
//      主循环因此永远不会被 AI 请求卡死。
//   2. **api_key 绝不出现在任何返回的字符串里**:CURLOPT_VERBOSE 恒为 0,
//      key 只走 Authorization 头;所有对外字符串(error/text)出门前统一过
//      scrubKey() 再洗一遍(即使理论上 body 里不会有 key,也留这道过滤)。
//   3. **没有任何硬编码 key,也没有写死的模型名**:model 为空直接报错返回,
//      绝不悄悄替换成某个默认模型。
//
// reasoning_content 的归属(架构 §5.6 + aihttp.h:50):
//   本层**不认识 AiMode**,所以不做"练习模式才追加"的策略判断。
//   做法是把思维链单独放进 ChatResponse::reasoning,并且**绝不**经由
//   on_delta 下发(on_delta 只喂正式 content)。这样上层 AiService:
//     - 练习模式(PanelOnly)可以取 resp.reasoning 追加到面板;
//     - 写代码模式只用 resp.text / on_delta 的 delta,思维链自然被丢弃,
//       不可能被当成代码插进缓冲区。
#include "aihttp.h"

#include <curl/curl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "json.h"
#include "util.h"

// ---------------------------------------------------------------- 小工具

namespace {

// 单帧 / 单响应体的内存上限。SSE 流理论上可以永远不发空行,
// 没有上限的话 carry_ 会无限增长。32MiB 远大于任何真实的 chat 响应。
constexpr size_t kMaxCarry = 32u * 1024u * 1024u;
// 非流式响应体上限(超过就不再累积,避免 OOM)。
constexpr size_t kMaxBody = 32u * 1024u * 1024u;
// 流式模式下为了诊断保留的 body 前缀长度。
constexpr size_t kKeepPrefix = 4096;
// 错误信息里附带的 body 片段长度(§5.6:前 200 字节)。
constexpr size_t kErrSnippet = 200;

// 把 key 从任意对外字符串里抹掉(最后一道防线)。
// 不只匹配完整 key:服务端有时会回显被截断的 key(实测 DeepSeek 的 401 会回
// "Your api key: ****xxxx is invalid"),所以把 key 的**任意 8 字节连续片段**
// 都当成敏感内容替换掉。8 字节窗口不会误伤正常文案(概率上不可能撞上),
// 而 key 短于 8 字节时不做替换,免得把普通文本打成马赛克。
// 注意:服务端自己打码后剩下的 4 字符尾巴(它设计如此)不在本函数职责内 ——
// 完整 key 与其任何 8 字节片段都不会出现,足以保证 key 无法被复原。
std::string scrubKey(std::string s, const std::string& key) {
  if (key.size() < 8 || s.empty()) return s;
  s = util::replaceAll(std::move(s), key, "[REDACTED]");
  constexpr size_t kWin = 8;
  for (size_t i = 0; i + kWin <= key.size(); ++i) {
    s = util::replaceAll(std::move(s), key.substr(i, kWin), "[REDACTED]");
  }
  return s;
}

// 错误文案要进状态栏(单行),把控制字符压成空格。
std::string oneLine(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool prev_space = false;
  for (unsigned char c : s) {
    if (c == '\n' || c == '\r' || c == '\t' || c < 0x20 || c == 0x7f) {
      if (!prev_space && !out.empty()) out += ' ';
      prev_space = true;
    } else {
      out += static_cast<char>(c);
      prev_space = false;
    }
  }
  return util::trim(out);
}

// body 的诊断片段:截断 + 单行化(UTF-8 安全,clipBytes 不会切碎多字节)。
std::string bodySnippet(const std::string& body) {
  return oneLine(util::clipBytes(body, kErrSnippet));
}

// 首个非空白字节是否为 '{' / '['(§5.6:据此判定 body 是普通 JSON 而非 SSE)。
bool looksLikeJsonBody(const std::string& s) {
  for (unsigned char c : s) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    return c == '{' || c == '[';
  }
  return false;  // 全是空白:还看不出来,先按 SSE 走
}

// 从 mj 值里安全取字符串(越界/类型不符返回空串,json.h 保证不抛)。
const std::string& str(const mj::Value& v) { return v.asString(); }

}  // namespace

// ---------------------------------------------------------------- ChatRequest

// 生成 chat/completions 请求体。
// model 一律来自入参 req.model(**不写死任何模型名**);api_key 不出现在 body 里
// (它只走 Authorization 头)。转义交给 mj::Value::dump,中文原样透传 UTF-8。
std::string ChatRequest::toJson() const {
  mj::Value root = mj::Value::object();
  root.set("model", model);

  mj::Value msgs = mj::Value::array();
  for (const ChatMessage& m : messages) {
    mj::Value o = mj::Value::object();
    o.set("role", m.role);
    o.set("content", m.content);
    msgs.push(std::move(o));
  }
  root.set("messages", std::move(msgs));

  root.set("stream", stream);
  root.set("temperature", temperature);
  if (max_tokens > 0) root.set("max_tokens", max_tokens);
  if (!stop.empty()) {
    mj::Value arr = mj::Value::array();
    for (const std::string& s : stop) arr.push(mj::Value(s));
    root.set("stop", std::move(arr));
  }
  return root.dump();
}

// ---------------------------------------------------------------- SseParser
//
// 关键性质(test_sse.cpp 逐条验):**输出与分片方式无关**。
// 实现手段:carry_ 里只留"还没凑成完整帧"的原始字节,
// 只有看到空行(帧结束)才解析并回调。因此不管字节流被怎么切
// (哪怕切在 "data:" 关键字中间、切在 UTF-8 多字节中间),
// 每次 feed 后的内部状态都只是"已消费的字节前缀"的函数。
//
// 帧内按 SSE 规范处理:多条 data: 行用 '\n' 拼接;
// event:/id:/retry: 等字段忽略;以 ':' 开头的注释行忽略;
// 没有 "data:" 前缀的垃圾行忽略。行尾兼容 "\n" 与 "\r\n"。

namespace {

// 去掉一行末尾的 '\r'。
inline void stripCr(std::string& line) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
}

// 解析一个帧块(不含结尾空行),把拼好的 data 载荷交给 cb。
void dispatchFrame(const std::string& block,
                   const std::function<void(const std::string&)>& cb) {
  std::string payload;
  bool have_data = false;
  size_t pos = 0;
  while (pos <= block.size()) {
    size_t nl = block.find('\n', pos);
    std::string line = (nl == std::string::npos) ? block.substr(pos)
                                                 : block.substr(pos, nl - pos);
    pos = (nl == std::string::npos) ? block.size() + 1 : nl + 1;
    stripCr(line);
    if (line.empty()) continue;
    if (line[0] == ':') continue;  // 注释行
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;  // 无字段名的垃圾行
    std::string field = line.substr(0, colon);
    if (field != "data") continue;             // event:/id:/retry:/其它 → 忽略
    std::string val = line.substr(colon + 1);
    // 规范:字段值前恰好一个空格要去掉(多余的空格属于数据)。
    if (!val.empty() && val[0] == ' ') val.erase(0, 1);
    if (have_data) payload += '\n';
    payload += val;
    have_data = true;
  }
  if (have_data) cb(payload);
}

// 在 s 中从 from 起找"空行"(即帧结束)。返回帧内容长度与终止符长度。
// 支持 "\n\n" / "\r\n\r\n" / "\n\r\n" / "\r\n\n"。
bool findFrameEnd(const std::string& s, size_t& frame_len, size_t& term_len) {
  size_t pos = 0;
  while (true) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos) return false;
    // 这一行的内容是 [pos, nl),去掉可能的 '\r'
    size_t end = nl;
    if (end > pos && s[end - 1] == '\r') --end;
    if (end == pos) {  // 空行 → 帧在 pos 之前结束
      frame_len = pos;
      term_len = nl + 1 - pos;
      return true;
    }
    pos = nl + 1;
  }
}

}  // namespace

void SseParser::feed(const char* p, size_t n,
                     const std::function<void(const std::string& payload)>& cb) {
  if (p != nullptr && n > 0) carry_.append(p, n);

  size_t frame_len = 0, term_len = 0;
  while (findFrameEnd(carry_, frame_len, term_len)) {
    std::string block = carry_.substr(0, frame_len);
    carry_.erase(0, frame_len + term_len);
    if (cb) dispatchFrame(block, cb);
  }
  // 保险:服务端一直不发空行时别把内存吃光。真实响应远小于 32MiB,
  // 所以正常路径永远不会走到这里(触发后丢弃残帧,保证进程活着)。
  if (carry_.size() > kMaxCarry) carry_.clear();
}

void SseParser::reset() { carry_.clear(); }

void SseParser::finish(const std::function<void(const std::string& payload)>& cb) {
  // 流结束时最后一帧可能没有以空行结尾(服务端直接关连接),补吐出来。
  if (!carry_.empty()) {
    std::string block = carry_;
    carry_.clear();
    if (cb) dispatchFrame(block, cb);
  }
}

// ---------------------------------------------------------------- 协议解析

bool sseChunkToDelta(const std::string& payload, std::string& content,
                     std::string& reasoning, bool& done) {
  content.clear();
  reasoning.clear();
  done = false;

  const std::string t = util::trim(payload);
  if (t.empty()) return false;
  if (t == "[DONE]") {
    done = true;
    return true;
  }
  std::string err;
  mj::Value v = mj::Value::parse(t, err);
  if (!err.empty() || !v.isObject()) return false;  // 非法 JSON 帧:跳过不崩

  const mj::Value& ch0 = v["choices"][0];
  const mj::Value& delta = ch0["delta"];
  const mj::Value& msg = ch0["message"];  // 少数实现在流里也用 message
  content = str(delta["content"]);
  if (content.empty()) content = str(msg["content"]);
  reasoning = str(delta["reasoning_content"]);
  if (reasoning.empty()) reasoning = str(msg["reasoning_content"]);

  // 结构上认得(有 choices 数组)就算成功 —— role-only 的首帧、
  // 只带 finish_reason 的末帧都是合法帧,不该被当成协议不匹配报给用户。
  if (v["choices"].isArray()) return true;
  return !content.empty() || !reasoning.empty();
}

bool chatBodyToText(const std::string& body, std::string& text,
                    std::string& reasoning, int& prompt_tokens,
                    int& completion_tokens) {
  text.clear();
  reasoning.clear();
  prompt_tokens = 0;
  completion_tokens = 0;

  std::string err;
  mj::Value v = mj::Value::parse(body, err);
  if (!err.empty() || !v.isObject()) return false;

  const mj::Value& ch0 = v["choices"][0];
  text = str(ch0["message"]["content"]);
  if (text.empty()) text = str(ch0["delta"]["content"]);
  reasoning = str(ch0["message"]["reasoning_content"]);
  if (reasoning.empty()) reasoning = str(ch0["delta"]["reasoning_content"]);

  const mj::Value& usage = v["usage"];
  prompt_tokens = usage["prompt_tokens"].asInt(0);
  completion_tokens = usage["completion_tokens"].asInt(0);

  // body 是错误对象({"error":{...}})时没有 choices → 让调用方走错误路径。
  if (!v["choices"].isArray()) return false;
  return !text.empty() || !reasoning.empty() || v["choices"].size() > 0;
}

std::string curlErrorZh(int curl_code) {
  switch (curl_code) {
    case CURLE_OK:
      return std::string();
    case CURLE_OPERATION_TIMEDOUT:
      return "请求超时";
    case CURLE_COULDNT_RESOLVE_HOST:
      return "无法解析域名,检查网络或 base_url";
    case CURLE_COULDNT_RESOLVE_PROXY:
      return "无法解析代理域名,检查 http_proxy 设置";
    case CURLE_COULDNT_CONNECT:
      return "无法连接服务器,检查网络或 base_url";
    case CURLE_SSL_CONNECT_ERROR:
      return "TLS 连接失败";
    case CURLE_PEER_FAILED_VERIFICATION:
      return "TLS 证书校验失败";
    case CURLE_SSL_CACERT_BADFILE:
      return "找不到可用的 CA 证书";
    case CURLE_ABORTED_BY_CALLBACK:
      return "已取消";
    case CURLE_WRITE_ERROR:
      return "已取消";  // 我们只在取消时从写回调返回短计数
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT:
      return "base_url 非法或协议不支持";
    case CURLE_GOT_NOTHING:
      return "服务端没有返回任何数据";
    case CURLE_RECV_ERROR:
      return "接收数据失败,连接可能被中断";
    case CURLE_SEND_ERROR:
      return "发送数据失败,连接可能被中断";
    case CURLE_PARTIAL_FILE:
      return "响应被截断";
    case CURLE_TOO_MANY_REDIRECTS:
      return "重定向次数过多,检查 base_url";
    case CURLE_OUT_OF_MEMORY:
      return "内存不足";
    default: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "网络错误(curl 错误码 %d)", curl_code);
      return std::string(buf);
    }
  }
}

std::string chatErrorZh(long http_status, int curl_code,
                        const std::string& body) {
  // curl 层就失败了(连不上/超时/TLS):HTTP 状态码没有意义。
  if (curl_code != CURLE_OK) return curlErrorZh(curl_code);

  std::string head;
  if (http_status == 401 || http_status == 403) {
    head = "API key 无效或已过期";
  } else if (http_status == 402 || http_status == 429) {
    head = "余额不足或请求过于频繁";
  } else if (http_status >= 500 && http_status <= 599) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "服务端错误(HTTP %ld)", http_status);
    head = buf;
  } else if (http_status == 404) {
    head = "接口不存在(HTTP 404),检查 base_url 与 chat_path";
  } else if (http_status == 400 || http_status == 422) {
    head = "请求被拒绝(HTTP " + std::to_string(http_status) + "),检查模型名与参数";
  } else if (http_status >= 400) {
    head = "请求失败(HTTP " + std::to_string(http_status) + ")";
  } else if (http_status == 0) {
    head = "请求失败";
  } else {
    head = "响应异常(HTTP " + std::to_string(http_status) + ")";
  }

  // 尽量把服务端给的 error.message 附上(§5.6);解析不出来就贴 body 前 200 字节。
  std::string err;
  mj::Value v = mj::Value::parse(body, err);
  std::string detail;
  if (err.empty() && v.isObject()) {
    detail = str(v["error"]["message"]);
    if (detail.empty()) detail = str(v["error"]["msg"]);
    if (detail.empty()) detail = str(v["message"]);
    if (detail.empty() && v["error"].isString()) detail = str(v["error"]);
  }
  if (detail.empty() && !util::trim(body).empty()) detail = bodySnippet(body);
  if (!detail.empty()) head += ":" + oneLine(detail);
  return head;
}

// ---------------------------------------------------------------- HttpChat

namespace {

// 一次 send() 的传输上下文。裸指针成员都指向 send() 栈上的对象,
// 生命周期严格短于 curl_easy_perform 调用,不会悬垂。
struct Xfer {
  const ChatRequest* req = nullptr;
  const ChatDeltaFn* on_delta = nullptr;
  const ChatCancelFn* should_cancel = nullptr;

  SseParser parser;
  bool sse_mode = false;        // 走 SSE 解析还是当整块 JSON
  bool mode_decided = false;
  std::string body;             // 原始 body(SSE 模式只留前缀做诊断)
  size_t body_bytes = 0;        // 收到的总字节数(不受截断影响)
  std::string text;             // 拼好的 content
  std::string reasoning;        // 思维链,单独存,不下发 on_delta
  bool saw_done = false;
  bool canceled = false;        // should_cancel 或 on_delta 要求中止
  bool bad_frame = false;       // 出现过解析不了的帧
  std::string first_bad;        // 第一个坏帧原文(诊断用)

  bool cancelRequested() const {
    return should_cancel != nullptr && *should_cancel != nullptr &&
           (*should_cancel)();
  }
};

// 进度回调:**这是"用户继续打字 → 在飞请求立即作废"的实现点**。
// curl 在连接/DNS/传输的各阶段都会周期性调用它,非 0 返回值 →
// 立刻中止,curl_easy_perform 返回 CURLE_ABORTED_BY_CALLBACK。
int xferInfoCb(void* userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  Xfer* x = static_cast<Xfer*>(userp);
  if (x == nullptr) return 0;
  if (x->canceled) return 1;
  if (x->cancelRequested()) {
    x->canceled = true;
    return 1;  // → CURLE_ABORTED_BY_CALLBACK
  }
  return 0;
}

// 一帧 SSE 载荷 → 累积 + 下发 delta。返回 false 表示上层要求中止。
bool handleFrame(Xfer* x, const std::string& payload) {
  std::string content, reasoning;
  bool done = false;
  if (!sseChunkToDelta(payload, content, reasoning, done)) {
    if (!x->bad_frame) {
      x->bad_frame = true;
      x->first_bad = payload;
    }
    return true;  // 坏帧跳过,不崩、不中断整个流
  }
  if (done) {
    x->saw_done = true;
    return true;
  }
  if (!reasoning.empty()) x->reasoning += reasoning;  // 只入 resp.reasoning
  if (!content.empty()) {
    x->text += content;
    if (x->on_delta != nullptr && *x->on_delta != nullptr) {
      if (!(*x->on_delta)(content)) return false;  // 上层要求中止
    }
  }
  return true;
}

// 写回调。检测到取消时**返回短计数**以中止传输(§5.6)。
size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userp) {
  Xfer* x = static_cast<Xfer*>(userp);
  const size_t n = size * nmemb;
  if (x == nullptr) return n;
  if (x->canceled) return 0;
  if (x->cancelRequested()) {
    x->canceled = true;
    return 0;  // 短计数 → CURLE_WRITE_ERROR,立刻中止
  }
  if (n == 0) return 0;

  x->body_bytes += n;
  if (!x->mode_decided) {
    // §5.6 兼容性兜底:4xx 的 body 往往是普通 JSON 错误对象而不是 SSE。
    // 按首个非空白字节判定;非流式请求一律当整块 JSON。
    std::string head(ptr, std::min(n, static_cast<size_t>(64)));
    bool json_body = looksLikeJsonBody(head);
    if (!json_body && x->req != nullptr && !x->req->stream) json_body = true;
    x->sse_mode = !json_body;
    x->mode_decided = true;
  }

  if (x->sse_mode) {
    if (x->body.size() < kKeepPrefix) {  // 只留前缀,够诊断就行
      x->body.append(ptr, std::min(n, kKeepPrefix - x->body.size()));
    }
    bool keep_going = true;
    x->parser.feed(ptr, n, [&](const std::string& payload) {
      if (keep_going && !handleFrame(x, payload)) keep_going = false;
    });
    if (!keep_going) {
      x->canceled = true;
      return 0;  // 上层 on_delta 返回 false → 中止
    }
  } else {
    if (x->body.size() + n <= kMaxBody) x->body.append(ptr, n);
  }
  return n;
}

}  // namespace

void HttpChat::globalInit() {
  // 必须在任何线程启动之前调用一次(curl_global_init 不是线程安全的)。
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

void HttpChat::globalCleanup() { curl_global_cleanup(); }

HttpChat::HttpChat() { curl_ = curl_easy_init(); }

HttpChat::~HttpChat() {
  if (curl_ != nullptr) {
    curl_easy_cleanup(static_cast<CURL*>(curl_));
    curl_ = nullptr;
  }
}

ChatResponse HttpChat::send(const ChatRequest& req, const ChatDeltaFn& on_delta,
                            const ChatCancelFn& should_cancel) {
  ChatResponse resp;
  const int64_t t0 = util::nowMs();
  sse_carry_.clear();  // 每次请求都从干净状态开始,不跨请求残留半帧

  auto finish_time = [&]() { resp.elapsed_ms = util::nowMs() - t0; };
  // 出门前统一洗一遍:api_key 绝不出现在任何返回的字符串里。
  auto scrub_all = [&]() {
    resp.error = scrubKey(std::move(resp.error), req.api_key);
    resp.text = scrubKey(std::move(resp.text), req.api_key);
    resp.reasoning = scrubKey(std::move(resp.reasoning), req.api_key);
  };

  // 0) 请求发出前就已经被取消 → 立刻返回。取消**不是错误**,error 保持为空。
  //    (这条让"用户继续打字"的作废路径不依赖 curl 是否已回调过进度函数。)
  if (should_cancel != nullptr && should_cancel()) {
    resp.canceled = true;
    resp.ok = false;
    finish_time();
    return resp;
  }

  // 1) 参数校验。**model 为空就报错,绝不替换成写死的默认模型名。**
  if (req.url.empty()) {
    resp.error = "未配置 base_url";
  } else if (req.api_key.empty()) {
    resp.error = "未配置 API key(在配置文件里填 api_key)";
  } else if (req.model.empty()) {
    resp.error = "未配置模型名(在配置文件里填 model)";
  } else if (curl_ == nullptr) {
    resp.error = "curl 初始化失败";
  }
  if (!resp.error.empty()) {
    finish_time();
    scrub_all();
    return resp;
  }

  CURL* h = static_cast<CURL*>(curl_);
  curl_easy_reset(h);  // 复用句柄(连接池还在),但清掉上一次的所有选项

  Xfer x;
  x.req = &req;
  x.on_delta = &on_delta;
  x.should_cancel = &should_cancel;
  x.sse_mode = req.stream;

  const std::string payload = req.toJson();  // 生命周期覆盖 perform

  // ---- header:key 只走这里,不进 body、不进 URL、不进日志 ----
  curl_slist* hdrs = nullptr;
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "Expect:");  // 关掉 100-continue 的一次往返
  {
    std::string auth = "Authorization: Bearer " + req.api_key;
    hdrs = curl_slist_append(hdrs, auth.c_str());
    // auth 在这里离开作用域:curl_slist_append 已经拷走了内容。
  }
  if (req.stream) hdrs = curl_slist_append(hdrs, "Accept: text/event-stream");
  else hdrs = curl_slist_append(hdrs, "Accept: application/json");

  char errbuf[CURL_ERROR_SIZE];
  errbuf[0] = '\0';

  // ---- §5.6 逐条选项 ----
  curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(h, CURLOPT_POST, 1L);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(payload.size()));
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &x);

  // 多线程程序里 NOSIGNAL 是**强制**的:否则 curl 的 DNS 超时走
  // SIGALRM + siglongjmp,在非主线程上会破坏状态甚至崩溃。
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);

  const long total_ms = req.total_timeout_ms > 0 ? req.total_timeout_ms : 20000;
  long conn_ms = req.connect_timeout_ms > 0 ? req.connect_timeout_ms : 5000;
  if (conn_ms > total_ms) conn_ms = total_ms;  // 连接超时不该超过总超时
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, total_ms);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, conn_ms);
  // 卡死的流会自己死掉:10 秒内平均速率 < 1 B/s 就放弃。
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 10L);
  // 已知限制(实测,写进 README 的候选):libcurl 用线程化解析器时,
  // Curl_resolver_kill() 会 **join 解析线程**,因此 CURLOPT_TIMEOUT_MS 打不断
  // 一次正在进行的 getaddrinfo —— send() 的实际下限是系统 DNS 的解析耗时
  // (本机冷缓存实测 ~3.2s,热缓存 ~2ms)。这不影响两条硬性保证:
  //   * send() 一定会返回(系统解析器自身有超时);
  //   * send() 只在 worker 线程跑,UI 线程永不阻塞、输入不卡。
  // 句柄复用带来的 DNS 缓存(默认 60s)让后续请求不再付这笔钱。
  curl_easy_setopt(h, CURLOPT_DNS_CACHE_TIMEOUT, 300L);

  // 进度回调 = 取消点。NOPROGRESS 必须为 0,否则回调根本不会被调用。
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferInfoCb);
  curl_easy_setopt(h, CURLOPT_XFERINFODATA, &x);

  // 恒为 0:调试输出会把 Authorization 头打到 stderr。
  curl_easy_setopt(h, CURLOPT_VERBOSE, 0L);
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);

  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_USERAGENT, "cppide/1.0");
  // 不设 FAILONERROR:4xx 的 body 正是错误信息的来源。
  // 不动 SSL_VERIFY*:保持 libcurl 默认(校验开),对 OpenSSL/GnuTLS/
  // SecureTransport 各后端行为一致,不依赖任何后端特有选项。

  const CURLcode rc = curl_easy_perform(h);

  long status = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
  resp.http_status = status;

  curl_slist_free_all(hdrs);
  hdrs = nullptr;
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, nullptr);  // errbuf 即将离开作用域
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, nullptr);

  // 流末尾可能有一个没以空行结尾的帧,补吐。
  if (x.sse_mode && rc == CURLE_OK) {
    x.parser.feed(nullptr, 0, [&](const std::string& p) { handleFrame(&x, p); });
    x.parser.finish([&](const std::string& p) { handleFrame(&x, p); });
  }
  x.parser.reset();

  resp.reasoning = x.reasoning;

  // ---- 取消:**不是错误**,不给用户报错 ----
  if (rc == CURLE_ABORTED_BY_CALLBACK ||
      (rc == CURLE_WRITE_ERROR && x.canceled) || x.canceled) {
    resp.canceled = true;
    resp.ok = false;
    resp.error.clear();
    resp.text = x.text;  // 已经收到的部分内容原样交回,上层自行决定丢不丢
    finish_time();
    scrub_all();
    return resp;
  }

  if (rc == CURLE_OPERATION_TIMEDOUT) {
    resp.timed_out = true;
    resp.ok = false;
    resp.error = curlErrorZh(rc);
    finish_time();
    scrub_all();
    return resp;
  }

  if (rc != CURLE_OK) {
    resp.ok = false;
    resp.error = chatErrorZh(status, static_cast<int>(rc), std::string());
    finish_time();
    scrub_all();
    return resp;
  }

  // ---- HTTP 层错误:body 通常是 {"error":{"message":...}} ----
  if (status < 200 || status >= 300) {
    resp.ok = false;
    resp.error = chatErrorZh(status, CURLE_OK, x.body);
    finish_time();
    scrub_all();
    return resp;
  }

  // ---- 2xx ----
  if (x.sse_mode) {
    resp.text = x.text;
    if (!resp.text.empty() || x.saw_done) {
      resp.ok = true;
    } else if (looksLikeJsonBody(x.body)) {
      // 说了 stream 却回了整块 JSON:按非流式再试一次(只有前缀,可能不全)。
      std::string t, r;
      int pt = 0, ct = 0;
      if (chatBodyToText(x.body, t, r, pt, ct)) {
        resp.text = t;
        if (!r.empty()) resp.reasoning = r;
        resp.prompt_tokens = pt;
        resp.completion_tokens = ct;
        resp.ok = true;
        if (!resp.text.empty() && on_delta != nullptr) on_delta(resp.text);
      }
    }
    if (!resp.ok) {
      // 协议不匹配:把 body 前 200 字节(已抹 key)附上,便于无调试器时诊断。
      resp.error = "无法解析 AI 响应(协议不匹配)";
      const std::string& diag = x.bad_frame ? x.first_bad : x.body;
      if (!util::trim(diag).empty()) resp.error += ":" + bodySnippet(diag);
      else if (x.body_bytes == 0) resp.error = "服务端返回空响应";
    }
  } else {
    std::string t, r;
    int pt = 0, ct = 0;
    if (chatBodyToText(x.body, t, r, pt, ct)) {
      resp.text = t;
      if (!r.empty()) resp.reasoning = r;
      resp.prompt_tokens = pt;
      resp.completion_tokens = ct;
      resp.ok = true;
      // 非流式:只在末尾回调一次(aihttp.h:100 的契约)。
      if (!resp.text.empty() && on_delta != nullptr) on_delta(resp.text);
    } else {
      resp.ok = false;
      resp.error = "无法解析 AI 响应(协议不匹配)";
      // body 本身可能就是错误对象,优先用它的 error.message。
      const std::string alt = chatErrorZh(status, CURLE_OK, x.body);
      if (!alt.empty() && alt.find(':') != std::string::npos) resp.error = alt;
      else if (!util::trim(x.body).empty()) resp.error += ":" + bodySnippet(x.body);
      else resp.error = "服务端返回空响应";
    }
  }

  // token 用量:流式响应的末帧可能带 usage,这里不强求(上层不依赖)。
  finish_time();
  scrub_all();
  return resp;
}

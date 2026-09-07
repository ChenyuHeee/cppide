// aihttp.h —— HTTP/SSE 层(libcurl 封装)
//
// 边界:本层只懂 HTTP 与 OpenAI 兼容的 chat/completions 协议,**不懂编辑器**。
// 它不认识 TextBuffer、不认识 AiMode/AiSink,也不决定回复能变成什么。
//
// 头文件里刻意不 include <curl/curl.h>:CURL* 用 void* 存,
// 免得 curl 的宏与类型污染每个 TU(编译更快,也避免与 ncurses 的宏打架)。
//
// 线程契约:HttpChat **每线程一个实例**,复用同一个 CURL* 句柄。
// send() 是阻塞的,只在 AiService 的 worker 线程调用。
// globalInit() 必须在任何线程启动之前于 main() 里调用一次。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ChatMessage {
  std::string role;      // "system" / "user" / "assistant"
  std::string content;
};

struct ChatRequest {
  std::string url, api_key, model;
  std::vector<ChatMessage> messages;
  double temperature = 0.2;
  int    max_tokens  = 256;
  bool   stream      = true;
  int    connect_timeout_ms = 5000;
  int    total_timeout_ms   = 20000;
  std::vector<std::string> stop;
  std::string toJson() const;             // 用 mj::Value 生成,自动转义
};

// 增量回调:返回 false 表示上层要求中止传输。
using ChatDeltaFn  = std::function<bool(const std::string& delta)>;
// 取消轮询:由 curl 的进度回调每秒调用数次,返回 true 即刻中止(CURLE_ABORTED_BY_CALLBACK)。
using ChatCancelFn = std::function<bool()>;

struct ChatResponse {
  bool ok = false;
  std::string text;              // 拼接后的完整内容
  std::string error;             // 中文,面向用户;**绝不含 api_key**
  long http_status = 0;
  bool canceled = false, timed_out = false;
  int64_t elapsed_ms = 0;
  int prompt_tokens = 0, completion_tokens = 0;
  // DeepSeek 的思维链(reasoning_content);写代码模式必须丢弃,只有练习模式可用。
  std::string reasoning;
};

// SSE 帧切分器:纯函数式、可单测(tests/test_sse.cpp)。
// feed() 可被任意切分的字节流多次调用(半帧会留在内部 carry 里),
// 每凑齐一个 "data: <payload>\n\n" 就回调一次 payload(已去掉 "data: " 前缀)。
// 同时兼容 "\r\n" 行尾;非 data: 的字段(event:/id:/retry:/注释行)被忽略。
class SseParser {
 public:
  void feed(const char* p, size_t n,
            const std::function<void(const std::string& payload)>& cb);
  void reset();
  // 流结束时调用:把 carry 里最后一个未以空行结尾的帧吐出来。
  void finish(const std::function<void(const std::string& payload)>& cb);

 private:
  std::string carry_;
};

// ---- 协议解析(纯函数,与 curl 无关,方便单测)----
// 解析一帧 SSE payload:
//   payload == "[DONE]"           -> 置 done=true,返回 true
//   choices[0].delta.content      -> content
//   choices[0].delta.reasoning_content -> reasoning
// 解析不出内容时返回 false(调用方可把原文前 200 字节丢进 AI 面板辅助诊断)。
bool sseChunkToDelta(const std::string& payload, std::string& content,
                     std::string& reasoning, bool& done);
// 非流式响应体 -> 完整内容 + token 用量。choices[0].message.content。
bool chatBodyToText(const std::string& body, std::string& text,
                    std::string& reasoning, int& prompt_tokens, int& completion_tokens);
// HTTP 4xx/5xx 的 body(通常是 {"error":{"message":...}})-> 中文错误文案。
// 映射:401 -> "API key 无效或已过期";402/429 -> "余额不足或请求过于频繁";
//       5xx -> "服务端错误(HTTP xxx)"。curl_code 见下。
std::string chatErrorZh(long http_status, int curl_code, const std::string& body);
// curl 错误码 -> 中文:OPERATION_TIMEDOUT -> "请求超时";
// COULDNT_RESOLVE_HOST -> "无法解析域名,检查网络或 base_url";
// SSL_CONNECT_ERROR -> "TLS 连接失败";ABORTED_BY_CALLBACK -> "已取消"。
std::string curlErrorZh(int curl_code);

class HttpChat {
 public:
  HttpChat();
  ~HttpChat();
  HttpChat(const HttpChat&) = delete;
  HttpChat& operator=(const HttpChat&) = delete;

  static void globalInit();      // curl_global_init,必须在任何线程启动前于 main 调用
  static void globalCleanup();

  // 阻塞发送。on_delta 每收到一段增量回调一次(非流式时只在末尾回调一次);
  // should_cancel 被 curl 进度回调轮询,返回 true 则立刻中止并置 canceled。
  //
  // 必须设置的 curl 选项(逐条都有理由,别省):
  //   CURLOPT_NOSIGNAL=1        多线程程序里是**强制**的,否则 DNS 超时走
  //                             SIGALRM+siglongjmp,在非主线程上会破坏状态甚至崩溃;
  //   CURLOPT_TIMEOUT_MS / CONNECTTIMEOUT_MS;
  //   CURLOPT_LOW_SPEED_LIMIT=1 / LOW_SPEED_TIME=10   卡死的流会自己死掉;
  //   CURLOPT_NOPROGRESS=0 + XFERINFOFUNCTION -> 查 should_cancel();
  //   HTTPHEADER: Authorization: Bearer <key> / Content-Type: application/json /
  //               流式时 Accept: text/event-stream;
  //   CURLOPT_VERBOSE 恒为 0 —— **api_key 绝不出现在面板、日志、错误消息里**。
  ChatResponse send(const ChatRequest& req,
                    const ChatDeltaFn& on_delta,
                    const ChatCancelFn& should_cancel);

 private:
  void* curl_ = nullptr;         // CURL*,保持 void* 以免头文件引 curl.h
  std::string sse_carry_;
};

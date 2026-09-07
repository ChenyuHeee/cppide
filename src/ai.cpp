// ai.cpp —— AI 子系统实现:worker 线程 + 模式闸门 + generation 取消 +
//            prompt 构建 + 上下文截断 + 补全后处理。
//
// ★ 本文件是 §6「练习模式绝不写缓冲区」的成型处,做法是**结构性**的:
//   1) 本文件只见 `const TextBuffer&`,全文没有任何一处对缓冲区的写操作
//      (没有对 TextBuffer 的写原语调用,也没有非 const 的 TextBuffer 引用);
//      tests/test_ai.cpp 会 grep 本文件源码来钉住这条事实。
//   2) `AiSink` 只由 `sinkFor(mode)`(ai.h,全工程唯一决策点)在**请求创建时**
//      定死写进 AiRequest,再随每个 AppEvent 一路带回。练习模式的回复
//      在数据层面就不是 GhostText,下游没有能搞错的余地。
//   3) `setMode()` / `noteActivity()` / `cancelInFlight()` 都先 `gen_++`,
//      于是在飞回复在被"检视"之前就已作废 —— 不存在"切模式后旧回复才落地"的时间窗。
//
// 线程模型(§5.1):
//   * 唯一 worker 线程,**仅在 cfg.aiEnabled() 时创建**(key 为空则线程根本不存在);
//   * worker 阻塞在 in_.waitPop(req, 200ms) 上,每 200ms 有机会看 stop_;
//   * UI 线程侧的所有公开方法都只做原子量读写 + 一次 Mailbox::push,
//     绝不阻塞在网络上(HttpChat::send 只在 worker 线程调用);
//   * 析构 = gen_++ 作废在飞 + stop_ + in_.close() + join(绝不 detach)。
//
// 事件约定(与 architecture.md §5.5 / §6(2) 一致,App 侧务必按此消费):
//   AiStarted : text 为空。请求已开始(此时 generation() 就是 ev.gen)。
//   AiDelta   : text = **增量**原始文本,最多每 40ms 一个(背压)。
//   AiDone    : text = **完整最终文本**(写代码模式已做完后处理;练习模式已追加
//               reasoning_content)。语义是**替换**已流式累积的内容,不是再追加。
//   AiError   : text = 中文错误文案(绝不含 api_key)。canceled 不算错误,不发事件。

#include "ai.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "aihttp.h"
#include "util.h"

// ---------------------------------------------------------------------------
// 模式名 / 内置 prompt
// ---------------------------------------------------------------------------

const char* aiModeZh(AiMode m) {
  return m == AiMode::Code ? "写代码模式" : "练习模式";
}

namespace aiprompt {

const char* defaultCodeSystem() {
  return "你是 C/C++ 竞赛代码补全引擎。只输出应插入在 <CURSOR> 处的代码;"
         "不要重复已有代码,不要解释,不要 markdown 代码围栏。";
}

const char* defaultPracticeSystem() {
  return "你是 ICPC 教练。绝对不要输出可直接编译运行的完整代码或代码块。"
         "只用中文给出:1) 可能的算法方向 2) 时间/空间复杂度估计 "
         "3) 当前代码的卡点或潜在 bug 方向 4) 下一步该想什么。不超过 300 字。";
}

const char* practiceHardSuffix() {
  return "再次强调:不要输出任何可直接编译运行的代码。";
}

const char* cursorMark() { return "<CURSOR>"; }

}  // namespace aiprompt

// ---------------------------------------------------------------------------
// 文件内常量与小工具
// ---------------------------------------------------------------------------

namespace {

// 上下文截断标记(§5.7)。
const char* const kElide = "/* ...省略... */";
// 背压:delta 最多每 40ms 推一个事件(§5.5)。
constexpr int64_t kDeltaFlushMs = 40;
// worker 在请求队列上的等待粒度:200ms 一醒,用于及时看 stop_(§5.1)。
constexpr int kWaitPopMs = 200;
// 单次请求最多攒多少字节的正文(防服务端发疯把内存吃光)。
constexpr size_t kMaxAnswerBytes = 1u << 20;

const char* const kZhNotConfigured =
    "AI 未配置:配置文件里的 api_key 为空(编辑与编译不受影响)";
// model 也是"完整配置"的一部分:需求不允许代码里写死模型名,所以默认为空,
// 空就等于没配好 —— 这时一次请求都不发(省掉一次注定 400 的网络往返)。
const char* const kZhNoModel =
    "AI 未配置:配置文件里的 model 为空 —— 请按 DeepSeek 官方文档填写模型名"
    "(编辑与编译不受影响)";
const char* const kZhEmptyAnswer = "AI 返回内容为空";
const char* const kZhFailed = "AI 请求失败";

// 统一的事件出口(自由函数而非成员:ai.h 已冻结,不加成员函数)。
void pushEvent(Mailbox<AppEvent>* out, EvKind kind, uint64_t gen, AiSink sink,
               std::string text) {
  if (out == nullptr) return;
  AppEvent ev;
  ev.kind = kind;
  ev.gen = gen;
  ev.sink = sink;  // 随请求一路带回,§6 的关键
  ev.text = std::move(text);
  out->push(std::move(ev));
}

bool isFenceLine(const std::string& line) {
  return util::startsWith(util::trimLeft(line), "```");
}

// 兜底洗一遍 key:aihttp 已在所有返回路径洗过(见 wave3-aihttp.md),
// 这里再挡一次,保证"错误消息里不含 key"这条不依赖下层实现细节。
std::string scrubKey(std::string s, const std::string& key) {
  if (key.size() < 8 || s.empty()) return s;
  const std::string tag = "[REDACTED]";
  for (size_t w = 0; w + 8 <= key.size(); ++w) {
    const std::string frag = key.substr(w, 8);
    for (size_t at = s.find(frag); at != std::string::npos; at = s.find(frag)) {
      s = s.substr(0, at) + tag + s.substr(at + frag.size());
    }
  }
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

AiService::AiService(const Config& cfg, Mailbox<AppEvent>* out)
    : cfg_(cfg), out_(out) {
  last_activity_ms_.store(util::nowMs());
  last_auto_request_ms_.store(0);
  mode_.store(static_cast<int>(AiMode::Practice));  // 默认练习模式(更安全的那一档)
  if (enabled()) {
    state_.store(static_cast<int>(AiState::Idle));
    // ★ 只有 key 与 model 都非空才创建线程。缺任一项时线程根本不存在。
    th_ = std::thread(&AiService::workerLoop, this);
  } else {
    state_.store(static_cast<int>(AiState::Disabled));
  }
}

AiService::~AiService() {
  // 顺序要紧:先让在飞请求的取消回调立刻返回 true(gen 变化 + stop_),
  // 再 close 唤醒 waitPop,最后 join。绝不 detach。
  stop_.store(true);
  gen_.fetch_add(1);
  in_.close();
  if (th_.joinable()) th_.join();
  in_.drain();
  in_flight_.store(false);
}

// ---------------------------------------------------------------------------
// UI 线程侧:全部必须立即返回(只碰原子量 / 一次 push)
// ---------------------------------------------------------------------------

bool AiService::enabled() const {
  // "配置完整"= key 非空 **且** model 非空。model 的默认值是空串(需求:代码里
  // 不许出现写死的模型名),空 model 下不起 worker、不发请求、状态栏显示"未配置",
  // 但编辑/高亮/编译/运行完全照常 —— 与 key 为空时同一条降级路径。
  return cfg_.aiEnabled() && !cfg_.model.empty();
}

AiMode AiService::mode() const { return static_cast<AiMode>(mode_.load()); }

void AiService::setMode(AiMode m) {
  // §6(3):先作废在飞工作,再换模式。于是"切到练习模式后写代码模式的回复才落地"
  // 这个时间窗在结构上不存在。
  gen_.fetch_add(1);
  mode_.store(static_cast<int>(m));
  const int64_t now = util::nowMs();
  last_activity_ms_.store(now);
  // 切模式本身不算"新活动":不给自动请求开额度,免得一按 Ctrl-T 就白花一次钱。
  // 只有用户真的打字/移动光标(noteActivity)之后才会再自动请求。
  auto_done_gen_.store(gen_.load());
  if (enabled()) state_.store(static_cast<int>(AiState::Idle));
}

AiMode AiService::toggleMode() {
  const AiMode next = (mode() == AiMode::Code) ? AiMode::Practice : AiMode::Code;
  setMode(next);
  return next;
}

void AiService::noteActivity() {
  last_activity_ms_.store(util::nowMs());
  gen_.fetch_add(1);  // 在飞请求就此作废(双重过滤的第一层)
}

void AiService::cancelInFlight() {
  gen_.fetch_add(1);
  // 取消(Esc 丢弃 ghost 也走这里)之后不许自动重发同一份建议:
  // 把新 gen 直接标记成"已请求过",只有用户下一次真实活动才会重新开额度。
  auto_done_gen_.store(gen_.load());
  if (enabled()) state_.store(static_cast<int>(AiState::Idle));
}

uint64_t AiService::generation() const { return gen_.load(); }

bool AiService::inFlight() const { return in_flight_.load(); }

AiState AiService::state() const {
  if (!enabled()) return AiState::Disabled;
  if (in_flight_.load()) return AiState::Thinking;
  const AiState s = static_cast<AiState>(state_.load());
  return (s == AiState::Error) ? AiState::Error : AiState::Idle;
}

const char* AiService::stateZh() const {
  switch (state()) {
    case AiState::Disabled: return "未配置";
    case AiState::Thinking: return "思考中…";
    case AiState::Error:    return "错误";
    case AiState::Idle:     break;
  }
  return "就绪";
}

void AiService::maybeAutoTrigger(const TextBuffer& b, Pos cur) {
  // §5.3:条件全部满足才发。任何一条不满足都是"立刻返回",绝不阻塞输入。
  if (!enabled()) return;
  if (mode() != AiMode::Code) return;       // 练习模式没有自动补全
  if (in_flight_.load()) return;
  if (stop_.load()) return;
  // ★ 省钱的关键一条:同一次停顿只自动请求一次。
  //   时间阈值只能限频率,限不住总量 —— 手离开键盘时 last_activity_ms_ 不再前进,
  //   于是"停顿够久"永远成立,每过 ghost_min_interval_ms 就会再发一次(实测
  //   默认配置下 20 秒空转发了 23 次)。gen_ 只在用户又有活动时才变,
  //   所以拿它当"这次停顿的身份"最省事,也顺带覆盖了切模式/取消的情形。
  const uint64_t g = gen_.load();
  if (auto_done_gen_.load() == g) return;
  const int64_t now = util::nowMs();
  if (now - last_activity_ms_.load() < cfg_.ghost_delay_ms) return;
  if (now - last_auto_request_ms_.load() < cfg_.ghost_min_interval_ms) return;
  if (!b.hasNonBlank()) return;
  auto_done_gen_.store(g);                  // 本次停顿用掉了唯一的额度
  last_auto_request_ms_.store(now);
  submit(buildRequest(b, cur, std::string(), AiMode::Code));
}

void AiService::askNow(const TextBuffer& b, Pos cur, const std::string& question) {
  if (!enabled()) {
    // 未配置也必须给出可见提示,且绝不崩(需求:key/model 为空时仅提示未配置)。
    if (out_ != nullptr) {
      AppEvent ev;
      ev.kind = EvKind::AiError;
      ev.gen = gen_.load();
      ev.sink = sinkFor(mode());
      ev.text = cfg_.api_key.empty() ? kZhNotConfigured : kZhNoModel;
      out_->push(std::move(ev));
    }
    return;
  }
  // 手动请求无视 ghost_min_interval_ms,但同样先取消在飞请求(§5.3)。
  gen_.fetch_add(1);
  const int64_t now = util::nowMs();
  last_activity_ms_.store(now);
  last_auto_request_ms_.store(now);
  // 手动问过了,同一次停顿就不必再自动补一发(手动本身不受任何限制)。
  auto_done_gen_.store(gen_.load());
  submit(buildRequest(b, cur, question, mode()));
}

void AiService::submit(AiRequest req) {
  if (!enabled() || stop_.load()) return;
  // in_flight_ 在 UI 线程置位:否则"提交后 worker 还没跑起来"的窗口里
  // maybeAutoTrigger 会重复触发同一个请求。worker 收尾时负责清零。
  in_flight_.store(true);
  state_.store(static_cast<int>(AiState::Idle));
  in_.push(std::move(req));
}

// ---------------------------------------------------------------------------
// 上下文与 prompt(§5.7)
// ---------------------------------------------------------------------------

void AiService::buildContext(const TextBuffer& b, Pos cur, int max_lines,
                             std::string& prefix, std::string& suffix) {
  prefix.clear();
  suffix.clear();
  const int n = b.lineCount();
  const Pos p = b.clampPos(cur);

  int first = 0, last = n - 1;
  if (max_lines > 0 && n > max_lines) {
    // 光标前后各取一半;某一侧不足时把剩余配额补给另一侧。
    const int half = std::max(1, max_lines / 2);
    first = std::max(0, p.line - half);
    last = std::min(n - 1, p.line + half);
    int slack = max_lines - (last - first + 1);
    if (slack > 0) {
      const int grow_up = std::min(slack, first);
      first -= grow_up;
      slack -= grow_up;
      last = std::min(n - 1, last + slack);
    }
  }

  if (first > 0) {
    prefix += kElide;
    prefix += '\n';
  }
  for (int i = first; i < p.line; ++i) {
    prefix += b.line(i);
    prefix += '\n';
  }
  const std::string& cl = b.line(p.line);
  const size_t col = std::min(static_cast<size_t>(std::max(0, p.col)), cl.size());
  prefix += cl.substr(0, col);
  suffix += cl.substr(col);
  for (int i = p.line + 1; i <= last; ++i) {
    suffix += '\n';
    suffix += b.line(i);
  }
  if (last < n - 1) {
    suffix += '\n';
    suffix += kElide;
  }
}

AiRequest AiService::buildRequest(const TextBuffer& b, Pos cur,
                                  const std::string& q, AiMode m) const {
  AiRequest r;
  r.gen = gen_.load();
  r.mode = m;
  // ★ 唯一的 sink 决策点。此后 sink 就是数据,不再有任何 if (mode==...) 决定去向。
  r.sink = sinkFor(m);
  r.stream = cfg_.stream;

  std::string prefix, suffix;
  buildContext(b, cur, cfg_.ai_max_context_lines, prefix, suffix);

  const Pos p = b.clampPos(cur);
  const std::string& cl = b.line(p.line);
  const size_t col = std::min(static_cast<size_t>(std::max(0, p.col)), cl.size());
  r.line_prefix = cl.substr(0, col);  // 后处理据此去掉被模型重复的当前行前缀

  const std::string code = prefix + aiprompt::cursorMark() + suffix;

  if (m == AiMode::Code) {
    r.system_prompt =
        cfg_.prompt_code.empty() ? aiprompt::defaultCodeSystem() : cfg_.prompt_code;
    r.temperature = cfg_.temperature_code;
    r.max_tokens = cfg_.max_tokens_code;
    r.user_prompt.clear();
    if (!util::trim(q).empty()) {
      r.user_prompt += "用户要求:";
      r.user_prompt += q;
      r.user_prompt += "\n\n";
    }
    r.user_prompt += code;
  } else {
    // 练习模式:system 可被配置覆盖,但硬后缀由代码**无条件追加**,不可覆盖。
    r.system_prompt = cfg_.prompt_practice.empty() ? aiprompt::defaultPracticeSystem()
                                                   : cfg_.prompt_practice;
    r.system_prompt += "\n";
    r.system_prompt += aiprompt::practiceHardSuffix();
    r.temperature = cfg_.temperature_practice;
    r.max_tokens = cfg_.max_tokens_practice;
    r.user_prompt = "当前代码(光标位置用 ";
    r.user_prompt += aiprompt::cursorMark();
    r.user_prompt += " 标记):\n";
    r.user_prompt += code;
    r.user_prompt += "\n\n";
    if (util::trim(q).empty()) {
      r.user_prompt += "我的问题:请按上述要求给出中文思路提示。";
    } else {
      r.user_prompt += "我的问题:";
      r.user_prompt += q;
    }
  }
  return r;
}

// ---------------------------------------------------------------------------
// 补全后处理(§5.7)。纯函数,可单测。
// ---------------------------------------------------------------------------

std::string AiService::postprocessCompletion(const std::string& raw,
                                             const std::string& line_prefix,
                                             int max_lines) {
  if (raw.empty()) return std::string();
  std::vector<std::string> ls = util::splitLines(raw);

  // 1) 剥掉 ``` 围栏:有围栏时只留第一个围栏块里的内容(围栏外的解释文字丢掉)。
  bool fenced = false;
  size_t open = ls.size();
  for (size_t i = 0; i < ls.size(); ++i) {
    if (isFenceLine(ls[i])) {
      open = i;
      break;
    }
  }
  if (open < ls.size()) {
    fenced = true;
    size_t close = ls.size();
    for (size_t i = open + 1; i < ls.size(); ++i) {
      if (isFenceLine(ls[i])) {
        close = i;
        break;
      }
    }
    std::vector<std::string> inner;
    for (size_t i = open + 1; i < close; ++i) inner.push_back(ls[i]);
    ls.swap(inner);
  }
  // 行尾残留的围栏(模型偶尔写成 "code```")也剥掉。
  for (std::string& l : ls) {
    while (util::endsWith(l, "```")) l = l.substr(0, l.size() - 3);
  }

  // 2) 有围栏时,围栏后的空行是格式噪声,去掉。
  if (fenced) {
    size_t drop = 0;
    while (drop < ls.size() && util::isBlank(ls[drop])) ++drop;
    if (drop > 0) {
      std::vector<std::string> rest;
      for (size_t i = drop; i < ls.size(); ++i) rest.push_back(ls[i]);
      ls.swap(rest);
    }
  }

  // 3) 模型常把光标所在行已有的文本重复一遍 —— 去掉这个重复前缀。
  if (!ls.empty() && !line_prefix.empty()) {
    const std::string lp_trimmed = util::trimLeft(line_prefix);
    std::string& f = ls[0];
    if (util::startsWith(f, line_prefix)) {
      f = f.substr(line_prefix.size());
    } else if (!lp_trimmed.empty() && util::startsWith(f, lp_trimmed)) {
      f = f.substr(lp_trimmed.size());
    } else if (!lp_trimmed.empty()) {
      const std::string ft = util::trimLeft(f);
      if (util::startsWith(ft, lp_trimmed)) f = ft.substr(lp_trimmed.size());
    }
  }

  // 4) 截断到 max_lines 行。
  if (max_lines > 0 && ls.size() > static_cast<size_t>(max_lines)) {
    ls.resize(static_cast<size_t>(max_lines));
  }

  // 5) 去掉尾部空行。
  while (!ls.empty() && util::isBlank(ls.back())) ls.pop_back();
  if (ls.empty()) return std::string();
  return util::join(ls, "\n");
}

// ---------------------------------------------------------------------------
// worker 线程
// ---------------------------------------------------------------------------

void AiService::workerLoop() {
  // HttpChat 每线程一个实例(aihttp.h 的线程契约),整个 worker 生命周期复用。
  HttpChat http;

  while (!stop_.load()) {
    AiRequest req;
    if (!in_.waitPop(req, kWaitPopMs)) {
      if (stop_.load() || in_.closed()) break;  // close 且队列空 -> 退出,不空转
      continue;                                 // 只是超时:回去再看 stop_
    }
    if (stop_.load()) break;

    const uint64_t g = req.gen;
    // 双重过滤的第一层:curl 的进度回调轮询它,非 0 立刻中止传输。
    const ChatCancelFn should_cancel = [this, g] {
      return gen_.load() != g || stop_.load();
    };
    if (should_cancel()) {          // 排队期间就已作废:一声不响地丢掉
      in_flight_.store(false);
      continue;
    }

    in_flight_.store(true);
    state_.store(static_cast<int>(AiState::Idle));
    pushEvent(out_, EvKind::AiStarted, g, req.sink, std::string());

    ChatRequest cr;
    cr.url = cfg_.chatUrl();
    cr.api_key = cfg_.api_key;
    cr.model = cfg_.model;
    cr.messages.push_back(ChatMessage{"system", req.system_prompt});
    cr.messages.push_back(ChatMessage{"user", req.user_prompt});
    cr.temperature = req.temperature;
    cr.max_tokens = req.max_tokens;
    cr.stream = req.stream;
    cr.connect_timeout_ms = cfg_.ai_connect_timeout_ms;
    cr.total_timeout_ms = cfg_.ai_timeout_ms;

    // 背压(§5.5):delta 攒着,最多每 kDeltaFlushMs 推一个事件。
    std::string pending;
    size_t total_bytes = 0;
    int64_t last_flush = util::nowMs();
    const ChatDeltaFn on_delta = [&](const std::string& d) -> bool {
      if (should_cancel()) return false;   // 返回 false = 上层要求中止
      total_bytes += d.size();
      if (total_bytes > kMaxAnswerBytes) return false;
      pending += d;
      const int64_t now = util::nowMs();
      if (now - last_flush >= kDeltaFlushMs && !pending.empty()) {
        pushEvent(out_, EvKind::AiDelta, g, req.sink, pending);
        pending.clear();
        last_flush = now;
      }
      return true;
    };

    const ChatResponse resp = http.send(cr, on_delta, should_cancel);

    // 取消不是错误(§5.4):不报错、不落地,连残余 delta 也不推。
    if (resp.canceled || should_cancel()) {
      in_flight_.store(false);
      continue;
    }
    if (!pending.empty()) {
      pushEvent(out_, EvKind::AiDelta, g, req.sink, pending);
      pending.clear();
    }

    if (!resp.ok) {
      std::string msg = resp.error.empty() ? std::string(kZhFailed) : resp.error;
      msg = scrubKey(std::move(msg), cfg_.api_key);
      state_.store(static_cast<int>(AiState::Error));
      in_flight_.store(false);
      pushEvent(out_, EvKind::AiError, g, req.sink, msg);
      continue;
    }

    std::string final_text;
    if (req.mode == AiMode::Code) {
      // 写代码模式:reasoning_content 一律丢弃(思维链不该被当代码用)。
      final_text = postprocessCompletion(resp.text, req.line_prefix,
                                         cfg_.ghost_max_lines);
    } else {
      final_text = resp.text;
      // 练习模式:只有这一档才追加思维链,而且它只会进面板(sink == PanelOnly)。
      if (!resp.reasoning.empty()) {
        if (!final_text.empty()) final_text += "\n\n";
        final_text += "【推理过程】\n";
        final_text += resp.reasoning;
      }
    }
    final_text = scrubKey(std::move(final_text), cfg_.api_key);

    if (util::trim(final_text).empty()) {
      state_.store(static_cast<int>(AiState::Error));
      in_flight_.store(false);
      pushEvent(out_, EvKind::AiError, g, req.sink, kZhEmptyAnswer);
      continue;
    }

    state_.store(static_cast<int>(AiState::Idle));
    in_flight_.store(false);   // 先清在飞标志,UI 收到 AiDone 时状态已一致
    pushEvent(out_, EvKind::AiDone, g, req.sink, final_text);
  }

  in_flight_.store(false);
}

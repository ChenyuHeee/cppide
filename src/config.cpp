// config.cpp —— Config 的加载、降级与示例生成。
//
// 三条不可动摇的规矩(需求硬要求):
//   1. load() 永不硬失败:文件缺失 / JSON 语法错 / 单字段类型错 / 数值越界,
//      全部降级为“该字段用默认值 + 一条中文 warning”,照常返回一个可用的 Config。
//   2. 本文件里不出现任何 API key 字面量(真实的或示例的都不行);示例配置的
//      api_key 恒为空串。**模型名同理**:model 没有内置默认值(见 config.h),
//      为空时 warnings 里明确提示“请按 DeepSeek 官方文档填写模型名”,
//      AI 功能整体降级为“未配置”,编辑/编译/运行不受影响。
//   3. api_key 的值绝不进入任何 warning / err / 日志字符串 —— 连片段也不行。
//      下面所有 warn() 调用都只提字段名 "api_key",不提它的内容。
#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "json.h"
#include "util.h"

namespace {

// warnings 是要塞进状态栏与 AI 面板的,数量必须有上界(一份被写坏的配置
// 可能有几千个未知字段),否则启动时 UI 会被淹没。
constexpr size_t kMaxWarnings = 40;

void warn(Config& c, std::string msg) {
  if (c.warnings.size() < kMaxWarnings) {
    c.warnings.push_back(std::move(msg));
  } else if (c.warnings.size() == kMaxWarnings) {
    c.warnings.push_back("配置告警过多,其余告警已省略。");
  }
}

const char* typeZh(mj::Type t) {
  switch (t) {
    case mj::Type::Null:   return "null";
    case mj::Type::Bool:   return "布尔值";
    case mj::Type::Number: return "数字";
    case mj::Type::String: return "字符串";
    case mj::Type::Array:  return "数组";
    case mj::Type::Object: return "对象";
  }
  return "未知类型";
}

std::string numStr(double d) {
  char buf[64];
  if (std::isfinite(d) && d == std::trunc(d) && std::fabs(d) < 1e18) {
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(d));
  } else {
    std::snprintf(buf, sizeof buf, "%g", d);
  }
  return std::string(buf);
}

std::string fieldPrefix(const char* key) {
  return std::string("配置字段 \"") + key + "\" ";
}

// ---- 单字段读取器:键不存在就什么都不做(保留内置默认值) ----

void takeStr(const mj::Value& o, const char* key, std::string& out, bool do_trim, Config& c) {
  if (!o.has(key)) return;
  const mj::Value& v = o.get(key);
  if (!v.isString()) {
    warn(c, fieldPrefix(key) + "应为字符串,实际为" + typeZh(v.type()) + ",已改用默认值。");
    return;
  }
  out = do_trim ? util::trim(v.asString()) : v.asString();
}

void takeBool(const mj::Value& o, const char* key, bool& out, Config& c) {
  if (!o.has(key)) return;
  const mj::Value& v = o.get(key);
  if (!v.isBool()) {
    warn(c, fieldPrefix(key) + "应为布尔值 true/false,实际为" + typeZh(v.type()) +
                ",已改用默认值 " + (out ? "true" : "false") + "。");
    return;
  }
  out = v.asBool();
}

// 返回“清洗后的数值”;is_ok=false 表示该键不可用,调用方应保留默认值。
bool readNumber(const mj::Value& o, const char* key, double lo, double hi, bool want_int,
                Config& c, double& out) {
  if (!o.has(key)) return false;
  const mj::Value& v = o.get(key);
  if (!v.isNumber()) {
    warn(c, fieldPrefix(key) + "应为数字,实际为" + typeZh(v.type()) + ",已改用默认值。");
    return false;
  }
  double d = v.asNumber();
  if (!std::isfinite(d)) {
    warn(c, fieldPrefix(key) + "不是有限数值,已改用默认值。");
    return false;
  }
  if (want_int) {
    double t = std::trunc(d);
    if (t != d) {
      warn(c, fieldPrefix(key) + "应为整数,已截断为 " + numStr(t) + "。");
      d = t;
    }
  }
  double cl = std::min(std::max(d, lo), hi);
  if (cl != d) {
    warn(c, fieldPrefix(key) + "的值 " + numStr(d) + " 超出允许范围 [" + numStr(lo) + ", " +
                numStr(hi) + "],已修正为 " + numStr(cl) + "。");
  }
  out = cl;
  return true;
}

void takeInt(const mj::Value& o, const char* key, int& out, int lo, int hi, Config& c) {
  double d = 0;
  if (readNumber(o, key, static_cast<double>(lo), static_cast<double>(hi), true, c, d))
    out = static_cast<int>(d);
}

void takeSize(const mj::Value& o, const char* key, size_t& out, double lo, double hi, Config& c) {
  double d = 0;
  if (readNumber(o, key, lo, hi, true, c, d)) out = static_cast<size_t>(d);
}

void takeDouble(const mj::Value& o, const char* key, double& out, double lo, double hi, Config& c) {
  double d = 0;
  if (readNumber(o, key, lo, hi, false, c, d)) out = d;
}

void takeStrArray(const mj::Value& o, const char* key, std::vector<std::string>& out, Config& c) {
  if (!o.has(key)) return;
  const mj::Value& v = o.get(key);
  if (!v.isArray()) {
    warn(c, fieldPrefix(key) + "应为字符串数组(例如 [\"-O2\", \"-Wall\"]),实际为" +
                typeZh(v.type()) + ",已改用默认值。");
    return;
  }
  // 显式的 [] 是合法的“我不要任何编译参数”,不是错误。
  std::vector<std::string> tmp;
  tmp.reserve(v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    const mj::Value& e = v.value(i);
    if (!e.isString()) {
      warn(c, fieldPrefix(key) + "的第 " + numStr(static_cast<double>(i + 1)) +
                  " 个元素不是字符串(是" + typeZh(e.type()) + "),已跳过。");
      continue;
    }
    tmp.push_back(e.asString());
  }
  out = std::move(tmp);
}

// 全部承认的顶层键。以 '_' 开头的键一律视为注释(sampleJson 就是这么标注说明的)。
const char* const kKnownKeys[] = {
    "api_key", "base_url", "chat_path", "model", "stream",
    "ghost_delay_ms", "ghost_min_interval_ms", "ghost_max_lines",
    "ai_connect_timeout_ms", "ai_timeout_ms", "ai_max_context_lines",
    "max_tokens_code", "max_tokens_practice", "temperature_code", "temperature_practice",
    "prompt_code", "prompt_practice",
    "cc", "cxx", "cflags", "cxxflags", "compile_timeout_ms",
    "run_timeout_ms", "run_output_limit", "stdin_file",
    "tab_width", "expand_tab", "auto_indent", "show_line_numbers", "panel_height", "tick_ms",
};

bool isKnownKey(const std::string& k) {
  if (!k.empty() && k[0] == '_') return true;  // 注释键
  for (const char* known : kKnownKeys)
    if (k == known) return true;
  return false;
}

void applyObject(const mj::Value& o, Config& c) {
  // ---- AI ----
  takeStr(o, "api_key", c.api_key, true, c);
  takeStr(o, "base_url", c.base_url, true, c);
  takeStr(o, "chat_path", c.chat_path, true, c);
  takeStr(o, "model", c.model, true, c);
  takeBool(o, "stream", c.stream, c);
  takeInt(o, "ghost_delay_ms", c.ghost_delay_ms, 0, 60000, c);
  takeInt(o, "ghost_min_interval_ms", c.ghost_min_interval_ms, 0, 600000, c);
  takeInt(o, "ghost_max_lines", c.ghost_max_lines, 1, 200, c);
  takeInt(o, "ai_connect_timeout_ms", c.ai_connect_timeout_ms, 100, 300000, c);
  takeInt(o, "ai_timeout_ms", c.ai_timeout_ms, 100, 600000, c);
  takeInt(o, "ai_max_context_lines", c.ai_max_context_lines, 1, 100000, c);
  takeInt(o, "max_tokens_code", c.max_tokens_code, 1, 32768, c);
  takeInt(o, "max_tokens_practice", c.max_tokens_practice, 1, 32768, c);
  takeDouble(o, "temperature_code", c.temperature_code, 0.0, 2.0, c);
  takeDouble(o, "temperature_practice", c.temperature_practice, 0.0, 2.0, c);
  takeStr(o, "prompt_code", c.prompt_code, false, c);
  takeStr(o, "prompt_practice", c.prompt_practice, false, c);
  // ---- 编译 ----
  takeStr(o, "cc", c.cc, true, c);
  takeStr(o, "cxx", c.cxx, true, c);
  takeStrArray(o, "cflags", c.cflags, c);
  takeStrArray(o, "cxxflags", c.cxxflags, c);
  takeInt(o, "compile_timeout_ms", c.compile_timeout_ms, 1000, 600000, c);
  // ---- 运行 ----
  takeInt(o, "run_timeout_ms", c.run_timeout_ms, 100, 600000, c);
  takeSize(o, "run_output_limit", c.run_output_limit, 1024.0, 268435456.0, c);
  takeStr(o, "stdin_file", c.stdin_file, true, c);
  // ---- 编辑器 ----
  takeInt(o, "tab_width", c.tab_width, 1, 16, c);
  takeBool(o, "expand_tab", c.expand_tab, c);
  takeBool(o, "auto_indent", c.auto_indent, c);
  takeBool(o, "show_line_numbers", c.show_line_numbers, c);
  takeInt(o, "panel_height", c.panel_height, 3, 100, c);
  takeInt(o, "tick_ms", c.tick_ms, 10, 1000, c);

  // 空串在这几个字段上是“配了但配空了”,必须回落,否则会 exec("") / 请求空 URL。
  const Config d;
  if (c.base_url.empty()) {
    warn(c, fieldPrefix("base_url") + "为空,已改用默认值 " + d.base_url + "。");
    c.base_url = d.base_url;
  }
  if (c.chat_path.empty()) {
    warn(c, fieldPrefix("chat_path") + "为空,已改用默认值 " + d.chat_path + "。");
    c.chat_path = d.chat_path;
  }
  if (c.cc.empty()) {
    warn(c, fieldPrefix("cc") + "为空,已改用默认值 " + d.cc + "。");
    c.cc = d.cc;
  }
  if (c.cxx.empty()) {
    warn(c, fieldPrefix("cxx") + "为空,已改用默认值 " + d.cxx + "。");
    c.cxx = d.cxx;
  }
  if (!c.stdin_file.empty()) c.stdin_file = util::expandUser(c.stdin_file);

  for (size_t i = 0; i < o.size(); ++i) {
    const std::string& k = o.key(i);
    if (!isKnownKey(k)) warn(c, "配置中有无法识别的字段 \"" + k + "\",已忽略。");
  }
}

// 环境变量优先级最高(便于临时切换而不改文件)。空值等同于未设置。
void applyEnv(Config& c) {
  std::string k = util::trim(util::envOr("CPPIDE_API_KEY", ""));
  if (k.empty()) k = util::trim(util::envOr("DEEPSEEK_API_KEY", ""));
  if (!k.empty()) c.api_key = k;  // 注意:不写进任何 warning
  const std::string m = util::trim(util::envOr("CPPIDE_MODEL", ""));
  if (!m.empty()) c.model = m;
  const std::string b = util::trim(util::envOr("CPPIDE_BASE_URL", ""));
  if (!b.empty()) c.base_url = b;
}

// key 只允许可见 ASCII:粘贴时带进来的换行/空格/中文引号会让 HTTP 头非法,
// 与其让 libcurl 在运行期报怪错,不如现在就丢掉并提示重贴。
// 只看字符类别,不看内容,更不把内容写进消息。
bool keyLooksSane(const std::string& s) {
  for (unsigned char ch : s)
    if (ch < 0x21 || ch > 0x7e) return false;
  return true;
}

}  // namespace

std::string Config::chatUrl() const {
  std::string b = util::trim(base_url);
  std::string p = util::trim(chat_path);
  while (!b.empty() && b.back() == '/') b.pop_back();
  size_t i = 0;
  while (i < p.size() && p[i] == '/') ++i;
  p.erase(0, i);
  if (p.empty()) return b;
  if (b.empty()) return "/" + p;
  return b + "/" + p;
}

std::string ConfigLoader::defaultPath() {
  const std::string env = util::trim(util::envOr("CPPIDE_CONFIG", ""));
  if (!env.empty()) return util::expandUser(env);
  const std::string xdg = util::trim(util::envOr("XDG_CONFIG_HOME", ""));
  if (!xdg.empty()) return util::joinPath(util::expandUser(xdg), "cppide/config.json");
  std::string home = util::homeDir();
  if (home.empty()) home = ".";
  return util::joinPath(home, ".config/cppide/config.json");
}

Config ConfigLoader::load(const std::string& path, std::string& err) {
  err.clear();
  Config c;
  // --config 显式给出的路径优先于一切;两种来源都做 "~/" 展开。
  const std::string p = path.empty() ? defaultPath() : util::expandUser(path);

  std::string text;
  bool have_text = false;
  if (p.empty()) {
    err = "无法确定配置文件路径(HOME 与 XDG_CONFIG_HOME 均不可用):已全部使用默认设置。";
    warn(c, err);
  } else if (util::isDirectory(p)) {
    err = "配置路径 " + p + " 是一个目录而不是文件:已全部使用默认设置。";
    warn(c, err);
  } else if (!util::fileExists(p)) {
    err = "未找到配置文件 " + p + ":已使用默认设置,AI 功能已关闭。";
    warn(c, err);
    warn(c, "可执行 `cppide --print-config > " + p + "` 生成示例配置,再把 api_key 填进去。");
  } else {
    std::string rerr;
    if (!util::readFile(p, text, rerr)) {
      err = "配置文件 " + p + " 读取失败(" + rerr + "):已全部使用默认设置。";
      warn(c, err);
    } else {
      have_text = true;
    }
  }

  if (have_text) {
    std::string jerr;
    const mj::Value root = mj::Value::parse(text, jerr);
    if (!jerr.empty()) {
      err = "配置文件 " + p + " 不是合法 JSON(" + jerr + "):已全部使用默认设置。";
      warn(c, err);
    } else if (!root.isObject()) {
      err = "配置文件 " + p + " 的顶层不是 JSON 对象(实际为" + typeZh(root.type()) +
            "):已全部使用默认设置。";
      warn(c, err);
    } else {
      c.source_path = p;
      applyObject(root, c);
    }
  }

  applyEnv(c);

  if (!c.api_key.empty() && !keyLooksSane(c.api_key)) {
    c.api_key.clear();  // 内容自始至终没有被打印
    warn(c, "api_key 含有空白或不可见字符,已忽略(其内容不会被打印)。请重新粘贴一遍。");
  }
  if (c.api_key.empty()) {
    warn(c, "AI 未配置:请在 " + (p.empty() ? std::string("配置文件") : p) +
                " 里填写 api_key,或设置环境变量 CPPIDE_API_KEY。"
                "编辑、编译、运行功能均不受影响。");
  }
  const Config d;
  (void)d;   // model 已无内置默认值,这里不再需要"与默认值比较"
  if (c.model.empty()) {
    // model 没有内置默认值(代码里不许写死模型名),所以空 = 没配好。
    warn(c, "未配置 model,请按 DeepSeek 官方文档填写模型名。"
            "在此之前 AI 功能显示为\"未配置\"且不会发出任何请求;"
            "编辑、语法高亮、编译、运行均不受影响。");
  }
  return c;
}

std::string ConfigLoader::sampleJson() {
  // 所有取值都来自 Config 的成员默认值,本函数里不写死任何具体值 ——
  // 默认值改了这里自动跟着改,也就没有“示例与实现不一致”的可能。
  // JSON 标准不支持注释,所以说明放在 "_comment*" 键里:它们是合法 JSON
  // 字符串,加载器会把以 '_' 开头的键当注释跳过。
  const Config d;
  mj::Value o = mj::Value::object();

  o.set("_comment", "cppide 配置文件。以 _ 开头的键是注释,加载时会被忽略。"
                    "所有字段都可省略,省略即用内置默认值。");
  o.set("_comment_env", "环境变量优先级高于本文件:CPPIDE_API_KEY(或 DEEPSEEK_API_KEY)、"
                        "CPPIDE_MODEL、CPPIDE_BASE_URL。配置文件路径可用 --config 或 CPPIDE_CONFIG 指定。");

  o.set("_comment_api_key", "把 DeepSeek 控制台申请到的 key 填在这里。留空则 AI 功能关闭,"
                            "编辑/编译/运行照常可用。切勿把填好 key 的本文件提交到版本库。");
  o.set("api_key", "");
  o.set("_comment_model", "模型 ID:留空则 AI 功能显示\"未配置\"且不发任何请求"
                          "(编辑/编译/运行照常)。请按 DeepSeek 官方文档填写模型名 —— "
                          "本程序不内置任何模型名,也不对模型能力做假设。");
  o.set("model", d.model);
  o.set("base_url", d.base_url);
  o.set("chat_path", d.chat_path);
  o.set("stream", d.stream);

  o.set("_comment_ghost", "写代码模式的行内补全:停顿 ghost_delay_ms 毫秒后自动请求,"
                          "两次请求至少间隔 ghost_min_interval_ms 毫秒(省钱),最多显示 ghost_max_lines 行。"
                          "同一次停顿只会自动请求一次 —— 手离开键盘不动不会持续计费,"
                          "要再来一次就再打字/移动光标,或按 Ctrl-A 手动请求。");
  o.set("ghost_delay_ms", d.ghost_delay_ms);
  o.set("ghost_min_interval_ms", d.ghost_min_interval_ms);
  o.set("ghost_max_lines", d.ghost_max_lines);
  o.set("ai_connect_timeout_ms", d.ai_connect_timeout_ms);
  o.set("ai_timeout_ms", d.ai_timeout_ms);
  o.set("ai_max_context_lines", d.ai_max_context_lines);
  o.set("max_tokens_code", d.max_tokens_code);
  o.set("max_tokens_practice", d.max_tokens_practice);
  o.set("temperature_code", d.temperature_code);
  o.set("temperature_practice", d.temperature_practice);
  o.set("_comment_prompt", "留空则使用内置提示词。练习模式的“不要输出可运行代码”后缀由程序强制追加,无法被覆盖。");
  o.set("prompt_code", d.prompt_code);
  o.set("prompt_practice", d.prompt_practice);

  o.set("_comment_build", "编译命令与参数。cflags 用于 .c,cxxflags 用于 .cc/.cpp/.cxx。"
                          "写成 [] 表示不加任何参数。");
  o.set("cc", d.cc);
  o.set("cxx", d.cxx);
  mj::Value cflags = mj::Value::array();
  for (const std::string& f : d.cflags) cflags.push(f);
  o.set("cflags", std::move(cflags));
  mj::Value cxxflags = mj::Value::array();
  for (const std::string& f : d.cxxflags) cxxflags.push(f);
  o.set("cxxflags", std::move(cxxflags));
  o.set("compile_timeout_ms", d.compile_timeout_ms);

  o.set("_comment_run", "run_output_limit 是单次运行捕获的输出字节上限;"
                        "stdin_file 留空则用【输入】面板里编辑的内容。");
  o.set("run_timeout_ms", d.run_timeout_ms);
  o.set("run_output_limit", static_cast<int64_t>(d.run_output_limit));
  o.set("stdin_file", d.stdin_file);

  o.set("_comment_editor", "tick_ms 是主循环的空转周期(毫秒),影响 ghost 触发与状态刷新的精度。");
  o.set("tab_width", d.tab_width);
  o.set("expand_tab", d.expand_tab);
  o.set("auto_indent", d.auto_indent);
  o.set("show_line_numbers", d.show_line_numbers);
  o.set("panel_height", d.panel_height);
  o.set("tick_ms", d.tick_ms);

  std::string s = o.dump(2);
  if (s.empty() || s.back() != '\n') s.push_back('\n');
  return s;
}

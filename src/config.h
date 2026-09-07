// config.h —— 配置结构与加载器
//
// 路径解析优先级(由 ConfigLoader::defaultPath() 实现后三条):
//   1. 命令行 --config <PATH>            (main.cpp 直接传给 load())
//   2. 环境变量 CPPIDE_CONFIG
//   3. $XDG_CONFIG_HOME/cppide/config.json
//   4. ~/.config/cppide/config.json
//
// 取值优先级:内置默认 < 配置文件 < 环境变量
//   (CPPIDE_API_KEY 或 DEEPSEEK_API_KEY、CPPIDE_MODEL、CPPIDE_BASE_URL)
//
// 降级契约(需求硬要求:配置有问题也绝不崩、绝不中断启动):
//   * 文件不存在      -> 全默认 + 一条中文 warning,AI 关闭,编辑/编译/运行照常;
//   * JSON 语法错误    -> 全默认 + 带行号的 warning;
//   * 单字段类型不对   -> 该字段用默认值 + 一条 warning,其余字段照常生效;
//   * api_key 为空     -> aiEnabled()==false,AiService 不启动 worker 线程。
//   * model   为空     -> AI 同样视为"未完整配置":AiService::enabled()==false,
//                         不起线程、不发请求、状态栏显示"未配置",并给出中文提示;
//                         编辑、语法高亮、编译、运行完全不受影响。
//                         (aiEnabled() 只看 key —— 它是"key 是否可用"的语义;
//                          "配置是否完整"由 AiService::enabled() 判定。)
//   load() 永不返回失败:err 只是“最严重的那条说明”,warnings 里是全部。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct Config {
  // ---- AI ----
  std::string api_key;                       // 空 => AI 关闭
  std::string base_url  = "https://api.deepseek.com";
  std::string chat_path = "/v1/chat/completions";
  // 模型 ID **没有内置默认值**:需求硬性要求"代码里不出现任何硬编码 key 或写死的
  // 模型名",而且冻结的需求里也说明「DeepSeek V4」这类名字无法确认是官方 API 的有效
  // 模型 ID,所以代码不做任何模型能力假设。空 => 视为 AI 未完整配置(见下)。
  std::string model;
  bool   stream                 = true;
  int    ghost_delay_ms         = 500;       // 停顿多久后自动请求补全
  int    ghost_min_interval_ms  = 1200;      // 两次自动请求的最小间隔(省钱);
                                             // 另外"同一次停顿只自动请求一次"由
                                             // AiService 保证,见 ai.h auto_done_gen_
  int    ghost_max_lines        = 8;         // ghost 最多几行
  int    ai_connect_timeout_ms  = 5000;
  int    ai_timeout_ms          = 20000;
  int    ai_max_context_lines   = 400;
  int    max_tokens_code        = 256;
  int    max_tokens_practice    = 512;
  double temperature_code       = 0.2;
  double temperature_practice   = 0.7;
  std::string prompt_code;                   // 空 => 用内置默认
  std::string prompt_practice;               // 空 => 用内置默认
  // ---- 编译 ----
  std::string cc  = "cc";
  std::string cxx = "c++";
  std::vector<std::string> cflags   = {"-O2", "-std=c11", "-Wall"};
  std::vector<std::string> cxxflags = {"-O2", "-std=c++17", "-Wall"};
  int compile_timeout_ms = 30000;
  // ---- 运行 ----
  int    run_timeout_ms   = 5000;
  size_t run_output_limit = 1u << 20;
  std::string stdin_file;                    // 空 => 用【输入】面板内容
  // ---- 编辑器 ----
  int  tab_width = 4;
  bool expand_tab = true;
  bool auto_indent = true;
  bool show_line_numbers = true;
  int  panel_height = 10;
  int  tick_ms = 60;
  // ---- 元信息 ----
  std::string source_path;                   // 实际加载自哪(""=全默认)
  std::vector<std::string> warnings;         // 非致命解析告警,启动时进状态栏与 AI 面板

  bool aiEnabled() const { return !api_key.empty(); }
  // base_url + chat_path,处理重复/缺失斜杠。
  std::string chatUrl() const;
};

class ConfigLoader {
 public:
  // 按上面优先级 2/3/4 推导出应当读取的配置文件路径(文件可能不存在)。
  static std::string defaultPath();
  // 永不硬失败:任何问题都降级为默认值 + warnings。path 为空时用 defaultPath()。
  static Config load(const std::string& path, std::string& err);
  // 带注释说明的示例配置(--print-config 与 README 共用)。
  static std::string sampleJson();
};

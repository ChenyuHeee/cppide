// test_config.cpp —— ConfigLoader / Config 的降级契约与优先级回归测试。
//
// 纯断言,无测试框架:main 返回 0 即全部通过,任何一条不成立立刻 abort 并打印行号。
// 用 CHECK 而不是 assert(),是为了在 -O2 / -DNDEBUG 下断言也不会被消掉。
//
// 覆盖点:文件缺失 / JSON 语法错 / 字段类型错 / 数值越界 clamp / 环境变量覆盖 /
// --config 优先 / ~ 展开 / chatUrl 斜杠矩阵 / 数组字段 / sampleJson 往返 /
// 源码里不含硬编码 api_key。
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"

static int g_checks = 0;

#define CHECK(cond)                                                                \
  do {                                                                             \
    ++g_checks;                                                                    \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      std::fflush(stderr);                                                         \
      std::abort();                                                                \
    }                                                                              \
  } while (0)

#define CHECK_EQ_STR(a, b)                                                              \
  do {                                                                                  \
    ++g_checks;                                                                         \
    const std::string aa_ = (a), bb_ = (b);                                             \
    if (aa_ != bb_) {                                                                   \
      std::fprintf(stderr, "FAIL %s:%d: [%s] != [%s]\n", __FILE__, __LINE__,            \
                   aa_.c_str(), bb_.c_str());                                           \
      std::fflush(stderr);                                                              \
      std::abort();                                                                     \
    }                                                                                   \
  } while (0)

// ---------------------------------------------------------------- 辅助

static std::string g_dir;      // 本次运行的临时目录
static std::string g_realhome; // 进程原本的 HOME

static const char* kEnvNames[] = {"CPPIDE_CONFIG",   "XDG_CONFIG_HOME", "CPPIDE_API_KEY",
                                  "DEEPSEEK_API_KEY", "CPPIDE_MODEL",   "CPPIDE_BASE_URL"};

static void resetEnv() {
  for (const char* n : kEnvNames) ::unsetenv(n);
  ::setenv("HOME", g_realhome.c_str(), 1);
}

static void putEnv(const char* k, const std::string& v) { ::setenv(k, v.c_str(), 1); }

static std::string writeFile(const std::string& name, const std::string& data) {
  const std::string p = g_dir + "/" + name;
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  CHECK(f.good());
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  f.close();
  CHECK(!f.fail());
  return p;
}

static std::string readWholeFile(const std::string& p, bool& ok) {
  std::ifstream f(p, std::ios::binary);
  ok = f.good();
  if (!ok) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static bool warnHas(const Config& c, const std::string& needle) {
  for (const std::string& w : c.warnings)
    if (w.find(needle) != std::string::npos) return true;
  return false;
}

// 任何一条 warning 里都不许出现 needle(用于证明 api_key 不泄漏)。
static bool warnHasNone(const Config& c, const std::string& needle) {
  return !warnHas(c, needle);
}

// 除 source_path / warnings 外,所有可配置字段都等于内置默认值?
static bool isAllDefaults(const Config& c) {
  const Config d;
  return c.api_key == d.api_key && c.base_url == d.base_url && c.chat_path == d.chat_path &&
         c.model == d.model && c.stream == d.stream && c.ghost_delay_ms == d.ghost_delay_ms &&
         c.ghost_min_interval_ms == d.ghost_min_interval_ms &&
         c.ghost_max_lines == d.ghost_max_lines &&
         c.ai_connect_timeout_ms == d.ai_connect_timeout_ms &&
         c.ai_timeout_ms == d.ai_timeout_ms &&
         c.ai_max_context_lines == d.ai_max_context_lines &&
         c.max_tokens_code == d.max_tokens_code &&
         c.max_tokens_practice == d.max_tokens_practice &&
         c.temperature_code == d.temperature_code &&
         c.temperature_practice == d.temperature_practice &&
         c.prompt_code == d.prompt_code && c.prompt_practice == d.prompt_practice &&
         c.cc == d.cc && c.cxx == d.cxx && c.cflags == d.cflags && c.cxxflags == d.cxxflags &&
         c.compile_timeout_ms == d.compile_timeout_ms &&
         c.run_timeout_ms == d.run_timeout_ms && c.run_output_limit == d.run_output_limit &&
         c.stdin_file == d.stdin_file && c.tab_width == d.tab_width &&
         c.expand_tab == d.expand_tab && c.auto_indent == d.auto_indent &&
         c.show_line_numbers == d.show_line_numbers && c.panel_height == d.panel_height &&
         c.tick_ms == d.tick_ms;
}

// ---------------------------------------------------------------- 1. 文件缺失

static void testMissingFile() {
  resetEnv();
  std::string err;
  const Config c = ConfigLoader::load(g_dir + "/does-not-exist.json", err);
  CHECK(isAllDefaults(c));
  CHECK(!c.aiEnabled());
  CHECK(c.source_path.empty());          // ""=全默认
  CHECK(!c.warnings.empty());
  CHECK(!err.empty());
  CHECK(warnHas(c, "does-not-exist.json"));   // warning 必须写明路径
  CHECK(warnHas(c, "api_key"));               // 必须提示如何填
  CHECK(warnHas(c, "官方文档"));               // model 只是占位
}

// 传空路径时走 defaultPath();指向一个必然不存在的 HOME,也不许崩。
static void testMissingFileViaDefaultPath() {
  resetEnv();
  putEnv("HOME", g_dir + "/no-such-home");
  std::string err;
  const Config c = ConfigLoader::load("", err);
  CHECK(isAllDefaults(c));
  CHECK(!c.aiEnabled());
  CHECK(!err.empty());
  resetEnv();
}

// 配置路径指向一个目录:不许崩,不许把目录当文件读。
static void testDirectoryAsConfig() {
  resetEnv();
  std::string err;
  const Config c = ConfigLoader::load(g_dir, err);
  CHECK(isAllDefaults(c));
  CHECK(!c.aiEnabled());
  CHECK(!err.empty());
  CHECK(!c.warnings.empty());
}

// ---------------------------------------------------------------- 2. JSON 语法错

static void testBrokenJson() {
  resetEnv();
  struct Case { const char* name; std::string text; };
  const std::vector<Case> cases = {
      {"missing-brace.json", "{\"tab_width\": 8"},
      {"trailing-comma.json", "{\"tab_width\": 8,}"},
      {"trailing-comma-array.json", "{\"cflags\": [\"-O2\",]}"},
      {"empty.json", ""},
      {"only-space.json", "   \n\t\n"},
      {"binary.json", std::string("\x00\x01\x02\xff\xfe garbage \x00", 15)},
      {"top-array.json", "[1,2,3]"},
      {"top-string.json", "\"hello\""},
      {"top-number.json", "42"},
      {"top-null.json", "null"},
      {"unquoted-key.json", "{tab_width: 8}"},
      {"single-quotes.json", "{'tab_width': 8}"},
      {"comment.json", "// 注释\n{\"tab_width\": 8}"},
      {"half-string.json", "{\"cc\": \"gcc}"},
      {"nul-inside.json", std::string("{\"cc\": \"g\x00" "cc\"}", 14)},
  };
  for (const Case& cs : cases) {
    const std::string p = writeFile(cs.name, cs.text);
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(isAllDefaults(c));       // 语法错 => 全默认
    CHECK(!c.aiEnabled());
    CHECK(!err.empty());           // err 是“最严重的那条”
    CHECK(!c.warnings.empty());
    CHECK(c.source_path.empty());  // 没有任何字段真的生效过
    CHECK(warnHas(c, cs.name));    // 告警里要有路径,用户才知道去改哪个文件
  }
}

// ---------------------------------------------------------------- 3. 单字段类型错

static void testFieldTypeErrors() {
  resetEnv();
  const std::string p = writeFile("types.json",
                                  "{\n"
                                  "  \"tab_width\": \"four\",\n"
                                  "  \"cflags\": \"not-an-array\",\n"
                                  "  \"stream\": 1,\n"
                                  "  \"base_url\": 123,\n"
                                  "  \"temperature_code\": \"hot\",\n"
                                  "  \"expand_tab\": \"yes\",\n"
                                  "  \"run_output_limit\": null,\n"
                                  "  \"prompt_code\": [\"a\"],\n"
                                  "  \"panel_height\": 20,\n"
                                  "  \"cxx\": \"g++-14\",\n"
                                  "  \"cxxflags\": [\"-O3\", \"-std=c++20\"],\n"
                                  "  \"show_line_numbers\": false,\n"
                                  "  \"model\": \"some-model-name\"\n"
                                  "}\n");
  std::string err;
  const Config c = ConfigLoader::load(p, err);
  const Config d;
  // 坏字段回落默认
  CHECK(c.tab_width == d.tab_width);
  CHECK(c.cflags == d.cflags);
  CHECK(c.stream == d.stream);
  CHECK_EQ_STR(c.base_url, d.base_url);
  CHECK(c.temperature_code == d.temperature_code);
  CHECK(c.expand_tab == d.expand_tab);
  CHECK(c.run_output_limit == d.run_output_limit);
  CHECK_EQ_STR(c.prompt_code, d.prompt_code);
  // 好字段照常生效 —— 这是本条验收的关键
  CHECK(c.panel_height == 20);
  CHECK_EQ_STR(c.cxx, "g++-14");
  CHECK(c.cxxflags == (std::vector<std::string>{"-O3", "-std=c++20"}));
  CHECK(c.show_line_numbers == false);
  CHECK_EQ_STR(c.model, "some-model-name");
  CHECK_EQ_STR(c.source_path, p);
  // 每个坏字段都要有一条点名的告警
  for (const char* k : {"tab_width", "cflags", "stream", "base_url", "temperature_code",
                        "expand_tab", "run_output_limit", "prompt_code"})
    CHECK(warnHas(c, k));
  CHECK(!warnHas(c, "panel_height"));
  CHECK(!warnHas(c, "官方文档"));  // model 被显式配过了,不该再提示占位
  CHECK(err.empty());             // 单字段类型错不算“最严重的说明”
  CHECK(!c.aiEnabled());
}

// 空字符串在这些字段上等于“配坏了”,必须回落,否则会 exec("") / 请求空 URL。
static void testEmptyStringFallback() {
  resetEnv();
  const std::string p = writeFile("empties.json",
                                  "{\"base_url\":\"\",\"chat_path\":\"  \",\"cc\":\"\","
                                  "\"cxx\":\"\",\"model\":\"\",\"stdin_file\":\"\"}");
  std::string err;
  const Config c = ConfigLoader::load(p, err);
  const Config d;
  CHECK_EQ_STR(c.base_url, d.base_url);
  CHECK_EQ_STR(c.chat_path, d.chat_path);
  CHECK_EQ_STR(c.cc, d.cc);
  CHECK_EQ_STR(c.cxx, d.cxx);
  CHECK_EQ_STR(c.model, d.model);
  CHECK(c.stdin_file.empty());  // 空 stdin_file 是合法的(=用【输入】面板)
  CHECK(warnHas(c, "base_url"));
  CHECK(warnHas(c, "chat_path"));
  CHECK(warnHas(c, "cc"));
  CHECK(warnHas(c, "cxx"));
}

static void testUnknownKeys() {
  resetEnv();
  const std::string p = writeFile("unknown.json",
                                  "{\"_comment\":\"忽略我\",\"_x\":1,"
                                  "\"tabwidth\":8,\"tab_width\":6}");
  std::string err;
  const Config c = ConfigLoader::load(p, err);
  CHECK(c.tab_width == 6);
  CHECK(warnHas(c, "tabwidth"));
  CHECK(!warnHas(c, "_comment"));  // 下划线开头的键是注释,不该报
  CHECK(!warnHas(c, "\"_x\""));
}

// ---------------------------------------------------------------- 4. 数组字段

static void testArrayFields() {
  resetEnv();
  {
    const std::string p = writeFile("arr-ok.json",
                                    "{\"cflags\":[\"-O0\",\"-g\",\"-fsanitize=address\"],"
                                    "\"cxxflags\":[\"-std=c++17\"]}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.cflags == (std::vector<std::string>{"-O0", "-g", "-fsanitize=address"}));
    CHECK(c.cxxflags == (std::vector<std::string>{"-std=c++17"}));
    CHECK(c.warnings.size() >= 1);  // 至少还有 api_key 未配置那条
  }
  {  // 显式空数组 = 明确要求“不加任何参数”,不是错误
    const std::string p = writeFile("arr-empty.json", "{\"cflags\":[],\"cxxflags\":[]}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.cflags.empty());
    CHECK(c.cxxflags.empty());
    CHECK(!warnHas(c, "cflags"));
    CHECK(!warnHas(c, "cxxflags"));
  }
  {  // 含非字符串元素:跳过坏元素 + 告警,好元素照常保留
    const std::string p = writeFile("arr-mixed.json",
                                    "{\"cflags\":[\"-O2\",7,null,\"-Wall\",[\"x\"],{},true]}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.cflags == (std::vector<std::string>{"-O2", "-Wall"}));
    CHECK(warnHas(c, "cflags"));
  }
  {  // 全是坏元素 => 空数组 + 告警(而不是崩)
    const std::string p = writeFile("arr-allbad.json", "{\"cxxflags\":[1,2,3]}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.cxxflags.empty());
    CHECK(warnHas(c, "cxxflags"));
  }
  {  // 嵌套对象顶层字段:类型错但不崩
    const std::string p = writeFile("arr-obj.json", "{\"cflags\":{\"a\":1}}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    const Config d;
    CHECK(c.cflags == d.cflags);
    CHECK(warnHas(c, "cflags"));
  }
}

// ---------------------------------------------------------------- 5. 极端数值

static void testExtremeNumbers() {
  resetEnv();
  const std::string p = writeFile("extreme.json",
                                  "{\n"
                                  "  \"run_timeout_ms\": -1,\n"
                                  "  \"tab_width\": 0,\n"
                                  "  \"ai_timeout_ms\": 99999999,\n"
                                  "  \"compile_timeout_ms\": 0,\n"
                                  "  \"run_output_limit\": -5,\n"
                                  "  \"tick_ms\": 1e300,\n"
                                  "  \"temperature_code\": -1.5,\n"
                                  "  \"temperature_practice\": 5,\n"
                                  "  \"ghost_max_lines\": 0,\n"
                                  "  \"ghost_delay_ms\": -1000,\n"
                                  "  \"max_tokens_code\": 1000000,\n"
                                  "  \"ai_max_context_lines\": -2,\n"
                                  "  \"panel_height\": 1e9\n"
                                  "}\n");
  std::string err;
  const Config c = ConfigLoader::load(p, err);
  // 全部落进各自的合法区间(clamp),而不是留着负数去喂 poll()/malloc()
  CHECK(c.run_timeout_ms == 100);
  CHECK(c.tab_width == 1);
  CHECK(c.ai_timeout_ms == 600000);
  CHECK(c.compile_timeout_ms == 1000);
  CHECK(c.run_output_limit == 1024u);
  CHECK(c.tick_ms == 1000);
  CHECK(c.temperature_code == 0.0);
  CHECK(c.temperature_practice == 2.0);
  CHECK(c.ghost_max_lines == 1);
  CHECK(c.ghost_delay_ms == 0);
  CHECK(c.max_tokens_code == 32768);
  CHECK(c.ai_max_context_lines == 1);
  CHECK(c.panel_height == 100);
  for (const char* k : {"run_timeout_ms", "tab_width", "ai_timeout_ms", "compile_timeout_ms",
                        "run_output_limit", "tick_ms", "temperature_code",
                        "temperature_practice", "ghost_max_lines", "ghost_delay_ms",
                        "max_tokens_code", "ai_max_context_lines", "panel_height"})
    CHECK(warnHas(c, k));
  // 边界值本身不该报警
  const std::string p2 = writeFile("edge.json",
                                   "{\"tab_width\":16,\"tick_ms\":10,\"run_output_limit\":1024,"
                                   "\"temperature_code\":0,\"temperature_practice\":2}");
  std::string err2;
  const Config c2 = ConfigLoader::load(p2, err2);
  CHECK(c2.tab_width == 16);
  CHECK(c2.tick_ms == 10);
  CHECK(c2.run_output_limit == 1024u);
  CHECK(c2.temperature_code == 0.0);
  CHECK(c2.temperature_practice == 2.0);
  CHECK(!warnHas(c2, "超出允许范围"));
  // 小数给整数字段:截断 + 告警,但值可用
  const std::string p3 = writeFile("frac.json", "{\"tab_width\":4.7,\"panel_height\":9.99}");
  std::string err3;
  const Config c3 = ConfigLoader::load(p3, err3);
  CHECK(c3.tab_width == 4);
  CHECK(c3.panel_height == 9);
  CHECK(warnHas(c3, "截断"));
  // 巨大的 run_output_limit 不许溢出 size_t
  const std::string p4 = writeFile("huge.json", "{\"run_output_limit\":1e30}");
  std::string err4;
  const Config c4 = ConfigLoader::load(p4, err4);
  CHECK(c4.run_output_limit == 268435456u);
}

// ---------------------------------------------------------------- 6. 优先级

static void testEnvOverridesFile() {
  resetEnv();
  const std::string p = writeFile("prio.json",
                                  "{\"api_key\":\"FILEKEYAAA\",\"model\":\"file-model\","
                                  "\"base_url\":\"https://file.example\"}");
  {  // 只有文件
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.aiEnabled());
    CHECK_EQ_STR(c.model, "file-model");
    CHECK_EQ_STR(c.base_url, "https://file.example");
    CHECK(warnHasNone(c, "FILEKEY"));  // key 的值绝不进日志
  }
  {  // 环境变量赢
    resetEnv();
    putEnv("CPPIDE_API_KEY", "ENVKEYBBB");
    putEnv("CPPIDE_MODEL", "env-model");
    putEnv("CPPIDE_BASE_URL", "https://env.example");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.aiEnabled());
    CHECK_EQ_STR(c.api_key, "ENVKEYBBB");
    CHECK_EQ_STR(c.model, "env-model");
    CHECK_EQ_STR(c.base_url, "https://env.example");
    CHECK(warnHasNone(c, "ENVKEY"));
    CHECK(warnHasNone(c, "FILEKEY"));
  }
  {  // DEEPSEEK_API_KEY 是 CPPIDE_API_KEY 的兜底
    resetEnv();
    putEnv("DEEPSEEK_API_KEY", "DSKEYCCC");
    std::string err;
    const Config c = ConfigLoader::load(g_dir + "/nope.json", err);
    CHECK(c.aiEnabled());
    CHECK_EQ_STR(c.api_key, "DSKEYCCC");
    CHECK(warnHasNone(c, "DSKEY"));
    CHECK(!warnHas(c, "AI 未配置"));
  }
  {  // CPPIDE_API_KEY 优先于 DEEPSEEK_API_KEY
    resetEnv();
    putEnv("CPPIDE_API_KEY", "PRIMARYKEY");
    putEnv("DEEPSEEK_API_KEY", "FALLBACKKEY");
    std::string err;
    const Config c = ConfigLoader::load("", err);
    CHECK_EQ_STR(c.api_key, "PRIMARYKEY");
  }
  {  // 空的环境变量等于没设,不许把 key 抹掉
    resetEnv();
    putEnv("CPPIDE_API_KEY", "");
    putEnv("CPPIDE_MODEL", "  ");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.aiEnabled());
    CHECK_EQ_STR(c.api_key, "FILEKEYAAA");
    CHECK_EQ_STR(c.model, "file-model");
  }
  resetEnv();
}

static void testExplicitPathWins() {
  resetEnv();
  const std::string a = writeFile("from-env.json", "{\"tab_width\":3}");
  const std::string b = writeFile("from-flag.json", "{\"tab_width\":5}");
  putEnv("CPPIDE_CONFIG", a);
  {  // 空路径 => 走 CPPIDE_CONFIG
    std::string err;
    const Config c = ConfigLoader::load("", err);
    CHECK(c.tab_width == 3);
    CHECK_EQ_STR(c.source_path, a);
  }
  {  // --config 显式给出 => 赢过环境变量
    std::string err;
    const Config c = ConfigLoader::load(b, err);
    CHECK(c.tab_width == 5);
    CHECK_EQ_STR(c.source_path, b);
  }
  resetEnv();
}

static void testDefaultPathPriority() {
  resetEnv();
  putEnv("CPPIDE_CONFIG", "/tmp/explicit-cppide.json");
  putEnv("XDG_CONFIG_HOME", "/tmp/xdg");
  CHECK_EQ_STR(ConfigLoader::defaultPath(), "/tmp/explicit-cppide.json");

  ::unsetenv("CPPIDE_CONFIG");
  CHECK_EQ_STR(ConfigLoader::defaultPath(), "/tmp/xdg/cppide/config.json");

  ::unsetenv("XDG_CONFIG_HOME");
  putEnv("HOME", "/tmp/fakehome");
  CHECK_EQ_STR(ConfigLoader::defaultPath(), "/tmp/fakehome/.config/cppide/config.json");

  // XDG_CONFIG_HOME 带尾斜杠不许拼出 //
  putEnv("XDG_CONFIG_HOME", "/tmp/xdg2/");
  CHECK_EQ_STR(ConfigLoader::defaultPath(), "/tmp/xdg2/cppide/config.json");

  // 空/纯空白的环境变量视为未设置
  putEnv("XDG_CONFIG_HOME", "   ");
  CHECK_EQ_STR(ConfigLoader::defaultPath(), "/tmp/fakehome/.config/cppide/config.json");
  putEnv("CPPIDE_CONFIG", "");
  CHECK_EQ_STR(ConfigLoader::defaultPath(), "/tmp/fakehome/.config/cppide/config.json");
  resetEnv();
}

static void testTildeExpansion() {
  resetEnv();
  const std::string p = writeFile("tilde.json", "{\"tab_width\":7}");
  putEnv("HOME", g_dir);
  {  // --config "~/tilde.json"
    std::string err;
    const Config c = ConfigLoader::load("~/tilde.json", err);
    CHECK(c.tab_width == 7);
    CHECK_EQ_STR(c.source_path, p);
    CHECK(err.empty());
  }
  {  // CPPIDE_CONFIG="~/tilde.json"
    putEnv("CPPIDE_CONFIG", "~/tilde.json");
    CHECK_EQ_STR(ConfigLoader::defaultPath(), p);
    std::string err;
    const Config c = ConfigLoader::load("", err);
    CHECK(c.tab_width == 7);
  }
  {  // XDG_CONFIG_HOME 也走展开
    resetEnv();
    putEnv("HOME", g_dir);
    putEnv("XDG_CONFIG_HOME", "~/cfgroot");
    CHECK_EQ_STR(ConfigLoader::defaultPath(), g_dir + "/cfgroot/cppide/config.json");
  }
  {  // 路径中间的 ~ 不该被动:只展开开头的 "~/"
    resetEnv();
    putEnv("HOME", g_dir);
    std::string err;
    const Config c = ConfigLoader::load("/tmp/a~b/none.json", err);
    CHECK(!err.empty());
    CHECK(warnHas(c, "/tmp/a~b/none.json"));
  }
  resetEnv();
}

// ---------------------------------------------------------------- 7. chatUrl

static void testChatUrl() {
  const std::string want = "https://x.com/v1/chat/completions";
  for (const char* b : {"https://x.com", "https://x.com/", "https://x.com//",
                        "  https://x.com/  "}) {
    for (const char* p : {"v1/chat/completions", "/v1/chat/completions",
                          "//v1/chat/completions", "  /v1/chat/completions "}) {
      Config c;
      c.base_url = b;
      c.chat_path = p;
      CHECK_EQ_STR(c.chatUrl(), want);
    }
  }
  {  // 带端口和子路径的 base_url
    Config c;
    c.base_url = "http://127.0.0.1:8080/api/";
    c.chat_path = "/v1/chat/completions";
    CHECK_EQ_STR(c.chatUrl(), "http://127.0.0.1:8080/api/v1/chat/completions");
  }
  {  // chat_path 为空 => 只剩去掉尾斜杠的 base
    Config c;
    c.base_url = "https://x.com/";
    c.chat_path = "";
    CHECK_EQ_STR(c.chatUrl(), "https://x.com");
  }
  {  // chat_path 只有斜杠
    Config c;
    c.base_url = "https://x.com";
    c.chat_path = "///";
    CHECK_EQ_STR(c.chatUrl(), "https://x.com");
  }
  {  // base_url 为空:仍返回一个不含 "//" 的东西,不许崩
    Config c;
    c.base_url = "";
    c.chat_path = "/v1/chat";
    CHECK_EQ_STR(c.chatUrl(), "/v1/chat");
  }
  {  // 两个都空
    Config c;
    c.base_url = "";
    c.chat_path = "";
    CHECK(c.chatUrl().empty());
  }
  {  // 默认配置拼出来的必须是完整可用的 URL,且 "://" 之后没有 "//"
    const Config d;
    const std::string u = d.chatUrl();
    CHECK(u.find("://") != std::string::npos);
    const size_t after = u.find("://") + 3;
    CHECK(u.find("//", after) == std::string::npos);
  }
}

// ---------------------------------------------------------------- 8. sampleJson

static void testSampleJsonRoundTrip() {
  resetEnv();
  const std::string sample = ConfigLoader::sampleJson();
  CHECK(!sample.empty());
  // 生成的示例必须能被自己的 loader 原样吃回去
  const std::string p = writeFile("sample.json", sample);
  std::string err;
  const Config c = ConfigLoader::load(p, err);
  CHECK(err.empty());                       // 合法 JSON,无致命问题
  CHECK(!c.aiEnabled());                    // api_key 必须留空
  CHECK(c.api_key.empty());
  CHECK_EQ_STR(c.source_path, p);
  CHECK(isAllDefaults(c));                  // 示例值 == 内置默认值
  CHECK(!warnHas(c, "无法识别的字段"));      // 注释键与字段名都得被认识
  CHECK(!warnHas(c, "应为"));                // 没有任何类型错
  CHECK(!warnHas(c, "超出允许范围"));         // 没有任何越界
  CHECK(warnHas(c, "api_key"));             // 但要提示去填 key
  CHECK(warnHas(c, "官方文档"));             // 以及 model 只是占位

  // 示例里绝不能出现真 key;api_key 必须是空串
  CHECK(sample.find("\"api_key\": \"\"") != std::string::npos ||
        sample.find("\"api_key\":\"\"") != std::string::npos);
  CHECK(sample.find("sk-") == std::string::npos);
  // 每个可配置字段都该在示例里出现,否则用户不知道有这个开关
  for (const char* k : {"api_key", "base_url", "chat_path", "model", "stream",
                        "ghost_delay_ms", "ghost_min_interval_ms", "ghost_max_lines",
                        "ai_connect_timeout_ms", "ai_timeout_ms", "ai_max_context_lines",
                        "max_tokens_code", "max_tokens_practice", "temperature_code",
                        "temperature_practice", "prompt_code", "prompt_practice", "cc", "cxx",
                        "cflags", "cxxflags", "compile_timeout_ms", "run_timeout_ms",
                        "run_output_limit", "stdin_file", "tab_width", "expand_tab",
                        "auto_indent", "show_line_numbers", "panel_height", "tick_ms"})
    CHECK(sample.find(std::string("\"") + k + "\"") != std::string::npos);
  // 两次调用结果一致(README 与 --print-config 共用,不能有随机顺序)
  CHECK_EQ_STR(ConfigLoader::sampleJson(), sample);
  // 环境变量不该污染示例
  putEnv("CPPIDE_API_KEY", "SHOULDNOTAPPEAR");
  CHECK(ConfigLoader::sampleJson().find("SHOULDNOTAPPEAR") == std::string::npos);
  resetEnv();
}

// ---------------------------------------------------------------- 9. api_key 卫生

static void testApiKeyHygiene() {
  resetEnv();
  {  // 粘贴时带进来的换行/空格要被去掉,key 照常可用
    const std::string p = writeFile("key-ws.json", "{\"api_key\":\"  GOODKEY123  \"}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.aiEnabled());
    CHECK_EQ_STR(c.api_key, "GOODKEY123");
    CHECK(warnHasNone(c, "GOODKEY"));
  }
  {  // key 中间有空格/不可见字符:丢掉 + 告警,且告警里没有它的任何片段
    // 用不会与提示文案(里面本来就有 "CPPIDE_API_KEY" 字样)撞车的独特值来验证不泄漏
    const std::string p = writeFile("key-bad.json", "{\"api_key\":\"Zq1AAA Zq2BBB\\u0007\"}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(!c.aiEnabled());
    CHECK(c.api_key.empty());
    CHECK(warnHas(c, "api_key"));
    CHECK(warnHasNone(c, "Zq1"));
    CHECK(warnHasNone(c, "Zq2"));
    CHECK(warnHasNone(c, "\x07"));
    CHECK(err.find("Zq1") == std::string::npos);
  }
  {  // 环境变量里的坏 key 同样要被挡住
    resetEnv();
    putEnv("CPPIDE_API_KEY", "Zq3CCC Zq4DDD");
    std::string err;
    const Config c = ConfigLoader::load("", err);
    CHECK(!c.aiEnabled());
    CHECK(warnHasNone(c, "Zq3"));
    CHECK(warnHasNone(c, "Zq4"));
  }
  resetEnv();
}

// ---------------------------------------------------------------- 10. 源码自检

// 需求硬要求:代码里不出现任何硬编码 key。这条用测试钉住,而不是靠人看。
static void testNoHardcodedKeyInSource() {
  const char* env_dir = ::getenv("CPPIDE_SRC_DIR");
  std::vector<std::string> cands;
#ifdef CONFIG_CPP_PATH
  cands.push_back(CONFIG_CPP_PATH);
#endif
  if (env_dir && *env_dir) cands.push_back(std::string(env_dir) + "/config.cpp");
  cands.push_back("src/config.cpp");
  cands.push_back("../src/config.cpp");
  cands.push_back("/work/src/config.cpp");

  std::string src;
  bool found = false;
  for (const std::string& p : cands) {
    bool ok = false;
    src = readWholeFile(p, ok);
    if (ok && !src.empty()) { found = true; break; }
  }
  CHECK(found);  // 找不到源码就算失败:这条断言不许被静默跳过

  CHECK(src.find("sk-") == std::string::npos);
  CHECK(src.find("sk_") == std::string::npos);
  CHECK(src.find("Bearer ") == std::string::npos);  // 拼 Authorization 是 aihttp 的事
  // 任何长度 >= 28 的 key 样字面量都视为可疑
  const std::string kw = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-";
  size_t run = 0, longest = 0;
  for (char ch : src) {
    if (kw.find(ch) != std::string::npos) { ++run; if (run > longest) longest = run; }
    else run = 0;
  }
  CHECK(longest < 28);
}

// ---------------------------------------------------------------- 11. 杂项健壮性

static void testMiscRobustness() {
  resetEnv();
  {  // 只有注释键的配置:合法,全默认
    const std::string p = writeFile("only-comment.json", "{\"_comment\":\"hi\"}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(err.empty());
    CHECK(isAllDefaults(c));
    CHECK_EQ_STR(c.source_path, p);
  }
  {  // 空对象
    const std::string p = writeFile("empty-obj.json", "{}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(err.empty());
    CHECK(isAllDefaults(c));
  }
  {  // warnings 必须有上界:几百个未知字段不许淹掉 UI
    std::string t = "{";
    for (int i = 0; i < 500; ++i) {
      if (i) t += ",";
      t += "\"bogus" + std::to_string(i) + "\":1";
    }
    t += "}";
    const std::string p = writeFile("flood.json", t);
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.warnings.size() <= 64);
    CHECK(!c.warnings.empty());
  }
  {  // stdin_file 的 ~ 也要展开
    resetEnv();
    putEnv("HOME", g_dir);
    const std::string p = writeFile("stdin.json", "{\"stdin_file\":\"~/in.txt\"}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK_EQ_STR(c.stdin_file, g_dir + "/in.txt");
  }
  {  // UTF-8 中文提示词与路径不许被截断/破坏
    resetEnv();
    const std::string p = writeFile("utf8.json",
                                    "{\"prompt_practice\":\"你是 ICPC 教练,请用中文回答。\","
                                    "\"cc\":\"clang\"}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK_EQ_STR(c.prompt_practice, "你是 ICPC 教练,请用中文回答。");
    CHECK_EQ_STR(c.cc, "clang");
  }
  {  // 同一个键重复出现:后者生效,不崩
    const std::string p = writeFile("dup.json", "{\"tab_width\":2,\"tab_width\":8}");
    std::string err;
    const Config c = ConfigLoader::load(p, err);
    CHECK(c.tab_width == 8);
  }
  {  // 反复加载同一份配置结果稳定(没有全局可变状态)
    const std::string p = writeFile("stable.json", "{\"tab_width\":6,\"cc\":\"gcc\"}");
    std::string e1, e2;
    const Config c1 = ConfigLoader::load(p, e1);
    const Config c2 = ConfigLoader::load(p, e2);
    CHECK(c1.tab_width == c2.tab_width);
    CHECK(c1.warnings.size() == c2.warnings.size());
    CHECK_EQ_STR(e1, e2);
  }
  {  // err 是输出参数:进来时的脏值必须被清掉
    const std::string p = writeFile("clean-err.json", "{}");
    std::string err = "旧的脏数据";
    const Config c = ConfigLoader::load(p, err);
    CHECK(err.empty());
    (void)c;
  }
  resetEnv();
}

// ---------------------------------------------------------------- main

int main() {
  const char* h = ::getenv("HOME");
  g_realhome = h ? h : "/tmp";

  char tmpl[] = "/tmp/cppide_cfgtest_XXXXXX";
  const char* d = ::mkdtemp(tmpl);
  CHECK(d != nullptr);
  g_dir = d;

  testMissingFile();
  testMissingFileViaDefaultPath();
  testDirectoryAsConfig();
  testBrokenJson();
  testFieldTypeErrors();
  testEmptyStringFallback();
  testUnknownKeys();
  testArrayFields();
  testExtremeNumbers();
  testEnvOverridesFile();
  testExplicitPathWins();
  testDefaultPathPriority();
  testTildeExpansion();
  testChatUrl();
  testSampleJsonRoundTrip();
  testApiKeyHygiene();
  testNoHardcodedKeyInSource();
  testMiscRobustness();

  std::printf("test_config: OK (%d 条断言)\n", g_checks);
  return 0;
}

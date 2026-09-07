// test_json.cpp —— mj::Value 的自检(纯 assert;main 返回 0 即通过)
//
// 覆盖:往返 / DeepSeek 响应体 / 转义(中文·引号·换行·emoji·代理对)/
//       恶意输入(空串·截断·万层嵌套·超长数字·裸 \u)/ 数字边界 / 越界安全。
//
// 编译:g++ -std=c++17 -O2 -Wall -Wextra tests/test_json.cpp src/json.cpp
// 必须在 -fsanitize=address,undefined 下同样通过。

#undef NDEBUG  // 断言在 -O2 -DNDEBUG 下也要生效
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../src/json.h"

using mj::Type;
using mj::Value;

static int g_checks = 0;
static int g_cases = 0;

#define CK(cond)                                                       \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,     \
                   #cond);                                             \
      assert(false && #cond);                                          \
      return 1;                                                        \
    }                                                                  \
  } while (0)

#define CASE(name)                     \
  do {                                 \
    ++g_cases;                         \
    std::fprintf(stderr, "  [%02d] %s\n", g_cases, name); \
  } while (0)

// 结构等价(数字比 double + int64,不比字面量文本)
static bool eq(const Value& a, const Value& b) {
  if (a.type() != b.type()) return false;
  switch (a.type()) {
    case Type::Null: return true;
    case Type::Bool: return a.asBool() == b.asBool();
    case Type::Number: {
      double x = a.asNumber(), y = b.asNumber();
      if (std::isnan(x) || std::isnan(y)) return false;
      return x == y && a.asInt64() == b.asInt64();
    }
    case Type::String: return a.asString() == b.asString();
    case Type::Array: {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i)
        if (!eq(a.value(i), b.value(i))) return false;
      return true;
    }
    case Type::Object: {
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); ++i) {
        if (a.key(i) != b.key(i)) return false;  // 保序
        if (!eq(a.value(i), b.value(i))) return false;
      }
      return true;
    }
  }
  return false;
}

// parse -> dump -> parse,要求两次的值结构等价;返回是否成功
static bool roundtrip(const std::string& text, std::string* why) {
  std::string e1, e2;
  Value a = Value::parse(text, e1);
  if (!e1.empty()) {
    *why = "首次解析失败: " + e1;
    return false;
  }
  std::string d = a.dump();
  Value b = Value::parse(d, e2);
  if (!e2.empty()) {
    *why = "dump 产物不可解析: " + e2 + " | dump=" + d;
    return false;
  }
  if (!eq(a, b)) {
    *why = "往返后结构不等价 | dump=" + d;
    return false;
  }
  // 美化输出也必须是合法 JSON 且等价
  std::string p = a.dump(2);
  std::string e3;
  Value c = Value::parse(p, e3);
  if (!e3.empty()) {
    *why = "dump(2) 产物不可解析: " + e3;
    return false;
  }
  if (!eq(a, c)) {
    *why = "dump(2) 往返后结构不等价";
    return false;
  }
  return true;
}

int main() {
  std::string err, why;

  // ---------------------------------------------------------------- 1 ----
  CASE("标量类型与取值");
  {
    Value n;
    CK(n.isNull() && n.type() == Type::Null);
    CK(Value(nullptr).isNull());
    CK(Value(true).isBool() && Value(true).asBool() == true);
    CK(Value(false).asBool(true) == false);
    CK(Value(3.5).isNumber() && Value(3.5).asNumber() == 3.5);
    CK(Value(42).asInt() == 42);
    CK(Value(static_cast<int64_t>(-7)).asInt64() == -7);
    CK(Value("hi").isString() && Value("hi").asString() == "hi");
    CK(Value(static_cast<const char*>(nullptr)).asString().empty());
    CK(Value(std::string("ab")).asString() == "ab");
    // 类型不匹配 -> 返回 def,不抛
    CK(Value("x").asBool(true) == true);
    CK(Value("x").asNumber(-1.0) == -1.0);
    CK(Value("x").asInt(9) == 9);
    CK(Value("x").asInt64(9) == 9);
    CK(Value(1.0).asString().empty());
    CK(Value(1.0).asString("d") == "d");
    CK(Value::array().isArray() && Value::array().size() == 0);
    CK(Value::object().isObject() && Value::object().size() == 0);
  }

  // ---------------------------------------------------------------- 2 ----
  CASE("越界与错类型访问全部安全");
  {
    Value v;  // null
    CK(v.size() == 0);
    CK(v.value(0).isNull() && v.value(99999).isNull());
    CK(v[static_cast<size_t>(3)].isNull());
    CK(v.key(0).empty());
    CK(!v.has("a"));
    CK(v.get("a").isNull());
    CK(v["a"].isNull());
    Value o = Value::object();
    o.set("a", 1);
    CK(o.key(0) == "a" && o.key(1).empty());
    CK(o.value(1).isNull());
    Value a = Value::array();
    a.push(1);
    CK(a.key(0).empty());        // 非 object 取键
    CK(a.get("a").isNull());     // 非 object 取键值
    CK(!a.has("a"));
    // 深链式:全程不存在也不崩
    CK(v["x"]["y"][static_cast<size_t>(0)]["z"].asString("def") == "def");
  }

  // ---------------------------------------------------------------- 3 ----
  CASE("对象保序 / 覆盖 / push / clearContents");
  {
    Value o = Value::object();
    o.set("z", 1);
    o.set("a", 2);
    o.set("m", 3);
    CK(o.size() == 3);
    CK(o.key(0) == "z" && o.key(1) == "a" && o.key(2) == "m");
    o.set("a", 99);  // 覆盖,不改顺序
    CK(o.size() == 3 && o.key(1) == "a" && o["a"].asInt() == 99);
    CK(o.dump() == "{\"z\":1,\"a\":99,\"m\":3}");

    Value v(1.0);
    v.push(7);  // 非数组 -> 先变空数组
    CK(v.isArray() && v.size() == 1 && v[static_cast<size_t>(0)].asInt() == 7);
    Value w("str");
    w.set("k", 1);  // 非对象 -> 先变空对象
    CK(w.isObject() && w.size() == 1 && w["k"].asInt() == 1);

    Value c = Value::object();
    c.set("a", 1);
    c.clearContents();
    CK(c.isObject() && c.size() == 0 && c.dump() == "{}");
    Value ca = Value::array();
    ca.push(1);
    ca.clearContents();
    CK(ca.isArray() && ca.size() == 0 && ca.dump() == "[]");
  }

  // ---------------------------------------------------------------- 4 ----
  CASE("基础解析 + 空白 + BOM");
  {
    Value v = Value::parse("  \r\n\t {\n \"a\" : [ 1 , 2 , null ] }  ", err);
    CK(err.empty());
    CK(v.isObject() && v["a"].isArray() && v["a"].size() == 3);
    CK(v["a"][static_cast<size_t>(2)].isNull());
    Value b = Value::parse("\xEF\xBB\xBF{\"a\":1}", err);  // UTF-8 BOM
    CK(err.empty() && b["a"].asInt() == 1);
    CK(Value::parse("true", err).asBool() == true && err.empty());
    CK(Value::parse("false", err).isBool() && err.empty());
    CK(Value::parse("null", err).isNull() && err.empty());
    CK(Value::parse("\"s\"", err).asString() == "s" && err.empty());
    CK(Value::parse("[]", err).isArray() && err.empty());
    CK(Value::parse("{}", err).isObject() && err.empty());
    CK(Value::parse("0", err).asInt() == 0 && err.empty());
    // 重复键:后者覆盖
    Value d = Value::parse("{\"a\":1,\"a\":2}", err);
    CK(err.empty() && d.size() == 1 && d["a"].asInt() == 2);
  }

  // ---------------------------------------------------------------- 5 ----
  CASE("往返:parse -> dump -> parse 等价");
  {
    const char* docs[] = {
        "null", "true", "false", "0", "-1", "3.25", "\"\"", "[]", "{}",
        "[1,2,3]",
        "[[[1]],[{\"a\":[]}],null,true]",
        "{\"a\":1,\"b\":[1,2,{\"c\":\"x\"}],\"d\":{\"e\":null},\"f\":false}",
        "{\"k\":\"含中文与\\\"引号\\\"和\\n换行\"}",
        "{\"emoji\":\"\\ud83d\\ude00\\u4e2d\\u00e9\"}",
        "[1e10,1.5e-10,-0.5,123456789012345,1e308]",
        "{\"deep\":{\"a\":{\"b\":{\"c\":{\"d\":[1,[2,[3,[4]]]]}}}}}",
        "{\"\":\"空键也合法\"}",
        "[\"\\u0000\\u001f\\t\",\"\\\\\",\"/\"]",
    };
    for (const char* t : docs) {
      if (!roundtrip(t, &why)) {
        std::fprintf(stderr, "FAIL roundtrip [%s]: %s\n", t, why.c_str());
        CK(false);
      }
      ++g_checks;
    }
  }

  // ---------------------------------------------------------------- 6 ----
  CASE("DeepSeek 流式增量 delta.content");
  {
    const char* body =
        "{\"id\":\"chatcmpl-1\",\"object\":\"chat.completion.chunk\","
        "\"created\":1700000000,\"model\":\"deepseek-chat\","
        "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"int main\"},"
        "\"finish_reason\":null}]}";
    Value v = Value::parse(body, err);
    CK(err.empty());
    CK(v["choices"].isArray() && v["choices"].size() == 1);
    const Value& ch0 = v["choices"][static_cast<size_t>(0)];
    CK(ch0["delta"]["content"].asString() == "int main");
    CK(ch0["finish_reason"].isNull());
    CK(v["created"].asInt64() == 1700000000);
    CK(v["model"].asString() == "deepseek-chat");
    // 缺字段时链式取值给默认
    CK(v["choices"][static_cast<size_t>(0)]["message"]["content"]
           .asString("")
           .empty());
    CK(v["choices"][static_cast<size_t>(9)]["delta"]["content"]
           .asString("fallback") == "fallback");
    // [DONE] 哨兵不是 JSON,必须优雅失败
    Value done = Value::parse("[DONE]", err);
    CK(!err.empty() && done.isNull());
  }

  // ---------------------------------------------------------------- 7 ----
  CASE("DeepSeek 非流式 message.content");
  {
    const char* body =
        "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
        "\"content\":\"第一行\\n第二行\\t制表\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":34,"
        "\"total_tokens\":46}}";
    Value v = Value::parse(body, err);
    CK(err.empty());
    const Value& m = v["choices"][static_cast<size_t>(0)]["message"];
    CK(m["role"].asString() == "assistant");
    CK(m["content"].asString() == "第一行\n第二行\t制表");
    CK(v["choices"][static_cast<size_t>(0)]["finish_reason"].asString() ==
       "stop");
    CK(v["usage"]["total_tokens"].asInt() == 46);
    CK(roundtrip(body, &why));
  }

  // ---------------------------------------------------------------- 8 ----
  CASE("DeepSeek 错误体 error.message");
  {
    const char* body =
        "{\"error\":{\"message\":\"Authentication Fails, Your api key: "
        "****xxxx is invalid\",\"type\":\"authentication_error\","
        "\"param\":null,\"code\":\"invalid_request_error\"}}";
    Value v = Value::parse(body, err);
    CK(err.empty());
    CK(v["error"].isObject());
    CK(v["error"]["message"].asString().find("Authentication Fails") == 0);
    CK(v["error"]["type"].asString() == "authentication_error");
    CK(v["error"]["param"].isNull());
    CK(!v.has("choices"));
    CK(v["choices"][static_cast<size_t>(0)]["delta"]["content"]
           .asString()
           .empty());
    CK(roundtrip(body, &why));
  }

  // ---------------------------------------------------------------- 9 ----
  CASE("请求体生成:转义正确(中文/引号/换行/反斜杠/控制字符)");
  {
    Value msg = Value::object();
    msg.set("role", "user");
    msg.set("content",
            std::string("请解释这段代码:\nprintf(\"a\\tb\");\t"
                        "路径 C:\\tmp\x01"));
    Value arr = Value::array();
    arr.push(msg);
    Value req = Value::object();
    req.set("model", "deepseek-chat");
    req.set("messages", arr);
    req.set("stream", true);
    req.set("max_tokens", 2048);
    req.set("temperature", 0.2);
    std::string body = req.dump();
    // 生成物必须能被自己解回来,且内容一模一样
    Value back = Value::parse(body, err);
    CK(err.empty());
    CK(back["messages"][static_cast<size_t>(0)]["content"].asString() ==
       msg["content"].asString());
    CK(back["stream"].asBool() == true);
    CK(back["max_tokens"].asInt() == 2048);
    CK(back["temperature"].asNumber() == 0.2);
    // 关键转义必须出现,且不能有裸的换行/制表/控制字符
    CK(body.find("\\n") != std::string::npos);
    CK(body.find("\\t") != std::string::npos);
    CK(body.find("\\\"") != std::string::npos);
    CK(body.find("\\\\") != std::string::npos);
    CK(body.find("\\u0001") != std::string::npos);
    for (unsigned char c : body) CK(c >= 0x20 || c == 0x00);
    // 中文原样透传(不转成 \u)
    CK(body.find("请解释这段代码") != std::string::npos);
    CK(body.find("\\u8bf7") == std::string::npos);
  }

  // --------------------------------------------------------------- 10 ----
  CASE("escape() 逐项");
  {
    CK(Value::escape("") == "");
    CK(Value::escape("abc") == "abc");
    CK(Value::escape("a\"b") == "a\\\"b");
    CK(Value::escape("a\\b") == "a\\\\b");
    CK(Value::escape("a\nb") == "a\\nb");
    CK(Value::escape("a\rb") == "a\\rb");
    CK(Value::escape("a\tb") == "a\\tb");
    CK(Value::escape("a\bb") == "a\\bb");
    CK(Value::escape("a\fb") == "a\\fb");
    CK(Value::escape(std::string("a\0b", 3)) == "a\\u0000b");
    CK(Value::escape("\x1f") == "\\u001f");
    CK(Value::escape("/") == "/");            // 不必转义斜杠
    CK(Value::escape("中文") == "中文");        // UTF-8 透传
    CK(Value::escape("😀") == "😀");           // 4 字节透传
    CK(Value::escape("\x7f") == "\x7f");       // DEL 不是控制字符范围内(<0x20)
  }

  // --------------------------------------------------------------- 11 ----
  CASE("\\uXXXX 解码:BMP / 中文 / 代理对 / 孤立代理");
  {
    Value v = Value::parse("\"\\u0041\\u00e9\\u4e2d\\u0000\"", err);
    CK(err.empty());
    const std::string& s = v.asString();
    CK(s.size() == 1 + 2 + 3 + 1);
    CK(s.compare(0, 1, "A") == 0);
    CK(s.compare(1, 2, "\xC3\xA9") == 0);      // é
    CK(s.compare(3, 3, "\xE4\xB8\xAD") == 0);  // 中
    CK(s[6] == '\0');

    // 代理对 -> 4 字节 UTF-8 的 😀 (U+1F600)
    Value e = Value::parse("\"\\ud83d\\ude00\"", err);
    CK(err.empty());
    CK(e.asString() == "\xF0\x9F\x98\x80");
    CK(e.asString() == "😀");
    CK(e.asString().size() == 4);
    // 大写十六进制同样接受
    CK(Value::parse("\"\\uD83D\\uDE00\"", err).asString() == "😀");
    CK(err.empty());
    // U+10FFFF 边界
    CK(Value::parse("\"\\udbff\\udfff\"", err).asString() ==
       "\xF4\x8F\xBF\xBF");
    CK(err.empty());
    // 孤立高代理 / 孤立低代理 / 高代理后跟普通转义 -> U+FFFD,不报错不越界
    CK(Value::parse("\"\\ud83d\"", err).asString() == "\xEF\xBF\xBD");
    CK(err.empty());
    CK(Value::parse("\"\\ude00\"", err).asString() == "\xEF\xBF\xBD");
    CK(err.empty());
    CK(Value::parse("\"\\ud83dA\"", err).asString() == "\xEF\xBF\xBD" "A");
    CK(err.empty());
    CK(Value::parse("\"\\ud83d\\n\"", err).asString() == "\xEF\xBF\xBD" "\n");
    CK(err.empty());
    CK(Value::parse("\"\\ud83d\\u0041\"", err).asString() ==
       "\xEF\xBF\xBD" "A");
    CK(err.empty());
    // 4 字节 emoji 原样输入也要能往返
    Value raw = Value::parse("\"a😀b\"", err);
    CK(err.empty() && raw.asString() == "a😀b");
    CK(raw.dump() == "\"a😀b\"");
    CK(roundtrip("\"a😀b中\"", &why));
  }

  // --------------------------------------------------------------- 12 ----
  CASE("恶意 / 畸形输入必须优雅失败");
  {
    std::vector<std::string> bad = {
        "",  " ",  "\n\t ", "\xEF\xBB\xBF",          // 空 / 只有空白 / 只有 BOM
        "{", "[",  "}",     "]", ",", ":",           // 只有开括号/裸符号
        "{\"a\"", "{\"a\":", "{\"a\":1", "{\"a\":}", "{\"a\"}", "{\"a\",1}",
        "{a:1}", "{'a':1}", "{1:2}", "{\"a\":1,}", "{\"a\":1,,\"b\":2}",
        "[1", "[1,", "[1,]", "[,1]", "[1 2]", "[1;2]",
        "\"abc",        // 未闭合字符串
        "\"abc\\",      // 转义被截断
        "\"a\nb\"",     // 字符串里裸换行(控制字符)
        "\"a\x01\"",    // 字符串里裸控制字符
        "\"\\u\"", "\"\\u1\"", "\"\\u12\"", "\"\\u123\"", "\"\\uZZZZ\"",
        "\"\\u12",      // \u 直接被截断在末尾
        "\"\\u",        // 裸 \u 截断
        "\"\\q\"",      // 未知转义
        "tru", "truex", "fals", "nul", "nulll", "TRUE", "None", "undefined",
        "NaN", "Infinity", "-Infinity", "nan", "inf",
        "01", "00", "-01", "+1", ".5", "-.5", "1.", "1.e5", "1e", "1e+",
        "1e+-2", "--1", "-", "0x10", "1,2", "1 1",
        "{\"a\":1}}", "[1]]", "[]{}", "null null", "\"a\" \"b\"",
        "[DONE]", "data: {\"a\":1}",
    };
    for (const std::string& t : bad) {
      std::string e;
      Value v = Value::parse(t, e);
      if (e.empty() || !v.isNull()) {
        std::fprintf(stderr, "FAIL 应当失败但通过了: [%s] err=[%s]\n",
                     t.c_str(), e.c_str());
        CK(false);
      }
      ++g_checks;
    }
  }

  // --------------------------------------------------------------- 13 ----
  CASE("深嵌套:达标层数可解析,超限优雅失败(不爆栈)");
  {
    // 正常深度(100 层数组 / 100 层对象)必须成功
    std::string ok = std::string(100, '[') + "1" + std::string(100, ']');
    Value v = Value::parse(ok, err);
    CK(err.empty() && v.isArray());
    std::string oo;
    for (int i = 0; i < 100; ++i) oo += "{\"a\":";
    oo += "1";
    for (int i = 0; i < 100; ++i) oo += "}";
    CK(Value::parse(oo, err).isObject() && err.empty());

    // 10000 层不闭合
    std::string deep(10000, '[');
    Value d1 = Value::parse(deep, err);
    CK(!err.empty() && d1.isNull());
    // 10000 层完整闭合
    std::string deep2 = std::string(10000, '[') + std::string(10000, ']');
    Value d2 = Value::parse(deep2, err);
    CK(!err.empty() && d2.isNull());
    // 10000 层对象
    std::string deep3;
    for (int i = 0; i < 10000; ++i) deep3 += "{\"a\":";
    deep3 += "1";
    for (int i = 0; i < 10000; ++i) deep3 += "}";
    Value d3 = Value::parse(deep3, err);
    CK(!err.empty() && d3.isNull());
    // 混合 100000 层
    std::string deep4;
    for (int i = 0; i < 50000; ++i) deep4 += "[{\"k\":";
    Value d4 = Value::parse(deep4, err);
    CK(!err.empty() && d4.isNull());
    // 错误串必须带位置信息
    CK(err.find("第") != std::string::npos && err.find("列") !=
       std::string::npos);
  }

  // --------------------------------------------------------------- 14 ----
  CASE("超长 / 病态数字");
  {
    std::string big = "1" + std::string(10000, '0');  // 10001 位整数
    Value v = Value::parse(big, err);
    CK(!err.empty() && v.isNull());
    std::string big2 = std::string(400, '9');
    CK(Value::parse(big2, err).isNull() && !err.empty());
    CK(Value::parse("1e309", err).isNull() && !err.empty());
    CK(Value::parse("-1e400", err).isNull() && !err.empty());
    CK(Value::parse("1e99999999999999999999", err).isNull() && !err.empty());
    // 下溢当 0(不报错)
    Value u = Value::parse("1e-400", err);
    CK(err.empty() && u.isNumber() && u.asNumber() == 0.0);
    Value u2 = Value::parse("-1e-400", err);
    CK(err.empty() && u2.asNumber() == 0.0);
    // 超长但尾数为 0 的小数
    Value z = Value::parse("0." + std::string(5000, '0'), err);
    CK(err.empty() && z.asNumber() == 0.0);
    // 超长小数位(在范围内)不该报错
    Value m = Value::parse("1." + std::string(5000, '1'), err);
    CK(err.empty() && m.asNumber() > 1.0 && m.asNumber() < 1.2);
  }

  // --------------------------------------------------------------- 15 ----
  CASE("数字边界:-0 / 1e308 / int64 极值 / 1.5e-10");
  {
    // -0:值为 0 但符号位保留,dump 仍是 -0
    Value nz = Value::parse("-0", err);
    CK(err.empty() && nz.isNumber());
    CK(nz.asNumber() == 0.0 && std::signbit(nz.asNumber()));
    CK(nz.asInt64() == 0 && nz.asInt() == 0);
    CK(nz.dump() == "-0");
    Value nzf = Value::parse("-0.0", err);
    CK(err.empty() && nzf.asNumber() == 0.0 && std::signbit(nzf.asNumber()));
    CK(Value::parse("0", err).dump() == "0" && err.empty());

    // 1e308
    Value big = Value::parse("1e308", err);
    CK(err.empty() && big.asNumber() == 1e308);
    CK(std::isfinite(big.asNumber()));
    CK(roundtrip("1e308", &why));
    CK(Value::parse(big.dump(), err).asNumber() == 1e308 && err.empty());
    CK(big.asInt64() == std::numeric_limits<int64_t>::max());  // 夹住不 UB
    CK(big.asInt() == std::numeric_limits<int>::max());
    Value nbig = Value::parse("-1e308", err);
    CK(err.empty());
    CK(nbig.asInt64() == std::numeric_limits<int64_t>::min());
    CK(nbig.asInt() == std::numeric_limits<int>::min());

    // int64 极值:必须精确,不能走 double 丢精度
    Value mx = Value::parse("9223372036854775807", err);
    CK(err.empty() && mx.isNumber());
    CK(mx.asInt64() == std::numeric_limits<int64_t>::max());
    CK(mx.dump() == "9223372036854775807");
    Value mn = Value::parse("-9223372036854775808", err);
    CK(err.empty());
    CK(mn.asInt64() == std::numeric_limits<int64_t>::min());
    CK(mn.dump() == "-9223372036854775808");
    // 超出 int64 但在 double 范围内:夹住,不 UB,dump 仍合法
    Value ov = Value::parse("9223372036854775808", err);
    CK(err.empty() && ov.asInt64() == std::numeric_limits<int64_t>::max());
    Value ov2 = Value::parse("99999999999999999999", err);  // 20 位
    CK(err.empty() && ov2.asInt64() == std::numeric_limits<int64_t>::max());
    CK(Value::parse(ov2.dump(), err).isNumber() && err.empty());
    // 常规大整数精确往返
    Value ts = Value::parse("1700000000123", err);
    CK(err.empty() && ts.asInt64() == 1700000000123LL);
    CK(ts.dump() == "1700000000123");
    CK(Value(static_cast<int64_t>(4503599627370497LL)).dump() ==
       "4503599627370497");
    CK(Value(static_cast<int64_t>(std::numeric_limits<int64_t>::min()))
           .asInt64() == std::numeric_limits<int64_t>::min());

    // 1.5e-10
    Value tiny = Value::parse("1.5e-10", err);
    CK(err.empty() && tiny.asNumber() == 1.5e-10);
    CK(tiny.asInt64() == 0 && tiny.asInt() == 0);
    CK(roundtrip("1.5e-10", &why));
    CK(Value::parse(tiny.dump(), err).asNumber() == 1.5e-10 && err.empty());

    // 其它:大小写 E、正负指数、截断取整
    CK(Value::parse("1E2", err).asNumber() == 100.0 && err.empty());
    CK(Value::parse("1e+2", err).asNumber() == 100.0 && err.empty());
    CK(Value::parse("-2.75", err).asInt() == -2 && err.empty());  // 向零截断
    CK(Value::parse("2.75", err).asInt() == 2 && err.empty());
    CK(Value::parse("1e-323", err).asNumber() > 0.0 && err.empty());
    CK(Value(std::numeric_limits<double>::quiet_NaN()).dump() == "0");
    CK(Value(std::numeric_limits<double>::infinity()).dump() == "0");
    CK(Value(std::numeric_limits<double>::quiet_NaN()).asInt64(-5) == -5);
  }

  // --------------------------------------------------------------- 16 ----
  CASE("dump 缩进美化");
  {
    Value o = Value::object();
    o.set("a", 1);
    Value arr = Value::array();
    arr.push(1);
    arr.push("x");
    o.set("b", arr);
    o.set("c", Value::object());
    o.set("d", Value::array());
    CK(o.dump() == "{\"a\":1,\"b\":[1,\"x\"],\"c\":{},\"d\":[]}");
    CK(o.dump(2) ==
       "{\n"
       "  \"a\": 1,\n"
       "  \"b\": [\n"
       "    1,\n"
       "    \"x\"\n"
       "  ],\n"
       "  \"c\": {},\n"
       "  \"d\": []\n"
       "}");
    CK(Value::array().dump(2) == "[]");
    CK(Value().dump(2) == "null");
    CK(o.dump(-1) == o.dump(0));  // 负 indent 视作紧凑
  }

  // --------------------------------------------------------------- 17 ----
  CASE("拷贝 / 移动 / 自赋值");
  {
    std::string body = "{\"a\":[1,{\"b\":\"x\"}],\"c\":true}";
    Value v = Value::parse(body, err);
    CK(err.empty());
    Value cp = v;                       // 拷贝
    CK(eq(cp, v) && cp.dump() == v.dump());
    Value mv = std::move(cp);           // 移动
    CK(eq(mv, v));
    Value asg;
    asg = v;
    CK(eq(asg, v));
    Value big = Value::parse("[[1,2],[3,4]]", err);
    CK(err.empty());
    big.push(big[static_cast<size_t>(0)]);  // 自身子树入自身
    CK(big.size() == 3 && big[static_cast<size_t>(2)].size() == 2);
  }

  // --------------------------------------------------------------- 18 ----
  CASE("config.json 样式文档端到端");
  {
    const char* cfg =
        "{\n"
        "  \"api_key\": \"sk-abcdef\",\n"
        "  \"base_url\": \"https://api.deepseek.com/v1\",\n"
        "  \"model\": \"deepseek-chat\",\n"
        "  \"mode\": \"practice\",\n"
        "  \"tab_width\": 4,\n"
        "  \"temperature\": 0.3,\n"
        "  \"stream\": true,\n"
        "  \"timeout_ms\": 30000,\n"
        "  \"extra\": {\"headers\": [], \"retries\": 0}\n"
        "}\n";
    Value v = Value::parse(cfg, err);
    CK(err.empty());
    CK(v["api_key"].asString() == "sk-abcdef");
    CK(v["tab_width"].asInt(8) == 4);
    CK(v["temperature"].asNumber(0.7) == 0.3);
    CK(v["stream"].asBool(false) == true);
    CK(v["timeout_ms"].asInt64(0) == 30000);
    CK(v["missing"].asInt(7) == 7);
    CK(v["extra"]["retries"].asInt(9) == 0);
    CK(v["extra"]["headers"].isArray() && v["extra"]["headers"].size() == 0);
    CK(v.size() == 9);
    CK(roundtrip(cfg, &why));
    // 语法错误要带行号(第 3 行缺逗号)
    std::string broken = "{\n  \"a\": 1\n  \"b\": 2\n}\n";
    Value bv = Value::parse(broken, err);
    CK(bv.isNull() && !err.empty());
    CK(err.find("第 3 行") != std::string::npos);
  }

  // --------------------------------------------------------------- 19 ----
  CASE("字符串里的 NUL 与二进制安全");
  {
    std::string withNul("a\0b\0", 4);
    Value v(withNul);
    CK(v.asString().size() == 4);
    std::string d = v.dump();
    CK(d == "\"a\\u0000b\\u0000\"");
    Value back = Value::parse(d, err);
    CK(err.empty() && back.asString() == withNul);
    CK(back.asString().size() == 4);
    // 输入文本本身含内嵌 NUL(std::string 长度精确,不靠 '\0' 终止)
    std::string txt("[1]\0garbage", 11);
    Value g = Value::parse(txt, err);
    CK(g.isNull() && !err.empty());   // NUL 之后有内容 -> 多余内容
  }

  // --------------------------------------------------------------- 20 ----
  CASE("嵌套深度上限的边界");
  {
    // 上限是 200 层容器:199 层必成功,201 层必失败(且不爆栈)
    std::string ok = std::string(199, '[') + "1" + std::string(199, ']');
    CK(Value::parse(ok, err).isArray() && err.empty());
    std::string over = std::string(201, '[') + "1" + std::string(201, ']');
    CK(Value::parse(over, err).isNull() && !err.empty());
    CK(err.find("嵌套层数") != std::string::npos);
  }

  // --------------------------------------------------------------- 21 ----
  CASE("确定性 fuzz:任意前缀截断 + 单字节变异都不得崩");
  {
    const char* corpus[] = {
        "{\"choices\":[{\"delta\":{\"content\":\"你好\\ud83d\\ude00\"}}]}",
        "{\"error\":{\"message\":\"bad key\",\"code\":401}}",
        "[1,-0,1.5e-10,1e308,9223372036854775807,true,false,null]",
        "{\"a\":{\"b\":[{\"c\":\"\\u4e2d\\t\\\"x\\\"\\\\\"}]},\"d\":[]}",
        "  \"\\u0000\\uD83D\\uDE00\"  ",
    };
    size_t survived = 0;
    for (const char* c : corpus) {
      std::string s(c);
      // 1) 全部前缀(含空前缀):只要求不崩、要么成功要么给出错误串
      for (size_t k = 0; k <= s.size(); ++k) {
        std::string e;
        Value v = Value::parse(s.substr(0, k), e);
        CK(e.empty() ? !v.isNull() || s.substr(0, k).find("null") !=
                                          std::string::npos
                     : v.isNull());
        ++survived;
      }
      // 2) 单字节变异(把每个位置依次换成若干"讨厌"的字节)
      static const char kEvil[] = {'\\', '"', '{', '[', ':', ',', '\0',
                                   '\n', 'u', '\x01', '-', 'e', '\xFF'};
      for (size_t k = 0; k < s.size(); ++k) {
        for (char ev : kEvil) {
          std::string m = s;
          m[k] = ev;
          std::string e;
          Value v = Value::parse(m, e);
          if (!e.empty()) CK(v.isNull());  // 失败必返回 null
          ++survived;
        }
      }
      // 3) 单字节删除
      for (size_t k = 0; k < s.size(); ++k) {
        std::string m = s;
        m.erase(k, 1);
        std::string e;
        Value v = Value::parse(m, e);
        if (!e.empty()) CK(v.isNull());
        ++survived;
      }
    }
    CK(survived > 3000);
    std::fprintf(stderr, "       fuzz 变体数: %zu\n", survived);
  }

  std::fprintf(stderr,
               "test_json: OK —— %d 个用例组 / %d 处断言\n", g_cases,
               g_checks);
  return 0;
}

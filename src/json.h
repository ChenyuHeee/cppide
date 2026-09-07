// json.h —— 极简 JSON(mj = mini json)
//
// 为什么自己写:构建环境不能联网取第三方库,而我们只需要
//   * 解析 DeepSeek 的响应与用户的 config.json;
//   * 生成 chat/completions 的请求体(关键是字符串转义正确)。
// 不追求性能、不支持注释/尾逗号(但解析失败绝不抛异常,只返回 err)。
//
// 契约:
//   * 全部只读访问器都“越界安全”:取不到就返回静态的 null / 空串 / 默认值,
//     绝不抛异常、绝不 UB。上层(config.cpp / aihttp.cpp)因此可以直接链式取值。
//   * Value 可拷贝可移动;对象保序(按插入顺序),便于 dump 出稳定的示例配置。
//   * 数字统一用 double 存;asInt/asInt64 做截断转换。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mj {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
 public:
  Value();                                  // null
  Value(std::nullptr_t);                    // null
  Value(bool b);
  Value(double d);
  Value(int i);
  Value(int64_t i);
  Value(const char* s);
  Value(std::string s);

  static Value array();                     // 空数组
  static Value object();                    // 空对象

  Type type() const;
  bool isNull() const;
  bool isBool() const;
  bool isNumber() const;
  bool isString() const;
  bool isArray() const;
  bool isObject() const;

  // ---- 标量取值:类型不匹配时返回 def,不报错 ----
  bool    asBool(bool def = false) const;
  double  asNumber(double def = 0.0) const;
  int     asInt(int def = 0) const;
  int64_t asInt64(int64_t def = 0) const;
  // 非字符串时返回静态空串的引用(可安全绑定)。
  const std::string& asString() const;
  std::string asString(const std::string& def) const;

  // ---- 容器访问 ----
  size_t size() const;                      // array/object 的元素数;其它类型 0
  // array/object 的第 i 个值;越界返回静态 null。
  const Value& value(size_t i) const;
  // object 的第 i 个键(保序);越界或非 object 返回静态空串。
  const std::string& key(size_t i) const;
  const Value& operator[](size_t i) const;  // 同 value(i)

  bool has(const std::string& k) const;
  // object 里取键;不存在返回静态 null。
  const Value& get(const std::string& k) const;
  const Value& operator[](const std::string& k) const;

  // ---- 构造 ----
  void push(Value v);                       // 追加到数组(非数组则先变成空数组)
  void set(std::string k, Value v);         // 设置对象键(非对象则先变成空对象);同键覆盖
  void clearContents();                     // 清空元素但保留类型

  // indent == 0 输出紧凑一行;> 0 输出缩进美化(用于 sampleJson / --print-config)。
  std::string dump(int indent = 0) const;

  // 解析。失败返回 null 值,并把中文错误(带行:列)写进 err。
  static Value parse(const std::string& text, std::string& err);
  // 字符串转义(不含外层引号)。控制字符走 \uXXXX,原样透传 UTF-8。
  static std::string escape(const std::string& s);

 private:
  Type t_ = Type::Null;
  bool b_ = false;
  double num_ = 0.0;
  std::string str_;
  // 注意:object 的键与值分两个 vector 存,而不是 vector<pair<string,Value>> ——
  // 后者在类内部会要求 pair<string,Value> 在 Value 尚未完整时实例化(非法)。
  // vector<Value> 用不完整类型是 C++17 明确允许的。
  std::vector<std::string> keys_;   // 仅 Object 使用,与 vals_ 一一对应
  std::vector<Value> vals_;         // Array / Object 的值
};

}  // namespace mj

// json.cpp —— 极简 JSON(mj)的实现
//
// 实现要点(与 json.h 的契约对应):
//   1. 解析绝不抛异常:parse() 内部完全用返回值传递失败,不 throw、不 assert。
//      唯一可能抛的是 std::string / std::vector 的内存分配失败(bad_alloc),
//      那已经不是"解析错误"而是进程级问题,不在契约范围内。
//   2. 递归深度硬上限 kMaxParseDepth:先判断再递归,所以 "[[[[..." 一万层
//      不会爆栈,而是干净地返回错误。dump() 同样有上限(手工构造的深树)。
//   3. 数字既要能当 double 也要能当精确 int64。json.h 冻结了成员布局
//      (数值只有一个 double num_),所以这里做了一个不改头的约定 ——
//      完全落在已有成员的语义里,没有新增任何成员:
//         Type::Number 且 b_ == true 时,str_ 里是该整数的精确十进制文本
//         (只由 Value(int) / Value(int64_t) 产生),asInt64() 直接读它,
//         因此 9223372036854775807 不经过 double、既精确又能原样 dump。
//         b_ == false 时 str_ 为空,dump 走 to_chars 的最短往返表示。
//   4. 全程不依赖 locale:数字进出都用 <charconv> 的 from_chars / to_chars,
//      而不是 strtod / snprintf("%g")。上层会为 ncurses 调 setlocale(LC_ALL, ""),
//      若用 %g 在 LC_NUMERIC=de_DE 下会吐出 "1,5" 这种非法 JSON。

#include "json.h"

#include <charconv>
#include <cmath>
#include <limits>

namespace mj {
namespace {

// 解析递归上限。DeepSeek 的响应和 config.json 实际嵌套 < 10 层,
// 200 层已经远超正常需求,同时保证栈开销可忽略。
constexpr int kMaxParseDepth = 200;
// dump 的递归上限(防止上层用 push/set 手工造出病态深树时爆栈)。
constexpr int kMaxDumpDepth = 512;

const Value& nullRef() {
  static const Value kNull;
  return kNull;
}
const std::string& emptyRef() {
  static const std::string kEmpty;
  return kEmpty;
}

// ---- 数字格式化(locale 无关) ----

std::string fmtDouble(double d) {
  // JSON 没有 NaN/Infinity。为了保证 dump() 永远产出合法 JSON,退化成 0。
  if (!std::isfinite(d)) return "0";
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof(buf), d);  // 最短往返表示
  if (r.ec != std::errc()) return "0";
  return std::string(buf, r.ptr);
}

std::string fmtI64(int64_t v) {
  char buf[24];
  auto r = std::to_chars(buf, buf + sizeof(buf), v);
  if (r.ec != std::errc()) return "0";
  return std::string(buf, r.ptr);
}

// ---- UTF-8 编码 ----

void appendUtf8(std::string& o, uint32_t cp) {
  if (cp > 0x10FFFFu) cp = 0xFFFDu;
  if (cp < 0x80u) {
    o.push_back(static_cast<char>(cp));
  } else if (cp < 0x800u) {
    o.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    o.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp < 0x10000u) {
    o.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    o.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    o.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    o.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    o.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    o.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    o.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 从已验证的 JSON 数字字面量估算十进制阶。仅在 from_chars 报
// result_out_of_range 时用来区分"上溢"(应报错)和"下溢"(当 0 处理)。
// 返回值 > 0 视为上溢方向。
long normExp10(const char* p, size_t len) {
  size_t k = 0;
  if (k < len && (p[k] == '-' || p[k] == '+')) ++k;
  // 整数部分
  size_t intBeg = k;
  while (k < len && isDigit(p[k])) ++k;
  size_t intEnd = k;
  size_t fracBeg = 0, fracEnd = 0;
  if (k < len && p[k] == '.') {
    ++k;
    fracBeg = k;
    while (k < len && isDigit(p[k])) ++k;
    fracEnd = k;
  }
  long ex = 0;
  if (k < len && (p[k] == 'e' || p[k] == 'E')) {
    ++k;
    bool neg = false;
    if (k < len && (p[k] == '+' || p[k] == '-')) {
      neg = (p[k] == '-');
      ++k;
    }
    // 指数本身也可能超长,夹到安全范围即可(判方向足够)。
    long v = 0;
    for (; k < len && isDigit(p[k]); ++k) {
      if (v < 1000000L) v = v * 10 + (p[k] - '0');
    }
    ex = neg ? -v : v;
  }
  // 去掉整数部分的前导零
  size_t ib = intBeg;
  while (ib < intEnd && p[ib] == '0') ++ib;
  if (ib < intEnd) return ex + static_cast<long>(intEnd - ib);
  // 整数部分全是 0:看小数部分的前导零
  size_t fb = fracBeg;
  while (fb < fracEnd && p[fb] == '0') ++fb;
  if (fb < fracEnd) return ex - static_cast<long>(fb - fracBeg);
  return ex;  // 尾数为 0
}

// ---- 递归下降解析器 ----

class Parser {
 public:
  Parser(const char* s, size_t n) : s_(s), n_(n) {}

  bool run(Value& out) {
    skipBom();
    skipWs();
    if (i_ >= n_) return fail(i_, "输入为空,期待一个 JSON 值");
    if (!parseValue(out, 0)) return false;
    skipWs();
    if (i_ < n_) return fail(i_, "JSON 值之后有多余内容");
    return true;
  }

  const std::string& err() const { return err_; }

 private:
  const char* s_;
  size_t n_;
  size_t i_ = 0;
  std::string err_;

  void skipBom() {
    if (n_ >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
        static_cast<unsigned char>(s_[1]) == 0xBB &&
        static_cast<unsigned char>(s_[2]) == 0xBF) {
      i_ = 3;
    }
  }

  void skipWs() {
    while (i_ < n_) {
      char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
      else break;
    }
  }

  bool fail(size_t pos, const std::string& msg) {
    if (!err_.empty()) return false;  // 只保留最内层/最早的那个错误
    size_t line = 1, col = 1;
    for (size_t k = 0; k < pos && k < n_; ++k) {
      if (s_[k] == '\n') {
        ++line;
        col = 1;
      } else {
        ++col;
      }
    }
    err_ = "第 " + std::to_string(line) + " 行第 " + std::to_string(col) +
           " 列:" + msg;
    return false;
  }

  bool lit(const char* word, size_t len) {
    if (n_ - i_ < len) return false;
    for (size_t k = 0; k < len; ++k)
      if (s_[i_ + k] != word[k]) return false;
    i_ += len;
    return true;
  }

  bool parseValue(Value& out, int depth) {
    if (depth >= kMaxParseDepth)
      return fail(i_, "嵌套层数超过上限(" + std::to_string(kMaxParseDepth) +
                          "),疑似恶意输入");
    if (i_ >= n_) return fail(i_, "输入意外结束,期待一个 JSON 值");
    char c = s_[i_];
    switch (c) {
      case '{': return parseObject(out, depth);
      case '[': return parseArray(out, depth);
      case '"': {
        std::string sv;
        if (!parseString(sv)) return false;
        out = Value(std::move(sv));
        return true;
      }
      case 't':
        if (!lit("true", 4)) return fail(i_, "非法字面量,期待 true");
        out = Value(true);
        return true;
      case 'f':
        if (!lit("false", 5)) return fail(i_, "非法字面量,期待 false");
        out = Value(false);
        return true;
      case 'n':
        if (!lit("null", 4)) return fail(i_, "非法字面量,期待 null");
        out = Value();
        return true;
      default:
        if (c == '-' || isDigit(c)) return parseNumber(out);
        return fail(i_, std::string("非法字符 '") + c + "',无法开始一个 JSON 值");
    }
  }

  bool parseObject(Value& out, int depth) {
    size_t open = i_;
    ++i_;  // '{'
    out = Value::object();
    skipWs();
    if (i_ < n_ && s_[i_] == '}') {
      ++i_;
      return true;
    }
    for (;;) {
      skipWs();
      if (i_ >= n_) return fail(open, "对象没有闭合的 '}'");
      if (s_[i_] != '"') return fail(i_, "对象的键必须是字符串");
      std::string k;
      if (!parseString(k)) return false;
      skipWs();
      if (i_ >= n_ || s_[i_] != ':') return fail(i_, "对象的键之后期待 ':'");
      ++i_;
      skipWs();
      Value v;
      if (!parseValue(v, depth + 1)) return false;
      out.set(std::move(k), std::move(v));  // 同键后者覆盖
      skipWs();
      if (i_ >= n_) return fail(open, "对象没有闭合的 '}'");
      if (s_[i_] == ',') {
        ++i_;
        skipWs();
        if (i_ < n_ && s_[i_] == '}') return fail(i_, "对象不允许尾随逗号");
        continue;
      }
      if (s_[i_] == '}') {
        ++i_;
        return true;
      }
      return fail(i_, "对象里期待 ',' 或 '}'");
    }
  }

  bool parseArray(Value& out, int depth) {
    size_t open = i_;
    ++i_;  // '['
    out = Value::array();
    skipWs();
    if (i_ < n_ && s_[i_] == ']') {
      ++i_;
      return true;
    }
    for (;;) {
      skipWs();
      Value v;
      if (!parseValue(v, depth + 1)) return false;
      out.push(std::move(v));
      skipWs();
      if (i_ >= n_) return fail(open, "数组没有闭合的 ']'");
      if (s_[i_] == ',') {
        ++i_;
        skipWs();
        if (i_ < n_ && s_[i_] == ']') return fail(i_, "数组不允许尾随逗号");
        continue;
      }
      if (s_[i_] == ']') {
        ++i_;
        return true;
      }
      return fail(i_, "数组里期待 ',' 或 ']'");
    }
  }

  // 读 4 位十六进制;成功返回 true 并写 cp。
  bool hex4(uint32_t& cp) {
    if (n_ - i_ < 4) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
      int h = hexVal(s_[i_ + k]);
      if (h < 0) return false;
      v = (v << 4) | static_cast<uint32_t>(h);
    }
    i_ += 4;
    cp = v;
    return true;
  }

  bool parseString(std::string& out) {
    size_t open = i_;
    ++i_;  // 开引号
    out.clear();
    for (;;) {
      if (i_ >= n_) return fail(open, "字符串没有闭合的双引号");
      unsigned char c = static_cast<unsigned char>(s_[i_]);
      if (c == '"') {
        ++i_;
        return true;
      }
      if (c < 0x20) {
        return fail(i_, "字符串里出现未转义的控制字符");
      }
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++i_;
        continue;
      }
      // 转义
      size_t esc = i_;
      ++i_;
      if (i_ >= n_) return fail(esc, "转义序列在字符串结束前被截断");
      char e = s_[i_];
      ++i_;
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(cp)) return fail(esc, "\\u 之后需要 4 位十六进制数字");
          if (cp >= 0xD800u && cp <= 0xDBFFu) {
            // 高代理,尝试配对低代理
            if (n_ - i_ >= 6 && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
              size_t save = i_;
              i_ += 2;
              uint32_t lo = 0;
              if (hex4(lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
              } else {
                i_ = save;      // 不是合法低代理,回退
                cp = 0xFFFDu;   // 孤立代理 -> U+FFFD(宽容,不报错)
              }
            } else {
              cp = 0xFFFDu;
            }
          } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;  // 孤立低代理
          }
          appendUtf8(out, cp);
          break;
        }
        default:
          return fail(esc, std::string("未知的转义序列 \\") + e);
      }
    }
  }

  bool parseNumber(Value& out) {
    size_t start = i_;
    if (i_ < n_ && s_[i_] == '-') ++i_;
    if (i_ >= n_) return fail(start, "数字缺少整数部分");
    if (s_[i_] == '0') {
      ++i_;
    } else if (isDigit(s_[i_])) {
      while (i_ < n_ && isDigit(s_[i_])) ++i_;
    } else {
      return fail(i_, "数字缺少整数部分");
    }
    bool isInt = true;
    if (i_ < n_ && s_[i_] == '.') {
      isInt = false;
      ++i_;
      if (i_ >= n_ || !isDigit(s_[i_])) return fail(i_, "小数点后需要数字");
      while (i_ < n_ && isDigit(s_[i_])) ++i_;
    }
    if (i_ < n_ && (s_[i_] == 'e' || s_[i_] == 'E')) {
      isInt = false;
      ++i_;
      if (i_ < n_ && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
      if (i_ >= n_ || !isDigit(s_[i_])) return fail(i_, "指数部分需要数字");
      while (i_ < n_ && isDigit(s_[i_])) ++i_;
    }
    const char* p = s_ + start;
    size_t len = i_ - start;

    double d = 0.0;
    auto r = std::from_chars(p, p + len, d, std::chars_format::general);
    if (r.ec == std::errc::result_out_of_range) {
      if (normExp10(p, len) > 0)
        return fail(start, "数字超出双精度可表示范围");
      d = (len > 0 && p[0] == '-') ? -0.0 : 0.0;  // 下溢当 0
    } else if (r.ec != std::errc() || r.ptr != p + len) {
      return fail(start, "数字格式非法");
    }

    // 纯整数且塞得进 int64 -> 走 Value(int64_t),str_ 存精确十进制文本,
    // asInt64() 就不会经过 double 而丢精度,dump 也能原样吐回去。
    // 例外:"-0" 要保住符号位,交给 double 分支(否则 dump 成 "0")。
    if (isInt && !(d == 0.0 && p[0] == '-')) {
      int64_t iv = 0;
      auto ri = std::from_chars(p, p + len, iv);
      if (ri.ec == std::errc() && ri.ptr == p + len) {
        out = Value(iv);
        return true;
      }
    }
    out = Value(d);
    return true;
  }
};

}  // namespace

// ---------------------------------------------------------------- 构造 ----

Value::Value() : t_(Type::Null) {}
Value::Value(std::nullptr_t) : t_(Type::Null) {}
Value::Value(bool b) : t_(Type::Bool), b_(b) {}
Value::Value(double d) : t_(Type::Number), b_(false), num_(d) {}
Value::Value(int i)
    : t_(Type::Number), b_(true), num_(static_cast<double>(i)),
      str_(fmtI64(static_cast<int64_t>(i))) {}
Value::Value(int64_t i)
    : t_(Type::Number), b_(true), num_(static_cast<double>(i)),
      str_(fmtI64(i)) {}
Value::Value(const char* s) : t_(Type::String), str_(s ? s : "") {}
Value::Value(std::string s) : t_(Type::String), str_(std::move(s)) {}

Value Value::array() {
  Value v;
  v.t_ = Type::Array;
  return v;
}
Value Value::object() {
  Value v;
  v.t_ = Type::Object;
  return v;
}

// ---------------------------------------------------------------- 类型 ----

Type Value::type() const { return t_; }
bool Value::isNull() const { return t_ == Type::Null; }
bool Value::isBool() const { return t_ == Type::Bool; }
bool Value::isNumber() const { return t_ == Type::Number; }
bool Value::isString() const { return t_ == Type::String; }
bool Value::isArray() const { return t_ == Type::Array; }
bool Value::isObject() const { return t_ == Type::Object; }

// ---------------------------------------------------------------- 取值 ----

bool Value::asBool(bool def) const { return t_ == Type::Bool ? b_ : def; }

double Value::asNumber(double def) const {
  return t_ == Type::Number ? num_ : def;
}

int64_t Value::asInt64(int64_t def) const {
  if (t_ != Type::Number) return def;
  if (b_ && !str_.empty()) {  // 精确整数字面量:不经过 double
    int64_t v = 0;
    auto r = std::from_chars(str_.data(), str_.data() + str_.size(), v);
    if (r.ec == std::errc() && r.ptr == str_.data() + str_.size()) return v;
  }
  if (std::isnan(num_)) return def;
  // double -> int64 的越界转换是 UB,必须先夹住。
  // 9223372036854775808.0 是 2^63 的精确 double 值。
  if (num_ >= 9223372036854775808.0) return std::numeric_limits<int64_t>::max();
  if (num_ <= -9223372036854775808.0) return std::numeric_limits<int64_t>::min();
  return static_cast<int64_t>(num_);
}

int Value::asInt(int def) const {
  if (t_ != Type::Number) return def;
  int64_t v = asInt64(static_cast<int64_t>(def));
  if (v > static_cast<int64_t>(std::numeric_limits<int>::max()))
    return std::numeric_limits<int>::max();
  if (v < static_cast<int64_t>(std::numeric_limits<int>::min()))
    return std::numeric_limits<int>::min();
  return static_cast<int>(v);
}

const std::string& Value::asString() const {
  return t_ == Type::String ? str_ : emptyRef();
}

std::string Value::asString(const std::string& def) const {
  return t_ == Type::String ? str_ : def;
}

// -------------------------------------------------------------- 容器 ----

size_t Value::size() const {
  if (t_ == Type::Array || t_ == Type::Object) return vals_.size();
  return 0;
}

const Value& Value::value(size_t i) const {
  if ((t_ == Type::Array || t_ == Type::Object) && i < vals_.size())
    return vals_[i];
  return nullRef();
}

const std::string& Value::key(size_t i) const {
  if (t_ == Type::Object && i < keys_.size()) return keys_[i];
  return emptyRef();
}

const Value& Value::operator[](size_t i) const { return value(i); }

bool Value::has(const std::string& k) const {
  if (t_ != Type::Object) return false;
  for (const auto& kk : keys_)
    if (kk == k) return true;
  return false;
}

const Value& Value::get(const std::string& k) const {
  if (t_ != Type::Object) return nullRef();
  for (size_t i = 0; i < keys_.size() && i < vals_.size(); ++i)
    if (keys_[i] == k) return vals_[i];
  return nullRef();
}

const Value& Value::operator[](const std::string& k) const { return get(k); }

// -------------------------------------------------------------- 构造 ----

void Value::push(Value v) {
  if (t_ != Type::Array) {
    t_ = Type::Array;
    b_ = false;
    num_ = 0.0;
    str_.clear();
    keys_.clear();
    vals_.clear();
  }
  vals_.push_back(std::move(v));
}

void Value::set(std::string k, Value v) {
  if (t_ != Type::Object) {
    t_ = Type::Object;
    b_ = false;
    num_ = 0.0;
    str_.clear();
    keys_.clear();
    vals_.clear();
  }
  for (size_t i = 0; i < keys_.size() && i < vals_.size(); ++i) {
    if (keys_[i] == k) {
      vals_[i] = std::move(v);
      return;
    }
  }
  keys_.push_back(std::move(k));
  vals_.push_back(std::move(v));
}

void Value::clearContents() {
  keys_.clear();
  vals_.clear();
  str_.clear();
  num_ = 0.0;
  b_ = false;
}

// -------------------------------------------------------------- 转义 ----

std::string Value::escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  static const char* kHex = "0123456789abcdef";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          o += "\\u00";
          o.push_back(kHex[(c >> 4) & 0xF]);
          o.push_back(kHex[c & 0xF]);
        } else {
          o.push_back(static_cast<char>(c));  // UTF-8 原样透传
        }
    }
  }
  return o;
}

// --------------------------------------------------------------- dump ----

std::string Value::dump(int indent) const {
  // 递归体写成成员函数里的局部类:C++ 规定这样的局部类拥有与外围成员函数
  // 相同的访问权限,于是它能读 b_ / str_ / num_ —— 不必给 json.h 加成员。
  struct D {
    static void pad(std::string& o, int indent, int depth) {
      if (indent <= 0) return;
      o.push_back('\n');
      o.append(static_cast<size_t>(indent) * static_cast<size_t>(depth), ' ');
    }
    static void num(const Value& v, std::string& o) {
      if (v.b_ && !v.str_.empty()) o += v.str_;  // 精确整数字面量
      else o += fmtDouble(v.num_);
    }
    static void go(const Value& v, std::string& o, int indent, int depth) {
      if (depth > kMaxDumpDepth) {
        o += "null";  // 病态深树:截断而不是爆栈
        return;
      }
      switch (v.t_) {
        case Type::Null: o += "null"; return;
        case Type::Bool: o += v.b_ ? "true" : "false"; return;
        case Type::Number: num(v, o); return;
        case Type::String:
          o.push_back('"');
          o += Value::escape(v.str_);
          o.push_back('"');
          return;
        case Type::Array: {
          if (v.vals_.empty()) {
            o += "[]";
            return;
          }
          o.push_back('[');
          for (size_t i = 0; i < v.vals_.size(); ++i) {
            if (i) o.push_back(',');
            pad(o, indent, depth + 1);
            go(v.vals_[i], o, indent, depth + 1);
          }
          pad(o, indent, depth);
          o.push_back(']');
          return;
        }
        case Type::Object: {
          if (v.vals_.empty()) {
            o += "{}";
            return;
          }
          o.push_back('{');
          for (size_t i = 0; i < v.vals_.size() && i < v.keys_.size(); ++i) {
            if (i) o.push_back(',');
            pad(o, indent, depth + 1);
            o.push_back('"');
            o += Value::escape(v.keys_[i]);
            o.push_back('"');
            o.push_back(':');
            if (indent > 0) o.push_back(' ');
            go(v.vals_[i], o, indent, depth + 1);
          }
          pad(o, indent, depth);
          o.push_back('}');
          return;
        }
      }
      o += "null";
    }
  };

  std::string o;
  o.reserve(64);
  D::go(*this, o, indent < 0 ? 0 : indent, 0);
  return o;
}

// -------------------------------------------------------------- parse ----

Value Value::parse(const std::string& text, std::string& err) {
  err.clear();
  Value out;
  Parser p(text.data(), text.size());
  if (!p.run(out)) {
    err = p.err().empty() ? std::string("JSON 解析失败(未知原因)") : p.err();
    return Value();  // 失败一律返回 null
  }
  return out;
}

}  // namespace mj

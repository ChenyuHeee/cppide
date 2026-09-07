// highlight.cpp —— C/C++ 逐行词法着色器 + 行首状态缓存
//
// 实现要点(对应 architecture.md §3):
//   * Highlighter::scanLine 是纯函数:无可变全局/静态可写状态,只读关键字表。
//     同一 (line, entryState, lang) 永远给同样的 spans 与返回状态。
//   * 输出的 spans 严格覆盖 [0, line.size()) 且不重叠:所有 push() 都以
//     “上一段的结束 = 本段的开始”推进游标,同 Tok 的相邻段会被合并。
//     所有分段点都落在 ASCII 字节上,因此多字节字符(中文)不会被切开。
//
// 两条自有的内部约定(不改头文件,外部不可观测):
//   1. 行注释的续行状态 = ST_CONTLINE | ST_BLOCKCOMMENT。
//      1 字节状态里没有第 4 个位可用,而这个组合本身不会自然出现
//      (块注释内部不产生续行状态),故复用它表示“上一行是以 \ 结尾的 //”。
//      ST_CONTLINE 单独出现 = 普通续行(#define / 表达式续行),本行按常规扫描。
//   2. HighlightCache 内部用 0xFF 作“该行从未算过”的哨兵(合法状态只有 0..7)。
//      头注释说“多出来的行状态置 0”——0xFF 在语义上就是“无效/未知”,
//      entryState() 对外永不返回它。收敛提前退出必须能区分
//      “旧值与新值相等” 和 “旧值只是填充的 0”,否则会误判收敛。

#include "highlight.h"

#include <algorithm>
#include <string>
#include <unordered_set>

namespace {

constexpr uint8_t kUnknownState = 0xFFu;  // HighlightCache 专用哨兵
constexpr uint8_t kStateMask = ST_BLOCKCOMMENT | ST_RAWSTRING | ST_CONTLINE;
// 行注释续行(见文件头约定 1)
constexpr uint8_t kLineCommentCont = ST_CONTLINE | ST_BLOCKCOMMENT;

inline bool isDigitB(unsigned char c) { return c >= '0' && c <= '9'; }
inline bool isAlphaB(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline bool isHexB(unsigned char c) {
  return isDigitB(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool isBinB(unsigned char c) { return c == '0' || c == '1'; }
// 标识符字节:ASCII 字母数字下划线,以及所有 >= 0x80 的字节。
// 把非 ASCII 一律并进标识符,是为了让中文/非法 UTF-8 字节整体落进同一个 span,
// 绝不会被切在多字节字符中间。
inline bool isIdentStartB(unsigned char c) {
  return c == '_' || isAlphaB(c) || c >= 0x80;
}
inline bool isIdentB(unsigned char c) {
  return c == '_' || isAlphaB(c) || isDigitB(c) || c >= 0x80;
}
inline bool isSpaceB(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}
inline unsigned char at(const std::string& s, size_t i) {
  return static_cast<unsigned char>(s[i]);
}

// 追加一段 [start, end);同 Tok 且相邻则合并。end <= start 时什么都不做。
void push(std::vector<Span>& out, size_t start, size_t end, Tok t) {
  if (end <= start) return;
  const int st = static_cast<int>(start);
  const int len = static_cast<int>(end - start);
  if (!out.empty() && out.back().tok == t && out.back().start + out.back().len == st) {
    out.back().len += len;
    return;
  }
  out.push_back(Span{st, len, t});
}

// 注释区间 [from, to):整体 Comment,内部的 TODO/FIXME/XXX(按词边界)染 TodoInComment。
void pushComment(const std::string& s, size_t from, size_t to, std::vector<Span>& out) {
  static const char* const kTodo[] = {"TODO", "FIXME", "XXX"};
  size_t seg = from;
  size_t i = from;
  while (i < to) {
    const bool leftOk = (i == 0) || !isIdentB(at(s, i - 1));
    if (leftOk) {
      for (const char* w : kTodo) {
        const size_t wl = std::string::traits_type::length(w);
        if (to - i >= wl && s.compare(i, wl, w) == 0 &&
            (i + wl == to || !isIdentB(at(s, i + wl)))) {
          push(out, seg, i, Tok::Comment);
          push(out, i, i + wl, Tok::TodoInComment);
          i += wl;
          seg = i;
          goto next;
        }
      }
    }
    ++i;
  next:;
  }
  push(out, seg, to, Tok::Comment);
}

// "..." / '...' 通用扫描:处理 \ 转义,未闭合则到行尾。返回结束位置(> i)。
size_t scanQuoted(const std::string& s, size_t i, char quote) {
  const size_t n = s.size();
  size_t j = i + 1;
  while (j < n) {
    const char c = s[j];
    if (c == '\\') {
      j += 2;  // 吃掉转义字符;可能越过行尾,下面夹紧
      continue;
    }
    if (c == quote) {
      ++j;
      break;
    }
    ++j;
  }
  return j > n ? n : j;
}

// 数字扫描:0x / 0b / 十进制 / 小数 / e|p 指数 / 后缀 / C++14 分隔符 '。
// 分隔符只在“前后都是数字”时被吃掉,所以 y = 'a' 的 ' 不会被误吞。
size_t scanNumber(const std::string& s, size_t i) {
  const size_t n = s.size();
  size_t j = i;
  auto run = [&](bool (*pred)(unsigned char)) {
    while (j < n) {
      if (pred(at(s, j))) {
        ++j;
      } else if (s[j] == '\'' && j + 1 < n && pred(at(s, j + 1))) {
        j += 2;
      } else {
        break;
      }
    }
  };
  auto expo = [&](char e1, char e2) {
    if (j < n && (s[j] == e1 || s[j] == e2)) {
      size_t k = j + 1;
      if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
      if (k < n && isDigitB(at(s, k))) {
        j = k;
        run(isDigitB);
      }
    }
  };

  if (s[j] == '0' && j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
    j += 2;
    run(isHexB);
    if (j < n && s[j] == '.') {
      ++j;
      run(isHexB);
    }
    expo('p', 'P');
  } else if (s[j] == '0' && j + 1 < n && (s[j + 1] == 'b' || s[j + 1] == 'B')) {
    j += 2;
    run(isBinB);
  } else {
    run(isDigitB);
    if (j < n && s[j] == '.') {
      ++j;
      run(isDigitB);
    }
    expo('e', 'E');
  }
  // 后缀:u U l L f F z Z 以及用户自定义字面量 _km
  if (j < n && (isAlphaB(at(s, j)) || s[j] == '_')) {
    while (j < n && isIdentB(at(s, j))) ++j;
  }
  return j > n ? n : j;
}

bool isRawPrefix(const std::string& s, size_t i, size_t j) {
  const size_t len = j - i;
  if (len == 0 || len > 2 || s[j - 1] != 'R') return false;
  if (len == 1) return true;
  return s[i] == 'L' || s[i] == 'u' || s[i] == 'U';  // LR" uR" UR"(u8R" 见下)
}

const std::unordered_set<std::string>& kwSet(Lang l) {
  static const std::unordered_set<std::string> cpp = {
      "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
      "break", "case", "catch", "class", "compl", "concept", "const",
      "consteval", "constexpr", "constinit", "const_cast", "continue",
      "co_await", "co_return", "co_yield", "decltype", "default", "delete",
      "do", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
      "false", "final", "for", "friend", "goto", "if", "inline", "mutable",
      "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator",
      "or", "or_eq", "override", "private", "protected", "public", "register",
      "reinterpret_cast", "requires", "return", "sizeof", "static",
      "static_assert", "static_cast", "struct", "switch", "template", "this",
      "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
      "union", "using", "virtual", "volatile", "while", "xor", "xor_eq",
      "restrict", "_Pragma"};
  static const std::unordered_set<std::string> c = {
      "asm", "auto", "break", "case", "const", "continue", "default", "do",
      "else", "enum", "extern", "for", "goto", "if", "inline", "register",
      "restrict", "return", "sizeof", "static", "struct", "switch", "typedef",
      "union", "volatile", "while", "_Alignas", "_Alignof", "_Atomic", "_Bool",
      "_Complex", "_Generic", "_Imaginary", "_Noreturn", "_Static_assert",
      "_Thread_local", "_Pragma"};
  return l == Lang::C ? c : cpp;  // Lang::Unknown 按 C++ 处理
}

const std::unordered_set<std::string>& typeSet(Lang l) {
  // C 与 C++ 共有:内建类型 + <stdint.h>/<stddef.h> 常用别名
  static const std::unordered_set<std::string> base = {
      "void", "bool", "char", "short", "int", "long", "float", "double",
      "signed", "unsigned", "size_t", "ssize_t", "ptrdiff_t", "intptr_t",
      "uintptr_t", "intmax_t", "uintmax_t", "int8_t", "int16_t", "int32_t",
      "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "FILE",
      "wchar_t", "time_t", "clock_t", "va_list"};
  static const std::unordered_set<std::string> cpp = [] {
    std::unordered_set<std::string> s = base;
    // C++ 独有的内建类型
    for (const char* w : {"char8_t", "char16_t", "char32_t", "nullptr_t"})
      s.insert(w);
    // 竞赛代码里高频的 STL 名(架构文档 §3 明确要求收进类型表)
    for (const char* w :
         {"string", "wstring", "string_view", "vector", "deque", "list",
          "forward_list", "map", "multimap", "set", "multiset",
          "unordered_map", "unordered_set", "unordered_multimap",
          "unordered_multiset", "pair", "tuple", "array", "stack", "queue",
          "priority_queue", "bitset", "complex", "valarray", "optional",
          "variant", "any", "function", "shared_ptr", "unique_ptr",
          "weak_ptr", "initializer_list", "iterator", "istream", "ostream",
          "ifstream", "ofstream", "fstream", "stringstream", "istringstream",
          "ostringstream", "streambuf", "ll", "ull", "ld", "lll", "u32",
          "u64", "i64"})
      s.insert(w);
    return s;
  }();
  return l == Lang::C ? base : cpp;
}

}  // namespace

const char* tokName(Tok t) {
  static_assert(kTokCount == 11, "tokName 必须与 Tok 同步");
  switch (t) {
    case Tok::Normal: return "Normal";
    case Tok::Keyword: return "Keyword";
    case Tok::Type: return "Type";
    case Tok::Preproc: return "Preproc";
    case Tok::String: return "String";
    case Tok::Char: return "Char";
    case Tok::Number: return "Number";
    case Tok::Comment: return "Comment";
    case Tok::Operator: return "Operator";
    case Tok::Func: return "Func";
    case Tok::TodoInComment: return "TodoInComment";
  }
  return "?";
}

Highlighter::Highlighter(Lang lang) : lang_(lang) {}
void Highlighter::setLang(Lang l) { lang_ = l; }
Lang Highlighter::lang() const { return lang_; }

bool Highlighter::isKeyword(const std::string& w, Lang l) {
  const auto& s = kwSet(l);
  return s.find(w) != s.end();
}

bool Highlighter::isTypeWord(const std::string& w, Lang l) {
  const auto& s = typeSet(l);
  return s.find(w) != s.end();
}

uint8_t Highlighter::scanLine(const std::string& line, uint8_t entryState,
                              std::vector<Span>& out) const {
  out.clear();
  const size_t n = line.size();
  const uint8_t st = static_cast<uint8_t>(entryState & kStateMask);
  // 续行判据:行的最后一个字节是 '\'(不忽略其后的空白,与编译器一致)
  const bool cont = (n > 0 && line[n - 1] == '\\');
  const auto& kws = kwSet(lang_);
  const auto& tys = typeSet(lang_);

  size_t i = 0;

  // ---- 行首跨行状态 ----
  if ((st & kLineCommentCont) == kLineCommentCont) {
    // 上一行是 "// ... \":本行整行仍是注释
    pushComment(line, 0, n, out);
    return cont ? kLineCommentCont : static_cast<uint8_t>(ST_NONE);
  }
  if (st & ST_BLOCKCOMMENT) {
    const size_t e = line.find("*/");
    if (e == std::string::npos) {
      pushComment(line, 0, n, out);
      return ST_BLOCKCOMMENT;
    }
    pushComment(line, 0, e, out);
    push(out, e, e + 2, Tok::Comment);
    i = e + 2;
  } else if (st & ST_RAWSTRING) {
    // 近似:出现 )" 即认为原始字符串结束(见 highlight.h 的已知近似)
    const size_t e = line.find(")\"");
    if (e == std::string::npos) {
      push(out, 0, n, Tok::String);
      return ST_RAWSTRING;
    }
    push(out, 0, e + 2, Tok::String);
    i = e + 2;
  }

  // ---- 预处理指令 ----
  bool inInclude = false;
  if (i == 0) {
    size_t j = 0;
    while (j < n && isSpaceB(at(line, j))) ++j;
    if (j < n && line[j] == '#') {
      push(out, 0, j, Tok::Normal);
      push(out, j, j + 1, Tok::Preproc);
      size_t w = j + 1;
      while (w < n && isSpaceB(at(line, w))) ++w;
      push(out, j + 1, w, Tok::Normal);
      size_t we = w;
      while (we < n && isIdentB(at(line, we))) ++we;
      if (we > w) {
        push(out, w, we, Tok::Preproc);
        const std::string d = line.substr(w, we - w);
        inInclude = (d == "include" || d == "include_next" || d == "import");
      }
      i = we;
    }
  }

  // ---- 常规扫描 ----
  while (i < n) {
    const unsigned char c = at(line, i);

    if (isSpaceB(c)) {
      size_t j = i;
      while (j < n && isSpaceB(at(line, j))) ++j;
      push(out, i, j, Tok::Normal);
      i = j;
      continue;
    }
    if (c == '/' && i + 1 < n && line[i + 1] == '/') {
      pushComment(line, i, n, out);
      return cont ? kLineCommentCont : static_cast<uint8_t>(ST_NONE);
    }
    if (c == '/' && i + 1 < n && line[i + 1] == '*') {
      const size_t e = line.find("*/", i + 2);
      if (e == std::string::npos) {
        pushComment(line, i, n, out);
        return ST_BLOCKCOMMENT;  // 块注释内的行尾 '\' 无意义
      }
      pushComment(line, i, e, out);
      push(out, e, e + 2, Tok::Comment);
      i = e + 2;
      continue;
    }
    if (inInclude && c == '<') {
      const size_t e = line.find('>', i + 1);
      const size_t end = (e == std::string::npos) ? n : e + 1;
      push(out, i, end, Tok::String);
      i = end;
      inInclude = false;
      continue;
    }
    if (c == '"') {
      const size_t e = scanQuoted(line, i, '"');
      push(out, i, e, Tok::String);
      i = e;
      continue;
    }
    if (c == '\'') {
      const size_t e = scanQuoted(line, i, '\'');
      push(out, i, e, Tok::Char);
      i = e;
      continue;
    }
    if (isDigitB(c) || (c == '.' && i + 1 < n && isDigitB(at(line, i + 1)))) {
      const size_t e = scanNumber(line, i);
      push(out, i, e, Tok::Number);
      i = e;
      continue;
    }
    if (isIdentStartB(c)) {
      size_t j = i;
      while (j < n && isIdentB(at(line, j))) ++j;
      // 原始字符串 R"delim(...)":前缀 R / LR / uR / UR / u8R
      if (j < n && line[j] == '"' &&
          (isRawPrefix(line, i, j) ||
           (j - i == 3 && line.compare(i, 3, "u8R") == 0))) {
        const size_t dstart = j + 1;
        size_t p = dstart;
        while (p < n && p - dstart < 16 && line[p] != '(' && line[p] != '"' &&
               line[p] != ')' && line[p] != '\\' && !isSpaceB(at(line, p)))
          ++p;
        if (p < n && line[p] == '(') {
          const std::string close = ")" + line.substr(dstart, p - dstart) + "\"";
          const size_t e = line.find(close, p + 1);
          if (e == std::string::npos) {
            push(out, i, n, Tok::String);
            return ST_RAWSTRING;
          }
          push(out, i, e + close.size(), Tok::String);
          i = e + close.size();
          continue;
        }
        // 不是合法原始字符串:前缀按标识符处理,引号留给下一轮
      }
      const std::string w = line.substr(i, j - i);
      Tok t = Tok::Normal;
      if (kws.find(w) != kws.end()) {
        t = Tok::Keyword;
      } else if (tys.find(w) != tys.end()) {
        t = Tok::Type;
      } else if (j < n && line[j] == '(') {
        t = Tok::Func;  // 标识符后紧跟 '(' → 函数名
      }
      push(out, i, j, t);
      i = j;
      continue;
    }
    // 其余单字节(运算符、标点、控制字符)
    push(out, i, i + 1, Tok::Operator);
    ++i;
  }

  // 未闭合字符串/字符字面量不泄漏状态;行尾 '\' 只置普通续行位。
  return cont ? static_cast<uint8_t>(ST_CONTLINE) : static_cast<uint8_t>(ST_NONE);
}

// ============================ HighlightCache ============================

void HighlightCache::resize(int lineCount) {
  if (lineCount < 0) lineCount = 0;
  if (lineCount == static_cast<int>(entry_.size())) return;
  // 行数变了 = 插/删过行 → 原有缓存与行号整体错位(第 k 行的旧值其实属于别的行),
  // 而且我们拿不到“变化点”,只能整体作废:全部退回“未知”。
  // 不这样做的话,收敛提前退出会拿错位的旧值误判为收敛,给出错误的行首状态。
  // 代价:插/删行后要从第 0 行惰性重扫到视口底部(纯词法,无分配);
  // 而普通打字不改行数,走的是下面 invalidateFrom + 收敛退出的快路径。
  entry_.assign(static_cast<size_t>(lineCount), kUnknownState);
  if (lineCount > 0) {
    entry_[0] = ST_NONE;  // 第 0 行行首状态恒为 ST_NONE
    validUpTo_ = 1;
  } else {
    validUpTo_ = 0;
  }
}

void HighlightCache::invalidateFrom(int line) {
  if (line < 0) line = 0;
  if (line < validUpTo_) validUpTo_ = line;
  // 注意:**不清除** entry_[line..] 的旧值,收敛提前退出正是靠它们比对。
  if (!entry_.empty()) {
    entry_[0] = ST_NONE;
    if (validUpTo_ < 1) validUpTo_ = 1;
  }
}

void HighlightCache::clear() {
  entry_.clear();
  validUpTo_ = 0;
  scratch_.clear();
}

uint8_t HighlightCache::entryState(const TextBuffer& b, const Highlighter& h,
                                   int line) {
  const int nl = b.lineCount();
  if (static_cast<int>(entry_.size()) != nl) resize(nl);
  if (entry_.empty() || line <= 0) return ST_NONE;
  if (line >= static_cast<int>(entry_.size())) line = static_cast<int>(entry_.size()) - 1;

  while (validUpTo_ <= line) {
    const int src = validUpTo_ - 1;  // >= 0:entry_[0..validUpTo_-1] 有效
    const uint8_t in = entry_[static_cast<size_t>(src)];
    const uint8_t got =
        h.scanLine(b.line(src), in == kUnknownState ? uint8_t{ST_NONE} : in, scratch_);
    const int dst = validUpTo_;
    const uint8_t old = entry_[static_cast<size_t>(dst)];
    entry_[static_cast<size_t>(dst)] = got;
    validUpTo_ = dst + 1;
    if (old != kUnknownState && old == got) {
      // 收敛:新状态与旧缓存一致 → dst 之后的旧值仍然成立(递推式相同),
      // 直接把有效区推到第一个“从未算过”的行为止,不再逐行重扫。
      int k = validUpTo_;
      const int m = static_cast<int>(entry_.size());
      while (k < m && entry_[static_cast<size_t>(k)] != kUnknownState) ++k;
      if (k > validUpTo_) validUpTo_ = k;
      if (validUpTo_ > line) break;
    }
  }
  const uint8_t s = entry_[static_cast<size_t>(line)];
  return s == kUnknownState ? uint8_t{ST_NONE} : s;
}

void HighlightCache::spansFor(const TextBuffer& b, const Highlighter& h,
                              int line, std::vector<Span>& out) {
  out.clear();
  if (line < 0 || line >= b.lineCount()) return;
  const uint8_t st = entryState(b, h, line);
  h.scanLine(b.line(line), st, out);
}

int HighlightCache::validUpTo() const { return validUpTo_; }

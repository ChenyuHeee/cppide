// highlight.h —— C/C++ 语法高亮:逐行纯函数扫描 + 1 字节行首状态
//
// 设计原则:
//   * Highlighter::scanLine 是**纯函数**:给定“行首跨行状态”与行文本,产出
//     spans 并返回“行尾状态”。可单测、与渲染完全解耦。
//   * HighlightCache 只缓存每行的行首状态(1 字节/行,10000 行 = 10KB),
//     spans 每次按需重算 —— 渲染只需要屏幕上那几十行,重算代价可忽略。
//   * 编辑第 N 行 -> invalidateFrom(N);向下惰性重扫并**收敛提前退出**:
//     新状态等于旧缓存状态时后续全部不变,立刻停止。所以在文件中间敲 "/*"
//     也不会触发全文重扫。
//
// 已知近似(README 记为限制):原始字符串 R"delim(...)" 的分隔符是变长的,
// 塞不进 1 字节状态,故跨行时按“出现 )" 即结束”近似处理。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "textbuf.h"

enum class Tok : uint8_t {
  Normal, Keyword, Type, Preproc, String, Char, Number,
  Comment, Operator, Func, TodoInComment
};
// Tok 的种类数(颜色表/attr 表按它开数组)。
constexpr int kTokCount = 11;
const char* tokName(Tok t);   // 调试与单测用的英文名

// 行首跨行状态,打包进 1 字节
enum : uint8_t {
  ST_NONE         = 0,
  ST_BLOCKCOMMENT = 1 << 0,   // 处于 /* ... */ 中
  ST_RAWSTRING    = 1 << 1,   // 处于未闭合的 R"(...)" 中(近似)
  ST_CONTLINE     = 1 << 2,   // 上一行以 '\' 结尾(续行)
};

// 按字节的 run-length。同一行内的 spans 保证:按 start 升序、不重叠、
// 且覆盖整行 [0, line.size()) —— 渲染方可以直接顺序输出,无需处理空隙。
struct Span {
  int start = 0;
  int len = 0;
  Tok tok = Tok::Normal;
};

class Highlighter {
 public:
  explicit Highlighter(Lang lang);
  void setLang(Lang l);
  Lang lang() const;

  // 纯函数:给定行首状态,填充 spans(先 clear),返回行尾状态。
  uint8_t scanLine(const std::string& line, uint8_t entryState,
                   std::vector<Span>& out) const;

  static bool isKeyword(const std::string& w, Lang l);
  static bool isTypeWord(const std::string& w, Lang l);

 private:
  Lang lang_ = Lang::Cpp;
};

class HighlightCache {
 public:
  // 行数变化时调用(多出来的行状态置 0 并从变化处失效)。
  void resize(int lineCount);
  // 第 line 行(含)之后的缓存作废。
  void invalidateFrom(int line);
  void clear();
  // 取第 line 行的行首状态;必要时从 validUpTo_ 起惰性向下扫描(带收敛提前退出)。
  uint8_t entryState(const TextBuffer& b, const Highlighter& h, int line);
  // 取第 line 行的 spans(内部先保证 entryState 可用)。
  void spansFor(const TextBuffer& b, const Highlighter& h, int line,
                std::vector<Span>& out);
  int validUpTo() const;

 private:
  std::vector<uint8_t> entry_;   // entry_[i] = 第 i 行行首状态
  int validUpTo_ = 0;            // entry_[0 .. validUpTo_-1] 有效
  std::vector<Span> scratch_;    // 复用缓冲,避免每行一次分配
};

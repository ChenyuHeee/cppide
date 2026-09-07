// textbuf.cpp —— 行数组文本缓冲 + 逆操作日志撤销。实现 src/textbuf.h。
//
// 实现要点(与 architecture.md §2.1 对齐):
//   * lines_ 永远至少 1 行:唯一会删行的地方是 rawErase,而它从不删除 r.a.line
//     所在的那一行(只把 b 行的尾巴接过来),所以不变式在每条改动路径后自然成立。
//   * insert / erase 是唯一改动存储的原语;它们各自 revision_++、dirty_=true、
//     并把逆操作交给 recordOp()。undo/redo 走 applyOpNoJournal(),期间 journaling_=false。
//   * 越界不崩:所有外部 Pos/Range 先过 clampPos()/clampRange()(负数夹到 0、
//     行号夹到末行、col 夹到行长并退回 UTF-8 字符边界、a>b 自动交换)。
//   * 撤销分组:beginGroup() 在最外层**立刻压入**一个空组,于是"当前组"永远是
//     undo_.back(),recordOp() 不需要额外的状态位;合并判定推迟到 endGroup()
//     ——把刚关闭的组并进上一组,而不是在开组时猜。这样只用头文件冻结的那几个
//     成员就能表达全部状态(group_open_ = "undo_.back() 还能接受下一次合并")。
//
// 关于本文件**不依赖 util.cpp**:
//   计时只需要相对间隔(800ms 合并窗口),这里用 <chrono> steady_clock 直接实现,
//   语义与 util::nowMs() 完全一致(单调时钟毫秒);UTF-8 只需要"是否续字节"这一
//   个判断,内联在本文件。文件读写用 POSIX open/read/write + rename 自己做原子写。
//   因此 textbuf.o 可以单独链接,不引入 util.o。
//
// 关于换行风格(CRLF)的决定:
//   textbuf.h 已冻结,没有保存换行风格的成员,所以**不可能**在保存时还原 CRLF。
//   决定:loadFile() 把行尾的 "\r\n" 一律规范化成 LF(行中间孤立的 '\r' 原样保留,
//   保证二进制内容不被改写),saveFile() 一律写 LF。副作用是 CRLF 文件保存后变成
//   LF 文件——这对"单文件 ICPC 源码编辑器"是可接受且常见的行为,且缓冲区内容的
//   保存/加载往返是逐字节稳定的。同理:没有末尾换行的文件保存后会补一个末尾换行
//   (与 text() 的契约一致),空缓冲区保存为 0 字节文件。

#include "textbuf.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {

constexpr int64_t kMergeWindowMs = 800;   // §2.1 合并窗口
constexpr size_t kMaxUndoGroups = 2000;   // §2.1 撤销栈上限

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline bool isCont(unsigned char c) { return (c & 0xC0) == 0x80; }

// 静态空行:line() 越界时返回它的引用(必须有稳定地址)。
const std::string& emptyLine() {
  static const std::string kEmpty;
  return kEmpty;
}

// label 是静态字符串字面量,通常比指针就够;但不同 TU 的同内容字面量未必同址,
// 所以退化到 strcmp,语义更稳。两个 nullptr 视为同一个"匿名 label"。
bool sameLabel(const char* a, const char* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  return std::strcmp(a, b) == 0;
}

// text 从 at 插入后的结束位置(与 rawInsert 的返回值规则必须完全一致)。
Pos advancePos(Pos at, const std::string& text) {
  size_t last_nl = text.rfind('\n');
  if (last_nl == std::string::npos) {
    return Pos{at.line, at.col + static_cast<int>(text.size())};
  }
  int nl = 0;
  for (char c : text) {
    if (c == '\n') ++nl;
  }
  return Pos{at.line + nl, static_cast<int>(text.size() - last_nl - 1)};
}

// 组内是否只有一次"单个 UTF-8 字符、不含换行"的插入 —— 合并规则的种子形态。
bool isMergeSeed(const UndoGroup& g) {
  if (g.ops.size() != 1) return false;
  const UndoOp& op = g.ops[0];
  if (!op.isInsert) return false;
  const std::string& t = op.text;
  if (t.empty() || t.size() > 6) return false;
  if (t.find('\n') != std::string::npos) return false;
  if (isCont(static_cast<unsigned char>(t[0]))) return false;
  for (size_t i = 1; i < t.size(); ++i) {
    if (!isCont(static_cast<unsigned char>(t[i]))) return false;  // 不止一个字符
  }
  return true;
}

// journaling_ 的异常安全开关:undo/redo 中途抛异常也要恢复日志记录。
struct JournalPause {
  bool& flag;
  bool saved;
  explicit JournalPause(bool& f) : flag(f), saved(f) { flag = false; }
  ~JournalPause() { flag = saved; }
  JournalPause(const JournalPause&) = delete;
  JournalPause& operator=(const JournalPause&) = delete;
};

constexpr int kNoCursor = -1;   // cursorBefore/After 未记录的哨兵(输出前一律 clamp)

// Range 规范化 + 夹到合法范围。写成文件内自由函数而不是成员,因为 textbuf.h 已冻结。
Range clampRange(const TextBuffer& b, const Range& r0) {
  Range r = r0.normalized();
  r.a = b.clampPos(r.a);
  r.b = b.clampPos(r.b);
  return r.normalized();   // clampPos 是单调的,这一步只为绝对安全
}

}  // namespace

// ------------------------------------------------------------------ Pos 比较
bool operator==(const Pos& a, const Pos& b) { return a.line == b.line && a.col == b.col; }
bool operator!=(const Pos& a, const Pos& b) { return !(a == b); }
bool operator<(const Pos& a, const Pos& b) {
  return a.line != b.line ? a.line < b.line : a.col < b.col;
}
bool operator<=(const Pos& a, const Pos& b) { return !(b < a); }

// ------------------------------------------------------------------ 构造 / reset
TextBuffer::TextBuffer() { lines_.emplace_back(); }

void TextBuffer::reset(std::vector<std::string> lines) {
  lines_ = std::move(lines);
  if (lines_.empty()) lines_.emplace_back();
  clearHistory();
  ++revision_;
  dirty_ = false;
}

// ------------------------------------------------------------------ 只读访问
int TextBuffer::lineCount() const { return static_cast<int>(lines_.size()); }

const std::string& TextBuffer::line(int i) const {
  if (i < 0 || i >= static_cast<int>(lines_.size())) return emptyLine();
  return lines_[static_cast<size_t>(i)];
}

int TextBuffer::lineLen(int i) const { return static_cast<int>(line(i).size()); }

const std::vector<std::string>& TextBuffer::lines() const { return lines_; }

std::string TextBuffer::text() const {
  size_t n = 0;
  for (const std::string& l : lines_) n += l.size() + 1;
  std::string out;
  out.reserve(n);
  for (const std::string& l : lines_) {
    out += l;
    out += '\n';
  }
  return out;
}

std::string TextBuffer::textRange(const Range& r0) const {
  Range r = clampRange(*this, r0);
  std::string out;
  if (r.empty()) return out;
  if (r.a.line == r.b.line) {
    const std::string& l = lines_[static_cast<size_t>(r.a.line)];
    return l.substr(static_cast<size_t>(r.a.col),
                    static_cast<size_t>(r.b.col - r.a.col));
  }
  out += lines_[static_cast<size_t>(r.a.line)].substr(static_cast<size_t>(r.a.col));
  for (int i = r.a.line + 1; i < r.b.line; ++i) {
    out += '\n';
    out += lines_[static_cast<size_t>(i)];
  }
  out += '\n';
  out += lines_[static_cast<size_t>(r.b.line)].substr(0, static_cast<size_t>(r.b.col));
  return out;
}

Pos TextBuffer::clampPos(Pos p) const {
  if (p.line < 0) { p.line = 0; p.col = 0; }
  if (p.line >= static_cast<int>(lines_.size())) {
    p.line = static_cast<int>(lines_.size()) - 1;
    p.col = static_cast<int>(lines_[static_cast<size_t>(p.line)].size());
    return p;
  }
  const std::string& l = lines_[static_cast<size_t>(p.line)];
  if (p.col < 0) p.col = 0;
  if (p.col > static_cast<int>(l.size())) p.col = static_cast<int>(l.size());
  // 退回 UTF-8 字符边界(非法字节序列下最坏退到行首,不会越界)
  while (p.col > 0 && p.col < static_cast<int>(l.size()) &&
         isCont(static_cast<unsigned char>(l[static_cast<size_t>(p.col)]))) {
    --p.col;
  }
  return p;
}

Pos TextBuffer::endPos() const {
  int last = static_cast<int>(lines_.size()) - 1;
  return Pos{last, static_cast<int>(lines_[static_cast<size_t>(last)].size())};
}

bool TextBuffer::hasNonBlank() const {
  for (const std::string& l : lines_) {
    for (char ch : l) {
      unsigned char c = static_cast<unsigned char>(ch);
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\v' && c != '\f') {
        return true;
      }
    }
  }
  return false;
}

// ------------------------------------------------------------------ 存储原语
Pos TextBuffer::rawInsert(Pos at, const std::string& text) {
  // 前置:at 已 clamp,text 非空。
  std::string& first = lines_[static_cast<size_t>(at.line)];
  std::string tail = first.substr(static_cast<size_t>(at.col));
  first.resize(static_cast<size_t>(at.col));

  size_t start = 0;
  size_t nl = text.find('\n');
  if (nl == std::string::npos) {
    first += text;
    first += tail;
    return Pos{at.line, at.col + static_cast<int>(text.size())};
  }
  first.append(text, 0, nl);
  start = nl + 1;

  // 剩下的段落各自成新行,最后一段接上原来的行尾。
  std::vector<std::string> fresh;
  while (true) {
    nl = text.find('\n', start);
    if (nl == std::string::npos) {
      fresh.emplace_back(text, start, std::string::npos);
      break;
    }
    fresh.emplace_back(text, start, nl - start);
    start = nl + 1;
  }
  Pos end{at.line + static_cast<int>(fresh.size()),
          static_cast<int>(fresh.back().size())};
  fresh.back() += tail;
  lines_.insert(lines_.begin() + at.line + 1,
                std::make_move_iterator(fresh.begin()),
                std::make_move_iterator(fresh.end()));
  return end;
}

std::string TextBuffer::rawErase(const Range& r) {
  // 前置:r 已规范化 + clamp,且非空。
  std::string removed;
  if (r.a.line == r.b.line) {
    std::string& l = lines_[static_cast<size_t>(r.a.line)];
    removed = l.substr(static_cast<size_t>(r.a.col),
                       static_cast<size_t>(r.b.col - r.a.col));
    l.erase(static_cast<size_t>(r.a.col), static_cast<size_t>(r.b.col - r.a.col));
    return removed;
  }
  std::string& la = lines_[static_cast<size_t>(r.a.line)];
  const std::string& lb = lines_[static_cast<size_t>(r.b.line)];
  removed += la.substr(static_cast<size_t>(r.a.col));
  for (int i = r.a.line + 1; i < r.b.line; ++i) {
    removed += '\n';
    removed += lines_[static_cast<size_t>(i)];
  }
  removed += '\n';
  removed += lb.substr(0, static_cast<size_t>(r.b.col));

  std::string joined = la.substr(0, static_cast<size_t>(r.a.col)) +
                       lb.substr(static_cast<size_t>(r.b.col));
  lines_[static_cast<size_t>(r.a.line)] = std::move(joined);
  lines_.erase(lines_.begin() + r.a.line + 1, lines_.begin() + r.b.line + 1);
  // r.a.line 那一行从不被删除 -> lines_ 至少还剩 1 行。
  return removed;
}

Pos TextBuffer::insert(Pos at, const std::string& text) {
  at = clampPos(at);
  if (text.empty()) return at;
  Pos end = rawInsert(at, text);
  ++revision_;
  dirty_ = true;
  recordOp(UndoOp{true, at, text});
  return end;
}

Pos TextBuffer::erase(const Range& r0) {
  Range r = clampRange(*this, r0);
  if (r.empty()) return r.a;
  std::string removed = rawErase(r);
  ++revision_;
  dirty_ = true;
  recordOp(UndoOp{false, r.a, std::move(removed)});
  return r.a;
}

// ------------------------------------------------------------------ 撤销分组
void TextBuffer::beginGroup(const char* label) {
  if (group_depth_++ > 0) return;   // 嵌套:只有最外层真正开组
  cur_label_ = label;
  UndoGroup g;
  g.label = label;
  g.stamp = nowMs();
  g.cursorBefore = have_pending_before_ ? pending_before_ : Pos{kNoCursor, kNoCursor};
  g.cursorAfter = Pos{kNoCursor, kNoCursor};
  have_pending_before_ = false;
  undo_.push_back(std::move(g));
  // 注意:此刻 group_open_ 仍描述的是"下面那一组"能否接受合并,endGroup 才用它。
}

void TextBuffer::endGroup() {
  if (group_depth_ <= 0) return;      // 不配对的 endGroup:忽略,不崩
  if (--group_depth_ > 0) return;
  if (undo_.empty()) {                // clearHistory() 在事务中被调用过
    group_open_ = false;
    cur_label_ = nullptr;
    return;
  }
  UndoGroup& g = undo_.back();
  if (g.ops.empty()) {                // 空动作:撤销栈里不留痕迹
    undo_.pop_back();
    cur_label_ = nullptr;
    return;                           // group_open_ 保持原状(下面那一组没被动过)
  }
  if (g.cursorAfter.line == kNoCursor) {
    const UndoOp& last = g.ops.back();
    g.cursorAfter = last.isInsert ? advancePos(last.at, last.text) : last.at;
  }
  last_edit_ms_ = g.stamp;

  // 合并:把刚关闭的组并进上一组(相邻 + 单字符插入 + <800ms + 同 label)
  bool merged = false;
  if (group_open_ && undo_.size() >= 2) {
    UndoGroup& prev = undo_[undo_.size() - 2];
    if (!prev.ops.empty() && prev.ops.back().isInsert &&
        sameLabel(prev.label, g.label) && g.stamp - prev.stamp < kMergeWindowMs &&
        isMergeSeed(g) &&
        advancePos(prev.ops.back().at, prev.ops.back().text) == g.ops[0].at) {
      for (UndoOp& op : g.ops) prev.ops.push_back(std::move(op));
      prev.stamp = g.stamp;
      prev.cursorAfter = g.cursorAfter;
      undo_.pop_back();
      merged = true;
    }
  }
  if (!merged) {
    group_open_ = isMergeSeed(undo_.back());
    if (undo_.size() > kMaxUndoGroups) {
      undo_.erase(undo_.begin(),
                  undo_.begin() + static_cast<long>(undo_.size() - kMaxUndoGroups));
    }
  }
  cur_label_ = nullptr;
}

TextBuffer::Edit::Edit(TextBuffer& b, const char* label) : b_(b) { b_.beginGroup(label); }
TextBuffer::Edit::~Edit() { b_.endGroup(); }

void TextBuffer::noteCursorBefore(Pos p) {
  pending_before_ = p;
  have_pending_before_ = true;
  if (group_depth_ > 0 && !undo_.empty() && undo_.back().ops.empty()) {
    undo_.back().cursorBefore = p;   // 组已开但还没记 op,直接补进去
    have_pending_before_ = false;
  }
}

void TextBuffer::noteCursorAfter(Pos p) {
  // Editor 可能在 Edit 作用域内调,也可能在其后调 —— 两种都落到最近的那一组。
  if (!undo_.empty()) undo_.back().cursorAfter = p;
}

void TextBuffer::breakUndoMerge() {
  group_open_ = false;
  cur_label_ = nullptr;
  last_edit_ms_ = 0;
}

void TextBuffer::recordOp(UndoOp op) {
  if (!journaling_) return;
  redo_.clear();                       // 任何新改动清 redo
  if (group_depth_ > 0) {
    if (undo_.empty()) {               // 事务中途 clearHistory() 的兜底
      UndoGroup g;
      g.label = cur_label_;
      g.stamp = nowMs();
      g.cursorBefore = Pos{kNoCursor, kNoCursor};
      g.cursorAfter = Pos{kNoCursor, kNoCursor};
      undo_.push_back(std::move(g));
    }
    UndoGroup& g = undo_.back();
    if (g.ops.empty() && g.cursorBefore.line == kNoCursor) g.cursorBefore = op.at;
    g.ops.push_back(std::move(op));
    return;
  }
  // 没有 Edit 作用域的裸调用:自成一组(同样走合并/上限逻辑)。
  beginGroup(cur_label_);
  UndoGroup& g = undo_.back();
  if (g.cursorBefore.line == kNoCursor) g.cursorBefore = op.at;
  g.ops.push_back(std::move(op));
  endGroup();
}

// ------------------------------------------------------------------ 撤销 / 重做
void TextBuffer::applyOpNoJournal(const UndoOp& op, Pos& cursorOut) {
  // 正向施加 op:isInsert 就插回 text,否则删掉 [at, at+text)。
  // 前置:journaling_ 已被调用方置 false。
  if (op.isInsert) {
    Pos at = clampPos(op.at);
    if (op.text.empty()) { cursorOut = at; return; }
    cursorOut = rawInsert(at, op.text);
  } else {
    Range r = clampRange(*this, Range{op.at, advancePos(op.at, op.text)});
    if (!r.empty()) rawErase(r);
    cursorOut = r.a;
  }
  ++revision_;
  dirty_ = true;
}

bool TextBuffer::undo(Pos& cursorOut) {
  // 事务进行中(Edit 作用域内)拒绝撤销:此时 undo_.back() 是本次动作的占位组,
  // 弹掉它会让配对的 endGroup() 去动别人的组。撤销不是"编辑",Editor 不应该把它
  // 包在 Edit 里;这里选择返回 false 而不是破坏历史。
  if (group_depth_ > 0) return false;
  if (undo_.empty()) return false;
  UndoGroup g = std::move(undo_.back());
  undo_.pop_back();
  {
    JournalPause pause(journaling_);
    Pos cur = g.cursorBefore;
    for (size_t i = g.ops.size(); i-- > 0;) {
      const UndoOp& op = g.ops[i];
      UndoOp inv{!op.isInsert, op.at, op.text};   // 逆操作
      applyOpNoJournal(inv, cur);
    }
  }
  cursorOut = clampPos(g.cursorBefore.line == kNoCursor
                           ? (g.ops.empty() ? Pos{0, 0} : g.ops.front().at)
                           : g.cursorBefore);
  redo_.push_back(std::move(g));
  if (redo_.size() > kMaxUndoGroups) {
    redo_.erase(redo_.begin(),
                redo_.begin() + static_cast<long>(redo_.size() - kMaxUndoGroups));
  }
  group_open_ = false;                 // 撤销后不再跟任何旧组合并
  cur_label_ = nullptr;
  return true;
}

bool TextBuffer::redo(Pos& cursorOut) {
  if (group_depth_ > 0) return false;   // 同 undo():事务进行中不动历史
  if (redo_.empty()) return false;
  UndoGroup g = std::move(redo_.back());
  redo_.pop_back();
  Pos cur = g.cursorAfter;
  {
    JournalPause pause(journaling_);
    for (const UndoOp& op : g.ops) applyOpNoJournal(op, cur);
  }
  cursorOut = clampPos(g.cursorAfter.line == kNoCursor ? cur : g.cursorAfter);
  undo_.push_back(std::move(g));
  if (undo_.size() > kMaxUndoGroups) {
    undo_.erase(undo_.begin(),
                undo_.begin() + static_cast<long>(undo_.size() - kMaxUndoGroups));
  }
  group_open_ = false;
  cur_label_ = nullptr;
  return true;
}

bool TextBuffer::canUndo() const { return !undo_.empty(); }
bool TextBuffer::canRedo() const { return !redo_.empty(); }

void TextBuffer::clearHistory() {
  undo_.clear();
  redo_.clear();
  group_open_ = false;
  cur_label_ = nullptr;
  last_edit_ms_ = 0;
  have_pending_before_ = false;
  pending_before_ = Pos{};
}

// ------------------------------------------------------------------ 元信息
bool TextBuffer::dirty() const { return dirty_; }
void TextBuffer::clearDirty() { dirty_ = false; }
int TextBuffer::revision() const { return revision_; }
const std::string& TextBuffer::path() const { return path_; }

void TextBuffer::setPath(std::string p) {
  path_ = std::move(p);
  lang_ = langFromPath(path_);
}

Lang TextBuffer::lang() const { return lang_; }

// ★ 全工程**唯一**的"扩展名 -> 语言"判定入口。
//
//   * `main.cpp::checkOpenPath` 用 `== Lang::Unknown` 当"能不能打开这个文件"的门槛;
//   * `build.cpp::syntaxOnly` 也只在本函数认的集合里再问一句"是不是头文件"
//     (头扩展名全部以 h/H 开头,源扩展名全部以 c/C 开头,所以那边不需要第二份清单)。
//     曾经两边各有一份清单:`syntaxOnly` 认 `.h++/.hp/.inc` 而这里不认,于是那三个
//     分支永远走不到(门槛先把文件拒了)。现在统一成一份,并去掉 `.inc`
//     —— 它不是 C/C++ 的标准扩展名,留着等于扩大需求边界。
//
// 大小写规则(有意为之,不是疏漏):
//   * `.c`(小写、单字符)= C 语言;`.C`(大写、单字符)= C++。
//     这是 C/C++ 自 cfront 以来的既有约定,gcc/g++ 至今照此办事,不能因为
//     "大小写不敏感"就把它丢掉。
//   * 其余扩展名一律**大小写不敏感**:`.CPP/.Cpp/.HPP/.CC/.CXX/.H/.C++` 都认。
//     修之前 `langFromPath` 整体大小写敏感,`main.CPP` 会被上面那道门槛拒绝打开。
Lang TextBuffer::langFromPath(const std::string& path) {
  size_t slash = path.find_last_of('/');
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return Lang::Unknown;
  if (slash != std::string::npos && dot < slash) return Lang::Unknown;
  const std::string ext = path.substr(dot + 1);   // 不含 '.'
  if (ext.empty()) return Lang::Unknown;

  // 单字符 c/C:唯一保留大小写区分的一对。
  if (ext == "c") return Lang::C;
  if (ext == "C") return Lang::Cpp;

  // 其余:折成小写再查表。
  // §4.3:.h/.hpp/... 走 cxx + -fsyntax-only,所以按 Cpp 高亮/编译。
  std::string low;
  low.reserve(ext.size());
  for (char ch : ext) {
    low += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
  }
  if (low == "cpp" || low == "cc" || low == "cxx" || low == "c++" ||
      low == "h" || low == "hpp" || low == "hh" || low == "hxx" ||
      low == "h++" || low == "hp") {
    return Lang::Cpp;
  }
  return Lang::Unknown;
}

// ------------------------------------------------------------------ 文件读写
bool TextBuffer::loadFile(const std::string& path, std::string& err) {
  err.clear();
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    err = "打开文件失败:" + path + "(" + std::strerror(errno) + ")";
    return false;
  }
  struct stat st{};
  if (::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
    ::close(fd);
    err = "这是一个目录,不是文件:" + path;
    return false;
  }
  std::string data;
  if (S_ISREG(st.st_mode) && st.st_size > 0) {
    data.reserve(static_cast<size_t>(st.st_size));
  }
  char buf[65536];
  while (true) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n > 0) {
      data.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) break;
    if (errno == EINTR) continue;
    err = "读取文件失败:" + path + "(" + std::strerror(errno) + ")";
    ::close(fd);
    return false;
  }
  ::close(fd);

  // 按 '\n' 拆行;行尾的 "\r\n" 规范化为 LF(行中孤立的 '\r' 保留,二进制安全)。
  // 内容中的 '\0' 也原样保留 —— 全程用 std::string 的长度,不当 C 字符串使。
  std::vector<std::string> ls;
  size_t start = 0;
  while (start <= data.size()) {
    size_t nl = data.find('\n', start);
    if (nl == std::string::npos) {
      if (start < data.size()) ls.emplace_back(data, start, data.size() - start);
      break;                                   // 末尾无换行 -> 最后一段成行
    }
    size_t end = nl;
    if (end > start && data[end - 1] == '\r') --end;
    ls.emplace_back(data, start, end - start);
    start = nl + 1;
  }
  if (ls.empty()) ls.emplace_back();
  reset(std::move(ls));
  setPath(path);
  dirty_ = false;
  return true;
}

bool TextBuffer::saveFile(const std::string& path, std::string& err) {
  err.clear();
  if (path.empty()) {
    err = "保存失败:文件名为空";
    return false;
  }
  // 空缓冲区(单个空行)存成 0 字节文件;其余按 text() 写(末尾必有一个 '\n')。
  std::string data;
  if (!(lines_.size() == 1 && lines_[0].empty())) data = text();

  size_t slash = path.find_last_of('/');
  std::string dir = (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
  if (dir.empty()) dir = "/";
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  // tmp 必须与目标同目录,rename 才是原子的(跨设备 rename 会 EXDEV)。
  std::string tmp = dir + "/.cppide-save-" + base + "." +
                    std::to_string(static_cast<long>(::getpid())) + ".tmp";

  mode_t mode = 0644;
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
    mode = st.st_mode & 07777;
  }
  ::unlink(tmp.c_str());   // 上次崩溃留下的残骸
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
  if (fd < 0) {
    err = "无法写入目录:" + dir + "(" + std::strerror(errno) + ")";
    return false;
  }
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n > 0) {
      off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    err = "写入失败:" + tmp + "(" + std::strerror(errno) + ")";
    ::close(fd);
    ::unlink(tmp.c_str());
    return false;
  }
  if (::fsync(fd) != 0 && errno != EINVAL && errno != EROFS) {
    err = "落盘失败:" + tmp + "(" + std::strerror(errno) + ")";
    ::close(fd);
    ::unlink(tmp.c_str());
    return false;
  }
  if (::close(fd) != 0) {
    err = "关闭文件失败:" + tmp + "(" + std::strerror(errno) + ")";
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    err = "替换目标文件失败:" + path + "(" + std::strerror(errno) + ")";
    ::unlink(tmp.c_str());
    return false;
  }
  clearDirty();
  breakUndoMerge();     // §2.1:保存是结构性操作,立刻断开合并窗口
  return true;
}

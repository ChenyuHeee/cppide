// editor.cpp —— 光标 / 视口 / 编辑操作 / 自动缩进 / Ghost
//
// 本文件承载 §6“练习模式绝不插入缓冲区”的咽喉点:acceptGhost()。
// 它开头的四道 early-return 与 architecture.md §6 的代码逐字一致,顺序也一致:
//     !complete → text.empty() → sink != GhostText → gen != expected_gen_
// setGhost() 里除 assert 外还有一条**运行期** early-return —— assert 在 NDEBUG
// 下会消失,运行期检查才是真正的防线。
//
// 其它不变式:
//   * Pos::col 永远落在 UTF-8 字符边界(经 util::utf8Next/Prev 与 buf_.clampPos)。
//   * 一次用户动作 = 一个 TextBuffer::Edit 作用域,单次 undo 就能回退干净。
//   * undo()/redo() **不能**包进 Edit:TextBuffer 在 group_depth_>0 时拒绝撤销。
//   * 字节列 <-> 显示列的换算只经由 util::byteToDisplayCol / displayColToByte。
//   * 编辑区不折行(长行横向滚动)。

#include "editor.h"

#include <cassert>
#include <algorithm>
#include <string>

#include "util.h"

namespace {

// 词移动用的字符分类。>=0x80 的码点(中文/emoji)统一算“词”,
// 这样按词移动不会在一个汉字内部停下,也不会把整段中文拆成单字。
enum class CClass { Space, Word, Punct };

CClass classOf(uint32_t cp) {
  if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == '\v' || cp == '\f')
    return CClass::Space;
  if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
      (cp >= '0' && cp <= '9') || cp == '_' || cp >= 0x80)
    return CClass::Word;
  return CClass::Punct;
}

CClass classAt(const std::string& s, int i) {
  uint32_t cp = 0;
  util::utf8Decode(s, static_cast<size_t>(i), cp);
  return classOf(cp);
}

int stepNext(const std::string& s, int i) {
  return static_cast<int>(util::utf8Next(s, static_cast<size_t>(i)));
}
int stepPrev(const std::string& s, int i) {
  return static_cast<int>(util::utf8Prev(s, static_cast<size_t>(i)));
}

// 行首连续空白的字节长度。
int leadingBlankBytes(const std::string& s) {
  int k = 0;
  while (k < static_cast<int>(s.size()) && (s[static_cast<size_t>(k)] == ' ' ||
                                            s[static_cast<size_t>(k)] == '\t')) {
    ++k;
  }
  return k;
}

constexpr int kScrollOff = 2;   // 纵向上下各保留的行数
constexpr int kHStep     = 8;   // 横向滚动跳步(显示列)
constexpr int kHMargin   = 4;   // 光标距右边缘至少留的显示列

// 缩进宽度 / 一级缩进的字面文本。头文件已冻结,故写成文件内自由函数而非成员。
int indentWidthOf(const Config& c) { return c.tab_width > 0 ? c.tab_width : 1; }
std::string indentUnitOf(const Config& c) {
  return c.expand_tab ? std::string(static_cast<size_t>(indentWidthOf(c)), ' ')
                      : std::string("\t");
}

}  // namespace

// ---------------------------------------------------------------- Ghost
void Ghost::clear() {
  text.clear();
  anchor = Pos{};
  gen = 0;
  sink = AiSink::PanelOnly;   // 默认即“不可被接受”,清空后的退化方向是安全的
  complete = false;
}

int Ghost::lineCount() const {
  int n = 1;
  for (char c : text) {
    if (c == '\n') ++n;
  }
  return n;
}

// ---------------------------------------------------------------- 构造 / 缓冲区
Editor::Editor(const Config& cfg)
    : cfg_(cfg), buf_(), hl_(Lang::Cpp), hlc_() {
  hlc_.resize(buf_.lineCount());
  refreshLang();
}

TextBuffer& Editor::buffer() { return buf_; }
const TextBuffer& Editor::buffer() const { return buf_; }

bool Editor::loadFile(const std::string& path, std::string& err) {
  if (!buf_.loadFile(path, err)) return false;
  clearGhost();
  cursor_ = Pos{0, 0};
  desired_display_col_ = 0;
  vp_.top = 0;
  vp_.left = 0;
  clipboard_.clear();
  clip_accumulate_ = false;
  refreshLang();
  ensureCursorVisible();
  return true;
}

bool Editor::saveFile(const std::string& path, std::string& err) {
  buf_.breakUndoMerge();
  // Wave 2 约定:TextBuffer::saveFile() 只 clearDirty(),**不改 path_**。
  // 所以“另存为”必须先 setPath(),否则标题栏/语言推导仍指向旧路径。
  const std::string target = path.empty() ? buf_.path() : path;
  if (!target.empty() && target != buf_.path()) buf_.setPath(target);
  if (!buf_.saveFile(target, err)) return false;
  refreshLang();
  return true;
}

// ---------------------------------------------------------------- 光标 / 视口
Pos Editor::cursor() const { return cursor_; }

void Editor::setCursor(Pos p) {
  cursor_ = buf_.clampPos(p);
  desired_display_col_ = displayColOf(cursor_);
  afterMove();
}

int Editor::desiredDisplayCol() const { return desired_display_col_; }
const Viewport& Editor::viewport() const { return vp_; }

void Editor::setViewSize(int w, int h) {
  vp_.w = w < 0 ? 0 : w;
  vp_.h = h < 0 ? 0 : h;
  ensureCursorVisible();
}

int Editor::cursorDisplayCol() const { return displayColOf(cursor_); }

void Editor::ensureCursorVisible() {
  cursor_ = buf_.clampPos(cursor_);
  const int nl = buf_.lineCount();

  // ---- 纵向:上下各 2 行 scrolloff ----
  if (vp_.h <= 0) {
    // 退化尺寸(窗口高 0):没有“可见区”可言,只保证 top 合法、不越界。
    vp_.top = cursor_.line;
  } else {
    int so = kScrollOff;
    if (vp_.h < 2 * kScrollOff + 1) so = (vp_.h - 1) / 2;   // 太矮就压缩边距
    int top = vp_.top;
    if (cursor_.line - so < top) top = cursor_.line - so;
    if (cursor_.line + so > top + vp_.h - 1) top = cursor_.line + so - vp_.h + 1;
    const int maxtop = std::max(0, nl - vp_.h);
    if (top > maxtop) top = maxtop;          // 文件末尾:不留空屏,牺牲下边距
    if (top > cursor_.line) top = cursor_.line;                  // 兜底
    if (top + vp_.h - 1 < cursor_.line) top = cursor_.line - vp_.h + 1;
    vp_.top = top < 0 ? 0 : top;
  }

  // ---- 横向:按 8 列跳步,光标距右边缘至少 4 列;编辑区不折行 ----
  const int dc = cursorDisplayCol();
  if (vp_.w <= 0) {
    vp_.left = 0;
  } else {
    const int margin = (vp_.w >= kHStep + kHMargin) ? kHMargin : 0;
    int left = vp_.left < 0 ? 0 : vp_.left;
    if (left > dc) {                          // 光标在左边界之外:向左跳步
      const int steps = (left - dc + kHStep - 1) / kHStep;
      left -= steps * kHStep;
      if (left < 0) left = 0;
    }
    const int need = std::max(0, dc - (vp_.w - 1 - margin));
    if (left < need) {                        // 光标离右边缘不足 margin:向右跳步
      const int steps = (need - left + kHStep - 1) / kHStep;
      left += steps * kHStep;
    }
    // 极窄窗口(w < 8)下 8 列跳步可能把光标顶出左边界,此时放弃对齐保可见。
    if (left > dc) left = dc;
    vp_.left = left < 0 ? 0 : left;
  }
}

void Editor::scrollBy(int dlines) {
  const int nl = buf_.lineCount();
  int top = vp_.top + dlines;
  const int maxtop = (vp_.h > 0) ? std::max(0, nl - vp_.h) : std::max(0, nl - 1);
  if (top > maxtop) top = maxtop;
  if (top < 0) top = 0;
  vp_.top = top;
  // 把光标拉回可见区(并且直接落进 scrolloff 内圈,否则紧接着的
  // ensureCursorVisible() 会为了补边距又把 top 顶回去,滚动就“不听话”了)。
  if (vp_.h > 0) {
    const int so = (vp_.h < 2 * kScrollOff + 1) ? (vp_.h - 1) / 2 : kScrollOff;
    int lo = top + so;
    int hi = top + vp_.h - 1 - so;
    if (hi > nl - 1) hi = nl - 1;
    if (lo > hi) lo = hi;
    if (lo < 0) lo = 0;
    if (hi < 0) hi = 0;
    if (cursor_.line < lo) cursor_ = posFromDisplayCol(lo, desired_display_col_);
    else if (cursor_.line > hi) cursor_ = posFromDisplayCol(hi, desired_display_col_);
  }
  buf_.breakUndoMerge();
  clip_accumulate_ = false;
  ensureCursorVisible();
}

// ---------------------------------------------------------------- 移动
void Editor::moveLeft() {
  if (cursor_.col > 0) {
    cursor_.col = stepPrev(buf_.line(cursor_.line), cursor_.col);
  } else if (cursor_.line > 0) {
    --cursor_.line;
    cursor_.col = buf_.lineLen(cursor_.line);
  }
  desired_display_col_ = displayColOf(cursor_);
  afterMove();
}

void Editor::moveRight() {
  const int len = buf_.lineLen(cursor_.line);
  if (cursor_.col < len) {
    cursor_.col = stepNext(buf_.line(cursor_.line), cursor_.col);
  } else if (cursor_.line < buf_.lineCount() - 1) {
    ++cursor_.line;
    cursor_.col = 0;
  }
  desired_display_col_ = displayColOf(cursor_);
  afterMove();
}

void Editor::moveUp() {
  if (cursor_.line > 0) {
    // 关键:**不动** desired_display_col_,穿过短行再回长行不丢列。
    cursor_ = posFromDisplayCol(cursor_.line - 1, desired_display_col_);
  } else {
    cursor_.col = 0;
    desired_display_col_ = 0;
  }
  afterMove();
}

void Editor::moveDown() {
  if (cursor_.line < buf_.lineCount() - 1) {
    cursor_ = posFromDisplayCol(cursor_.line + 1, desired_display_col_);
  } else {
    cursor_.col = buf_.lineLen(cursor_.line);
    desired_display_col_ = displayColOf(cursor_);
  }
  afterMove();
}

void Editor::moveWordLeft() {
  if (cursor_.col == 0) {
    if (cursor_.line > 0) {
      --cursor_.line;
      cursor_.col = buf_.lineLen(cursor_.line);
    }
  } else {
    const std::string& l = buf_.line(cursor_.line);
    int c = cursor_.col;
    while (c > 0) {                              // 先跳过左侧空白
      const int q = stepPrev(l, c);
      if (classAt(l, q) != CClass::Space) break;
      c = q;
    }
    if (c > 0) {                                 // 再跳过同类字符串
      const CClass k = classAt(l, stepPrev(l, c));
      while (c > 0) {
        const int q = stepPrev(l, c);
        if (classAt(l, q) != k) break;
        c = q;
      }
    }
    cursor_.col = c;
  }
  desired_display_col_ = displayColOf(cursor_);
  afterMove();
}

void Editor::moveWordRight() {
  const std::string& l = buf_.line(cursor_.line);
  const int len = static_cast<int>(l.size());
  if (cursor_.col >= len) {
    if (cursor_.line < buf_.lineCount() - 1) {
      ++cursor_.line;
      cursor_.col = 0;
    }
  } else {
    int c = cursor_.col;
    const CClass k = classAt(l, c);
    if (k != CClass::Space) {
      while (c < len && classAt(l, c) == k) c = stepNext(l, c);
    }
    while (c < len && classAt(l, c) == CClass::Space) c = stepNext(l, c);
    cursor_.col = c;
  }
  desired_display_col_ = displayColOf(cursor_);
  afterMove();
}

void Editor::moveHome() {
  const std::string& l = buf_.line(cursor_.line);
  const int first = leadingBlankBytes(l);
  // 智能 Home:先到首个非空白,已经在那里(或整行空白)则到列 0。
  cursor_.col = (cursor_.col == first || first >= static_cast<int>(l.size())) ? 0 : first;
  desired_display_col_ = displayColOf(cursor_);
  afterMove();
}

void Editor::moveEnd() {
  cursor_.col = buf_.lineLen(cursor_.line);
  desired_display_col_ = displayColOf(cursor_);
  afterMove();
}

void Editor::movePageUp() {
  const int step = std::max(1, vp_.h - 1);
  const int line = std::max(0, cursor_.line - step);
  cursor_ = posFromDisplayCol(line, desired_display_col_);
  vp_.top = std::max(0, vp_.top - step);
  afterMove();
}

void Editor::movePageDown() {
  const int step = std::max(1, vp_.h - 1);
  const int line = std::min(buf_.lineCount() - 1, cursor_.line + step);
  cursor_ = posFromDisplayCol(line, desired_display_col_);
  vp_.top = std::min(std::max(0, buf_.lineCount() - 1), vp_.top + step);
  afterMove();
}

void Editor::moveBufferStart() {
  cursor_ = Pos{0, 0};
  desired_display_col_ = 0;
  afterMove();
}

void Editor::moveBufferEnd() {
  cursor_ = buf_.endPos();
  desired_display_col_ = displayColOf(cursor_);
  afterMove();
}

void Editor::gotoLine(int line_1based, int col_bytes) {
  int line = line_1based - 1;
  if (line < 0) line = 0;                                  // 越界:负行号 -> 第 1 行
  if (line > buf_.lineCount() - 1) line = buf_.lineCount() - 1;
  setCursor(Pos{line, col_bytes});                         // setCursor 内部 clamp
}

// ---------------------------------------------------------------- 编辑
void Editor::insertText(const std::string& utf8) {
  if (utf8.empty()) return;
  clearGhost();                                 // 用户一敲字,旧建议立即作废
  const bool one_char = (utf8.find('\n') == std::string::npos) &&
                        (util::utf8Next(utf8, 0) == utf8.size());
  // 单字符用 "insert-char":TextBuffer 只对这个 label 做 800ms 合并窗口,
  // 粘贴/多字符走 "insert-text",不会被粘进相邻的打字组。
  const char* label = one_char ? "insert-char" : "insert-text";
  const int from = cursor_.line;
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, label);
    cursor_ = buf_.insert(cursor_, utf8);
    buf_.noteCursorAfter(cursor_);
  }
  if (!one_char) buf_.breakUndoMerge();
  afterEdit(from);
  desired_display_col_ = displayColOf(cursor_);
}

void Editor::insertNewline() {
  clearGhost();
  const int from = cursor_.line;
  std::string ins = "\n";
  if (cfg_.auto_indent) {
    const std::string& l = buf_.line(cursor_.line);
    int k = leadingBlankBytes(l);
    if (k > cursor_.col) k = cursor_.col;       // 在缩进中间回车:只复制光标之前那段
    ins += l.substr(0, static_cast<size_t>(k));
    const std::string before =
        util::trimRight(l.substr(0, static_cast<size_t>(cursor_.col)));
    if (!before.empty() && before.back() == '{') ins += indentUnitOf(cfg_);
  }
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "newline");
    cursor_ = buf_.insert(cursor_, ins);
    buf_.noteCursorAfter(cursor_);
  }
  buf_.breakUndoMerge();                        // 回车是结构性操作,强制关组
  afterEdit(from);
  desired_display_col_ = displayColOf(cursor_);
}

void Editor::backspace() {
  clearGhost();
  if (cursor_.line == 0 && cursor_.col == 0) return;   // 缓冲区起点:无事可做
  Pos from;
  if (cursor_.col == 0) {
    from = Pos{cursor_.line - 1, buf_.lineLen(cursor_.line - 1)};   // 与上一行合并
  } else {
    const std::string& l = buf_.line(cursor_.line);
    int start = stepPrev(l, cursor_.col);
    if (cfg_.expand_tab && l[static_cast<size_t>(cursor_.col) - 1] == ' ') {
      bool all_space = true;
      for (int i = 0; i < cursor_.col; ++i) {
        if (l[static_cast<size_t>(i)] != ' ') { all_space = false; break; }
      }
      if (all_space) {                          // 处在纯空格缩进区:整级退格
        const int tw = indentWidthOf(cfg_);
        start = ((cursor_.col - 1) / tw) * tw;
      }
    }
    from = Pos{cursor_.line, start};
  }
  const Pos to = cursor_;
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "backspace");
    cursor_ = buf_.erase(Range{from, to});
    buf_.noteCursorAfter(cursor_);
  }
  buf_.breakUndoMerge();
  afterEdit(from.line);
  desired_display_col_ = displayColOf(cursor_);
}

void Editor::del() {
  clearGhost();
  const std::string& l = buf_.line(cursor_.line);
  Pos to;
  if (cursor_.col >= static_cast<int>(l.size())) {
    if (cursor_.line >= buf_.lineCount() - 1) return;   // 最后一行行尾:什么都不做
    to = Pos{cursor_.line + 1, 0};
  } else {
    to = Pos{cursor_.line, stepNext(l, cursor_.col)};
  }
  const Pos at = cursor_;
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "delete");
    buf_.erase(Range{at, to});
    cursor_ = buf_.clampPos(at);
    buf_.noteCursorAfter(cursor_);
  }
  buf_.breakUndoMerge();
  afterEdit(at.line);
  desired_display_col_ = displayColOf(cursor_);
}

void Editor::indent() {
  clearGhost();
  std::string ins;
  if (cfg_.expand_tab) {
    const int tw = indentWidthOf(cfg_);
    const int dc = cursorDisplayCol();
    int n = tw - (dc % tw);
    if (n <= 0) n = tw;
    ins.assign(static_cast<size_t>(n), ' ');   // 补到下一个制表位
  } else {
    ins = "\t";
  }
  const int from = cursor_.line;
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "indent");
    cursor_ = buf_.insert(cursor_, ins);
    buf_.noteCursorAfter(cursor_);
  }
  buf_.breakUndoMerge();
  afterEdit(from);
  desired_display_col_ = displayColOf(cursor_);
}

void Editor::unindent() {
  clearGhost();
  const std::string& l = buf_.line(cursor_.line);
  int remove = 0;
  if (!l.empty() && l[0] == '\t') {
    remove = 1;
  } else {
    const int tw = indentWidthOf(cfg_);
    while (remove < tw && remove < static_cast<int>(l.size()) &&
           l[static_cast<size_t>(remove)] == ' ') {
      ++remove;
    }
  }
  if (remove == 0) return;
  const int line = cursor_.line;
  const int oldcol = cursor_.col;
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "unindent");
    buf_.erase(Range{Pos{line, 0}, Pos{line, remove}});
    cursor_ = buf_.clampPos(Pos{line, std::max(0, oldcol - remove)});
    buf_.noteCursorAfter(cursor_);
  }
  buf_.breakUndoMerge();
  afterEdit(line);
  desired_display_col_ = displayColOf(cursor_);
}

void Editor::cutLine() {
  clearGhost();
  const int ln = cursor_.line;
  const int nl = buf_.lineCount();
  std::string taken = buf_.line(ln);
  taken += '\n';                                 // 行剪贴板一律以 '\n' 结尾
  if (clip_accumulate_) clipboard_ += taken; else clipboard_ = taken;

  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "cut-line");
    if (nl == 1) {
      buf_.erase(Range{Pos{ln, 0}, Pos{ln, buf_.lineLen(ln)}});   // 至少留一行
    } else if (ln == nl - 1) {
      // 最后一行:连上一行的换行一起删,否则会剩下一个空行
      buf_.erase(Range{Pos{ln - 1, buf_.lineLen(ln - 1)}, Pos{ln, buf_.lineLen(ln)}});
    } else {
      buf_.erase(Range{Pos{ln, 0}, Pos{ln + 1, 0}});
    }
    cursor_ = buf_.clampPos(Pos{std::min(ln, buf_.lineCount() - 1), 0});
    buf_.noteCursorAfter(cursor_);
  }
  buf_.breakUndoMerge();
  afterEdit(std::max(0, ln - 1));
  desired_display_col_ = displayColOf(cursor_);
  clip_accumulate_ = true;                       // 连续按 Ctrl-K 累积
}

void Editor::copyLine() {
  // §2.3:Ctrl-D“复制当前行”到内部剪贴板(不改缓冲区),连续按同样累积。
  std::string taken = buf_.line(cursor_.line);
  taken += '\n';
  if (clip_accumulate_) clipboard_ += taken; else clipboard_ = taken;
  clip_accumulate_ = true;
}

void Editor::pasteLines() {
  clearGhost();
  if (clipboard_.empty()) return;
  const bool line_mode = clipboard_.back() == '\n';
  const Pos at = line_mode ? Pos{cursor_.line, 0} : cursor_;
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "paste");
    cursor_ = buf_.insert(at, clipboard_);
    buf_.noteCursorAfter(cursor_);
  }
  buf_.breakUndoMerge();
  afterEdit(at.line);
  desired_display_col_ = displayColOf(cursor_);
}

const std::string& Editor::clipboard() const { return clipboard_; }

void Editor::setClipboard(std::string s) {
  clipboard_ = std::move(s);
  clip_accumulate_ = false;
}

bool Editor::undo() {
  // ★ 绝不包进 TextBuffer::Edit:group_depth_>0 时 TextBuffer::undo() 返回 false。
  clearGhost();
  Pos p = cursor_;
  if (!buf_.undo(p)) return false;
  cursor_ = buf_.clampPos(p);
  hlc_.resize(buf_.lineCount());
  hlc_.invalidateFrom(0);        // 撤销可能改动任意行,整体失效最省心
  desired_display_col_ = displayColOf(cursor_);
  clip_accumulate_ = false;
  ensureCursorVisible();
  return true;
}

bool Editor::redo() {
  clearGhost();
  Pos p = cursor_;
  if (!buf_.redo(p)) return false;
  cursor_ = buf_.clampPos(p);
  hlc_.resize(buf_.lineCount());
  hlc_.invalidateFrom(0);
  desired_display_col_ = displayColOf(cursor_);
  clip_accumulate_ = false;
  ensureCursorVisible();
  return true;
}

// ---------------------------------------------------------------- 查找
bool Editor::find(const std::string& needle, bool forward, bool from_cursor) {
  if (needle.empty()) return false;              // 退化输入:空串不算找到
  const int nl = buf_.lineCount();
  last_search_ = needle;
  const Pos start = from_cursor ? cursor_
                                : (forward ? Pos{0, 0} : buf_.endPos());

  if (forward) {
    // 起点:from_cursor 时从光标后一个字节开始,避免原地命中同一处。
    int from = start.col + (from_cursor ? 1 : 0);
    for (int step = 0; step <= nl; ++step) {     // <= nl:回绕一圈后再扫首行剩余部分
      const int ln = (start.line + step) % nl;
      const std::string& l = buf_.line(ln);
      const size_t f = (from < 0) ? 0 : static_cast<size_t>(from);
      if (f <= l.size()) {
        const size_t hit = l.find(needle, f);
        if (hit != std::string::npos) {
          setCursor(Pos{ln, static_cast<int>(hit)});
          return true;
        }
      }
      from = 0;
    }
  } else {
    // 反向:每行取“起点严格小于 upto 的最后一处”;upto < 0 表示整行都可当起点。
    int upto = from_cursor ? start.col : -1;
    for (int step = 0; step <= nl; ++step) {     // <= nl:回绕一圈后再扫起始行的前半
      const int ln = ((start.line - step) % nl + nl) % nl;
      const std::string& l = buf_.line(ln);
      if (upto != 0) {                           // upto==0:本行没有合法起点,跳过
        const size_t limit = (upto < 0) ? std::string::npos
                                        : static_cast<size_t>(upto - 1);
        const size_t hit = l.rfind(needle, limit);
        if (hit != std::string::npos) {
          setCursor(Pos{ln, static_cast<int>(hit)});
          return true;
        }
      }
      upto = -1;
    }
  }
  return false;
}

const std::string& Editor::lastSearch() const { return last_search_; }
void Editor::setLastSearch(std::string s) { last_search_ = std::move(s); }

// ---------------------------------------------------------------- Ghost
const Ghost& Editor::ghost() const { return ghost_; }

void Editor::setGhost(const Ghost& g) {
  // 两道防线:assert 抓开发期的逻辑错误;运行期 early-return 才是 NDEBUG 下真正
  // 起作用的那一道 —— 退化结果永远是“没有 ghost”,不可能是“代码被插入”。
  assert(g.sink == AiSink::GhostText);
  if (g.sink != AiSink::GhostText) {
    ghost_.clear();
    return;
  }
  ghost_ = g;
  ghost_.anchor = buf_.clampPos(ghost_.anchor);
}

void Editor::clearGhost() { ghost_.clear(); }

void Editor::appendGhostDelta(uint64_t gen, const std::string& delta) {
  if (ghost_.sink != AiSink::GhostText) return;   // 没有活的 ghost(含默认态)
  if (ghost_.gen != gen) return;                  // 过期流片段
  if (ghost_.complete) return;                    // 已收尾,不再追加
  ghost_.text += delta;
}

void Editor::markGhostComplete(uint64_t gen) {
  if (ghost_.sink != AiSink::GhostText) return;
  if (ghost_.gen != gen) return;
  ghost_.complete = true;
}

// ★★ §6 的咽喉点:AI 文本到达 TextBuffer::insert 的唯一路径。
// 下面四行 early-return 与 architecture.md §6 逐字一致,顺序不得调整、一条不得少。
bool Editor::acceptGhost() {
  if (!ghost_.complete)                    return false;   // 生成中不接受
  if (ghost_.text.empty())                 return false;
  if (ghost_.sink != AiSink::GhostText)    return false;   // ★ 结构性拦截
  if (ghost_.gen != expected_gen_)         return false;   // 过期建议不接受
  const Pos anchor = buf_.clampPos(ghost_.anchor);
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "accept-ai");
    cursor_ = buf_.insert(ghost_.anchor, ghost_.text);
    ghost_.clear();
    buf_.noteCursorAfter(cursor_);
  }
  buf_.breakUndoMerge();
  afterEdit(anchor.line);          // 一次动作一个 Edit 组 -> 单次 undo 全回退
  desired_display_col_ = displayColOf(cursor_);
  return true;
}

void Editor::setExpectedGen(uint64_t g) { expected_gen_ = g; }
uint64_t Editor::expectedGen() const { return expected_gen_; }

// ---------------------------------------------------------------- 高亮
Highlighter& Editor::highlighter() { return hl_; }
const Highlighter& Editor::highlighter() const { return hl_; }
HighlightCache& Editor::hlCache() { return hlc_; }

void Editor::onBufferChanged(int from_line) {
  hlc_.resize(buf_.lineCount());          // 行数变了则整体作废(见 highlight.cpp)
  hlc_.invalidateFrom(from_line);
  cursor_ = buf_.clampPos(cursor_);
}

void Editor::refreshLang() {
  Lang l = TextBuffer::langFromPath(buf_.path());
  // 无扩展名 / 未知扩展名的“未命名缓冲区”按 C++ 高亮 —— 这是个 C/C++ 编辑器。
  if (l == Lang::Unknown) l = Lang::Cpp;
  hl_.setLang(l);
  hlc_.clear();
  hlc_.resize(buf_.lineCount());
}

// ---------------------------------------------------------------- 私有
void Editor::afterMove() {
  buf_.breakUndoMerge();                  // 移动光标关掉 800ms 合并窗口
  clip_accumulate_ = false;               // 打断 Ctrl-K 的累积
  hlc_.resize(buf_.lineCount());
  hlc_.invalidateFrom(cursor_.line);      // 便宜:收敛提前退出会立刻停下
  ensureCursorVisible();
}

void Editor::afterEdit(int from_line) {
  onBufferChanged(from_line);
  clip_accumulate_ = false;               // cutLine/copyLine 自己在之后置回 true
  ensureCursorVisible();
}

int Editor::displayColOf(Pos p) const {
  return util::byteToDisplayCol(buf_.line(p.line), p.col, indentWidthOf(cfg_));
}

Pos Editor::posFromDisplayCol(int line, int display_col) const {
  if (line < 0) line = 0;
  if (line > buf_.lineCount() - 1) line = buf_.lineCount() - 1;
  // displayColToByte 落在宽字符/Tab 中间时向左吸附到字符起始字节,
  // 与 byteToDisplayCol 互为逆运算 —— 不要在这里另造一套。
  const int col = util::displayColToByte(buf_.line(line), display_col, indentWidthOf(cfg_));
  return buf_.clampPos(Pos{line, col});
}

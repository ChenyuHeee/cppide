// panel.cpp —— 底部输出面板(编译/运行/AI/输入)与 stdin 缓冲的实现
//
// 本文件是**纯逻辑**:不 include curses.h,不碰终端。
// (panel.h 有成员函数 `int scroll() const;`,curses 的宏会把它打坏 —— 见
//  wave1-report §2.3。绘制在 ui.cpp,那里才允许 curses。)
//
// 三条把“100MB 运行输出 / 中文 AI 流式 delta”兜住的不变式:
//
//  1. **环形裁剪**:lines_.size() 恒 <= max_lines_(默认 5000)。超了丢最旧,
//     且**批量丢**(多丢 1/8 容量)—— 否则每行一次 vector 前端 erase 是 O(n),
//     灌 250 万行就是 O(n²)。批量丢把它摊销成每次 append 常数级搬移。
//  2. **单行字节上限** kMaxLineBytes:没有 '\n' 的 100MB 输出(二进制、进度条)
//     否则会变成一个巨型行,把 layout_ 撑爆(视觉行数 ≈ 字节数/宽度)。
//     超了在 **UTF-8 字符边界**硬分行。两条上限合起来把内存钉死在
//     max_lines_ * kMaxLineBytes ≈ 20MB 量级。
//  3. **流式的“carry 缓冲”是隐式的**:delta 在多字节字符中间被切断时,我们
//     照原样把字节接到最后一行的尾巴上 —— 残缺序列只是**暂时**残缺,下一个
//     delta 一到就自动拼成完整字符。因此“每 1 字节投喂”与“一次性投喂”得到的
//     字节流完全相同(硬分行点也只取决于前 kMaxLineBytes+1 个字节,同样相同)。
//     残缺期间的显示由 util::utf8Decode 兜住(非法字节 = 1 字节的 U+FFFD),
//     wrapDisplay 因此永不把字符切两半。
#include "panel.h"

#include <algorithm>
#include <utility>

#include "util.h"

namespace {

// 单行字节上限。选 4096:比任何真实的编译诊断/运行输出行都长得多,而
// max_lines_(5000) * 4096 ≈ 20MB 是可以接受的最坏内存。
constexpr size_t kMaxLineBytes = 4096;

const PanelLine& emptyPanelLine() {
  static const PanelLine kEmpty;
  return kEmpty;
}

// ---------------------------------------------------------------------------
// UTF-8 边界:返回 (off, off+cap] 区间内 <= off+cap 的最大字符起点。
// 前置条件:off + cap < s.size()。
// 只依赖 s[off..off+cap] 这些字节 —— 这正是“1 字节投喂 == 一次性投喂”的关键:
// 两种投喂方式在这里看到的字节完全一样,分行点因此完全一样。
size_t utf8SplitPoint(const std::string& s, size_t off, size_t cap) {
  size_t p = off + cap;
  while (p > off && util::utf8IsContinuation(static_cast<unsigned char>(s[p]))) --p;
  if (p == off) {
    // 单个字符就比 cap 长(只可能是畸形序列):吃掉整个字符,保证前进。
    p = util::utf8Next(s, off);
    if (p <= off) p = off + 1;
  }
  return p;
}

// 环形裁剪:返回丢弃的行数。批量丢以摊销前端 erase 的 O(n)。
int ringTrim(std::vector<PanelLine>& lines, int max_lines) {
  const size_t cap = (max_lines < 1) ? 1u : static_cast<size_t>(max_lines);
  if (lines.size() <= cap) return 0;
  size_t drop = lines.size() - cap + cap / 8;
  if (drop > lines.size()) drop = lines.size();
  lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(drop));
  return static_cast<int>(drop);
}

// 把最后一行按 kMaxLineBytes 硬分行(UTF-8 边界),边分边裁剪,返回丢弃的行数。
// **一趟扫完**:早期版本是“切头 + substr 剩余”,对 8MB 无换行输出是 O(n²/cap)
// (16GB 拷贝),这里改成一次遍历 + assign,总拷贝 O(n)。
int splitLastIfTooLong(std::vector<PanelLine>& lines, int max_lines) {
  if (lines.empty() || lines.back().text.size() <= kMaxLineBytes) return 0;
  PanelLine proto = lines.back();          // 元信息(颜色/严重度/diag_index)
  const std::string all = std::move(lines.back().text);
  proto.text.clear();
  lines.pop_back();

  int dropped = 0;
  size_t off = 0;
  while (all.size() - off > kMaxLineBytes) {
    const size_t p = utf8SplitPoint(all, off, kMaxLineBytes);
    PanelLine one = proto;
    one.text.assign(all, off, p - off);
    lines.push_back(std::move(one));
    dropped += ringTrim(lines, max_lines);
    off = p;
  }
  PanelLine tail = proto;
  tail.text.assign(all, off, std::string::npos);
  lines.push_back(std::move(tail));
  dropped += ringTrim(lines, max_lines);
  return dropped;
}

// 追加一条完整逻辑行(含超长硬分行 + 裁剪),返回丢弃的行数。
int pushPanelLine(std::vector<PanelLine>& lines, PanelLine ln, int max_lines) {
  int dropped = 0;
  lines.push_back(std::move(ln));
  dropped += ringTrim(lines, max_lines);
  dropped += splitLastIfTooLong(lines, max_lines);
  return dropped;
}

// 顶部视觉行号的合法上界。view_h <= 0 时按 1 行算(高度 0 的面板也不该越界)。
int maxTopOf(const std::vector<VisualLine>& layout, int view_h) {
  const int h = (view_h < 1) ? 1 : view_h;
  const int n = static_cast<int>(layout.size());
  return (n > h) ? (n - h) : 0;
}

// 内容增长后的收尾:被裁掉 dropped 行后,滚动/光标坐标要跟着上移,
// 否则用户正在看的位置会莫名其妙地漂。折行缓存随后一律失效。
// (panel.h 的 private 区只有 rewrap(),不能加成员函数,故写成自由函数。)
void finishGrow(int dropped, std::vector<VisualLine>& layout, bool& layout_dirty,
                int& scroll, int& cursor_line) {
  if (dropped > 0) {
    if (!layout_dirty) {
      // 折行缓存还干净:能精确换算被丢掉的视觉行数。
      const auto it = std::lower_bound(
          layout.begin(), layout.end(), dropped,
          [](const VisualLine& v, int want) { return v.logical < want; });
      scroll -= static_cast<int>(it - layout.begin());
    }
    cursor_line -= dropped;
    if (cursor_line < 0) cursor_line = 0;
    if (scroll < 0) scroll = 0;
  }
  layout_dirty = true;
}

}  // namespace

// ---------------------------------------------------------------------- 标题
const char* panelTitleZh(PanelId id) {
  switch (id) {
    case PanelId::Compile: return "编译";
    case PanelId::Run:     return "运行";
    case PanelId::Ai:      return "AI";
    case PanelId::Input:   return "输入";
  }
  return "?";
}

// ---------------------------------------------------------------------- Panel
Panel::Panel(PanelId id) : id_(id) {}

PanelId Panel::id() const { return id_; }
const char* Panel::titleZh() const { return panelTitleZh(id_); }

void Panel::clear() {
  lines_.clear();
  streaming_ = false;
  scroll_ = 0;
  auto_scroll_ = true;
  cursor_line_ = 0;
  layout_.clear();
  layout_dirty_ = true;
  unread_ = false;
  error_count_ = 0;
}

void Panel::append(const std::string& text, bool is_stderr, int diag_index) {
  streaming_ = false;                     // 普通 append 不续写流式行
  if (text.empty()) return;               // 追加空串不产生行
  // 刻意**不**用 util::splitLines():那会先把整块(运行输出一次可能是 1MB)
  // 物化成一个 vector<string>,峰值内存翻倍。这里逐行扫,边扫边裁剪。
  // 语义与 splitLines 对齐:吃掉 CRLF 的 '\r',末尾换行不产生空尾行。
  int dropped = 0;
  size_t i = 0;
  const size_t n = text.size();
  while (i <= n) {
    size_t nl = text.find('\n', i);
    const bool last = (nl == std::string::npos);
    if (last) nl = n;
    size_t end = nl;
    if (end > i && text[end - 1] == '\r') --end;
    PanelLine ln;
    ln.text.assign(text, i, end - i);
    ln.diag_index = diag_index;
    ln.is_stderr = is_stderr;
    dropped += pushPanelLine(lines_, std::move(ln), max_lines_);
    if (last) break;
    i = nl + 1;
    if (i >= n) break;                    // 末尾换行不产生空尾行
  }
  finishGrow(dropped, layout_, layout_dirty_, scroll_, cursor_line_);
  unread_ = true;
}

void Panel::appendLine(PanelLine ln) {
  streaming_ = false;
  // appendLine 语义是“一条逻辑行”:内嵌的 '\n' 会破坏折行不变式,先拆掉。
  if (ln.text.find('\n') != std::string::npos) {
    const std::vector<std::string> parts = util::splitLines(ln.text);
    int dropped = 0;
    for (const std::string& t : parts) {
      PanelLine one = ln;
      one.text = t;
      dropped += pushPanelLine(lines_, std::move(one), max_lines_);
    }
    finishGrow(dropped, layout_, layout_dirty_, scroll_, cursor_line_);
    unread_ = true;
    return;
  }
  const int dropped = pushPanelLine(lines_, std::move(ln), max_lines_);
  finishGrow(dropped, layout_, layout_dirty_, scroll_, cursor_line_);
  unread_ = true;
}

void Panel::appendStreaming(const std::string& delta) {
  if (delta.empty()) return;
  int dropped = 0;
  if (!streaming_ || lines_.empty()) {
    dropped += pushPanelLine(lines_, PanelLine{}, max_lines_);   // 新开一行可续写
    streaming_ = true;
  }
  size_t i = 0;
  const size_t n = delta.size();
  while (i < n) {
    const size_t nl = delta.find('\n', i);
    const size_t end = (nl == std::string::npos) ? n : nl;
    if (end > i) {
      lines_.back().text.append(delta, i, end - i);
      dropped += splitLastIfTooLong(lines_, max_lines_);
    }
    if (nl == std::string::npos) break;
    // 遇 '\n':收掉当前行(顺手吃 CRLF 的 '\r'),再开一行,继承颜色/严重度。
    {
      std::string& cur = lines_.back().text;
      if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    }
    PanelLine nx;
    nx.is_stderr = lines_.back().is_stderr;
    nx.is_meta = lines_.back().is_meta;
    dropped += pushPanelLine(lines_, std::move(nx), max_lines_);
    i = nl + 1;
  }
  finishGrow(dropped, layout_, layout_dirty_, scroll_, cursor_line_);
  unread_ = true;
}

void Panel::endStreaming() {
  if (streaming_) {
    // 末尾的 '\n' 开出来的空尾行不留(与 append() 的 splitLines 语义一致)。
    if (!lines_.empty() && lines_.back().text.empty() && lines_.size() > 1) {
      lines_.pop_back();
      layout_dirty_ = true;
      if (cursor_line_ >= static_cast<int>(lines_.size())) {
        cursor_line_ = static_cast<int>(lines_.size()) - 1;
      }
    } else if (lines_.size() == 1 && lines_.back().text.empty()) {
      lines_.clear();
      cursor_line_ = 0;
      layout_dirty_ = true;
    }
  }
  streaming_ = false;
}

int Panel::lineCount() const { return static_cast<int>(lines_.size()); }

const PanelLine& Panel::lineAt(int i) const {
  if (i < 0 || i >= static_cast<int>(lines_.size())) return emptyPanelLine();
  return lines_[static_cast<size_t>(i)];
}

const std::vector<PanelLine>& Panel::lines() const { return lines_; }

void Panel::setMaxLines(int n) {
  max_lines_ = (n < 1) ? 1 : n;
  const int dropped = ringTrim(lines_, max_lines_);
  if (dropped > 0) {
    cursor_line_ = std::max(0, cursor_line_ - dropped);
    scroll_ = 0;                 // 视觉坐标已失效,退化到顶部(auto_scroll_ 优先)
    layout_dirty_ = true;
  }
}

size_t Panel::byteSize() const {
  size_t total = 0;
  for (const PanelLine& ln : lines_) total += ln.text.size() + 1;   // +1 = 换行
  return total;
}

// ------------------------------------------------------------- 折行与滚动
void Panel::setViewSize(int w, int h) {
  if (w == view_w_ && h == view_h_) return;
  const bool w_changed = (w != view_w_);
  // 宽度变了要重算折行:先记住“当前视口顶部落在哪个逻辑行”,重算后把它拉回
  // 顶部 —— 否则缩放窗口会把用户甩到莫名其妙的位置。贴底(auto_scroll_)时
  // 不用记锚点,scroll() 天然跟着底部走。
  int anchor_logical = -1;
  if (w_changed && !auto_scroll_ && !lines_.empty()) {
    anchor_logical = visualToLogical(scroll());
  }
  view_w_ = w;
  view_h_ = h;
  if (w_changed) layout_dirty_ = true;
  if (anchor_logical >= 0) {
    scroll_ = logicalToVisual(anchor_logical);     // 内部会按新宽度重算折行
  }
  // 存的 scroll_ 越界无害:scroll() 每次读都夹回合法范围。
}

int Panel::viewWidth() const { return view_w_; }
int Panel::viewHeight() const { return view_h_; }

const std::vector<VisualLine>& Panel::layout() const {
  rewrap();
  return layout_;
}

int Panel::visualLineCount() const {
  rewrap();
  return static_cast<int>(layout_.size());
}

int Panel::scroll() const {
  rewrap();
  const int top_max = maxTopOf(layout_, view_h_);
  if (auto_scroll_) return top_max;              // 贴底:新内容自动跟随
  return std::min(std::max(scroll_, 0), top_max);
}

void Panel::scrollBy(int dlines) { scrollTo(scroll() + dlines); }

void Panel::scrollTo(int visual_line) {
  rewrap();
  const int top_max = maxTopOf(layout_, view_h_);
  scroll_ = std::min(std::max(visual_line, 0), top_max);
  auto_scroll_ = (scroll_ >= top_max);           // 滚到底就重新贴底
}

void Panel::scrollToEnd() {
  rewrap();
  scroll_ = maxTopOf(layout_, view_h_);
  auto_scroll_ = true;
}

bool Panel::autoScroll() const { return auto_scroll_; }

void Panel::setAutoScroll(bool on) {
  auto_scroll_ = on;
  if (on) {
    rewrap();
    scroll_ = maxTopOf(layout_, view_h_);
  }
}

int Panel::logicalToVisual(int logical) const {
  rewrap();
  if (layout_.empty()) return 0;
  if (logical < 0) logical = 0;
  const int last = static_cast<int>(lines_.size()) - 1;
  if (logical > last) logical = last;
  const auto it = std::lower_bound(
      layout_.begin(), layout_.end(), logical,
      [](const VisualLine& v, int want) { return v.logical < want; });
  if (it == layout_.end()) return static_cast<int>(layout_.size()) - 1;
  return static_cast<int>(it - layout_.begin());
}

int Panel::visualToLogical(int visual) const {
  rewrap();
  if (layout_.empty()) return 0;
  if (visual < 0) visual = 0;
  if (visual >= static_cast<int>(layout_.size())) {
    visual = static_cast<int>(layout_.size()) - 1;
  }
  return layout_[static_cast<size_t>(visual)].logical;
}

// ------------------------------------------------------------- 标签栏状态
bool Panel::unread() const { return unread_; }
void Panel::setUnread(bool v) { unread_ = v; }
int Panel::errorCount() const { return error_count_; }
void Panel::setErrorCount(int n) { error_count_ = (n < 0) ? 0 : n; }

// ------------------------------------------------------------- 行光标
int Panel::cursorLine() const {
  if (lines_.empty()) return 0;
  const int last = static_cast<int>(lines_.size()) - 1;
  return std::min(std::max(cursor_line_, 0), last);
}

void Panel::setCursorLine(int i) {
  if (lines_.empty()) {
    cursor_line_ = 0;
    return;
  }
  const int last = static_cast<int>(lines_.size()) - 1;
  cursor_line_ = std::min(std::max(i, 0), last);
  // 让光标行留在视口内(编译面板上下移动光标挑诊断时必须跟着滚)。
  if (view_h_ >= 1) {
    const int v = logicalToVisual(cursor_line_);     // 内部 rewrap
    const int top_max = maxTopOf(layout_, view_h_);
    const int cur = auto_scroll_ ? top_max : std::min(std::max(scroll_, 0), top_max);
    if (v < cur) {
      scroll_ = v;
      auto_scroll_ = false;
    } else if (v >= cur + view_h_) {
      scroll_ = std::min(std::max(v - view_h_ + 1, 0), top_max);
      auto_scroll_ = (scroll_ >= top_max);
    } else if (!auto_scroll_) {
      scroll_ = cur;
    }
  }
}

void Panel::moveCursor(int dlines) { setCursorLine(cursorLine() + dlines); }

// ------------------------------------------------------------- 私有
void Panel::rewrap() const {
  if (!layout_dirty_) return;
  layout_.clear();
  if (view_w_ <= 0) {
    // 没有可用宽度就没法折行(也不能按宽度 1 折 —— 20MB 内容会炸出 2000 万
    // 个视觉行)。退化为“每逻辑行一个视觉行”。
    layout_.reserve(lines_.size());
    for (size_t i = 0; i < lines_.size(); ++i) {
      layout_.push_back(VisualLine{static_cast<int>(i), 0,
                                   static_cast<int>(lines_[i].text.size())});
    }
  } else {
    layout_.reserve(lines_.size());
    for (size_t i = 0; i < lines_.size(); ++i) {
      // wrapDisplay 保证:绝不切断 UTF-8 字符,空行也产出一段 {0,0}。
      const std::vector<util::WrapSeg> segs =
          util::wrapDisplay(lines_[i].text, view_w_, tab_width_);
      for (const util::WrapSeg& s : segs) {
        layout_.push_back(VisualLine{static_cast<int>(i), s.start, s.len});
      }
    }
  }
  layout_dirty_ = false;
}

// ====================================================================
// StdinBuffer —— 内部就是一个 TextBuffer,免费获得编辑与撤销
// ====================================================================
StdinBuffer::StdinBuffer() { buf_.reset({}); }

TextBuffer& StdinBuffer::buffer() { return buf_; }
const TextBuffer& StdinBuffer::buffer() const { return buf_; }

std::string StdinBuffer::data() const {
  // TextBuffer::text() 对“单个空行”会给出 "\n";喂 stdin 时那是凭空多一行,
  // 所以这里自己拼:空缓冲 -> 空串,非空且末尾无换行 -> 补一个。
  std::string s = util::join(buf_.lines(), "\n");
  if (s.empty()) return s;
  if (s.back() != '\n') s += '\n';
  return s;
}

void StdinBuffer::setData(const std::string& s) {
  buf_.reset(util::splitLines(s));      // reset 会清撤销栈
  cursor_ = Pos{0, 0};
  scroll_ = 0;
}

bool StdinBuffer::loadFile(const std::string& path, std::string& err) {
  std::string text;
  if (!util::readFile(path, text, err)) return false;
  setData(text);                        // 刻意不用 buf_.loadFile:不该污染 path()/lang()
  err.clear();
  return true;
}

void StdinBuffer::clear() {
  buf_.reset({});
  cursor_ = Pos{0, 0};
  scroll_ = 0;
}

Pos StdinBuffer::cursor() const { return cursor_; }

void StdinBuffer::setCursor(Pos p) {
  cursor_ = buf_.clampPos(p);
  buf_.breakUndoMerge();
  ensureVisible();
}

int StdinBuffer::scroll() const { return scroll_; }

void StdinBuffer::setScroll(int v) {
  const int max_top = (view_h_ >= 1) ? std::max(0, buf_.lineCount() - view_h_) : 0;
  scroll_ = std::min(std::max(v, 0), max_top);
}

void StdinBuffer::setViewSize(int w, int h) {
  view_w_ = w;
  view_h_ = h;
  ensureVisible();
}

void StdinBuffer::insertText(const std::string& utf8) {
  if (utf8.empty()) return;
  clampCursor();
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "input-insert");
    cursor_ = buf_.insert(cursor_, utf8);
  }
  buf_.noteCursorAfter(cursor_);
  ensureVisible();
}

void StdinBuffer::insertNewline() {
  clampCursor();
  buf_.breakUndoMerge();                // 回车结构性操作:关掉合并窗口
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "input-newline");
    cursor_ = buf_.insert(cursor_, "\n");
  }
  buf_.noteCursorAfter(cursor_);
  buf_.breakUndoMerge();
  ensureVisible();
}

void StdinBuffer::backspace() {
  clampCursor();
  if (cursor_.line == 0 && cursor_.col == 0) return;
  Pos from = cursor_;
  if (cursor_.col > 0) {
    from.col = static_cast<int>(
        util::utf8Prev(buf_.line(cursor_.line), static_cast<size_t>(cursor_.col)));
  } else {
    from.line = cursor_.line - 1;
    from.col = buf_.lineLen(from.line);
  }
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "input-backspace");
    cursor_ = buf_.erase(Range{from, cursor_});
  }
  buf_.noteCursorAfter(cursor_);
  ensureVisible();
}

void StdinBuffer::del() {
  clampCursor();
  const int len = buf_.lineLen(cursor_.line);
  Pos to = cursor_;
  if (cursor_.col < len) {
    to.col = static_cast<int>(
        util::utf8Next(buf_.line(cursor_.line), static_cast<size_t>(cursor_.col)));
  } else if (cursor_.line + 1 < buf_.lineCount()) {
    to.line = cursor_.line + 1;
    to.col = 0;
  } else {
    return;                             // 缓冲区末尾:无事可删
  }
  buf_.noteCursorBefore(cursor_);
  {
    TextBuffer::Edit guard(buf_, "input-delete");
    cursor_ = buf_.erase(Range{cursor_, to});
  }
  buf_.noteCursorAfter(cursor_);
  ensureVisible();
}

void StdinBuffer::moveLeft() {
  clampCursor();
  buf_.breakUndoMerge();
  if (cursor_.col > 0) {
    cursor_.col = static_cast<int>(
        util::utf8Prev(buf_.line(cursor_.line), static_cast<size_t>(cursor_.col)));
  } else if (cursor_.line > 0) {
    cursor_.line -= 1;
    cursor_.col = buf_.lineLen(cursor_.line);
  }
  ensureVisible();
}

void StdinBuffer::moveRight() {
  clampCursor();
  buf_.breakUndoMerge();
  if (cursor_.col < buf_.lineLen(cursor_.line)) {
    cursor_.col = static_cast<int>(
        util::utf8Next(buf_.line(cursor_.line), static_cast<size_t>(cursor_.col)));
  } else if (cursor_.line + 1 < buf_.lineCount()) {
    cursor_.line += 1;
    cursor_.col = 0;
  }
  ensureVisible();
}

void StdinBuffer::moveUp() {
  clampCursor();
  buf_.breakUndoMerge();
  if (cursor_.line > 0) {
    cursor_.line -= 1;
    clampCursor();                      // clampPos 顺手对齐到 UTF-8 字符边界
  } else {
    cursor_.col = 0;
  }
  ensureVisible();
}

void StdinBuffer::moveDown() {
  clampCursor();
  buf_.breakUndoMerge();
  if (cursor_.line + 1 < buf_.lineCount()) {
    cursor_.line += 1;
    clampCursor();
  } else {
    cursor_.col = buf_.lineLen(cursor_.line);
  }
  ensureVisible();
}

void StdinBuffer::moveHome() {
  clampCursor();
  buf_.breakUndoMerge();
  cursor_.col = 0;
  ensureVisible();
}

void StdinBuffer::moveEnd() {
  clampCursor();
  buf_.breakUndoMerge();
  cursor_.col = buf_.lineLen(cursor_.line);
  ensureVisible();
}

bool StdinBuffer::undo() {
  // 刻意**不**包在 TextBuffer::Edit 里:事务作用域内 undo() 恒返回 false。
  Pos c = cursor_;
  if (!buf_.undo(c)) return false;
  cursor_ = buf_.clampPos(c);
  ensureVisible();
  return true;
}

bool StdinBuffer::redo() {
  Pos c = cursor_;
  if (!buf_.redo(c)) return false;
  cursor_ = buf_.clampPos(c);
  ensureVisible();
  return true;
}

void StdinBuffer::clampCursor() { cursor_ = buf_.clampPos(cursor_); }

void StdinBuffer::ensureVisible() {
  if (view_h_ < 1) {
    scroll_ = 0;
    return;
  }
  if (cursor_.line < scroll_) scroll_ = cursor_.line;
  if (cursor_.line >= scroll_ + view_h_) scroll_ = cursor_.line - view_h_ + 1;
  const int max_top = std::max(0, buf_.lineCount() - view_h_);
  scroll_ = std::min(std::max(scroll_, 0), max_top);
}

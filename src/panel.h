// panel.h —— 底部输出面板(编译 / 运行 / AI / 输入)与 stdin 缓冲
//
// 布局决策:**单一底部面板 + 标签栏**,不是四个堆叠窗格 —— 24 行的终端
// 养不起四个窗格。只绘制当前聚焦标签的内容;其它标签只显示未读标记。
//
// 与编辑区相反,面板**折行**(中文 AI 输出必须折)。折行结果缓存在 layout(),
// 宽度或内容变化时重算;滚动以“视觉行”为单位。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "textbuf.h"

enum class PanelId : int { Compile = 0, Run = 1, Ai = 2, Input = 3 };
constexpr int kPanelCount = 4;
const char* panelTitleZh(PanelId id);        // "编译"/"运行"/"AI"/"输入"

// 面板里的一条逻辑行。
struct PanelLine {
  std::string text;
  int  diag_index = -1;      // >=0:对应 CompileOutcome::diags 的下标,Enter 可跳转
  bool is_stderr = false;    // 红色绘制
  bool is_meta = false;      // 暗色/加粗:摘要、时间戳、命令行回显
};

// 折行后的一段视觉行:指向 lines_[logical].text 的字节区间 [start, start+len)。
struct VisualLine {
  int logical = 0;
  int start = 0;
  int len = 0;
};

class Panel {
 public:
  explicit Panel(PanelId id);

  PanelId id() const;
  const char* titleZh() const;

  // ---- 内容 ----
  void clear();
  // 按 '\n' 拆成多条 PanelLine 追加。
  void append(const std::string& text, bool is_stderr = false, int diag_index = -1);
  void appendLine(PanelLine ln);
  // 流式追加:续写最后一行(遇 '\n' 才换行)。AI delta 用它。
  void appendStreaming(const std::string& delta);
  void endStreaming();                     // 收尾,关闭“最后一行可续写”状态
  int  lineCount() const;
  const PanelLine& lineAt(int i) const;    // 越界安全,返回静态空行
  const std::vector<PanelLine>& lines() const;
  void setMaxLines(int n);                 // 超出丢最旧(默认 5000)
  size_t byteSize() const;

  // ---- 折行与滚动 ----
  void setViewSize(int w, int h);          // w = 可用显示列,h = 可用行数
  int  viewWidth() const;
  int  viewHeight() const;
  // 折行结果(w 或内容变化后自动重算)。
  const std::vector<VisualLine>& layout() const;
  int  visualLineCount() const;
  int  scroll() const;                     // 顶部视觉行号
  void scrollBy(int dlines);
  void scrollTo(int visual_line);
  void scrollToEnd();
  bool autoScroll() const;                 // 贴底时新内容自动跟随
  void setAutoScroll(bool on);
  int  logicalToVisual(int logical) const; // 该逻辑行的首个视觉行号
  int  visualToLogical(int visual) const;

  // ---- 标签栏状态 ----
  bool unread() const;
  void setUnread(bool v);
  int  errorCount() const;                 // 标签上显示的数字,如 "[编译 2]"
  void setErrorCount(int n);

  // ---- 聚焦时的行光标(编译面板 Enter 跳诊断用)----
  int  cursorLine() const;                 // 逻辑行下标
  void setCursorLine(int i);
  void moveCursor(int dlines);

 private:
  void rewrap() const;                     // 惰性重算 layout_(mutable 缓存)

  PanelId id_;
  std::vector<PanelLine> lines_;
  int max_lines_ = 5000;
  bool streaming_ = false;
  int view_w_ = 0, view_h_ = 0;
  int scroll_ = 0;
  bool auto_scroll_ = true;
  int cursor_line_ = 0;
  int tab_width_ = 4;
  bool unread_ = false;
  int error_count_ = 0;
  mutable std::vector<VisualLine> layout_;
  mutable bool layout_dirty_ = true;
};

// 【输入】面板的内容:内部就是一个 TextBuffer,免费获得编辑与撤销。
// 运行程序时若 cfg.stdin_file 为空,就把它的内容喂给子进程的 stdin。
class StdinBuffer {
 public:
  StdinBuffer();

  TextBuffer& buffer();
  const TextBuffer& buffer() const;
  // 全部文本;非空且末尾无换行时补一个 '\n'(程序读行时更符合预期)。
  std::string data() const;
  void setData(const std::string& s);
  bool loadFile(const std::string& path, std::string& err);
  void clear();

  Pos cursor() const;
  void setCursor(Pos p);
  int  scroll() const;
  void setScroll(int v);
  void setViewSize(int w, int h);

  // 聚焦【输入】面板时的简易编辑(不需要 ghost/高亮,故不复用 Editor)。
  void insertText(const std::string& utf8);
  void insertNewline();
  void backspace();
  void del();
  void moveLeft();
  void moveRight();
  void moveUp();
  void moveDown();
  void moveHome();
  void moveEnd();
  bool undo();
  bool redo();

 private:
  void clampCursor();
  void ensureVisible();

  TextBuffer buf_;
  Pos cursor_{};
  int scroll_ = 0;
  int view_w_ = 0, view_h_ = 0;
};

// editor.h —— 编辑器:光标、视口、编辑操作、Ghost(AI 预览)
//
// ★ 本文件是 §6“练习模式绝不插入缓冲区”的咽喉点所在。
//   AI 产生的文本能到达 TextBuffer::insert 的路径**只有一条**:Editor::acceptGhost()。
//   ai.cpp / aihttp.cpp / ui.cpp 都不持有可写的 TextBuffer&(ui 只见 const&)。
//   acceptGhost() 开头依次拦截:未生成完 / 空 / sink != GhostText / gen 过期。
//   setGhost() 同样拒收 sink != GhostText 的 Ghost(assert + 运行期 early-return),
//   所以写错逻辑的退化结果永远是“没有 ghost”,不可能是“代码被插入”。
//
// 视口:编辑区**不折行**(长行横向滚动),避免“逻辑行 vs 视觉行”的全套复杂度;
// 面板才折行(见 panel.h)。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ai.h"        // 仅为 AiSink 枚举
#include "config.h"
#include "highlight.h"
#include "textbuf.h"

// 编辑区视口(单位:行/显示列)。
struct Viewport {
  int top = 0, left = 0, h = 0, w = 0;
};

// AI 补全预览。它不属于缓冲区,只是画在光标处的暗色文本。
struct Ghost {
  std::string text;
  Pos      anchor;                     // 插入锚点(请求发出时的光标位置)
  uint64_t gen = 0;                    // 产生它的 AiService generation
  AiSink   sink = AiSink::PanelOnly;   // 只有 GhostText 才可能被接受
  bool     complete = false;           // AiDone 之后才为 true;false 时 Tab 不接受

  void clear();
  bool empty() const { return text.empty(); }
  int  lineCount() const;              // text 的行数(至少 1)
};

class Editor {
 public:
  explicit Editor(const Config& cfg);

  // ---- 缓冲区 ----
  TextBuffer& buffer();
  const TextBuffer& buffer() const;
  bool loadFile(const std::string& path, std::string& err);
  bool saveFile(const std::string& path, std::string& err);

  // ---- 光标 / 视口 ----
  Pos cursor() const;
  void setCursor(Pos p);                 // 会 clamp + 重置 desiredDisplayCol
  int  desiredDisplayCol() const;
  const Viewport& viewport() const;
  void setViewSize(int w, int h);        // w/h 为编辑区可用的显示列/行数
  // 纵向保留 2 行 scrolloff;横向按 8 列跳步滚动,光标距右边缘至少 4 列。
  void ensureCursorVisible();
  void scrollBy(int dlines);
  // 光标的显示列(供状态栏与绘制用)。
  int cursorDisplayCol() const;

  // ---- 移动(全部会 breakUndoMerge + ensureCursorVisible)----
  void moveLeft();
  void moveRight();
  void moveUp();
  void moveDown();
  void moveWordLeft();
  void moveWordRight();
  void moveHome();          // 智能 Home:先到首个非空白,再到列 0
  void moveEnd();
  void movePageUp();
  void movePageDown();
  void moveBufferStart();
  void moveBufferEnd();
  void gotoLine(int line_1based, int col_bytes = 0);

  // ---- 编辑 ----
  void insertText(const std::string& utf8);   // 可打印字符 / 多字节字符 / 粘贴文本
  void insertNewline();                       // 带自动缩进(cfg.auto_indent)
  void backspace();
  void del();
  void indent();                              // Tab:expand_tab 则插空格到下一制表位
  void unindent();                            // Shift-Tab
  void cutLine();                             // Ctrl-K:连续按累积到剪贴板
  void copyLine();                            // Ctrl-D
  void pasteLines();                          // Ctrl-V
  const std::string& clipboard() const;
  void setClipboard(std::string s);
  bool undo();
  bool redo();

  // ---- 查找 ----
  // from_cursor=true 从光标后/前开始;找到则移动光标并返回 true(自动回绕一次)。
  bool find(const std::string& needle, bool forward, bool from_cursor);
  const std::string& lastSearch() const;
  void setLastSearch(std::string s);

  // ---- Ghost ----
  const Ghost& ghost() const;
  // 只接受 sink == AiSink::GhostText;否则直接返回(不设置)。
  void setGhost(const Ghost& g);
  void clearGhost();
  // 流式追加:gen 不匹配当前 ghost 则忽略。
  void appendGhostDelta(uint64_t gen, const std::string& delta);
  // 标记生成完成(此后 Tab 才会接受);gen 不匹配则忽略。
  void markGhostComplete(uint64_t gen);
  // ★ 唯一的 AI -> 缓冲区入口。被拦截时返回 false 且不改动任何东西。
  bool acceptGhost();
  // App 在收到 AiStarted 时把当前 generation 告知 Editor,acceptGhost 用它判过期。
  void setExpectedGen(uint64_t g);
  uint64_t expectedGen() const;

  // ---- 高亮 ----
  Highlighter& highlighter();
  const Highlighter& highlighter() const;
  HighlightCache& hlCache();
  // 缓冲区第 from_line 行起发生变化后调用:失效高亮缓存 + 同步行数。
  void onBufferChanged(int from_line);
  void refreshLang();      // 按 buffer().path() 重推语言并重置高亮缓存

 private:
  void afterMove();
  void afterEdit(int from_line);
  int  displayColOf(Pos p) const;
  Pos  posFromDisplayCol(int line, int display_col) const;

  const Config& cfg_;
  TextBuffer buf_;
  Highlighter hl_;
  HighlightCache hlc_;
  Ghost ghost_;
  uint64_t expected_gen_ = 0;
  Pos cursor_{};
  int desired_display_col_ = 0;
  Viewport vp_{};
  std::string clipboard_;
  bool clip_accumulate_ = false;   // 上一个动作也是 cutLine 时累积而非覆盖
  std::string last_search_;
};

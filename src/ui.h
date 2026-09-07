// ui.h —— ncurses 渲染器与配色主题
//
// ★ 本头文件**不 include <curses.h>**,也不出现任何 curses 类型:
//   WINDOW* 之类一律留在 ui.cpp 内部。理由见 keys.h 顶部的宏污染说明。
//   ui.cpp 里请先 `#define NCURSES_NOMACROS` 再 `#include <curses.h>`。
//
// 只读契约:Ui 只通过 const AppModel& 拿数据,**不持有可写的 TextBuffer/Editor**,
// 这是 §6 结构性保证的一部分(渲染层没有任何写缓冲区的能力)。
//
// AppModel 定义在 app.h;这里只用引用,故前置声明,避免 ui.h <-> app.h 成环。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "highlight.h"
#include "keys.h"

struct AppModel;   // app.h

// 屏幕矩形(行/列,均为 0 起)。
struct Rect {
  int y = 0, x = 0, h = 0, w = 0;
  bool empty() const { return h <= 0 || w <= 0; }
};

// 一帧的布局:编辑区(含行号槽)/ 标签栏 / 面板内容区 / 状态栏 / 提示行。
struct Layout {
  Rect gutter;     // 行号槽(cfg.show_line_numbers 为 false 时 w=0)
  Rect editor;     // 编辑文本区
  Rect tabs;       // 面板标签栏(1 行);面板折叠时 h=0
  Rect panel;      // 面板内容区;面板折叠时 h=0
  Rect prompt;     // 提示行(1 行);无提示时 h=0
  Rect status;     // 状态栏(1 行)
};

// 颜色对与属性。COLORS >= 256 时 ghost 用灰色(color 8),否则退化为 A_DIM。
struct Theme {
  // 颜色对编号(1 起,0 是 ncurses 保留的默认对)。
  enum Pair : short {
    P_Normal = 1, P_Keyword, P_Type, P_Preproc, P_String, P_Char, P_Number,
    P_Comment, P_Operator, P_Func, P_Todo,
    P_Ghost, P_LineNo, P_LineNoPractice,
    P_StatusCode, P_StatusPractice, P_StatusBar,
    P_TabActive, P_TabInactive, P_TabUnread,
    P_Error, P_Warn, P_Meta, P_Help,
    P_Count
  };

  bool has_color = false;
  int  colors = 0;         // COLORS
  int  pairs = 0;          // COLOR_PAIRS
  bool wide_ok = true;     // 宽字符可用(能否正常输出中文)

  void init();                        // start_color / use_default_colors / init_pair
  short pairForTok(Tok t) const;      // Tok -> 颜色对编号
  int   attrForTok(Tok t) const;      // 已合成的 attr(含 COLOR_PAIR)
  int   ghostAttr() const;            // 灰色 或 A_DIM
  int   modeAttr(bool practice) const;// 状态栏模式徽章:练习=黄/品红底,写代码=绿/青底
};

class Ui {
 public:
  Ui();
  ~Ui();
  Ui(const Ui&) = delete;
  Ui& operator=(const Ui&) = delete;

  // initscr + raw() + noecho() + keypad(TRUE) + set_escdelay(25) + curs_set(1) + 主题。
  // setlocale(LC_ALL,"") 由 main() 在此之前完成(必须早于 initscr)。
  bool init(std::string& err);
  void shutdown();                      // endwin,幂等
  // 崩溃安全网:atexit / std::set_terminate / SIGSEGV-SIGBUS-SIGABRT handler 里调,
  // 只做 endwin(),不碰任何堆对象。绝不把用户终端留在 raw+noecho 状态。
  static void emergencyShutdown();

  // 等键最多 timeout_ms 毫秒,返回 keys.h 定义的归一化键码。
  // 空闲返回 kKeyNone —— 主循环靠它每 60ms 跳一次,驱动 500ms 停顿判定。
  // 收到 27 时做一次非阻塞取键:有字符则合成 Alt(c),否则就是裸 Esc。
  int getKeyBlockingFor(int timeout_ms);

  void handleResize();                  // 重新取尺寸(不自装 SIGWINCH handler)
  int  rows() const;
  int  cols() const;
  bool tooSmall() const;                // < 60x16:只画一行中文提示

  // 依据模型算布局(面板高度、折叠、行号槽宽度、提示行是否存在)。
  const Layout& relayout(const AppModel& m);
  const Layout& layout() const;

  // 画一帧。内部顺序:编辑区 -> ghost 覆盖 -> 标签栏 -> 面板 -> 提示行 -> 状态栏
  // -> 帮助浮层 -> 最后把真实光标定位到编辑区/提示行(curs_set(1))。
  void draw(const AppModel& m);
  void forceFullRedraw();               // Ctrl-L:clearok + 全量重画
  void beep();

  const Theme& theme() const;

  // --doctor:打印 TERM / COLORS / COLOR_PAIRS / 宽字符能力等自查信息。
  static std::string doctorReport();
  // --doctor 的交互部分:回显按下的键的实际键码,便于用户自查键位。
  std::string describeKey(int key) const;

  // ---- Wave 5 最后一轮**新增**(协调者批准的唯一一处头文件改动;
  //      未改动本文件任何已有签名)----
  // F1 帮助浮层翻页。delta 以**页**为单位:+1 = 下一页,-1 = 上一页;
  // 结果在 [0, 页数) 内回绕(不是夹紧),所以在最后一页按 PgDn 会回到第 1 页。
  // App 在浮层打开时把 ↑/↓/PgUp/PgDn 路由到这里。
  //
  // 为什么这个方法必须存在于 Ui 而不是 AppModel:总页数取决于终端尺寸、分栏数
  // 与 helpLines() 的行数,只有渲染层算得出来;而 AppModel 已被协调者裁决冻结,
  // App 没有别的通道把"翻到第几页"告诉渲染层。
  // 页数取自**上一帧** drawHelp 的计算结果;浮层还没画过任何一帧时按 1 页处理
  // (即调用无副作用),不会越界。
  void scrollHelp(int delta);

 private:
  void drawEditor(const AppModel& m);
  void drawGhost(const AppModel& m);
  void drawTabs(const AppModel& m);
  void drawPanel(const AppModel& m);
  void drawPrompt(const AppModel& m);
  void drawStatus(const AppModel& m);
  void drawHelp(const AppModel& m);
  void drawTooSmall();

  bool inited_ = false;
  int rows_ = 0, cols_ = 0;
  Layout layout_{};
  Theme theme_{};
  std::vector<Span> spans_;      // 复用缓冲,避免每行一次分配
  int cursor_y_ = 0, cursor_x_ = 0;

  // ---- Wave 4 追加(只加 private 成员变量,未改任何已有签名)----
  // 影子高亮缓存:AppModel 只提供 const Editor&,而 Editor::hlCache() 是
  // 非 const 成员(渲染层拿不到),所以 Ui 自备一份。失效判据是
  // TextBuffer::revision();失效起点近似取"本帧与上一帧光标行的较小者"
  // (编辑总发生在光标处)。近似失败最坏只是几行颜色暂时不对,Ctrl-L
  // (forceFullRedraw)会把它整个丢掉重算。
  HighlightCache hl_cache_{};
  int  hl_rev_ = -1;                // 上次同步时的 buffer().revision()
  int  hl_prev_cursor_line_ = 0;    // 上一帧的光标行(失效起点用)
  bool pending_clear_ = false;      // 下一帧是否 clearok(全量重画)

  // ---- Wave 5 追加(同样只加 private 成员变量)----
  // F1 帮助浮层的翻页状态。helpLines() 有 71 行,80x24 的浮层只放得下 ~19 行,
  // 剩下 3/4 的键位在标准终端上根本看不到。
  // 主翻页方式是 `scrollHelp()`(App 把浮层打开时的 ↑/↓/PgUp/PgDn 路由过来);
  // 另外保留一条兜底路径:利用现有的"帮助浮层任意键关闭"行为,
  // **每次重新打开就自动翻到下一页**,于是"连按两下 F1"也是下一页
  // (F1 在某些终端上打不出来时用户还能靠这条;浮层底部两种方式都写明了)。
  int  help_page_ = 0;              // 当前页(会对总页数取模)
  bool help_was_visible_ = false;   // 上一帧是否显示着浮层(用于检测"新打开")
  int  help_pages_ = 1;             // 上一帧 drawHelp 算出的总页数(scrollHelp 用)
};

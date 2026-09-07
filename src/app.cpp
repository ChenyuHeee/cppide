// app.cpp —— 主循环、按键分派、事件排空、编译/运行/AI 编排
//
// ★ 本文件**不 include <curses.h>**:键码由 keys.h 的归一化空间给出
//   (kKeyNone / kKeyResize / Char(cp) / kAltFlag),curses 只存在于 ui.cpp。
// ★ §6 的落地点只有一处:App::onAiDone 里那一个 switch (ev.sink)。
//   除它以外,本文件任何地方都不按"模式"决定 AI 文本的去向。
// ★ 调 util 一律写 util:: 前缀(util::basename 与 glibc ::basename 撞名)。
// ★ TextBuffer::undo/redo 绝不包进 TextBuffer::Edit(Editor 已处理)。
#include "app.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "ai.h"
#include "build.h"   // ★ 组装 shared_ptr<CompileOutcome> 的地方必须自己 include
#include "proc.h"    // ★ 同上:shared_ptr<ProcResult>
#include "util.h"

namespace {

// Lang -> 状态栏用的中文名。
const char* langZh(Lang l) {
  switch (l) {
    case Lang::C: return "C";
    case Lang::Cpp: return "C++";
    default: return "文本";
  }
}

// 面板聚焦时仍然生效的"全局动作"白名单(§8.2 的全局区 + 面板区)。
// 面板聚焦时先由 onPanelKey 消费面板键,剩下的只有这些才转交给 onEditorKey,
// 于是"在面板里打字"绝不可能改到编辑缓冲区。
bool isGlobalAction(Action a) {
  switch (a) {
    case Action::Save:
    case Action::Quit:
    case Action::Compile:
    case Action::Run:
    case Action::CompileAndRun:
    case Action::ToggleAiMode:
    case Action::AskAi:
    case Action::GotoLine:
    case Action::Find:
    case Action::FindNext:
    case Action::NextDiag:
    case Action::PrevDiag:
    case Action::Redraw:
    case Action::Help:
    case Action::FocusCompile:
    case Action::FocusRun:
    case Action::FocusAi:
    case Action::FocusInput:
    case Action::TogglePanel:
    case Action::PanelTaller:
    case Action::PanelShorter:
    case Action::NextTab:
      return true;
    default:
      return false;
  }
}

// AI 未配置时提示用户去哪里填 key。config.cpp 已经把带确切路径的说明放进了
// cfg.warnings(启动时会进 AI 面板),这里只是运行期再提醒一次。
std::string aiHintPath(const Config& cfg) {
  if (!cfg.source_path.empty()) return cfg.source_path;
  const std::string d = ConfigLoader::defaultPath();
  return d.empty() ? std::string("配置文件") : d;
}

std::string aiUnconfiguredMsg(const Config& cfg) {
  // 缺哪一项就说哪一项:model 没有内置默认值(需求不许代码里写死模型名),
  // 所以"key 填了但 model 空着"是一种很常见的半配置状态,必须明确指出来。
  if (cfg.api_key.empty()) {
    return "AI 未配置:请在 " + aiHintPath(cfg) +
           " 填写 api_key(编辑、编译、运行不受影响)。";
  }
  return "AI 未配置:请在 " + aiHintPath(cfg) +
         " 填写 model —— 按 DeepSeek 官方文档写模型名"
         "(编辑、编译、运行不受影响)。";
}

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// =============================================================== 构造 / 初始化
App::App(Config cfg, std::string open_path)
    : cfg_(std::move(cfg)),
      ui_(),
      ed_(cfg_),
      panels_(),
      stdin_buf_(),
      builder_(cfg_),
      events_(),
      runner_(&events_),
      ai_(cfg_, &events_),
      open_path_(std::move(open_path)) {
  panels_.reserve(static_cast<size_t>(kPanelCount));
  for (int i = 0; i < kPanelCount; ++i) panels_.emplace_back(static_cast<PanelId>(i));
  panel_height_ = clampInt(cfg_.panel_height, 3, 40);
}

App::~App() {
  // 在飞工作先取消,再由成员逆序析构:AiService -> Runner(都是 close + join,
  // 绝不 detach)-> ... -> Ui(endwin)。顺序正是 §10 要求的。
  ai_.cancelInFlight();
  runner_.cancelQueued();
}

bool App::init(std::string& err) {
  if (!ui_.init(err)) return false;

  if (!open_path_.empty()) {
    if (!util::fileExists(open_path_)) {
      // Wave 5 裁决:路径不存在 -> 当**新文件**打开。
      // main() 已把"是目录 / 父目录不存在 / 扩展名不受支持"三种情况挡在门外,
      // 所以这里只需要:空缓冲区 + setPath(不 loadFile、不置脏)。
      // §6 提醒:setPath 之后 saveFile(path) 才会写到用户要求的位置。
      ed_.buffer().setPath(open_path_);
      new_file_ = true;
      appendAi("新文件 " + open_path_ + "(尚未存在;Ctrl-O 保存后即创建)", true);
    } else {
      std::string lerr;
      if (!ed_.loadFile(open_path_, lerr)) {
        // main() 已做过"是目录/可读"检查,走到这里只可能是竞态或权限变化:
        // 降级为提示,绝不中断启动。
        appendAi("打开 " + open_path_ + " 失败:" + lerr, true);
        setStatus("打开失败:" + lerr, 6000);
      }
    }
  }
  ed_.refreshLang();

  // cfg.stdin_file 非空时【输入】面板只作展示,实际喂给子进程的是文件内容。
  if (!cfg_.stdin_file.empty()) {
    std::string serr;
    if (!stdin_buf_.loadFile(cfg_.stdin_file, serr)) {
      appendAi("stdin_file 读取失败:" + serr, true);
    }
  }

  relayout();

  // 启动提示:cfg.warnings 必须可见(状态栏 + AI 面板)。
  appendAi(std::string("cppide 就绪 · ") + aiModeZh(ai_.mode()) +
               " · Ctrl-T 切模式 · Ctrl-B 编译 · Ctrl-R 运行 · F1 帮助",
           true);
  for (const std::string& w : cfg_.warnings) appendAi("配置:" + w, true);
  if (!cfg_.warnings.empty()) {
    setStatus(cfg_.warnings.front(), 8000);
  } else if (new_file_) {
    // 新文件的提示优先级高于"就绪":用户最需要知道的是"这是个还不存在的文件"。
    setStatus("新文件 " + util::basename(open_path_) + " · Ctrl-O 保存即创建", 8000);
  } else {
    setStatus(std::string("cppide 就绪 · ") + aiModeZh(ai_.mode()) + " · F1 帮助", 4000);
  }
  if (!ai_.enabled()) panel(PanelId::Ai).setUnread(true);

  last_busy_ = false;
  markDirty();
  return true;
}

// ==================================================================== 主循环
int App::run() {
  const int tick_ms = cfg_.tick_ms > 0 ? cfg_.tick_ms : 60;
  while (!quit_) {
    if (dirty_) {
      ui_.draw(model());
      dirty_ = false;
    }
    const int k = ui_.getKeyBlockingFor(tick_ms);
    if (k == kKeyNone) {          // 空闲:驱动 500ms 停顿判定与事件排空
      tick();
      continue;
    }
    if (k == kKeyResize) {
      ui_.handleResize();
      relayout();
      dirty_ = true;
      continue;
    }
    onKey(k);
    tick();
  }
  return 0;
}

void App::tick() {
  drainEvents();
  // 内部自查 mode==Code && enabled() && !inFlight() && 停顿/限流 && 有非空白字符。
  ai_.maybeAutoTrigger(ed_.buffer(), ed_.cursor());
  const bool busy = runner_.busy();
  if (busy != last_busy_) {       // 运行中/结束:状态栏要跟着变
    last_busy_ = busy;
    markDirty();
  }
  if (!status_msg_.empty() && util::nowMs() >= status_expire_ms_) {
    status_msg_.clear();
    markDirty();
  }
}

// tryPop,永不阻塞;每 tick 最多 200 个,保证键盘输入不被饿死。
void App::drainEvents() {
  AppEvent ev;
  for (int n = 0; n < 200; ++n) {
    if (!events_.tryPop(ev)) break;
    switch (ev.kind) {
      case EvKind::AiStarted:
      case EvKind::AiDelta:
      case EvKind::AiDone:
      case EvKind::AiError:
        // §5.2 第二层过滤:generation 变了说明用户已经继续打字,旧回复作废。
        if (ev.gen != ai_.generation()) continue;
        break;
      default:
        break;
    }
    switch (ev.kind) {
      case EvKind::AiStarted: onAiStarted(ev); break;
      case EvKind::AiDelta: onAiDelta(ev); break;
      case EvKind::AiDone: onAiDone(ev); break;
      case EvKind::AiError: onAiError(ev); break;
      case EvKind::CompileStarted: onCompileStarted(ev); break;
      case EvKind::CompileDone: onCompileDone(ev); break;
      case EvKind::RunStarted: onRunStarted(ev); break;
      case EvKind::RunDone: onRunDone(ev); break;
      case EvKind::Status:
        if (!ev.text.empty()) setStatus(ev.text);
        break;
    }
  }
}

// ================================================================== 按键分派
void App::onKey(int key) {
  const Action a = lookupAction(key);

  // 帮助浮层:↑/↓/PgUp/PgDn 翻页;Esc / F1 关闭;其余任意键也关闭。
  if (help_) {
    // 翻页(Wave 5 最后一轮:协调者批准给 ui.h 新增 Ui::scrollHelp)。
    // 为什么要有这一条:helpLines() 有 71 行,80x24 的浮层一页只放 ~19 行,
    // 修之前只能靠"连按两下 F1"(关掉再打开就翻页)才能看到后 3 页。
    // 那条兜底路径仍然保留(见 ui.h 的注释),这里是正规路径。
    if (a == Action::PageDown || a == Action::MoveDown) {
      ui_.scrollHelp(+1);
      markDirty();
      return;
    }
    if (a == Action::PageUp || a == Action::MoveUp) {
      ui_.scrollHelp(-1);
      markDirty();
      return;
    }
    doHelp(false);
    if (a == Action::Help || a == Action::Escape) return;
    // 其余键不再二次生效,避免"关帮助顺手改了缓冲区"。
    return;
  }
  if (prompt_ != PromptKind::None) {
    onPromptKey(key, a);
    return;
  }
  if (focus_ >= 0) {
    onPanelKey(key, a);
    return;
  }
  onEditorKey(key, a);
}

void App::onEditorKey(int key, Action a) {
  switch (a) {
    // ---------------- 移动 ----------------
    case Action::MoveLeft: ed_.moveLeft(); ai_.noteActivity(); markDirty(); return;
    case Action::MoveRight: ed_.moveRight(); ai_.noteActivity(); markDirty(); return;
    case Action::MoveUp: ed_.moveUp(); ai_.noteActivity(); markDirty(); return;
    case Action::MoveDown: ed_.moveDown(); ai_.noteActivity(); markDirty(); return;
    case Action::MoveWordLeft: ed_.moveWordLeft(); ai_.noteActivity(); markDirty(); return;
    case Action::MoveWordRight: ed_.moveWordRight(); ai_.noteActivity(); markDirty(); return;
    case Action::MoveHome: ed_.moveHome(); ai_.noteActivity(); markDirty(); return;
    case Action::MoveEnd: ed_.moveEnd(); ai_.noteActivity(); markDirty(); return;
    case Action::PageUp: ed_.movePageUp(); ai_.noteActivity(); markDirty(); return;
    case Action::PageDown: ed_.movePageDown(); ai_.noteActivity(); markDirty(); return;
    case Action::BufferStart: ed_.moveBufferStart(); ai_.noteActivity(); markDirty(); return;
    case Action::BufferEnd: ed_.moveBufferEnd(); ai_.noteActivity(); markDirty(); return;

    // ---------------- 编辑 ----------------
    case Action::Enter: ed_.insertNewline(); ai_.noteActivity(); markDirty(); return;
    case Action::Backspace: ed_.backspace(); ai_.noteActivity(); markDirty(); return;
    case Action::Delete: ed_.del(); ai_.noteActivity(); markDirty(); return;
    case Action::Tab: doTab(); return;
    case Action::ShiftTab: ed_.unindent(); ai_.noteActivity(); markDirty(); return;
    case Action::Undo:
      // Editor::undo 内部不包 Edit —— 包了 TextBuffer::undo 会返回 false。
      if (!ed_.undo()) setStatus("没有可撤销的操作");
      ai_.noteActivity();
      markDirty();
      return;
    case Action::Redo:
      if (!ed_.redo()) setStatus("没有可重做的操作");
      ai_.noteActivity();
      markDirty();
      return;
    case Action::CutLine: ed_.cutLine(); ai_.noteActivity(); markDirty(); return;
    case Action::CopyLine: ed_.copyLine(); setStatus("已复制当前行"); markDirty(); return;
    case Action::PasteLine: ed_.pasteLines(); ai_.noteActivity(); markDirty(); return;

    // ---------------- 全局 ----------------
    case Action::Escape: doEscape(); return;
    case Action::Save: doSave(); return;
    case Action::Quit: doQuit(false); return;
    case Action::Compile: doCompile(false); return;
    case Action::Run: doRun(true); return;
    case Action::CompileAndRun: doCompile(true); return;
    case Action::ToggleAiMode: doToggleAiMode(); return;
    case Action::AskAi: doAskAi(); return;
    case Action::GotoLine: openPrompt(PromptKind::GotoLine, "跳到行号: "); return;
    case Action::Find: openPrompt(PromptKind::Find, "查找: ", ed_.lastSearch()); return;
    case Action::FindNext:
      if (ed_.lastSearch().empty()) {
        openPrompt(PromptKind::Find, "查找: ");
      } else if (!ed_.find(ed_.lastSearch(), true, true)) {
        setStatus("未找到:" + ed_.lastSearch());
      }
      markDirty();
      return;
    case Action::NextDiag: cycleDiag(1); return;
    case Action::PrevDiag: cycleDiag(-1); return;
    case Action::Redraw: doRedraw(); return;
    case Action::Help: doHelp(true); return;
    case Action::FocusCompile: focusPanel(PanelId::Compile); return;
    case Action::FocusRun: focusPanel(PanelId::Run); return;
    case Action::FocusAi: focusPanel(PanelId::Ai); return;
    case Action::FocusInput: focusPanel(PanelId::Input); return;
    case Action::TogglePanel: togglePanel(); return;
    case Action::PanelTaller: resizePanel(1); return;
    case Action::PanelShorter: resizePanel(-1); return;
    case Action::NextTab: nextTab(); return;

    default: break;
  }

  // 未绑定的键:可打印字符进缓冲区,其它一律忽略(不响铃,免得误触很吵)。
  if (isTextInput(key)) {
    ed_.insertText(util::codepointToUtf8(charOf(key)));
    ai_.noteActivity();
    markDirty();
  }
}

void App::onPanelKey(int key, Action a) {
  const PanelId id = static_cast<PanelId>(clampInt(focus_, 0, kPanelCount - 1));
  Panel& p = panel(id);

  if (a == Action::Escape) {
    doEscape();
    return;
  }
  if (a == Action::Tab) {   // §8.3:面板聚焦时 Tab 切下一个标签
    nextTab();
    return;
  }

  if (id == PanelId::Input) {
    // §8.3:【输入】面板 = 正常编辑(内部就是一个 TextBuffer,支持撤销)。
    switch (a) {
      case Action::Enter: stdin_buf_.insertNewline(); markDirty(); return;
      case Action::Backspace: stdin_buf_.backspace(); markDirty(); return;
      case Action::Delete: stdin_buf_.del(); markDirty(); return;
      case Action::MoveLeft: stdin_buf_.moveLeft(); markDirty(); return;
      case Action::MoveRight: stdin_buf_.moveRight(); markDirty(); return;
      case Action::MoveUp: stdin_buf_.moveUp(); markDirty(); return;
      case Action::MoveDown: stdin_buf_.moveDown(); markDirty(); return;
      case Action::MoveHome: stdin_buf_.moveHome(); markDirty(); return;
      case Action::MoveEnd: stdin_buf_.moveEnd(); markDirty(); return;
      case Action::PageUp:
        for (int i = 0; i < 5; ++i) stdin_buf_.moveUp();
        markDirty();
        return;
      case Action::PageDown:
        for (int i = 0; i < 5; ++i) stdin_buf_.moveDown();
        markDirty();
        return;
      case Action::Undo:
        if (!stdin_buf_.undo()) setStatus("【输入】没有可撤销的操作");
        markDirty();
        return;
      case Action::Redo:
        if (!stdin_buf_.redo()) setStatus("【输入】没有可重做的操作");
        markDirty();
        return;
      case Action::ShiftTab: return;
      default: break;
    }
    if (isTextInput(key)) {
      stdin_buf_.insertText(util::codepointToUtf8(charOf(key)));
      markDirty();
      return;
    }
  } else {
    // 输出型面板:方向键/PgUp/PgDn 滚动;编译面板用行光标(挑诊断)。
    const int page = std::max(1, p.viewHeight() - 1);
    switch (a) {
      case Action::MoveUp:
        if (id == PanelId::Compile) p.moveCursor(-1); else p.scrollBy(-1);
        markDirty();
        return;
      case Action::MoveDown:
        if (id == PanelId::Compile) p.moveCursor(1); else p.scrollBy(1);
        markDirty();
        return;
      case Action::PageUp:
        if (id == PanelId::Compile) p.moveCursor(-page); else p.scrollBy(-page);
        markDirty();
        return;
      case Action::PageDown:
        if (id == PanelId::Compile) p.moveCursor(page); else p.scrollBy(page);
        markDirty();
        return;
      case Action::MoveHome:
        p.setCursorLine(0);
        p.scrollTo(0);
        markDirty();
        return;
      case Action::MoveEnd:
        p.setCursorLine(p.lineCount() - 1);
        p.scrollToEnd();
        markDirty();
        return;
      case Action::Enter:
        if (id == PanelId::Compile) {
          // 这里就是"编译错误跳到对应行号"的实现点之一。
          jumpToDiag(p.lineAt(p.cursorLine()).diag_index);
        } else if (id == PanelId::Ai) {
          // §8.3:AI 面板 Enter = 打开提问输入行(练习模式的问答入口)。
          if (!ai_.enabled()) {
            appendAi(aiUnconfiguredMsg(cfg_), true);
            setStatus("AI 未配置", 5000);
          } else {
            openPrompt(PromptKind::AiQuestion, "问 AI: ");
          }
        }
        return;
      case Action::MoveLeft:
      case Action::MoveRight:
      case Action::Backspace:
      case Action::Delete:
      case Action::ShiftTab:
      case Action::Undo:
      case Action::Redo:
      case Action::CutLine:
      case Action::CopyLine:
      case Action::PasteLine:
        return;   // 输出面板里这些键无意义,直接吞掉(绝不改到编辑缓冲区)
      default: break;
    }
    if (isTextInput(key)) return;   // 面板里打字不落到缓冲区
  }

  // 剩下的只有全局动作才转交;其它一律忽略。
  if (isGlobalAction(a)) onEditorKey(key, a);
}

void App::onPromptKey(int key, Action a) {
  // 退出确认走单独的 y/n 语义。
  if (prompt_ == PromptKind::ConfirmQuit) {
    if (a == Action::Escape) {
      cancelPrompt();
      return;
    }
    if (isChar(key)) {
      const uint32_t cp = charOf(key);
      if (cp == 'y' || cp == 'Y') {
        cancelPrompt();
        doQuit(true);
        return;
      }
      if (cp == 'n' || cp == 'N') {
        cancelPrompt();
        return;
      }
      if (cp == 's' || cp == 'S') {   // 顺手保存再退
        cancelPrompt();
        doSave();
        if (!ed_.buffer().dirty()) doQuit(true);
        return;
      }
    }
    if (a == Action::Enter) {         // 默认不退出,避免误触丢改动
      cancelPrompt();
      return;
    }
    return;
  }

  switch (a) {
    case Action::Escape: cancelPrompt(); return;
    case Action::Enter: submitPrompt(); return;
    case Action::Backspace: {
      if (prompt_cursor_ > 0) {
        const size_t prev = util::utf8Prev(prompt_input_, static_cast<size_t>(prompt_cursor_));
        prompt_input_.erase(prev, static_cast<size_t>(prompt_cursor_) - prev);
        prompt_cursor_ = static_cast<int>(prev);
      }
      markDirty();
      return;
    }
    case Action::Delete: {
      if (static_cast<size_t>(prompt_cursor_) < prompt_input_.size()) {
        const size_t next = util::utf8Next(prompt_input_, static_cast<size_t>(prompt_cursor_));
        prompt_input_.erase(static_cast<size_t>(prompt_cursor_),
                            next - static_cast<size_t>(prompt_cursor_));
      }
      markDirty();
      return;
    }
    case Action::MoveLeft:
      prompt_cursor_ = static_cast<int>(
          util::utf8Prev(prompt_input_, static_cast<size_t>(prompt_cursor_)));
      markDirty();
      return;
    case Action::MoveRight:
      prompt_cursor_ = static_cast<int>(
          util::utf8Next(prompt_input_, static_cast<size_t>(prompt_cursor_)));
      markDirty();
      return;
    case Action::MoveHome: prompt_cursor_ = 0; markDirty(); return;
    case Action::MoveEnd: prompt_cursor_ = static_cast<int>(prompt_input_.size()); markDirty(); return;
    case Action::CutLine: prompt_input_.clear(); prompt_cursor_ = 0; markDirty(); return;
    default: break;
  }

  if (isTextInput(key)) {
    const std::string s = util::codepointToUtf8(charOf(key));
    prompt_input_.insert(static_cast<size_t>(prompt_cursor_), s);
    prompt_cursor_ += static_cast<int>(s.size());
    markDirty();
  }
}

void App::relayout() {
  const Layout& lay = ui_.relayout(model());
  ed_.setViewSize(lay.editor.w, lay.editor.h);
  ed_.ensureCursorVisible();
  for (Panel& p : panels_) p.setViewSize(lay.panel.w, lay.panel.h);
  stdin_buf_.setViewSize(lay.panel.w, lay.panel.h);
}

void App::markDirty() { dirty_ = true; }

// ================================================================ AI 事件
void App::onAiStarted(const AppEvent& ev) {
  ai_stream_.clear();
  ai_anchor_ = ed_.cursor();          // ghost 的插入锚点 = 请求发出时的光标
  ai_anchor_gen_ = ev.gen;
  ed_.setExpectedGen(ev.gen);
  setStatus("AI 思考中…", 30000);
  markDirty();
}

void App::onAiDelta(const AppEvent& ev) {
  if (ev.text.empty()) return;
  // 这里刻意不判 sink/模式:只累积文本。文本"允许变成什么"由 onAiDone
  // 的那一个 switch 独家决定(§6(2))。
  //
  // ★ ai_stream_ 只用于"本轮收到多少字节"这类进度判断,**绝不落地**:
  //   既不进 ghost 也不进面板。落地只发生在 onAiDone 一处。
  //   为什么:AiDone.text 是完整最终文本(替换语义),如果 delta 也往面板
  //   流式追加,收尾时再整段 append 一次,内容就会显示两遍。
  //   (tests/test_appai.cpp 会 grep 本函数体,禁止出现任何落地调用。)
  ai_stream_ += ev.text;
  markDirty();
}

void App::onAiDone(const AppEvent& ev) {
  // ★ 协调者裁决(Wave 5):AiDone 的语义是**替换**,不是追加。
  //   ev.text 就是本轮的完整最终文本 —— 写代码模式已在 ai.cpp 做完补全后处理,
  //   练习模式已追加过【推理过程】。因此这里**只认 ev.text**,
  //   既不与 ai_stream_ 比长度,也不把 ai_stream_ 再拼上去。
  //   (契约见 ai.cpp 文件头 §5.5 的事件表;tests/test_appai.cpp 钉住"只出现一次"。)
  const std::string& full = ev.text;

  // ★★ 全工程唯一的"AI 回复去哪里"判决点(§6(2))。
  //    GhostText 分支才碰 Editor::setGhost;PanelOnly 分支只往 AI 面板追加。
  switch (ev.sink) {
    case AiSink::GhostText: {
      if (full.empty()) {
        setStatus("AI 没有给出补全", 4000);
        break;
      }
      Ghost g;
      g.text = full;
      g.anchor = (ai_anchor_gen_ == ev.gen) ? ai_anchor_ : ed_.cursor();
      g.gen = ev.gen;
      g.sink = AiSink::GhostText;
      g.complete = true;              // 只有 AiDone 之后 Tab 才接受
      ed_.setExpectedGen(ev.gen);
      ed_.setGhost(g);
      setStatus("AI 建议就绪:Tab 接受,Esc 丢弃", 6000);
      break;
    }
    case AiSink::PanelOnly: {
      Panel& p = panel(PanelId::Ai);
      // 替换语义:先 endStreaming 关掉"最后一行可续写",再整段 append 一次。
      // 全程只有这一处往【AI】面板落地本轮回复,所以内容不可能出现两遍。
      p.endStreaming();
      p.append(full.empty() ? std::string("(AI 没有返回内容)") : full, false, -1);
      p.endStreaming();
      p.append("\n");                 // 与下一条回复隔开
      if (focus_ != static_cast<int>(PanelId::Ai)) p.setUnread(true);
      if (p.autoScroll()) p.scrollToEnd();
      setStatus("AI 思路提示已更新(Alt-3 查看)", 5000);
      break;
    }
  }
  ai_stream_.clear();
  markDirty();
}

void App::onAiError(const AppEvent& ev) {
  const std::string msg = ev.text.empty() ? std::string("AI 请求失败") : ev.text;
  setStatus("AI:" + msg, 5000);       // 状态栏 3~5 秒
  appendAi("[" + util::formatTimeHMS(util::nowEpochMs()) + "] AI 错误:" + msg, true);
  ai_stream_.clear();
  markDirty();
}

// ============================================================ 编译 / 运行事件
void App::onCompileStarted(const AppEvent& ev) {
  if (ev.gen != compile_job_) return;
  setStatus("编译中… " + util::basename(compile_src_),
            std::max(5000, cfg_.compile_timeout_ms + 2000));
  markDirty();
}

void App::onCompileDone(const AppEvent& ev) {
  if (ev.gen != compile_job_) return;   // 过期作业(用户又按了一次 Ctrl-B)
  const ProcResult pr = ev.run ? *ev.run : ProcResult{};

  // §14:诊断解析与 CompileOutcome 的组装责任在 App,不在 Runner。
  auto out = std::make_shared<CompileOutcome>(
      Builder::analyze(pr, compile_binary_, compile_syntax_only_));

  Panel& p = panel(PanelId::Compile);
  // doCompile 已经写了命令回显,面板里已有的行数就是诊断行号的偏移量(§15)。
  const int offset = p.lineCount();
  // §15:面板分行必须与 parseDiagnostics 用同一个切分函数。
  const std::vector<std::string> elines = util::splitLines(out->proc.stderr_text);
  std::vector<int> diag_of(elines.size(), -1);
  for (size_t i = 0; i < out->diags.size(); ++i) {
    const int ol = out->diags[i].out_line;
    if (ol >= 0 && ol < static_cast<int>(elines.size()) && diag_of[static_cast<size_t>(ol)] < 0) {
      diag_of[static_cast<size_t>(ol)] = static_cast<int>(i);
    }
    out->diags[i].out_line = offset + ol;   // 存的是"面板里的行号"
  }
  for (size_t i = 0; i < elines.size(); ++i) {
    PanelLine ln;
    ln.text = elines[i];
    ln.diag_index = diag_of[i];
    ln.is_stderr = true;
    p.appendLine(std::move(ln));
  }
  if (!out->proc.stdout_text.empty()) p.append(out->proc.stdout_text, false, -1);
  if (out->syntax_only) {
    PanelLine ln;
    ln.text = "(头文件仅做语法检查,不产出可执行文件)";
    ln.is_meta = true;
    p.appendLine(std::move(ln));
  }
  if (!out->proc.spawn_error.empty()) {
    PanelLine ln;
    ln.text = "无法启动编译器:" + out->proc.spawn_error + "(检查配置里的 cc / cxx)";
    ln.is_meta = true;
    p.appendLine(std::move(ln));
  }
  {
    PanelLine ln;
    ln.text = out->summaryZh();
    ln.is_meta = true;
    p.appendLine(std::move(ln));
  }
  p.setErrorCount(out->errors);

  last_compile_ = out;
  build_summary_zh_ = out->summaryZh();
  diag_cursor_ = -1;
  setStatus(out->summaryZh(), 6000);

  const bool want_run = run_after_compile_;
  run_after_compile_ = false;

  if (!out->ok) {
    // §8.1:只有"编译失败"会自动抢焦点;且用户正在提示行里打字时不抢。
    if (prompt_ == PromptKind::None) {
      focusPanel(PanelId::Compile);
      if (!out->diags.empty()) p.setCursorLine(out->diags.front().out_line);
    } else {
      p.setUnread(true);
    }
    if (want_run) setStatus("编译失败,未运行 · " + out->summaryZh(), 6000);
  } else {
    if (focus_ != static_cast<int>(PanelId::Compile)) p.setUnread(true);
    p.scrollToEnd();
    if (want_run && !out->syntax_only) doRun(false);
  }
  markDirty();
}

void App::onRunStarted(const AppEvent& ev) {
  if (ev.gen != run_job_) return;
  setStatus("运行中… Esc 终止", std::max(5000, cfg_.run_timeout_ms + 2000));
  markDirty();
}

void App::onRunDone(const AppEvent& ev) {
  if (ev.gen != run_job_) return;
  last_run_ = ev.run;
  const ProcResult pr = ev.run ? *ev.run : ProcResult{};

  Panel& p = panel(PanelId::Run);
  if (!pr.stdout_text.empty()) p.append(pr.stdout_text, false, -1);      // 常规色
  if (!pr.stderr_text.empty()) p.append(pr.stderr_text, true, -1);       // 红色
  if (!pr.spawn_error.empty()) {
    PanelLine ln;
    ln.text = "无法启动程序:" + pr.spawn_error;
    ln.is_meta = true;
    p.appendLine(std::move(ln));
  }
  {
    PanelLine ln;                       // 末行:退出码 / 信号 / 用时 / 是否截断
    ln.text = pr.summary();
    ln.is_meta = true;
    p.appendLine(std::move(ln));
  }
  p.scrollToEnd();
  if (focus_ != static_cast<int>(PanelId::Run)) p.setUnread(true);
  setStatus("运行结束 · " + pr.summary(), 6000);
  markDirty();
}

// ==================================================================== 动作
void App::doSave() {
  const std::string path = ed_.buffer().path();
  if (path.empty()) {
    openPrompt(PromptKind::SaveAs, "另存为(文件名): ");
    return;
  }
  std::string err;
  // Editor::saveFile 内部会先 setPath 再 saveFile(TextBuffer::saveFile 不改 path_)。
  if (ed_.saveFile(path, err)) {
    setStatus((new_file_ ? std::string("已新建并保存 ") : std::string("已保存 ")) +
              util::basename(path));
    new_file_ = false;              // 落盘之后就不再是"新文件"了
  } else {
    setStatus("保存失败:" + err, 8000);
  }
  markDirty();
}

void App::doQuit(bool force) {
  if (!force && ed_.buffer().dirty()) {
    openPrompt(PromptKind::ConfirmQuit, "有未保存的修改。退出?(y=退出 / n=取消 / s=保存并退出) ");
    return;
  }
  quit_ = true;
}

void App::doCompile(bool then_run) {
  if (runner_.busy()) {
    setStatus("已有作业在执行,请稍候(Esc 可终止编译;【运行】面板里 Esc 终止运行)", 4000);
    return;
  }
  if (ed_.buffer().path().empty()) {
    setStatus("请先保存文件再编译", 4000);
    openPrompt(PromptKind::SaveAs, "另存为(文件名): ");
    return;
  }
  if (ed_.buffer().dirty()) {           // 编译的必须是磁盘上的内容
    std::string err;
    if (!ed_.saveFile(ed_.buffer().path(), err)) {
      setStatus("保存失败,未编译:" + err, 8000);
      markDirty();
      return;
    }
  }

  compile_src_ = ed_.buffer().path();
  compile_syntax_only_ = Builder::syntaxOnly(compile_src_);
  compile_binary_ = compile_syntax_only_ ? std::string()
                                         : Builder::tempBinaryPath(compile_src_);
  const ProcSpec spec = builder_.compileSpec(compile_src_, ed_.buffer().lang(), compile_binary_);
  run_after_compile_ = then_run;

  Panel& p = panel(PanelId::Compile);
  p.clear();
  p.setErrorCount(0);
  {
    PanelLine ln;
    ln.text = "$ " + commandLineForDisplay(spec);
    ln.is_meta = true;
    p.appendLine(std::move(ln));        // 这一行就是 §15 说的偏移量来源
  }
  compile_job_ = runner_.submit(spec, JobKind::Compile);
  setStatus("编译中… " + util::basename(compile_src_),
            std::max(5000, cfg_.compile_timeout_ms + 2000));
  markDirty();
}

void App::doRun(bool auto_compile) {
  if (runner_.busy()) {
    setStatus("已有作业在执行,请稍候(Esc 可终止编译;【运行】面板里 Esc 终止运行)", 4000);
    return;
  }
  const std::string src = ed_.buffer().path();
  if (src.empty()) {
    setStatus("请先保存文件再运行", 4000);
    openPrompt(PromptKind::SaveAs, "另存为(文件名): ");
    return;
  }
  if (Builder::syntaxOnly(src)) {
    setStatus("头文件只做语法检查,没有可运行的程序(Ctrl-B 检查语法)", 5000);
    return;
  }
  const std::string bin = Builder::tempBinaryPath(src);
  // §4.4:binaryStale() 或二进制不存在 -> 先自动编译;编译失败则不运行。
  if (auto_compile && (ed_.buffer().dirty() || !util::fileExists(bin) ||
                       Builder::binaryStale(src, bin))) {
    doCompile(true);
    return;
  }
  if (!util::fileExists(bin)) {
    setStatus("还没有可执行文件,请先按 Ctrl-B 编译", 5000);
    return;
  }

  const ProcSpec spec = builder_.runSpec(bin, stdinData());
  Panel& p = panel(PanelId::Run);
  p.clear();
  {
    PanelLine ln;
    ln.text = "$ " + commandLineForDisplay(spec);
    ln.is_meta = true;
    p.appendLine(std::move(ln));
  }
  if (!cfg_.stdin_file.empty()) {
    PanelLine ln;
    ln.text = "(stdin 来自文件 " + cfg_.stdin_file + ")";
    ln.is_meta = true;
    p.appendLine(std::move(ln));
  }
  run_job_ = runner_.submit(spec, JobKind::Run);
  // 显式的 Ctrl-R 才聚焦【运行】面板:这是用户主动请求(不是后台事件抢焦点),
  // 同时保证 §4.2 第三道防线"在【运行】面板按 Esc 终止"是按得到的。
  focusPanel(PanelId::Run);
  setStatus("运行中… Esc 终止", std::max(5000, cfg_.run_timeout_ms + 2000));
  markDirty();
}

void App::doToggleAiMode() {
  const AiMode m = ai_.toggleMode();    // 内部先 gen_++,销毁在飞工作(§6(3))
  if (m == AiMode::Practice) ed_.clearGhost();
  ai_stream_.clear();
  setStatus(std::string("已切换到") + aiModeZh(m), 4000);
  appendAi(std::string("已切换到") + aiModeZh(m) +
               (m == AiMode::Practice ? ":只给中文思路提示,不向缓冲区插入任何代码。"
                                      : ":停顿后自动补全,Tab 接受、Esc 丢弃。"),
           true);
  if (!ai_.enabled()) appendAi(aiUnconfiguredMsg(cfg_), true);
  markDirty();
}

void App::doAskAi() {
  if (!ai_.enabled()) {
    appendAi(aiUnconfiguredMsg(cfg_), true);
    setStatus("AI 未配置(详见【AI】面板,Alt-3)", 5000);
    focusPanel(PanelId::Ai);
    markDirty();
    return;
  }
  if (ai_.mode() == AiMode::Practice) {
    // 练习模式:先在 AI 面板打开提问输入行。
    focusPanel(PanelId::Ai);
    openPrompt(PromptKind::AiQuestion, "问 AI(中文思路提示): ");
    return;
  }
  ai_anchor_ = ed_.cursor();
  ai_stream_.clear();
  ai_.askNow(ed_.buffer(), ed_.cursor(), "");
  setStatus("已请求 AI 补全…", 5000);
  markDirty();
}

void App::doTab() {
  // §6 附带效果:这条路径上一行模式判断都没有。练习模式永远不会有 ghost,
  // 所以 Tab 自动就只是缩进。
  if (ed_.ghost().complete) {
    ed_.acceptGhost();
  } else {
    if (!ed_.ghost().empty()) setStatus("AI 正在生成…", 2000);
    ed_.indent();
  }
  ai_.noteActivity();
  markDirty();
}

void App::doEscape() {
  if (help_) {
    doHelp(false);
    return;
  }
  // §8.2 的优先级链:丢弃 ghost -> 取消提示行 -> 面板取消焦点。
  if (!ed_.ghost().empty()) {
    ed_.clearGhost();
    ai_.cancelInFlight();
    ai_stream_.clear();
    setStatus("已丢弃 AI 建议", 2000);
    markDirty();
    return;
  }
  if (prompt_ != PromptKind::None) {
    cancelPrompt();
    return;
  }
  // §4.2 第三道防线:Esc 终止在飞的子进程作业。终端里 Ctrl-C 不可用
  // (raw 模式且 §18 不许绑它),这是唯一的逃生口。
  //
  // 编译 vs 运行的作用域刻意不同(Wave 5 协调者裁决):
  //   * 运行:只在【运行】面板聚焦时生效 —— doRun 会自动聚焦该面板,
  //     所以用户按 Ctrl-R 之后 Esc 一定按得到;编辑区里的 Esc 留给 ghost。
  //   * 编译:**任何焦点下**都生效 —— 编译卡住(比如模板爆炸、编译器本身挂住)
  //     时用户根本没机会去聚焦面板,只靠 compile_timeout_ms(默认 30s)兜底
  //     体验太差。编辑区里此时也不会有 ghost(上面两个分支已 return)。
  if (runner_.busy()) {
    const Runner::JobId cur = runner_.currentJob();
    const bool is_compile = (cur != 0 && cur == compile_job_);
    const bool is_run = (cur != 0 && cur == run_job_);
    if (is_compile) {
      runner_.killInFlight();
      runner_.cancelQueued();
      run_after_compile_ = false;
      // 把作业号清零:随后必然到达的那个 CompileDone(被 SIGKILL 的编译器)
      // 会在 onCompileDone 的 `ev.gen != compile_job_` 处被丢掉,
      // 免得面板上再出现一条误导人的"编译失败,N 个错误"。
      compile_job_ = 0;
      {
        PanelLine ln;
        ln.text = "(编译已被用户按 Esc 终止)";
        ln.is_meta = true;
        panel(PanelId::Compile).appendLine(std::move(ln));
      }
      build_summary_zh_ = "编译已终止";
      setStatus("已终止编译作业", 4000);
      markDirty();
      return;
    }
    if (is_run && focus_ == static_cast<int>(PanelId::Run)) {
      runner_.killInFlight();
      runner_.cancelQueued();
      run_after_compile_ = false;
      setStatus("已终止运行中的程序", 4000);
      markDirty();
      return;
    }
  }
  if (focus_ >= 0) {
    unfocusPanel();
    return;
  }
  if (ai_.inFlight()) {
    ai_.cancelInFlight();
    setStatus("已取消 AI 请求", 3000);
    markDirty();
  }
}

void App::doHelp(bool on) {
  if (help_ == on) return;
  help_ = on;
  relayout();
  markDirty();
}

void App::doRedraw() {
  ui_.forceFullRedraw();
  relayout();
  markDirty();
}

// ================================================================ 诊断跳转
void App::jumpToDiag(int diag_index) {
  if (!last_compile_) {
    setStatus("还没有编译诊断(Ctrl-B 编译)", 3000);
    return;
  }
  const std::vector<Diag>& ds = last_compile_->diags;
  if (diag_index < 0 || diag_index >= static_cast<int>(ds.size())) return;
  const Diag& d = ds[static_cast<size_t>(diag_index)];

  // 只有 file 的 basename 与当前缓冲区一致的诊断可跳(需求只管单文件)。
  const std::string cur = ed_.buffer().path();
  if (!d.file.empty() && !cur.empty() &&
      util::basename(d.file) != util::basename(cur)) {
    setStatus("该诊断属于 " + util::basename(d.file) + ",不在当前文件里", 4000);
    return;
  }
  if (d.line <= 0) {
    setStatus("该行没有可跳转的行号", 3000);
    return;
  }
  ed_.gotoLine(d.line, d.col > 0 ? d.col - 1 : 0);
  ed_.ensureCursorVisible();
  diag_cursor_ = diag_index;
  panel(PanelId::Compile).setCursorLine(d.out_line);
  setStatus(std::string(diagSevZh(d.sev)) + " 第 " + std::to_string(d.line) + " 行:" + d.message,
            6000);
  markDirty();
}

void App::cycleDiag(int dir) {
  // 可跳的诊断 = basename 与当前缓冲区一致且有行号的那些。
  std::vector<int> jumpable;
  if (last_compile_) {
    const std::string cur = util::basename(ed_.buffer().path());
    for (size_t i = 0; i < last_compile_->diags.size(); ++i) {
      const Diag& d = last_compile_->diags[i];
      if (d.line <= 0) continue;
      if (!d.file.empty() && !cur.empty() && util::basename(d.file) != cur) continue;
      jumpable.push_back(static_cast<int>(i));
    }
  }
  if (jumpable.empty()) {
    // §8.2:无诊断时 Ctrl-N/Ctrl-P 退化为"查找下一处/上一处"。
    if (!ed_.lastSearch().empty()) {
      if (!ed_.find(ed_.lastSearch(), dir > 0, true)) setStatus("未找到:" + ed_.lastSearch());
      markDirty();
      return;
    }
    setStatus(last_compile_ ? "没有可跳转的诊断" : "还没有编译诊断(Ctrl-B 编译)", 3000);
    return;
  }
  // 当前位置在 jumpable 里的下标
  int at = -1;
  for (size_t i = 0; i < jumpable.size(); ++i) {
    if (jumpable[i] == diag_cursor_) {
      at = static_cast<int>(i);
      break;
    }
  }
  int next = 0;
  if (at < 0) {
    next = dir > 0 ? 0 : static_cast<int>(jumpable.size()) - 1;
  } else {
    const int n = static_cast<int>(jumpable.size());
    next = ((at + dir) % n + n) % n;
  }
  jumpToDiag(jumpable[static_cast<size_t>(next)]);
  setStatus(status_msg_ + "  [" + std::to_string(next + 1) + "/" +
                std::to_string(jumpable.size()) + "]",
            6000);
}

// ================================================================== 提示行
void App::openPrompt(PromptKind k, std::string label, std::string initial) {
  prompt_ = k;
  prompt_label_ = std::move(label);
  prompt_input_ = std::move(initial);
  prompt_cursor_ = static_cast<int>(prompt_input_.size());
  relayout();
  markDirty();
}

void App::submitPrompt() {
  const PromptKind k = prompt_;
  const std::string input = util::trim(prompt_input_);
  cancelPrompt();

  switch (k) {
    case PromptKind::GotoLine: {
      if (input.empty()) return;
      char* end = nullptr;
      const long n = std::strtol(input.c_str(), &end, 10);
      if (end == input.c_str() || n <= 0) {
        setStatus("行号无效:" + input, 4000);
        return;
      }
      unfocusPanel();
      ed_.gotoLine(static_cast<int>(n), 0);
      ed_.ensureCursorVisible();
      setStatus("已跳到第 " + std::to_string(n) + " 行", 3000);
      markDirty();
      return;
    }
    case PromptKind::Find: {
      if (input.empty()) return;
      unfocusPanel();
      ed_.setLastSearch(input);
      if (!ed_.find(input, true, true)) {
        setStatus("未找到:" + input, 4000);
      } else {
        setStatus("找到:" + input + "(Ctrl-N 下一处)", 4000);
      }
      markDirty();
      return;
    }
    case PromptKind::AiQuestion: {
      if (input.empty()) return;
      if (!ai_.enabled()) {
        appendAi(aiUnconfiguredMsg(cfg_), true);
        setStatus("AI 未配置", 5000);
        return;
      }
      appendAi("我:" + input, true);
      ai_stream_.clear();
      ai_anchor_ = ed_.cursor();
      ai_.askNow(ed_.buffer(), ed_.cursor(), input);
      setStatus("已发送提问,等待 AI…", 5000);
      markDirty();
      return;
    }
    case PromptKind::SaveAs: {
      if (input.empty()) return;
      const std::string target = util::expandUser(input);
      if (util::isDirectory(target)) {
        setStatus("保存失败:" + target + " 是一个目录", 6000);
        return;
      }
      std::string err;
      // Editor::saveFile 会先 setPath 再写(TextBuffer::saveFile 不改 path_)。
      if (ed_.saveFile(target, err)) {
        ed_.refreshLang();
        new_file_ = false;          // 另存为成功之后也不再是"新文件"
        setStatus("已保存 " + util::basename(target));
      } else {
        setStatus("保存失败:" + err, 8000);
      }
      markDirty();
      return;
    }
    case PromptKind::ConfirmQuit:
    case PromptKind::None:
      return;
  }
}

void App::cancelPrompt() {
  if (prompt_ == PromptKind::None) return;
  prompt_ = PromptKind::None;
  prompt_label_.clear();
  prompt_input_.clear();
  prompt_cursor_ = 0;
  relayout();
  markDirty();
}

// ==================================================================== 面板
Panel& App::panel(PanelId id) {
  const int i = clampInt(static_cast<int>(id), 0, kPanelCount - 1);
  return panels_[static_cast<size_t>(i)];
}

const Panel& App::panel(PanelId id) const {
  const int i = clampInt(static_cast<int>(id), 0, kPanelCount - 1);
  return panels_[static_cast<size_t>(i)];
}

void App::focusPanel(PanelId id) {
  panel_visible_ = true;
  focus_ = static_cast<int>(id);
  Panel& p = panel(id);
  p.setUnread(false);
  relayout();
  markDirty();
}

void App::unfocusPanel() {
  focus_ = -1;
  relayout();
  markDirty();
}

void App::nextTab() {
  if (focus_ < 0) {
    focusPanel(PanelId::Compile);
    return;
  }
  focusPanel(static_cast<PanelId>((focus_ + 1) % kPanelCount));
}

void App::togglePanel() {
  panel_visible_ = !panel_visible_;
  if (!panel_visible_) focus_ = -1;
  relayout();
  markDirty();
}

void App::resizePanel(int delta) {
  const int max_h = std::max(3, ui_.rows() - 6);
  panel_height_ = clampInt(panel_height_ + delta, 3, max_h);
  panel_visible_ = true;
  relayout();
  markDirty();
}

void App::appendAi(const std::string& text, bool meta) {
  if (text.empty()) return;
  Panel& p = panel(PanelId::Ai);
  p.endStreaming();
  if (meta) {
    for (const std::string& l : util::splitLines(text)) {
      PanelLine ln;
      ln.text = l;
      ln.is_meta = true;
      p.appendLine(std::move(ln));
    }
  } else {
    p.append(text, false, -1);
  }
  if (focus_ != static_cast<int>(PanelId::Ai)) p.setUnread(true);
  if (p.autoScroll()) p.scrollToEnd();
  markDirty();
}

void App::setStatus(std::string msg, int ms) {
  status_msg_ = std::move(msg);
  status_expire_ms_ = util::nowMs() + (ms > 0 ? ms : 3000);
  markDirty();
}

std::string App::stdinData() const {
  if (!cfg_.stdin_file.empty()) {
    std::string data, err;
    if (util::readFile(cfg_.stdin_file, data, err)) return data;
    return std::string();   // 读不到就当空 stdin(错误已在启动时提示过)
  }
  return stdin_buf_.data();
}

// ==================================================================== 模型
AppModel App::model() const {
  AppModel m;
  m.cfg = &cfg_;
  m.ed = &ed_;
  m.panels = panels_.data();
  m.panel_count = static_cast<int>(panels_.size());
  m.stdin_buf = &stdin_buf_;
  m.last_compile = last_compile_.get();

  m.focus = focus_;
  m.panel_visible = panel_visible_;
  m.panel_height = panel_height_;

  m.ai_mode = ai_.mode();
  m.ai_state = ai_.state();
  m.ai_state_zh = ai_.stateZh();

  const std::string& path = ed_.buffer().path();
  m.file_name = path.empty() ? std::string("未命名") : util::basename(path);
  // 新文件在存盘前一直挂"(新文件)"后缀:临时状态栏消息会过期,这个不会。
  if (new_file_) m.file_name += "(新文件)";
  m.file_dirty = ed_.buffer().dirty();
  m.cursor_line = ed_.cursor().line + 1;
  m.cursor_col = ed_.cursorDisplayCol() + 1;
  m.lang_zh = langZh(ed_.buffer().lang());
  m.build_summary_zh = build_summary_zh_;
  m.errors = last_compile_ ? last_compile_->errors : 0;
  m.warnings = last_compile_ ? last_compile_->warnings : 0;
  m.running = runner_.busy();
  m.status_msg = status_msg_;

  m.prompt = prompt_;
  m.prompt_label = prompt_label_;
  m.prompt_input = prompt_input_;
  m.prompt_cursor = prompt_cursor_;

  m.help_visible = help_;
  return m;
}

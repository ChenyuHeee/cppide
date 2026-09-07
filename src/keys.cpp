// keys.cpp —— 归一化键码 / 动作枚举 / 全局绑定表的实现(照 architecture.md §8.2、§8.3)
//
// ★ 三条不可动摇的约定,改这个文件前先读完:
//
// 1) 本文件**绝不 include <curses.h>**(纪律见 keys.h 头注释:curses 只允许出现在
//    ui.cpp,且必须先 `#define NCURSES_NOMACROS 1`)。但绑定表里要用到 KEY_LEFT /
//    KEY_F(5) 之类的数值,所以下面把它们**照 ncurses 的稳定 ABI 硬编码**一份。
//    这不是拍脑袋:tests/test_keys.cpp 会真的 include curses.h,对每一个常量做
//    static_assert 逐个比对 —— 一旦哪天数值变了,测试当场红,而不是运行期怪键位。
//
// 2) Alt 的合成不在本文件。keys.h 没有给出合成函数的签名,而合成必须做一次
//    **非阻塞读**(Terminal.app 把 Option 当 Esc 前缀发),那是 ui.cpp 的活。
//    本文件只提供纯粹的 `Alt()/isAlt()/altBase()`(在 keys.h 里已是 constexpr)
//    与查表。ui.cpp 必须实现的规则(逐字抄自 §8.2,也记在
//    .flower/notes/跨模块约定.md):
//        收到 27 -> nodelay 下再 wget_wch 一次:
//          有键 k  -> 返回 Alt(归一化(k))   // k 可能是 KEY_LEFT,故要先归一化再打 Alt 位
//          ERR     -> 返回 27(裸 Esc)
//
// 3) 刻意不绑定的键(§8.2):Ctrl-C(SIGINT)、Ctrl-Z(SIGTSTP)、Ctrl-S/Ctrl-Q
//    (XON/XOFF 冻屏)、Ctrl-\(SIGQUIT+core)、Ctrl-Y(macOS VDSUSP)、
//    Ctrl-J(10)、Ctrl-H(8)。tests/test_keys.cpp 断言它们查不到动作。
//    注意 Ctrl-M(13)/Ctrl-I(9) 这两个键码**必须**有动作 —— 它们就是 Enter/Tab
//    本身,不绑它们等于 Enter/Tab 不能用;"不绑 Ctrl-M/Ctrl-I" 的真实含义是
//    "不把它们当作独立的 Ctrl 快捷键、键名也永远显示 Enter/Tab"。测试对这两个
//    键码断言的是 "动作 == Enter/Tab 且 keyName 不含 'Ctrl-'"。
#include "keys.h"

#include <algorithm>
#include <cstddef>

#include "util.h"

namespace {

// ---------------------------------------------------------------- ncurses 键码镜像
// 值来自 ncurses <curses.h>(自 BSD curses 起就未变过);test_keys.cpp 逐个 static_assert。
constexpr int kNcDown  = 258;   // KEY_DOWN   0402
constexpr int kNcUp    = 259;   // KEY_UP     0403
constexpr int kNcLeft  = 260;   // KEY_LEFT   0404
constexpr int kNcRight = 261;   // KEY_RIGHT  0405
constexpr int kNcHome  = 262;   // KEY_HOME   0406
constexpr int kNcBksp  = 263;   // KEY_BACKSPACE 0407
constexpr int kNcF0    = 264;   // KEY_F0     0410
constexpr int kNcDC    = 330;   // KEY_DC     0512(Delete)
constexpr int kNcIC    = 331;   // KEY_IC     0513(Insert)
constexpr int kNcNPage = 338;   // KEY_NPAGE  0522(PgDn)
constexpr int kNcPPage = 339;   // KEY_PPAGE  0523(PgUp)
constexpr int kNcEnter = 343;   // KEY_ENTER  0527(小键盘 Enter)
constexpr int kNcBTab  = 353;   // KEY_BTAB   0541(Shift-Tab)
constexpr int kNcEnd   = 360;   // KEY_END    0550
constexpr int kNcSEnd  = 386;   // KEY_SEND   0602(Shift-End)
constexpr int kNcSHome = 391;   // KEY_SHOME  0607(Shift-Home)
constexpr int kNcResize = 410;  // KEY_RESIZE 0632(ui.cpp 归一化成 kKeyResize)

constexpr int NcF(int n) { return kNcF0 + n; }   // KEY_F(n)

// ---------------------------------------------------------------- 绑定表
// 排列顺序 = 帮助浮层的展示顺序。同一动作可以有多条(Ctrl 主绑定 + F 键别名),
// 第一条被视为"主绑定"(keyLabel 用它)。
constexpr Binding kBindings[] = {
    // ---- 移动(§8.2 第 1~3 行)----
    {kNcLeft,                Action::MoveLeft,      "\xe2\x86\x90"},        // ←
    {kNcRight,               Action::MoveRight,     "\xe2\x86\x92"},        // →
    {kNcUp,                  Action::MoveUp,        "\xe2\x86\x91"},        // ↑
    {kNcDown,                Action::MoveDown,      "\xe2\x86\x93"},        // ↓
    {Alt(kNcLeft),           Action::MoveWordLeft,  "Alt-\xe2\x86\x90"},    // Alt-←
    {Alt(kNcRight),          Action::MoveWordRight, "Alt-\xe2\x86\x92"},    // Alt-→
    {kNcHome,                Action::MoveHome,      "Home"},
    {kNcEnd,                 Action::MoveEnd,       "End"},
    {kNcPPage,               Action::PageUp,        "PgUp"},
    {kNcNPage,               Action::PageDown,      "PgDn"},
    // BufferStart/BufferEnd:§8.2 的表没给键。Ctrl-Home/Ctrl-End 在各终端上的
    // 转义序列五花八门且 ncurses 多半认不出,所以主绑定用最稳的单字符 Esc 前缀
    // (Alt-< / Alt->,emacs 习惯),再挂 Alt-Home/Alt-End 与 Shift-Home/End 做别名。
    {Alt(Char('<')),         Action::BufferStart,   "Alt-<"},
    {Alt(Char('>')),         Action::BufferEnd,     "Alt->"},
    {Alt(kNcHome),           Action::BufferStart,   "Alt-Home"},
    {Alt(kNcEnd),            Action::BufferEnd,     "Alt-End"},
    {kNcSHome,               Action::BufferStart,   "Shift-Home"},
    {kNcSEnd,                Action::BufferEnd,     "Shift-End"},

    // ---- 编辑(§8.2 第 4~6 行)----
    {13,                     Action::Enter,         "Enter"},
    {kNcEnter,               Action::Enter,         "Enter"},
    {127,                    Action::Backspace,     "Backspace"},
    {kNcBksp,                Action::Backspace,     "Backspace"},
    {kNcDC,                  Action::Delete,        "Delete"},
    {9,                      Action::Tab,           "Tab"},
    {kNcBTab,                Action::ShiftTab,      "Shift-Tab"},
    {Ctrl('U'),              Action::Undo,          "Ctrl-U"},
    {Ctrl('W'),              Action::Redo,          "Ctrl-W"},
    {Ctrl('K'),              Action::CutLine,       "Ctrl-K"},
    {Ctrl('V'),              Action::PasteLine,     "Ctrl-V"},
    {Ctrl('D'),              Action::CopyLine,      "Ctrl-D"},

    // ---- 全局 ----
    {27,                     Action::Escape,        "Esc"},
    {Ctrl('O'),              Action::Save,          "Ctrl-O"},
    {NcF(2),                 Action::Save,          "F2"},
    {Ctrl('X'),              Action::Quit,          "Ctrl-X"},
    {NcF(10),                Action::Quit,          "F10"},
    {Ctrl('B'),              Action::Compile,       "Ctrl-B"},
    {NcF(5),                 Action::Compile,       "F5"},
    {Ctrl('R'),              Action::Run,           "Ctrl-R"},
    {NcF(6),                 Action::Run,           "F6"},
    {Ctrl('E'),              Action::CompileAndRun, "Ctrl-E"},
    {NcF(7),                 Action::CompileAndRun, "F7"},
    {Ctrl('T'),              Action::ToggleAiMode,  "Ctrl-T"},
    {NcF(8),                 Action::ToggleAiMode,  "F8"},
    {Ctrl('A'),              Action::AskAi,         "Ctrl-A"},
    {NcF(4),                 Action::AskAi,         "F4"},   // §8.2:tmux 用户的别名
    {Ctrl('G'),              Action::GotoLine,      "Ctrl-G"},
    {Ctrl('F'),              Action::Find,          "Ctrl-F"},
    {NcF(3),                 Action::FindNext,      "F3"},
    {Ctrl('N'),              Action::NextDiag,      "Ctrl-N"},
    {Ctrl('P'),              Action::PrevDiag,      "Ctrl-P"},
    {Ctrl('L'),              Action::Redraw,        "Ctrl-L"},
    {NcF(1),                 Action::Help,          "F1"},

    // ---- 面板(§8.2 末 3 行 + §8.3)----
    {Alt(Char('1')),         Action::FocusCompile,  "Alt-1"},
    {Alt(Char('2')),         Action::FocusRun,      "Alt-2"},
    {Alt(Char('3')),         Action::FocusAi,       "Alt-3"},
    {Alt(Char('4')),         Action::FocusInput,    "Alt-4"},
    {Alt(Char('0')),         Action::TogglePanel,   "Alt-0"},
    {Alt(Char('=')),         Action::PanelTaller,   "Alt-="},
    {Alt(Char('-')),         Action::PanelShorter,  "Alt--"},
    // NextTab:§8.3 说"面板聚焦时 Tab 切下一个标签"。Tab 的键码(9)已经绑给
    // Action::Tab,所以那条转换由 app.cpp 在"面板有焦点"时做;这里额外给 F9
    // 一个直达绑定,使 Action 枚举里每个动作都至少有一个键(test_keys 会断言)。
    {NcF(9),                 Action::NextTab,       "F9"},
};

constexpr std::size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);

// 归一化键码 -> 固定键名(不含 Ctrl/Alt 前缀的那些特殊键)。查不到返回 nullptr。
const char* specialKeyName(int key) {
  switch (key) {
    case kNcDown:   return "\xe2\x86\x93";   // ↓
    case kNcUp:     return "\xe2\x86\x91";   // ↑
    case kNcLeft:   return "\xe2\x86\x90";   // ←
    case kNcRight:  return "\xe2\x86\x92";   // →
    case kNcHome:   return "Home";
    case kNcEnd:    return "End";
    case kNcBksp:   return "Backspace";
    case kNcDC:     return "Delete";
    case kNcIC:     return "Insert";
    case kNcNPage:  return "PgDn";
    case kNcPPage:  return "PgUp";
    case kNcEnter:  return "Enter";
    case kNcBTab:   return "Shift-Tab";
    case kNcSHome:  return "Shift-Home";
    case kNcSEnd:   return "Shift-End";
    case kNcResize: return "Resize";
    default:        break;
  }
  return nullptr;
}

// 帮助浮层:把键名列按显示宽度补齐(中文占 2 格,所以不能用 size())。
std::string padRight(const std::string& s, int width) {
  int w = util::displayWidth(s, 8, 0);
  std::string out = s;
  for (int i = w; i < width; ++i) out += ' ';
  return out;
}

// 找某个动作的"主绑定"(表里第一条)与"别名"(其余),拼成 "Ctrl-O / F2"。
std::string keyLabel(Action a) {
  std::string out;
  for (std::size_t i = 0; i < kBindingCount; ++i) {
    if (kBindings[i].action != a) continue;
    // 同名的别名(Enter 出现两次、Backspace 出现两次)只显示一次
    bool dup = false;
    for (std::size_t j = 0; j < i; ++j) {
      if (kBindings[j].action == a &&
          std::string(kBindings[j].key_zh) == kBindings[i].key_zh) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    if (!out.empty()) out += " / ";
    out += kBindings[i].key_zh;
  }
  return out;
}

// 键名列的宽度 = 表里所有 keyLabel 的最大**显示宽度** + 2 列间隔。
//
// 为什么要算而不是写死:原来写死 22,但 `Action::BufferStart` 的 label 是
// `Alt-< / Alt-Home / Shift-Home`(显示宽 28),22 列的补齐直接失效,
// `--help` 和 F1 浮层里就粘成 `Alt-< / Alt-Home / Shift-Home跳到文件开头`
// (键名和说明之间一个空格都没有)。写死 30 也能救眼下,但将来往 kBindings 里
// 多加一个别名就又粘上了 —— 所以按实际最长值算,并强制留 2 列间隔。
int keyColWidth() {
  static const int w = []() {
    int m = 0;
    for (std::size_t i = 0; i < kBindingCount; ++i) {
      m = std::max(m, util::displayWidth(keyLabel(kBindings[i].action), 8, 0));
    }
    return m + 2;   // 最长的那一行也保证有 2 个空格把说明隔开
  }();
  return w;
}

void helpRow(std::vector<std::string>& out, Action a) {
  out.push_back("  " + padRight(keyLabel(a), keyColWidth()) + actionZh(a));
}

}  // namespace

// ---------------------------------------------------------------- 表访问
const Binding* bindings(std::size_t& n) {
  n = kBindingCount;
  return kBindings;
}

Action lookupAction(int key) {
  if (key == kKeyNone || key == kKeyResize) return Action::None;
  for (std::size_t i = 0; i < kBindingCount; ++i) {
    if (kBindings[i].key == key) return kBindings[i].action;
  }
  return Action::None;
}

// ---------------------------------------------------------------- 动作名字
const char* actionName(Action a) {
  switch (a) {
    case Action::None:          return "None";
    case Action::MoveLeft:      return "MoveLeft";
    case Action::MoveRight:     return "MoveRight";
    case Action::MoveUp:        return "MoveUp";
    case Action::MoveDown:      return "MoveDown";
    case Action::MoveWordLeft:  return "MoveWordLeft";
    case Action::MoveWordRight: return "MoveWordRight";
    case Action::MoveHome:      return "MoveHome";
    case Action::MoveEnd:       return "MoveEnd";
    case Action::PageUp:        return "PageUp";
    case Action::PageDown:      return "PageDown";
    case Action::BufferStart:   return "BufferStart";
    case Action::BufferEnd:     return "BufferEnd";
    case Action::Enter:         return "Enter";
    case Action::Backspace:     return "Backspace";
    case Action::Delete:        return "Delete";
    case Action::Tab:           return "Tab";
    case Action::ShiftTab:      return "ShiftTab";
    case Action::Undo:          return "Undo";
    case Action::Redo:          return "Redo";
    case Action::CutLine:       return "CutLine";
    case Action::PasteLine:     return "PasteLine";
    case Action::CopyLine:      return "CopyLine";
    case Action::Escape:        return "Escape";
    case Action::Save:          return "Save";
    case Action::Quit:          return "Quit";
    case Action::Compile:       return "Compile";
    case Action::Run:           return "Run";
    case Action::CompileAndRun: return "CompileAndRun";
    case Action::ToggleAiMode:  return "ToggleAiMode";
    case Action::AskAi:         return "AskAi";
    case Action::GotoLine:      return "GotoLine";
    case Action::Find:          return "Find";
    case Action::FindNext:      return "FindNext";
    case Action::NextDiag:      return "NextDiag";
    case Action::PrevDiag:      return "PrevDiag";
    case Action::Redraw:        return "Redraw";
    case Action::Help:          return "Help";
    case Action::FocusCompile:  return "FocusCompile";
    case Action::FocusRun:      return "FocusRun";
    case Action::FocusAi:       return "FocusAi";
    case Action::FocusInput:    return "FocusInput";
    case Action::TogglePanel:   return "TogglePanel";
    case Action::PanelTaller:   return "PanelTaller";
    case Action::PanelShorter:  return "PanelShorter";
    case Action::NextTab:       return "NextTab";
    case Action::Count:         return "Count";
  }
  return "?";
}

const char* actionZh(Action a) {
  switch (a) {
    case Action::None:          return "(无)";
    case Action::MoveLeft:      return "左移一个字符";
    case Action::MoveRight:     return "右移一个字符";
    case Action::MoveUp:        return "上移一行";
    case Action::MoveDown:      return "下移一行";
    case Action::MoveWordLeft:  return "按词左移";
    case Action::MoveWordRight: return "按词右移";
    case Action::MoveHome:      return "行首";
    case Action::MoveEnd:       return "行尾";
    case Action::PageUp:        return "上翻一页";
    case Action::PageDown:      return "下翻一页";
    case Action::BufferStart:   return "跳到文件开头";
    case Action::BufferEnd:     return "跳到文件末尾";
    case Action::Enter:         return "换行(带自动缩进)";
    case Action::Backspace:     return "向左删除";
    case Action::Delete:        return "向右删除";
    case Action::Tab:           return "接受 AI 补全,否则缩进";
    case Action::ShiftTab:      return "反缩进";
    case Action::Undo:          return "撤销";
    case Action::Redo:          return "重做";
    case Action::CutLine:       return "剪切当前行";
    case Action::PasteLine:     return "粘贴行";
    case Action::CopyLine:      return "复制当前行";
    case Action::Escape:        return "依次取消:补全 / 提示行 / 面板焦点";
    case Action::Save:          return "保存文件";
    case Action::Quit:          return "退出(未保存会确认)";
    case Action::Compile:       return "编译";
    case Action::Run:           return "运行(必要时先自动编译)";
    case Action::CompileAndRun: return "编译并运行";
    case Action::ToggleAiMode:  return "切换 AI 模式(练习 / 写代码)";
    case Action::AskAi:         return "立即请求 AI";
    case Action::GotoLine:      return "跳到行号";
    case Action::Find:          return "查找";
    case Action::FindNext:      return "查找下一处";
    case Action::NextDiag:      return "下一个编译诊断";
    case Action::PrevDiag:      return "上一个编译诊断";
    case Action::Redraw:        return "强制重绘";
    case Action::Help:          return "帮助浮层";
    case Action::FocusCompile:  return "聚焦【编译】面板";
    case Action::FocusRun:      return "聚焦【运行】面板";
    case Action::FocusAi:       return "聚焦【AI】面板";
    case Action::FocusInput:    return "聚焦【输入】面板";
    case Action::TogglePanel:   return "折叠 / 展开面板区";
    case Action::PanelTaller:   return "面板加高";
    case Action::PanelShorter:  return "面板减高";
    case Action::NextTab:       return "切换到下一个面板标签";
    case Action::Count:         return "(无)";
  }
  return "(未知动作)";
}

// ---------------------------------------------------------------- 键名
std::string keyName(int key) {
  if (key == kKeyNone) return "(无按键)";
  if (key == kKeyResize) return "终端尺寸变化";
  if (isAlt(key)) {
    int base = altBase(key);
    if (base == 27) return "Esc Esc";
    return "Alt-" + keyName(base);
  }
  if (isChar(key)) {
    uint32_t cp = charOf(key);
    if (cp == ' ') return "空格";
    return util::codepointToUtf8(cp);
  }
  if (const char* s = specialKeyName(key)) return s;
  if (key > kNcF0 && key <= kNcF0 + 24) return "F" + std::to_string(key - kNcF0);
  switch (key) {
    case 0:   return "Ctrl-@";
    case 9:   return "Tab";        // 永远不显示 "Ctrl-I"(§8.2)
    case 13:  return "Enter";      // 永远不显示 "Ctrl-M"
    case 27:  return "Esc";
    case 32:  return "空格";       // 归一化后不该出现,兜底
    case 127: return "Backspace";  // 永远不显示 "Ctrl-?"
    default:  break;
  }
  if (key >= 1 && key <= 26) {
    return std::string("Ctrl-") + static_cast<char>('A' + key - 1);
  }
  if (key >= 28 && key <= 31) {
    static const char kRest[] = {'\\', ']', '^', '_'};
    return std::string("Ctrl-") + kRest[key - 28];
  }
  if (key > 0x100 && key <= 0x1FF) return "功能键#" + std::to_string(key);
  return "键码#" + std::to_string(key);
}

// ---------------------------------------------------------------- 帮助浮层
std::vector<std::string> helpLines() {
  std::vector<std::string> v;
  v.push_back("cppide 快捷键                        F1 / Esc 关闭本浮层");
  v.push_back("");
  v.push_back("── 移动 ──");
  helpRow(v, Action::MoveLeft);
  helpRow(v, Action::MoveRight);
  helpRow(v, Action::MoveUp);
  helpRow(v, Action::MoveDown);
  helpRow(v, Action::MoveWordLeft);
  helpRow(v, Action::MoveWordRight);
  helpRow(v, Action::MoveHome);
  helpRow(v, Action::MoveEnd);
  helpRow(v, Action::PageUp);
  helpRow(v, Action::PageDown);
  helpRow(v, Action::BufferStart);
  helpRow(v, Action::BufferEnd);
  v.push_back("");
  v.push_back("── 编辑 ──");
  helpRow(v, Action::Enter);
  helpRow(v, Action::Backspace);
  helpRow(v, Action::Delete);
  helpRow(v, Action::Tab);
  helpRow(v, Action::ShiftTab);
  helpRow(v, Action::Undo);
  helpRow(v, Action::Redo);
  helpRow(v, Action::CutLine);
  helpRow(v, Action::CopyLine);
  helpRow(v, Action::PasteLine);
  v.push_back("");
  v.push_back("── 文件 / 编译 / 运行 ──");
  helpRow(v, Action::Save);
  helpRow(v, Action::Quit);
  helpRow(v, Action::Compile);
  helpRow(v, Action::Run);
  helpRow(v, Action::CompileAndRun);
  helpRow(v, Action::NextDiag);
  helpRow(v, Action::PrevDiag);
  v.push_back("");
  v.push_back("── AI ──");
  helpRow(v, Action::ToggleAiMode);
  helpRow(v, Action::AskAi);
  v.push_back("");
  v.push_back("── 查找 / 跳转 / 其它 ──");
  helpRow(v, Action::Find);
  helpRow(v, Action::FindNext);
  helpRow(v, Action::GotoLine);
  helpRow(v, Action::Redraw);
  helpRow(v, Action::Help);
  helpRow(v, Action::Escape);
  v.push_back("                        (编译进行中 Esc = 终止编译作业)");
  v.push_back("                        (【运行】面板里 Esc = 终止运行中的程序)");
  v.push_back("");
  v.push_back("── 面板 ──");
  helpRow(v, Action::FocusCompile);
  helpRow(v, Action::FocusRun);
  helpRow(v, Action::FocusAi);
  helpRow(v, Action::FocusInput);
  helpRow(v, Action::TogglePanel);
  helpRow(v, Action::PanelTaller);
  helpRow(v, Action::PanelShorter);
  helpRow(v, Action::NextTab);
  v.push_back("");
  v.push_back("说明:F 键是别名 —— macOS 上 F1~F12 默认被系统媒体键占用,");
  v.push_back("      需在「键盘」设置里勾选\"将 F1、F2 等键用作标准功能键\"。");
  v.push_back("      Alt 组合由 Esc 前缀实现(Terminal.app 把 Option 当 Esc 发)。");
  v.push_back("      Ctrl-C / Ctrl-Z / Ctrl-S / Ctrl-Q / Ctrl-\\ / Ctrl-Y 一律不绑,");
  v.push_back("      它们在终端里有特殊含义(中断 / 挂起 / 流控 / core dump)。");
  return v;
}

// ---------------------------------------------------------------- 文本输入判定
bool isTextInput(int key) {
  if (!isChar(key)) return false;         // 控制字符 / KEY_* / Alt 组合都不是文本
  uint32_t cp = charOf(key);
  if (cp < 0x20 || cp == 0x7F) return false;   // C0 控制字符与 DEL
  if (cp >= 0x80 && cp <= 0x9F) return false;  // C1 控制字符
  if (cp > 0x10FFFF) return false;             // 非法码点
  if (cp >= 0xD800 && cp <= 0xDFFF) return false;  // UTF-16 代理区,不是字符
  return true;
}

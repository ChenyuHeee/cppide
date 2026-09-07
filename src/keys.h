// keys.h —— 归一化键码、动作枚举与绑定表
//
// ★ 本头文件**刻意不 include <curses.h>**。原因:ncurses 定义了一批函数式宏
//   (clear() / erase() / move() / refresh() / timeout() / scroll() / border() …),
//   一旦进入公共头,`buf.erase(r)`、`panel.clear()` 这类成员调用会被预处理器
//   当成宏调用而报错。curses 只允许出现在 ui.cpp 一个文件里,
//   并且建议在那里 `#define NCURSES_NOMACROS` 之后再 include。
//
// 归一化键码空间(Ui::getKeyBlockingFor 的返回值,全工程唯一约定):
//   kKeyNone                 空闲超时(== ncurses 的 ERR)
//   kKeyResize               终端尺寸变化(由 KEY_RESIZE 归一化而来)
//   0 .. 0x1F, 0x7F          控制字符原样(Ctrl-X / Enter=13 / Tab=9 / Backspace=127)
//   0x101 .. 0x1FF           ncurses 的 KEY_*(方向键、F 键等)原样
//   kCharBase + codepoint    可打印字符(含中文);用 isChar()/charOf() 取码点
//   kAltFlag | 上述任一       Alt/Option 组合(Terminal.app 把 Option 当 Esc 前缀发,
//                            Ui 收到 27 后做一次非阻塞 getch 合成)
// 之所以把可打印字符抬到 kCharBase 之上:Unicode 的 U+0101..U+01FF 与 ncurses 的
// KEY_DOWN(0402) 等键码数值重叠,不分开就会把 "ā" 当成方向键。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

constexpr int kKeyNone   = -1;
constexpr int kCharBase  = 0x1000000;          // 可打印字符基址
constexpr int kAltFlag   = 0x2000000;          // Alt 位
constexpr int kKeyResize = 0x4000001;          // 归一化后的 KEY_RESIZE

constexpr int  Ctrl(char c)        { return c & 0x1f; }
constexpr int  Alt(int key)        { return kAltFlag | key; }
constexpr bool isAlt(int key)      { return (key & kAltFlag) != 0; }
constexpr int  altBase(int key)    { return key & ~kAltFlag; }
constexpr int  Char(uint32_t cp)   { return kCharBase + static_cast<int>(cp); }
constexpr bool isChar(int key) {
  return !isAlt(key) && key >= kCharBase && key < kCharBase + 0x110000;
}
constexpr uint32_t charOf(int key) { return static_cast<uint32_t>(key - kCharBase); }

enum class Action {
  None = 0,
  // 移动
  MoveLeft, MoveRight, MoveUp, MoveDown,
  MoveWordLeft, MoveWordRight,
  MoveHome, MoveEnd, PageUp, PageDown, BufferStart, BufferEnd,
  // 编辑
  Enter, Backspace, Delete, Tab, ShiftTab,
  Undo, Redo, CutLine, PasteLine, CopyLine,
  // 全局
  Escape, Save, Quit, Compile, Run, CompileAndRun,
  ToggleAiMode, AskAi,
  GotoLine, Find, FindNext, NextDiag, PrevDiag,
  Redraw, Help,
  // 面板
  FocusCompile, FocusRun, FocusAi, FocusInput,
  TogglePanel, PanelTaller, PanelShorter, NextTab,
  Count
};

const char* actionName(Action a);   // 英文标识(调试 / --doctor)
const char* actionZh(Action a);     // 中文说明(帮助浮层)

struct Binding {
  int key;                // 归一化键码
  Action action;
  const char* key_zh;     // 展示用键名,如 "Ctrl-O"、"Alt-1"、"F5"
};

// 全局绑定表(含 Ctrl 主绑定与 F 键别名)。n 返回条目数。
const Binding* bindings(std::size_t& n);
// 查表。未绑定返回 Action::None。
Action lookupAction(int key);
// 归一化键码 -> 可读名:"Ctrl-O" / "Alt-1" / "F5" / "←" / "Enter" / "a"。
std::string keyName(int key);
// 帮助浮层的中文内容(每元素一行,已排好版)。
std::vector<std::string> helpLines();
// 该键是否应当被当作“往缓冲区插入的可打印字符”。
bool isTextInput(int key);

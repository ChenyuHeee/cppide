// textbuf.h —— 文本缓冲区:行数组 + 逆操作日志(撤销/重做)
//
// 为什么是行数组:单文件竞赛源码不过几千行,插入/删除行的 O(n) memmove
// 完全不可感知;gap buffer / piece table 换不来任何用户可感收益,而行数组
// 让语法高亮、渲染、行号跳转都变直接。
//
// 核心契约:
//   * lines_ 永远至少 1 行(空文件 = 一个空行)。
//   * **唯一两个改动存储的原语是 insert / erase**,二者都记录撤销。
//     §6 的“练习模式绝不插入缓冲区”正是建立在这个咽喉点唯一性之上:
//     AI 文本到达它们的路径只有 Editor::acceptGhost() 一条。
//   * Pos::col 是**行内字节偏移**,不是显示列。显示列换算见 util.h。
//   * 所有只读访问器越界安全(line(i) 越界返回静态空串)。
//   * TextBuffer 不是线程安全的,只在 UI 线程使用;工作线程只拿到它的快照文本。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 缓冲区内的位置:行号(0 起)+ 行内字节偏移。
struct Pos {
  int line = 0;
  int col = 0;
};
bool operator==(const Pos& a, const Pos& b);
bool operator!=(const Pos& a, const Pos& b);
bool operator<(const Pos& a, const Pos& b);
bool operator<=(const Pos& a, const Pos& b);

// 半开区间 [a, b),约定 a <= b(用 normalized() 规范化)。
struct Range {
  Pos a, b;
  bool empty() const { return a == b; }
  Range normalized() const { return (a <= b) ? Range{a, b} : Range{b, a}; }
};

enum class Lang : uint8_t { C, Cpp, Unknown };

// 撤销日志的一条逆操作。isInsert==true 表示“当初插入了 text”,
// 撤销它就是删掉;false 表示“当初删除了 text”,撤销它就是插回去。
struct UndoOp {
  bool isInsert = true;
  Pos at;
  std::string text;
};

// 一次用户动作 = 一个组。所以“接受 ghost 插入 8 行”一次撤销就回退干净。
struct UndoGroup {
  std::vector<UndoOp> ops;
  Pos cursorBefore, cursorAfter;
  int64_t stamp = 0;              // util::nowMs(),用于 800ms 合并窗口
  const char* label = nullptr;    // 静态字符串字面量,不拥有
};

class TextBuffer {
 public:
  TextBuffer();

  // 整体替换内容(清空撤销栈、revision++、dirty 置 false)。lines 为空时补一个空行。
  void reset(std::vector<std::string> lines);
  // 读文件。失败时 err 为中文说明。成功后 path()/lang() 更新,dirty 置 false。
  bool loadFile(const std::string& path, std::string& err);
  // 原子写:同目录 tmp + rename。成功后 clearDirty()。
  bool saveFile(const std::string& path, std::string& err);

  int lineCount() const;
  const std::string& line(int i) const;     // 越界安全,返回静态空行
  int lineLen(int i) const;                 // 字节数;越界返回 0
  const std::vector<std::string>& lines() const;
  std::string text() const;                 // 各行用 '\n' 连接,末尾补一个 '\n'
  std::string textRange(const Range& r) const;
  // 位置工具
  Pos clampPos(Pos p) const;                // 夹到合法范围(并对齐到 UTF-8 字符边界)
  Pos endPos() const;                       // 最后一行行尾
  bool hasNonBlank() const;                 // 是否存在至少一个非空白字符(AI 自动触发条件)

  // ---- 唯一两个改动存储的原语,二者都记录撤销 ----
  Pos insert(Pos at, const std::string& text);  // 处理 '\n' 拆行,返回结束位置
  Pos erase(const Range& r);                    // 返回规范化后的 r.a

  // ---- 撤销分组 ----
  // beginGroup/endGroup 可嵌套(只有最外层真正开/关组)。label 必须是静态字符串。
  void beginGroup(const char* label);
  void endGroup();
  // RAII 包装:一次用户动作写 `TextBuffer::Edit guard(buf, "insert-char");`
  struct Edit {
    Edit(TextBuffer& b, const char* label);
    ~Edit();
    Edit(const Edit&) = delete;
    Edit& operator=(const Edit&) = delete;
   private:
    TextBuffer& b_;
  };
  // 可选:告诉缓冲区“动作前/后的光标在哪”,用于撤销后精确恢复光标。
  // 不调用也能工作(退化为由 ops 位置推断)。Editor 在每次编辑前后各调一次。
  void noteCursorBefore(Pos p);
  void noteCursorAfter(Pos p);
  // 强制关闭当前合并窗口:回车、光标移动、保存、剪切行等结构性操作要调它,
  // 否则连续单字符插入的合并规则会把不相关的动作粘成一组。
  void breakUndoMerge();

  bool undo(Pos& cursorOut);
  bool redo(Pos& cursorOut);
  bool canUndo() const;
  bool canRedo() const;
  void clearHistory();

  bool dirty() const;
  void clearDirty();
  int revision() const;                     // 每次改动 +1 -> 高亮/AI 失效判据
  const std::string& path() const;
  void setPath(std::string p);              // 同时按扩展名重推 lang
  Lang lang() const;
  static Lang langFromPath(const std::string& path);

 private:
  void recordOp(UndoOp op);
  void applyOpNoJournal(const UndoOp& op, Pos& cursorOut);
  Pos rawInsert(Pos at, const std::string& text);
  std::string rawErase(const Range& r);

  std::vector<std::string> lines_;
  std::string path_;
  Lang lang_ = Lang::Unknown;
  bool dirty_ = false;
  int revision_ = 0;

  // 撤销状态
  bool journaling_ = true;                  // 撤销/重做期间挂起记录
  int group_depth_ = 0;
  bool group_open_ = false;                 // 当前是否有一个“可合并的开着的组”
  const char* cur_label_ = nullptr;
  int64_t last_edit_ms_ = 0;
  Pos pending_before_{};
  bool have_pending_before_ = false;
  std::vector<UndoGroup> undo_;
  std::vector<UndoGroup> redo_;
};

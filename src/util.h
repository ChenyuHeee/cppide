// util.h —— 字符串 / 路径 / 文件 / 时间 / UTF-8 与显示宽度工具
//
// 契约:
//   * 本头文件不依赖任何项目内其它头,只用标准库 —— 它是依赖图的叶子。
//   * 所有函数都是纯函数或只做一次系统调用,可在任意线程调用(无全局可变状态)。
//   * 所有“列”概念区分两种:
//       - 字节列(byte col):std::string 内的下标,Pos::col 用的就是它;
//       - 显示列(display col):终端上占的格子数,Tab 展开到制表位、宽字符算 2。
//     两者的换算只允许经由本文件的 byteToDisplayCol / displayColToByte。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace util {

// ---------------------------------------------------------------- 时间
// 单调时钟毫秒(steady_clock),用于所有超时/节流判定。不可用于显示时刻。
int64_t nowMs();
// 墙上时钟毫秒(system_clock),仅用于给面板消息打时间戳。
int64_t nowEpochMs();
// 把墙上时钟毫秒格式化成 "14:03:27"。
std::string formatTimeHMS(int64_t epoch_ms);
// 把毫秒格式化成人类可读:"12ms" / "1.4s"。
std::string formatDuration(int64_t ms);

// ---------------------------------------------------------------- 字符串
std::string trim(const std::string& s);        // 去两端空白
std::string trimLeft(const std::string& s);
std::string trimRight(const std::string& s);
bool startsWith(const std::string& s, const std::string& pre);
bool endsWith(const std::string& s, const std::string& suf);
std::string toLower(std::string s);
// 全是空白(含空串)则为 true。
bool isBlank(const std::string& s);
// 按 '\n' 拆行,并吃掉行尾的 '\r'(兼容 CRLF 文件)。末尾换行不产生空尾行。
std::vector<std::string> splitLines(const std::string& text);
std::vector<std::string> split(const std::string& s, char sep);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
// 截到最多 max_bytes 字节,且不切断 UTF-8 字符;被截断时追加 "…"。
std::string clipBytes(const std::string& s, size_t max_bytes);
// 大小写不敏感查找,找不到返回 std::string::npos。
size_t findNoCase(const std::string& hay, const std::string& needle, size_t from = 0);
// FNV-1a 64 位;用于 Builder::tempBinaryPath 之类的稳定短哈希。
uint64_t hash64(const std::string& s);
std::string toHex(uint64_t v);

// ---------------------------------------------------------------- 路径
std::string basename(const std::string& path);
std::string dirname(const std::string& path);          // 无目录部分时返回 "."
std::string extension(const std::string& path);        // 含点,如 ".cpp";无扩展名返回 ""
std::string stem(const std::string& path);             // basename 去掉扩展名
std::string joinPath(const std::string& a, const std::string& b);
std::string expandUser(const std::string& path);       // 展开开头的 "~/"
std::string tempDir();                                 // $TMPDIR,否则 "/tmp"(结尾不带斜杠)
std::string homeDir();                                 // $HOME,取不到则用 getpwuid

// ---------------------------------------------------------------- 环境 / 文件
std::string envOr(const char* name, const std::string& def);
bool   fileExists(const std::string& path);
bool   isDirectory(const std::string& path);
int64_t fileMtimeMs(const std::string& path);          // 不存在返回 -1
int64_t fileSize(const std::string& path);             // 不存在返回 -1
// 读整个文件。失败时 err 为中文说明并返回 false。
bool readFile(const std::string& path, std::string& out, std::string& err);
// 原子写:先写同目录的 "<name>.cppide.tmp",fsync 后 rename。失败 err 为中文说明。
bool writeFileAtomic(const std::string& path, const std::string& data, std::string& err);
// 递归建目录(mkdir -p 语义)。
bool makeDirs(const std::string& path, std::string& err);

// ---------------------------------------------------------------- UTF-8
// 解析 s[i] 开始的一个 UTF-8 字符:cp 为码点,返回消耗的字节数(>=1)。
// 非法字节序列按 1 字节处理,cp 置为 0xFFFD —— 绝不返回 0,保证调用方循环必然前进。
size_t utf8Decode(const std::string& s, size_t i, uint32_t& cp);
size_t utf8Next(const std::string& s, size_t i);       // 下一个字符起点(不超过 size())
size_t utf8Prev(const std::string& s, size_t i);       // 上一个字符起点(不小于 0)
bool   utf8IsContinuation(unsigned char c);            // (c & 0xC0) == 0x80
std::string codepointToUtf8(uint32_t cp);
std::vector<uint32_t> utf8ToCodepoints(const std::string& s);
size_t utf8Length(const std::string& s);               // 字符数(非字节数)
bool   utf8Valid(const std::string& s);

// 宽字符互转(ncurses 的 waddnwstr / wget_wch 需要 wchar_t)。
std::wstring toWide(const std::string& utf8);
std::string  fromWide(const std::wstring& w);

// ---------------------------------------------------------------- 显示宽度
// 单个码点的显示宽度:wcwidth 包装。控制字符返回 1(我们用 '?' 之类占位绘制),
// wcwidth 返回 -1 的也当 1,永不返回负数 —— 布局计算里出现负宽度是灾难。
int charDisplayWidth(uint32_t cp);
// 整串显示宽度;start_col 是本串起始的显示列(影响 Tab 的制表位对齐)。
int displayWidth(const std::string& s, int tab_width, int start_col = 0);
// 字节列 -> 显示列(byte_col 会被 clamp 到 [0, s.size()])。
int byteToDisplayCol(const std::string& s, int byte_col, int tab_width);
// 显示列 -> 字节列;落在宽字符/Tab 中间时取该字符的起始字节。
int displayColToByte(const std::string& s, int display_col, int tab_width);
// 把 s 折行成每行显示宽度不超过 width 的若干段,返回各段的 [起始字节, 字节长度]。
// 优先在空白处断,单词过长则硬断;绝不切断 UTF-8 字符。面板中文输出靠它。
struct WrapSeg { int start; int len; };
std::vector<WrapSeg> wrapDisplay(const std::string& s, int width, int tab_width);

}  // namespace util

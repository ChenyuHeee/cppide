// util.cpp —— util.h 的实现。
//
// ============================ 调用方必读 ============================
// 1. **locale**:`charDisplayWidth()` 首选 `wcwidth()`,而 `wcwidth()` 只有在
//    进程调用过 `setlocale(LC_ALL, "")`(或 "C.UTF-8")之后才认识非 ASCII 字符。
//    默认的 "C" locale 下 glibc 对所有 >0x7F 的码点一律返回 -1。
//    因此 `main()` 必须在做任何显示宽度计算之前 setlocale —— ncurses 宽字符
//    (`waddnwstr`/`wget_wch`)本来也有同样的要求。
//    作为兜底,本文件内置了一张 East Asian Width / 组合字符表:wcwidth 返回 -1
//    时改查该表,所以即使调用方忘了 setlocale,中文仍然算 2 列、组合符仍算 0 列,
//    布局不会崩(只是覆盖面不如系统表全)。
// 2. **宽度约定**(与 util.h 的注释一致,此处给出完整规则):
//    - 控制字符(cp < 0x20 或 0x7F)一律算 **1** 列:绘制时用 '?' 之类的占位符。
//      注意 '\t' 本身也是控制字符,`charDisplayWidth('\t') == 1`;Tab 的制表位
//      展开是**字符串级**的语义,只在 displayWidth / byteToDisplayCol /
//      displayColToByte / wrapDisplay 里生效。
//    - wcwidth 返回 -1 且内置表也不认识的码点(孤立代理、>0x10FFFF、私用区…)
//      算 **1** 列。永不返回负数 —— 布局计算里出现负宽度是灾难。
//    - 组合字符 / 零宽字符算 **0** 列。
// 3. **非法 UTF-8**:整个文件对任意字节串都安全。`utf8Decode()` 对非法序列
//    固定“吃 1 字节 + 返回 U+FFFD”,永不返回 0,所以所有 `i += utf8Decode(...)`
//    的循环必然前进,不会死循环、不会越界。编辑器会打开二进制文件,这是硬要求。
// 4. **字符边界**:所有对外返回的字节偏移都落在 UTF-8 字符起点上
//    (非法字节自成一个“字符”)。
// 5. **displayColToByte 的取整规则**:显示列落在一个宽字符或一个 Tab 的**中间**时,
//    吸附到**该字符的起始字节**(向左吸附)。列 <0 → 0;列 >= 整行宽度 → s.size()。

#include "util.h"

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>

namespace util {
namespace {

// 显示列的安全上限:极长行也不会让 int 溢出(UBSan 会抓有符号溢出)。
constexpr int kMaxCol = 1 << 28;

inline int clampTabWidth(int tab_width) {
  if (tab_width < 1) return 1;
  if (tab_width > 256) return 256;
  return tab_width;
}

// 从显示列 col 出发,一个 Tab 前进到下一个制表位所需的列数(恒 >= 1)。
inline int tabAdvance(int col, int tab_width) {
  if (col < 0) col = 0;
  return tab_width - (col % tab_width);
}

inline bool isAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

inline char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

struct Range32 { uint32_t lo, hi; };

bool inRanges(uint32_t cp, const Range32* r, size_t n) {
  size_t lo = 0, hi = n;
  while (lo < hi) {  // 二分,表按 lo 升序且互不重叠
    size_t mid = lo + (hi - lo) / 2;
    if (cp < r[mid].lo) hi = mid;
    else if (cp > r[mid].hi) lo = mid + 1;
    else return true;
  }
  return false;
}

// wcwidth 不可用(locale 是 "C")时的兜底:East Asian Wide/Fullwidth。
const Range32 kWideFallback[] = {
    {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},
    {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},
    {0x2693, 0x2693},   {0x26A1, 0x26A1},   {0x26AA, 0x26AB},   {0x26BD, 0x26BE},
    {0x26C4, 0x26C5},   {0x26CE, 0x26CE},   {0x26D4, 0x26D4},   {0x26EA, 0x26EA},
    {0x26F2, 0x26F3},   {0x26F5, 0x26F5},   {0x26FA, 0x26FA},   {0x26FD, 0x26FD},
    {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},   {0x274C, 0x274C},
    {0x274E, 0x274E},   {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},
    {0x27B0, 0x27B0},   {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},   {0x2E80, 0x303E},   {0x3041, 0x33FF},   {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},   {0xA960, 0xA97F},   {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},
    {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4}, {0x17000, 0x18CD5}, {0x1B000, 0x1B152},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
    {0x1F200, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
    {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

// 同上兜底:组合 / 零宽字符(算 0 列)。
const Range32 kZeroFallback[] = {
    {0x0300, 0x036F},   {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x0610, 0x061A},   {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC},
    {0x0711, 0x0711},   {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x07EB, 0x07F3},
    {0x0816, 0x0819},   {0x081B, 0x0823}, {0x0825, 0x0827}, {0x0829, 0x082D},
    {0x0951, 0x0957},   {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x1AB0, 0x1AFF},   {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x2028, 0x202E},
    {0x2060, 0x2064},   {0x206A, 0x206F}, {0x20D0, 0x20F0}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F},   {0xFEFF, 0xFEFF}, {0x1D167, 0x1D169}, {0x1D17B, 0x1D182},
    {0xE0100, 0xE01EF},
};

// 严格 UTF-8 解码。合法则返回长度(1..4)并写 cp;非法返回 0(cp 不动)。
size_t decodeStrict(const unsigned char* p, size_t avail, uint32_t& cp) {
  if (avail == 0) return 0;
  unsigned char b0 = p[0];
  if (b0 < 0x80) { cp = b0; return 1; }
  if (b0 < 0xC2) return 0;                    // 0x80..0xBF 续字节;0xC0/0xC1 超长
  auto cont = [&](size_t k, unsigned char lo, unsigned char hi) -> bool {
    return k < avail && p[k] >= lo && p[k] <= hi;
  };
  if (b0 <= 0xDF) {
    if (!cont(1, 0x80, 0xBF)) return 0;
    cp = (static_cast<uint32_t>(b0 & 0x1F) << 6) | (p[1] & 0x3Fu);
    return 2;
  }
  if (b0 <= 0xEF) {
    unsigned char lo = (b0 == 0xE0) ? 0xA0 : 0x80;   // E0: 防超长
    unsigned char hi = (b0 == 0xED) ? 0x9F : 0xBF;   // ED: 防代理区 D800..DFFF
    if (!cont(1, lo, hi) || !cont(2, 0x80, 0xBF)) return 0;
    cp = (static_cast<uint32_t>(b0 & 0x0F) << 12) |
         (static_cast<uint32_t>(p[1] & 0x3F) << 6) | (p[2] & 0x3Fu);
    return 3;
  }
  if (b0 <= 0xF4) {
    unsigned char lo = (b0 == 0xF0) ? 0x90 : 0x80;   // F0: 防超长
    unsigned char hi = (b0 == 0xF4) ? 0x8F : 0xBF;   // F4: 防 >U+10FFFF
    if (!cont(1, lo, hi) || !cont(2, 0x80, 0xBF) || !cont(3, 0x80, 0xBF)) return 0;
    cp = (static_cast<uint32_t>(b0 & 0x07) << 18) |
         (static_cast<uint32_t>(p[1] & 0x3F) << 12) |
         (static_cast<uint32_t>(p[2] & 0x3F) << 6) | (p[3] & 0x3Fu);
    return 4;
  }
  return 0;                                    // 0xF5..0xFF 一定非法
}

// 一步:解出 [i, i+len) 这个“显示单元”的宽度(Tab 用制表位),并返回字节长度。
// col 是该字符起始的显示列。
size_t stepWidth(const std::string& s, size_t i, int col, int tab_width, int& w) {
  uint32_t cp = 0;
  size_t len = utf8Decode(s, i, cp);
  w = (cp == '\t') ? tabAdvance(col, tab_width) : charDisplayWidth(cp);
  return len;
}

std::string errText(const char* what, const std::string& path) {
  std::string e = what;
  e += ": ";
  e += path;
  e += " (";
  e += std::strerror(errno);
  e += ")";
  return e;
}

}  // namespace

// ---------------------------------------------------------------- 时间

int64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t nowEpochMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string formatTimeHMS(int64_t epoch_ms) {
  time_t secs = static_cast<time_t>(epoch_ms >= 0 ? epoch_ms / 1000
                                                  : (epoch_ms - 999) / 1000);
  struct tm tmv;
  if (!localtime_r(&secs, &tmv)) return "??:??:??";
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return buf;
}

// "12ms" / "1.4s" / "2m03s"。负数按 0 处理。
std::string formatDuration(int64_t ms) {
  if (ms < 0) ms = 0;
  char buf[40];
  if (ms < 1000) {
    std::snprintf(buf, sizeof(buf), "%lldms", static_cast<long long>(ms));
  } else if (ms < 60000) {
    std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(ms) / 1000.0);
  } else {
    long long total = ms / 1000;
    std::snprintf(buf, sizeof(buf), "%lldm%02llds", total / 60, total % 60);
  }
  return buf;
}

// ---------------------------------------------------------------- 字符串

std::string trimLeft(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && isAsciiSpace(s[i])) ++i;
  return s.substr(i);
}

std::string trimRight(const std::string& s) {
  size_t n = s.size();
  while (n > 0 && isAsciiSpace(s[n - 1])) --n;
  return s.substr(0, n);
}

std::string trim(const std::string& s) {
  size_t i = 0, n = s.size();
  while (i < n && isAsciiSpace(s[i])) ++i;
  while (n > i && isAsciiSpace(s[n - 1])) --n;
  return s.substr(i, n - i);
}

bool startsWith(const std::string& s, const std::string& pre) {
  return s.size() >= pre.size() && std::memcmp(s.data(), pre.data(), pre.size()) == 0;
}

bool endsWith(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() &&
         std::memcmp(s.data() + (s.size() - suf.size()), suf.data(), suf.size()) == 0;
}

// 只动 ASCII 的 A-Z:UTF-8 多字节序列的字节绝不会被误改。
std::string toLower(std::string s) {
  for (char& c : s) c = lowerAscii(c);
  return s;
}

bool isBlank(const std::string& s) {
  for (char c : s)
    if (!isAsciiSpace(c)) return false;
  return true;
}

// 行数 = '\n' 个数 + (末尾无 '\n' ? 1 : 0),且**至少 1 行**:
// splitLines("") == {""},splitLines("\n") == {""},splitLines("a\n") == {"a"}。
std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      size_t end = i;
      if (end > start && text[end - 1] == '\r') --end;   // 吃掉 CRLF 的 '\r'
      out.emplace_back(text, start, end - start);
      if (i == text.size()) break;
      start = i + 1;
      if (start == text.size()) break;                   // 末尾换行不产生空尾行
    }
  }
  if (out.empty()) out.emplace_back();
  return out;
}

// 标准 split 语义:结果元素数 == sep 出现次数 + 1(split("", c) == {""})。
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == sep) {
      out.emplace_back(s, start, i - start);
      start = i + 1;
    }
  }
  out.emplace_back(s, start, s.size() - start);
  return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
  std::string out;
  size_t total = 0;
  for (const auto& p : parts) total += p.size() + sep.size();
  out.reserve(total);
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += sep;
    out += parts[i];
  }
  return out;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;                     // 否则会死循环
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  for (;;) {
    size_t p = s.find(from, i);
    if (p == std::string::npos) {
      out.append(s, i, s.size() - i);
      break;
    }
    out.append(s, i, p - i);
    out += to;
    i = p + from.size();
  }
  return out;
}

// 结果(含省略号)总字节数 <= max_bytes。max_bytes < 3 时放不下 "…",
// 就只给不切断字符的前缀(可能是空串)。
std::string clipBytes(const std::string& s, size_t max_bytes) {
  if (s.size() <= max_bytes) return s;
  static const char kEllipsis[] = "\xE2\x80\xA6";      // U+2026,3 字节
  size_t budget = (max_bytes >= 3) ? max_bytes - 3 : max_bytes;
  size_t cut = 0;
  for (size_t i = 0; i < s.size();) {
    size_t nxt = utf8Next(s, i);
    if (nxt > budget) break;
    cut = nxt;
    i = nxt;
  }
  std::string out = s.substr(0, cut);
  if (max_bytes >= 3) out += kEllipsis;
  return out;
}

size_t findNoCase(const std::string& hay, const std::string& needle, size_t from) {
  if (needle.empty()) return from <= hay.size() ? from : std::string::npos;
  if (from > hay.size() || needle.size() > hay.size() - from) return std::string::npos;
  size_t last = hay.size() - needle.size();
  for (size_t i = from; i <= last; ++i) {
    size_t k = 0;
    while (k < needle.size() && lowerAscii(hay[i + k]) == lowerAscii(needle[k])) ++k;
    if (k == needle.size()) return i;
  }
  return std::string::npos;
}

uint64_t hash64(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;                  // FNV-1a 64 offset basis
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;                              // FNV prime
  }
  return h;
}

// 固定 16 位小写十六进制(零填充),便于拼稳定的文件名。
std::string toHex(uint64_t v) {
  static const char* kDigits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = kDigits[v & 0xF];
    v >>= 4;
  }
  return out;
}

// ---------------------------------------------------------------- 路径

namespace {
// 去掉结尾的 '/'(全是 '/' 时保留一个,即根目录)。
std::string stripTrailingSlash(const std::string& p) {
  size_t n = p.size();
  while (n > 1 && p[n - 1] == '/') --n;
  return p.substr(0, n);
}
}  // namespace

std::string basename(const std::string& path) {
  if (path.empty()) return "";
  std::string p = stripTrailingSlash(path);
  if (p == "/") return "/";
  size_t pos = p.find_last_of('/');
  return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

std::string dirname(const std::string& path) {
  if (path.empty()) return ".";
  std::string p = stripTrailingSlash(path);
  if (p == "/") return "/";
  size_t pos = p.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return stripTrailingSlash(p.substr(0, pos));
}

// 含点。"a.tar.gz" -> ".gz";".bashrc" -> ""(点开头的隐藏文件没有扩展名);"a." -> "."。
std::string extension(const std::string& path) {
  std::string b = basename(path);
  size_t pos = b.find_last_of('.');
  if (pos == std::string::npos || pos == 0) return "";
  return b.substr(pos);
}

std::string stem(const std::string& path) {
  std::string b = basename(path);
  std::string e = extension(path);
  return b.substr(0, b.size() - e.size());
}

std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (b[0] == '/') return b;                       // b 是绝对路径
  std::string out = stripTrailingSlash(a);
  if (out.empty() || out.back() != '/') out += '/';
  out += b;
  return out;
}

// 只展开开头的 "~" 与 "~/";"~user" 不支持,原样返回。
std::string expandUser(const std::string& path) {
  if (path.empty() || path[0] != '~') return path;
  if (path.size() == 1) return homeDir();
  if (path[1] != '/') return path;
  std::string home = homeDir();
  if (home.empty()) return path;
  return stripTrailingSlash(home) + path.substr(1);
}

std::string tempDir() {
  const char* t = std::getenv("TMPDIR");
  std::string d = (t && *t) ? t : "/tmp";
  return stripTrailingSlash(d);
}

std::string homeDir() {
  const char* h = std::getenv("HOME");
  if (h && *h) return h;
  struct passwd pw;
  struct passwd* res = nullptr;
  char buf[4096];
  if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &res) == 0 && res && res->pw_dir)
    return res->pw_dir;
  return "";
}

// ---------------------------------------------------------------- 环境 / 文件

std::string envOr(const char* name, const std::string& def) {
  if (!name) return def;
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : def;
}

bool fileExists(const std::string& path) {
  struct stat st;
  return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

bool isDirectory(const std::string& path) {
  struct stat st;
  return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t fileMtimeMs(const std::string& path) {
  struct stat st;
  if (path.empty() || ::stat(path.c_str(), &st) != 0) return -1;
#if defined(__APPLE__)
  return static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000 +
         st.st_mtimespec.tv_nsec / 1000000;
#else
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
}

int64_t fileSize(const std::string& path) {
  struct stat st;
  if (path.empty() || ::stat(path.c_str(), &st) != 0) return -1;
  return static_cast<int64_t>(st.st_size);
}

bool readFile(const std::string& path, std::string& out, std::string& err) {
  out.clear();
  err.clear();
  if (path.empty()) { err = "读取失败:路径为空"; return false; }
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) { err = errText("打开文件失败", path); return false; }
  struct stat st;
  if (::fstat(fd, &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      ::close(fd);
      err = "读取失败:是一个目录: " + path;
      return false;
    }
    if (S_ISREG(st.st_mode) && st.st_size > 0)
      out.reserve(static_cast<size_t>(st.st_size));
  }
  char buf[65536];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) { out.append(buf, static_cast<size_t>(n)); continue; }
    if (n == 0) break;
    if (errno == EINTR) continue;
    err = errText("读取文件失败", path);
    ::close(fd);
    out.clear();
    return false;
  }
  ::close(fd);
  return true;
}

bool writeFileAtomic(const std::string& path, const std::string& data, std::string& err) {
  err.clear();
  if (path.empty()) { err = "写入失败:路径为空"; return false; }
  const std::string dir = dirname(path);
  const std::string tmp = joinPath(dir, basename(path) + ".cppide.tmp");

  mode_t mode = 0644;
  struct stat st;
  if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
    mode = st.st_mode & 07777;                      // 保留原文件权限

  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) { err = errText("创建临时文件失败", tmp); return false; }

  auto fail = [&](const char* what, const std::string& p) {
    err = errText(what, p);
    ::close(fd);
    ::unlink(tmp.c_str());
    return false;
  };

  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n > 0) { off += static_cast<size_t>(n); continue; }
    if (n < 0 && errno == EINTR) continue;
    return fail("写临时文件失败", tmp);
  }
  if (::fsync(fd) != 0) return fail("fsync 临时文件失败", tmp);
  if (::close(fd) != 0) {
    err = errText("关闭临时文件失败", tmp);
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    err = errText("重命名临时文件失败", tmp);
    ::unlink(tmp.c_str());
    return false;
  }
  // 目录项也刷一次:崩溃后 rename 才保证可见。失败不算错(有些文件系统不支持)。
  int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

bool makeDirs(const std::string& path, std::string& err) {
  err.clear();
  if (path.empty()) { err = "建目录失败:路径为空"; return false; }
  std::string cur;
  size_t i = 0;
  if (path[0] == '/') { cur = "/"; i = 1; }
  while (i <= path.size()) {
    size_t sl = path.find('/', i);
    std::string comp = path.substr(i, (sl == std::string::npos ? path.size() : sl) - i);
    if (!comp.empty()) {
      if (cur.empty()) cur = comp;
      else if (cur == "/") cur += comp;
      else { cur += '/'; cur += comp; }
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
        err = errText("建目录失败", cur);
        return false;
      }
    }
    if (sl == std::string::npos) break;
    i = sl + 1;
  }
  if (!isDirectory(path)) {
    errno = ENOTDIR;
    err = errText("建目录失败:已存在同名非目录", path);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- UTF-8

bool utf8IsContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// 非法 -> 吃 1 字节、cp = U+FFFD。i 越界 -> 返回 1、cp = 0(调用方本不该越界调)。
size_t utf8Decode(const std::string& s, size_t i, uint32_t& cp) {
  if (i >= s.size()) { cp = 0; return 1; }
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data()) + i;
  size_t len = decodeStrict(p, s.size() - i, cp);
  if (len == 0) { cp = 0xFFFD; return 1; }
  return len;
}

size_t utf8Next(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  uint32_t cp = 0;
  size_t n = i + utf8Decode(s, i, cp);
  return n > s.size() ? s.size() : n;
}

size_t utf8Prev(const std::string& s, size_t i) {
  if (i > s.size()) i = s.size();
  if (i == 0) return 0;
  size_t j = i - 1;
  while (j > 0 && utf8IsContinuation(static_cast<unsigned char>(s[j])) && i - j < 4) --j;
  uint32_t cp = 0;
  // 只有当 j 处的字符正好覆盖到 i 时,j 才是真正的字符起点;否则说明中间有坏字节。
  if (utf8Decode(s, j, cp) == i - j) return j;
  return i - 1;
}

std::string codepointToUtf8(uint32_t cp) {
  std::string out;
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;   // 非法 -> 替换符
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return out;
}

std::vector<uint32_t> utf8ToCodepoints(const std::string& s) {
  std::vector<uint32_t> out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = 0;
    i += utf8Decode(s, i, cp);
    out.push_back(cp);
  }
  return out;
}

size_t utf8Length(const std::string& s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size(); i = utf8Next(s, i)) ++n;
  return n;
}

bool utf8Valid(const std::string& s) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
  size_t i = 0;
  while (i < s.size()) {
    uint32_t cp = 0;
    size_t len = decodeStrict(p + i, s.size() - i, cp);
    if (len == 0) return false;
    i += len;
  }
  return true;
}

std::wstring toWide(const std::string& utf8) {
  std::wstring out;
  out.reserve(utf8.size());
  for (size_t i = 0; i < utf8.size();) {
    uint32_t cp = 0;
    i += utf8Decode(utf8, i, cp);
    if constexpr (sizeof(wchar_t) >= 4) {
      out += static_cast<wchar_t>(cp);
    } else {                                   // 16 位 wchar_t:拆代理对
      if (cp >= 0x10000) {
        uint32_t v = cp - 0x10000;
        out += static_cast<wchar_t>(0xD800 + (v >> 10));
        out += static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
      } else {
        out += static_cast<wchar_t>(cp);
      }
    }
  }
  return out;
}

std::string fromWide(const std::wstring& w) {
  std::string out;
  out.reserve(w.size());
  for (size_t i = 0; i < w.size(); ++i) {
    uint32_t cp = static_cast<uint32_t>(w[i]);
    if constexpr (sizeof(wchar_t) < 4) {       // 16 位 wchar_t:合并代理对
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
        uint32_t lo = static_cast<uint32_t>(w[i + 1]);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          ++i;
        }
      }
    }
    out += codepointToUtf8(cp);
  }
  return out;
}

// ---------------------------------------------------------------- 显示宽度

int charDisplayWidth(uint32_t cp) {
  if (cp < 0x20 || cp == 0x7F) return 1;       // 控制字符:占位符 1 列('\t' 见文件头注释)
  if (cp < 0x7F) return 1;                     // 纯 ASCII 可打印,不必进 wcwidth
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 1;   // 非法码点:占位 1 列
  int w = ::wcwidth(static_cast<wchar_t>(cp));
  if (w >= 0) return w;
  // wcwidth 说不认识:多半是 locale 还是 "C"。查内置兜底表。
  if (inRanges(cp, kZeroFallback, sizeof(kZeroFallback) / sizeof(kZeroFallback[0]))) return 0;
  if (inRanges(cp, kWideFallback, sizeof(kWideFallback) / sizeof(kWideFallback[0]))) return 2;
  return 1;                                    // 永不返回负数
}

int displayWidth(const std::string& s, int tab_width, int start_col) {
  const int tw = clampTabWidth(tab_width);
  int col = (start_col > 0) ? start_col : 0;
  if (col > kMaxCol) col = kMaxCol;
  const int begin = col;
  for (size_t i = 0; i < s.size();) {
    int w = 0;
    i += stepWidth(s, i, col, tw, w);
    if (col < kMaxCol) col += w;               // 极长行钳位,防止 int 溢出
  }
  return col - begin;
}

// byte_col 落在某个字符中间时(调用方算错了)向左吸附到该字符起点 ——
// 与 displayColToByte 的吸附方向一致,两者互为逆运算。
int byteToDisplayCol(const std::string& s, int byte_col, int tab_width) {
  const int tw = clampTabWidth(tab_width);
  size_t limit = (byte_col <= 0) ? 0
                                 : (static_cast<size_t>(byte_col) > s.size()
                                        ? s.size()
                                        : static_cast<size_t>(byte_col));
  int col = 0;
  for (size_t i = 0; i < limit;) {
    int w = 0;
    size_t len = stepWidth(s, i, col, tw, w);
    if (i + len > limit) break;                // 半个字符不计宽度(向左吸附)
    if (col < kMaxCol) col += w;
    i += len;
  }
  return col;
}

// 落在宽字符 / Tab 中间 -> 吸附到该字符的起始字节(向左)。
int displayColToByte(const std::string& s, int display_col, int tab_width) {
  if (display_col <= 0) return 0;
  const int tw = clampTabWidth(tab_width);
  int col = 0;
  for (size_t i = 0; i < s.size();) {
    int w = 0;
    size_t len = stepWidth(s, i, col, tw, w);
    int next = (col < kMaxCol) ? col + w : col;
    if (display_col < next) return static_cast<int>(i);   // 落在本字符覆盖的列范围内
    col = next;
    i += len;
  }
  return static_cast<int>(s.size());
}

// 空串返回一段 {0,0}(面板里空行也要占一行)。
// '\n' 作硬换行处理:该字节不属于任何一段。
// 在空白处断行时,断点上的空白被吃掉(不属于任何一段)。
// 单个字符宽度就超过 width 时,该段会超宽 —— 这是唯一可能超宽的情形。
std::vector<WrapSeg> wrapDisplay(const std::string& s, int width, int tab_width) {
  std::vector<WrapSeg> out;
  const int tw = clampTabWidth(tab_width);
  const int w_limit = (width < 1) ? 1 : width;
  const size_t n = s.size();
  size_t i = 0;

  for (;;) {
    const size_t seg_start = i;
    int col = 0;
    size_t j = i;
    bool hard_nl = false;
    size_t ws_run_start = std::string::npos;    // 本段内最后一个“内容后的空白run”起点
    bool prev_is_ws = true;                     // 段首的空白不算断点机会

    while (j < n) {
      if (s[j] == '\n') { hard_nl = true; break; }
      const bool is_ws = (s[j] == ' ' || s[j] == '\t');
      if (is_ws && !prev_is_ws) ws_run_start = j;

      int w = 0;
      size_t len = stepWidth(s, j, col, tw, w);
      if (col + w > w_limit && j > seg_start) break;   // 放不下了(至少已收 1 个字符)
      col += w;
      j += len;
      prev_is_ws = is_ws;
    }

    if (j >= n) {                                // 到结尾
      out.push_back(WrapSeg{static_cast<int>(seg_start), static_cast<int>(n - seg_start)});
      break;
    }
    if (hard_nl) {
      out.push_back(WrapSeg{static_cast<int>(seg_start), static_cast<int>(j - seg_start)});
      i = j + 1;
      if (i >= n) break;                         // 末尾换行不产生空尾段
      continue;
    }
    // 需要折行:优先在最后一个空白 run 上断,否则硬断在 j。
    size_t seg_end = j, resume = j;
    if (ws_run_start != std::string::npos && ws_run_start > seg_start) {
      seg_end = ws_run_start;
      resume = ws_run_start;
      while (resume < n && (s[resume] == ' ' || s[resume] == '\t')) ++resume;
      if (resume <= seg_start) { seg_end = j; resume = j; }   // 兜底:保证前进
    }
    out.push_back(WrapSeg{static_cast<int>(seg_start), static_cast<int>(seg_end - seg_start)});
    i = resume;
    if (i >= n) break;
  }
  return out;
}

}  // namespace util

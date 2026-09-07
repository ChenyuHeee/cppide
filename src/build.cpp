// build.cpp —— 编译命令装配与诊断解析(照 architecture.md §4.3)
//
// 本文件里全是纯函数:装配 ProcSpec、解析 stderr、比 mtime。不启动进程、不碰 UI,
// 所以 tests/test_diag.cpp 能脱离终端把它全测一遍。执行由 Runner(proc.h)负责。
//
// 四条最容易写错、也最容易在 code review 里被放过的点:
//
//  1) env_extra 必带 "LC_ALL=C"。中文 locale 下 gcc 会把 "error:" 翻成 "错误:",
//     下面的解析器一条也认不出来 —— 用户会看到"编译失败但 0 个错误"。
//     tests/test_diag.cpp 里有一条中文样本专门证明这个陷阱是真的。
//  2) 参数逐个进 ProcSpec::args,**不经过 shell**;所以路径含空格、含 ':'、
//     含引号都天然安全,不需要任何 quoting。
//  3) 行号解析不能用 "从右往左数冒号"也不能用 <regex>:路径本身可能含 ':'
//     (实测样本 "dir:with space/my file:2.cpp:3:11: error: ...")。
//     做法见 parseOneLine:从左往右找第一个"以 ':' 或空白收口、且其后能找到
//     严重度关键字"的 ":<数字>"。
//  4) 头文件(.h/.hpp/...)不是直接编译它自己,而是编译一个只 #include 它的
//     stdin TU。直接 `g++ -fsyntax-only foo.h` 会无条件报
//     "warning: #pragma once in main file"(该警告无法用任何 -Wno-* 关掉,
//     libcpp 里是 CPP_W_NONE),用户每次检查头文件都会看到一条假警告。
//     走 stdin 既没有假警告,诊断里的文件名/行号也仍然指向真实的头文件。
//     (Wave 1 的 make check-headers 用的是同一招,见 wave1-report §2.1。)
#include "build.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "util.h"

// ---------------------------------------------------------------- 严重度
const char* diagSevZh(DiagSev s) {
  switch (s) {
    case DiagSev::Error:   return "错误";
    case DiagSev::Warning: return "警告";
    case DiagSev::Note:    return "注";
    case DiagSev::Info:    return "信息";
  }
  return "信息";
}

namespace {

bool isDigitCh(char c) { return c >= '0' && c <= '9'; }

std::string cwdPath() {
  char buf[4096];
  if (::getcwd(buf, sizeof buf) != nullptr) return std::string(buf);
  return std::string(".");
}

// 相对路径补成绝对路径(不做 realpath:不需要解析符号链接,也不想碰不存在的文件)。
std::string absPath(const std::string& p) {
  if (p.empty()) return p;
  if (p[0] == '/') return p;
  return util::joinPath(cwdPath(), p);
}

// 临时文件名里只留安全字符,避免奇形怪状的源文件名把 $TMPDIR 弄脏。
std::string sanitizeStem(const std::string& stem) {
  std::string out;
  for (char c : stem) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    out += ok ? c : '_';
    if (out.size() >= 40) break;
  }
  while (!out.empty() && out[0] == '.') out.erase(out.begin());   // 别造隐藏文件
  if (out.empty()) out = "src";
  return out;
}

// ---------------------------------------------------------------- 严重度关键字
struct SevHit {
  std::size_t pos = std::string::npos;   // 关键字起点
  std::size_t len = 0;                   // 关键字长度(不含冒号)
  DiagSev sev = DiagSev::Error;
  bool found = false;
};

// 在 s 里找最左边的严重度关键字。要求:关键字左侧是行首/空白/':',右侧紧跟 ':'。
// "fatal error" 与 "error" 都会命中同一处,取更靠左的那个 => 优先识别 "fatal error"。
SevHit findSeverity(const std::string& s) {
  static const struct {
    const char* kw;
    DiagSev sev;
  } kTable[] = {
      {"fatal error", DiagSev::Error},
      {"internal compiler error", DiagSev::Error},
      {"error", DiagSev::Error},
      {"warning", DiagSev::Warning},
      {"note", DiagSev::Note},
      {"remark", DiagSev::Info},     // clang 的 -Rpass 之类
      {"info", DiagSev::Info},
  };
  SevHit best;
  for (const auto& e : kTable) {
    const std::size_t kwlen = std::strlen(e.kw);
    std::size_t p = 0;
    while ((p = s.find(e.kw, p)) != std::string::npos) {
      const std::size_t after = p + kwlen;
      const bool left_ok = (p == 0) || s[p - 1] == ' ' || s[p - 1] == '\t' ||
                           s[p - 1] == ':';
      const bool right_ok = after < s.size() && s[after] == ':';
      if (left_ok && right_ok) {
        if (!best.found || p < best.pos) {
          best.pos = p;
          best.len = kwlen;
          best.sev = e.sev;
          best.found = true;
        }
        break;                       // 每个关键字只看最左的一处
      }
      p = after;
    }
  }
  return best;
}

// GCC/clang 的源码回显与指示行:
//   "    3 |   int x = ;"     "      |           ^"      "   ^~~~~"
// 这些原样进面板,但不是诊断。必须先挡掉:否则回显的源码里出现 "// error: xxx"
// 就会被当成一条诊断。
bool isEchoOrCaretLine(const std::string& s) {
  if (s.empty()) return true;
  if (s[0] == '|') return true;                  // "| ^~~~"(已左裁空白)
  std::size_t i = 0;
  while (i < s.size() && isDigitCh(s[i])) ++i;
  if (i > 0) {
    std::size_t j = i;
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
    if (j < s.size() && s[j] == '|') return true;  // "3 |   int x = ;"
  }
  bool has_caret = false;
  for (char c : s) {
    if (c == '^' || c == '~') {
      has_caret = true;
    } else if (c != ' ' && c != '\t') {
      return false;
    }
  }
  return has_caret;                              // clang 风格的纯指示行
}

// 解析一行。返回 false = 这行不是诊断(原样进面板即可)。
bool parseOneLine(const std::string& raw, Diag& out) {
  std::size_t b = 0;
  while (b < raw.size() && (raw[b] == ' ' || raw[b] == '\t')) ++b;
  const std::string s = raw.substr(b);
  if (s.empty()) return false;
  if (isEchoOrCaretLine(s)) return false;
  // 包含链:"In file included from a.h:2," / "                 from b.cpp:1:"
  if (util::startsWith(s, "In file included from")) return false;
  if (util::startsWith(s, "from ") &&
      (util::endsWith(s, ":") || util::endsWith(s, ","))) {
    return false;
  }

  // 整行只找一次严重度关键字:候选行号必须出现在它左边。
  // (这同时把 O(候选数 x 行长) 压回 O(行长),"极长垃圾单行"不会退化成平方级。)
  const SevHit hit = findSeverity(s);
  if (!hit.found) return false;      // 没有 error/warning/note => 不是诊断

  int line = 0, col = 0;
  std::size_t file_end = std::string::npos;

  for (std::size_t i = 0; i + 1 < s.size() && i < hit.pos; ++i) {
    if (s[i] != ':' || !isDigitCh(s[i + 1])) continue;
    long ln = 0;
    std::size_t j = i + 1;
    while (j < s.size() && isDigitCh(s[j])) {
      if (ln < 100000000L) ln = ln * 10 + (s[j] - '0');
      ++j;
    }
    // 行号 token 必须以 ':' / 空白 / 行尾 收口。否则 ":1/x" ":2.cpp" 只是路径的一段。
    if (j < s.size() && s[j] != ':' && s[j] != ' ' && s[j] != '\t') continue;
    // 可选列号
    long cl = 0;
    std::size_t k = j;
    if (k + 1 < s.size() && s[k] == ':' && isDigitCh(s[k + 1])) {
      long c2 = 0;
      std::size_t m = k + 1;
      while (m < s.size() && isDigitCh(s[m])) {
        if (c2 < 100000000L) c2 = c2 * 10 + (s[m] - '0');
        ++m;
      }
      if (m >= s.size() || s[m] == ':' || s[m] == ' ' || s[m] == '\t') {
        cl = c2;
        k = m;
      }
    }
    if (k > hit.pos) continue;       // 行号得在严重度关键字之前
    line = static_cast<int>(ln);
    col = static_cast<int>(cl);
    file_end = i;
    break;
  }

  if (file_end != std::string::npos) {
    out.file = s.substr(0, file_end);
    out.line = line;
    out.col = col;
  } else {
    // 没有行号:形如 "g++: fatal error: no input files" / "collect2: error: ..."
    std::string pre = s.substr(0, hit.pos);
    while (!pre.empty() && (pre.back() == ' ' || pre.back() == '\t' ||
                            pre.back() == ':')) {
      pre.pop_back();
    }
    out.file = pre;
    out.line = 0;
    out.col = 0;
  }
  out.sev = hit.sev;
  std::size_t mp = hit.pos + hit.len;
  if (mp < s.size() && s[mp] == ':') ++mp;
  out.message = util::trim(s.substr(mp));
  return true;
}

}  // namespace

// ---------------------------------------------------------------- CompileOutcome
std::string CompileOutcome::summaryZh() const {
  switch (proc.outcome) {
    case ProcOutcome::SpawnFailed:
      return proc.spawn_error.empty() ? std::string("无法启动编译器")
                                      : "无法启动编译器:" + proc.spawn_error;
    case ProcOutcome::Timeout:
      return "编译超时,已终止 · 用时 " + util::formatDuration(proc.wall_ms);
    case ProcOutcome::Killed:
      return "编译被终止 · 用时 " + util::formatDuration(proc.wall_ms);
    case ProcOutcome::Ok:
      break;
  }
  const std::string tail = " · 用时 " + util::formatDuration(proc.wall_ms);
  if (ok) {
    std::string head = syntax_only ? "语法检查通过" : "编译成功";
    if (warnings > 0) head += " · " + std::to_string(warnings) + " 警告";
    return head + tail;
  }
  std::string head;
  if (errors > 0) head = std::to_string(errors) + " 错误";
  if (warnings > 0) {
    if (!head.empty()) head += " ";
    head += std::to_string(warnings) + " 警告";
  }
  if (head.empty()) {
    if (proc.signaled) {
      head = std::string("编译器被信号 ") + signalName(proc.signal) + " 终止";
    } else {
      head = "编译失败(退出码 " + std::to_string(proc.exit_code) + ")";
    }
  }
  return head + tail;
}

// ---------------------------------------------------------------- Builder
Builder::Builder(const Config& cfg) : cfg_(cfg) {}

// 支持哪些扩展名这件事**只有一处判定**:`TextBuffer::langFromPath`。
//
// 为什么不在这里再列一遍:原来这里认 `.h .hpp .hh .hxx .h++ .hp .inc`,而
// `langFromPath` 只认前四个;`main.cpp::checkOpenPath` 又用 `langFromPath` 当
// "能不能打开"的门槛 —— 于是 `.h++ / .hp / .inc` 三个分支是**永远走不到的死代码**。
// 现在改成:先问 `langFromPath` 认不认(唯一清单),再问一个正交的问题
// "它是头文件吗"。C/C++ 的头扩展名全部以 h 开头(.h/.hpp/.hh/.hxx/.h++/.hp),
// 源扩展名全部以 c 开头(.c/.cpp/.cc/.cxx/.C/.c++),所以点后的首字母就够区分,
// 不需要第二份清单。(`.inc` 已被去掉:不是 C/C++ 标准扩展名。)
bool Builder::syntaxOnly(const std::string& src) {
  if (TextBuffer::langFromPath(src) == Lang::Unknown) return false;
  const std::string ext = util::extension(src);   // 含点,如 ".hpp"
  if (ext.size() < 2) return false;
  return ext[1] == 'h' || ext[1] == 'H';
}

ProcSpec Builder::compileSpec(const std::string& src, Lang lang,
                              const std::string& out_path) const {
  ProcSpec s;
  const bool syn = syntaxOnly(src);
  // §4.3:.h/.hpp 一律走 cxx(C 头文件用 C++ 前端做语法检查也是有意义的近似,
  // 而 TextBuffer::langFromPath 本来就把 .h 归到 Lang::Cpp,这里只是兜底)。
  const bool use_c = (lang == Lang::C) && !syn;
  s.prog = use_c ? cfg_.cc : cfg_.cxx;
  const std::vector<std::string>& flags = use_c ? cfg_.cflags : cfg_.cxxflags;
  for (const std::string& f : flags) s.args.push_back(f);

  if (syn) {
    const std::string abs = absPath(src);
    // 见文件头注释 4):编译一个只 #include 它的 stdin TU。
    // 文件名带 '"' 或换行时这招不成立,退回直接编该文件(代价:一条假警告)。
    if (abs.find('"') == std::string::npos &&
        abs.find('\n') == std::string::npos) {
      s.args.push_back("-fsyntax-only");
      s.args.push_back("-x");
      s.args.push_back("c++");
      s.args.push_back("-");
      s.stdin_data = "#include \"" + abs + "\"\n";
    } else {
      s.args.push_back("-fsyntax-only");
      s.args.push_back(src);
    }
  } else {
    if (!out_path.empty()) {
      s.args.push_back("-o");
      s.args.push_back(out_path);
    }
    s.args.push_back(src);
  }

  s.env_extra.push_back("LC_ALL=C");   // ★ 不可删,见文件头注释 1)
  s.timeout_ms = cfg_.compile_timeout_ms;
  s.max_stdout = 1u << 20;
  s.max_stderr = 4u << 20;             // 模板炸开的 stderr 能很大
  s.hard_kill_bytes = 32u << 20;
  return s;
}

ProcSpec Builder::runSpec(const std::string& binary,
                          const std::string& stdin_data) const {
  ProcSpec s;
  // execvp 对不含 '/' 的名字会走 PATH —— 那是绝对不能发生的(会跑到同名系统命令上)。
  s.prog = (binary.find('/') == std::string::npos) ? "./" + binary : binary;
  s.stdin_data = stdin_data;
  s.timeout_ms = cfg_.run_timeout_ms;
  const std::size_t lim = cfg_.run_output_limit;
  s.max_stdout = lim;
  s.max_stderr = lim;
  std::size_t hard = (lim > (static_cast<std::size_t>(-1) / 4))
                         ? static_cast<std::size_t>(-1)
                         : lim * 4;
  if (hard < (8u << 20)) hard = 8u << 20;
  s.hard_kill_bytes = hard;
  // cwd 留空(继承编辑器的工作目录):用户程序里的相对路径应当相对于他敲命令的地方,
  // 而不是 $TMPDIR。
  return s;
}

std::string Builder::tempBinaryPath(const std::string& src) {
  const std::string abs = absPath(src);
  const std::string h = util::toHex(util::hash64(abs));
  return util::tempDir() + "/cppide-" + h + "-" + sanitizeStem(util::stem(src));
}

bool Builder::binaryStale(const std::string& src, const std::string& bin) {
  if (bin.empty()) return true;
  const int64_t bt = util::fileMtimeMs(bin);
  if (bt < 0) return true;                 // 二进制不存在 => stale
  const int64_t st = util::fileMtimeMs(src);
  if (st < 0) return true;                 // 源文件读不到 => 保守当 stale
  return st > bt;
}

std::vector<Diag> Builder::parseDiagnostics(const std::string& stderr_text) {
  std::vector<Diag> out;
  if (stderr_text.empty()) return out;
  // 用 util::splitLines 而不是自己切:面板那一侧也用它,out_line 才对得上。
  const std::vector<std::string> lines = util::splitLines(stderr_text);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    Diag d;
    if (!parseOneLine(lines[i], d)) continue;
    d.out_line = static_cast<int>(i);      // 0 起:stderr 里的行序,调用方可再回填
    out.push_back(std::move(d));
  }
  return out;
}

CompileOutcome Builder::analyze(const ProcResult& pr,
                                const std::string& binary_path,
                                bool syntax_only) {
  CompileOutcome o;
  o.proc = pr;
  o.syntax_only = syntax_only;
  o.binary_path = syntax_only ? std::string() : binary_path;
  o.diags = parseDiagnostics(pr.stderr_text);
  for (const Diag& d : o.diags) {
    if (d.sev == DiagSev::Error) {
      ++o.errors;
    } else if (d.sev == DiagSev::Warning) {
      ++o.warnings;
    }
  }
  // ok 必须同时满足"进程正常退出且退出码 0"和"没有 error 级诊断"。
  // 只看 exit_code 会漏掉 -Werror 之类的场景;只看 diags 会漏掉 spawn 失败。
  o.ok = pr.ok() && o.errors == 0;
  return o;
}

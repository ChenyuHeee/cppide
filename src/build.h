// build.h —— 编译命令装配与诊断解析
//
// 设计:Builder 的方法**全是纯函数**(装配 ProcSpec / 解析 stderr),
// 不启动进程、不碰 UI,因此可以脱离终端单测(tests/test_diag.cpp)。
// 真正的执行交给 Runner(见 proc.h)。
//
// 两条容易漏的关键约定:
//   * 编译子进程的环境必须带 "LC_ALL=C",强制编译器输出英文 error:/warning:,
//     否则中文 locale 下诊断解析全部失效。
//   * 命令**不经过 shell**,参数逐个进 argv,路径含空格天然安全。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#include "proc.h"
#include "textbuf.h"

enum class DiagSev { Error, Warning, Note, Info };
const char* diagSevZh(DiagSev s);   // "错误"/"警告"/"注"/"信息"

struct Diag {
  std::string file;
  int line = 0;                     // 1 起;0 表示没解析出行号
  int col = 0;                      // 1 起;0 表示没有列号
  DiagSev sev = DiagSev::Error;
  std::string message;
  int out_line = 0;                 // 在编译面板中的行号,用于双向定位
};

struct CompileOutcome {
  bool ok = false;
  std::string binary_path;          // syntax_only 时为空
  std::vector<Diag> diags;
  int errors = 0, warnings = 0;
  bool syntax_only = false;         // .h/.hpp:只做语法检查,不产出二进制
  ProcResult proc;
  // 中文一行摘要,进状态栏:"编译成功 · 用时 320ms" / "2 错误 1 警告"
  std::string summaryZh() const;
};

class Builder {
 public:
  explicit Builder(const Config& cfg);

  // ---- 纯函数,可单测,无副作用 ----
  // 装配编译命令。lang==C 用 cfg.cc + cflags,否则 cfg.cxx + cxxflags;
  // 头文件(见 syntaxOnly)追加 -fsyntax-only 且不传 -o。
  // 必带 env_extra {"LC_ALL=C"},timeout_ms = cfg.compile_timeout_ms。
  ProcSpec compileSpec(const std::string& src, Lang lang,
                       const std::string& out_path) const;
  // 装配运行命令:直接 exec 该二进制,stdin_data 原样喂进去,
  // timeout_ms = cfg.run_timeout_ms,输出上限 = cfg.run_output_limit。
  ProcSpec runSpec(const std::string& binary,
                   const std::string& stdin_data) const;

  // $TMPDIR/cppide-<hash>-<stem>,hash 取源文件绝对路径的 util::hash64。
  static std::string tempBinaryPath(const std::string& src);
  // 手写解析器,**不用 <regex>**(编译慢、体积大):
  // 从左往右找第一个 ":<数字>" 作为行号边界(路径本身可能含 ':'),
  // 然后可选的 ":<数字>" 列号,再取 error|warning|note|fatal error 严重度,余下为 message。
  // "In file included from …" 与 "^~~~" 指示行不计入结果(但原样进面板)。
  // out_line 由调用方按面板实际行号回填;本函数按 stderr 的行序填 0 起下标。
  static std::vector<Diag> parseDiagnostics(const std::string& stderr_text);
  // 二进制不存在,或比源文件旧(mtime 比较)则为 true。
  static bool binaryStale(const std::string& src, const std::string& bin);
  // .h/.hpp/.hh/.hxx -> true(只做语法检查)。
  static bool syntaxOnly(const std::string& src);
  // 把 Runner 给回来的 ProcResult 组装成 CompileOutcome:解析诊断、统计
  // 错误/警告数、判定 ok。App 收到 EvKind::CompileDone 后调它。
  static CompileOutcome analyze(const ProcResult& pr,
                                const std::string& binary_path,
                                bool syntax_only);

 private:
  const Config& cfg_;
};

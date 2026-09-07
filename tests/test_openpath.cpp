// test_openpath.cpp —— Wave 5 裁决②:命令行给的路径怎么分类。
//
// 被测对象是 **cppide 可执行文件本身**(main.cpp::checkOpenPath 在匿名 namespace 里,
// 而 main.o 不参与 `make tests` 的链接)。所以这里 fork + execv 真正跑一遍,
// 按退出码与中文提示分类。这也顺手把"启动阶段绝不挂死"一起测了(每次都有超时)。
//
// 退出码约定(main.cpp):
//   2 = 参数/路径被拒(checkOpenPath 失败)
//   3 = 路径检查**通过**了,但环境没有 tty(本测试刻意把三个 fd 都接到管道/devnull)
// 所以 "rc == 3" 就是"这个路径被接受了"的判据,"rc == 2 且 stderr 含某句中文"
// 就是"这个路径被拒了,且理由正确"。
//
// 覆盖的分支:
//   接受:已存在的可读 .c/.cpp/.h/.hpp/.cc/.cxx/.C/.h++/.hp(含大写变体)
//         +  不存在但父目录在(新文件)
//   拒绝:目录 / 扩展名不受支持(含 .inc)/ 父目录不存在 / 命名管道 /
//         存在但读不了(非 root 才测)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static int g_checks = 0;
static int g_cases = 0;
static int g_skipped = 0;

static void failAt(const char* file, int line, const char* expr, const std::string& note) {
  std::fflush(stdout);
  std::fprintf(stderr, "\nFAIL %s:%d  CHECK(%s)  %s\n", file, line, expr, note.c_str());
  std::fflush(stderr);
  std::abort();
}

#define CHECK_M(cond, note)                                           \
  do {                                                                \
    ++g_checks;                                                       \
    if (!(cond)) failAt(__FILE__, __LINE__, #cond, std::string(note)); \
  } while (0)

static void caseBegin(const char* name) {
  ++g_cases;
  std::printf("  [%d] %s\n", g_cases, name);
  std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 找到 cppide 可执行文件
// ---------------------------------------------------------------------------
static std::string findBinary() {
  const char* env = std::getenv("CPPIDE_BIN");
  const char* cands[] = {env, "./cppide", "../cppide", "/work/cppide"};
  for (const char* c : cands) {
    if (c == nullptr || *c == '\0') continue;
    if (::access(c, X_OK) == 0) return c;
  }
  return std::string();
}

struct RunOut {
  int rc = -1;          // 正常退出的退出码;被信号打死时为 -信号号
  bool timed_out = false;
  std::string err;      // stderr
};

// 跑一次 cppide <arg>。三个标准 fd 一律不是 tty(stdin=/dev/null,stdout/stderr=管道),
// 所以路径检查通过时必然停在 "不是交互式终端" 那一步,退出码 3。
// timeout_ms 到点先 SIGKILL:不允许任何路径把启动阶段挂住(FIFO 曾经能做到)。
static RunOut runOnce(const std::string& bin, const std::vector<std::string>& args,
                      int timeout_ms = 10000) {
  RunOut r;
  int ep[2];
  if (::pipe(ep) != 0) {
    r.err = "pipe 失败";
    return r;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(ep[0]);
    ::close(ep[1]);
    r.err = "fork 失败";
    return r;
  }
  if (pid == 0) {
    ::close(ep[0]);
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
    ::dup2(ep[1], STDOUT_FILENO);
    ::dup2(ep[1], STDERR_FILENO);
    ::close(ep[1]);
    // 配置一定要走一个不存在的路径:不许读到跑测试这台机器上的真实 api_key。
    ::setenv("CPPIDE_CONFIG", "/nonexistent/cppide-test-config.json", 1);
    ::unsetenv("CPPIDE_API_KEY");
    ::unsetenv("DEEPSEEK_API_KEY");
    ::setenv("TERM", "dumb", 1);
    std::vector<char*> av;
    std::string b = bin;
    av.push_back(const_cast<char*>(b.c_str()));
    std::vector<std::string> keep = args;
    for (std::string& s : keep) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    ::execv(bin.c_str(), av.data());
    ::_exit(127);
  }
  ::close(ep[1]);
  // 读 stderr 直到 EOF,同时看着超时。
  const int64_t deadline = util::nowMs() + timeout_ms;
  for (;;) {
    char buf[4096];
    // 非阻塞轮询:管道有数据就读,没有就看时间。
    int fl = ::fcntl(ep[0], F_GETFL, 0);
    ::fcntl(ep[0], F_SETFL, fl | O_NONBLOCK);
    const ssize_t n = ::read(ep[0], buf, sizeof(buf));
    if (n > 0) {
      r.err.append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) break;                       // EOF:子进程已关掉两个 fd
    if (util::nowMs() > deadline) {
      r.timed_out = true;
      ::kill(pid, SIGKILL);
      break;
    }
    ::usleep(5000);
  }
  ::close(ep[0]);
  int status = 0;
  // 收尾也要有上限。
  for (int i = 0; i < 400; ++i) {
    const pid_t w = ::waitpid(pid, &status, WNOHANG);
    if (w == pid) break;
    if (w < 0) break;
    if (i == 200) ::kill(pid, SIGKILL);
    ::usleep(5000);
  }
  if (WIFEXITED(status)) {
    r.rc = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    r.rc = -WTERMSIG(status);
  }
  return r;
}

// ---------------------------------------------------------------------------
// 临时工作目录
// ---------------------------------------------------------------------------
static std::string g_dir;

static void setupDir() {
  char tmpl[] = "/tmp/cppide-openpath-XXXXXX";
  const char* d = ::mkdtemp(tmpl);
  if (d == nullptr) {
    std::fprintf(stderr, "mkdtemp 失败\n");
    std::abort();
  }
  g_dir = d;
}

static std::string P(const std::string& name) { return g_dir + "/" + name; }

static void writeF(const std::string& path, const std::string& data) {
  std::string err;
  if (!util::writeFileAtomic(path, data, err)) {
    std::fprintf(stderr, "写 %s 失败:%s\n", path.c_str(), err.c_str());
    std::abort();
  }
}

// ---------------------------------------------------------------------------
// 1. 接受:已存在的 C/C++ 文件(每种受支持的扩展名都过一遍)
// ---------------------------------------------------------------------------
static void case_accept_existing(const std::string& bin) {
  caseBegin("接受:已存在且可读的 C/C++ 文件(全部受支持扩展名)");
  const char* exts[] = {".c", ".cpp", ".cc", ".cxx", ".C", ".c++",
                        ".h", ".hpp", ".hh", ".hxx", ".h++", ".hp",
                        // Wave 5 最后一轮:除 .c/.C 那一对外,扩展名大小写不敏感。
                        // 修之前 main.CPP 会被 langFromPath 判成 Unknown 而拒绝打开。
                        ".CPP", ".Cpp", ".CC", ".CXX", ".C++",
                        ".H", ".HPP", ".Hpp", ".HH", ".HXX", ".HP"};
  for (const char* e : exts) {
    const std::string p = P(std::string("ok") + e);
    writeF(p, "int main(){return 0;}\n");
    const RunOut r = runOnce(bin, {p});
    CHECK_M(!r.timed_out, std::string("打开 ") + e + " 竟然超时了");
    CHECK_M(r.rc == 3, std::string(e) + " 应当被接受(停在无 tty,rc=3),实际 rc=" +
                           std::to_string(r.rc) + " stderr=" + r.err);
    CHECK_M(r.err.find("不是交互式终端") != std::string::npos,
            std::string(e) + " 的 stderr 不是「无 tty」提示:" + r.err);
  }
  std::printf("      %zu 种扩展名全部被接受\n", sizeof(exts) / sizeof(exts[0]));
}

// ---------------------------------------------------------------------------
// 2. 接受:路径不存在但父目录存在 -> 新文件
// ---------------------------------------------------------------------------
static void case_accept_new_file(const std::string& bin) {
  caseBegin("接受:不存在但父目录存在 -> 按新文件打开");
  const char* names[] = {"brand-new.cpp", "new.c", "hdr.h", "x.hpp", "Weird Name.cc"};
  for (const char* n : names) {
    const std::string p = P(n);
    CHECK_M(!util::fileExists(p), std::string(n) + " 应当还不存在");
    const RunOut r = runOnce(bin, {p});
    CHECK_M(!r.timed_out, std::string("新文件 ") + n + " 竟然超时了");
    CHECK_M(r.rc == 3, std::string("新文件 ") + n + " 应当被接受,实际 rc=" +
                           std::to_string(r.rc) + " stderr=" + r.err);
    // 关键:不能再出现"找不到文件"这种拒绝理由。
    CHECK_M(r.err.find("找不到文件") == std::string::npos,
            std::string("新文件 ") + n + " 仍然被当成「找不到文件」拒绝了");
    // 而且**绝不能**在启动检查阶段就把文件创建出来(要等用户按 Ctrl-O)。
    CHECK_M(!util::fileExists(p), std::string(n) + " 还没保存就被创建了");
  }
  // 相对路径也要能当新文件开(dirname 返回 "." 的分支)。
  {
    const RunOut r = runOnce(bin, {"relative-new.cpp"});
    CHECK_M(r.rc == 3, "相对路径的新文件应当被接受,实际 rc=" + std::to_string(r.rc) +
                           " stderr=" + r.err);
    CHECK_M(!util::fileExists("relative-new.cpp"), "相对路径的新文件被提前创建了");
  }
}

// ---------------------------------------------------------------------------
// 3. 拒绝:传入的是目录
// ---------------------------------------------------------------------------
static void case_reject_directory(const std::string& bin) {
  caseBegin("拒绝:传入的是目录");
  // 连"名字看起来像源码"的目录也要拒(先判目录,再判扩展名)。
  for (const char* n : {"subdir", "looks-like.cpp"}) {
    const std::string p = P(n);
    ::mkdir(p.c_str(), 0755);
    const RunOut r = runOnce(bin, {p});
    CHECK_M(!r.timed_out, "打开目录竟然超时了");
    CHECK_M(r.rc == 2, std::string("目录 ") + n + " 应当被拒(rc=2),实际 rc=" +
                           std::to_string(r.rc));
    CHECK_M(r.err.find("是一个目录") != std::string::npos,
            std::string("目录 ") + n + " 的拒绝理由不对:" + r.err);
  }
  // 目录本身也不能被当"新文件"创建。
  const RunOut r2 = runOnce(bin, {g_dir});
  CHECK_M(r2.rc == 2 && r2.err.find("是一个目录") != std::string::npos,
          "临时目录本身应当被拒:" + r2.err);
}

// ---------------------------------------------------------------------------
// 4. 拒绝:扩展名不受支持(必须给出"只支持 C/C++"的中文提示)
// ---------------------------------------------------------------------------
static void case_reject_unsupported_ext(const std::string& bin) {
  caseBegin("拒绝:扩展名不受支持,提示只支持 C/C++");
  // 既有"存在的",也有"不存在的" —— 两条路都不许放过。
  // ★ `e.CPP` 曾经在这张表里(那时 langFromPath 大小写敏感)。Wave 5 最后一轮
  //   已裁决"除 .c/.C 外大小写不敏感",所以 .CPP 现在是**被接受**的
  //   (见 case_accept_existing 的扩展名表)。这里换成 `.inc`:它原来只出现在
  //   `Builder::syntaxOnly` 的第二份清单里、langFromPath 从不认它,
  //   本轮统一两处时按"不扩大需求边界"把它去掉了 —— 它不是 C/C++ 标准扩展名。
  const char* names[] = {"a.py", "b.txt", "c.rs", "d.java", "Makefile", "noext",
                         "e.inc", "e.INC", "g.cp", "h.hs", "f.tar.gz"};
  for (const char* n : names) {
    const std::string p = P(n);
    const RunOut r = runOnce(bin, {p});
    CHECK_M(!r.timed_out, std::string("打开 ") + n + " 竟然超时了");
    CHECK_M(r.rc == 2, std::string(n) + "(不存在)应当被拒,实际 rc=" +
                           std::to_string(r.rc) + " stderr=" + r.err);
    CHECK_M(r.err.find("不支持的文件类型") != std::string::npos,
            std::string(n) + " 缺少「不支持的文件类型」提示:" + r.err);
    CHECK_M(r.err.find("只支持 C / C++") != std::string::npos,
            std::string(n) + " 缺少「只支持 C/C++」的中文说明:" + r.err);
    // 存在的版本也一样拒。
    writeF(p, "whatever\n");
    const RunOut r2 = runOnce(bin, {p});
    CHECK_M(r2.rc == 2 && r2.err.find("不支持的文件类型") != std::string::npos,
            std::string(n) + "(已存在)也应当被拒:" + r2.err);
    ::unlink(p.c_str());
  }
}

// ---------------------------------------------------------------------------
// 5. 拒绝:父目录不存在
// ---------------------------------------------------------------------------
static void case_reject_missing_parent(const std::string& bin) {
  caseBegin("拒绝:父目录不存在");
  const std::string p = P("no/such/dir/x.cpp");
  const RunOut r = runOnce(bin, {p});
  CHECK_M(!r.timed_out, "父目录不存在的路径竟然超时了");
  CHECK_M(r.rc == 2, "父目录不存在应当被拒(rc=2),实际 rc=" + std::to_string(r.rc));
  CHECK_M(r.err.find("目录不存在") != std::string::npos,
          "拒绝理由应当是「目录不存在」:" + r.err);
  // 一层不存在也算。
  const RunOut r2 = runOnce(bin, {P("nodir/y.c")});
  CHECK_M(r2.rc == 2 && r2.err.find("目录不存在") != std::string::npos,
          "单层缺失的父目录也应当被拒:" + r2.err);
  // 父目录"存在但是个普通文件"同样不行。
  writeF(P("afile.cpp"), "x\n");
  const RunOut r3 = runOnce(bin, {P("afile.cpp/z.cpp")});
  CHECK_M(r3.rc == 2, "把普通文件当目录用应当被拒,实际 rc=" + std::to_string(r3.rc));
}

// ---------------------------------------------------------------------------
// 6. 拒绝:文件存在但不是普通文件(命名管道)
//    这条不是洁癖:open(O_RDONLY) 在 FIFO 上会**一直阻塞**,
//    修之前 `cppide fifo.cpp` 会永久挂死在启动阶段(实测 timeout 124)。
// ---------------------------------------------------------------------------
static void case_reject_fifo(const std::string& bin) {
  caseBegin("拒绝:命名管道(否则 open 会永久阻塞)");
  const std::string p = P("pipe.cpp");
  if (::mkfifo(p.c_str(), 0644) != 0) {
    ++g_skipped;
    std::printf("      跳过:mkfifo 失败(%s)\n", std::strerror(errno));
    return;
  }
  const RunOut r = runOnce(bin, {p}, 8000);
  CHECK_M(!r.timed_out, "命名管道让启动阶段挂死了(这正是那个 bug)");
  CHECK_M(r.rc == 2, "命名管道应当被拒(rc=2),实际 rc=" + std::to_string(r.rc));
  CHECK_M(r.err.find("不是普通文件") != std::string::npos,
          "拒绝理由应当是「不是普通文件」:" + r.err);
  ::unlink(p.c_str());
}

// ---------------------------------------------------------------------------
// 7. 拒绝:文件存在但读不了(root 下无法构造,自动跳过)
// ---------------------------------------------------------------------------
static void case_reject_unreadable(const std::string& bin) {
  caseBegin("拒绝:文件存在但不可读");
  if (::geteuid() == 0) {
    ++g_skipped;
    std::printf("      跳过:以 root 运行,chmod 000 也照样读得到\n");
    std::printf("      (已用 setpriv --reuid=65534 手工验证过,见 wave5-polish.md)\n");
    return;
  }
  const std::string p = P("unreadable.cpp");
  writeF(p, "int main(){}\n");
  ::chmod(p.c_str(), 0000);
  const RunOut r = runOnce(bin, {p});
  CHECK_M(!r.timed_out, "不可读的文件竟然超时了");
  CHECK_M(r.rc == 2, "不可读应当被拒(rc=2),实际 rc=" + std::to_string(r.rc));
  CHECK_M(r.err.find("无法读取") != std::string::npos,
          "拒绝理由应当是「无法读取」:" + r.err);
  ::chmod(p.c_str(), 0644);
}

// ---------------------------------------------------------------------------
// 8. 顺手:非交互命令不受路径检查影响(--help / --version 永远是 0)
// ---------------------------------------------------------------------------
static void case_noninteractive_unaffected(const std::string& bin) {
  caseBegin("--help / --version / --print-config 不被路径检查干扰");
  for (const char* f : {"--help", "--version", "--print-config", "--doctor"}) {
    const RunOut r = runOnce(bin, {f});
    CHECK_M(r.rc == 0, std::string(f) + " 应当以 0 退出,实际 " + std::to_string(r.rc));
  }
  // 即使同时给了一个坏路径,--help 也照样先返回 0(参数优先于路径检查)。
  const RunOut r = runOnce(bin, {"--help", P("bad.py")});
  CHECK_M(r.rc == 0, "--help 加坏路径也应当返回 0,实际 " + std::to_string(r.rc));
}

int main() {
  ::alarm(600);
  std::printf("test_openpath:\n");
  const std::string bin = findBinary();
  if (bin.empty()) {
    std::printf("  跳过全部:找不到 cppide 可执行文件"
                "(设 CPPIDE_BIN=<路径>,或先 `make`)\n");
    return 0;
  }
  std::printf("  被测二进制:%s\n", bin.c_str());
  setupDir();
  std::printf("  临时目录:%s\n", g_dir.c_str());

  case_accept_existing(bin);
  case_accept_new_file(bin);
  case_reject_directory(bin);
  case_reject_unsupported_ext(bin);
  case_reject_missing_parent(bin);
  case_reject_fifo(bin);
  case_reject_unreadable(bin);
  case_noninteractive_unaffected(bin);

  std::printf("test_openpath: 全部通过(%d 个用例 / %d 个断言 / 跳过 %d)\n",
              g_cases, g_checks, g_skipped);
  return 0;
}

// proc.h —— 子进程执行:fork + execvp + poll,以及它的后台线程包装
//
// 为什么不用 popen/system:popen 只能单向、拿不到 pid(无法超时 kill)、
// 无法分离 stdout/stderr、拿不到真实退出状态、且经过 /bin/sh(路径含空格就出事)。
// 这四项我们全都需要,所以自己 fork。
//
// ★★ 最重要的契约:runProcess() 是**阻塞式**的,必须在工作线程调用,
//    永远不要在 UI 线程调用。UI 线程调它就等于把“死循环程序挂死编辑器”
//    这个 bug 亲手写回来。UI 线程只允许通过 Runner::submit() 间接使用。
#pragma once

#include <sys/types.h>   // pid_t

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "mailbox.h"

// AppEvent 定义在 ai.h。这里只用指针,故前置声明即可 —— 这样 proc.h 保持
// 对 ai.h 无编译期依赖(proc.cpp 需要 include "ai.h" 才能构造事件)。
struct AppEvent;

struct ProcSpec {
  std::string prog;                       // execvp 走 PATH 解析
  std::vector<std::string> args;          // 不含 prog
  std::string cwd;                        // "" = 继承
  std::vector<std::string> env_extra;     // "K=V";编译时必带 "LC_ALL=C"
  std::string stdin_data;                 // "" -> 立即 EOF
  int    timeout_ms = 5000;               // <=0 表示不限(仅编译用大值)
  size_t max_stdout = 1u << 20;
  size_t max_stderr = 1u << 20;
  size_t hard_kill_bytes = 8u << 20;      // 超此量直接 kill,不等超时
};

enum class ProcOutcome { Ok, Timeout, SpawnFailed, Killed };

struct ProcResult {
  ProcOutcome outcome = ProcOutcome::SpawnFailed;
  int  exit_code = -1;
  bool signaled = false;
  int  signal = 0;
  std::string stdout_text, stderr_text;
  bool stdout_truncated = false, stderr_truncated = false;
  int64_t wall_ms = 0;
  std::string spawn_error;                // execvp 失败的 strerror(中文包装后再给用户)
  // 中文一行摘要,例:"退出码 0 · 用时 12ms" / "超时 5000ms,已终止" /
  // "被信号 SIGSEGV 终止 · 用时 8ms" /(截断时追加 " · 输出已截断")
  std::string summary() const;
  bool ok() const { return outcome == ProcOutcome::Ok && exit_code == 0 && !signaled; }
};

// 信号号 -> 名字("SIGSEGV");未知返回 "SIG?"。
const char* signalName(int sig);
// 把 spec 拼成可展示的一行命令(仅用于面板显示/调试,不用于执行)。
std::string commandLineForDisplay(const ProcSpec& spec);

// 阻塞式执行一个子进程。**必须**在工作线程调用,永远不要在 UI 线程调用。
//
// 实现要点(proc.cpp 必须逐条照做,这是最容易出 bug 的文件):
//  1. 建 4 个管道:stdin/stdout/stderr + **exec 状态管道**(全部 FD_CLOEXEC);
//     子进程 execvp 失败时把 errno 写进状态管道再 _exit(127),父进程读到 EOF
//     即说明 exec 成功 —— 这是 fork 之后唯一可靠的报错通道。
//  2. 子进程:setpgid(0,0) 自成进程组(父进程才能 kill(-pgid) 连带杀掉孙子进程)、
//     dup2 三个管道、关掉其它 fd、把 SIGPIPE/SIGINT/SIGTERM 恢复 SIG_DFL、
//     chdir(cwd)、setenv(env_extra)、execvp。
//  3. 父进程 poll 循环:分块写 stdin 写完立刻 close(EPIPE/POLLERR 是正常情况,不算错误);
//     stdout/stderr 到达上限后**继续排空只置 truncated**(停止读取 = 管道写满 = 子进程
//     永久阻塞 = 看起来挂死);累计超过 hard_kill_bytes 立即 kill。
//  4. 超时:kill(-pgid,SIGTERM) -> 等 200ms -> kill(-pgid,SIGKILL),期间继续排空。
//  5. 两个输出管道都 EOF(或已 SIGKILL)之后才 waitpid。**先 waitpid 再排空 = 经典死锁**。
//  6. 全程不装 SIGCHLD handler(总是 waitpid 指定 pid),避免干扰 ncurses / libcurl。
//
// out_pgid(可为 null):spawn 成功后立即写入子进程组 id,供外部 killInFlight() 使用。
ProcResult runProcess(const ProcSpec& spec, std::atomic<pid_t>* out_pgid = nullptr);

enum class JobKind { Compile, Run };

// 一个工作线程 + 作业队列,作业**串行**执行。
//
// 事件契约(很重要,别搞错分工):
//   Runner 只负责“把进程跑完”,它推出的事件是
//     EvKind::CompileStarted / CompileDone(kind==Compile)
//     EvKind::RunStarted    / RunDone    (kind==Run)
//   事件的 gen 字段 = 本作业的 JobId;`run` 字段 = shared_ptr<ProcResult>。
//   **Runner 不填 AppEvent::compile** —— 诊断解析与 CompileOutcome 的组装由
//   App 收到 CompileDone 后用 Builder::analyze() 完成。这样 proc.cpp 不必依赖 build.h。
class Runner {
 public:
  using JobId = uint64_t;

  explicit Runner(Mailbox<AppEvent>* out);
  ~Runner();                        // 关队列并 join,绝不 detach
  Runner(const Runner&) = delete;
  Runner& operator=(const Runner&) = delete;

  // 入队一个作业,立即返回 JobId(UI 线程调用,永不阻塞)。
  JobId submit(ProcSpec spec, JobKind kind);
  // 读原子 pgid 并 kill(-pgid, SIGKILL)。用户在【运行】面板按 Esc 的逃生口。
  void killInFlight();
  bool busy() const;
  JobId currentJob() const;         // 正在跑的作业 id(空闲时为 0)
  // 丢弃尚未开始的排队作业(不影响在跑的那个)。
  void cancelQueued();

 private:
  struct Job {
    JobId id = 0;
    JobKind kind = JobKind::Run;
    ProcSpec spec;
  };
  void workerLoop();

  Mailbox<AppEvent>* out_ = nullptr;
  Mailbox<Job> jobs_;
  std::thread th_;
  std::atomic<pid_t> cur_pgid_{0};   // 工作线程 spawn 后写入
  std::atomic<bool> busy_{false};
  std::atomic<uint64_t> next_id_{1};
  std::atomic<uint64_t> cur_job_{0};
  std::atomic<bool> stop_{false};
};

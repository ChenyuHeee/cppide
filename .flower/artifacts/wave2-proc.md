# Wave 2 · proc 模块报告(runProcess + Runner)

日期:2026-09-06 · 环境:Debian trixie/aarch64 容器(root),g++ 14.2.0,/bin/sh = dash

## 1. 产出

| 文件 | 行数 | 说明 |
|---|---|---|
| `/work/src/proc.cpp` | 833 | `runProcess()`、`Runner`、`ProcResult::summary()`、`signalName()`、`commandLineForDisplay()` |
| `/work/tests/test_proc.cpp` | 847 | 32 个用例 / 1237 处断言,自带墙钟守卫 |
| `/work/.flower/scripts/wave2-proc-verify.sh` | — | 一键验收:零 warning 门 + 20 轮 -O2 + 20 轮 ASan/UBSan |

**未修改任何 `.h`**(所有头文件 mtime 仍为 Wave 1 的 16:4x),`make check-headers` 仍 OK(15/15)。

## 2. 验收结果

- `g++ -std=c++17 -O2 -Wall -Wextra -c src/proc.cpp` → **零输出**(脚本第 1 步会断言输出为空)。
- `make src/proc.o`(带 `-MMD -MP -Wno-unused-parameter`)→ OK。
- test_proc:**32 用例 / 1237 断言 / 总耗时约 4.3s**(硬性上限 60s,代码里有断言)。
- ASan + UBSan(`detect_leaks=1`, `halt_on_error=1`):通过,连跑 20 轮无失败。
- -O2:连跑 20 轮无失败。
- **TSan 在本机不可用**:`FATAL: ThreadSanitizer: unexpected memory mapping` 对**空 main 程序**同样出现
  (aarch64 VA 布局 + 无法关 ASLR:`/proc/sys/kernel/randomize_va_space` 只读,`setarch -R` 报
  `Operation not permitted`,容器无 CAP_SYS_ADMIN)。替代手段:
  (a) ASan 连跑 20 轮;(b) 新增两条真并发用例(`4 线程并发 runProcess × 25`、
  `Runner submit/kill/析构 竞争 ×40`)把竞争真实撞出来 —— 见 §4 的 bug 1。
- fd 泄漏:`/proc/self/fd` 数量在 500 次 spawn 前后、并发用例前后、整份测试首尾均为 6 → 6。
- 僵尸:多处断言 `waitpid(-1,…,WNOHANG) == -1 && errno == ECHILD`(测试结尾也有一次)。

## 3. 契约逐条落点(§4.1 的 6 条 + §4.2)

| 契约 | 落点 |
|---|---|
| 4 个管道全 FD_CLOEXEC;execvp 失败写 errno + `_exit(127)`,父读到 EOF = 成功 | `makePipe()` / `childFail()` / 读 exec 管道那段 |
| 子进程 `setpgid(0,0)`、dup2×3、关其它 fd、SIGPIPE/INT/TERM 恢复 SIG_DFL、chdir、env、execvp | `childMain()` |
| fork 后只调 async-signal-safe:argv/envp 全在 fork 前固化成 `char* const[]` | `ChildCtx` + `argv_store`/`env_store`;**不用 setenv**,改为在子进程里换 `environ` 指针(macOS 走 `_NSGetEnviron()`) |
| stdin 分块写、写完即 close、EPIPE/POLLERR 视为正常 | poll 循环的 `idx_in` 分支 |
| 到上限继续排空只置 truncated;超 `hard_kill_bytes` 立即 kill | `appendCapped()` + `drainFd()` + `hardKill()` |
| 超时 SIGTERM → 200ms → SIGKILL,期间继续排空 | `escalate()`(poll 循环与 wait 循环共用) |
| 两管道 EOF(或已 SIGKILL)之后才 waitpid | 循环退出后才进 wait 段 |
| 不装 SIGCHLD handler | 全文无 `signal(SIGCHLD, handler)`(子进程里只是恢复 SIG_DFL) |
| poll 超时由截止时刻推算 + EINTR 重试 | `wait_ms` 计算 + `if (errno == EINTR) continue` |
| Runner:单线程 + 队列串行、submit 返回 JobId、killInFlight、析构 close+join 绝不 detach | `workerLoop()` / `~Runner()` |
| 事件:`CompileStarted/CompileDone`、`RunStarted/RunDone`,`gen=JobId`,`run=shared_ptr<ProcResult>`,不填 `compile` | `workerLoop()`;测试逐字段断言 |

## 4. 实现期发现并修掉的两个真 bug(都是"挂死"级)

1. **`~Runner()` 的一次性 `killInFlight()` 会打空。** 工作线程刚 `waitPop` 到作业、还没把 pgid
   写进 `cur_pgid_` 时,析构里那一发 kill 落空,于是那个作业跑满自己的 `timeout_ms`(测试里 30s)
   → 析构挂住。被 `Runner submit/kill/析构 竞争 ×40` 抓到(SIGALRM 打断)。
   修法:Dekker 式握手 —— 工作线程"先立 `busy_` 再读 `stop_`",析构"先置 `stop_` 再读 `busy_`",
   两边都是 seq_cst,至少一方能看到对方;析构循环补刀直到 `busy_` 落下再 join。
2. **多线程同时 fork 会互相漏 fd。** `pipe()` 到 `fcntl(FD_CLOEXEC)` 之间有窗口,另一个线程此刻
   fork 出的子进程会继承我们的管道写端 → 我们永远读不到 EOF = 挂死。macOS 没有 `pipe2()`,
   所以加了 `spawnMutex()` 把"建管道 → fork → 父进程关子端"串起来(poll 循环**不**持锁)。

另外两处主动加固(都有对应用例):
- 循环条件把 `fd_in` 也算进去:`exec 1>&- 2>&-; wc -c` 这类"先关输出再吃 stdin"的程序,
  只看输出端会把它的 stdin 提前截断。**已验证该用例在旧条件下确实失败**(临时回退后复现)。
- 最终等待改成 `waitpid(WNOHANG)` 轮询 + 到期升级 kill:否则"先关 stdout/stderr 再死循环"
  的程序会把 `waitpid` 永久堵住(用例 `先关 stdout/stderr 再死循环` 覆盖)。

## 5. 用例清单(全部实测通过)

01 纯函数(signalName/命令行/摘要)· 02 echo hello 走 PATH · 03 argv 不经过 shell ·
04 退出码非 0 + 输出分离 · 05 `kill -SEGV $$` · 06 空 stdin 立刻 EOF · 07 `timeout_ms<=0` 不限时 ·
08 死循环 → Timeout 且进程组死光 · 09 `trap '' TERM` → 升级 SIGKILL · 10 先关输出再死循环 ·
11 先关输出再吃完 2MB stdin · 12 到上限只截断进程仍正常结束 · 13 `yes` → hard_kill_bytes 生效 ·
14 `head -c 100000000 /dev/zero` 不挂死 · 15 10MB stdin 灌给不读的进程(SIGPIPE 不炸) ·
16 `cat` 回显 10MB 完整逐字节相等 · 17 stdout/stderr 各 400KB 并发不死锁 ·
18 孙子进程随进程组被杀 · 19 execvp 失败 → SpawnFailed(×51 次不泄漏) · 20 cwd 不存在 ·
21 cwd 生效 · 22 env_extra 生效并覆盖同名 · 23 Runner 串行 + 事件顺序 S1D1S2D2S3D3 ·
24 Compile 事件种类 · 25 killInFlight 杀在飞死循环 · 26 cancelQueued · 27 析构时有在飞作业 ·
28 out=nullptr · 29 空闲析构 · 30 4 线程并发 runProcess · 31 submit/kill/析构 竞争 ×40 ·
32 500 次 spawn 无 fd 泄漏无僵尸

测试自身的防挂死:`alarm(90)` + 每个用例的墙钟预算断言 + 所有等待循环都有轮次上限。

## 6. 测试环境相关的一个坑(给后续 agent)

**不能**用 `kill(-pgid, 0) == -1 && errno == ESRCH` 断言"进程组已杀干净":容器里被杀掉的
**孙子进程**变成挂在 PID 1 下的僵尸,僵尸仍算进程组成员,`kill(-pgid,0)` 依旧返回 0。
测试里改为扫 `/proc/*/stat` 数"pgrp == pgid 且状态非 Z/X"的进程数(`groupLiveCount()`)。

## 7. 遗留风险 / 未验证

1. **macOS 上未实测**(本机是 Debian/aarch64)。只用了 POSIX:`pipe/fcntl(F_DUPFD_CLOEXEC)/poll/
   dup2/setpgid/kill/waitpid/execvp/clock_gettime(CLOCK_MONOTONIC)`,`environ` 走 `_NSGetEnviron()`
   分支 —— 这些在 macOS 10.12+ 全部可用,但**编译与行为都需要在 macOS 上再跑一次 test_proc**。
2. `strerror()` 严格来说非线程安全(glibc/macOS 对已知 errno 返回静态常量串,实际安全);
   没用 `strerror_r`,因为 GNU/XSI 两种签名会让跨平台代码更脏。
3. 子进程关 fd 的循环上界把 `sysconf(_SC_OPEN_MAX)` 钳到 4096(本机 rlimit 是 1048576)。
   若父进程真的持有 fd > 4096 且该 fd 非 CLOEXEC,它会漏进子进程。`spawnMutex()` 已消除我们
   自己造成的这种漏,但别的模块若开了 4096+ 个 fd 仍有理论风险。
4. pid 回收:`killInFlight()` 读原子 pgid 后 kill,`runProcess` 在 `waitpid` 成功后立刻把它清 0,
   窗口只有几条指令(且期间子进程是僵尸,pid 不会被复用)。理论上不可能完全归零。
5. `~Runner()` 会 SIGKILL 在飞作业(否则退出会卡满 `timeout_ms`)。这是我加的语义,
   文档只写了"close 队列并 join"。**若协调者认为退出时应等作业跑完,请告知**。
6. `summary()` 的截断后缀采用 `proc.h` 注释里的 `" · 输出已截断"`(任务书里写的是
   `(输出已截断)`,两者冲突,以冻结的头文件注释为准);`SpawnFailed` 的摘要是
   `"无法启动:<spawn_error>"`,文档没给样例,如需改文案只动 `ProcResult::summary()` 即可。
7. `Runner::busy()` 返回 `busy_ || !jobs_.empty()`(排队中也算忙),文档未明确,按 UI 语义选的。

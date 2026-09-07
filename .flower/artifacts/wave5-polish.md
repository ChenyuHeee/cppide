# Wave 5 —— 裁决落地 · 打磨 · 敌意路径实测

> 本文只记**真跑出来的证据**。每一项都给命令、数字、以及"为什么这个断言方式是对的"。
> 一把重跑:`.flower/scripts/wave5-verify.sh`(可传子集名,见脚本头注释)。
> ★ 全程未使用任何真实 API key:AI 场景一律用假 key + 本地假 401 服务器 / 不可路由地址,
>   每个被测子进程都显式清空 `CPPIDE_API_KEY` / `DEEPSEEK_API_KEY`。

环境:Debian aarch64,g++ 14.2.0,ncurses 6.5.20250216,TERM=xterm-256color(pty)。

**一把干净重跑的结论**(`rm -rf /tmp/wave5 && .flower/scripts/wave5-verify.sh`,约 12 分钟):

```
==================== Wave 5 汇总:失败 0 条 ====================
35 条顶层断言全 PASS / 0 FAIL
```
覆盖:make 零 warning、check-headers、make tests(16 个测试)、
任务 A①②③⑤⑤b⑥、任务 B①②③④⑤⑥⑦⑧、任务 C、方向键回归、ASan/UBSan 四轮。

---

## 0. 改动清单

| 文件 | 改了什么 |
|---|---|
| `src/app.cpp` | A① `onAiDone` 改为明确的替换语义;A③ Esc 终止编译作业;A② 新文件分支;`new_file_` 相关状态 |
| `src/app.h` | private 区新增 `bool new_file_`;更新 `ai_stream_` / `onAiDone` 的注释契约 |
| `src/main.cpp` | A② `checkOpenPath` 重写(新文件 / 目录 / 扩展名 / 父目录 / 非普通文件 / 不可读);`--help` 文案 |
| `src/keys.cpp` | 帮助浮层补"编译进行中 Esc = 终止编译作业" |
| `src/ui.cpp` | 新增 `registerCsiCursorKeys()`(修方向键插字母的真 bug);`drawHelp` 重写为分页 + 条件分栏 |
| `src/ui.h` | private 区新增 `help_page_` / `help_was_visible_` |
| `tests/test_appai.cpp` | 新增:钉住 A① 的替换语义(行为断言 + 函数体 grep 断言) |
| `tests/test_openpath.cpp` | 新增:钉住 A② 的接受/拒绝六条分支(fork+execv 真跑 `./cppide`) |
| `tests/test_proc.cpp` | 那个 `kill -SEGV $$` 的用例改成 `cwd=/tmp` + `ulimit -c 0`(见 §7) |
| `tests/test_editor.cpp` | 那个"fork 出去等 assert abort"的子进程加 `setrlimit(RLIMIT_CORE, 0)`(见 §7) |
| `.flower/notes/跨模块约定.md` | 追加 §19(Ctrl-J→Enter)、§20(必须是普通文件)、§21(AiDone 替换语义) |

公开签名一处没动;`make check-headers` 通过(15 个头文件)。

---

## 1. 任务 A:6 项裁决

### A① `AiDone` = 替换语义

`ai.cpp:528` 推出的 `AiDone.text` 就是完整最终文本(写代码模式已过 `postprocessCompletion`,
练习模式已追加【推理过程】)。`app.cpp` 原先写的是

```cpp
const std::string& full = ev.text.size() >= ai_stream_.size() ? ev.text : ai_stream_;
```

**为什么"取较长者"比不兼容更坏**:写代码模式的后处理**会让最终文本比 delta 原文短**
(去掉模型重复的行前缀、截到 `ghost_max_lines`),取较长者等于把后处理丢掉,
ghost 里直接出现重复代码。现在是:

```cpp
const std::string& full = ev.text;      // src/app.cpp:onAiDone
```

并且 `onAiDelta` 明确只累加 `ai_stream_`、**永不落地**;`PanelOnly` 分支改成
`endStreaming(); append(full); endStreaming();`,整轮回复只往面板落地**一次**。

**测试**:`tests/test_appai.cpp`,5 个用例 / 145 个断言,全过。
它把 App 构造出来但**不调 `App::init()`** —— `Ui::Ui()` 是 `= default`,curses 只在
`Ui::init()` 里碰,`Ui::~Ui()` 在没 init 过时只做幂等的 `endwinOnce()`(内部
`g_screen_live == 0`),所以在没有 tty 的 CI 里也能直接调事件处理函数、检查
`ed_.ghost()` 与 `panels_` 的**真实状态**。

钉住的具体断言:

| 断言 | 说明 |
|---|---|
| Delta 序列切成 1/2/3/7/40/100 段都不影响结果 | 含把 UTF-8 字符切两半的切点 |
| `AiDone` **之前** ghost 为空、面板一行没多 | delta 不落地 |
| `AiDone` 之后 `ghost().text == final`,`countOccurrences(...) == 1` | 不重复、不截断 |
| 面板里最终文本的**每一行**都恰好出现 1 次 | 中文 + 多行 + 空行样本 |
| `Done.text` 比 delta 累积**短**时仍以 Done 为准 | 就是"取较长者"会挂的那一条 |
| 连续两轮互不污染,ghost 是替换而不是拼接 | `ai_stream_` 收尾即清空 |
| grep `onAiDelta` 函数体:不许出现 `setGhost` / `appendGhostDelta` / `appendStreaming` / `appendAi` | 行为断言可被"改回兜底"绕过,grep 断言不能 |
| grep `onAiDone` 函数体:`ai_stream_` 只允许出现在 `clear()` 上 | 同上 |

(grep 断言先 `stripLineComments` —— 本工程注释里大量出现这些词来解释"为什么不这么做"。)

### A② 不存在的文件按新文件打开

`main.cpp::checkOpenPath` 现在的判定顺序与理由:

1. `isDirectory` → 拒(即使名字叫 `looks-like.cpp`);
2. `TextBuffer::langFromPath(p) == Lang::Unknown` → 拒,提示
   `不支持的文件类型:X` + `cppide 只支持 C / C++:.c / .cpp / .cc / .cxx / .C / .c++ / .h / .hpp / .hh / .hxx`;
3. 不存在 → 父目录必须 `isDirectory`,否则拒(`目录不存在:<dir>(无法在其中新建 X)`);
   通过则**当新文件**;
4. 存在但 **不是 `S_ISREG`** → 拒(`不是普通文件(命名管道 / 设备 / 套接字)`);
5. `util::readFile` 失败 → 拒(`无法读取 X:...`)。

第 4 条是**这次新发现的挂死 bug**:

```
$ mkfifo /tmp/a2/fifo.cpp && timeout 8 ./cppide /tmp/a2/fifo.cpp
rc=124                      # 修之前:永久挂死在启动阶段,连 endwin 安全网都跑不到
$ timeout 8 ./cppide /tmp/a2/fifo.cpp
cppide: /tmp/a2/fifo.cpp 不是普通文件(命名管道 / 设备 / 套接字),无法当源码打开。   # 修之后
```

原因:FIFO 上的 `open(O_RDONLY)` 会一直阻塞到有人写它(`util::readFile:488`)。
已记进跨模块约定 §20。

App 侧:`App::init` 里路径不存在就走 `ed_.buffer().setPath(open_path_)`(不 `loadFile`、
不置脏),`new_file_ = true`。可见性做了两层:
* 临时状态栏消息 `新文件 X · Ctrl-O 保存即创建`(8s);
* `AppModel::file_name` 后挂 `(新文件)`,**存盘前一直挂着**(临时消息会过期,这个不会)。
存盘成功(`doSave` 与 SaveAs 两条路)后 `new_file_ = false`,提示改成 `已新建并保存 X`。

**测试**:`tests/test_openpath.cpp`,8 个用例 / 117 个断言 / 跳过 1,已进 `make tests`。
它 fork+execv 真跑 `./cppide`,三个标准 fd 都接管道/`/dev/null`,于是
`rc == 3`(无 tty)= 路径被接受、`rc == 2` + 特定中文 = 被拒且理由正确;每次都有超时,
"启动阶段挂死"会直接判失败。跳过的 1 项是"文件存在但读不了" —— root 下 `chmod 000`
照样读得到,已单独用 `setpriv --reuid=65534` 验证:

```
cppide: 无法读取 /tmp/a2/unread.cpp:打开文件失败: /tmp/a2/unread.cpp (Permission denied)
```

端到端(pty,`wave5-a2-newfile.txt`,全部通过):打开不存在的 `brand_new.cpp` →
状态栏出现 `新文件` 与 `(新文件)` → 打进一段 C++ → `Ctrl-O` → `已新建并保存` +
`(新文件)` 标记消失(用 `expectnotfrom` 断言,见 §4 的坑)→ `Ctrl-E` 编译并运行 →
面板出现 `退出码` → 文件真的落在磁盘上(90 字节)。

### A③ 编译面板也给 Esc 终止

`App::doEscape` 里现在是:

```
ghost -> 提示行 -> [编译在飞:任何焦点下 Esc 都终止] -> [运行在飞且焦点在【运行】面板:终止]
      -> 面板取消焦点 -> 取消 AI 请求
```

**编译与运行的作用域刻意不同**:`doRun` 会主动聚焦【运行】面板,所以用户按完 Ctrl-R
一定按得到那个 Esc;而编译卡住时(模板爆炸、编译器本身挂住)用户根本没机会去聚焦面板,
只靠 `compile_timeout_ms`(默认 30s)兜底体验太差。

终止时 `compile_job_ = 0`:随后必然到达的那个 `CompileDone`(被 SIGKILL 的编译器)
会在 `onCompileDone` 的 `ev.gen != compile_job_` 处被丢掉 —— 否则面板上会再冒出一条
误导人的"编译失败,N 个错误"。面板里留一行 `(编译已被用户按 Esc 终止)`,
状态栏 `已终止编译作业`,`build_summary_zh_ = "编译已终止"`。

**实测**(`wave5-a3-compile-esc.txt`,样本是 `-O2` 下要编 ~25.6s 的 6 万行文件):

```
PASS  expectlast "编译中"
PASS  expect "已终止编译作业"
PASS  expectnot "编译失败"                       # 过期作业真的被丢掉了
PASS  expectlast "编译已被用户按 Esc 终止"
PASS  expectlast "AFTER_ESC_OK"                  # 终止后编辑器照常可用
PASS  expect "编译中" / PASS expect "已终止编译作业"   # 第二次也行,作业槽已放开
== 全部通过 ==(失败 0 条)   + 无编译器孤儿进程
```

`--help` 与 F1 帮助浮层都补了这条说明。

### A④ `Ctrl-J(10) → Enter` 已写进跨模块约定

`.flower/notes/跨模块约定.md` 新增 §19,含:termios 的 `ICRNL`、ncurses `raw()`
不清 `ICRNL`、`nonl()` 只影响输出这三层原因;"折叠只能发生在归一化层,不许改成给
`keys.cpp` 里的 10 绑动作"(那会违反 §18 并弄红 `test_keys.cpp`);
以及看门人 `tests/test_ui.cpp:115` 的 `CHECK_EQ_I(normalizeKey(OK, 10), 13)`。
顺带把 §20(必须是普通文件)、§21(AiDone 替换语义)也补了。

### A⑤ 退出期挂住:定论 = **代码不挂**;两份报告的冲突已解释

harness:`.flower/scripts/wave5-exit-loop.cpp`(forkpty,每轮独立进程,超时先抓
`/proc/<pid>/wchan` + 每个线程的 `task/*/wchan` 再 SIGKILL)。

**100 轮 `启动 -> Ctrl-X -> y -> 退出`(每轮预算 10s):**

```
Ctrl-X -> 进程真正退出的耗时:min=141ms  p50=173ms  p95=248ms  max=267ms  平均=188.4ms
卡住(超过 10000ms 没退)的轮数:0 / 100
非 0 退出码 / 被信号打死的轮数:0 / 100
```

**更凶的一轮:退出时还有在飞的 AI 请求**(Ctrl-T 切写代码模式 + 打字触发自动补全,
`base_url = http://10.255.255.1:9` 不可路由、`ai_connect_timeout_ms = 30000`),40 轮:

```
min=508ms  p50=752ms  p95=829ms  max=847ms  平均=729.6ms
卡住的轮数:0 / 40
```

即:`AiService` 的 `cancelInFlight` + join 会在 1s 内收掉一个还在 30s 连接超时里的 curl 传输,
`~Runner()` 的 SIGKILL + join 也没有问题。**`wave4-ui.md` 报的"卡在 `request_wait_answer`(FUSE)"
不是代码问题**:那是 /work 挂在 FUSE 上、`endwin` 之后的 `atexit`/析构路径里写盘慢,
与 `Ui::shutdown` 之后的任何 join 都无关(本轮 100+40 次全部在 /tmp 上跑,零卡住;
被 SIGKILL 的那些"卡住"样本一律显示 `wchan=do_poll` = 在等键盘,不是在等 futex)。

**顺带纠正 harness 自己的一个假阳性**(值得记下来):第一版把
"PREKEYS 打了字 → 缓冲区变脏 → Ctrl-X 弹确认框 → 没人按 y" 误判成卡死,
20/40 报 STUCK。现场证据是 `wchan=do_poll` + `Threads: 3`,一看就是在等按键。
现在 harness 无条件补一发 `y`。**教训:判"卡死"必须看 wchan,别只看"没退出"。**

### A⑥ `AppModel` 不加 `active_tab`;Tab 轮换实测**可用**

`ui.cpp::activePanel` 的启发式(聚焦面板则画该面板;聚焦编辑区则画第一个有未读的,
否则【编译】)在 pty 里实测能用。判别方式**不能靠颜色**(pty 抓到的纯文本里颜色属性
已被滤掉,`[编译*]` 里的 `*` 是**未读**标记不是焦点标记),所以改用**面板内容**当判据 ——
先给四个面板各塞一段独一无二的文本:

```
mark tab0 -> Alt-1 -> PASS expect "$ c++"              # 【编译】
mark tab1 -> Tab   -> PASS expect "HELLO_A6"           # 【运行】(程序输出)
mark tab2 -> Tab   -> PASS expect "Ctrl-T 切模式"       # 【AI】(启动横幅)
mark tab3 -> Tab   -> PASS expect "STDIN_MARKER_42"    # 【输入】
mark tab4 -> Tab   -> PASS expect "$ c++"              # 绕回【编译】
+ 再连按 9 次 Tab:不崩、光标合法、UTF-8 干净
== 全部通过 ==(失败 0 条)
```

**结论:不需要协调者再裁决,启发式够用。**

---

## 2. 任务 B:敌意路径与退化场景(9 项)

### B① 80x24 / 60x16 布局 + 猛拉缩放 400 次 —— 通过

`wave5-b1-resize.txt`。断言方式:`cursorin <行> <列>` 解析原始流里**最后一个** CUP
(`ESC[r;cH`)必须落在当前尺寸内;`nocorrupt` 断言原始流里**每一个**非转义字节都构成
合法 UTF-8(把宽字符切成两半是"花屏"最常见的成因)。

```
80x24 首帧:cursorin (1,15) OK / nocorrupt OK / 标签栏【编译】【运行】都在
resize 16x60(§8.1 的最小可用尺寸):cursorin (2,10) OK / nocorrupt OK
resizestorm 200 次 5x20 <-> 60x200(pace=0,尽可能快):耗时 5ms,58KB 重绘输出
    -> cursorin (2,10) 落在 60x200 内 / nocorrupt(59848 字节)OK
resizestorm 200 次 5x20 <-> 60x200(pace=12ms,每帧都真的画完):耗时 2908ms
    -> cursorin OK / nocorrupt(234345 字节)OK
风暴后打字 xyzzy_after_storm 立刻可见;切回 24x80 一切正常
峰值 RSS = 11392KB
== 全部通过 ==(失败 0 条)
```

两种口径都测了:`pace=0` 走 ncurses 合并 `KEY_RESIZE` 的路径,`pace=12ms` 走逐次重排的路径。

### B② 死循环程序 —— 两条路径都通过

**Esc 路径**(`wave5-b2-esc-kill.txt`,`run_timeout_ms=60000` 让超时不抢戏):

```
Ctrl-E -> expectlast "运行中"
Alt-3 -> PASS "AI"                       # 死循环期间按键仍然有反应
Esc(从【AI】面板)+ 打字 -> PASS "ALIVE_WHILE_SPINNING"   # 编辑照常
Alt-2 + Esc -> PASS "已终止运行中的程序"
终止后 Ctrl-L / cursorin 全部正常;无孤儿进程
```

⚠️ 脚本上的坑:**取消面板焦点的那一下 Esc 必须在【AI】面板里按**,在【运行】面板里按
会直接把子进程杀掉(§4.2 第三道防线),就测不到"打字"了。

**超时兜底路径**(`wave5-b2b-timeout.txt`,`run_timeout_ms=3000`,全程不按任何键):

```
PASS expectlast "超时"        # proc.cpp: kill(-pgid,SIGTERM) -> 200ms -> SIGKILL
PASS expectlast "AFTER_TIMEOUT_OK"   # 兜底之后编辑器照常
```

### B③ 100MB 输出 —— 通过,峰值 RSS 见下

三种口径都跑了,因为默认配置根本到不了"100MB 进编辑器":

| 场景 | `run_output_limit` | 结果 | 当前 RSS | **峰值 RSS(VmHWM)** |
|---|---|---|---|---|
| 启动基线 | — | — | 11264 KB | 11264 KB |
| B3a 100MB 输出 / 默认 | 1 MB | 11ms 内撞 `hard_kill_bytes`(= max(8MB, 4×limit))被 SIGKILL,面板显示 `已终止 · 信号 SIGKILL · 用时 11ms · 输出已截断` | 13156 KB | **14184 KB** |
| B3b 1.5MB 输出 | 1 MB | 程序正常跑完,面板显示**退出码** + `截断` | 13216 KB | 14164 KB |
| B3c 100MB 输出 / 放开上限 | **100 MB** | 整整 100MB 真的灌进编辑器,正常结束,退出码可见 | 114512 KB | **216916 KB(212 MiB)** |

结论:编辑器全程不挂死(B3c 之后 `bench 30` 单帧 0ms)、面板正确裁剪、
峰值内存 ≈ 2.1× 数据量(临时双份缓冲),100MB 场景峰值 212 MiB —— 不爆。
默认配置下 `hard_kill` 会在 11ms 内保护住编辑器,这是 §4.2 的设计取舍;
**代价**是超过 8MB 输出的程序看不到退出码(已在 B3a 的脚本注释里写明)。

### B④ 无网络 / 错 key / 无配置 —— 通过

`wave5-b4-ai-states.sh`,四个场景(无配置文件 / 有文件但 key 为空 / 错 key / 无网络)。
"错 key"用**本地起的假 HTTP 服务器**回 401 + DeepSeek 风格错误体(不碰真实 API)。

| 场景 | 中文提示(屏幕实拍) | 编辑 | 编译+运行 | 输入单帧耗时 |
|---|---|---|---|---|
| 无配置文件 | `AI 未配置:请在 <path> 填写 api_key(编辑、编译、运行不受影响)` | ✅ | ✅ `退出码` + `AI_RUN_42` | p50 0ms / max 1ms |
| key 为空 | 同上 | ✅ | ✅ | p50 0ms / max 1ms |
| 错 key(401) | `AI 错误:API key 无效或已过期:Authentication Fails, Your api key is invalid` | ✅ | ✅ | p50 0ms / max 1ms |
| 无网络(不可路由) | `AI 错误:请求超时` | ✅ | ✅ | p50 0ms / max 1ms |

另外每个场景都断言 **屏幕上不出现 api_key 原文**(`scrubKey` 生效)。

⚠️ 两个断言坑(都踩过,已写进脚本注释):
* 往缓冲区插的必须是**注释**。第一版插了 `int main() {`,于是文件里有两个 main,
  编译必然失败,"编译+运行照常"根本测不到。
* 断言"程序真跑过了"必须用**只有运行期才会出现**的字符串。第一版断言源码里就有的
  `AI_STATE_PROBE_RAN`,而那串字在编辑区里本来就显示着 —— 假通过。现在程序用
  `printf("%s_%d", "AI_RUN", 42)` 拼出 `AI_RUN_42`,源码里不存在这个字面量。

### B⑤ 5 万行大文件 —— 通过,单帧耗时见下

样本:50006 行 / 3.46 MB,混合中文注释 / 字符串 / 预处理指令(给高亮器压力)。

```
打开之后 RSS = 20116KB(峰值 20116KB)        # 3.46MB 源码 -> 20MB 驻留
bench 120 次 ↓        : min=0 p50=0 p95=1 max=1 平均=0.1ms
bench 60 次 PgDn      : min=0 p50=0 p95=1 max=1 平均=0.1ms
Alt-> 跳到文件尾      : PASS 50006:  (状态栏行:列)
Ctrl-G 25000 Enter    : PASS 25000
bench 20 次 Enter(在第 25000 行连按回车):min=6 p50=7 p95=8 max=8 平均=7.0ms
bench 60 次 ↑         : 平均 0.1ms      # 连按 20 次回车之后没有退化
bench 30 次 PgUp      : 平均 0.1ms
Ctrl-O 保存 5 万行    : PASS 已保存,RSS 峰值 23444KB
== 全部通过 ==(失败 0 条)
```

补测的"最贵一帧"(`Ctrl-L` 强制全量重画,高亮缓存整个丢掉):

| 操作 | 5 万行文件 · 光标在第 1 行 | 5 万行文件 · 光标在第 25000 行 | 空文件对照 |
|---|---|---|---|
| `Ctrl-L` 全量重画 ×30 | p50 **0ms** / max 1ms | p50 **7ms** / max 7ms | p50 0ms |
| PgDn ×60 | — | p50 0ms / max 1ms | p50 0ms |
| 单字符插入 ×60 | — | p50 0ms / max 1ms | p50 0ms |
| Enter ×30 | — | p50 **7ms** / max 8ms | p50 0ms |

`bench` 的定义:发键 -> 直到 pty 输出**静默 30ms** 为止的毫秒数(即"这一帧画完了")。
最差 8ms ≈ 125fps,手感可用。第 25000 行的 7ms 应该是 `forceFullRedraw` 丢掉
`hl_cache_` 后重新确定块注释状态要从文件头扫 —— 只在按 Ctrl-L / 回车时出现,不影响滚动。

### B⑥ 二进制 / 含非法 UTF-8 的文件 —— 通过

样本 A:105 字节的手工非法 UTF-8(裸 `\xff\xfe\x80\x81`、截断的 `\xc3`/`\xe4\xb8`/`\xf0\x9f`、
UTF-16 代理区 `\xed\xa0\x80`、overlong `\xc0\xaf`/`\xe0\x80\xaf`、裸 NUL 与 `\x1b[31m`)。
样本 B:`cppide` 自己的前 400000 字节(真二进制,含大量 NUL 与随机高位字节)。

```
样本 A:打开 OK / nocorrupt(全部 5 次采样)OK / 状态栏在 / 滚动 90 次 OK
        跳尾 + 跳首 OK / 60 次缩放风暴 OK / 插字符 OK / 正常退出(退出码 0)
        RSS 11392KB
样本 B:同上全部通过,RSS 12120KB,原始流 189291 字节全是合法 UTF-8
两份都 == 全部通过 ==(失败 0 条)
```

关键:**编辑器往终端写出去的字节全程都是合法 UTF-8**(`nocorrupt`),即脏字节没有被
原样吐给终端 —— 这就是"不花屏"的可验证含义。

### B⑦ 只读文件保存失败 / 目录不可写 —— 通过

root 能写任何文件,所以整段以 `setpriv --reuid=65534`(nobody)跑。

```
场景:目录 555 + 文件 444(nobody 既不能写文件也不能在目录里建 tmp)
PASS expectlast "B7_UNSAVED_EDIT"        # 先改脏
PASS expect     "保存失败"
     实拍:保存失败:无法写入目录:/tmp/w5b7/ro(Permission denied)
PASS expectlast "B7_UNSAVED_EDIT"        # ★ 不丢数据:刚打的那行还在
PASS expectlast "*"                      # ★ 仍然标记为脏
PASS expect     "保存失败"(再试一次,同样报错不崩)
PASS expectlast "STILL_ALIVE"            # 编辑器仍然可用
PASS expectlast "未保存"                  # Ctrl-X 走确认分支
磁盘检查:只读源文件内容一字未改;目录里没有 .cppide.tmp 残骸
== 全部通过 ==(失败 0 条)
```

**顺带记一条行为**(不是 bug,但值得知道):文件 444 而**目录可写**时保存会**成功** ——
`util::writeFileAtomic` 是"写 tmp + rename",rename 只看目录权限。这与 vim 的
`:w!` 行为一致,而且原文件权限(444)被保留。已实测:`已保存 ro2.cpp`,文件内容更新,
权限仍是 `-r--r--r--`。

### B⑧ `make debug`(ASan/UBSan)下重跑 B1 / B2 / B4 —— 通过,零报告

构建方式(不用 `make debug`:它会 `clean all`,把 release 的 `.o` 冲掉,
而 `make tests` 正需要那些 `.o`。所以复制到 `/tmp/dbgsrc` 用同一组 flag 单独构建):

```sh
rm -rf /tmp/dbgsrc && cp -r /work/src /tmp/dbgsrc && rm -f /tmp/dbgsrc/*.o
(cd /tmp/dbgsrc && for f in *.cpp; do g++ -I. -std=c++17 -O0 -g \
   -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -c -o ${f%.cpp}.o $f; done)
g++ -fsanitize=address,undefined -o /tmp/cppide-asan /tmp/dbgsrc/*.o -lncursesw -lcurl -lpthread
```

编译零 warning。跑法:`ASAN_OPTIONS=detect_leaks=1:log_path=...` + `UBSAN_OPTIONS=print_stacktrace=1`。

| 用例 | 结果 | 峰值 RSS | ASan/UBSan 报告 |
|---|---|---|---|
| B1 缩放风暴(400 次) | 全部通过,退出码 0 | 35200 KB | **无** |
| B2a 死循环 + Esc 终止 | 全部通过,退出码 0 | 33536 KB | **无** |
| B2b 超时兜底 | 全部通过,退出码 0 | — | **无** |
| B4 四种 AI 异常态 | 失败 0 条 | — | **无** |

一个 `log_path` 文件都没生成,且所有进程退出码为 0(ASan 检出泄漏会以 1 退出)⇒
**无内存错误、无 UB、无泄漏**。

### B⑨ `make tests` + `make check-headers` 最终复跑

```
make check-headers: OK (15 个头文件)
make tests: rc=0 —— 16 个测试全过
  test_ai(15 cases/319)  test_aihttp(20/198)  test_appai(5/145)★新增
  test_config(417)  test_diag(24/539)  test_editor(430)  test_highlight(12/355631)
  test_json(21/3081)  test_keys(10/2418)  test_openpath(8/117/跳过1)★新增
  test_panel(17/106123)  test_proc  test_sse(22/90)  test_textbuf  test_ui(6/833467)
  test_util(267024)
make(release):零 warning
```

---

## 3. 任务 C:`Ctrl-F` 查找 与 `F1` 帮助浮层

先 grep 确认现状:两者**都已实现**(`app.cpp:289` Find / `app.cpp:1078` 提交 /
`editor.cpp:600` `Editor::find` 带回绕 / `app.cpp:1016` 无诊断时 `Ctrl-N/P` 退化为
查找下一处/上一处 / `ui.cpp` 的 `drawHelp` + `keys.cpp::helpLines`)。
于是任务 C 主要是**实测 + 补一个真实缺口**。

**实测**(`wave5-c-find-help.txt`,样本第 5/40/120 行有 `NEEDLE_TARGET`,33 条断言全过):

```
Ctrl-F -> PASS "查找"(中文标签)
输入 needle + Enter -> PASS "找到" / PASS 光标 "5:5"
Ctrl-N -> "40:5" -> Ctrl-N -> "120:5" -> Ctrl-N -> "5:5"(自动回绕一圈)
Ctrl-P -> "120:5"(反向)
查一个不存在的串 -> PASS "未找到"
Ctrl-F 后按 Esc -> 提示行取消且**光标没动**("120:5" 不变)
F1 -> PASS "cppide 帮助" / 页码 / 页脚
连翻各页 -> PASS Ctrl-O / Ctrl-B / Ctrl-T / Alt-1 / Ctrl-C / 终止编译作业 全部出现过
F1 再按 -> 关闭,状态栏 "F1帮助" 回来、编辑区内容重新可见
F1 -> Esc 关闭 OK;60x16 下打开/关闭不崩、光标合法;任意键关闭且**不改缓冲区**
== 全部通过 ==(失败 0 条)
```

**补齐的缺口:帮助浮层在 80x24 上只能显示 1/4 的内容。**
`helpLines()` 有 71 行(最宽 76 显示列),80x24 的浮层内部只有 19 行,原实现直接
截断并写一句"…(终端太矮,帮助未显示完)" —— 也就是说标准终端上用户**永远看不到**
`Ctrl-O` / `Alt-1` 这些键。现在:

1. **分页**:底部写 `第 X/Y 页 · 再按一下 F1 看下一页 · 完整列表见 cppide --help`,
   标题栏也带页码。80x24 = 4 页。
   翻页状态放在 `Ui` 的 private 成员(`help_page_` / `help_was_visible_`)——
   `AppModel` 已被协调者裁决冻结,App 没有通道把页码告诉渲染层;于是复用现有的
   "帮助浮层任意键关闭"行为:**关掉时页码 +1**,所以"连按两下 F1"就是下一页。
2. **条件分栏**:只有"每栏都放得下最宽那行"时才分栏(`inner_w / (content_w + 2)`,上限 3 栏)。
   为什么这么保守 —— 第一版按"每栏至少 24 列"硬分,80 列下分 2 栏,
   `(编译进行中 Esc = 终止编译作业)` 这种带缩进的续行被整条截没,pty 断言直接抓到了。
   现在 80x24 保持单栏 4 页,≥160 列的宽终端才会真用上 2 栏。

---

## 4. 这一轮修掉的两个真 bug(都不在任务清单里,是实测撞出来的)

### (1) 方向键在非 DECCKM 模式下会往缓冲区插字母

**症状**:pty 里发 `ESC [ B`,编辑区里多一个字母 `B`(光标 `1:1 -> 1:4`),
发 `ESC O B` 才是下移。

**原因链**:`keypad(stdscr, TRUE)` 输出 smkx(`\E[?1h\E=`)把终端切到 application
cursor keys 模式,此时终端发 `ESC O A/B/C/D`;而 `xterm-256color` 的 terminfo
**只登记了这一套**(`kcuu1=\EOA`)。一旦终端不理 smkx(某些 tmux/screen 配置、
精简终端、把方向键塞进宏或粘贴),它发的是 `ESC [ A`,ncurses 认不出 -> 返回裸 27 ->
落进 `ui.cpp` 的 Alt 合成 -> 得到 `Alt-'['`,紧跟的 `A` 被当普通字符**插进缓冲区**。

**修法**:`ui.cpp::registerCsiCursorKeys()`(在 `Ui::init` 里调),用 ncurses 的
`define_key` 把 `ESC[A/B/C/D/H/F`、`ESC[1~`、`ESC[4~` 与 `ESC O A/B/C/D/H/F`
两套都登记上。`define_key` 是 ncurses 扩展,用 `#if defined(NCURSES_VERSION)` 兜住。

**回归脚本**:`.flower/scripts/wave5-keyprobe-cursor.txt`
```
\e[B ×3 -> PASS 4:1        (修之前是 1:4)
\eOB ×2 -> PASS 6:1
\e[6~   -> PASS 17:1       \e[5~ -> PASS 6:1
PASS expectnot "keys.cpp*"  # 方向键一个字母都没插进去,缓冲区没变脏
```

### (2) 命名管道让启动阶段永久挂死

见 §1 A②。`mkfifo x.cpp && cppide x.cpp` 修前 `timeout` 报 124。
已修 + 已进跨模块约定 §20 + `tests/test_openpath.cpp::case_reject_fifo`(带 8s 超时)。

---

## 5. pty 驱动器的增强(可复用)

`.flower/scripts/wave5-pty-drive.cpp`(基于 wave4 版),新增脚本命令:

| 命令 | 用途 |
|---|---|
| `resize <行>x<列>` | `ioctl(TIOCSWINSZ)` + 补一发 SIGWINCH |
| `resizestorm <n> <RxC> <RxC> [pace_ms]` | 两尺寸间来回切 n 次;不给 pace = 尽可能快 |
| `cursorin <行> <列>` | 断言原始流里最后一个 CUP 落在该尺寸内 |
| `nocorrupt` | 断言原始流里所有非转义字节构成合法 UTF-8(花屏检测) |
| `rss <标签>` | 打印子进程当前 RSS 与峰值 `VmHWM` |
| `bench <n> <转义串>` | 重复 n 次"发键 -> 等输出静默 30ms",报单帧耗时分位数 |
| `expectlast <文本>` | 只在最近 200KB 里找(wave4 文档里写了但没实现) |
| `expectnotfrom <文本>` | 只在最后一个 mark 之后断言"没有出现"(判提示是否消失) |
| `sendfile <路径>` / `repeat <n> <转义串>` | 灌大输入 / 连按同一个键 |

### 写 pty 断言必须知道的五个坑(全部踩过)

1. **`mark` 要放在"发按键"之前**。它记的是此刻已收到的字节数;放在 `wait` 之后
   会把要断言的那一屏刷新圈到 mark 前面去,断言必然失败。
2. **断言整串文本前先 `send \x0c`(Ctrl-L)**。ncurses 是增量重绘,逐字打进去的
   `abc` 在过滤后的屏幕文本里是 `a<空格>b<空格>c`(每个字符后面跟着转义序列),
   整串永远匹配不上。Ctrl-L 强制全量重画之后才是连续的。
3. **`expect` / `expectfrom` 的参数是原样子串**,`\s` `\xNN` 只有 `send` 会解释。
4. **横向滚动会把行内容切掉**。光标在第 54 列时编辑区已右滚,屏幕上只剩行尾片段 ——
   断言"跳到了哪一行"用状态栏的 `行:列` 比用行内容可靠。
5. **帮助浮层开着时不一定发新的 CUP**(`ui.cpp` 走 `curs_set(0)` + `wmove(0,0)`),
   `cursorin` 必须在关掉浮层之后测;而且**不能靠"再按一次 F1"数开关奇偶** ——
   一次 write 里连发两个 `ESC O P` 可能只算一次按键(实测 12 次 F1 之后浮层还是开着的)。
   用 Ctrl-L 收尾最稳:开着时它当"任意键"关掉浮层,关着时只是重绘。

---

## 6. 未验证 / 风险 / 留给下一轮

1. **macOS 一次都没跑过**。本轮全部在 Debian aarch64 + ncursesw 6.5 上。
   与 macOS 相关的具体不确定项:`define_key` 在 Apple 自带 ncurses(5.7 系)上存在但
   行为未验证;Terminal.app 的 `kf1` 可能是 `\E[11~` 而不是 `\EOP`(F1 打不开帮助的话
   用 `Ctrl-L` 之外的键位或看 `--doctor`);`brew --prefix ncurses` 分支未走过。
2. **默认配置下输出 > 8MB 的程序看不到退出码**(`hard_kill_bytes = max(8MB, 4×run_output_limit)`
   会先 SIGKILL 它)。这是 §4.2 的防挂死设计,但用户可能困惑。
   缓解办法是把 `run_output_limit` 调大;是否要在面板里加一句"如需完整输出请调大
   run_output_limit"由协调者定。
3. **帮助浮层的翻页方式偏怪**:必须"连按两下 F1"才到下一页(因为 `AppModel` 冻结,
   没有通道传页码)。正规做法是让浮层吃掉 ↑/↓/PgDn 并把 scroll 传给 Ui —— 那需要
   给 `AppModel` 加一个 `int help_scroll` 或给 `ui.h` 加一个公开方法。**要不要做请协调者裁决。**
4. **`Ctrl-N` 的双重语义**:有编译诊断时是"下一个诊断",没有时才是"查找下一处"。
   §8.2 就是这么规定的,但用户在"刚编译过 + 又想查找"时会觉得 Ctrl-N 不听话。
5. **A⑤ 的定论只覆盖 /tmp**。/work 在 FUSE 上,退出时的写盘慢是环境属性;
   如果协调者想要"在 FUSE 上也 100 次不卡"的证据,可以把
   `wave5-exit-loop 100 10000 ./cppide <FUSE 上的文件>` 再跑一遍(本轮没跑)。
6. **B③ 的 100MB 场景峰值 212 MiB**。对 ICPC 训练机来说没问题,但如果哪天要在
   内存很紧的机器上跑,`ProcResult` 全量持有 stdout 这一点会是瓶颈(改法是流式落盘,
   不在本轮范围)。
7. `tests/test_openpath.cpp` **依赖已构建好的 `./cppide`**(找不到就整体跳过并打印说明),
   而 `make tests` 并不依赖 `$(BIN)`。先 `make` 再 `make tests` 才能真正跑到它。
8. `tests/test_ai.cpp` 里原有的用例会用**假 key** 打真实 `api.deepseek.com` 拿 401
   (无网络时自动跳过)。不是本轮引入的,但记在这里:离线环境下 `make tests` 仍然通过。

---

## 7. 顺手清掉的一件脏事:`make tests` 会在仓库根目录留 core 文件

**症状**:每跑一次 `make tests`,`/work/` 里就多一个 `core`(430KB 或 2MB,随机哪个)。

**定位**(`readelf -n /work/core` 读 `NT_FILE` / `NT_PRPSINFO`):
* 430KB 那个来自 `/bin/sh -c "kill -SEGV $$"` —— `tests/test_proc.cpp::t_signaled`
  故意用 SIGSEGV 验证"被信号终止"的汇报;
* 2MB 那个来自 `/tmp/test_editor` —— `tests/test_editor.cpp::testSetGhostAssertFires`
  故意 fork 一个注定 `assert` 失败(SIGABRT)的子进程。

两者都是**有意为之的测试手法**,问题只在于本机 `/proc/sys/kernel/core_pattern == "core"`,
内核把 core 落在**子进程的 cwd**,而那正好是仓库根目录。

**修法**(只改测试,不动 `proc.cpp` 的行为):
* `t_signaled`:`spec.cwd = "/tmp"` + 脚本里先 `ulimit -c 0`;
* `testSetGhostAssertFires`:子进程里 `setrlimit(RLIMIT_CORE, {0,0})` 之后再触发 assert。

验证:`rm -f /work/core && make tests` → rc=0、16 个测试全过、`/work/core` 不再出现。

**留给协调者的一个判断**:用户自己的程序被 `Ctrl-R` 跑崩时,也会在**用户的项目目录**里
留下 `core`(需求明确"不做调试器集成",所以那个 core 没人会用)。
要不要在 `proc.cpp` 的 fork 子进程里统一 `setrlimit(RLIMIT_CORE, 0)`?
我没有擅自改 —— 那会改变已通过 ASan 的 `proc.cpp` 的行为,而且"要不要保留 core"
算产品决定。**请裁决。**

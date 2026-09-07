# Wave 4 · `src/app.cpp` + `src/main.cpp` 实现与验收记录

> 负责范围:主循环、按键分派、事件排空、编译/运行/AI 编排、进程入口与 argv。
> 只新建了 `src/app.cpp`(1200 行)、`src/main.cpp`(374 行),并给 `src/app.h`
> 的 **private 区加了 7 个成员变量**(无任何签名/公开接口变更)。

---

## 1. 结果速览

| 项 | 结果 |
|---|---|
| `g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/app.cpp src/main.cpp` | 零 warning |
| `make check-headers` | OK(15 个头) |
| `make` 完整链接 | 成功(`./cppide`) |
| `make tests` | 全部通过(test_ui 833467 断言 / test_util 267024 断言 等) |
| 非交互验收 1~4 | 全通过 |
| pty 冒烟(验收 5) | 全通过 |
| pty 死循环 + Esc 终止(验收 6) | 全通过 |
| ASan/UBSan 下重跑全部 8 组 | 48 项断言全过,无 sanitizer 报告 |

一键复跑:

```sh
sh /work/.flower/scripts/wave4-app-verify.sh          # release
sh /work/.flower/scripts/wave4-app-verify.sh --asan   # ASan/UBSan(对象编到 /tmp/asan-obj,不动 src/*.o)
```

---

## 2. 给 `app.h` 新增的 private 成员(7 个,仅变量)

```cpp
std::string compile_src_;         // 本次编译的源文件(analyze / 跳行都要它)
std::string compile_binary_;      // 本次编译的产物路径(syntax_only 时为空)
bool compile_syntax_only_ = false;
bool last_busy_ = false;          // 上一 tick 的 runner_.busy():变化时刷状态栏
std::string ai_stream_;           // 本轮 AI 已收到的流式全文(AiDone 的兜底来源)
Pos ai_anchor_{};                 // AiStarted 时的光标 = ghost 的插入锚点
uint64_t ai_anchor_gen_ = 0;      // ai_anchor_ 属于哪个 generation
```

理由:`Builder::analyze(pr, binary_path, syntax_only)` 需要"提交编译时"的三个信息,
而 `AppEvent` 只带 `gen` + `ProcResult`;ghost 的锚点必须是**请求发出时**的光标而不是
回复到达时的光标;`last_busy_` 用来把"运行中→结束"这个状态变化变成一次重绘。

---

## 3. 关键设计落点(评审请看这几处)

### §6 练习模式硬保证
- `app.cpp` 里 **`switch (ev.sink)` 只出现一次**,在 `App::onAiDone`(第 546 行);
  `ed_.setGhost(...)` 也**只被调用一次**(第 559 行,GhostText 分支内)。
- `onAiDelta` 只做 `ai_stream_ += ev.text`,**刻意不判 sink / 不判模式**,
  于是"文本允许变成什么"这件事全工程只在一处被回答。
- `doTab()` 逐字照 §6:`if (ed_.ghost().complete) ed_.acceptGhost(); else ed_.indent();`
  —— 这条路径**一行模式判断都没有**。
- `grep "AiMode::" src/app.cpp` 的 6 处全部与"文本去向"无关:
  启动/切模式的中文提示、`doAskAi` 决定是否弹提问输入行、`model()` 填徽章。

### §5.2 第二层过滤
`drainEvents()` 用 `tryPop`(永不阻塞),对 4 种 Ai 事件先比
`ev.gen != ai_.generation()` 再分发;编译/运行事件比 `ev.gen != compile_job_/run_job_`
丢弃过期作业。每 tick 上限 200 个。

### §15 诊断行号偏移
`doCompile()` 先往编译面板写一行 `$ <命令回显>`;`onCompileDone()` 取
`offset = p.lineCount()`,把 `util::splitLines(stderr_text)` 的每一行 `appendLine`
(不用 `append`,因为它会吞掉空行、破坏 1:1 行号对应),同时
`diags[i].out_line = offset + out_line`。于是"面板 Enter 跳源码"与
"Ctrl-N 高亮面板行"用的是同一套行号。

### §4.2 第三道防线
- `doRun()` 在**显式 Ctrl-R/Ctrl-E** 时聚焦【运行】面板 —— 这是用户主动请求
  (不是后台事件抢焦点),同时保证"在【运行】面板按 Esc 终止"这条逃生口按得到。
- `doEscape()` 优先级链:帮助浮层 → ghost → 提示行 → **运行面板且 busy 则 killInFlight**
  → 面板取消焦点 → 取消在飞 AI 请求。

### §8.1 自动聚焦
只有 `!out->ok` 的编译结果会 `focusPanel(Compile)`,且**提示行打开时不抢**
(改为只置未读)。其它一切事件(运行结束 / AI 回复 / AI 错误)一律只 `setUnread(true)`。

### 面板聚焦时的键
`onPanelKey` 先消费面板键(滚动 / 编译面板行光标 / Tab 切标签 / Enter / Esc /
【输入】面板的编辑与撤销),**剩下的只有 `isGlobalAction()` 白名单里的动作才转交**
给 `onEditorKey`。所以"在面板里打字"在结构上不可能改到编辑缓冲区。

---

## 4. 非交互入口(验收 1~4 的实测)

```
$ ./cppide --help          退出码 0,3907 字节(用法 + 配置说明 + keys.h 的全键表)
$ ./cppide --version       退出码 0,"cppide 0.1.0"
$ ./cppide --print-config  退出码 0,2252 字节,python json.load 通过(可直接重定向成配置文件)
$ ./cppide --doctor        退出码 0,2130 字节
```

`--doctor` 的分段:`[终端]`(tty 判定 + `Ui::doctorReport()` 给的 TERM / LANG /
LC_CTYPE / ncurses 版本 / MB_CUR_MAX / COLORS / 最小尺寸)、`[配置]`(查找路径 /
是否存在 / 实际加载自 / 致命说明 / **api_key 只显示"已配置(已隐去)"** / model /
chatUrl / 各超时 / 全部 warnings)、`[编译器]`(在 PATH 里 `access(X_OK)` 找 cc/cxx,
**不启动任何子进程**)、`[结论]`。

**key 不泄露**(用假 key `sk-DEADBEEF...FAKEKEY` 灌进临时配置实测):

```
PASS  --doctor 不含 api_key 值
PASS  --doctor 有 key 时显示 已配置(已隐去)
PASS  --print-config 不含 api_key 值
```

缺配置:`./cppide --config /nonexistent/x.json --doctor` 退出码 0,warnings 里明确
"未找到配置文件 /nonexistent/x.json:已使用默认设置,AI 功能已关闭。"

非法入参一律**中文报错 + 退出码 2 + 不崩**:

```
cppide: 找不到文件 /tmp/xxx.cpp。
       提示:不带参数启动 cppide 会开一个空缓冲区,编辑后用 Ctrl-O 另存为。
cppide: /tmp 是一个目录,不是文件。
cppide: 未知选项 --bogus。请用 `cppide --help` 查看用法。
cppide: 选项 --config 缺少参数。用法:cppide --config <配置文件路径>
cppide: 一次只能打开一个文件(本编辑器只编译当前打开的单个文件),但收到 2 个:a.cpp b.cpp
```

无 tty(stdin/stdout 都不是终端)**退出码 3**,不崩不挂死:

```
cppide: 当前环境不是交互式终端(stdin 是 tty:否,stdout 是 tty:否),无法启动全屏界面。
       请在真实终端里运行;非交互场景可用:--help / --version / --print-config / --doctor。
```

---

## 5. pty 端到端(验收 5)

驱动器:`.flower/scripts/wave4-pty-drive.cpp`(`forkpty` + 80x24 winsize +
`TERM=xterm-256color` + `LANG=C.UTF-8`,脚本命令 send/wait/expect/expectfrom/mark)。
脚本:`.flower/scripts/wave4-pty-smoke.txt`。

```
PASS  expect "练习模式"          <- 模式徽章可见
PASS  expect "t.cpp"
PASS  expect "// 中文注释"        <- 输入中文(Ctrl-L 强制全量重绘后断言,见下)
PASS  expect "已保存"
PASS  expect "zzz_undeclared"     <- Ctrl-B 编译失败,自动聚焦编译面板
PASS  expect "错误"
PASS  expect "第 4 行"            <- Ctrl-N 跳诊断,行号正确
PASS  expect "写代码模式"          <- Ctrl-T 切模式
退出码 = 0                        <- Ctrl-X 正常退出
```

磁盘核对:`head -1 /tmp/t.cpp` == `// 中文注释`(原第 3 行的错误因此下移到第 4 行,
`gcc` 报的是 `/tmp/t.cpp:4:13: error:`,状态栏跳转显示"错误 第 4 行:'zzz_undeclared'…",
光标位置字段显示 `4:13` —— 行号与列号都对得上)。

屏幕片段(过滤掉 ESC 序列后):

```
[编译 1]  ─ [运行] ─  [AI*]  ─ [输入]   Esc 回编辑区
/tmp/t.cpp: In function 'int main()':
/tmp/t.cpp:4:13: error: 'zzz_undeclared' was not declared in this scope
    4 |     int y = zzz_undeclared;
      |             ^~~~~~~~~~~~~~
1 错误 · 用时 101ms
 写代码模式  │  t.cpp  │ 4:13 │ C++ │ 已切换到写代码模式 │ AI 未配置
```

**踩坑记录(给后来者)**:ncurses 是增量刷屏,逐字输入中文时字节流里出现的是
`中#include <iostream>` / `文#include <iostream>` 这种"新字符 + 重画行尾"的交错,
整行 `// 中文注释` **不会连续出现**。断言前先按一次 `Ctrl-L`(`forceFullRedraw`)
逼出全量重绘,整行才会以连续字节出现。

---

## 6. pty 死循环(验收 6)

被测程序:`volatile long x=0; for(;;){x=x+1;}`;配置 `run_timeout_ms = 60000`
(故意远大于测试时长,排除"是超时而不是 Esc 干掉的"这种解释)。
脚本:`.flower/scripts/wave4-pty-loop.txt`。

```
PASS  expect "运行中"                 <- Ctrl-R 自动编译后运行,状态栏 "运行中… Esc 终止"
PASS  expect "AI 未配置"              <- 死循环还在跑时按 Alt-3 切到 AI 面板,画面照常更新
                                         => UI 线程没有被子进程挂住
PASS  expect "已终止运行中的程序"      <- Alt-2 回到运行面板,Esc
PASS  expect "SIGKILL"                <- 面板末行 "已终止 · 信号 SIGKILL · 用时 4417ms"
PASS  expect "写代码模式"              <- 终止后仍响应 Ctrl-T
退出码 = 0                            <- Ctrl-X 正常退出,无孤儿进程(/proc 核对过)
```

---

## 7. 附加 pty 用例(不在清单里,但覆盖了需求验收项)

`.flower/scripts/wave4-pty-stdin.txt`:【输入】面板输入 `21` → `Ctrl-E` 编译并运行
→ 面板出现 `n=42` 与 `退出码 0 · 用时 2ms`;随后 `Ctrl-A` 在 AI 未配置时只产生
面板消息 `AI 未配置:请在 /tmp/nocfg-missing.json 填写 api_key(编辑、编译、运行不受影响)。`
—— 对应需求"能从输入区喂 stdin、看到 stdout 和退出码"与"key 为空时只关 AI 不崩"。

`.flower/scripts/wave4-pty-edit.txt`:`Ctrl-G` 3 Enter → "已跳到第 3 行";
`Ctrl-F` main Enter → "找到:main";`Ctrl-U` → "没有可撤销的操作";
`F1` → 帮助浮层("cppide 快捷键");`Esc` 关浮层;`Alt-0` 折叠面板;`Ctrl-L` 重绘;
之后仍响应 `Ctrl-T`。

---

## 8. 已知取舍与残留风险

1. **不存在的文件 → 报错退出(码 2)**,而不是"当新文件打开"。这是任务书验收 3 的
   明文要求;代价是新建文件必须"不带参数启动 → 编辑 → Ctrl-O 另存为"。
   若产品上想改成 nano 语义,只需删掉 `main.cpp::checkOpenPath` 里的 `fileExists` 分支。
2. **`Ctrl-R`/`Ctrl-E` 会聚焦【运行】面板**。严格读 §8.1"只有编译失败会自动聚焦"
   的话这是一处扩张;理由写在 `doRun()` 的注释里(用户主动按键 ≠ 后台事件抢焦点,
   且不聚焦就按不到 Esc 逃生口)。若评审要求严格,把 `focusPanel(PanelId::Run)`
   一行删掉即可,但那样必须放宽 `doEscape()` 里的焦点判断,否则死循环无法终止。
3. **编译前会自动保存**(路径为空则弹另存为)。理由:编译的必须是磁盘上的内容,
   否则用户会对着"改过但没保存"的源码看诊断行号。
4. **AiDone 的载荷约定做了兼容**:`full = ev.text.size() >= ai_stream_.size() ? ev.text : ai_stream_`,
   无论 ai.cpp 选择"Done 带全文"还是"Done 只收尾"都能工作。**真机 AI 链路未验证**
   (不联网、且绝不使用真实 key),`AiStarted/AiDelta/AiDone/AiError` 四条路径只做了
   编译期与逻辑评审,没有端到端证据。
5. **`Esc` 只在【运行】面板终止子进程**。编译作业卡住时靠 `compile_timeout_ms`
   (默认 30s)兜底,没有给编译面板同样的逃生口(严格照 §8.3 的"Esc 交回编辑区")。
6. `Ui::doctorReport()` 自带一行 "按键后会回显实际键码;Ctrl-X 退出 --doctor" 的提示,
   但 `--doctor` 是纯非交互的;我在后面补了一行说明纠正,没有改 ui.cpp。
7. pty 用例依赖 `expect` 在字节流里做子串匹配。宽字符字段被 ncurses 拆开重画时
   可能匹配不到(见 §5 的踩坑记录),新增断言时优先选"短且独占一行"的文本,
   或先 `Ctrl-L`。

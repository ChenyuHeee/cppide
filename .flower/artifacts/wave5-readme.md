# Wave 5 · README + 示例配置:核对记录与差异清单

产出:
- `/work/README.md`
- `/work/config.sample.json`
- `/work/.flower/scripts/wave5-readme-verify.sh`(`# desc:` 首行已加,可重复跑)

原则:**代码是唯一权威**。architecture.md 与 README 冲突时按代码写,并在下面列出差异。

---

## 1. 快捷键核对(`src/keys.cpp` 的 `kBindings[]` 逐条 vs architecture.md §8.2/§8.3)

绑定表位置:`src/keys.cpp:60-132`(**58 条绑定,覆盖 45 个不同 Action**)。

### 与 architecture.md 不一致、README 按代码修正的 5 处

| # | architecture.md §8.2 | 代码实际(`keys.cpp`) | README 采用 |
|---|---|---|---|
| 1 | F 键别名只列 `F2 F5 F6 F8 F10`(F1 单列一行) | 另有 `F3`=FindNext(`:114`)、`F4`=AskAi(`:111`)、`F7`=CompileAndRun(`:107`)、`F9`=NextTab(`:131`) | 列出全部 10 个:F1 帮助 / F2 保存 / F3 查找下一处 / F4 问 AI / F5 编译 / F6 运行 / F7 编译并运行 / F8 切模式 / F9 下一标签 / F10 退出 |
| 2 | 表里**完全没给** BufferStart / BufferEnd 的键 | `Alt-<` `Alt-Home` `Shift-Home` → BufferStart;`Alt->` `Alt-End` `Shift-End` → BufferEnd(`:75-80`) | 全部写入「移动」组,并解释为何不用 `Ctrl-Home/End` |
| 3 | 「`Ctrl-F` 查找;`Ctrl-N` 下一处(无诊断时)」 | FindNext **没有 Ctrl 绑定**,唯一直达键是 `F3`;`Ctrl-N/Ctrl-P` 是 NextDiag/PrevDiag,仅在 `cycleDiag()` 找不到可跳诊断时才退化为查找下一处/上一处(`app.cpp:957-978`) | 查找下一处写成 `F3`;`Ctrl-N/P` 写成「诊断,无诊断时退化为查找」 |
| 4 | §8.3「面板聚焦时 `Tab` 切下一个标签」 | `Tab`(键码 9)全局绑到 `Action::Tab`;面板聚焦时的转换由 `app.cpp:330-333` 做。`keys.cpp` 另给 `F9` 一个不依赖焦点的直达绑定 | 两条都写(`Tab` 需面板聚焦,`F9` 无条件) |
| 5 | 「同理不绑 `Ctrl-M`/`Ctrl-J`/`Ctrl-I`/`Ctrl-H`」 | `13`(Ctrl-M)→ Enter、`9`(Ctrl-I)→ Tab **必须绑**,否则 Enter/Tab 不能用(`:83,88`);真正不绑的是 `Ctrl-H`(8)与 `Ctrl-J`(10) | 明确区分:「不作为独立 Ctrl 快捷键、键名永远显示 Enter/Tab」 |

### 其余逐条一致(无需修正)

移动 `←→↑↓`/`Alt-←→`/`Home`/`End`/`PgUp`/`PgDn`;编辑 `Enter`(含 `KEY_ENTER`)/`Backspace`
(`127` 与 `KEY_BACKSPACE` 两个键码)/`Delete`/`Tab`/`Shift-Tab`/`Ctrl-U` 撤销 /`Ctrl-W` 重做 /
`Ctrl-K` 剪切行 /`Ctrl-V` 粘贴行 /`Ctrl-D` 复制行;全局 `Esc`/`Ctrl-O`/`Ctrl-X`/`Ctrl-B`/`Ctrl-R`/
`Ctrl-E`/`Ctrl-T`/`Ctrl-A`/`Ctrl-G`/`Ctrl-F`/`Ctrl-N`/`Ctrl-P`/`Ctrl-L`/`F1`;
面板 `Alt-1..4`/`Alt-0`/`Alt-=`/`Alt--`。**不绑**:`Ctrl-C`/`Ctrl-Z`/`Ctrl-S`/`Ctrl-Q`/`Ctrl-\`/
`Ctrl-Y`/`Ctrl-H`/`Ctrl-J` —— 与 §8.2 及 `跨模块约定.md` §18 一致。

面板聚焦时的键取自 `app.cpp:326-433`(`Esc` 在【运行】面板且 `runner_.busy()` 时终止子进程:
`app.cpp:881-890`)。

---

## 2. 配置字段核对(`src/config.h` vs `src/config.cpp` vs README)

- `Config` 结构体成员共 **33** 个,其中 **31 个是可配字段**,另 2 个(`source_path`、
  `warnings`)是运行期元信息(`config.h:60-62`)。
- `config.cpp:154-163` 的 `kKnownKeys[]` 正好 **31** 个,与可配字段一一对应。
- `--print-config` 输出 40 个 JSON 键 = 31 个字段 + 9 个 `_comment*` 注释键。
- **README 表格 31 行,全覆盖,一个不漏**;「范围」列直接抄 `config.cpp:174-207` 的 clamp 实参
  (`takeInt/takeDouble/takeSize` 的 lo/hi),这是 architecture.md §7 里没有的信息。
- 与 architecture.md §7 的字段名 / 默认值 **完全一致**,无差异。

实测过的降级分支(全部写进 README §2.5):
- 文件不存在 → 全默认 + warning「未找到配置文件 …,已使用默认设置,AI 功能已关闭。」
- JSON 语法错 → 全默认 + **带行列号**的 warning(实测:`第 2 行第 9 列:对象的键之后期待 ':'`)
- 类型错 → `配置字段 "tab_width" 应为数字,实际为字符串,已改用默认值。`
- 越界 → `配置字段 "panel_height" 的值 999 超出允许范围 [3, 100],已修正为 100。`
- 环境变量覆盖:`CPPIDE_MODEL` / `CPPIDE_BASE_URL` / `DEEPSEEK_API_KEY` / `CPPIDE_CONFIG`
  实测全部生效(`--doctor` 显示 `api_key: 已配置(已隐去)`,**不回显 key**)。

---

## 3. `config.sample.json`

- 生成方式:`./cppide --print-config > config.sample.json`,`diff` 确认与
  `ConfigLoader::sampleJson()` 输出**逐字节相同**。
- `python3 json.load` 通过;`api_key == ""`;非 `_` 前缀键 31 个。
- `./cppide --config config.sample.json --doctor` → **rc=0**,warnings 2 条:
  1. `AI 未配置:请在 config.sample.json 里填写 api_key,或设置环境变量 CPPIDE_API_KEY。…`
  2. `model 当前是占位默认值 "deepseek-chat",请按 DeepSeek 官方文档填写实际模型名。`
- 与 `sampleJson()` **无差异**,不需要以任何一方为准的裁决。

---

## 4. 命令真跑记录

脚本:`.flower/scripts/wave5-readme-verify.sh`(全部命令用 `timeout` 包住)。

| 命令 | 结果 |
|---|---|
| `make` | rc=0 |
| `make check-headers` | rc=0(15 个头) |
| `make tests` | rc=0(16 个测试二进制全过) |
| `./cppide --help` | rc=0 |
| `./cppide --version` | rc=0(`cppide 0.1.0`) |
| `./cppide --print-config` | rc=0 |
| `./cppide --doctor` | rc=0 |
| `./cppide --config config.sample.json --doctor` | rc=0 |
| `./cppide --bogus` | rc=2 |
| `./cppide a.cpp b.cpp` | rc=2 |
| `./cppide /tmp/x.txt`(不支持的扩展名) | rc=2 |
| `./cppide /nope/dir/a.cpp` | rc=2 |
| `./cppide /tmp`(目录) | rc=2 |
| `./cppide /tmp/_fifo.cpp`(命名管道) | rc=2 |
| `./cppide /tmp/_newfile.cpp`(非交互环境) | rc=3 |
| `CPPIDE_MODEL=… DEEPSEEK_API_KEY=… --doctor` | rc=0,覆盖生效 |
| `CPPIDE_CONFIG=… --doctor` | rc=0,路径生效 |
| `--config` + 语法错 / 类型错 / 越界 JSON | rc=0,warnings 如 §2 |
| `--config` + 假编译器名 | rc=0,`cc = no-such-compiler-xyz -> 未找到!` |

**没跑的 3 条**(README 里提到但本环境不宜执行,已在此诚实标注):
- `make debug` —— 它是 `clean all`,会删掉 `.o` 和 `cppide`,而**有并行 agent 正在改代码**,
  删了会打断他们。Makefile 目标本身是 `CXXFLAGS += -O0 -g -fsanitize=address,undefined`
  + `LDFLAGS` 同步,写法正确。
- `make clean` —— 同上原因。
- `make install` —— 会往 `/usr/local/bin` 写文件,不在容器里做。

---

## 5. 发现的代码层面问题(只报告,未修改任何 `.h`/`.cpp`/`Makefile`)

1. **`make tests` 曾在核对期间失败,现已自行恢复(并行 agent 的 in-flight 状态)** ——
   记录在此以免协调者困惑。中途两次快照:
   ```
   FAIL tests/test_appai.cpp:373  CHECK(total == cleared)
        onAiDone 里 ai_stream_ 出现在了 clear() 之外的地方   → Aborted (core dumped)
   tests/test_openpath.cpp:48  编译错误(CHECK_M 宏展开)
   ```
   两个文件都是**本次任务期间**新增的(我开工时 `tests/` 有 14 个 test,现在 16 个)。
   **最后一次完整跑:`make` / `make check-headers` / `make tests` 三个全部 rc=0。**
   `src/main.cpp` 的 CLI 语义也在此期间被改过(见 §6)。

2. **`keys.cpp:190` 的 `helpRow` 列宽不够,帮助浮层与 `--help` 输出有粘连**:
   `padRight(keyLabel(a), 22)`,但 `BufferStart` 的 label 是
   `Alt-< / Alt-Home / Shift-Home`(显示宽度 28),补齐失效,输出成
   `Alt-< / Alt-Home / Shift-Home跳到文件开头`(键名和说明之间没有空格)。
   `BufferEnd` 同。纯观感问题,把 22 改成 30 即可。README 里我写的是自己排的表格,不受影响。

3. **`syntaxOnly()` 与 `langFromPath()` 的扩展名集合不一致**:
   `build.cpp:265-269` 的 `syntaxOnly` 认 `.h .hpp .hh .hxx .h++ .hp .inc`;
   `textbuf.cpp:506-513` 的 `langFromPath` 只认 `.h .hpp .hh .hxx`(不认 `.h++ .hp .inc`)。
   而 `main.cpp:289` 用 `langFromPath == Unknown` 做开文件的门槛 —— 所以
   `.h++` / `.hp` / `.inc` **打不开**,`syntaxOnly` 里那三个分支是死代码。
   不是 bug(对用户无可见影响),但 README 只能写 `langFromPath` 认的那一套,已照此写。

4. **`langFromPath()` 大小写敏感,`syntaxOnly()` 不敏感**:`main.CPP` 会被
   `langFromPath` 判成 `Unknown` 从而被拒绝打开(而 `main.C` 可以)。属于取舍,已按现状写 README。

5. **`--help` / `--doctor` 与真实行为在一处对不上**(wave4-app.md §6 已记,此处复述):
   `Ui::doctorReport()` 里印着「按键后会回显实际键码;Ctrl-X 退出 --doctor」,
   但 `--doctor` 是纯非交互的。`main.cpp` 在后面补了一行说明纠正。README 按**真实行为**写
   (「`--doctor` 不进按键回显」),没有照抄那行提示。

---

## 6. 遗留风险

- **README 里所有关于 macOS 的具体 UI 路径**(「系统设置 → 键盘 → …」「Terminal.app 设置 →
  描述文件 → 键盘 → 将 Option 键用作 Meta 键」「iTerm2 → Keys → Esc+」)**无法在本环境验证**,
  来自 architecture.md §8.2 的表述 + 通用知识。我已把「先按 `Esc` 松开再按目标键」写成
  **在任何终端都成立的兜底路径**(这条是有代码依据的:`ui.cpp` 的 Esc 前缀合成),
  所以即使那些菜单路径在新版 macOS 上改了名,用户也不会被卡住。
- **macOS 编译/键码风险**在 README §9 给了修法(`-D_XOPEN_SOURCE_EXTENDED`、改 `kNcS*` 常量),
  但**修法本身未经验证** —— 只是把 Makefile 注释和 wave3 artifact 的判断写成了操作步骤。
- **AI 端到端链路从未真机验证**(不联网、绝不使用真实 key)。README §9 的错误对照表
  是从 `aihttp.cpp:281-364` 的 `curlErrorZh` / `chatErrorZh` **逐条抄的文案**,
  文案本身准确;但「401 实际会不会返回」这类端到端行为没有证据。
- **`main.cpp` 的 CLI 语义在我写作期间被并行 agent 改过一次**(原本「不存在的文件」rc=2 报错,
  改成了「当新文件打开」,并新增非交互环境 rc=3 检查)。README §7 已按**当前**代码重写并重新
  跑通全部退出码。若之后又改,`.flower/scripts/wave5-readme-verify.sh` 会把差异跑出来。
- README 未涵盖 `make debug` 的实际 ASan 输出(没跑,见 §4)。

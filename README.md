# cppide

一个从零实现的、跑在终端里的 **C/C++ 单文件代码编辑器**:自带语法高亮、一键编译运行、
以及基于 DeepSeek API 的 AI 辅助(「练习模式」只给中文思路,「写代码模式」给灰色 ghost 补全)。
为 ICPC / 算法竞赛训练场景做的,不依赖 vim / neovim / VSCode。

## 边界(先看这一段,省得白试)

- **只服务 C / C++**。**只接受 C/C++ 扩展名**,其它类型在启动时就被拒绝。
  `.c` = C;`.C` `.cpp` `.cc` `.cxx` `.c++` `.h` `.hpp` `.hh` `.hxx` `.h++` `.hp` = C++。
  除 `.c`(C)与 `.C`(C++)这一对之外,扩展名**大小写不敏感**(`.CPP` / `.Hpp` 都认)。
  不做其他语言。
- **只面向 macOS 终端**(Terminal.app / iTerm2)。不做 GUI、不做网页版、不做 Windows 适配。
  Linux 上能编能跑(开发就是在 Linux 容器里做的),但不是交付目标。
- **只编译当前打开的那一个文件**。没有工程、没有 CMake / Makefile 驱动、没有多文件链接。
- 没有 LSP、没有跨文件索引、没有重构、没有调试器集成、没有 Git 集成。
- 不做 OJ 对接、不做自动提交、不做题库。
- 不代买 / 不代配 API key。key 要你自己去 DeepSeek 官方控制台申请。

---

## 1. 编译本编辑器

### 1.1 macOS(交付目标平台)

依赖全部是系统自带的:Command Line Tools 里的 `clang++`、系统 `ncurses`(自带的就是宽字符版)、
系统 `libcurl`。

```sh
xcode-select --install        # 如果还没装过 Command Line Tools
cd /path/to/cppide
make                          # 产物:./cppide
./cppide --doctor             # 先自查一遍环境
./cppide main.cpp             # 开始用
```

`make` 会走 Makefile 里的 `Darwin` 分支:链 `-lncurses -lcurl -lpthread`,
并且如果 `brew --prefix ncurses` 存在就优先用 Homebrew 的头和库。

> **诚实声明**:本项目的全部自动化验证(`make`、`make tests`、`make check-headers`、
> `--doctor`、pty 冒烟)都是在 **Debian aarch64 容器**里跑完的。**macOS 上未经真机验证。**
> 首次在 mac 上 `make` 若报错,请直接跳到 [§9 故障排查](#9-故障排查) 的
> 「macOS 首次编译报错」一节 —— 已知的两个候选原因(宽字符原型、`KEY_S*` 键码)都在那里,
> 各有一行现成的修法。

### 1.2 Linux(开发 / 验证环境)

```sh
sudo apt-get install build-essential libncursesw5-dev libcurl4-openssl-dev
# Debian trixie 上包名亦可写作 libncurses-dev + libcurl4-gnutls-dev
make
```

Linux 分支链的是 **`-lncursesw`**(不是 `-lncurses`)—— 宽字符符号 `get_wch` / `add_wch`
只在 `ncursesw` 里,链错了中文会乱码或直接链接失败。

### 1.3 其它 make 目标

| 命令 | 作用 |
|---|---|
| `make` | 编译出 `./cppide`(15 个 `.cpp` → 15 个 `.o` → 链接) |
| `make tests` | 编译并逐个运行 `tests/test_*.cpp`,任一失败即停 |
| `make check-headers` | 验收门:对每个 `src/*.h` 生成一个只 `#include` 它的 TU 做 `-fsyntax-only`,证明每个头自给自足(15 个头) |
| `make debug` | 用 `-O0 -g -fsanitize=address,undefined` 编出**独立**的 `./cppide-debug`(目标文件是 `src/*.dbg.o`)。不依赖 `clean`、不动 release 的 `./cppide`,所以 `make debug -j4` 安全,也不会打挂正在跑的验收 |
| `make clean` | 删掉 `.o` / `.d` / `cppide` 与 debug 的 `*.dbg.o` / `cppide-debug` |
| `make install` | `install -m 0755 cppide $(PREFIX)/bin/`,`PREFIX` 默认 `/usr/local` |

---

## 2. 配置文件

### 2.1 放在哪(路径优先级,从高到低)

1. 命令行 `--config <PATH>`
2. 环境变量 `CPPIDE_CONFIG`
3. `$XDG_CONFIG_HOME/cppide/config.json`
4. `~/.config/cppide/config.json` ← **推荐**

`./cppide --doctor` 会把「查找路径 / 文件存在 / 实际加载自」三行打出来,不用猜。

### 2.2 怎么创建

```sh
mkdir -p ~/.config/cppide
./cppide --print-config > ~/.config/cppide/config.json
$EDITOR ~/.config/cppide/config.json      # 把 api_key 填进去
```

仓库里的 `config.sample.json` 就是 `--print-config` 的原样输出,可以直接抄。
以 `_` 开头的键(`_comment`、`_comment_env`、…)是**注释**,加载时会被忽略,可以随手删掉。
所有字段都可以省略,省略即用内置默认值。

> `api_key` 一填,这个文件就变成机密文件了。**不要提交进版本库。**

### 2.3 字段全表

字段名与默认值以 `src/config.h` 为准。「范围」列是 `src/config.cpp` 里的 clamp 区间:
超范围不会报错,会被夹到边界并留一条 warning。

#### AI

| 字段 | 类型 | 默认值 | 范围 | 说明 |
|---|---|---|---|---|
| `api_key` | string | `""` | — | DeepSeek API key。**留空 = 关闭 AI**,编辑 / 高亮 / 编译 / 运行完全不受影响。需自行到 DeepSeek 官方控制台申请 |
| `base_url` | string | `"https://api.deepseek.com"` | 非空 | API 根地址。填空串会回落默认值并留 warning |
| `chat_path` | string | `"/v1/chat/completions"` | 非空 | chat completions 的路径。与 `base_url` 拼成完整 URL(重复/缺失斜杠会被处理) |
| `model` | string | `""`(**空**) | — | **模型名只从配置文件读,代码里没有任何内置模型名。** 请按 DeepSeek 官方文档上的模型名填写。**留空 = AI 未完整配置**:状态栏显示 `AI 未配置`,一次请求都不会发出,AI 面板给出中文提示;编辑 / 高亮 / 编译 / 运行完全不受影响。本项目不对任何模型名的有效性或能力做断言;填错时你会看到 HTTP 400/404 的中文错误提示 |
| `stream` | bool | `true` | — | `true` 走 SSE 流式(边生成边显示);`false` 走一次性响应 |
| `ghost_delay_ms` | int | `500` | 0–60000 | 写代码模式:光标停顿多久后自动请求补全 |
| `ghost_min_interval_ms` | int | `1200` | 0–600000 | 两次**自动**请求的最小间隔(省钱)。手动 `Ctrl-A` 无视此限制。另外**同一次停顿只会自动请求一次** —— 手离开键盘不动不会持续计费,要再来一次就再打字/移动光标,或按 `Ctrl-A` |
| `ghost_max_lines` | int | `8` | 1–200 | ghost text 最多显示几行 |
| `ai_connect_timeout_ms` | int | `5000` | 100–300000 | 建连超时 |
| `ai_timeout_ms` | int | `20000` | 100–600000 | 单次请求总超时 |
| `ai_max_context_lines` | int | `400` | 1–100000 | 发给模型的上下文**行数**上限;超了就截取光标附近若干行,中间填 `/* ...省略... */` |
| `max_tokens_code` | int | `256` | 1–32768 | 写代码模式的 `max_tokens` |
| `max_tokens_practice` | int | `512` | 1–32768 | 练习模式的 `max_tokens` |
| `temperature_code` | double | `0.2` | 0.0–2.0 | 写代码模式的 temperature |
| `temperature_practice` | double | `0.7` | 0.0–2.0 | 练习模式的 temperature |
| `prompt_code` | string | `""` | — | 写代码模式的 system prompt。**空 = 用内置默认** |
| `prompt_practice` | string | `""` | — | 练习模式的 system prompt。空 = 用内置默认。注意:「不要输出可运行代码」这条后缀由程序**强制追加**,覆盖不掉 |

#### 编译

| 字段 | 类型 | 默认值 | 范围 | 说明 |
|---|---|---|---|---|
| `cc` | string | `"cc"` | 非空 | 编 `.c` 用的编译器 |
| `cxx` | string | `"c++"` | 非空 | 编 `.cpp` / `.cc` / `.cxx` / `.c++` / `.C` 用的编译器;`.h` / `.hpp` / `.hh` / `.hxx` / `.h++` / `.hp` 的语法检查也用它 |
| `cflags` | string[] | `["-O2","-std=c11","-Wall"]` | — | `.c` 的编译参数。写 `[]` 表示不加任何参数 |
| `cxxflags` | string[] | `["-O2","-std=c++17","-Wall"]` | — | `.cpp` 系列 + 头文件语法检查的编译参数 |
| `compile_timeout_ms` | int | `30000` | 1000–600000 | 编译超时。超时后子进程连同它的整个进程组被杀 |

#### 运行

| 字段 | 类型 | 默认值 | 范围 | 说明 |
|---|---|---|---|---|
| `run_timeout_ms` | int | `5000` | 100–600000 | 运行超时(死循环兜底) |
| `run_output_limit` | size | `1048576`(1 MiB) | 1024–268435456 | 单次运行捕获的 stdout / stderr 字节上限(各自)。超了截断并在末行标注 |
| `stdin_file` | string | `""` | — | 非空则从这个文件喂 stdin(每次运行时重读),【输入】面板只作展示;**空 = 用【输入】面板里编辑的内容** |

#### 编辑器

| 字段 | 类型 | 默认值 | 范围 | 说明 |
|---|---|---|---|---|
| `tab_width` | int | `4` | 1–16 | Tab 的显示宽度 / 缩进宽度 |
| `expand_tab` | bool | `true` | — | `true` 时 Tab 插入空格 |
| `auto_indent` | bool | `true` | — | Enter 时沿用上一行缩进 |
| `show_line_numbers` | bool | `true` | — | 是否显示行号槽(练习模式下这个槽会被染色当模式指示器,关掉就少一重提示) |
| `panel_height` | int | `10` | 3–100 | 底部面板区高度(行)。运行时可用 `Alt-=` / `Alt--` 临时调 |
| `tick_ms` | int | `60` | 10–1000 | 主循环空转周期(毫秒),影响 ghost 触发与状态刷新的精度 |

一共 **31 个可配字段**。`config.h` 里另有 `source_path` 和 `warnings` 两个成员,
那是程序内部的元信息(实际加载自哪 / 解析告警),不是配置项,写进 JSON 会被当成未知键。

### 2.4 环境变量覆盖

取值优先级:**内置默认 < 配置文件 < 环境变量**。

| 环境变量 | 覆盖谁 |
|---|---|
| `CPPIDE_API_KEY` | `api_key` |
| `DEEPSEEK_API_KEY` | `api_key`(`CPPIDE_API_KEY` 优先) |
| `CPPIDE_MODEL` | `model` |
| `CPPIDE_BASE_URL` | `base_url` |
| `CPPIDE_CONFIG` | 配置文件路径(被 `--config` 压过) |

临时换一把 key / 换个模型试一下,不用改文件:

```sh
CPPIDE_MODEL=某个官方模型名 DEEPSEEK_API_KEY=... ./cppide main.cpp
```

### 2.5 配错了会怎样(降级契约)

**`cppide` 不会因为配置有问题而崩溃或拒绝启动**,一条都不会:

| 情况 | 行为 |
|---|---|
| 配置文件不存在 | 全部用默认值 + 一条中文 warning,AI 关闭,编辑/编译/运行照常 |
| JSON 语法错误 | 全部用默认值 + **带行号列号**的 warning,不中断启动 |
| 单个字段类型不对(如 `"tab_width": "four"`) | 该字段用默认值 + 一条 warning,**其余字段照常生效** |
| 字段值越界(如 `"panel_height": 999`) | 夹到边界 + 一条 warning |
| 未知的顶层键 | 一条 warning(以 `_` 开头的键视为注释,不告警) |
| `api_key` 为空 | AI 关闭:worker 线程**根本不创建** |

所有 warning 会在启动时进【AI】面板(`Alt-3` 看),`--doctor` 里也会全部列出。

---

## 3. 不配 key(或不配 model)也完全能用

这是硬性设计,不是「凑合能用」:

- **照常可用**:打开 / 编辑 / 保存文件、C/C++ 语法高亮、撤销重做、查找、跳行、
  `Ctrl-B` 编译、`Ctrl-N`/`Ctrl-P` 跳编译错误、`Ctrl-R` 运行、喂 stdin、看 stdout / 退出码。
- **唯一的差别**:状态栏 AI 状态格显示「未配置」;按 `Ctrl-A` 或切模式时,
  【AI】面板里出现一条中文提示,告诉你去哪个文件填 `api_key` 或 `model`。不弹窗、不阻塞、不崩。
- **`model` 留空与不填 key 等价**:`model` 没有内置默认值(代码里不写死任何模型名),
  空着就按「未完整配置」处理 —— 同样不起 worker、不发请求、只给提示。
- 技术上:`api_key` 或 `model` 为空时 `AiService::enabled()` 返回 false,worker 线程
  **不会被创建**,所以不存在「没配好却在后台发请求」这种事。

---

## 4. 快捷键全表

以 `src/keys.cpp` 的绑定表为准(下表已逐条核对)。程序内按 **`F1`** 可以看同一份表的浮层版本,
`./cppide --help` 也会把它整份打出来。

### 4.1 移动

| 键 | 动作 |
|---|---|
| `←` `→` `↑` `↓` | 左 / 右 / 上 / 下移动一格 |
| `Alt-←` / `Alt-→` | 按词左移 / 右移 |
| `Home` / `End` | 行首 / 行尾 |
| `PgUp` / `PgDn` | 上翻 / 下翻一页 |
| `Alt-<` / `Alt-Home` / `Shift-Home` | 跳到文件开头(三个键等效) |
| `Alt->` / `Alt-End` / `Shift-End` | 跳到文件末尾(三个键等效) |

> 为什么文件首尾不是 `Ctrl-Home`/`Ctrl-End`:那两个键的转义序列各终端各不相同,
> ncurses 多半认不出来。用最稳的 Esc 前缀单字符(emacs 的 `Alt-<`/`Alt->` 习惯)当主绑定。

### 4.2 编辑

| 键 | 动作 |
|---|---|
| `Enter`(含小键盘 Enter) | 换行,带自动缩进(`auto_indent`) |
| `Backspace`(`127` 或 `KEY_BACKSPACE`) | 向左删除 |
| `Delete` | 向右删除 |
| `Tab` | **有已生成完的 ghost 就接受它,否则缩进** |
| `Shift-Tab` | 反缩进 |
| `Ctrl-U` / `Ctrl-W` | 撤销 / 重做 |
| `Ctrl-K` | 剪切当前行(**连续按会累积**成多行) |
| `Ctrl-D` | 复制当前行 |
| `Ctrl-V` | 粘贴行 |

`Ctrl-K`/`Ctrl-D`/`Ctrl-V` 用的是**编辑器内部**的行剪贴板,不是系统剪贴板 —— 见 [§10 已知限制](#10-已知限制)。

### 4.3 文件 / 编译 / 运行

| 键 | 别名 | 动作 |
|---|---|---|
| `Ctrl-O` | `F2` | 保存(nano 风格 write out)。没有文件名时会弹「另存为」输入行 |
| `Ctrl-X` | `F10` | 退出。有未保存修改会先问 `y`=退出 / `n`=取消 / `s`=保存并退出(**默认不退出**,`Enter` 不等于确认) |
| `Ctrl-B` | `F5` | 编译当前文件(脏缓冲区会**先自动保存**再编译) |
| `Ctrl-R` | `F6` | 运行。缓冲区脏 / 二进制不存在 / 二进制比源码旧时会**先自动编译**,编译失败就不运行 |
| `Ctrl-E` | `F7` | 编译并运行 |
| `Ctrl-N` | — | 下一个编译诊断(无可跳诊断时退化为「查找下一处」) |
| `Ctrl-P` | — | 上一个编译诊断(同上,退化为「查找上一处」) |

### 4.4 AI

| 键 | 别名 | 动作 |
|---|---|---|
| `Ctrl-T` | `F8` | 切换 AI 模式(练习 ⇄ 写代码) |
| `Ctrl-A` | `F4` | 立即请求 AI。练习模式下先在【AI】面板打开提问输入行 |
| `Tab` | — | 接受 ghost(仅写代码模式下可能有 ghost) |
| `Esc` | — | 丢弃 ghost / 取消在飞的 AI 请求 |

> `F4` 这个别名是给 tmux 用户的:tmux 的默认 prefix 常配成 `Ctrl-B` 或 `Ctrl-A`,
> 那种情况下 `Ctrl-B`(编译)和 `Ctrl-A`(问 AI)会被 tmux 吃掉,请改用 `F5` / `F4`。

### 4.5 查找 / 跳转 / 其它

| 键 | 动作 |
|---|---|
| `Ctrl-F` | 查找(打开输入行) |
| `F3` | 查找下一处(还没查过时等于打开查找输入行) |
| `Ctrl-G` | 跳到行号 |
| `Ctrl-L` | 强制全屏重绘(**顺便把高亮缓存整个丢掉重算** —— 颜色不对时按它) |
| `F1` | 帮助浮层(全键表)。80x24 下共 4 页:`PgDn`/`↓` 下一页、`PgUp`/`↑` 上一页(到头回绕),再按一下 `F1` 也翻页;`Esc` 或 `F1` 关闭 |
| `Esc` | 依次取消:ghost → 输入提示行 → 面板焦点 → 在飞 AI 请求 |

### 4.6 面板

| 键 | 动作 |
|---|---|
| `Alt-1` / `Alt-2` / `Alt-3` / `Alt-4` | 聚焦【编译】/【运行】/【AI】/【输入】面板 |
| `Alt-0` | 折叠 / 展开整个面板区 |
| `Alt-=` / `Alt--` | 面板加高 / 减高 |
| `Tab`(面板聚焦时) | 切到下一个面板标签 |
| `F9` | 同上(不依赖焦点的直达绑定) |

**面板聚焦时**:

| 键 | 行为 |
|---|---|
| `↑` `↓` `PgUp` `PgDn` | 滚动;在【编译】面板里是移动行光标(用来挑诊断) |
| `Home` / `End` | 跳到面板首行 / 末行 |
| `Enter` | 【编译】= 跳到该诊断对应的源码行;【AI】= 打开提问输入行;【运行】无动作 |
| `Esc` | 【运行】面板且有程序在跑 → **终止它**;否则把焦点交回编辑区 |
| 打字 | 只在【输入】面板生效(它内部就是一个支持撤销的 TextBuffer);其它面板里打字**不会**落到代码缓冲区 |

### 4.7 为什么有些键一个都不绑

| 键 | 原因 |
|---|---|
| `Ctrl-C` | SIGINT。绑了就等于跟「中断」抢语义,而且 tmux / ssh 层可能有自己的映射 |
| `Ctrl-Z` | SIGTSTP,挂起后回到前台终端状态难恢复 |
| `Ctrl-S` / `Ctrl-Q` | XON/XOFF 流控 —— ssh 下的经典「屏幕冻住了」陷阱 |
| `Ctrl-\` | SIGQUIT + core dump |
| `Ctrl-Y` | macOS / BSD 的 `VDSUSP` |
| `Ctrl-H`(8)、`Ctrl-J`(10) | 它们是 Backspace / Enter 的别名字节,不作为独立快捷键 |

虽然 ncurses 的 `raw()` 会清掉 `ISIG` / `IXON`、把这些字节变成普通输入,
但终端仿真器和 tmux 可能在到达程序之前就截走它们,而且用户的肌肉记忆会带来意外,所以一个都不绑。

`Ctrl-M`(13)和 `Ctrl-I`(9)是例外:它们**就是** Enter 和 Tab 本身,必须有动作。
「不绑」在这里的意思是「不把它们当作独立的 Ctrl 快捷键,键名永远显示 `Enter` / `Tab`」。

### 4.8 F 键为什么只是别名

**macOS 上 `F1`–`F12` 默认被系统媒体键(亮度、音量、Mission Control…)占用。**
要让它们发送真正的功能键,去
`系统设置 → 键盘 → 键盘快捷键 → 功能键 → 勾选「将 F1、F2 等键用作标准功能键」`
(旧版 macOS:`系统偏好设置 → 键盘 → 勾选「将 F1、F2 等键用作标准功能键」`)。
不想改系统设置就按住 `fn` 再按 F 键。

所以每个常用动作都同时有 `Ctrl` 绑定和 F 键别名,任一路可用。
完整别名映射:`F1`=帮助、`F2`=保存、`F3`=查找下一处、`F4`=问 AI、`F5`=编译、`F6`=运行、
`F7`=编译并运行、`F8`=切模式、`F9`=下一个面板标签、`F10`=退出。

### 4.9 Alt 在 macOS 终端上怎么发

macOS 的终端**不发 Meta 位**,Alt 组合只能靠 **Esc 前缀**表达
(按 `Alt-1` 实际发出 `ESC` `1` 两个字节)。cppide 正是按这个约定实现的:
收到 `27` 之后立刻做一次非阻塞读 —— 有键就合成 `Alt-<键>`,没键就是裸 `Esc`。
所以你要做的只是让终端把 Option 发成 Esc 前缀:

- **Terminal.app**:`设置 → 描述文件 → 键盘 → 勾选「将 Option 键用作 Meta 键」
  (Use Option as Meta key)`。勾上之后 Option 组合就是标准的 Esc 前缀。
  不勾时 Option 可能被用来输入特殊字符(`Option--` 变成一个连字符而不是 `Alt--`),那样就用不了。
- **iTerm2**:`Preferences → Profiles → Keys → Left/Right Option Key` 设为 **`Esc+`**。
- **懒得配 / 配不通**:先按一下 `Esc` 松开,再按目标键 —— 完全等价于 Alt 组合
  (`Esc` 然后 `3` = `Alt-3`,`Esc` 然后 `←` = `Alt-←`)。**这条路在任何终端上都成立**,
  因为程序本来就是这么实现的。
- **诊断**:某个 Alt 组合没反应时,那个终端很可能把 Option 配成了 CSI 修饰符
  (`Alt-←` 发 `ESC[1;3D`),ncurses 认不出来 —— 见 [§9](#9-故障排查)。

---

## 5. 两种 AI 模式

**启动时默认是「练习模式」**(更安全的那一档)。`Ctrl-T` / `F8` 切换。

### 5.1 练习模式

- **只在【AI】面板里输出中文思路提示**:可能的算法方向、时间/空间复杂度估计、
  当前代码的卡点或潜在 bug 方向、下一步该想什么。
- **绝不向代码缓冲区插入任何东西**。此模式下 **`Tab` 只是缩进**。
- 没有自动触发 —— 你不按 `Ctrl-A`,它一个请求都不发。
- `Ctrl-A`(或聚焦【AI】面板后按 `Enter`)会打开一个提问输入行:
  你可以用中文问「这题该往哪个方向想」「我这个 dp 状态是不是漏了」。
  提问内容会和当前代码一起发过去。

### 5.2 写代码模式

- 光标处停顿 `ghost_delay_ms`(默认 500ms)后自动请求;两次自动请求至少隔
  `ghost_min_interval_ms`(默认 1200ms)。缓冲区全是空白时不发请求。
- **同一次停顿只会自动请求一次(省钱的硬保证)**:把编辑器挂在写代码模式上去想题、
  去吃饭、切到别的窗口,都**不会**继续计费 —— 实测 30 秒完全不碰键盘只发出 1 次请求。
  想要下一次自动补全,就再打字或移动光标(任何一次 `noteActivity`);或者直接按
  `Ctrl-A` 手动请求(手动永远不受任何限流约束)。
  切换模式(`Ctrl-T`)与 `Esc` 丢弃 ghost 都**不**重新开启额度,免得白花钱。
- 返回结果以**灰色 ghost text** 显示在光标之后,最多 `ghost_max_lines` 行。
- **`Tab` 接受**(整段插入到光标位置)/ **`Esc` 丢弃**。
- ghost 还在生成中时按 `Tab` 不会接受半截内容,只会缩进 + 提示「AI 正在生成…」。
- 你一按键就 `noteActivity()`,在飞的旧请求当场作废 —— 不会出现「打完两行字之后
  三秒前的建议才飘上来」。

### 5.3 界面上怎么看出当前模式(三重冗余)

1. **状态栏最左格的模式徽章**:反色显示「练习模式」/「写代码模式」。
2. **练习模式下整个行号槽被染成同一强调色**(黄底),眼睛盯着代码时余光也能看见。
   (前提是 `show_line_numbers` 为 true。)
3. 切换时状态栏和【AI】面板各留一条中文消息。

### 5.4 「练习模式不插代码」是结构性保证,不是散落的 if 判断

这条是需求里的硬要求,所以实现上不靠「记得每个地方都判一下模式」,而是靠三条结构性事实:

1. **缓冲区只有一个 AI 入口。** AI 产生的文本能到达 `TextBuffer::insert` 的路径**只有一条**:
   `Editor::acceptGhost()`。`ai.cpp` / `aihttp.cpp` / `ui.cpp` 都不持有可写的 `TextBuffer&`。
   这个唯一咽喉点开头就检查 `ghost_.sink != AiSink::GhostText` 并直接返回 false。
2. **练习模式的回复不可能变成 ghost。** `AiSink` 在**请求创建时**由唯一函数
   `AiService::sinkFor(mode)` 定死、写进请求、随事件一路带回。练习模式的 sink 是
   `PanelOnly`,它在 `App::onAiDone` 的 `switch` 里只有「往【AI】面板追加」这一个分支。
   就算将来有人写错逻辑,退化结果是「没有 ghost」,**永远不是「代码被插入」**。
3. **切模式即销毁在飞工作。** `setMode()` 先把 generation 序号 `+1`,
   所有已在网络上的回复在被检视之前就已作废;写代码模式 → 练习模式还会额外
   `clearGhost()`。**不存在**「切到练习模式之后,写代码模式的回复才落地」的时间窗。

于是 `Tab` 的处理函数里**一行模式判断都没有**:
`if (ghost 已生成完) 接受; else 缩进;`。练习模式永远不会有 ghost,所以 Tab 自动就只是缩进。

---

## 6. 编译与运行

### 6.1 编译

`Ctrl-B` / `F5`。行为:

1. 缓冲区脏就**先自动保存**(编译的必须是磁盘上的内容);保存失败就不编译并提示。
2. 【编译】面板清空,第一行回显完整命令(`$ c++ -O2 -std=c++17 -Wall -o /tmp/cppide-… main.cpp`)。
3. 编译在**后台线程**跑,UI 不卡。超时上限 `compile_timeout_ms`(默认 30s),
   超时会连同整个进程组一起杀掉。
4. 完成后标签变成 `[编译 2]`(数字是错误数);**只有编译失败会自动聚焦编译面板**,
   成功只置一个未读标记 —— 永不在你打字时抢走焦点。

按扩展名分派:

| 扩展名 | 命令 |
|---|---|
| `.c` | `cc` + `cflags` + `-o <临时二进制> <源文件>` |
| `.cpp` `.cc` `.cxx` `.C` `.c++` | `cxx` + `cxxflags` + `-o <临时二进制> <源文件>` |
| `.h` `.hpp` `.hh` `.hxx` `.h++` `.hp` | **只做语法检查**:`cxx` + `cxxflags` + `-fsyntax-only -x c++ -`,并从 stdin 喂 `#include "<绝对路径>"`。没有可运行的产物 |

(除 `.c`(C)与 `.C`(C++)这一对之外,扩展名判定**不区分大小写**;`.inc` 不在支持清单里。
「是否只做语法检查」与「哪些扩展名受支持」是同一处判定 —— `TextBuffer::langFromPath`。)

> 头文件走 stdin TU 而不是直接 `c++ -fsyntax-only foo.h`,是因为后者会无条件报
> `warning: #pragma once in main file`,而那条警告**没有任何 `-Wno-*` 能关掉**。
> 走 stdin 既没有假警告,诊断里的文件名/行号也仍然指向真实的头文件。

编译子进程固定带 `LC_ALL=C`。这不是可选项:中文 locale 下编译器输出的是 `错误:`/`警告:`,
诊断解析器一条都认不出来,你会看到「编译失败,0 个错误」而且一个诊断都跳不过去。

临时二进制放在 `$TMPDIR/cppide-<源文件绝对路径的哈希>-<文件名主干>`
(`--doctor` 会打印实际的 `TMPDIR`)。不污染你的源码目录,不同文件互不覆盖。

### 6.2 编译错误怎么跳行

三条路都通向同一处:

- **`Ctrl-N` / `Ctrl-P`** —— 下一个 / 上一个诊断,光标直接落到出错行列,面板里对应行同步高亮。
  只在「诊断的文件名和当前缓冲区一致、且有行号」时才算可跳。
- **聚焦【编译】面板(`Alt-1`)后用 `↑`/`↓` 挑一条,`Enter` 跳过去。**
- 没有可跳诊断时,`Ctrl-N` / `Ctrl-P` 自动退化成「查找下一处 / 上一处」。

### 6.3 运行

`Ctrl-R` / `F6`(必要时先自动编译),或 `Ctrl-E` / `F7`(总是先编译)。

- **stdin 两种喂法**:
  1. **【输入】面板**(`Alt-4` 聚焦)——它内部就是一个支持撤销的编辑器,直接把测试数据敲/贴进去。这是默认方式。
  2. **`stdin_file` 配置项** —— 填一个文件路径,每次运行时重读该文件;
     此时【输入】面板只作展示,【运行】面板会打一行 `(stdin 来自文件 …)` 提示。
- **stdout 在【运行】面板**(`Alt-2`),常规色;**stderr 同一面板,红色**。
- **退出码 / 信号 / 耗时 / 是否截断** 在面板**末行**(meta 行),同时也进状态栏。
  形如 `退出码 0 · 用时 12ms`、`超时 5000ms,已终止`、`被信号 SIGSEGV 终止 · 用时 3ms`、
  `无法启动:<原因>`。
- 子进程的工作目录 = **你启动 cppide 的目录**(不是源文件所在目录,也不是 `$TMPDIR`)。
  所以程序里 `fopen("data.txt")` 是相对于你敲命令的地方。

### 6.4 程序跑飞了怎么办(三道防线)

1. **`Esc`**:聚焦【运行】面板(`Alt-2`)后按 `Esc` → 立即终止(会 `kill(-pgid)`,
   连子孙进程一起杀)。终端里 `Ctrl-C` 不可用(raw 模式且不绑它),**这是唯一的手动逃生口**。
2. **`run_timeout_ms`**(默认 5000ms)超时自动杀。
3. **输出上限**:stdout / stderr 各 `run_output_limit`(默认 1 MiB)后截断;
   再往上到硬上限(`4×limit`,至少 8 MiB)直接杀进程 —— 防「死循环里 printf」把内存吃光。

另外:退出编辑器时如果还有在飞作业,会被直接 SIGKILL,不会让你等满 30 秒超时。

> 编译作业**没有** `Esc` 逃生口(【编译】面板的 `Esc` 是「交回焦点」),
> 卡住的编译只能等 `compile_timeout_ms` 兜底。

---

## 7. 命令行选项

```
cppide [选项] [文件]
```

不给文件名就打开一个未命名的空缓冲区(`Ctrl-O` 会提示另存为)。
给一个**还不存在**的路径,则当**新文件**打开(父目录必须已存在),`Ctrl-O` 直接存盘。
**一次只能打开一个文件。**

启动前的检查(任一条不过 → 中文报错 + **退出码 2**,绝不带着坏状态进 TUI):

| 情况 | 报错 |
|---|---|
| 给了两个及以上文件 | `一次只能打开一个文件…` |
| 扩展名不是 C/C++ | `不支持的文件类型:…` |
| 路径是个目录 | `… 是一个目录,不是文件。` |
| 新文件的父目录不存在 | `目录不存在:…(无法在其中新建 …)` |
| 不是普通文件(命名管道 / 设备 / 套接字) | `… 不是普通文件…`(否则 `open()` 一个 fifo 会永久挂死在启动阶段) |
| 存在但读不了 | `无法读取 …:<原因>` |
| 未知选项 | `未知选项 --xxx。请用 cppide --help 查看用法。` |

另外:**不在交互式终端里跑**(stdin 或 stdout 不是 tty,例如被重定向 / 在 CI 里)会报
「当前环境不是交互式终端…」并以**退出码 3** 退出。`--help` / `--version` /
`--print-config` / `--doctor` 四个非交互命令不受此限制,可以随便重定向。

| 选项 | 作用 |
|---|---|
| `--config <PATH>` | 指定配置文件,优先于 `CPPIDE_CONFIG` 和默认路径 |
| `--print-config` | 把带中文注释的示例配置打到 stdout。`./cppide --print-config > ~/.config/cppide/config.json` 就是标准的初始化方式 |
| `--doctor` | 非交互自查(见下),然后退出 |
| `-h` / `--help` | 用法 + 配置文件位置 + **整份快捷键表** |
| `-V` / `--version` | 版本号(`cppide 0.1.0`) |

### `--doctor` 能自查什么

```sh
./cppide --doctor
./cppide --config config.sample.json --doctor      # 自查某份具体配置
```

- **终端**:stdin/stdout 是否 tty、`TERM`、`LANG` / `LC_ALL` / 生效的 `LC_CTYPE`、
  ncurses 版本、`MB_CUR_MAX`(> 1 才能输出中文)、`sizeof(wchar_t)`、
  terminfo 里的 `COLORS`、最小可用尺寸(60x16)。
- **配置**:`--config` 是否指定、查找路径、文件是否存在、**实际加载自哪**、
  致命解析说明、`api_key` 是否已配(**只显示「已配置(已隐去)」,绝不回显 key 本身**)、
  `model`、拼好的 chat URL、`stream`、ghost 触发参数、AI 超时、
  `tick_ms` / `panel_height` / `tab_width`、`stdin_file`、以及**全部 warnings**。
- **编译器**:`cc` 和 `cxx` 分别解析到哪个绝对路径(找不到会明说)、实际生效的
  `cflags` / `cxxflags`、两个超时、`TMPDIR`。
- **结论**:两行 —— 编译运行是否就绪、AI 是否启用。

`--doctor` 是纯非交互的(不进按键回显)。想看某个键的**实际键码**,请在真实终端里启动
`cppide` 后按 `F1` 看键表,或参考 [§9](#9-故障排查) 的 Alt / 方向键条目。

---

## 8. 项目结构

```
Makefile
README.md
config.sample.json     --print-config 的原样输出,可直接当模板
src/                   15 个头 + 15 个 .cpp
tests/                 test_*.cpp 单元测试,make tests 逐个跑
```

| 文件 | 职责 |
|---|---|
| `src/main.cpp` | 进程入口:`setlocale` / 忽略 SIGPIPE / curl 全局初始化 / `endwin` 崩溃安全网 / 解析 argv / `--help` `--version` `--print-config` `--doctor` |
| `src/app.h` `app.cpp` | `AppModel` + `App`:主循环、按键分派、事件排空、编译/运行/AI 的编排 |
| `src/ui.h` `ui.cpp` | ncurses 初始化与**全部**绘制(编辑区 / 行号槽 / ghost / 面板 / 状态栏 / 提示行 / 帮助浮层)。**curses.h 只允许出现在这里** |
| `src/editor.h` `editor.cpp` | `Editor`:光标、视口、编辑操作、行剪贴板、查找、Ghost 状态与**唯一的** `acceptGhost()` |
| `src/textbuf.h` `textbuf.cpp` | `TextBuffer`:行数组 + 逆操作日志撤销、文件读写、语言判定 |
| `src/highlight.h` `highlight.cpp` | `Highlighter`(纯函数,单行扫描)+ `HighlightCache`(行首状态缓存) |
| `src/panel.h` `panel.cpp` | `Panel`(输出面板:行、未读标记、错误数、滚动、行光标)+ `StdinBuffer` |
| `src/proc.h` `proc.cpp` | `runProcess`(fork+exec+poll,带超时/输出上限/进程组)+ `Runner`(后台作业线程) |
| `src/build.h` `build.cpp` | `Diag` / `Builder`:编译命令装配、诊断解析、临时二进制路径、`binaryStale` |
| `src/ai.h` `ai.cpp` | `AiMode` / `AiSink` / `AiRequest` / `AiService`:worker 线程、prompt 组装、上下文截断、generation 取消、代码围栏剥离 |
| `src/aihttp.h` `aihttp.cpp` | `HttpChat`(libcurl)+ `SseParser`(流式解析)+ 中文错误映射。**要适配非 OpenAI 兼容的 API 只需改这一个文件** |
| `src/config.h` `config.cpp` | `Config` + `ConfigLoader`:路径推导、加载、clamp、降级、`sampleJson()` |
| `src/json.h` `json.cpp` | `mj::Value`:极简 JSON。链式下标对缺失键/越界/类型不符一律返回 Null 而不抛 |
| `src/keys.h` `keys.cpp` | `Action` 枚举 + 全局绑定表 + `Ctrl()`/`Alt()` + 帮助浮层文本。**本 README 的键表就是这里的镜像** |
| `src/mailbox.h` | `Mailbox<T>`:线程安全队列(header-only),UI 线程与 worker 线程之间的唯一通道 |
| `src/util.h` `util.cpp` | 字符串 / 路径 / 文件 / 时间 / UTF-8 / 显示宽度(`wcwidth`) |

---

## 9. 故障排查

### macOS 首次编译报错

| 症状 | 原因与修法 |
|---|---|
| `error: use of undeclared identifier 'get_wch'` / `add_wch`,或 `cchar_t` 未声明 | macOS 自带的 ncurses 头没主动给出宽字符原型。**打开 `Makefile` 里 Darwin 分支下那行已经写好的注释**:`# CPPFLAGS += -D_XOPEN_SOURCE_EXTENDED` → 去掉 `#` |
| `static_assert failed: KEY_SHOME == 391`(或 `KEY_SEND` / `KEY_BTAB`)在 `make tests` 时炸 | macOS 自带 curses 的这三个键码与 Debian ncurses 不同。`src/keys.cpp` 里那几个 `kNcS*` / `kNcBTab` 常量是手抄的镜像,把它们改成 `make tests` 报出的实际值即可(方向键、F 键、Home/End/PgUp/PgDn/Delete 自 BSD curses 起就没变过,风险只集中在 `S*` 系列)。这个 `static_assert` 存在的意义就是让问题在编译期就红,而不是运行期表现成「某个键没反应」 |
| `ld: library not found for -lcurl` | 装 Command Line Tools:`xcode-select --install` |

### 中文乱码 / 宽度错位 / 光标跳位

- **`--doctor` 里 `MB_CUR_MAX` 是 1** → locale 没设成 UTF-8。
  `export LANG=zh_CN.UTF-8`(或 `en_US.UTF-8`),或在 Terminal.app 的
  `设置 → 描述文件 → 高级 → 字符编码` 里确认是 Unicode (UTF-8)。
  程序会在 `initscr()` 之前 `setlocale(LC_ALL, "")`,但它只能沿用你环境里的 locale。
- **Linux 上中文全是问号或半角** → 链错库了。确认 `Makefile` 走的是 `-lncursesw` 分支,
  `make clean && make` 重来一遍。
- **中文字符宽度对不上**(emoji、国旗、ZWJ 表情):这是 `wcwidth` 层面无解的问题,
  所有终端编辑器都有。用 `Ctrl-L` 重绘可以消掉残留的半个字符。

### 方向键 / Alt / Backspace 失灵

| 症状 | 原因与修法 |
|---|---|
| 方向键打出 `[A` `[B` 之类的字符 | `TERM` 不对。`--doctor` 看一眼;应该是 `xterm-256color` / `screen-256color` 之类。`export TERM=xterm-256color` 再试。tmux 里要在 `~/.tmux.conf` 设 `set -g default-terminal "screen-256color"` |
| `Alt-1` / `Alt-←` 没反应 | 终端把 Option 配成了 CSI 修饰符(`ESC[1;3D`)或「输入特殊字符」。按 [§4.9](#49-alt-在-macos-终端上怎么发) 改配置;或改用「先按 `Esc` 松开,再按目标键」的两步法 |
| `F5` / `F8` 触发了亮度或音量 | macOS 的 F 键被媒体键占用。按 [§4.8](#48-f-键为什么只是别名) 勾选「用作标准功能键」,或按住 `fn`,或改用 `Ctrl-` 绑定 |
| Backspace 完全没反应 | 你的终端把 Backspace 配成了「发送 Control-H」(iTerm2 里可配)。cppide **刻意不绑 `Ctrl-H`**,请把终端改回发送 `Delete`(`0x7F`) |
| `Ctrl-B` / `Ctrl-A` 被吃掉 | tmux 的 prefix。改用 `F5`(编译)/ `F4`(问 AI) |

### 颜色不对

- `--doctor` 里 `COLORS(terminfo)` 是 `-2` 是**正常的** —— 那只表示还没 `initscr()`。
  真正的颜色数要在 TUI 里才拿得到。
- 颜色数不足(8 色终端)会自动退化到单色/粗体方案,不会崩。
- **某几行颜色暂时不对**:高亮缓存的失效起点是近似值。**按 `Ctrl-L`** 会把缓存整个丢掉重算,
  这是确定有效的修复手段。

### 编译器找不到

`--doctor` 的 `[编译器]` 段会直说 `cc = cc  -> 未找到!`。

- macOS:`xcode-select --install`。
- 想指定具体编译器:配置文件里把 `cc` / `cxx` 写成绝对路径,例如
  `"cxx": "/opt/homebrew/bin/g++-14"`。
- 想换标准或参数:改 `cflags` / `cxxflags`。比如 ICPC 常见的
  `"cxxflags": ["-O2","-std=c++20","-Wall","-Wextra"]`。

### AI 报错都是什么意思

错误文案全部是中文,出现在【AI】面板(`Alt-3`)和状态栏。**任何错误文案里都不会包含你的 api_key。**

| 你看到的 | 含义 / 该怎么办 |
|---|---|
| `AI 未配置:请在 … 里填写 api_key` | 压根没配。见 [§2.2](#22-怎么创建) |
| `AI 未配置:请在 … 里填写 model` | key 填了、`model` 还空着。`model` 没有内置默认值,按 DeepSeek 官方文档填上即可;在此之前 AI 不会发出任何请求(编辑/编译/运行照常) |
| `API key 无效或已过期` | HTTP **401 / 403**。key 抄错了、被吊销了,或者环境变量里有一把旧 key 在压着配置文件(`--doctor` 看 `api_key` 那行) |
| `余额不足或请求过于频繁` | HTTP **402 / 429**。去官方控制台看余额;429 就是限流,把 `ghost_min_interval_ms` 调大 |
| `接口不存在(HTTP 404),检查 base_url 与 chat_path` | 两者拼出来的 URL 不对。`--doctor` 会打印拼好的完整 `chat url`,拿它跟官方文档比 |
| `请求被拒绝(HTTP 400/422),检查模型名与参数` | 最常见的原因是 **`model` 填了一个不存在的模型名**。按 DeepSeek 官方文档改 `model` |
| `服务端错误(HTTP 5xx)` | 对方的问题,过一会儿再试 |
| `请求超时` | 超过了 `ai_timeout_ms`(默认 20s)。网络慢就调大它;也可能是 `max_tokens_*` 给太大导致生成太久 |
| `无法解析域名,检查网络或 base_url` | DNS 挂了或 `base_url` 域名拼错。注意:**DNS 解析阶段打不断超时**,见 [§10](#10-已知限制) |
| `无法连接服务器,检查网络或 base_url` | 连不上。防火墙 / 代理 / 断网 |
| `无法解析代理域名,检查 http_proxy 设置` | 环境里的 `http_proxy` / `https_proxy` 有问题 |
| `TLS 连接失败` / `TLS 证书校验失败` / `找不到可用的 CA 证书` | 系统证书链有问题,或者中间有个做 TLS 拦截的代理 |
| `base_url 非法或协议不支持` | `base_url` 不是 `http://` / `https://` 开头 |
| `服务端返回空响应` / `响应被截断` | 连接中途断了。重试;持续出现的话把 `stream` 设成 `false` 看看是不是 SSE 解析的问题 |
| `已取消` | 你按了 `Esc` 或期间改了缓冲区。**不是错误** |

出现任何一种错误,编辑器都不崩、不卡输入 —— 网络请求全在 worker 线程里,UI 线程一次都不会阻塞在它上面。

---

## 10. 已知限制

写在这里的都是**已知且有意接受**的取舍,不是待修的 bug。

### 编辑 / 高亮

1. **原始字符串字面量 `R"delim(...)"` 跨行时高亮是近似的。** 自定义 delim 且体内含 `)"`
   会让高亮提前结束。单行状态只有 1 字节,放不下完整的 delim 匹配。
2. **未闭合的字符串 + 行尾 `\`** 只按普通续行处理(下一行按代码扫描),不是「字符串继续」。
3. **数字后缀一律吞进数字**:`1else` 会整体染成数字色(合法 C++ 里不存在这种写法)。
4. **未闭合的单引号**会把行尾染成字符字面量色。观感问题。
5. **没有选区。** 块操作请用行级快捷键(`Ctrl-K` 剪切行、`Ctrl-D` 复制行、`Ctrl-V` 粘贴行;
   `Ctrl-K` 连按会累积多行)。
6. **`Ctrl-D`/`Ctrl-K`/`Ctrl-V` 用的是编辑器内部剪贴板,不通系统剪贴板。**
   要复制到系统剪贴板,请用**终端自身的鼠标选择 + `Cmd-C`**;从系统剪贴板粘贴用 `Cmd-V`
   (终端会把它当键盘输入送进来,`Ctrl-V` 与之不冲突)。
7. **词移动把所有 `>= 0x80` 的码点算作同一类词字符**,所以一整段中日韩文本会被
   `Alt-←`/`Alt-→` 当成一个词跨过。刻意不做 Unicode 断词。
8. **CRLF 文件保存后会变成 LF。** 加载时行尾的 `\r\n` 一律规范化为 LF
   (行**中间**孤立的 `\r` 原样保留,二进制安全),保存时一律写 LF。
   `TextBuffer` 里没有保存换行风格的地方,所以还原不了。
9. `saveFile()` 只 `fsync` 文件本身、不 `fsync` 目录项;掉电语义弱于严格的原子写。

### 编译 / 运行

10. **只编译当前打开的单个文件**(需求边界)。没有多文件链接、没有工程。
11. **诊断解析对「路径里含 `:<数字>:`」会误判行号。** 单文件场景下路径通常来自你自己敲的参数,可接受。
12. **子进程的工作目录是你启动 cppide 的目录**,不是源文件所在目录。程序里的相对路径按这个算。
13. **编译作业没有手动逃生口**,只能等 `compile_timeout_ms` 兜底(默认 30s)。运行作业有 `Esc`。
14. 退出编辑器时在飞的编译/运行作业会被直接 SIGKILL,极端情况下输出可能不完整。
15. `.h` / `.hpp` 一律用 **C++ 前端**(`cxx` + `cxxflags`)做语法检查,纯 C 头文件也是。
    这是有意的近似。
    - **`Ctrl-R` 跑起来的程序禁用了 core dump**(fork 之后 `setrlimit(RLIMIT_CORE, 0)`)。
      需求明确「不做调试器集成」,那个 core 没人会用,只会在你的项目目录里堆垃圾。
      代价:想拿 core 事后调试的话,得在 cppide 之外直接跑那个程序。
      禁 core **不影响**信号汇报 —— 段错误照样显示「被信号 SIGSEGV 终止」。

### AI

16. **假定 DeepSeek API 与 OpenAI 的 chat completions 协议兼容、支持 SSE 流式返回。**
    这是一条假设,不是已验证的事实。**若实际不兼容,改 `src/aihttp.cpp` 一个文件即可**
    —— 请求组装、响应解析、SSE 分帧、错误映射全在里面,上层看不到差别。
17. **模型名不做任何校验、不做任何能力假设,代码里也没有任何内置模型名。** `model` 的默认值是
    **空串**,空着就等于 AI 未配置(不发请求、状态栏显示 `AI 未配置`,编辑/编译/运行照常)。
    **请按 DeepSeek 官方文档上的模型名填 `model`。** 本 README 不断言任何具体模型名有效。
18. **DNS 解析阶段无法被超时打断**(libcurl 的线程化解析器限制:
    `Curl_resolver_kill()` 会 join 解析线程,`CURLOPT_TIMEOUT_MS` 打不断一次正在进行的
    `getaddrinfo`)。**首次请求最慢可能要等满系统 DNS 超时**(开发机上冷缓存实测约 3.2s,
    热缓存约 2ms)。两条硬保证不受影响:请求一定会返回;**它只发生在后台 worker 线程,不卡 UI**。
19. **流式模式下不显示 token 计数** —— 本实现不解析流式响应末帧的 `usage`,
    `prompt_tokens` / `completion_tokens` 在 `stream: true` 下保持 0。非流式路径正常。
20. **不做会话持久化。** 关掉编辑器,【AI】面板的全部内容和模式状态即清空。
21. `ai_max_context_lines` 是**行数**上限而不是字节上限。单行超长(比如一行 1 MB 的压缩代码)
    时 prompt 仍可能很大。
22. 写代码模式的 markdown 代码围栏剥离是**行级**的:```` ```cpp int a=1;``` ```` 这种
    「同一行内开闭围栏」会被整行丢掉。有围栏时围栏**外**的解释文字也会被丢掉
    (那些文字插进代码里就是语法错误)。
23. HTTP 403 与 401 共用「API key 无效或已过期」这一条文案。

### 平台

24. **macOS 上未经真机验证。** 全部自动化验证都在 Debian aarch64 容器里完成。
    平台相关的风险点:宽字符原型、`KEY_S*` 键码、`$TMPDIR` 形态、clang 诊断格式
    —— 四条都在 [§9](#9-故障排查) 里给了对应的修法。
25. **只跑 macOS 终端**(需求边界)。Linux 上能编能跑但不是交付目标;Windows 完全没做。
26. `Alt` 组合依赖终端把 Option 发成 Esc 前缀。配成 CSI 修饰符的终端下按词移动会失效
    —— 这是 Esc 前缀方案本身的取舍。
27. **面板区显示哪个标签**跟着焦点走:聚焦某面板时画它;焦点在编辑区时画**第一个有未读输出**的
    面板,都没有未读则画【编译】。没有「编辑时固定盯着某个面板」这个选项。
28. **最小可用尺寸 60x16。** 更小的窗口只画一行中文提示。
29. **状态栏挤不下时会按优先级丢字段**:徽章 > 运行中 > 文件名 > 行:列 > `F1帮助` >
    AI 状态 > 构建结果 > 语言。80 列终端上常见的效果是「上次构建结果」被截断成 `2 错误…`。
    `F1帮助` 固定右对齐、永不被挤掉。
30. **不做打包分发**(没有 brew formula、没有签名、没有安装器)。`make install` 就是 `cp`。

---

## 11. 安全

- **代码里没有任何硬编码的 API key**,一个都没有。
- `--doctor` 只显示「已配置(已隐去)」,**绝不回显 key 本身**;AI 的错误文案里也不会带 key。
- 填好 `api_key` 的配置文件是机密文件。**不要提交进版本库。**
  `config.sample.json` 里的 `api_key` 是空字符串,可以安全地提交。
- 不做任何绕过 API 计费或鉴权的事情。key 请自行到 DeepSeek 官方控制台申请。

# Wave 5 最后一轮 —— 5 项小修的实施与实测

范围:协调者裁决的 5 项。**只改了 1 处头文件(`src/ui.h` 新增 `Ui::scrollHelp`,已批准),
未改动任何已有签名。**

复用脚本:`.flower/scripts/wave5-pty-drive.cpp`(现成的 forkpty 驱动器,未改)。
新增脚本:`.flower/scripts/wave5-finalfix-verify.sh` + `.flower/scripts/wave5-finalfix-help-page.txt`。

---

## 0. 改动清单

| 文件 | 改动 |
|---|---|
| `src/keys.cpp` | `+1` include;新增 `keyColWidth()`(17 行);`helpRow` 的 `22` -> `keyColWidth()` |
| `src/textbuf.cpp` | `langFromPath` 重写(15 行 -> 41 行,含 18 行注释) |
| `src/build.cpp` | `Builder::syntaxOnly` 重写(5 行 -> 15 行,含 9 行注释) |
| `src/main.cpp` | `--help` 的扩展名说明(2 -> 4 行);`checkOpenPath` 的拒绝提示(3 -> 4 行) |
| `src/ui.h` | **新增** `void scrollHelp(int delta);` + 13 行注释;新增 private 成员 `help_pages_` |
| `src/ui.cpp` | `drawHelp` 里设 `help_pages_`;页脚文案;新增 `Ui::scrollHelp`(9 行实现 + 6 行注释) |
| `src/app.cpp` | `App::onKey` 的 `if (help_)` 分支加 PgDn/PgUp/↓/↑ 路由(14 行) |
| `src/proc.cpp` | `+1` include(`<sys/resource.h>`);`childMain` 里加 `setrlimit(RLIMIT_CORE,{0,0})`(11 行,含 5 行注释) |
| `tests/test_proc.cpp` | `+1` include;`t_signaled` 注释;**新增 `t_no_core_dump`**(48 行) |
| `tests/test_textbuf.cpp` | `testLangFromPath` 增 30 行断言(大小写 / `.h++` / `.hp` / `.inc`) |
| `tests/test_diag.cpp` | `case_compile_spec_header` 增 21 行断言(含"syntaxOnly ⇒ langFromPath 认得"的不变式) |
| `tests/test_openpath.cpp` | 接受表 10 -> 23 种扩展名;拒绝表把 `e.CPP` 换成 `.inc/.INC/.cp/.hs` |
| `README.md` | 5 处(见 §7) |
| `.flower/scripts/wave5-c-find-help.txt` | 1 行:页脚断言跟随文案改动 |

---

## 1. 帮助输出列宽

`keys.cpp:190` 原本是 `padRight(keyLabel(a), 22)`,而 `Action::BufferStart` 的 label
是 `Alt-< / Alt-Home / Shift-Home`(显示宽 **28**),补齐直接失效,输出粘成
`Alt-< / Alt-Home / Shift-Home跳到文件开头`。

**`padRight` 本来就用的是 `util::displayWidth`**(`keys.cpp:161-166`),不是 `size()` ——
这一点已确认,中文描述不会错位,不需要改。

修法:不写死 30,而是按表里实际最长 label 算 + 强制 2 列间隔:

```cpp
int keyColWidth() {
  static const int w = []() {
    int m = 0;
    for (std::size_t i = 0; i < kBindingCount; ++i)
      m = std::max(m, util::displayWidth(keyLabel(kBindings[i].action), 8, 0));
    return m + 2;
  }();
  return w;
}
```
为什么算而不写死:将来往 `kBindings` 里多加一个别名就又粘上了。

### 实测(`--help`)

```
    Alt-< / Alt-Home / Shift-Home  跳到文件开头
    Alt-> / Alt-End / Shift-End    跳到文件末尾
    Ctrl-O / F2                    保存文件
    Esc                            依次取消:补全 / 提示行 / 面板焦点
```

机器核对(`wave5-finalfix-verify.sh cols`):45 行 helpRow,**全部**有 ≥2 空格间隔,
最长 helpRow 行显示宽 68。`test_keys.cpp::helpLines` 报 `66 行,最宽 67 列`
—— 仍然 < 80,`drawHelp` 的 `content_w` 与分页数都没变(80x24 仍是 4 页)。

### 实测(F1 浮层,pty)

```
第 1/4 页 · PgDn/PgUp 或 ↓/↑ 翻页 · 再按 F1 也翻页 · Esc 关闭
...  PgDn  下翻一页            Alt-< / Alt-Home / Shift-Home  跳到文件开头
```

**踩到的坑**:第一版页脚写成
`第 X/Y 页 · PgDn/↓ 或再按一下 F1 看下一页 · PgUp/↑ 上一页 · Esc 关闭`,
在 80 列下被 `fitDisplay` 截成 `… Esc 关…`。原因:`·`(U+00B7)/`↓`(U+2193)/`↑`
都是 East-Asian **Ambiguous**,本 locale 下 `wcwidth` 给的是 **2** 而不是 1,
按 1 估算会算少 5 列。已把文案缩短并在代码里写下这条注释。

---

## 2. 扩展名大小写

`langFromPath` 原来整体大小写敏感 -> `main.CPP` 被 `main.cpp:289` 的门槛拒绝打开。

**规则(有意保留的不对称)**:
* `.c`(小写单字符)= `Lang::C`;`.C`(大写单字符)= `Lang::Cpp` —— **保持大小写敏感**。
  这是 C/C++ 自 cfront 起的既有约定,gcc/g++ 至今照此办事。
* 其余一律折成小写查表 —— `.CPP/.Cpp/.cPp/.HPP/.Hpp/.CC/.CXX/.HXX/.HH/.H/.C++/.HP` 都认。
  注意 `.h`/`.H` 都归 `Lang::Cpp`,所以对它们"不敏感"不丢任何信息。

单元测试:`tests/test_textbuf.cpp::testLangFromPath` 新增 30 行断言,逐个钉住
`a.c`=C / `a.C`=C++ / `main.CPP`=C++ / `main.Hpp`=C++ / `a.inc`=Unknown。

端到端(`wave5-finalfix-verify.sh ext`,31 条):23 种扩展名被接受、8 种被拒绝,
全部 PASS。判据用的是"非交互环境 rc=3 + 『不是交互式终端』"= 门槛放行,
"『不支持的文件类型』"= 门槛拒绝。

---

## 3. 死代码(`syntaxOnly` 与 `langFromPath` 两份清单)

原状:`build.cpp:265-269` 认 `.h .hpp .hh .hxx .h++ .hp .inc`;
`langFromPath` 只认前四个;`main.cpp:289` 用后者当门槛 ⇒ 后三个分支永远走不到。

**裁决与实施**:
* `.h++` / `.hp` **加入** `langFromPath`(GCC 也把它们当 C++ 头,不算扩大边界);
* `.inc` **去掉** —— 它不是 C/C++ 的标准扩展名,留着等于扩大需求边界;
* 两处**合成一份判定**,做法是让 `syntaxOnly` 复用 `langFromPath` 再问一个**正交**问题:

```cpp
bool Builder::syntaxOnly(const std::string& src) {
  if (TextBuffer::langFromPath(src) == Lang::Unknown) return false;
  const std::string ext = util::extension(src);   // 含点
  if (ext.size() < 2) return false;
  return ext[1] == 'h' || ext[1] == 'H';
}
```
为什么首字母够用:受支持的头扩展名**全部**以 h 开头(`.h/.hpp/.hh/.hxx/.h++/.hp`),
源扩展名**全部**以 c 开头(`.c/.C/.cpp/.cc/.cxx/.c++`)。于是 `build.cpp` 里
一份扩展名清单都不剩(`wave5-finalfix-verify.sh dead` 用 grep 当看门人)。

**最终支持清单(共 12 种,大小写变体另计)**
```
C   : .c
C++ : .C  .cpp  .cc  .cxx  .c++          (源码 -> 编成临时二进制)
C++ : .h  .hpp  .hh  .hxx  .h++  .hp     (头文件 -> 只 -fsyntax-only)
```
新增的不变式断言(`test_diag.cpp`):`syntaxOnly(p) ⇒ langFromPath(p) != Unknown`。
这条一旦被破坏就意味着"能语法检查但打不开",也就是死代码回来了。

---

## 4. 帮助浮层翻页(唯一批准的头文件改动)

`src/ui.h` **新增**(未动任何已有签名):
```cpp
void scrollHelp(int delta);          // +1 = 下一页, -1 = 上一页, 到头回绕
```
配套新增 private 成员 `int help_pages_ = 1;`(由 `drawHelp` 每帧填),
因为总页数取决于终端尺寸 × 分栏数 × `helpLines()` 行数,**只有渲染层算得出来**,
而 `AppModel` 已被裁决冻结。

`app.cpp::onKey` 的 `if (help_)` 分支:`PageDown`/`MoveDown` -> `scrollHelp(+1)`,
`PageUp`/`MoveUp` -> `scrollHelp(-1)`,两者都 `markDirty()` 后 `return`(**不关浮层**);
`Esc`/`F1` 与其余任意键仍然关闭浮层。

`scrollHelp` 里的取模写了两遍 `+pages`:C++ 的 `%` 对负数给负结果,
`PgUp` 会算出负页码,`drawHelp` 里 `page * per_page` 就成了负下标。

**保留**了"连按两下 F1 = 下一页"这条兜底路径(`draw()` 末尾关浮层时 `++help_page_`),
理由:某些终端(尤其 macOS Terminal.app,`kf1` 可能是 `\E[11~`)上 F1 之外的
功能键也可能对不上,多一条路更稳;页脚把两种方式都写明了。

### pty 实测:`wave5-finalfix-help-page.txt`(全 PASS,0 失败)

```
打开 -> 第 1/4 页 / PgDn/PgUp / 再按 F1 也翻页 / Esc 关闭 / 左移一个字符
PgDn -> 第 2 页(── 文件 / 编译 / 运行 ── + 剪切当前行)
PgDn -> 第 3 页(── AI ── + 聚焦【编译】面板)
PgDn -> 第 4 页(聚焦【AI】面板 + 切换到下一个面板标签)
PgDn -> 回绕第 1 页(── 移动 ── + 左移一个字符)     ← 不越界
PgUp -> 第 4 页;PgUp -> 第 3 页                      ← 反向,负页码不越界
↓    -> 第 4 页;↑    -> 第 3 页
Esc  -> 关闭,F1帮助 回来 / PAGEPROBE_MARKER 可见 / cursorin 24x80
       expectnot "pageprobe.cpp*"                     ← 翻页键没写进缓冲区
5 行 x 20 列:F1 + PgDn + PgUp + ↓ 全部 nocorrupt,不崩
20 行 x 5 列:F1 + PgDn 全部 nocorrupt,不崩
回到 24x80:cursorin 24 80 + F1帮助 + nocorrupt,子进程退出码 0
== 全部通过 ==(失败 0 条)
```

**踩到的大坑(值得记住)**:第一版脚本按 `expectfrom 第 2/4 页` 断言,4 条全 FAIL,
而实际翻页是好的。原因:**ncurses 只重发变化了的单元格**。翻页时标题/页脚里唯一变的
就是那一位数字,所以流里只会出现一个孤零零的 `2`,`第 2/4 页` 这个整串永远不会再出现。
改成断言"该页独有的正文内容"才有效。
同理,`Ctrl-L` 不能用来强制全量重发 —— 浮层开着时它作为"任意键"会把浮层关掉。

另一个坑:这段断言**必须跑在新起的 cppide 进程里**。混进
`wave5-c-find-help.txt` 尾部时起始页号不确定(前面第 13 步关过一次浮层,
"关掉时页码 +1" 仍然生效),所以做成了独立脚本。

---

## 5. 子进程禁 core dump

`proc.cpp::childMain` 在**信号处置之后、dup fd 之前**加:
```cpp
struct rlimit no_core;
no_core.rlim_cur = 0;
no_core.rlim_max = 0;
(void)::setrlimit(RLIMIT_CORE, &no_core);
```
`setrlimit` 是纯系统调用,async-signal-safe,满足 `proc.cpp` 头注释 I2 / 约定 §4.1
("fork 之后子进程里只能调 async-signal-safe 的函数")。失败忽略。

### 反证 + 正证

本机 `core_pattern = core`、`ulimit -c = unlimited`,所以这条断言是有意义的:
```
$ d=$(mktemp -d); (cd "$d" && sh -c 'kill -SEGV $$')
Segmentation fault (core dumped)
$ ls -A "$d"
core                                  ← 不禁 core 时确实会留下
```

新增用例 `tests/test_proc.cpp::t_no_core_dump`(脚本里**故意不写** `ulimit -c 0`,
否则测的是 `/bin/sh` 而不是 `proc.cpp`):
* `r.signaled && r.signal == SIGSEGV` —— 禁 core **不影响**信号汇报;
* 子进程 cwd(一个 `mkdtemp` 出来的空目录)里 `core*` 文件数 == 0;
* 收尾 `rmdir` 必须成功(顺带证明目录真是空的);
* 自带前置条件自检:`core_pattern` 是管道/绝对路径,或本进程 `RLIMIT_CORE` 已是 0 时,
  只断言信号汇报并打印说明 —— 免得在别的机器上给出假 PASS。

`t_signaled`(原有的"被信号杀死"用例)保持 `ulimit -c 0` + `cwd=/tmp` 不变,
仍然正确报告 `被信号 SIGSEGV 终止`,已复跑通过。

```
test_proc 通过:33 个用例 / 1242 处断言(含 [06] 段错误不留 core 文件)
```

---

## 6. 回归(全部真跑)

| 项 | 结果 |
|---|---|
| `make check-headers` | rc=0,15 个头文件全部自给自足 |
| `make`(零 warning) | rc=0,`grep -ci warning build.log` = **0** |
| `make tests` | rc=0,**16 个** test 文件全过(`test_proc` 33 个用例 / 1242 断言,含新增 `[06] 段错误不留 core 文件`) |
| `bash .flower/scripts/wave5-verify.sh` | rc=0,**52 PASS / 0 FAIL**,「Wave 5 汇总:失败 0 条」 |
| `bash .flower/scripts/wave5-finalfix-verify.sh` | rc=0,**46 PASS / 0 FAIL**(本轮新增的专项验证) |
| ASan/UBSan(用**本轮代码**重建 `/tmp/cppide-asan`) | `wave5-verify.sh asan` 全过 + 新增的 F1 翻页脚本在 ASan 下全过,**零报告**(`detect_leaks=1`,没有生成任何 `asan-*.log`) |
| `/work/core` | 跑完全部回归后不存在 |

**任务书写的是「wave5-verify.sh 35 条断言」,实跑是 52 条。**
差在 ASan 子集:那一节需要 `/tmp/cppide-asan` 存在才跑,上一轮报 35 条时它不存在被跳过了。
本轮我用**改后的源码**重建了那个二进制,所以 ASan 一节也真跑了(+ 若干条)。

### 唯一一条 FAIL 是**脚本里刻意不计分**的已知项(与本轮改动无关)

`wave5-b3b-midflood.txt` 的 `expectlast 退出码` 失败。
`wave5-verify.sh:293` 对它的调用是 `... 2> /dev/null || true` 且**没有 `want`**
—— 上一轮就已经把它排除在计分之外。原因是 `wave5-polish.md §6.2` 记的那件事:
`hard_kill_bytes = max(8MB, 4×run_output_limit)` 会先把 100MB 输出的程序 SIGKILL,
所以面板上写的是「已终止 · 信号 SIGKILL · 输出已截断」而不是「退出码 N」。
从 `b3b.out` 里读到的就是这一句,已确认是配置语义问题、不是本轮引入的回归。

---

## 7. README 改了哪几处(5 处)

1. §0 需求边界的扩展名清单 —— 补 `.h++` `.hp`,加一句大小写规则。
2. §3 配置表 `cxx` 那一行 —— 头扩展名清单补全。
3. §6 编译规则表(源文件 / 头文件两行)+ 下面那句括注 —— 补 `.h++` `.hp`、
   写明 `.inc` 不支持、写明"两处是同一份判定"。
4. §4.5 `F1` 那一行 —— 补 `PgDn`/`PgUp`/`↓`/`↑` 翻页与"到头回绕"。
5. §10 已知取舍第 15 条下新增一个子条目 —— `Ctrl-R` 跑的程序禁 core 及其代价。

(只改了受影响的那几行,没动 README 的其它段落。)

---

## 8. 未验证 / 风险

1. **macOS 一次都没跑过**(与 Wave 5 同)。与本轮直接相关的不确定项:
   * `setrlimit(RLIMIT_CORE)` 在 macOS 上同样是标准 POSIX 调用,但 macOS 默认
     `kern.coredump` / `/cores` 的行为与 Linux 的 `core_pattern` 不同,
     "不留 core"这条在 macOS 上没有实测证据(`t_no_core_dump` 的前置条件自检
     会在 macOS 上直接走"只断言信号汇报"的分支,**不会假 PASS,但也不构成证据**)。
   * `PgDn`/`PgUp` 依赖 terminfo 的 `knp=\E[6~` / `kpp=\E[5~`。本环境是
     `xterm-256color`。Terminal.app 的 `knp`/`kpp` 未核对;若对不上,
     `↓`/`↑` 与"连按两下 F1"两条兜底路径仍在。
2. **`·`/`↓`/`↑` 的显示宽度依赖 locale**。本轮页脚在中文 locale(ambiguous=2)下
   量过、放得下;若在 ambiguous=1 的 locale 下,只会更宽松,不会截断。
   但**反过来没验证**:如果哪天有终端把它们当 3 列算,页脚会再被截。
   截断只影响提示文字,不影响功能。
3. **`keyColWidth()` 用了函数内 `static` 初始化**(线程安全的,C++11 起有保证),
   但它意味着"绑定表是编译期常量"这个假设 —— `kBindings` 确实是 `constexpr` 静态表,
   成立。若将来改成运行期可配置键位,这个 cache 必须去掉。
4. **`syntaxOnly` 的"首字母 h/c"技巧依赖清单的形状**。现在成立,而且有
   `test_diag.cpp` 的不变式断言看门;但如果将来要支持 `.ipp` / `.tcc` 这种
   不以 h 开头的头扩展名,必须改回显式判定(注释里写了这条依赖)。
5. **`wave5-c-find-help.txt` 里 9b 那段"连按 6 次 F1 对翻页"的断言仍然保留**,
   它依赖"关浮层时页码 +1"这条兜底路径。如果将来有人把兜底路径删掉,那段会红。
6. `.inc` 从支持清单里去掉是**行为变更**:以前 `.inc` 本来也打不开(门槛拒),
   所以对用户不可见;但 `Builder::syntaxOnly(".inc")` 从 `true` 变成 `false`
   —— 这是唯一的对外可见语义变化,且只有直接调该 API 的代码才看得到(只有 App)。

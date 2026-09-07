# Wave 3 · build.cpp + keys.cpp + Makefile 修复 · 实施记录

日期:2026-09-06 · 环境:Debian trixie/aarch64,g++ (Debian) 14.2.0,ncursesw 6.5,GNU make 4.4.1

## 1. 产出

| 文件 | 行数 | 说明 |
|---|---|---|
| `/work/src/build.cpp` | 381 | §4.3 编译命令装配 + 手写诊断解析 + `Builder::analyze()` |
| `/work/src/keys.cpp` | 432 | §8.2/§8.3 绑定表 + 键名 + 帮助浮层 + `isTextInput` |
| `/work/tests/test_diag.cpp` | 1235 | 24 用例 / 539 断言(含 4 个真编译用例) |
| `/work/tests/test_keys.cpp` | 526 | 10 用例 / 2415 断言 |
| `/work/Makefile` | +6 行 | `CPPFLAGS += -Isrc` |
| `/work/.flower/notes/跨模块约定.md` | 165 | 18 条跨模块硬约定(Wave 4 必读) |
| `/work/.flower/scripts/gen-diag-samples.py` | 121 | 现场编译真实 g++/gcc 采集诊断样本并回填测试 |
| `/work/.flower/scripts/wave3-build-keys-verify.sh` | — | 可复用验收脚本(6 步全绿) |

复跑:`bash /work/.flower/scripts/wave3-build-keys-verify.sh 10`(退出码 0 = 全绿)

## 2. Makefile 的改动(全文 diff)

```diff
 PREFIX   ?= /usr/local
 UNAME_S  := $(shell uname -s)
+
+# tests/*.cpp 直接写 #include "config.h"(不带 ../src/),所以搜索路径必须带 -Isrc。
+# 放在 CPPFLAGS 而不是 CXXFLAGS:check-headers 的 SYNFLAGS 会过滤 CXXFLAGS,
+# 而 CPPFLAGS 三个目标(%.o / check-headers / tests)都会用到。
+CPPFLAGS += -Isrc
 
 ifeq ($(UNAME_S),Darwin)
```

这是唯一的改动。**没有**触碰:Darwin/Linux 分支、`-lncursesw -lcurl -lpthread`、
`-MMD -MP` 依赖生成、`SYNFLAGS := $(filter-out -MMD -MP,$(CXXFLAGS))`、
`check-headers` 的 stdin TU 技巧、`debug` / `install` / `clean` / `-include $(DEP)`。

位置选择的理由:`CPPFLAGS +=` 必须出现在 Darwin 分支之前或之后都可以(分支里也是
`CPPFLAGS +=`,`+=` 可叠加),放在 `UNAME_S` 之后是为了让"跨平台无关的搜索路径"
和"平台相关的 brew 路径"分开看。

验证:
```
make check-headers  -> OK (15 个头文件)     # 与 Wave 1 一致
make tests          -> 11 个测试全过        # 修复前挂在 test_config.cpp 的 #include "config.h"
```

## 3. 验收结果

```
=== 零 warning 门 ===
g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/build.cpp   -> 零输出
g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/keys.cpp    -> 零输出

=== make 两个门 ===
make check-headers                    -> OK (15 个头文件)
make tests                            -> 11 个测试全部通过
   test_aihttp / test_config / test_diag / test_editor / test_highlight /
   test_json / test_keys / test_panel / test_proc / test_sse / test_textbuf / test_util

=== 测试 ===
test_diag  24 个用例 / 539 个断言
test_keys  10 个用例 / 2415 个断言
两者 x (-O2 / -O1 -g -fsanitize=address,undefined) x 5 轮连跑,全绿
ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

=== 性能(诊断解析,-O2) ===
病态候选串(30 万字符、10 万个伪行号候选)  1 ms      # 平方级实现会卡死
5 万行诊断                                  11 ms
1 MB 单行 / 4 MB 无严重度单行               不崩,即刻返回
```

## 4. ★ 真编译跳行的实测(验收要求的直接证据)

`tests/test_diag.cpp::case_real_compile_line_numbers` 真的 fork 了一个编译器:

源文件 `/tmp/cppide-diag-XXXXXX/broken one.cpp`(**路径含空格**,顺手一起验):
```
1  #include <string>
2  int main() {
3    int a = undefined_symbol_here;      <- 错
4    (void)a;
5    std::string s = 5;                  <- 错
6    (void)s;
7    return not_declared_either;         <- 错
8  }
```
实测输出:
```
c++ -O2 -std=c++17 -Wall -o /tmp/cppide-51bbaaf3c04d026e-broken_one '/tmp/cppide-diag-VT3SDo/broken one.cpp'
  -> 3 错误 · 用时 90ms
源文件里的 error 行号: 3 5 7(期望 3 5 7)
```
断言是 `err_lines == {3,5,7}`(**不许多也不许少**),且每条诊断:
`d.file == 传进去的完整路径`、`d.line > 0`、`d.col > 0`、
`stderr` 的第 `d.out_line` 行确实包含 `d.message`(面板双向定位的前提)。

另外三个真编译用例:
- 单错误最小样本:`one_error.cpp:4:10 'oops' was not declared in this scope`(断言 line==4)。
- 成功路径:真编译 + `runSpec` 真运行,喂 stdin `"40 2\n"` 拿到 stdout `"42\n"`、stderr `"done\n"`,
  `binaryStale` 在编译前为 true、编译后为 false、改源文件后又为 true。
- 头文件语法检查:`bad header.hpp:4` 的错误被正确定位,`binary_path` 为空,
  **没有** `#pragma once in main file` 假警告,没有 `.gch` 副产品。
- 编译器不存在:`无法启动编译器:无法执行 cppide-no-such-compiler-xyz:No such file or directory`。

## 5. 诊断解析器的算法(为什么这么写)

不用 `<regex>`。逐行处理,顺序如下:

1. 左裁空白。
2. **挡掉源码回显与指示行**:`^\|`、`^\d+\s*\|`(GCC 的 `  3 |   int x = ;` / `   | ^~~~`)、
   以及"只由 `^ ~ 空白` 组成"的 clang 风格指示行。
   为什么必须先挡:回显的源码里如果有 `// error: xxx` 注释,不挡就会多出一条假诊断
   (`case_caret_lines` 有这条断言)。
3. 挡掉 `In file included from …` 与它的续行 `from x.h:2,` / `from y.cpp:1:`。
4. **整行只找一次严重度关键字**(`fatal error` / `internal compiler error` / `error` /
   `warning` / `note` / `remark` / `info`,要求左侧是行首/空白/`:`、右侧紧跟 `:`),
   找不到 → 这行不是诊断。
   为什么只找一次:如果对每个 `:<数字>` 候选都重新全行搜一遍,"极长垃圾单行"会退化成
   平方级(10 万候选 x 30 万字符)。改成"候选必须落在关键字左边"后是严格 O(行长)。
   `fatal error` 与 `error` 会命中同一处,取更靠左者 → `fatal error` 优先。
5. 从左往右找第一个**合法**的 `:<数字>`:数字后面必须是 `:` / 空白 / 行尾才算收口。
   这一条是路径含 `:` 时唯一的解法 —— 实测样本 `dir:with space/my file:2.cpp:3:11: error:`
   里,`:2` 后面是 `.` → 被否掉;`:3` 后面是 `:` → 收口,行号 3、列号 11、
   `file == "dir:with space/my file:2.cpp"`。
6. 没有行号但有关键字 → `line = 0`,`file` 取关键字左边那段去掉尾部 `:`,
   于是 `g++: fatal error: no input files` → `file="g++"`,
   `collect2: error: ld returned 1 exit status` → `file="collect2"`。
   这样链接失败也会计成 1 个错误,而不是"失败但 0 错误"。
7. `out_line` = 该行在 `stderr_text` 里的 0 起下标,切分用 `util::splitLines`
   (面板必须用同一个函数,见跨模块约定 §15)。

### 已知行为(不是 bug,是规则的必然结果)
- `"/x:9:8:7:6.cc:100:2: warning: hmm"` 会被解析成 `line=9, col=8, file="/x"`。
  路径里出现 `:<数字>:` 时无法与真正的行号区分。测试里明确记录了这个行为。
- 路径里同时含 `" error:"` 这种字面串(如 `/tmp/my error:1:1: error: x`)会退化成"无行号诊断"。
- clang 老版本不带 `N | ` 前缀的源码回显行,若其中含 `error:` 可能产生假阳性;
  目标平台 macOS 的 clang(≥15)是带前缀的,已挡住。

## 6. keys.cpp 的三个决策

1. **不 include curses.h**,把 `KEY_*` 数值硬编码成 `kNcLeft = 260` 之类的镜像。
   看门人是 `tests/test_keys.cpp` 顶部 19 条 `static_assert(KEY_LEFT == 260, ...)` +
   `static_assert(KEY_MIN==257 && KEY_MAX==511)` + `static_assert(kCharBase > KEY_MAX)` +
   `static_assert(Char(0x102) != KEY_DOWN)`(wave1-report §3.1(5) 那个坑的本体)。
   test_keys.cpp 是全工程**唯一**除 ui.cpp 之外 include curses.h 的文件,且已 `NCURSES_NOMACROS`。
2. **Alt 合成不放在 keys.cpp**。`keys.h` 没给合成函数的签名,而合成必须做一次非阻塞读
   —— 那是 ui.cpp 的活。keys.cpp 只提供查表;合成规则以"等价实现"的形式钉在
   `test_keys.cpp::synthesizeAfterEsc()` 里,并写进跨模块约定 §2 要求 ui.cpp 逐字照做。
   要点:第二次读到的键**也必须先归一化**再打 `kAltFlag`,否则 `Alt-←` 永远匹配不上。
   **不需要改任何头文件。**
3. **§8.2 没给键的动作**补了别名(都记在表里的注释上):
   `BufferStart/BufferEnd` → `Alt-<` / `Alt->`(主)+ `Alt-Home` / `Alt-End` + `Shift-Home/End`;
   `FindNext` → `F3`;`CompileAndRun` → `F7`(Ctrl-E 已在 §8.2);`NextTab` → `F9`。
   为什么补:`Action` 枚举里每个动作都得有键,否则 `bindings()` 覆盖不全、帮助浮层缺行。
   `Ctrl-Home/Ctrl-End` 刻意不用 —— 各终端的转义序列不统一,ncurses 多半认不出。
   `NextTab` 在"面板有焦点"时还应由 app.cpp 从 `Action::Tab` 转换而来(§8.3)。

### "绝不绑定"这一条的一个必要澄清
任务与 §8.2 都要求不绑 `Ctrl-M` / `Ctrl-I`,但它们的键码就是 `13` / `9` —— 也就是
Enter / Tab 本身。真把 13 和 9 从表里去掉,回车和 Tab 就不能用了。
所以实现取的是 §8.2 括号里那句话的字面意思("它们就是 Enter/Tab/Backspace 的别名"):
- `13 → Action::Enter`、`9 → Action::Tab`、`127 → Action::Backspace` 必须在表里;
- 但键名永远显示 `Enter` / `Tab` / `Backspace`,**任何绑定项的 `key_zh` 都不含 `Ctrl-`**;
- 测试对这两个键码断言的是"动作正确 + keyName 不含 Ctrl-";
- 真正查不到动作的是:`Ctrl-C`(3)、`Ctrl-Z`(26)、`Ctrl-S`(19)、`Ctrl-Q`(17)、
  `Ctrl-Y`(25)、`Ctrl-\`(28)、`Ctrl-J`(10)、`Ctrl-H`(8) —— 8 个键码,
  既断言 `lookupAction()==None`,也遍历整张表确认没有任何条目落在这些键码上。

## 7. build.cpp 的一个实现选择:头文件走 stdin TU

`compileSpec(".hpp")` 生成的是
`cxx <cxxflags> -fsyntax-only -x c++ -` + `stdin_data = "#include \"<绝对路径>\"\n"`,
而不是 `cxx -fsyntax-only foo.hpp`。

实测反证:
```
$ LC_ALL=C g++ -std=c++17 -O2 -Wall -fsyntax-only bad.h
bad.h:1:9: warning: #pragma once in main file      <- 关不掉的假警告
bad.h:2:18: error: 'notdefined' was not declared in this scope

$ printf '#include "%s"\n' "$PWD/bad.h" | LC_ALL=C g++ ... -fsyntax-only -x c++ -
In file included from <stdin>:1:
/tmp/bk/bad.h:2:18: error: 'notdefined' was not declared in this scope
```
文件名/行号仍然指向真实的头文件(所以照样能跳行),但少了那条假警告。
`-fsyntax-only` 下不会产出 `.gch`(测试有断言)。
文件名含 `"` 或换行时这招不成立,自动退回直接编该文件。

## 8. 遗留风险

1. **`KEY_*` 数值硬编码**:靠 test_keys.cpp 的 `static_assert` 把关。若 macOS 自带 curses
   的 `KEY_SHOME`/`KEY_SEND`/`KEY_BTAB` 与 Debian ncurses 不同,那三条 static_assert 会在
   macOS 上编译期失败(这正是想要的行为:当场发现,而不是"某个键没反应")。方向键、F 键、
   Home/End/PgUp/PgDn/DC/RESIZE 这些自 BSD curses 起就未变过,风险集中在 S* 系列。
2. **不绑 `Ctrl-H`(8)** 是 §8.2 的硬规定。某些终端(把 Backspace 配成"发送 Control-H"的
   iTerm2 配置)下退格会失灵。`keypad(stdscr, TRUE)` 下 ncurses 一般给 `KEY_BACKSPACE`,
   暂按规定不绑;若用户报此问题,需协调者决定是否放宽。
3. **`Alt-←/→` 依赖终端把 Option 发成 Esc 前缀**。Terminal.app / iTerm2 默认如此,
   但配成 CSI 修饰符(`ESC[1;3D`)的终端下 ncurses 认不出,按词移动会失效。
   这是 §8.2 已知的取舍,不是本实现的缺陷;`--doctor` 打印实际键码正是为了让用户自查。
4. **诊断解析对"路径里含 `:<数字>:`"会误判行号**(见 §5 已知行为)。需求只管单文件编辑,
   源文件路径一般来自用户自己敲的参数,可接受;测试里已把行为固定下来防止无声变化。
5. **`runSpec` 的 `cwd` 留空**(继承编辑器的工作目录),不是源文件所在目录。
   用户程序里 `fopen("data.txt")` 会相对于启动 cppide 的目录。若 Wave 4/5 认为应该
   改成源文件目录,只需改 `runSpec` 一行,但那会影响 `--doctor`/README 的描述。
6. **未在 macOS 上验证**(本机 Debian/aarch64)。涉及平台差异的点:`$TMPDIR`
   (已用 `util::tempDir()`,测试里 setenv 成 `/var/folders/xy/T` 验过前缀与去重复斜杠)、
   `KEY_S*` 数值、clang 的诊断格式(格式与 gcc 一致,已按 `N | ` 前缀挡回显行)。
7. `gen-diag-samples.py` 采集的 `kLinkError` 样本里含一个随机的临时 `.o` 名
   (`/tmp/ccUM5t2z.o`),重新生成样本会变。测试不依赖那一行(它不是诊断),但重新生成后
   若有断言对不上,先看是不是 g++ 版本变了。

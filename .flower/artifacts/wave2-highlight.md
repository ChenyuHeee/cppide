# Wave 2 · 语法高亮(src/highlight.cpp + tests/test_highlight.cpp)

日期:2026-09-06 · Debian aarch64 · g++ 14.2.0

## 1. 产出

| 文件 | 行数 | 说明 |
|---|---|---|
| `/work/src/highlight.cpp` | 496 | `tokName` / `Highlighter` / `HighlightCache` 全部实现 |
| `/work/tests/test_highlight.cpp` | 798 | 12 组、35.5 万处断言(纯 assert) |
| `/work/.flower/scripts/wave2-highlight-verify.sh` | — | 4 步验收,`ALL GREEN` 即通过 |

未修改任何 `.h`。未实现 `textbuf.cpp`。

## 2. 验收

```
=== 1. g++ -std=c++17 -O2 -Wall -Wextra -c src/highlight.cpp   零输出(零 warning)
=== 2. 测试 -O2 无消毒器                                        OK (12 组, 355631 处断言)
=== 3. 测试 -O1 -g -fsanitize=address,undefined                 OK(硬要求达成,0.1s)
=== 4. 测试 + 真实 src/textbuf.cpp 一起链接                     OK,且 nm 确认用的是强符号
```

复跑:`bash /work/.flower/scripts/wave2-highlight-verify.sh`(退出码 0 = 全绿)。

`make tests` 目前在 **别人的** `tests/test_config.cpp` 上就挂了:
`fatal error: config.h: No such file or directory` —— Makefile 的 `CPPFLAGS` 没有 `-Isrc`,
而那个测试写的是 `#include "config.h"`。本测试写的是 `#include "../src/highlight.h"`,
加不加 `-Isrc` 都能编。协调者要么给 Makefile 补 `-Isrc`,要么让各测试统一用 `../src/`。

## 3. 三个需要协调者知道的实现决定(都不动头文件)

### 3.1 行注释续行复用了状态位组合 `ST_CONTLINE | ST_BLOCKCOMMENT`

`// comment \` 的下一行仍是注释,但 1 字节状态里没有第 4 个位。
该组合不会自然出现(块注释内部从不产生续行状态),故约定:

- `ST_CONTLINE | ST_BLOCKCOMMENT` = “上一行是以 `\` 结尾的 `//` 注释”,本行整行 Comment;
- `ST_CONTLINE` 单独出现 = 普通续行(`#define X \`、表达式续行),本行按常规代码扫描。

对外只是一个不透明的字节,渲染/编辑层不解释它,`HighlightCache` 也只做等值比较。

### 3.2 `HighlightCache` 内部用 `0xFF` 表示“该行从未算过”

收敛提前退出必须能区分“新状态 == 旧缓存值”和“新状态 == 从没算过的填充 0”,
否则在刚 `resize` 的全 0 数组上会立刻误判收敛,把后面从未扫过的行当成有效。
合法状态只有 `0..7`,故 `0xFF` 作哨兵。`entryState()` 对外永不返回它(转成 `ST_NONE`)。
头注释里“多出来的行状态置 0”在语义上等价于“无效”,行为不可从外部观测。

收敛后的推进方式:一旦某行新旧状态相同,就把 `validUpTo_` 一路推到**第一个哨兵行**为止
(纯字节扫描,不再做词法),所以在文件中间敲 `/*` 不会全文重扫。

### 3.3 `resize()` 在行数变化时整体作废缓存

插/删行会让 `entry_[k]` 与行号错位(旧值其实属于别的行),而 `resize` 拿不到“变化点”。
若保留错位的旧值,收敛比较会误判(实测能构造出错误结果,已成为测试用例:
在 200 行文件最前面插一行 `/*`,期望第 106 行仍在注释中)。
故 `resize()` 一旦发现行数变了就 `assign(kUnknown)` 并把 `validUpTo_` 退回 1。

代价:插/删行后要从第 0 行惰性重扫到视口底部(一次纯词法扫描,无分配),
普通打字不改行数,走的仍是 `invalidateFrom` + 收敛退出的快路径。

## 4. 词法覆盖(architecture.md §3 逐条)

- 预处理:行首(跳空白)`#` → `#` 与指令词 `Preproc`,其后常规扫描;
  `#include` / `#include_next` / `#import` 后的 `<...>` 当 `String`(未闭合染到行尾)。
- `//` 到行尾 `Comment`;以 `\` 结尾 → 行注释续行状态(见 3.1)。
- `/* */` 跨行用 `ST_BLOCKCOMMENT`;块注释内的 `\` 不产生续行(块注释优先)。
- 注释内 `TODO` / `FIXME` / `XXX` 按**词边界**染 `TodoInComment`(`TODOS`、`XTODO`、`xxx` 不算)。
- 字符串处理 `\` 转义;未闭合染到行尾且**不泄漏状态**。
- 原始字符串:同行内按真实 `)delim"` 精确闭合;跨行置 `ST_RAWSTRING`,后续行按 §3 的
  有意近似“出现 `)"` 即结束”。前缀支持 `R" LR" uR" UR" u8R"`,delim 长度上限 16。
- 数字:`0x/0X`(含 `.` 与 `p` 指数)、`0b/0B`、十进制、`.5`、`2.`、`e/E±`、
  后缀 `uUlLfF` 与用户字面量 `_km`、C++14 分隔符 `'`(仅当前后都是数字才吞)。
- 字符字面量 `'a' '\n' '\'' '\x41' '\\'`;未闭合染到行尾、不泄漏状态。
- 关键字表/类型表:惰性构造的 `static const std::unordered_set<std::string>`,
  C 与 C++ 分开(`class/template/namespace/new/nullptr/true/false/try/...` 在 C 下不是关键字;
  STL 名 `string/vector/map/set/pair/queue/priority_queue/ll/...` 只在 C++ 下是类型)。
  `Lang::Unknown` 按 C++ 处理。
- 标识符紧跟 `(` → `Func`(关键字/类型优先,`if (` 仍是 `Keyword`)。

## 5. 不变式与它的保证方式

`scanLine` 的所有分支都以“上一段的结束 = 本段的开始”推进游标,相邻同 `Tok` 自动合并;
所有分段点都落在 ASCII 字节上,`>= 0x80` 的字节一律并进标识符/注释/字符串整段,
因此**多字节字符(中文)不会被切在中间**。测试对每次扫描都跑:

- `checkCover`:`len > 0`、`start` 严格接续、`start/len` 在界内、`tok` 在 `[0,kTokCount)`、
  各段长度之和 == 行字节数、空行必须 0 段;
- `checkUtf8Boundaries`:合法 UTF-8 行的任何内部分段点都不是 continuation byte
  (故意非法的输入不做此要求,只要覆盖不变式);
- 纯函数性:同一 `(line, entryState)` 重扫一次,状态与每个 span 必须逐字段相同。

## 6. 测试组(12 组)

`tokName` / 竞赛语料覆盖不变式(3 种 Lang × 42 行,含中文注释与中文字符串) /
预处理与 `#define` 续行 / 行注释与续行链(3 行) / 块注释跨 3 行 /
字符串 vs 注释(`"/*"`、`"// ..."`、`"a\"b"`、未闭合、单个 `"`) /
原始字符串(单行、自定义 delim、跨行、`u8R`/`LR`、伪 `R`) /
数字 17 例 / 字符字面量 6 例 + `1'000` 不被当字符字面量 /
关键字与类型表的 C/C++ 差异 / 极端输入 / `HighlightCache`。

极端输入组包含:空行 × 6 种入口状态、30 个单字符行、只有 `/`、只有 `"`、只有 `'`、
`\`、`/*`、`//`、`*/`、非法 UTF-8(`\xff\xfe\x80\xc3`、开头就是 continuation byte、
1..255 全字节串 × 3 种入口状态)、内嵌 `NUL`、10 万字符行(混合语法 / 全空格 /
未闭合块注释 / 未闭合字符串)。

`HighlightCache` 组:空缓存与越界(`-5`、`999`)、200 行惰性扫描并逐行与
“从头老实扫一遍”的参考实现比对、**收敛提前退出的可观测断言**
(改一行后 `invalidateFrom(0)`,只问第 1 行的状态,`validUpTo()` 应直接跳到 200)、
会真正传播的改动(行首插 `/*`)、插行、删行、跨行原始字符串、
300 轮伪随机 `invalidateFrom` + 随机访问必须始终等于参考实现。

## 7. 测试怎么在 Wave 2 就测到了 `HighlightCache`

`entryState/spansFor` 收 `const TextBuffer&`,无法用别的类型伪造。
`tests/test_highlight.cpp` 就地给出 `TextBuffer` 的 5 个成员
(`ctor / reset / lineCount / line / lineLen`)的最小实现,全部标 `__attribute__((weak))`:

- 只链 `highlight.cpp` 时用这份假实现(Wave 2 场景);
- 链上 `textbuf.cpp` 时强符号覆盖弱符号 —— 不重复定义报错,且自动改用**真实现**。

两种链接方式都实测通过(验收脚本第 3、4 步),`nm` 确认第 4 步用的是强符号 `T`。
本测试只用到 `reset/lineCount/line` 这三个 architecture.md 冻结的公开语义。

## 8. 遗留风险

1. **原始字符串跨行是有意近似**(§3 已写明):自定义 delim 且体内含 `)"` 会提前结束。
   已记为 README 限制。
2. **未闭合字符串 + 行尾 `\`** 只置普通 `ST_CONTLINE`(下一行按代码扫描),
   不是“字符串继续”。1 字节状态放不下第 4 种延续,且 ICPC 代码不会这么写。
3. **数字后缀一律吞进 Number**:`1else` 会整体染成 Number(合法 C++ 里不存在这种写法)。
4. `Span.start/len` 是 `int`:单行超过 2^31 字节才会溢出,不可能发生;10 万字符已测。
5. 未闭合的 `'` 会把行尾染成 `Char`(如代码里孤立的撇号)。观感问题,不影响不变式。
6. TSan 未跑(容器 ASLR 限制,Wave 1 已记录);`scanLine` 无可变共享状态,
   `HighlightCache` 是非线程安全的普通对象,按设计只在 UI 线程用。

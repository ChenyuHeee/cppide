# Wave 2 — util 模块实现日志

产出:
- `/work/src/util.cpp`（约 700 行）
- `/work/tests/test_util.cpp`（约 780 行，267024 次 CHECK）
- `/work/.flower/scripts/verify-util.sh`（一键验收）

**未修改任何 `.h`。**

## 1. 验收结果

| 项 | 结果 |
|---|---|
| `g++ -std=c++17 -O2 -Wall -Wextra -c src/util.cpp` | 0 warning |
| 额外 `-Wpedantic -Wshadow -Wconversion` | 0 warning |
| test_util @ `-fsanitize=address,undefined` | PASS（267024 checks，含 LeakSanitizer） |
| test_util @ `-O2` 无 sanitizer | PASS |

跑法：`sh /work/.flower/scripts/verify-util.sh`

## 2. 语义决策（util.h 注释没写全的部分，已全部落在 util.cpp 文件头注释里并测到）

### 2.1 显示宽度
- `charDisplayWidth`：控制字符（`cp < 0x20` 或 `0x7F`）→ **1**（占位符绘制）。
  `'\t'` 本身也算 1，制表位展开是**字符串级**语义，只在
  `displayWidth / byteToDisplayCol / displayColToByte / wrapDisplay` 里生效。
- **wcwidth 返回 -1**：先查内置兜底表 —— 命中 East Asian Wide/Fullwidth → 2，
  命中组合/零宽表 → 0，都不命中 → **1**。永不返回负数。
  兜底表的存在理由：glibc 在 `LC_CTYPE=C` 下对**所有** >0x7F 的码点返回 -1（已实测），
  没有兜底表的话，一旦 `main()` 忘了 `setlocale`，中文就会按 1 列排版、整个屏幕错位。
  有兜底表时最坏也只是覆盖面不如系统表全。测试 `testWidthFallbackInCLocale()` 专门
  切到 `C` locale 验证这条路径。
- 孤立代理（D800–DFFF）、`> 0x10FFFF` → 1（当作 U+FFFD 占位）。
- 组合字符 / 零宽字符 → 0。
- Tab：`adv = tab_width - (col % tab_width)`，恒 >= 1；`col` 已经在制表位上时**跳整格**
  （`"abcd\t"` @tw=4 宽度是 8，不是 4）。`tab_width` 被钳到 `[1, 256]`。
- `displayWidth(s, tw, start_col)` 返回的是**本串占用的宽度**（`end_col - start_col`），
  不是终点列。

### 2.2 列落在字符中间的取整规则
- `displayColToByte`：**向左吸附到该字符的起始字节**。列 <0 → 0；列 >= 整行宽度 → `s.size()`。
  例：`"中文"` 列 0/1 → 0，列 2/3 → 3，列 >=4 → 6。
- `byteToDisplayCol`：byte_col 落在字符中间时**同样向左吸附**（半个字符不计宽度）。
  两者吸附方向一致，因此在无零宽字符时**互为逆运算**，测试里对
  `{ASCII, 纯中文, 混排, Tab, Tab+中文, emoji, 前后空白}` × `tw ∈ {1,2,3,4,8}`
  做了全边界 × 全显示列的双向穷举验证（`checkColBijection`）。
- 有零宽字符时列与字节不再一一对应（这是数学事实，不是 bug）：组合符归属前一个字符，
  `displayColToByte` 返回下一个占位字符的起点。测试 `"e" + U+0301 + "zh"` 覆盖。

### 2.3 非法 UTF-8（硬要求）
- `utf8Decode` 用严格解码器：拒绝超长编码（C0/C1、E0 80、F0 80）、代理区（ED A0..BF）、
  `> U+10FFFF`（F4 90+ / F5..FF）、截断序列、孤立续字节。
  非法一律 **吃 1 字节 + cp = U+FFFD**，永不返回 0 → 所有 `i += utf8Decode(...)` 循环必然前进。
- `i >= s.size()` 时返回 1、`cp = 0`（不越界读）。
- `utf8Prev` 最多回退 3 个续字节，并校验“该字符长度正好覆盖到 i”，否则退 1 字节。
- 测试覆盖：`0x80`/`0xBF` 孤立续字节、`0xFF`/`0xFE`、`F5 80 80 80`、截断的 2/3/4 字节、
  `C0 AF`/`C1 BF`/`E0 80 AF`/`F0 82 82 AC` 超长编码、`ED A0 80`/`ED BF BF` 代理区、
  CESU-8 代理对、内嵌 NUL。
- 另有 **600 组固定种子随机二进制串** 的 fuzz（`testBinaryFuzz`），检查：正反遍历必然前进、
  `displayColToByte` 永远落在字符边界且单调、`byteToDisplayCol` 单调有界、wrap 不变式、
  `fromWide(toWide(·))` 幂等。全程在 ASan/UBSan 下跑。

### 2.4 wrapDisplay
- 空串 → 一段 `{0,0}`（面板里空行也要占一行）。
- `'\n'` 作硬换行，该字节不属于任何段；末尾换行不产生空尾段（与 `splitLines` 一致）。
- 优先在空白（空格/Tab）处断，断点上的空白被吃掉；无空白则硬断。
- **唯一可能超宽的情形**：单个字符本身就比 `width` 宽（如 width=1 遇到中文）——
  此时该段只含这一个字符，保证前进不死循环。测试显式断言了这一点。
- `width < 1` 按 1 处理。
- 测试不变式（`checkWrapInvariants`，14 种串 × width 1..12 × tw ∈ {1,4,7}）：
  段单调不重叠、不越界、两端都在 UTF-8 字符边界、宽度 <= width（除上述单字符例外）、
  **段与段之间被丢掉的字节只能是空格/Tab/换行**（不吞正文）。

### 2.5 其它约定
- `splitLines`：行数 = `'\n'` 个数 + (末尾无 `'\n'` ? 1 : 0)，**结果至少 1 个元素**；
  `splitLines("") == {""}`（空文件 = 一个空行，正好对上 `textbuf.h` 的“lines_ 永远 >= 1 行”）。
  吃掉行尾 `'\r'`（CRLF）。
- `split(s, sep)`：标准语义，元素数 = sep 出现次数 + 1，`split("", c) == {""}`。
- `toLower` / `findNoCase` **只处理 ASCII A-Z**：绝不会改动 UTF-8 多字节序列的字节。
- `clipBytes`：结果（含 `…`）总字节数 <= `max_bytes`；`max_bytes < 3` 放不下省略号时
  只给不切断字符的前缀。
- `toHex`：固定 16 位小写十六进制零填充（`Builder::tempBinaryPath` 可以自行 substr）。
- `extension`：`".bashrc"` → `""`（隐藏文件不算扩展名）；`"a.tar.gz"` → `".gz"`；`"a."` → `"."`。
- `dirname("")` → `"."`；`basename("/")` → `"/"`；`joinPath(a, "/abs")` → `"/abs"`。
- `formatDuration`：`<1s` → `"12ms"`；`<60s` → `"1.4s"`；否则 `"2m03s"`；负数按 0。
- `writeFileAtomic`：同目录 `<name>.cppide.tmp` → write → `fsync` → `close` → `rename`，
  再 best-effort `fsync` 目录项；失败时删掉临时文件；已存在目标文件时**保留其权限位**。
- `readFile`：目录会被明确拒绝（中文 err），失败时 `out` 清空。

## 3. 给协调者/其它 agent 的两条提醒（都不需要改头）

1. **`util::basename` / `util::dirname` 与 POSIX 的 `::basename` / `::dirname` 撞名。**
   在 `using namespace util;` 之后写裸 `basename("/a/b/")`，重载决议会选中
   `<string.h>` 里的 glibc `::basename`（`const char*` 精确匹配胜过到
   `const std::string&` 的用户定义转换），行为不同（glibc 不剥尾部斜杠，返回 `""`）。
   这是我写测试时**真被咬到**的一次失败。约定:**调用处一律写 `util::basename(...)`**。
2. **locale**：`main()` 必须在任何显示宽度计算前 `setlocale(LC_ALL, "")`
   （ncurses 宽字符本来也要求）。忘了也不会崩 —— 有内置兜底表，见 §2.1。

## 4. 遗留风险

- 内置兜底宽度表是 East Asian Width 的**子集**（覆盖 CJK/假名/谚文/全角/常见 emoji/
  常见组合符）。只有在 locale 不是 UTF-8 时才会被用到；此时冷僻脚本可能算 1 列。
  正常运行路径（setlocale 成功）走系统 `wcwidth`，不受影响。
- emoji 的 ZWJ 序列 / 肤色修饰 / 区域指示符（国旗）按各码点分别计宽，
  终端实际渲染可能只占 2 列 —— 这是所有终端编辑器的共同问题，`wcwidth` 层面无解。
- `displayWidth` 在超长行上把列钳到 `1<<28` 防 int 溢出;正常行远达不到。
- 文件相关测试依赖 `/tmp` 可写、`rm -rf` 可用。

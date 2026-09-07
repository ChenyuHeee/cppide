# Wave 3 — panel.cpp(输出面板 + StdinBuffer)实现日志

产出:
- `/work/src/panel.cpp`(684 行)
- `/work/tests/test_panel.cpp`(1305 行,17 个测试用例,106112 次断言)
- `/work/.flower/scripts/wave3-panel-verify.sh`(一键自证:零 warning + 严格编译 + ASan/UBSan/LSan + 4 个变异测试)

头文件**一个字节都没改**。

---

## 1. 验收结果

| 项 | 结果 |
|---|---|
| `g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/panel.cpp` | 零 warning |
| 再加 `-Wpedantic -Wshadow -Wconversion` | 零 warning |
| ASan+UBSan、`ASAN_OPTIONS=detect_leaks=1` | 通过,无泄漏、无 UB |
| `-O2` 无 sanitizer | 通过 |
| 变异测试 4/4 被单测抓住 | 通过 |
| panel.cpp 是否 include curses | 否(`nm -u` 也无 curses 符号) |

灌 100MB 运行输出(`append()` 1MB × 100 块,共 1,709,209 行)实测:

```
-O2      :耗时 92ms;峰值 RSS 3.3MB -> 4.9MB(灌入前后);保留 4611 行 / 285,882 字节
ASan+UBSan:耗时 539ms;峰值 RSS 23.7MB -> 220.9MB(ASan 的 redzone/quarantine 开销,
            1.7M 次 string 分配全进隔离区;非 sanitizer 版同样负载只涨 1.6MB)
8MB 无换行的单块输出:-O2 下 4ms(早期 O(n²) 版本要 16GB 拷贝,已改成一趟扫)
```

结论:100MB 输入下常驻内存增量 ≈ 1.6MB,行数被 `max_lines_` 限住(4611 ≤ 5000),
最新一行内容逐字节正确、最旧行确实被丢。

---

## 2. 三条把边界情况兜住的不变式(panel.cpp 头注释里也写了)

1. **环形裁剪**:`lines_.size()` 恒 `<= max_lines_`。超了**批量丢**(多丢 1/8 容量),
   否则每行一次 `vector` 前端 `erase` 是 O(n),灌 250 万行就是 O(n²)。
   批量丢把它摊销成每次 append 常数级搬移 —— 这就是 92ms/100MB 的来源。
   代价:`lineCount()` 稳态在 `max_lines_ * 7/8 .. max_lines_` 之间(实测 4611),
   语义上仍满足"超出丢最旧"。
2. **单行字节上限 4096**:没有 `'\n'` 的 100MB 输出(进度条、二进制、`cat` 二进制文件)
   否则会变成一个巨型逻辑行,把 `layout_` 撑爆(视觉行数 ≈ 字节数/宽度)。
   超了在 **UTF-8 字符边界**硬分行,元信息(颜色/`diag_index`)在各段上保留。
   两条上限合起来把最坏内存钉死在 `5000 * 4097 ≈ 20MB`。
3. **流式的 carry 缓冲是隐式的**。`panel.h` 冻结版没有 carry 成员(也不能加),
   但也**不需要**:delta 在多字节字符中间被切断时,照原样把字节接到最后一行尾巴上,
   残缺序列只是暂时残缺,下一个 delta 一到就自动拼成完整字符。
   于是"每 1 字节投喂"与"一次性投喂"得到的字节流**完全相同**;
   硬分行点也只取决于前 `kMaxLineBytes+1` 个字节(`utf8SplitPoint` 只看
   `s[off..off+cap]`),两种投喂方式看到的这些字节一样,分行点因此一样。
   残缺期间的绘制由 `util::utf8Decode` 兜住(非法字节 = 1 字节的 U+FFFD),
   `wrapDisplay` 因此永不把字符切两半。

折行**全部委托给 `util::wrapDisplay`**(优先空白断、单词过长硬断、绝不切断 UTF-8),
panel.cpp 自己不做任何字节切分决策 —— 这样"不切半个字符"只有一个决策点。

## 3. 其它实现决定

- `scroll()` 是 const 且 `scroll_` 不是 mutable,所以**读时夹取**:
  `auto_scroll_` 为真时直接返回 `maxTop()`(贴底自动跟随),否则返回夹到
  `[0, maxTop()]` 的存值。存的 `scroll_` 越界无害。
  `scrollTo/scrollBy` 落到底部会自动重新打开 `auto_scroll_`。
- **宽度变化保持滚动锚点**:`setViewSize()` 在宽度变化且不贴底时,先记下当前视口顶部
  所在的**逻辑行**,重算折行后把它拉回顶部。80→20→80 能精确回到原位(单测断言)。
  贴底的面板缩放后依然贴底。只改高度不重算折行。
- `view_w_ <= 0` 时**不折行**(每逻辑行一个视觉行、覆盖整行)。若按宽度 1 折,
  20MB 内容会炸出 2000 万个 `VisualLine`。
- `append()` 不用 `util::splitLines()`:那会先把整块(1MB)物化成
  `vector<string>`,峰值内存翻倍。改成逐行扫 + 边扫边裁剪,语义与 splitLines 对齐
  (吃掉 CRLF 的 `'\r'`、末尾换行不产生空尾行)。
- 流式续行**继承** `is_stderr`/`is_meta`(stderr 流不会中途变白),但不继承 `diag_index`。
- `endStreaming()` 收掉"末尾换行开出来的空尾行",但保留有意的空行(连续两个换行)。
- 任何 append 都置 `unread`(标签显示 `*`);`clear()` 清 unread 与 errorCount。
  聚焦时由 App 调 `setUnread(false)`。
- `setCursorLine()/moveCursor()` 会把光标行滚进视口(编译面板挑诊断的前提)。
- 裁剪时 `cursor_line_` 与 `scroll_` 跟着上移(折行缓存干净时精确换算被丢的视觉行数),
  用户正在看的位置不会莫名其妙漂走。
- `StdinBuffer::data()`:空缓冲返回**空串**(不能凭空给子进程多喂一个 `"\n"`),
  非空且末尾无换行时补一个。`loadFile()` 走 `util::readFile` + `setData`,
  刻意不用 `buf_.loadFile()` —— 那会污染 `path()`/`lang()`。
  `undo()/redo()` **没有**包在 `TextBuffer::Edit` 里(Edit 作用域内 undo 恒返回 false)。
  移动光标/回车都调 `breakUndoMerge()`。

## 4. ★ 需要协调者放行的头文件修正(唯一一处)

`panel.h` 声明了 `unread()/setUnread()/errorCount()/setErrorCount()` 四个方法,
**但 private 区没有对应的存储成员**(只有 `id_/lines_/max_lines_/streaming_/
view_w_/view_h_/scroll_/auto_scroll_/cursor_line_/tab_width_/layout_/layout_dirty_`)。

不改头的兜底(已实现):把两个标签状态放在 panel.cpp 里一张**按 `PanelId` 索引的静态表**
(`TabState st[kPanelCount+1]`,零动态分配、LSan 干净),构造函数重置对应表项。

为什么按 id 而不是按 `this` 指针:`app.h` 里是 `std::vector<Panel> panels_`,
`emplace_back` 触发重分配时元素会被**移动**到新地址 —— 按地址存的状态会在那一刻
静默丢失(标签上的 `*` 无声无息不见了,极难查)。按 id 存天然免疫移动/拷贝/重分配,
单测里有一条专门断言这个(`std::vector<Panel>` 强制重分配后 unread/errorCount 仍在)。

**残留代价**:同一个 `PanelId` 的多个**同时存活**的实例会共享标签状态。
真实程序里 App 每个 id 只有一个实例,所以不影响功能;但这是个陷阱。

**建议的修复**(3 行,风险为零):在 `panel.h` 的 private 区加

```cpp
  bool unread_ = false;
  int  error_count_ = 0;
```

然后删掉 panel.cpp 里 `★ 头文件缺口的兜底` 那一节 + 改 4 个访问器和构造函数里的一行。
**别处一行都不用动**(ui.cpp / app.cpp 看不见差别)。

## 5. 单测覆盖(17 个用例)

| 用例 | 覆盖 |
|---|---|
| `testTitles` | `panelTitleZh` 四个 id + 越界 id 不崩 |
| `testEmptyPanel` | 空面板上滚动/移光标/查映射全不崩且坐标合法;`lineAt` 越界安全;`clear()` |
| `testAppendBasics` | 多行/CRLF/空串/末尾换行/stderr/diag/meta/`appendLine` 内嵌换行/`byteSize` |
| `testWrapInvariants` | **折行不变式**:14 种行(中英混排、emoji、Tab、超长无空格、纯空白、组合字符)× width 1..12/20/80 —— 每段宽度 ≤ width(唯一例外:单字符本身超宽)、段边界必在 UTF-8 字符起点、拼回去只允许丢断点空白、逻辑行覆盖连续不跳号 |
| `testStreamingChunkEquivalence` | **关键测试**:含中文/emoji/CRLF/超长段的文本按 1,2,3,4,5,7,13,64,4095,4096,4097 字节分片 + 20 组随机分片投喂,内容与折行结果必须与一次性投喂**逐字段相同** |
| `testStreamingMidCharSafety` | 只喂 lead byte 的中途状态下所有只读操作安全;拼回完整字符;分片边界落在硬分行点附近 |
| `testStreamingLineSemantics` | 续写/换行/CRLF/`endStreaming` 收尾/被 `append` 打断/颜色继承 |
| `testRingTrimSmall` | `setMaxLines` 10/1/0/-7/放大;最新在最旧丢;裁剪时光标与滚动合法;流式灌入也受约束 |
| `testLongLineSplit` | 12288 字节无换行行硬分行:每段 ≤ 4096、每段合法 UTF-8、拼回原串、元信息保留;4 字节 emoji 边界 |
| `testFlood100MB` | 100MB / 171 万行:行数被限、`byteSize` 有硬上界、最新在最旧丢、布局滚动自洽、峰值 RSS 打印并设门槛;另测 8MB 无换行块 |
| `testScrollEdges` | 到顶/到底/超范围/空面板/单行/高度 0/高度 1/autoScroll 语义/折行后按视觉行滚 |
| `testUnreadAndErrorCount` | 未读置位与清除、两面板互不影响、负错误数夹 0、`clear` 清标签、new/delete 干净、`std::vector<Panel>` 重分配后状态仍在 |
| `testDiagIndex` | 正向(面板行→诊断下标)、反向(诊断下标→面板行)、折行后每段指回同一逻辑行、光标跟随视口、裁剪后仍正确 |
| `testResizeRewrap` | 80→20→80 折行重算 + 滚动位置**精确**回到原位;贴底面板缩放后仍贴底;14 个宽度的横跳序列坐标始终合法 |
| `testLogicalVisualMapping` | `logicalToVisual`/`visualToLogical` 全量自洽 + 越界夹取 |
| `testStdinBufferData` | 空输入→空串;多行含中文往返一致;补末尾换行;CRLF 规整;`loadFile` 成功/失败;视口滚动与越界光标 |
| `testStdinBufferEditing` | 中文插入/多字节移动与删除/行合并/全撤销回到初始 + 全重做回到终态/直接操作底层 `TextBuffer` 后光标不坏 |

### 变异测试(`wave3-panel-verify.sh` 第 6 步)

单测必须能抓住这 4 个"看起来能跑"的退化,否则测试是自欺:

| 变异 | 被哪条断言抓住 |
|---|---|
| m1 朴素流式:丢掉 delta 尾部残缺的多字节序列 | `FAIL chunk=1: lineCount 10 vs 9` |
| m2 按字节折行(切半个字符) | `FAIL !isCont(text[end])` |
| m3 宽度变化时不保持滚动锚点 | `FAIL visualToLogical(scroll()) == 10 (5 vs 10)` |
| m4 不做环形裁剪 | `FAIL lineCount() <= 10` |

## 6. 遗留风险

1. **头文件缺 `unread_`/`error_count_`**(见 §4)。当前兜底能用,但"同 id 多实例共享状态"
   是个隐藏陷阱,建议 Wave 4 前把两个成员补进 `panel.h`。
2. **`layout_` 内存是 O(内容字节/宽度)**。宽度极小(1~3)且内容接近上限(20MB)时,
   `layout_` 理论上能涨到上百 MB。真实程序的面板宽度 = 终端宽度(架构规定最小可用
   60x16),不会走到这里;单测里 width 1..12 只用小内容。若要彻底封死,需要给
   `rewrap()` 加"只折视口附近若干页"的窗口化折行 —— 那需要改头(加成员),没做。
3. **单行 4096 字节上限**会把超长的单行编译诊断(极少见)拆成多行,拆点处的
   `diag_index` 在各段上都保留,所以跳转仍可用,但视觉上是两行。
4. `append()` 会把 `'\r'` 吃掉(CRLF 兼容),因此**回车覆盖式的进度条**
   (`\rProgress: 50%`)不会被特殊处理:裸 `'\r'` 原样留在行内,由 ui.cpp 决定怎么画。
   如果需要"`\r` 覆盖本行"的终端语义,得在 panel 或 ui 层另加处理 —— 需求没要求。
5. 未读标记由 panel 自己在 append 时置位。若 App 想"聚焦中的面板追加内容不算未读",
   需要 App 在追加后自己 `setUnread(false)`(架构 §8.1 的自动聚焦规则由 App 实现)。

# Wave 3 — Editor(src/editor.cpp)实现日志

日期:2026-09-06 · Debian aarch64 · g++ 14.2.0

## 1. 产出

| 文件 | 行数 | 说明 |
|---|---|---|
| `/work/src/editor.cpp` | 747 | 光标/视口/编辑/自动缩进/Ghost/查找/剪贴板 |
| `/work/tests/test_editor.cpp` | 1256 | 纯 assert,441 个 check(NDEBUG)/430(assert 开启) |
| `/work/.flower/scripts/wave3-editor-verify.sh` | — | 6 步验收,一条命令跑完 |

**未改动任何 `.h`**(`src/editor.h` md5 `ec22de6aab0966b62f9c917edceccdf1`,时间戳仍为 Wave 1 的 16:48)。

## 2. 验收结果(`.flower/scripts/wave3-editor-verify.sh`)

| 步骤 | 结果 |
|---|---|
| `g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/editor.cpp` | 零 warning,零输出 |
| 额外 `-Wshadow -Wsign-conversion -fsyntax-only` | 零输出(仅参考) |
| ASan+UBSan,assert 开启 | PASS(430 checks) |
| **ASan+UBSan `-DNDEBUG`** | **PASS(441 checks)** ← §6 反证测试在这里真正跑到运行期防线 |
| `-O2 -DNDEBUG` | PASS(441 checks) |
| `-O0` 无 sanitizer,assert 开启 | PASS(430 checks) |

UBSan 用 `halt_on_error=1`,ASan 用 `detect_leaks=1`。

## 3. §6 咽喉点的落地方式

`acceptGhost()`(editor.cpp:679-696)开头四行与 architecture.md §6 逐字一致、顺序一致:

```
if (!ghost_.complete)                    return false;   // 生成中不接受
if (ghost_.text.empty())                 return false;
if (ghost_.sink != AiSink::GhostText)    return false;   // ★ 结构性拦截
if (ghost_.gen != expected_gen_)         return false;   // 过期建议不接受
```

`setGhost()`(editor.cpp:650-659)是 `assert(g.sink == AiSink::GhostText)` **加**运行期
`if (g.sink != AiSink::GhostText) { ghost_.clear(); return; }`。运行期那条比 §6 描述更强一点:
不只是“不设置”,而是把已有 ghost 也清掉,退化方向严格朝“没有 ghost”。

`appendGhostDelta` / `markGhostComplete` 也各自先查 `sink != GhostText` 再查 gen,
所以 ghost 被 clear 之后(默认 sink = PanelOnly)任何在飞的流式片段都进不来。

### 反证测试怎么做到“不是靠 assert”

assert 在 NDEBUG 下消失,所以测试分两路:

* **NDEBUG 构建**:直接把 `sink == PanelOnly` 的 ghost 交给 `setGhost()`,断言
  `ghost().empty()`、`acceptGhost() == false`、`buffer().text()` 逐字节未变、
  `revision()` 未变、`dirty() == false`、`canUndo() == false`(连撤销记录都没产生);
  循环 5 次仍然如此。这一路是 §6 保证的**真正**证明。
* **assert 开启的构建**:`testSetGhostAssertFires()` 用 `fork()` 出子进程调同一条路径,
  断言子进程**不是**以 0 退出(即 assert 真的 abort 了)。子进程 stderr 重定向到
  /dev/null,免得预期的 "Assertion failed" 混进测试输出。

另外三条拦截(gen 过期 / `complete == false` / `text` 空)在**所有构建**下都跑:
每条都用“除被测项外全部合格”的 ghost,断言不插入 + 缓冲区未变,再把被测项修正
后断言这次**能**接受 —— 证明拦住的确实是那一条,不是别的原因顺带拦住的。

## 4. 关键实现决定

1. **desired_display_col_**:只有 `moveUp/moveDown/movePageUp/movePageDown/scrollBy`
   保持它不变,其余移动与全部编辑都重算。跨行落点走 `util::displayColToByte`
   (落在宽字符/Tab 中间时向左吸附),没有另造一套换算。
2. **`ensureCursorVisible()`**:纵向 scrolloff 2;窗口太矮时压成 `(h-1)/2`;
   文件末尾把 top 贴到 `max(0, nl-h)`(牺牲下边距,不留空屏)。
   横向按 8 列跳步,右边距 4 列,窗口宽 `< 12` 时放弃右边距、`< 8` 时放弃 8 列对齐,
   最后一道 `if (left > dc) left = dc` 保证光标一定可见。`w<=0 / h<=0` 走独立分支不崩。
3. **`scrollBy()`** 把光标拉进 scrolloff **内圈**(`top+so .. top+h-1-so`),
   否则紧接着的 `ensureCursorVisible()` 会为了补边距把 top 又顶回去,滚动“不听话”。
4. **一次动作 = 一个 `TextBuffer::Edit`**,每个动作一个独立 label
   (`insert-char` / `insert-text` / `newline` / `backspace` / `delete` / `indent` /
   `unindent` / `cut-line` / `paste` / `accept-ai`)。只有单字符 `insert-char` 允许落进
   TextBuffer 的 800ms 合并窗口 —— 粘贴走 `insert-text`,不会被粘进相邻的打字组。
   结构性动作(回车/退格/删除/缩进/剪切/粘贴/接受 ghost)在 endGroup 之后调
   `breakUndoMerge()`。
5. **`undo()/redo()` 绝不包进 `Edit`**(Wave 2 约定:`group_depth_>0` 时返回 false)。
6. **另存为**先 `setPath()` 再 `saveFile()`(Wave 2 约定:`saveFile` 不改 `path_`),
   之后 `refreshLang()` 重推语言。
7. **`indentWidthOf/indentUnitOf` 写成文件内自由函数**(取 `const Config&`),
   因为头文件冻结、不能加私有成员。
8. **Backspace 整级退格**只在“光标之前全是空格”(纯缩进区)且 `expand_tab` 时生效,
   退到最近的制表位;`a    b` 这种非缩进空格仍然退一格。
9. **`copyLine()`(Ctrl-D)= 复制当前行到内部剪贴板**,不改缓冲区,连续按累积
   —— 按 §2.3 的“Ctrl-D 复制当前行”与方法名 `copyLine` 取的语义(不是“复制/重复插入一行”)。
   若协调者要的是 duplicate-line,改这一个函数即可,别处不受影响。
10. **未命名缓冲区按 C++ 高亮**:`refreshLang()` 把 `Lang::Unknown` 映射成 `Lang::Cpp`。
11. **移动光标也调 `hlCache().invalidateFrom(cursor_.line)`**(按任务要求)。代价接近零:
    `invalidateFrom` 不清 `entry_` 旧值,`entryState` 的收敛提前退出会立刻停下。

## 5. 测试覆盖清单

| 组 | 覆盖 |
|---|---|
| `testGhostSecurity` | §6 六路反证(PanelOnly / gen 过期 / 未完成 / 空文本 / 过期 delta / 编辑作废 ghost) |
| `testSetGhostAssertFires` | fork death test,证明 assert 存在(仅 !NDEBUG) |
| `testGhostSingleUndo` | 8 行 ghost 接受 -> 单次 undo 全回退 + redo 也是一次 |
| `testCursorUtf8` | 中文/emoji/Tab 上左右移动的合法字节序列、跨行、显示列换算 |
| `testDesiredCol` | 短行往返不丢列、穿过宽字符行不丢列、首/末行边界 |
| `testWordMove` | ASCII/中文/跨行按词移动 |
| `testHomeEnd` | 智能 Home 两段行为、整行空白、buffer 首尾 |
| `testInsertAndNewline` | 多字节整体插入、空串 no-op、Enter 缩进(含 `{`、含尾空白、缩进中间、关掉 auto_indent) |
| `testBackspaceDelete` | 跨行合并、多字节整体退格、整级退格(对齐/非对齐/非缩进区/expand_tab=false)、Delete 跨行、**最后一行 Delete 无副作用** |
| `testIndentUnindent` | Tab 补到制表位、Shift-Tab、`\t` 缩进、无缩进可退、expand_tab=false |
| `testLineClipboard` | Ctrl-K 累积、移动打断累积、最后一行/单行剪切、粘贴往返 byte-for-byte、Ctrl-D、非行模式粘贴、逐次撤销 |
| `testViewport` | 顶部不早滚、scrolloff、**文件末尾 top=190**、**超长行右端 left=168 且 %8==0 且右边距>=4**、8 列跳步次数上界、回短行、scrollBy 三种越界 |
| `testDegenerateViewport` | 7 组尺寸(含 1、0、负)× 20 种操作,断言 top/left 非负、光标在可见区 |
| `testGotoAndFind` | gotoLine 负数/0/越界/列越界/落在汉字中间;find 前后向、回绕、UTF-8 needle、**空串 false 且不动光标不改 lastSearch**、单行不死循环 |
| `testEmptyBuffer` | 空缓冲区上 20 余种操作 + undo/redo/find/acceptGhost 全部安全 |
| `testHighlightInvalidation` | 编辑后 `validUpTo()` 回退、插行后整体作废、`/*` 真的把后续行状态改成 ST_BLOCKCOMMENT、spans 覆盖整行 |
| `testLangAndFile` | 未命名=Cpp、load 推 C、另存为 .cpp 改 path+lang、读不存在文件不改缓冲区 |
| `testOneActionOneUndo` | 8 种动作各自“一次 undo 回到原样” |
| `testFuzzInvariants` | 4000 步随机操作,每步断言:光标不落 UTF-8 续字节、行列合法、视口自洽、未设置过的 ghost 永不可接受 |

## 6. 遗留风险

1. `copyLine()` 的语义按 §2.3 取“复制到剪贴板”。若 keymap 作者预期的是“重复当前行”,
   需要协调者裁定 —— 改动只涉及 `Editor::copyLine()` 一个函数。
2. `scrollBy()` 会把光标拉进可见区(而不是让光标留在屏外)。这是为了让 `ui.cpp`
   永远不必处理“光标在视口外”的屏幕行号。若 UI 期望“滚动不动光标”,需协调。
3. 词移动把所有 `>= 0x80` 的码点归为“词”,所以中文标点与汉字算同一类,
   一整段中日韩文本会被当成一个词跨过。真编辑器会按 Unicode 断词,这里刻意不做。
4. 反向 `find` 的“上一处”定义为“起点严格小于光标列”。若与 keys/app 的 FindPrev
   语义(比如期望以选区为准)不一致,需要在 Wave 4 对齐 —— 但没有选区,应该没问题。
5. macOS 上一行都没编过(与 Wave 1/2 同样的风险);本文件只用标准库 + `util`,
   没碰任何平台 API,预计是全工程最低风险的一块。

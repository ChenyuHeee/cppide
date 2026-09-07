# Wave 2 — src/textbuf.cpp 实现日志

产出:`/work/src/textbuf.cpp`(632 行)、`/work/tests/test_textbuf.cpp`(992 行)、
自证脚本 `/work/.flower/scripts/wave2-textbuf-verify.sh`。头文件**未改动一个字节**
(`src/textbuf.h` md5 `5cfa47936be4d27c4d2a8d6414105cfb`,保持 Wave 1 冻结态)。

## 验收结果

```
1) g++ -std=c++17 -O2 -Wall -Wextra -c src/textbuf.cpp      零 warning
2) 再加 -Wpedantic -Wshadow -Wconversion                     零 warning
3) nm -uC textbuf.o | grep 'util::'                          无匹配(不依赖 util.cpp)
4) -fsanitize=address,undefined 下 test_textbuf               全部通过,退出码 0
5) -O2 无 sanitizer 再跑一遍                                  通过
6) /work 无残留产物,/tmp 无 .cppide-save-* 残骸               通过
```

随机往返规模:主测 **2000 次**随机 insert/erase(固定种子 `0xC0FFEE`,最终 98 行 /
658 字节,1990 组撤销 + 1990 组重做逐字节相等),另加 5 个种子 × 700 次 = 3500 次
(3452 组),以及 400 次**逐组快照**核对(332 组:每撤销一组必须精确等于该组动作前的
快照,比"全撤销后相等"严格得多)。

## 关键实现决策(Wave 3 会碰到的)

1. **不依赖 util.cpp**。`util::nowMs()` 只用于 800ms 合并窗口这一处相对时间比较,
   本文件用 `<chrono>` steady_clock 内联实现,语义与 `util::nowMs()` 一致;UTF-8 只需
   "是否续字节"一个判断;文件读写用 POSIX `open/read/write` + `rename` 自己做原子写。
   好处:`textbuf.o` 可单独链接、单测不必等 util.cpp。

2. **CRLF**:`textbuf.h` 已冻结,没有存放换行风格的成员,因此保存时**无法**还原 CRLF。
   决定:`loadFile()` 把行尾 `"\r\n"` 一律规范化为 LF(行中间/末段的孤立 `'\r'` 原样
   保留,保证二进制内容不被改写),`saveFile()` 一律写 LF。副作用:CRLF 文件保存后变成
   LF 文件。已写进 textbuf.cpp 顶部注释。

3. **末尾换行 / 空文件**:无末尾换行的文件加载后行数组相同,保存时按 `text()` 的契约补
   一个末尾 `'\n'`;单个空行的缓冲区保存为 **0 字节**文件(而不是 `"\n"`),这样
   "空文件 → 加载 → 保存"仍是 0 字节。两个方向的往返都在单测里。

4. **分组状态只用冻结的那几个成员表达**。`beginGroup()` 在最外层**立刻压入一个空组**,
   于是"当前组"永远是 `undo_.back()`,`recordOp()` 不需要额外状态位;**合并判定推迟到
   `endGroup()`** —— 把刚关闭的组并进上一组(同 label + `<800ms` + 相邻 + 单个 UTF-8
   字符的插入)。`group_open_` 的含义 = "`undo_.back()` 还能接受下一次合并"。
   空动作的组在 `endGroup()` 里被丢弃,撤销栈上不留痕。

5. **`'\n'` 天然断组**:插入 `"\n"` 不满足"单字符可合并"的种子条件(显式排除换行),
   所以即使 Editor 忘了调 `breakUndoMerge()`,回车也不会被粘进上一组。
   `saveFile()` 成功后会自己调一次 `breakUndoMerge()`(§2.1 要求保存断组)。

6. **`undo()` / `redo()` 在 `group_depth_ > 0` 时返回 false**(⚠ 给 Wave 3 的约定)。
   事务进行中 `undo_.back()` 是本次动作的占位组,弹掉它会让配对的 `endGroup()` 去动
   别人的组。撤销不是"编辑",**Editor 不要把 `undo()/redo()` 包在 `TextBuffer::Edit`
   作用域里**,否则会静默无效。已有单测钉住这个行为。

7. **`saveFile()` 不改 `path_`**(只 `clearDirty()`)。另存为请先 `setPath()` 再
   `saveFile()`。`loadFile()` 成功才会改 `path_`/`lang_`,失败时缓冲区完全不动。

8. **越界规范化的确定行为**:`clampPos` 先夹行号(负→0 且 col=0;超界→末行行尾),
   再夹 col,最后**退回 UTF-8 字符边界**(非法字节序列下最坏退到行首)。
   `erase` 的 Range 先 `normalized()` 再两端 clamp 再 `normalized()`;规范化后为空的
   Range 是**无操作**(不 revision++、不记撤销、不清 redo)。空串 `insert` 同理。
   落在同一个多字节字符内部的 Range 因两端都退到字符起点而变空 —— 不会切出半个字符。

9. `.h/.hpp/.hh/.hxx` → `Lang::Cpp`(依 §4.3 头文件走 `cxx -fsyntax-only`);
   `.c` → C;`.cpp/.cc/.cxx/.C/.c++` → Cpp;其它 Unknown;点在目录名里(`/a.d/noext`)
   不算扩展名。

10. 撤销/重做也算改动:`revision()` 递增、`dirty()` 置 true(头文件没有"原始状态"
    标记,无法判断撤销回原始内容,取保守方向)。redo 栈同样限 2000 组。

## 单测覆盖清单

Pos/Range 运算符、基本插入(含多行、纯 `\n`、末行末尾 `\n`、多行末尾带 `\n`、中文
字节偏移)、基本删除(含删空缓冲区仍剩 1 空行、空缓冲区删除、删换行合并两行)、
越界(负数 Pos / 行号超界 / col 超行长 / Range a>b / 全越界 / 落在续字节上)、
text/textRange(含"textRange 与 erase 内容一致")、分组与合并(连续单字符一次撤销
全消、中文单字符合并、回车断组、不相邻不合并、label 不同不合并、真实 sleep 850ms
验证 800ms 窗口、8 行 ghost 一次撤销)、嵌套 Edit、异常穿过 Edit、不配对 endGroup、
事务内 undo 被拒、新改动清 redo、反复 undo/redo 不膨胀、revision/dirty/reset、
撤销栈上限(压 3000 组 → 恰好 2000 次撤销 + 2000 次重做后仍可用)、
随机往返(2000 + 5×700 次)、逐组快照核对(400 次)、
文件往返(ascii/中文/空/空行/含 `\0`/5000 字长行/孤立 CR/CRLF/混合换行/无末尾换行/
512 字节二进制非法 UTF-8)、文件错误(不存在、目录、父目录不存在、目标是目录导致
rename 失败、空文件名、失败不破坏原文件、不留 tmp 残骸)、langFromPath、hasNonBlank。

## 遗留风险

- **权限失败分支未在本环境实测**:容器里 uid=0,`/proc` 那条不可写路径测试被跳过。
  已用"父目录不存在"(ENOENT)和"目标是目录"(rename EISDIR)覆盖 `saveFile` 的失败
  返回路径,与 uid 无关。真实 macOS 上的 EACCES 走的是同一段 `open` 失败分支。
- **`noteCursorAfter()` 落到 `undo_.back()`**:如果 Editor 在 `Edit` 作用域结束后很久
  才调它,可能写到"上一组"的 `cursorAfter` 上。只影响撤销后的光标落点,不影响内容。
- `saveFile()` 只 `fsync` 文件本身,不 `fsync` 目录项;掉电语义弱于严格的原子写,
  但对编辑器场景(§13 已知限制)足够。
- `text()` 每次都是全量拷贝(高亮/AI 快照会频繁调)。几千行规模无所谓,若 Wave 4 发现
  它进了热路径,再加缓存(不影响接口)。

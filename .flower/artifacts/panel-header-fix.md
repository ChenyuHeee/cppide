# panel.h 存储成员补齐 —— 受控头文件改动执行日志

日期:2026-09-06。范围:`src/panel.h`、`src/panel.cpp`、`tests/test_panel.cpp`。其它文件一个字节未动。

## 1. 改动清单

### `src/panel.h`(143 -> 144 行,+2 行)
`Panel` 类 private 区 `tab_width_ = 4;` 之后插入:

```cpp
  bool unread_ = false;
  int error_count_ = 0;
```

命名风格(小写下划线 + 尾下划线 + 默认初值)与该类其余成员一致。
**没有改动任何已有签名、注释、include 或 StdinBuffer。**

### `src/panel.cpp`(684 -> 653 行,-31 行)
1. 删掉 `★ 头文件缺口的兜底` 整节(29 行注释 + `struct TabState` + `tabState(PanelId)`)。
2. 构造函数:`Panel::Panel(PanelId id) : id_(id) { tabState(id) = TabState{}; }`
   -> `Panel::Panel(PanelId id) : id_(id) {}`(成员默认初值已给出干净状态)。
3. `clear()`:`tabState(id_) = TabState{};` -> `unread_ = false; error_count_ = 0;`
   (语义不变:clear 同时清未读标记和错误计数。)
4. 4 处 append 路径的 `tabState(id_).unread = true;` -> `unread_ = true;`
   (`append` / `appendLine` 的两条分支 / `appendStreaming`,共 4 处 —— 保持“追加内容自动置未读”的原语义。)
5. 4 个访问器直读直写成员;`setErrorCount` 的负数夹到 0 的行为保留:

```cpp
bool Panel::unread() const { return unread_; }
void Panel::setUnread(bool v) { unread_ = v; }
int Panel::errorCount() const { return error_count_; }
void Panel::setErrorCount(int n) { error_count_ = (n < 0) ? 0 : n; }
```

留了一处**故意不动**的注释:`finishGrow()` 上方 "panel.h 的 private 区只有 rewrap(),
不能加成员函数,故写成自由函数" —— 本次只放行了加**数据成员**,没有放行加成员函数,
所以该自由函数保持原样,注释仍然准确。

### `tests/test_panel.cpp`(1305 -> 1329 行,+24 行)
在用例 12(标签栏状态)的向量重分配段之后追加断言,直接钉住这次修复解掉的陷阱:
- 同一 `PanelId`(`Run`)的**两个同时存活实例** `s1`/`s2`:置位、append、clear 互不干扰。
  (旧的静态表实现在这里必然失败。)
- 隐式拷贝构造带走标签状态,且拷贝体与源体后续互不影响。

断言数 106112 -> 106123。

## 2. 验收结果

| 项 | 结果 |
|---|---|
| `make check-headers` | OK,15 个头文件 |
| `g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/panel.cpp` | 零 warning(输出 0 行) |
| `bash .flower/scripts/wave3-panel-verify.sh` | `ALL GREEN`,exit 0 |
| ├ 严格编译 `+ -Wpedantic -Wshadow -Wconversion` | 零 warning |
| ├ 无 curses 依赖 | OK |
| ├ ASan+UBSan+LSan 跑单测 | `17 cases, 106123 checks`,无泄漏/无 UB |
| ├ -O2 版单测 | 同上;峰值 RSS 30MB(灌 100MB / 1709209 行) |
| └ 4 个变异测试 | m1..m4 全部被单测抓住 |
| `make tests` | exit 0,全过 |
| `make`(all) | 仍是 undefined `main`(ui/ai/app/main 未实现),预期 |

`make tests` 逐项:test_util 267024 checks / test_json 3081 / test_highlight 355631 /
test_textbuf / test_proc 32 cases 1237 / test_config 417 / test_editor 430 /
test_diag 24 cases 539 / test_keys 10 cases 2415 / test_aihttp 20 cases 198 /
test_sse 22 cases 90 / test_panel 17 cases 106123。
(test_aihttp 里那条 `status=401 API key 无效` 是**用例内预期**的真实网络断言,不是失败。)

任务要求确认的那条单测 —— `tests/test_panel.cpp` 用例 12 里 "app.h 里是
`std::vector<Panel> panels_`,emplace_back 触发重分配会移动元素,标签状态必须活过移动"
—— 依然通过。现在它是**天然满足**的:状态成了成员,随对象一起移动/拷贝。

## 3. 顺手扫的 15 个头文件(只报告,未改动)

脚本:`.flower/scripts/scan-header-storage.py`
(启发式:对每个 `T name() const;` 和 `void setName(...)`,在同文件里找 `name_` /
snake_case 化的 `na_me_`;找不到就报出来。)

修复后的报告里 `panel.h` 已不再出现 `errorCount` / `unread`。剩余条目**全部人工核对为
误报**(计算型 getter,或后备成员换了名字):

| 头 | 报出的 getter | 实际情况 |
|---|---|---|
| ai.h | `enabled` / `generation` / `stateZh` | `cfg_.aiEnabled()` / `gen_` / `state_`,均有后备 |
| app.h | `model` / `stdinData` / `setStatus` | 前两个每帧现算;`setStatus` 写 `status_msg_` + `status_expire_ms_` |
| proc.h | `currentJob` / `summary` | `cur_job_`;`summary()` 从 `diags` 现算 |
| config.h | `chatUrl` | `base_url + chat_path` 现拼 |
| build.h | `summaryZh` | 从诊断计数现算 |
| ui.h | `ghostAttr` / `tooSmall` | 从主题 / 终端尺寸现算 |
| editor.h | `buffer` / `lineCount` / `viewport` / `cursorDisplayCol` / `highlighter` | 委托给 `buf_` / `hl_` 等,editor.cpp 已编译通过 |
| textbuf.h / json.h / panel.h 余项 | `lineCount` / `isNull` / `viewWidth` … | 纯派生 |

**结论:`panel.h` 是 15 个头里唯一真实的"声明了访问器却没有存储"的缺口,现已补齐。**

未验证项:`ui.h` / `app.h` / `ai.h` / `mailbox.h` 的访问器只做了**人工阅读**核对 ——
这四个头对应的 .cpp 还没写(Wave 4),没有编译器帮忙确认后备成员真的够用。
若 Wave 4 实现者发现新缺口,需要再走一次受控头文件改动。

# Wave 4 — `src/ai.cpp`(AiService)实现日志

产出(只创建了这三个文件,**没有修改任何 `.h` 或其它 `.cpp`**):

- `/work/src/ai.cpp`(532 行)
- `/work/tests/test_ai.cpp`(1350 行,15 cases / 319 checks)
- `/work/.flower/scripts/wave4-ai-verify.sh`(`ROUNDS=20` 复跑全部验收)

`ai.h` **一个字都没改**(不需要新增 private 成员:worker 里要用的
delta 聚合状态、HttpChat 实例全部是栈上局部变量;事件出口做成了匿名命名空间里的
自由函数 `pushEvent(out, ...)`,因为约定只允许加成员**变量**)。

## 验收结果

| 项 | 结果 |
|---|---|
| `g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/ai.cpp` | 零 warning(零输出) |
| 额外严格档 `-Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast` | 也是零输出 |
| `make check-headers` | OK(15 个头文件) |
| test_ai(ASan+UBSan,`detect_leaks=1:detect_stack_use_after_return=1`)× 20 轮 | **20/20 通过**,无 leak / 无 UB |
| test_ai(`-O2 -DNDEBUG`,无 sanitizer) | 通过(§6 反证不依赖 assert) |
| 真实 `api.deepseek.com` 401 | 20/20 轮都真的拿到 401 并映射成中文,文案不含 key |
| TSan | 本机不可用(aarch64 + 无法关 ASLR),按要求用 ASan 连跑 20 轮替代 |

单轮 ~4.3s(其中 ~1.0s 是背压用例故意拉长的流,~1.5s 是真实 DNS+TLS 往返)。

## 验收清单逐项

1. **§6 反证** — `case_sink_decision_point` + `case_source_has_no_buffer_writes`。
   `sinkFor(Practice)==PanelOnly`、`sinkFor(Code)==GhostText`;练习模式在 3 种
   question × 切模式前后建出的每个 `AiRequest.sink` 恒为 `PanelOnly`;端到端用例
   (`case_end_to_end_practice`)断言**回传的每一个事件**(Started/Delta/Done)
   `sink == PanelOnly`。源码 grep 断言(读 `src/ai.cpp` 全文):
   - 全文没有 `insert(` / `erase(` / `.insert` / `->insert` / `.erase` / `->erase`;
   - 没有 `TextBuffer::Edit` / `beginGroup` / `reset(` / `loadFile` / `saveFile` /
     `setPath` / `undo(` / `redo(`;
   - 不出现 `Editor` / `editor.h` / `acceptGhost`(ghost 落地不在本层);
   - 每一行提到 `TextBuffer` 的地方要么是注释、要么写的是 `const TextBuffer&`
     (共 3 处 const 引用参数);
   - **`AiSink::GhostText` / `AiSink::PanelOnly` 两个枚举字面量在 ai.cpp 里一次都不出现**
     —— sink 只能由 `sinkFor()` 产出、由 `req.sink` 透传,不存在"某处自己决定了 GhostText"。
   这份用例在 `-DNDEBUG` 下同样跑(CHECK 宏用 `failAt()+abort()`,不用 `assert`)。
2. **key 为空** — `enabled()==false`、`stateZh()=="未配置"`、`state()==Disabled`、
   `th_.joinable()==false` **且** `/proc/self/status` 的 Threads 计数不增加(双重可观测);
   `askNow` 产出恰好 1 个 `AiError`("AI 未配置:配置文件里的 api_key 为空(编辑与编译不受影响)",
   `gen == generation()` 所以不会被 UI 过滤掉);50 次 `maybeAutoTrigger` 一个事件都不发、
   `in_` 队列为空;`noteActivity/cancelInFlight/toggleMode/空缓冲区` 全部不崩。
   对照组:key 非空 → 线程数 +1、`stateZh()=="就绪"`,析构后线程回收。
3. **取消语义** — 用一个"只 accept 不回字节"的本地服务器造稳定在飞态:
   (a) `noteActivity()` 后所有事件的 `gen != generation()`,且**不报错**(canceled 不是错误),
   curl 因取消回调中止耗时实测 **0~1ms**;(b) `setMode()` 后没有任何
   `gen==generation() && sink==GhostText` 的事件;(c) `cancelInFlight()` 生效、无 `AiDone`;
   (d) 排队中就被作废的请求一个事件都不发。
4. **不卡死** — 在飞时对 8 个 UI 线程入口各测 40 次,取最坏值:
   ASan+UBSan/-O1 下最坏 **2102 us**(20 轮的最大值,全部出现在 `askNow`
   —— 它要为 2000 行缓冲区拼 400 行上下文);`-O2 -DNDEBUG` 下最坏 **24 us**。
   `noteActivity/maybeAutoTrigger/stateZh/inFlight/generation/toggleMode/cancelInFlight`
   全部 0~几 us。断言线是 5ms。
5. **错误路径** — 本地:401(body 里回显完整假 key)→ "API key 无效或已过期:…",
   key 的**任意 8 字节片段**都搜不到;超时(`ai_timeout_ms=400`)→ "请求超时",404ms 返回;
   200 但内容为空 → "AI 返回内容为空";连接被直接掐断 → 中文错误。
   真实网络:不可解析域名 → "无法解析域名,检查网络或 base_url";
   假 key 打 `api.deepseek.com` → 真 401 → "API key 无效或已过期:Authentication Fails,
   Your api key: ****cdef is invalid"(服务端自己打的码,不可复原)。网络不可用时该用例
   会打印跳过原因而不是假通过(`CPPIDE_NO_NET=1` 可强制跳过)。
6. **析构安全** — 在飞时 `delete`:实测 **0~1ms**(靠取消回调中止,不是等
   `ai_timeout_ms=20000` 超时);50 轮构造/析构(其中 10 轮带在飞请求)≈5ms、
   线程计数回到基线、LSan 无泄漏;另有"队列里塞 20 个请求直接析构"的用例。
7. **限流** — 练习模式自动触发永不发请求(服务端 hits==0);连续 `noteActivity` 期间
   hits==0;空白缓冲区 hits==0;停顿够久后连调 `maybeAutoTrigger` 200 次
   **只有 1 次 hits / 1 个 AiStarted / 1 个 AiDone**;完成后立刻再触发被
   `ghost_min_interval_ms` 挡住;`askNow` 无视该间隔(hits 变 2)。
8. **后处理** — 24 条断言:围栏(有闭/无闭/围栏外解释文字/围栏后空行/行尾残留 ```)、
   重复当前行前缀(精确/去缩进/模型自加缩进/不该剪的情况)、`max_lines` 截断
   (含 `max_lines<=0` 视为不截断)、尾部空行/全空白/空串、四种情况的组合、UTF-8 中文不被切坏。
9. **上下文截断** — 1000 行 + `ai_max_context_lines=100`:`user_prompt` 含 `<CURSOR>`、
   含**两个** `/* ...省略... */`(光标在中间,上下都被截)、行数 103(≤104)、字节数 < 8000、
   光标附近的 v499/v500/v501 在、v0/v999 不在;光标在文件头/尾时只有一侧有省略标记;
   小文件逐字节比对整个 prompt;`buildContext` 单独测(prefix 末尾即光标处、
   `max_lines=1`/`0`、越界光标被 clamp、空缓冲区)。

## 关键实现决定

### 事件语义(★ 需要 app.cpp 对齐)

| 事件 | `text` 的含义 |
|---|---|
| `AiStarted` | 空。请求已开始 |
| `AiDelta` | **增量**原始文本,最多每 40ms 一个(§5.5 背压) |
| `AiDone` | **完整最终文本**:写代码模式已做完后处理(剥围栏/去重复前缀/截行/去尾空行);练习模式已把 `reasoning_content` 追加在正文后面(带"【推理过程】"小标题) |
| `AiError` | 中文文案,绝不含 key。**canceled 不发任何事件** |

`AiDone.text` 是**替换**语义,不是"再追加一段"。理由:后处理只能对全文做,而
architecture.md §509 也说 `Editor::setGhost()` 是在 `App::onAiDone` 里调的。
App 侧建议:`onAiDelta` 只做渐显(`appendGhostDelta` / `panel.appendStreaming`),
`onAiDone` 用 `ev.text` **覆盖**(ghost:`setGhost(完整文本)` 再
`markGhostComplete`;面板:替换本次流式段落)。若 App 在 Done 时改成再 append 一次,
后果是"内容显示两遍"(不是安全问题,但很难看)。**这条需要协调者和 app.cpp 对齐。**

### 其它

- `in_flight_` 由 **UI 线程**在 `submit()` 里置位、worker 收尾清零。否则"已提交但
  worker 还没跑起来"的窗口里 `maybeAutoTrigger` 会把同一个请求重复发出去
  (验收 7 的 "连调 200 次只发一个" 就是钉这条)。
- `should_cancel = [this,g]{ return gen_.load()!=g || stop_.load(); }` —— 把 `stop_`
  也放进去,于是**析构 = 立刻中止在飞传输**(实测 0~1ms),不必等 `ai_timeout_ms`。
- worker 拿到请求先查一次 `should_cancel()`:排队期间已作废的请求**一个事件都不发**
  (连 `AiStarted` 都不发),省得 UI 侧去过滤。
- `state()` 是派生的:`!enabled()→Disabled`;`in_flight_→Thinking`;否则
  `state_`(只存 Idle/Error)。所以"思考中…"不会和在飞标志打架。
- 默认模式是**练习模式**(更安全的那一档);`setMode/noteActivity/cancelInFlight`
  三个入口都先 `gen_++`。
- 上下文截断在某一侧行数不够时,把剩余配额补给另一侧(光标在文件头也能拿到 100 行上下文)。
- `scrubKey()` 在 ai.cpp 里又洗了一遍 error/正文(aihttp 已经洗过):这条"错误消息不含 key"
  的保证不依赖下层实现细节。口径与 aihttp 一致 —— key 的任意 8 字节片段都替换成 `[REDACTED]`。
- 练习模式 system = (`cfg.prompt_practice` 或内置) + `"\n"` + `practiceHardSuffix()`,
  硬后缀**无条件追加、不可覆盖**(测试里用自定义 prompt 覆盖后仍断言它在)。
- 写代码模式**完全不碰** `resp.reasoning`(端到端用例断言 AiDone 文本里没有思维链)。

## 测试设施

`tests/test_ai.cpp` 自带一个 mini HTTP 服务器(POSIX socket + poll,50ms 粒度可停):

- `hang=true`:只 accept、扣住连接不回字节 → 制造**稳定的在飞态**(测取消/不卡死/析构/超时)。
  用数字 IP,所以不受 wave3 记录的"DNS 打不断"影响。
- `resp` + `chunk`/`chunk_delay_ms`:按任意字节切分慢速吐 SSE → 测背压与分片。
- 同时留存最后一次请求原文,用来断言 **body 里没有 key、头里有 `Authorization: Bearer`**。

`#define private public`(在**所有标准库头 include 完之后**才 define,`#undef` 收尾)
用来直接断言 `sinkFor` / `buildRequest` / `buildContext` / `th_.joinable()`。
这只影响测试 TU 的访问控制,无虚函数、成员声明顺序不变,布局不受影响。

看门狗:`alarm(240)` + 每个网络等待都有毫秒上限 + 服务器线程有停止标志,
**整份测试不可能永久挂住**;`signal(SIGPIPE, SIG_IGN)`(curl 中止后往半关闭的连接写会 EPIPE)。

## 已知限制 / 风险

1. `AiDone` 的**替换语义**必须和 app.cpp 对齐(见上表)。这是唯一的跨模块未定项。
2. 后处理的围栏剥离是**行级**的:```` ```cpp int a=1;``` ```` 这种"同一行内开闭围栏"
   会被整行当成围栏丢掉。真实模型输出里没见过;要支持得再加一段解析。
3. 有围栏时,围栏**外**的解释文字被丢掉(只留第一个围栏块)。这是故意的:那些文字
   插进代码里就是语法错误。
4. `/proc/self/status` 的线程计数断言是 Linux 专属;macOS 上 `threadCount()` 返回 -1,
   该断言自动跳过,但 `th_.joinable()` 那条仍然有效。
   注:`pthread_join` 返回后内核 task 从 /proc 消失有一个极短窗口(20 轮里命中过 2 次),
   所以线程计数断言必须轮询(`waitThreadCount`),直接读会假阳性。
5. 流式下 aihttp 不填 token 用量(wave3 已记),所以 AiService 也不上报 token 数。
6. `ai_max_context_lines` 是**行数**上限,不是字节上限。单行超长(比如一行 1MB 的
   压缩代码)时 prompt 仍可能很大。真要防需要另加字节上限 —— 需求没要求,没做。

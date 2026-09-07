# Wave 3 — aihttp(HTTP/SSE 层)实现日志

产出:
- `/work/src/aihttp.cpp`(718 行)
- `/work/tests/test_sse.cpp`(418 行,22 cases / 90 checks)
- `/work/tests/test_aihttp.cpp`(790 行,20 cases / 198 checks)
- `/work/.flower/scripts/wave3-aihttp-verify.sh`(`ROUNDS=20` 可复跑全部验收)

未修改任何 `.h`。未创建/修改除上述之外的任何文件。

## 验收结果

| 项 | 结果 |
|---|---|
| `g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/aihttp.cpp` | 零 warning |
| 额外严格档 `-Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast` | 也是 0 行输出 |
| ASan+UBSan(`detect_leaks=1:detect_stack_use_after_return=1`)× 20 轮 | 20/20 通过,无 leak / 无 UB |
| 真实 401(case (c)) | 20/20 轮都真的拿到 HTTP 401 并映射成中文 |
| TSan | 本机不可用,按要求用 ASan 连跑 20 轮替代 |

单轮耗时约 10s(其中 ~3s 是真实 DNS + TLS 往返)。

## §5.6 curl 选项逐条落地

`grep -o "CURLOPT_[A-Z_]*" src/aihttp.cpp | sort -u` 共 22 项,要求的全在:

- `CURLOPT_NOSIGNAL = 1L` ✅
- `CURLOPT_TIMEOUT_MS` / `CURLOPT_CONNECTTIMEOUT_MS` ✅(connect 被 clamp 到 ≤ total;≤0 走默认 20000/5000)
- `CURLOPT_LOW_SPEED_LIMIT = 1` / `CURLOPT_LOW_SPEED_TIME = 10` ✅
- `CURLOPT_NOPROGRESS = 0` + `CURLOPT_XFERINFOFUNCTION` / `XFERINFODATA` ✅ → 回调里查
  `should_cancel()`,非 0 返回 → `CURLE_ABORTED_BY_CALLBACK`
- `CURLOPT_HTTPHEADER`:`Authorization: Bearer <key>` + `Content-Type: application/json`
  + 流式 `Accept: text/event-stream`(非流式 `Accept: application/json`)+ `Expect:`(去掉 100-continue) ✅
- `CURLOPT_VERBOSE = 0L` 恒为 0 ✅
- 写回调检测到取消 → **返回短计数 0** 中止 ✅
- 额外:`FOLLOWLOCATION/MAXREDIRS=3`、`TCP_KEEPALIVE`、`USERAGENT`、`ERRORBUFFER`
  (perform 后立刻置回 nullptr,栈缓冲不悬垂)、`DNS_CACHE_TIMEOUT=300`。
- **没设** `FAILONERROR`(4xx 的 body 正是错误信息来源);**没动** `SSL_VERIFY*`
  (保持 libcurl 默认校验,不依赖 OpenSSL/GnuTLS/SecureTransport 任一后端的特有行为)。
- 每次 `send()` 开头 `curl_easy_reset()`:句柄复用(连接池/DNS 缓存还在)但选项不跨请求残留。

## 网络错误路径实测(全部不带真 key)

| 用例 | 结果 |
|---|---|
| (a) `https://this-host-does-not-exist-cppide-test.invalid/...` | `无法解析域名,检查网络或 base_url`,elapsed ~3.2s / 热缓存 ~3ms,不崩 |
| (b) `http://127.0.0.1:1/...` | `无法连接服务器,检查网络或 base_url`,elapsed 0ms,不崩 |
| (c) 假 key 打 `https://api.deepseek.com/v1/chat/completions` | **真的 HTTP 401** → `API key 无效或已过期:Authentication Fails, ...`,20/20 轮命中,不含 key |
| (d1) `total_timeout_ms=1` 打本地慢服务器 | `timed_out=true` + `请求超时`,毫秒级返回 |
| (d2) `total_timeout_ms=1` 打真实域名 | `请求超时`,冷 DNS 下 elapsed ~3.2s(见下"已知限制") |
| (e) `should_cancel` 立刻 true | `canceled=true`、`ok=false`、**`error` 为空**、0ms 返回、进度/前置检查 polls≥1 |
| 传输中转 true(第 3 个 delta 后) | 立即中止,`canceled=true`、`error` 空,158ms |
| `on_delta` 返回 false | 同样按取消处理(上层主动中止不是错误) |
| 4 线程 × 各自 HttpChat × 20 次 | 80 次全部记账正确,不崩、LSan 无泄漏 |

## key 泄漏防线

- key 只进 `Authorization` 头。测试断言服务端收到的 **body 里没有 key、头里有 key**。
- `toJson()` 输出断言不含 key、不含 `api_key`/`Authorization` 字样。
- `send()` 所有返回路径出门前统一过 `scrubKey()`,`error`/`text`/`reasoning` 三个字段都洗。
- `scrubKey` 不只匹配完整 key,还把 key 的**任意 8 字节连续片段**替换成 `[REDACTED]`
  —— 因为实测 DeepSeek 的 401 body 会回显 `Your api key: ****c123 is invalid`。
  专门加了一个本地用例:服务端把完整 key + 截断 key 都回显,断言返回的 error 里
  key 的每一个 8 字节窗口都搜不到。
- 唯一残留:服务端**自己打码后**的 4 字符尾巴(它设计如此)会出现在文案里。
  完整 key 与任何 8 字节片段都不出现,无法复原 key。

## 没有硬编码 key / 写死模型名

- `model` 一律取 `req.model`;**`req.model` 为空时 `send()` 直接返回错误
  "未配置模型名(在配置文件里填 model)"**,绝不悄悄替换成默认模型。
- 测试断言 `toJson()` 输出里 model 等于入参的 `test-model-XYZ-9`,且全文不含
  `deepseek-chat`;换一个 model 再断言输出跟着变。
- 全文件 `grep -i "sk-\|deepseek-chat"` 无命中。

## SseParser:分片不变式

只用了头里冻结的唯一成员 `std::string carry_`,做法是 **carry_ 里只留"还没凑成
完整帧"的原始字节,只有看到空行(帧结束)才解析并回调**。因此每次 feed 之后的
内部状态只是"已消费字节前缀"的函数 → 输出与分片方式**无关**(这正是测试断言的)。

test_sse 22 个 case:1 字节切 / 随机切(两个固定种子)/ 一次性,三种投喂的
原始 payload 序列与语义事件序列必须全等。覆盖:`[DONE]`、空行与连续空行、
`event:`/`id:`/`retry:`、注释行 `:`、`\r\n` 与 `\n\r\n`/`\r\n\n` 畸形终止符、
UTF-8 中文与 emoji 被切在多字节中间、`data:` 关键字被切断、多条 `data:` 行拼一帧、
`data:` 后 0/2 个空格、1MiB 超长单帧、非法 JSON 帧(跳过不崩)、无 `data:` 前缀的
垃圾、无效 UTF-8 字节、1000 小帧顺序、半帧永不早发、`reset()`、空回调、
真实 DeepSeek 形状(role 首帧 + finish_reason 末帧 + usage)。

## reasoning_content 放在哪层(按 aihttp.h:50 的签名决定)

本层不认识 `AiMode`,所以**不做**"练习模式才追加"的策略判断:
思维链单独进 `ChatResponse::reasoning`,并且**绝不**经由 `on_delta` 下发
(`on_delta` 只喂正式 content)。于是上层 AiService:练习模式取 `resp.reasoning`
追加到面板;写代码模式只用 `resp.text` / delta,思维链自然被丢弃,
不可能被当成代码插进缓冲区。代码里与 `sseChunkToDelta` 的注释都写明了。
测试断言:SSE 流里的 `reasoning_content` 出现在 `resp.reasoning`,且不出现在任何 delta 里。

## 已知限制 / 风险

1. **DNS 解析耗时是 `send()` 的下限**(实测,libcurl 8.14.1 + 线程化解析器):
   `Curl_resolver_kill()` 会 join 解析线程,所以 `CURLOPT_TIMEOUT_MS` 打不断一次
   正在进行的 `getaddrinfo`。本机冷缓存 ~3.2s、热缓存 ~2ms。两条硬性保证不受影响:
   `send()` 一定会返回;它只在 worker 线程跑,UI 输入不卡。
   建议写进 README 的"已知限制"。
2. `SseParser` 的 carry 有 32MiB 上限(防服务端永不发空行把内存吃光)。
   超过上限会丢弃残帧 —— 此时"输出与分片无关"这条不再保证。真实响应远小于此,
   测试里最大单帧 1MiB。
3. HTTP body 累积上限 32MiB;流式模式只为诊断保留 body 前 4KiB。
4. 本层不解析流式响应末帧的 `usage`(`prompt_tokens`/`completion_tokens` 在流式下
   保持 0)。非流式路径正常填充。上层若要显示 token 数需注意。
5. `chatErrorZh` 把 403 也映射成"API key 无效或已过期"(实践中 403 多为 key 权限问题)。
   如果协调者认为 403 应有独立文案,需要改一行。
6. Makefile 的 test 目标由别的 agent 维护;我的测试用 `#include "../src/aihttp.h"`,
   不依赖 `-Isrc`。链接需要 `-lcurl -lpthread`(Makefile 里已有)。
   `test_aihttp.cpp` 额外用了 POSIX socket(起本地 mini HTTP server)与 `<thread>`。

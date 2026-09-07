# Wave 2 · config.cpp 实现日志

## 产出
| 文件 | 行数 | 说明 |
|---|---|---|
| `/work/src/config.cpp` | 410 | `Config::chatUrl` + `ConfigLoader::{defaultPath,load,sampleJson}` |
| `/work/tests/test_config.cpp` | 804 | 纯断言,417 条 CHECK,`main` 返回 0 = 通过 |
| `/work/.flower/scripts/wave2-config-verify.sh` | — | 一键验收(编译零 warning + ASan/UBSan 跑测 + -O2/NDEBUG 复跑 + `sk-` 源码检查) |

`src/*.h` 一个字节都没动(`src/config.h` md5 = `faf8e7d27118def6bcfded6de9669c75`)。

**依赖处理**:先用自写的临时 json/util 桩(放在 `.flower/scratch/wave2cfg/`)跑通;
随后并行 agent 的 `src/json.cpp` 与 `src/util.cpp` 落地,**已改用真实现重跑并全绿**,
临时桩随即删除。最终验收数据全部来自真实的 `src/json.cpp` + `src/util.cpp`。

## 验收结果
```
== 1. 验收编译:-O2 -Wall -Wextra,必须零 warning ==   OK(0 行输出)
== 2. ASan+UBSan 下编译并运行 tests/test_config.cpp == test_config: OK (417 条断言)
== 3. -O2 -DNDEBUG 再跑一遍                          == test_config: OK (417 条断言)
== 4. 源码硬性检查:不许有 sk- 字面量                  == OK
ALL PASS
```
ASan 开了 `detect_leaks=1`,UBSan 开了 `halt_on_error=1`,均无报告。

## 配置字段:与 architecture.md §7 的对应关系
JSON 用**扁平顶层键,键名与 `Config` 成员名逐字相同**,共 31 个可配置字段,
与 §7 的 struct 一一对应,**无增、无删、无改名**。`source_path` / `warnings` 是元信息,
不从文件读。以 `_` 开头的键一律当注释跳过(`sampleJson()` 用 9 个 `_comment*` 键写说明,
因为 JSON 标准不支持 `//`)。未知键 → 一条 warning,不影响其它字段。

## 取值与路径优先级(实现细节)
- 路径:`--config`(load 的 path 参数,非空即用)> `CPPIDE_CONFIG` > `$XDG_CONFIG_HOME/cppide/config.json` > `$HOME/.config/cppide/config.json`;两条来源都过 `util::expandUser`(只展开开头的 `~/`)。`HOME` 也取不到时 `util::homeDir()` 回落 getpwuid,再不行用 `.`。
- 取值:默认 < 文件 < 环境变量。环境变量的**空串/纯空白视为未设置**(否则 `CPPIDE_MODEL=` 会把文件里配好的模型抹成空)。`CPPIDE_API_KEY` 优先于 `DEEPSEEK_API_KEY`。

## 降级矩阵(全部有测试)
| 输入 | 行为 |
|---|---|
| 文件不存在 / 路径是目录 / 读失败 / 无法确定路径 | 全默认,`source_path=""`,`err` 非空 + warning 写明路径 + 一条 `--print-config` 生成示例的提示 |
| JSON 语法错(缺 `}`、多逗号、空文件、纯空白、二进制垃圾、`//` 注释、单引号、裸键、字符串未闭合、内嵌 NUL) | 全默认 + 带解析器行列号的 warning,`source_path=""` |
| 顶层不是对象(`[1,2,3]` / `"s"` / `42` / `null`) | 全默认 + warning |
| 单字段类型错 | 该字段用默认 + 点名 warning,**其余字段照常生效**(测试里 `panel_height/cxx/cxxflags/show_line_numbers/model` 同文件内仍然生效) |
| 数值越界 / 非有限 / 小数给整数字段 | clamp 到区间(或截断)+ warning,见下表 |
| `base_url`/`chat_path`/`cc`/`cxx`/`model` 配成空串 | 回落默认 + warning(否则会 `exec("")` 或请求空 URL) |
| 数组字段给了非数组 | 用默认 + warning;给 `[]` 是**合法**的“不加任何参数”,不报警;元素含非字符串 → 跳过该元素 + warning,其余保留 |
| 同一键重复出现 | 后者生效(由 `mj::Value::set` 覆盖语义决定) |
| 500 个未知字段 | warnings 有上界 40(+1 条“其余已省略”),不淹 UI |

### clamp 区间
`ghost_delay_ms`[0,60000] · `ghost_min_interval_ms`[0,600000] · `ghost_max_lines`[1,200] ·
`ai_connect_timeout_ms`[100,300000] · `ai_timeout_ms`[100,600000] · `ai_max_context_lines`[1,100000] ·
`max_tokens_code`/`max_tokens_practice`[1,32768] · `temperature_*`[0.0,2.0] ·
`compile_timeout_ms`[1000,600000] · `run_timeout_ms`[100,600000] ·
`run_output_limit`[1024, 268435456] · `tab_width`[1,16] · `panel_height`[3,100] · `tick_ms`[10,1000]。
越界一律 clamp 并告警(不静默),边界值本身不告警。

## 安全
- 源码里没有任何 key 字面量:测试直接读 `src/config.cpp`,断言不含 `sk-`、`sk_`、`Bearer `,
  且**最长的 `[A-Za-z0-9+/=_-]` 连续串 < 28 字符**(真 key 一般 ≥ 35),从结构上排除硬编码。
- `api_key` 的值不进任何 `warning`/`err`:相关分支只提字段名。测试用独特值(`Zq1AAA…`)
  从文件与环境变量两条路径灌进去,断言所有 warning 与 err 都不含其片段。
- `api_key` 只允许可见 ASCII(`0x21..0x7e`);粘贴带进来的换行/空格会先 trim,
  中间仍有空白或控制字符则整条丢弃 + 告警(避免 libcurl 在运行期报怪错),内容不打印。
- `model` 若等于内置占位默认值,warnings 里明确“请按 DeepSeek 官方文档填写实际模型名”;
  `sampleJson()` 的 `_comment_model` 也写了同一句。`sampleJson()` 的 `api_key` 恒为 `""`。
- `sampleJson()` 的所有取值都来自默认构造的 `Config`,函数体内不写死任何具体值 ——
  默认值一改示例自动跟着改,不会出现“示例与实现不一致”。

## chatUrl() 斜杠矩阵(已测 4×4 + 6 个特例)
`base ∈ {无尾斜杠, 一个, 两个, 带首尾空格}` × `path ∈ {无前斜杠, 一个, 两个, 带空格}`
→ 全部得到 `https://x.com/v1/chat/completions`。另测:带端口子路径
(`http://127.0.0.1:8080/api/` + `/v1/chat/completions`)、`chat_path` 为空、`chat_path` 为 `///`、
`base_url` 为空、两者都空、以及默认配置拼出的 URL 在 `://` 之后不含 `//`。

## 未验证 / 风险
1. `err` 的语义我定为“只装致命级说明”(文件缺失 / 读失败 / JSON 语法错 / 顶层非对象),
   单字段类型错**不**写进 `err`,只进 `warnings`。若 main.cpp 作者期望 `err` 也带上字段级
   问题,需要协调者定一句。
2. clamp 的具体上下界是我定的(§7 没规定)。如果 UI 层对 `panel_height` 上限有更强假设
   (比如必须小于终端高度),那是 `ui.cpp` 的 clamp 责任,这里只保证不是负数/不是 0。
3. `run_output_limit` 上限 256 MiB 是为防 `size_t` 溢出与 OOM 拍的;若真有人要更大得改这里。
4. `sampleJson()` 的往返自洽只依赖“自己能解析回来”,不依赖 `dump(2)` 的具体空白格式;
   若 `json.cpp` 以后改缩进风格,本测试不会红,但 README 里贴的示例需要同步重新生成。
5. 没有测“配置文件无读权限”(容器里以 root 跑,`chmod 000` 照样读得到),
   该分支走的是 `util::readFile` 失败 → 全默认 + warning,与“读失败”同一条路径。

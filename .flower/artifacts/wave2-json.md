# Wave 2 —— src/json.cpp 实现日志

产出:
- `/work/src/json.cpp`(704 行)
- `/work/tests/test_json.cpp`(728 行,21 个用例组 / 3081 处断言)
- `/work/.flower/scripts/wave2-json-verify.sh`(可重复跑的验收脚本)

未修改任何 `.h`(`src/json.h` md5 = 1f702d8c2bada41da3c57998008a2481,mtime 未变)。

## 验收结果

| 项 | 结果 |
|---|---|
| `g++ -std=c++17 -O2 -Wall -Wextra -c src/json.cpp` | 零输出、零 warning |
| 附加 `-Wpedantic -Wshadow` | 零输出 |
| `-O2` 跑 test_json | 通过(21 组 / 3081 断言) |
| `-O1 -g -fsanitize=address,undefined` + LSan(detect_leaks=1, strict_string_checks=1, detect_stack_use_after_return=1) | 通过,无任何报告 |
| `-O0 -g` | 通过 |

## 关键实现决策(需要 review 的都在这里)

### 1. 不改头也能让 int64 精确
json.h 冻结了成员布局,数值只有一个 `double num_`。为了让
`9223372036854775807` 既精确又能原样 dump,复用了已有成员的语义
(**没有新增任何成员**):

- `Type::Number` 且 `b_ == true` ⇒ `str_` 里是该整数的精确十进制文本。
  只由 `Value(int)` / `Value(int64_t)` 产生,`asInt64()` 直接 `from_chars`
  读 `str_`,完全不经过 double;`dump()` 也直接吐 `str_`。
- `b_ == false` ⇒ `str_` 空,`dump()` 走 `std::to_chars(double)` 的最短往返表示。
- `b_` 对 Number 的这层含义与 Bool 不冲突:`asBool()` 只在 `t_==Bool` 时读 `b_`。

解析器里纯整数且能塞进 int64 的字面量走 `Value(int64_t)`;例外是 `-0`,
它交给 double 分支以保住符号位(否则会 dump 成 `0`)。

`dump()` 的递归体写成**成员函数内部的局部类** `struct D`,这样它合法地拥有
与外围成员函数相同的访问权限,能读 `b_/str_/num_/keys_/vals_` —— 因此不需要给
json.h 加任何私有辅助成员。

### 2. 全程不依赖 locale
数字进出都用 `<charconv>`(`from_chars` / `to_chars`),**不用 strtod / %g**。
原因:上层为 ncurses 会调 `setlocale(LC_ALL, "")`,若在 `LC_NUMERIC=de_DE`
下用 `%g` 会吐出 `1,5` 这种非法 JSON,是个很难查的线上 bug。

### 3. 深度上限 200
`kMaxParseDepth = 200`(容器嵌套层数)。先判断再递归,所以一万层 `[[[[…`
是"返回错误",不是爆栈。实测边界:199 层成功,201 层失败并报
"嵌套层数超过上限(200)"。`dump()` 另有 512 层上限(防上层用 push/set
手工造病态深树),超限处截断为 `null`。

### 4. 数字上溢报错、下溢当 0
`from_chars` 对上溢和下溢都返回 `result_out_of_range`,无法区分。
用 `normExp10()`(只在这个罕见分支里跑)从字面量估算十进制阶来判方向:
- `> 0` ⇒ 上溢 ⇒ 报错"数字超出双精度可表示范围"(`1e309` / 400 个 9 / 一万位整数都在这)
- `<= 0` ⇒ 下溢 ⇒ 静默取 ±0.0(`1e-400` ⇒ 0,不报错)

### 5. 刻意宽容的两处(与严格 RFC 8259 不同)
- **孤立代理**(`"\ud83d"` 单独出现、或高代理后面不是低代理)译成 `U+FFFD`
  而**不报错**。理由:这是 LLM 流式输出被切断时的常见形态,报错会让整块
  响应作废;换成 U+FFFD 用户只看到一个替换字符。合法代理对正常合成 4 字节 UTF-8。
- **UTF-8 BOM** 会被跳过(用户手编 config.json 时常见)。

严格的地方:不接受注释、尾随逗号、单引号、`NaN`/`Infinity`、前导零(`01`)、
`+1`、`.5`、`1.`、字符串里的裸控制字符;值之后有多余内容一律报错。

### 6. 错误串格式
`第 L 行第 C 列:<中文原因>`。C 是**字节**列(不是显示列)。只保留第一个
(最内层)错误。失败时 `parse()` 一律返回 null Value 且 `err` 非空;
成功时 `err.clear()`。

### 7. 其它语义澄清
- `set()` 同键覆盖且**不改变原有顺序**;`push()`/`set()` 在类型不匹配时先重置成
  空数组/空对象。
- `clearContents()` 清空 keys_/vals_/str_ 并把 num_=0、b_=false(保留 `t_`)。
- NaN / ±Inf 的 Number `dump()` 成 `0`(JSON 没有这些字面量,保证产物永远合法)。
- `asInt64()` 对 double 做夹取(`±2^63` 用精确 double 常量比较),避免
  double→int64 越界转换的 UB —— 这是 UBSan 最常抓到的点。

## 测试覆盖(21 组)
标量/取值默认值、越界与错类型访问全安全、对象保序覆盖、空白与 BOM、
往返(18 个文档 × 紧凑 + indent=2 两种 dump)、DeepSeek 三种响应体
(delta.content / message.content / error.message,含 `[DONE]` 哨兵必须失败)、
请求体生成转义(断言 dump 产物里不含任何裸控制字符)、`escape()` 逐项、
`\uXXXX` 与代理对(含 U+10FFFF 边界、5 种孤立代理形态)、
78 条恶意/畸形输入全部必须失败、深嵌套(1e4 数组 / 1e4 对象 / 5e4 混合)、
超长病态数字、数字边界(`-0` 保符号位、`1e308`、int64 极值精确、`1.5e-10`)、
缩进美化逐字节比对、拷贝/移动/子树自插入、config.json 端到端(错误带行号)、
字符串内嵌 NUL 的二进制安全、深度上限边界(199/201)、
**确定性 fuzz(3335 个变体:全部前缀截断 + 13 种讨厌字节的单点变异 + 单字节删除)**。

## 遗留风险
1. **`at("a/0/b")` 路径查找没有实现** —— json.h 冻结版里没有这个签名,
   加它必须改头。全仓 grep 也没有任何调用方(config.cpp / aihttp.cpp 用链式
   `["a"][0]["b"]` 即可,越界安全)。若协调者确认要,需要在 json.h 加
   `const Value& at(const std::string& path) const;`。
2. 非法 UTF-8 字节序列按 json.h 的"原样透传"契约不做校验也不替换 —— 若上层
   把非法 UTF-8 塞进字符串,dump 出的 JSON 在字节层面就不是合法 UTF-8
   (ncurses 渲染侧需自行防护;DeepSeek 端到端不受影响,因为 API 回的是合法 UTF-8)。
3. 错误串里的"列"是字节列;含中文的行报出来的列号会大于视觉列。
4. `asNumber()` 对精确 int64(如 `9223372036854775807`)返回的是四舍五入后的
   double(`9223372036854775808.0`)—— 这是 json.h "数字统一用 double 存"
   的固有结果,要精确值必须用 `asInt64()`。
5. 内存分配失败(bad_alloc)不在 no-throw 契约内 —— 那已是进程级问题。

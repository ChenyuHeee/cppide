# cppide 实现架构设计

> 依据:`.flower/notes/需求.md`(冻结)。本文件是实现期的唯一接口权威。
> 头文件在 Wave 1 冻结,之后任何签名变更必须由协调者统一改。

## 0. 技术决策摘要

| 项 | 决策 | 理由 |
|---|---|---|
| 语言 | C++17 | macOS 自带工具链可直接编译 |
| 渲染 | ncurses(宽字符)| 系统自带;AI 面板要显示中文,必须走 `wget_wch`/`waddnwstr` |
| HTTP | libcurl | macOS 自带 |
| JSON | 自写 `src/json.{h,cpp}` | 不联网下载、不引外部依赖 |
| 构建 | 手写 Makefile | 不引 CMake |
| 平台 API | 仅 POSIX + ncurses + libcurl | 无 macOS 独有 API,可在 Linux 容器编译验证 |
| 可执行名 | `cppide` | `icpc` 会与 Intel 编译器冲突,`cpp` 是预处理器;`cppide` 无冲突 |
| 选区 | **不做** | 需求未要求;终端自身的鼠标选择 + Cmd-C 已够。改为行级操作(剪切行/粘贴行/复制行) |
| 子进程 | `fork`+`execvp`+`poll`,**绝不用 popen/system** | 需要 pid(超时 kill)、双向管道、stdout/stderr 分离、退出状态、不经过 `/bin/sh` |

> 开发期注意:Linux 容器需先 `apt-get install build-essential libncursesw5-dev libcurl4-openssl-dev`。

---

## 1. 文件划分

```
Makefile
README.md
src/
  main.cpp        90    进程入口:locale / SIGPIPE / curl global / endwin 安全网 / argv
  app.h  150      AppModel + App 类声明
  app.cpp 570     主循环、按键分派、事件排空、编译/运行/AI 编排
  ui.h   140      Ui 渲染器声明 + Theme
  ui.cpp 570      ncurses 初始化与全部绘制(编辑区/行号/ghost/面板/状态栏/提示行/帮助)
  editor.h 180    Editor:光标、视口、编辑操作、Ghost
  editor.cpp 540
  textbuf.h 150   TextBuffer:行数组 + 撤销
  textbuf.cpp 430
  highlight.h 90  Highlighter(纯函数)+ HighlightCache(行首状态缓存)
  highlight.cpp 400
  panel.h 120     Panel(输出面板)+ StdinBuffer
  panel.cpp 300
  proc.h 130      ProcSpec / ProcResult / runProcess / Runner(后台线程)
  proc.cpp 420
  build.h 130     Diag / Builder:命令装配 + 诊断解析
  build.cpp 380
  aihttp.h 100    ChatRequest / ChatResponse / HttpChat / SseParser
  aihttp.cpp 380
  ai.h   160      AiMode / AiSink / AiRequest / AiService
  ai.cpp 430
  config.h 120    Config + ConfigLoader
  config.cpp 270
  json.h 130      mj::Value 声明
  json.cpp 420    极简 JSON 实现
  mailbox.h 140   Mailbox<T> 线程安全队列(header-only)
  keys.h 130      Action 枚举 + 绑定表 + Ctrl()/Alt()
  keys.cpp 190
  util.h 90 / util.cpp 230   字符串/路径/文件/时间/UTF-8
tests/
  test_json.cpp test_highlight.cpp test_diag.cpp test_sse.cpp test_proc.cpp  ~300
```

模块依赖(单向,无环):

```
main -> app -> {ui, editor, panel, ai, build, proc, config, keys}
ui   -> {editor, panel, highlight, keys, util, app.h(仅 AppModel)}
ai   -> {aihttp, config, textbuf, mailbox, json}
build-> {proc, config, util}
editor->{textbuf, highlight, util, ai.h(仅 AiSink 枚举)}
aihttp->{json, config, util}
config->{json, util}
textbuf/highlight/panel/proc/json/util -> 仅 util 或无依赖
```

`json.h` 拆成 `.h`+`.cpp`(而非纯 header-only)是为了让 Wave 2 的多个 agent 能并行、且不拖慢每个 TU 的编译;"不引外部依赖"的要求仍然满足。

---

## 2. 核心数据结构

### 2.1 文本缓冲区:行数组 + 逆操作日志

`std::vector<std::string> lines_`,永远至少 1 行。理由:单文件 ICPC 源码不过几千行,插入/删除行的 O(n) memmove 完全不可感知;gap buffer / piece table 的复杂度换不来任何用户可感的收益,而且行数组让语法高亮、渲染、行号跳转都变直接。

```cpp
struct Pos { int line = 0; int col = 0; };   // col = 行内字节偏移(非显示列)
bool operator==(Pos,Pos); bool operator<(Pos,Pos);
struct Range { Pos a, b; };                  // 约定 a <= b(规范化后)

enum class Lang : uint8_t { C, Cpp, Unknown };

class TextBuffer {
public:
  TextBuffer();
  void reset(std::vector<std::string> lines);
  bool loadFile(const std::string& path, std::string& err);
  bool saveFile(const std::string& path, std::string& err);   // 原子写:tmp + rename

  int  lineCount() const;
  const std::string& line(int i) const;     // 越界安全,返回空行静态量
  int  lineLen(int i) const;                // 字节数
  std::string text() const;
  std::string textRange(const Range& r) const;

  // 唯一两个改动存储的原语,二者都记录撤销
  Pos insert(Pos at, const std::string& text);   // 处理 '\n' 拆行,返回结束位置
  Pos erase(const Range& r);                     // 返回 r.a

  void beginGroup(const char* label);
  void endGroup();
  struct Edit {                                   // RAII
    Edit(TextBuffer& b, const char* label); ~Edit();
  };
  bool undo(Pos& cursorOut);
  bool redo(Pos& cursorOut);

  bool dirty() const;  void clearDirty();
  int  revision() const;                    // 每次改动 +1 -> 高亮/AI 失效判据
  const std::string& path() const;  void setPath(std::string p);
  Lang lang() const;                        // 由扩展名推断
};
```

**撤销:逆操作日志 + 分组。**

```cpp
struct UndoOp   { bool isInsert; Pos at; std::string text; };
struct UndoGroup{ std::vector<UndoOp> ops; Pos cursorBefore, cursorAfter;
                  int64_t stamp; const char* label; };
```

- `insert` 记录 `{isInsert=true, at, text}`;`erase` 记录 `{isInsert=false, at=r.a, text=被删内容}`。
- 撤销 = 倒序遍历组内 ops,施加逆操作,期间**挂起日志记录**(`journaling_ = false`),同时把该组压入 redo 栈。
- 合并规则:同组内连续单字符插入,若位置相邻、间隔 < 800ms、label 相同,则并入当前组;回车、光标移动、保存、剪切行等结构性操作立刻关闭当前组。
- 任何新改动清空 redo 栈。撤销栈上限 2000 组,超出丢最旧。
- 一次用户动作 = 一个 `TextBuffer::Edit` 作用域,所以"接受 ghost 插入 8 行"是一次撤销就回退干净。

### 2.2 光标 / 视口

光标 `Pos{line, byteCol}`,外加 `Editor::desired_display_col_`:上下移动时保持期望显示列,穿过短行再回到长行不会丢列——这是所有真编辑器的行为。

显示列由 `util` 计算:Tab 展开到 `tab_width` 制表位,宽字符按 `wcwidth` 计 2。

```cpp
struct Viewport { int top = 0, left = 0, h = 0, w = 0; };
```

`ensureCursorVisible()`:纵向保留 2 行 scrolloff;横向按 8 列跳步滚动,光标距右边缘至少 4 列。**编辑区不折行**(长行横向滚动),避免"逻辑行 vs 视觉行"的全套复杂度;**面板折行**(中文 AI 输出必须折)。

### 2.3 选区

不实现。替代能力:`Ctrl-K` 剪切当前行到内部剪贴板(连续按累积)、`Ctrl-V` 粘贴行、`Ctrl-D` 复制当前行。`Range` 仍然存在,因为 `erase` 内部需要它。

---

## 3. 语法高亮

设计原则:**逐行纯函数扫描 + 1 字节行首状态**。

```cpp
enum class Tok : uint8_t {
  Normal, Keyword, Type, Preproc, String, Char, Number,
  Comment, Operator, Func, TodoInComment
};

// 行首跨行状态,打包进 1 字节
enum : uint8_t {
  ST_NONE        = 0,
  ST_BLOCKCOMMENT= 1 << 0,   // 处于 /* ... */ 中
  ST_RAWSTRING   = 1 << 1,   // 处于未闭合的 R"(...)" 中(近似)
  ST_CONTLINE    = 1 << 2,   // 上一行以 '\' 结尾(续行)
};

struct Span { int start; int len; Tok tok; };   // 按字节的 run-length

class Highlighter {
public:
  explicit Highlighter(Lang lang);
  void setLang(Lang l);
  // 纯函数:给定行首状态,填充 spans,返回行尾状态
  uint8_t scanLine(const std::string& line, uint8_t entryState,
                   std::vector<Span>& out) const;
  static bool isKeyword(const std::string& w, Lang l);
  static bool isTypeWord(const std::string& w, Lang l);
};

class HighlightCache {
public:
  void resize(int lineCount);
  void invalidateFrom(int line);
  uint8_t entryState(const TextBuffer& b, const Highlighter& h, int line);
  void spansFor(const TextBuffer& b, const Highlighter& h, int line,
                std::vector<Span>& out);
private:
  std::vector<uint8_t> entry_;   // entry_[i] = 第 i 行行首状态
  int validUpTo_ = 0;
};
```

- 缓存只存行首状态:1 字节/行,10000 行 = 10KB。
- 编辑第 N 行 → `invalidateFrom(N)`。渲染只需要 `[0 .. top+h]` 的状态,惰性向下扫描;**收敛提前退出**:重算到某行时若新状态等于旧缓存状态,后续全部不变,立刻停止。所以在文件中间敲 `/*` 也不会全文重扫。
- 词法细节:
  - 预处理:行首(跳过空白)为 `#` → `#` 与指令词染 `Preproc`,其后继续常规扫描;`#include` 后的 `<...>` 当 `String`。
  - `//` 到行尾 `Comment`(以 `\` 结尾则置 `ST_CONTLINE`)。
  - `/* */` 用 `ST_BLOCKCOMMENT`。注释内的 `TODO`/`FIXME`/`XXX` 染 `TodoInComment`。
  - 字符串 `"..."` 处理 `\` 转义;未闭合染到行尾但**不泄漏状态**(C 字符串不能跨行,除续行)。
  - **原始字符串**:`R"delim(...)"` 的分隔符是变长的,无法塞进 1 字节。决策:仅支持单行内的原始字符串;未闭合时置 `ST_RAWSTRING`,后续行整行当 `String` 直到出现 `)"`。这是**有意的近似**,自定义分隔符且体内含 `)"` 时会误染——ICPC 代码里基本不出现,README 记为已知限制。
  - 数字:`0x` / `0b` / 十进制 / `.` / `e±` / 后缀 `uUlLfF` / C++14 数字分隔符 `'`(在数字扫描器内部消化,避免与字符字面量的 `'` 冲突)。
  - 字符字面量 `'a'` / `'\n'` / `'\x41'`。
  - 标识符查关键字表与类型表(两张 `static const std::unordered_set<std::string>`,首次使用惰性构造)。类型表除内建类型外收入常用 STL 名(`string vector map set pair queue priority_queue size_t int64_t ll` 等),让竞赛代码颜色好看。
  - 标识符后紧跟 `(` → `Tok::Func`(便宜的观感提升)。
- 颜色对:8 个 pair;`COLORS >= 256` 时 ghost 用 gray(color 8),否则 `A_DIM`。

---

## 4. 编译 / 运行子系统(重点:绝不挂死编辑器)

### 4.1 为什么是 fork+exec 而不是 popen

`popen` 只能单向、拿不到 pid(无法超时 kill)、无法分离 stdout/stderr、无法拿到真实退出状态、且经过 `/bin/sh`(路径含空格就出事)。我们四项都需要,所以自己 fork。

```cpp
struct ProcSpec {
  std::string prog;                       // execvp 走 PATH 解析
  std::vector<std::string> args;          // 不含 prog
  std::string cwd;                        // "" = 继承
  std::vector<std::string> env_extra;     // "K=V";编译时必带 "LC_ALL=C"
  std::string stdin_data;                 // "" -> 立即 EOF
  int    timeout_ms = 5000;               // <=0 表示不限(仅编译用大值)
  size_t max_stdout = 1u << 20;
  size_t max_stderr = 1u << 20;
  size_t hard_kill_bytes = 8u << 20;      // 超此量直接 kill,不等超时
};

enum class ProcOutcome { Ok, Timeout, SpawnFailed, Killed };

struct ProcResult {
  ProcOutcome outcome = ProcOutcome::SpawnFailed;
  int  exit_code = -1;
  bool signaled = false;  int signal = 0;
  std::string stdout_text, stderr_text;
  bool stdout_truncated = false, stderr_truncated = false;
  int64_t wall_ms = 0;
  std::string spawn_error;                // execvp 失败的 strerror
  std::string summary() const;            // "退出码 0 · 用时 12ms" / "超时 5000ms,已终止"
};

// 阻塞式。**必须**在工作线程调用,永远不要在 UI 线程调用。
ProcResult runProcess(const ProcSpec& spec);
```

**实现契约(逐条照做,这是最容易出 bug 的文件):**

1. 建 4 个管道:stdin / stdout / stderr / **exec 状态管道**。全部 `FD_CLOEXEC`。exec 状态管道是标准手法:子进程 `execvp` 失败时写入 `errno` 然后 `_exit(127)`;父进程读到 EOF 即说明 exec 成功。fork 之后子进程无法回写父进程内存,这是唯一可靠的报错通道。
2. `fork()`。子进程:
   - `setpgid(0, 0)` —— 自己成为进程组长。这样父进程可以 `kill(-pgid, ...)`,**连带杀掉失控程序自己 fork 出来的孙子进程**。
   - `dup2` 三个管道到 0/1/2,关掉所有其它 fd。
   - `SIGPIPE`/`SIGINT`/`SIGTERM` 恢复 `SIG_DFL`(父进程忽略了 SIGPIPE,不能继承给子进程)。
   - `chdir(cwd)`(若非空)、`setenv` env_extra、`execvp`。
3. 父进程:关掉子端 fd,三个 fd 设非阻塞,进入 `poll()` 循环,超时值由截止时刻推算:
   - **写 stdin** 分块写,写完立刻 `close(fd0)`。必须优雅处理 `EPIPE` / `POLLERR`:子进程不读 stdin 就退出是**正常情况**,不是错误。
   - **读 stdout/stderr**:到达上限后**继续排空到丢弃缓冲**,只置 `truncated` 标志。**绝不能停止读取**——管道写满会让子进程永久阻塞,看起来就是"挂死"。若累计超过 `hard_kill_bytes`,立即 kill(狂喷 GB 输出的程序没有等超时的意义)。
   - **超时**:`kill(-pgid, SIGTERM)` → 等 200ms → `kill(-pgid, SIGKILL)`,期间继续排空直到 EOF。
4. 两个输出管道都 EOF(或已 SIGKILL)之后才 `waitpid(pid, &st, 0)`。**先 waitpid 再排空 = 经典死锁**。
5. 全程不装 `SIGCHLD` handler(我们总是 waitpid 指定 pid),避免干扰 ncurses / libcurl。
6. `main()` 里一次性 `signal(SIGPIPE, SIG_IGN)`(libcurl 也需要)。

### 4.2 异步包装

```cpp
enum class JobKind { Compile, Run };

class Runner {                      // 一个工作线程 + 作业队列,作业串行
public:
  using JobId = uint64_t;
  explicit Runner(Mailbox<AppEvent>* out);
  ~Runner();                        // 关队列并 join,绝不 detach
  JobId submit(ProcSpec spec, JobKind kind);
  void killInFlight();              // 读原子 pgid 并 kill(-pgid, SIGKILL)
  bool busy() const;
private:
  std::atomic<pid_t> cur_pgid_{0};  // 工作线程 spawn 后写入
};
```

**"死循环程序挂死编辑器"这个问题有三道独立防线**:
1. 子进程从不在 UI 线程跑 —— UI **在结构上不可能**被挂住,主循环照常 60ms 一跳。
2. `timeout_ms`(默认 5000)+ 输出上限 + `hard_kill_bytes`。
3. 用户逃生口:运行进行中,在【运行】面板按 **Esc** 立即 `killInFlight()`。注意终端里 Ctrl-C 不可用(我们 raw 模式且不绑它),所以必须提供这个显式键,并写进状态栏提示:`运行中… Esc 终止`。

### 4.3 编译与诊断

```cpp
enum class DiagSev { Error, Warning, Note, Info };

struct Diag {
  std::string file; int line = 0; int col = 0;
  DiagSev sev = DiagSev::Error;
  std::string message;
  int out_line = 0;                 // 在编译面板中的行号,用于双向定位
};

struct CompileOutcome {
  bool ok = false;
  std::string binary_path;
  std::vector<Diag> diags;
  int errors = 0, warnings = 0;
  ProcResult proc;
};

class Builder {
public:
  explicit Builder(const Config& cfg);
  // 纯函数,可单测,无副作用
  ProcSpec compileSpec(const std::string& src, Lang lang,
                       const std::string& out_path) const;
  ProcSpec runSpec(const std::string& binary,
                   const std::string& stdin_data) const;
  static std::string tempBinaryPath(const std::string& src);   // $TMPDIR/cppide-<hash>-<name>
  static std::vector<Diag> parseDiagnostics(const std::string& stderr_text);
  static bool binaryStale(const std::string& src, const std::string& bin);  // mtime
};
```

- 语言选择:`.c` → `cfg.cc` + `cflags`;`.cpp/.cc/.cxx/.C` → `cfg.cxx` + `cxxflags`;`.h/.hpp` → `cxx` + `-fsyntax-only`,不产出二进制,面板提示"头文件仅做语法检查"。
- 编译命令 = `cxx` + flags + `-o out` + src。**不经过 shell**,参数逐个进 argv,路径含空格天然安全。
- 子进程 env 带 `LC_ALL=C`,强制编译器输出英文 `error:`/`warning:`,否则中文 locale 下诊断解析全部失效。这是很容易漏的一条。
- **诊断解析不用 `<regex>`**(编译慢、体积大):手写解析器,从左往右找第一个 `:<数字>` 作为行号边界(路径本身可能含 `:`),然后可选的 `:<数字>` 列号,再取 `error|warning|note|fatal error` 严重度,余下为 message。`In file included from …` 与 `^~~~` 指示行原样进面板但不计入 `diags`。
- 跳行:编译面板每行携带 `diag_index`;面板聚焦时 `Enter` 跳到该诊断;不聚焦时 `Ctrl-N`/`Ctrl-P` 顺序遍历诊断。只有 `file` 的 basename 与当前缓冲区一致的诊断可跳(需求只管单文件)。

### 4.4 运行

- stdin 来源:配置了 `stdin_file` 则读文件,否则用【输入】面板里编辑的内容(`StdinBuffer`,内部就是一个 `TextBuffer`,免费获得编辑与撤销)。
- 结果进【运行】面板:stdout 常规色,stderr 红色,末尾一行 `ProcResult::summary()`(退出码 / 信号名 / 用时 / 是否截断)。
- `Ctrl-R` 时若 `binaryStale()` 或不存在,自动先编译;编译失败则不运行,并自动聚焦编译面板。

---

## 5. AI 子系统

### 5.1 线程模型

- **一个** worker 线程,仅在 `cfg.aiEnabled()` 时创建。它阻塞在 `in_.waitPop(req, 200ms)` 上。
- UI 线程与 worker 之间只有两个 `Mailbox`:请求进、事件出。UI 线程**只用 `tryPop`**,永不阻塞。
- 主循环用 `timeout(60)` 让 `getch()` 在空闲时每 60ms 返回 `ERR`,以此驱动 500ms 停顿判定。**不用 `halfdelay()`** ——它只有 100ms(十分之一秒)粒度且强制 cbreak 语义。60ms 唤醒的空闲 CPU 接近 0,因为 ncurses 底层就是在 tty 上 poll。

```cpp
enum class AiMode { Practice, Code };          // 练习模式 / 写代码模式
enum class AiSink { PanelOnly, GhostText };    // 回复"允许变成什么"

struct AiRequest {
  uint64_t gen;
  AiMode   mode;
  AiSink   sink;
  std::string system_prompt, user_prompt;
  double temperature; int max_tokens; bool stream;
};

class AiService {
public:
  AiService(const Config& cfg, Mailbox<AppEvent>* out);
  ~AiService();                               // 取消 + join

  bool   enabled() const;
  AiMode mode() const;  void setMode(AiMode m);  AiMode toggleMode();

  void noteActivity();                        // 任意改动/移动光标时调用:记时间 + gen_++
  void maybeAutoTrigger(const TextBuffer& b, Pos cur);   // 每 tick 调用一次
  void askNow(const TextBuffer& b, Pos cur, const std::string& question);
  void cancelInFlight();
  uint64_t generation() const;
  bool inFlight() const;
  const char* stateZh() const;                // "就绪"/"思考中…"/"未配置"/"错误"

private:
  static AiSink sinkFor(AiMode m) {           // ★ 全工程唯一的 sink 决策点
    return m == AiMode::Code ? AiSink::GhostText : AiSink::PanelOnly;
  }
  AiRequest buildRequest(const TextBuffer&, Pos, const std::string& q, AiMode) const;
  static void buildContext(const TextBuffer&, Pos, int max_lines,
                           std::string& prefix, std::string& suffix);
  void workerLoop();
  std::atomic<uint64_t> gen_{0};
  std::atomic<bool> in_flight_{false};
  std::atomic<int64_t> last_activity_ms_{0};
  Mailbox<AiRequest> in_;  Mailbox<AppEvent>* out_;
  std::thread th_;
};
```

### 5.2 取消:generation 序号(双重过滤)

- `gen_` 是 `std::atomic<uint64_t>`。任何缓冲区改动或光标移动 → `noteActivity()` → `gen_.fetch_add(1)`。
- worker 拿到 `req.gen` 后,把 `[this, g = req.gen]{ return gen_.load() != g; }` 作为取消回调传给 `HttpChat::send`。curl 的**进度回调**每秒被调用数次,返回非 0 即刻中止传输(`CURLE_ABORTED_BY_CALLBACK`);写回调也会在检测到取消时返回短计数以中止。
- 每个 `AppEvent` 都携带 `gen`。UI 线程 `drainEvents()` 丢弃 `gen != ai_.generation()` 的事件。
- 两层过滤互相独立:**在飞的回复既到不了网络层之外,即使到了也画不出来**。用户继续打字就等于旧请求作废。

### 5.3 触发与限流

- 自动触发条件全部满足才发:`mode == Code` && `enabled()` && `!inFlight()` && `now - last_activity >= cfg.ghost_delay_ms`(默认 500)&& `now - last_auto_request >= cfg.ghost_min_interval_ms`(默认 1200,省钱)&& 缓冲区有至少一个非空白字符。
- 手动 `Ctrl-A` 无视 `ghost_min_interval_ms`,但同样先取消在飞请求。
- 超时由 curl 保证(`total_timeout_ms` 默认 20000 + `LOW_SPEED_LIMIT` 1 字节/10 秒),所以 worker 不需要额外看门狗——`send` 一定会返回。

### 5.4 错误上报

worker 推 `EvKind::AiError` + 中文消息。`App` 同时:状态栏显示 3 秒 + 追加到 AI 面板(带时间戳),这样错误不会一闪而过丢失。错误文案映射见 §5.6。

### 5.5 流式与背压

- `AiDelta` 事件追加到 `ghost_.text`,ghost 逐字渐显(手感好)。
- ghost **只在 `AiDone` 后才可接受**(`ghost_.complete`):接受半截生成的代码等于插入语法不完整的片段。生成中按 Tab → 状态栏提示"AI 正在生成…",不插入。
- 背压:worker 把 delta 攒起来,**最多每 40ms 推一个事件**,避免 UI 线程被几千个微事件淹没。`drainEvents()` 每 tick 最多处理 200 个事件,保证键盘输入不被饿死。

### 5.6 HTTP 层

```cpp
struct ChatMessage { std::string role, content; };

struct ChatRequest {
  std::string url, api_key, model;
  std::vector<ChatMessage> messages;
  double temperature = 0.2;
  int    max_tokens  = 256;
  bool   stream      = true;
  int    connect_timeout_ms = 5000;
  int    total_timeout_ms   = 20000;
  std::vector<std::string> stop;
  std::string toJson() const;                  // 用 mj::Value 生成,自动转义
};

using ChatDeltaFn  = std::function<bool(const std::string& delta)>;  // 返回 false 中止
using ChatCancelFn = std::function<bool()>;                          // 由 curl 进度回调轮询

struct ChatResponse {
  bool ok = false;
  std::string text;              // 拼接后的完整内容
  std::string error;             // 中文,面向用户
  long http_status = 0;
  bool canceled = false, timed_out = false;
  int64_t elapsed_ms = 0;
  int prompt_tokens = 0, completion_tokens = 0;
};

class HttpChat {                 // 每线程一个实例,复用同一个 CURL* 句柄
public:
  HttpChat(); ~HttpChat();
  static void globalInit();      // curl_global_init,必须在任何线程启动前于 main 调用
  static void globalCleanup();
  ChatResponse send(const ChatRequest& req,
                    const ChatDeltaFn& on_delta,
                    const ChatCancelFn& should_cancel);
private:
  void* curl_ = nullptr;         // CURL*,保持 void* 以免头文件引 curl.h
  std::string sse_carry_;
};

class SseParser {                // 纯函数式,可单测
public:
  void feed(const char* p, size_t n,
            const std::function<void(const std::string& payload)>& cb);
  void reset();
};
```

必须设置的 curl 选项(逐条都有理由):
- `CURLOPT_NOSIGNAL = 1L` —— **多线程程序里是强制的**。否则 curl 的 DNS 超时走 `SIGALRM` + `siglongjmp`,在非主线程上会破坏状态甚至崩溃。
- `CURLOPT_TIMEOUT_MS`、`CURLOPT_CONNECTTIMEOUT_MS`、`CURLOPT_LOW_SPEED_LIMIT=1` / `CURLOPT_LOW_SPEED_TIME=10`(卡死的流会自己死掉)。
- `CURLOPT_NOPROGRESS = 0` + `CURLOPT_XFERINFOFUNCTION` → 回调里查 `should_cancel()`,非 0 返回即刻中止。这是"用户继续打字 → 请求立即作废"的实现点。
- `CURLOPT_HTTPHEADER`:`Authorization: Bearer <key>`、`Content-Type: application/json`、流式时 `Accept: text/event-stream`。
- `CURLOPT_VERBOSE` 恒为 0。**API key 绝不出现在面板、日志、错误消息里**。
- SSE 解析:帧为 `data: {json}\n\n`,结束为 `data: [DONE]`;取 `choices[0].delta.content`。DeepSeek 若返回 `reasoning_content`,**只在练习模式追加**,写代码模式丢弃(思维链不该被当成代码插入)。
- 非流式回退(`cfg.stream == false`):取 `choices[0].message.content`。
- 兼容性兜底:HTTP 4xx 时 body 往往是普通 JSON 错误对象而非 SSE;按首字节是 `{` 或 content-type 判定,走错误路径解析 `error.message`。
- 解析不出来时,把 body 前 200 字节(**去掉 key**)追加到 AI 面板,便于在没有调试器的情况下诊断协议不匹配。
- 错误文案:401 → "API key 无效或已过期";402/429 → "余额不足或请求过于频繁";5xx → "服务端错误(HTTP xxx)";`OPERATION_TIMEDOUT` → "请求超时";`COULDNT_RESOLVE_HOST` → "无法解析域名,检查网络或 base_url";`SSL_CONNECT_ERROR` → "TLS 连接失败"。

### 5.7 Prompt 设计

- **写代码模式**(可被配置覆盖):system = "你是 C/C++ 竞赛代码补全引擎。只输出应插入在 `<CURSOR>` 处的代码;不要重复已有代码,不要解释,不要 markdown 代码围栏。" user = 光标前 N 行 + `<CURSOR>` + 光标后 N 行。
  后处理(在 `ai.cpp`):剥掉 ``` 围栏;若模型重复了当前行已有文本则去掉该前缀;截断到 `cfg.ghost_max_lines`(默认 8);去掉尾部空行。
- **练习模式**:system = "你是 ICPC 教练。**绝对不要输出可直接编译运行的完整代码或代码块**。只用中文给出:1) 可能的算法方向 2) 时间/空间复杂度估计 3) 当前代码的卡点或潜在 bug 方向 4) 下一步该想什么。不超过 300 字。" 并由代码**无条件追加**一句不可被配置覆盖的后缀:"再次强调:不要输出任何可直接编译运行的代码。"
  这只是 prompt 层的礼貌;真正的保证是 §6 的结构性保证。
- 上下文截断:`lineCount > cfg.ai_max_context_lines` 时,取光标前后各一半行,中间插 `/* ...省略... */` 标记。

---

## 6. 两种模式:练习模式绝不插入缓冲区的硬性保证

不靠散落各处的 `if (mode == ...)`。保证由**三条结构性事实**构成:

**(1) 缓冲区只有一个 AI 入口。**
`TextBuffer` 的全部改动只经过 `insert`/`erase` 两个原语;而 AI 产生的文本能到达它们的路径**只有一条**:`Editor::acceptGhost()`。`ai.cpp`、`aihttp.cpp`、`ui.cpp` 都不持有可写的 `TextBuffer&`(`ui.cpp` 只见 `const TextBuffer&`)。该唯一咽喉点开头即拦截:

```cpp
bool Editor::acceptGhost() {
  if (!ghost_.complete)                    return false;   // 生成中不接受
  if (ghost_.text.empty())                 return false;
  if (ghost_.sink != AiSink::GhostText)    return false;   // ★ 结构性拦截
  if (ghost_.gen != expected_gen_)         return false;   // 过期建议不接受
  TextBuffer::Edit guard(buf_, "accept-ai");
  cursor_ = buf_.insert(ghost_.anchor, ghost_.text);
  ghost_.clear();
  return true;
}
```

**(2) 练习模式的回复不可能变成 Ghost。**
`AiSink` 在**请求创建时**就由唯一函数 `AiService::sinkFor(mode)` 定死并写进 `AiRequest`,随事件一路带回。`Editor::setGhost()` 只在 `App::onAiDone` 里那**一个** `switch (ev.sink)` 的 `GhostText` 分支被调用;`PanelOnly` 分支只会 `panels_[Ai].appendStreaming(...)`。`setGhost` 内部再加 `assert(g.sink == AiSink::GhostText)` + 运行期 early-return,所以即使将来有人写错逻辑,退化结果是"没有 ghost",**永远不是"代码被插入"**。

**(3) 切模式即销毁在飞工作。**
`setMode()` 先 `gen_.fetch_add(1)`,于是所有已在网络上的回复在被"检视"之前就已被 generation 检查丢弃;Code→Practice 还会调 `Editor::clearGhost()`。因此**不存在**"切到练习模式之后,写代码模式的回复才落地"的时间窗。

**附带效果:Tab 路径根本不需要判模式。** keymap 把 Tab 映射到 `Action::Tab`,处理函数是 `if (ed_.ghost().complete) ed_.acceptGhost(); else ed_.indent();`。练习模式永远不会有 ghost,所以 Tab 自动就只是缩进——满足"此模式下 Tab 不接受任何 AI 内容",且这条路径上一行模式判断都没有。模式问题在请求创建时被回答一次,编码进数据(`AiSink`),下游没有任何可以搞错的余地。

---

## 7. 配置

**路径解析优先级**:
1. 命令行 `--config <PATH>`
2. 环境变量 `CPPIDE_CONFIG`
3. `$XDG_CONFIG_HOME/cppide/config.json`
4. `~/.config/cppide/config.json`(推荐、写进 README)

**取值优先级**:内置默认 < 配置文件 < 环境变量(`CPPIDE_API_KEY` 或 `DEEPSEEK_API_KEY`、`CPPIDE_MODEL`、`CPPIDE_BASE_URL`)。环境变量最高,便于临时切换而不改文件。

```cpp
struct Config {
  // ---- AI ----
  std::string api_key;                       // 空 => AI 关闭
  std::string base_url  = "https://api.deepseek.com";
  std::string chat_path = "/v1/chat/completions";
  std::string model     = "deepseek-chat";   // 默认值仅作占位,README 要求用户按官方文档填
  bool   stream                 = true;
  int    ghost_delay_ms         = 500;
  int    ghost_min_interval_ms  = 1200;
  int    ghost_max_lines        = 8;
  int    ai_connect_timeout_ms  = 5000;
  int    ai_timeout_ms          = 20000;
  int    ai_max_context_lines   = 400;
  int    max_tokens_code        = 256;
  int    max_tokens_practice    = 512;
  double temperature_code       = 0.2;
  double temperature_practice   = 0.7;
  std::string prompt_code;                   // 空 => 用内置默认
  std::string prompt_practice;               // 空 => 用内置默认
  // ---- 编译 ----
  std::string cc  = "cc";
  std::string cxx = "c++";
  std::vector<std::string> cflags   = {"-O2","-std=c11","-Wall"};
  std::vector<std::string> cxxflags = {"-O2","-std=c++17","-Wall"};
  int compile_timeout_ms = 30000;
  // ---- 运行 ----
  int    run_timeout_ms   = 5000;
  size_t run_output_limit = 1u << 20;
  std::string stdin_file;                    // 空 => 用【输入】面板内容
  // ---- 编辑器 ----
  int  tab_width = 4;
  bool expand_tab = true;
  bool auto_indent = true;
  bool show_line_numbers = true;
  int  panel_height = 10;
  int  tick_ms = 60;
  // ---- 元信息 ----
  std::string source_path;                   // 实际加载自哪(""=全默认)
  std::vector<std::string> warnings;         // 非致命解析告警,启动时进状态栏与 AI 面板
  bool aiEnabled() const { return !api_key.empty(); }
  std::string chatUrl() const;               // base_url + chat_path,处理重复斜杠
};

class ConfigLoader {
public:
  static std::string defaultPath();
  static Config load(const std::string& path, std::string& err);  // 永不硬失败
  static std::string sampleJson();           // --print-config 与 README 共用
};
```

**降级行为(需求明确要求不崩)**:
- 配置文件不存在:用全默认,`warnings` 加一条"未找到配置文件 <path>,AI 功能已关闭",编辑/编译/运行完全可用。
- JSON 语法错误:用全默认 + `warnings` 记录行号,**不中断启动**。
- `api_key` 为空:`aiEnabled() == false` → **worker 线程根本不创建**;`Ctrl-A` / 停顿触发都立即变成一条面板消息"AI 未配置:请在 <path> 填写 api_key",状态栏 AI 状态显示"未配置"。
- 单个字段类型不对(例如 `tab_width: "four"`):该字段用默认值 + 一条 warning,其余字段照常生效。
- 代码中**不出现任何硬编码 key**;`model` 的默认值只是占位,README 明确要求用户填官方文档上的模型名。

---

## 8. UI 布局与快捷键

### 8.1 布局

```
┌──────────────────────────────────────────────────────────────┐
│  1 #include <bits/stdc++.h>                                  │  编辑区
│  2 using namespace std;                                      │  行号槽在练习模式
│  3 int main(){                                               │  会被染成同色强调
│  4     int n; cin >> n;▏for(int i=0;i<n;i++) cin>>a[i];       │  ← ghost(暗色)
│ ~5                                                           │  ← ghost 续行,槽内 ~
├─[编译 2]─[运行*]─[AI]─[输入]─────────────────────────────────┤  面板标签栏
│ main.cpp:7:15: error: 'x' was not declared in this scope     │  仅画聚焦标签的内容
│ main.cpp:9:3: warning: unused variable 'y'                   │  中文自动折行
├──────────────────────────────────────────────────────────────┤
│ 练习模式 │ main.cpp* │ 12:5 │ C++ │ 2错误1警告 │ AI思考中… F1帮助│  状态栏
└──────────────────────────────────────────────────────────────┘
```

- **单一底部面板 + 标签栏**,不是四个堆叠窗格:24 行的终端养不起四个窗格。高度 `cfg.panel_height`(默认 10),`Alt-0` 折叠/展开,`Alt-=`/`Alt--` 调高度。
- 标签上的 `*` 表示有未读输出,`[编译 2]` 里的数字是错误数。这样后台编译完成可见,但**不抢焦点**。
- **自动聚焦规则**:只有"编译失败"会自动聚焦编译面板(错误你总是想看);其它一切只置未读标记。**永不在用户打字时抢走焦点。**
- **模式指示器**:状态栏最左格,反色 + 配色 —— 练习模式用黄/品红底,写代码模式用绿/青底。此外**练习模式下行号槽整体染同一强调色**,眼睛盯着代码时也能看见模式。文字 + 颜色 + 固定位置三重冗余,满足"一眼可见"。
- 状态栏字段(左→右):模式徽章 │ 文件名 + 脏标记 `*` │ `行:列` │ 语言 │ 上次构建结果 │ AI 状态(就绪/思考中…/未配置/错误)│ `F1帮助`。
- **Ghost 渲染**:光标处用 `A_DIM`(256 色时用灰色 pair)。多行时首行在光标后内联,其余行覆盖绘制在下方屏幕行上、**不推挤任何真实内容**,并在行号槽画 `~` 表示"这是预览"。绘制顺序保证真实光标最后定位,终端插入符落在真实位置(`curs_set(1)`)。
- **宽字符**:`setlocale(LC_ALL, "")` 必须在 `initscr()` 之前;输入用 `wget_wch`,输出用 `waddnwstr`;链接 `ncursesw`(Linux)/ `ncurses`(macOS 自带即宽字符版)。子进程另给 `LC_ALL=C`,两者互不干扰。
- **重绘策略**:仅在 `dirty_` 为真或本 tick 有事件时重绘,60ms 唤醒的空闲 CPU 接近 0。
- **窗口缩放**:靠 ncurses 的 `KEY_RESIZE`(SIGWINCH 后由 getch 返回),不自装 SIGWINCH handler。收到后重算布局 + `ensureCursorVisible()` + 全量重绘。最小可用尺寸 60x16,更小则只画一行提示。
- **崩溃安全网**:`atexit(endwin_once)` + `std::set_terminate` + `SIGSEGV/SIGBUS/SIGABRT` handler 内 `endwin()` 后重新 raise。绝不把用户终端留在 raw + noecho 状态。

### 8.2 快捷键(macOS 终端安全性是首要约束)

**被刻意避开的键**,以及原因:`Ctrl-C`(SIGINT)、`Ctrl-Z`(SIGTSTP,挂起后回来终端状态难恢复)、`Ctrl-S`/`Ctrl-Q`(XON/XOFF 流控,ssh 下的经典冻屏陷阱)、`Ctrl-\`(SIGQUIT + core dump)、`Ctrl-Y`(BSD/macOS 的 VDSUSP)。虽然 ncurses 的 `raw()` 会清掉 `ISIG`/`IXON` 让它们变成普通字节,但我们仍然一个都不绑——终端仿真器和 tmux 可能有自己的映射,而且用户的肌肉记忆会带来意外。同理不绑 `Ctrl-M`/`Ctrl-J`/`Ctrl-I`/`Ctrl-H`(它们就是 Enter/Tab/Backspace 的别名)。

初始化用 `raw()`(不是 `cbreak()`)+ `noecho()` + `keypad(stdscr, TRUE)` + `set_escdelay(25)`。

| 键 | 动作 | 备注 |
|---|---|---|
| 方向键 / PgUp / PgDn / Home / End | 移动 | |
| `Alt-←` / `Alt-→` | 按词移动 | |
| Enter / Backspace / Delete / 可打印字符 | 编辑 | Enter 带自动缩进 |
| `Tab` | 有 ghost 则接受,否则缩进 | 练习模式下永远只是缩进(见 §6) |
| `Shift-Tab` | 反缩进 | |
| `Esc` | 依次:丢弃 ghost → 取消提示行 → 面板取消焦点 | 运行面板中 = **终止运行中的程序** |
| `Ctrl-O` | 保存(nano 风格 write out) | |
| `Ctrl-X` | 退出(脏则确认) | |
| `Ctrl-B` | 编译 | |
| `Ctrl-R` | 运行(必要时先自动编译) | |
| `Ctrl-E` | 编译并运行 | |
| `Ctrl-T` | 切换 AI 模式(练习 ⇄ 写代码) | T = Toggle |
| `Ctrl-A` | 立即请求 AI(练习模式下先在 AI 面板输入问题) | tmux 用户的 prefix 是 `Ctrl-B`/`Ctrl-A`,README 提示可用 `F4` 别名 |
| `Ctrl-U` / `Ctrl-W` | 撤销 / 重做 | |
| `Ctrl-K` / `Ctrl-V` / `Ctrl-D` | 剪切行 / 粘贴行 / 复制行 | macOS 下粘贴用 Cmd-V(终端自己处理),`Ctrl-V` 不冲突 |
| `Ctrl-G` | 跳到行号 | |
| `Ctrl-F` | 查找;`Ctrl-N` 下一处(无诊断时) | |
| `Ctrl-N` / `Ctrl-P` | 下一个 / 上一个编译诊断 | n/p 助记 |
| `Ctrl-L` | 强制重绘 | |
| `Alt-1..4` | 聚焦 编译 / 运行 / AI / 输入 面板 | 由 Esc 前缀合成,各终端通用 |
| `Alt-0` | 折叠/展开面板区 | |
| `Alt-=` / `Alt--` | 面板加高 / 减高 | |
| `F1` | 帮助浮层(全键表) | |
| `F2` `F5` `F6` `F8` `F10` | 保存 / 编译 / 运行 / 切模式 / 退出(别名) | |

**F 键为何只作别名**:macOS 上 F1–F12 默认被系统媒体键占用(除非勾选"将 F1、F2 等键用作标准功能键")。所以每个动作都同时有 Ctrl 绑定和 F 键绑定,任一路可用。**Alt 为何用 Esc 前缀实现**:Terminal.app 默认不发 Meta 位,把 Option 当 Esc 前缀发送;iTerm2 可配但默认亦然。收到 `27` 时做一次非阻塞 `getch()`:有字符 → 合成 `Alt(c)`;`ERR` → 是裸 Esc。

`--doctor` 打印 `TERM`、`COLORS`、`COLOR_PAIRS`、ncurses 宽字符能力、以及按下的键的实际键码,便于用户自查键位问题。

### 8.3 面板聚焦时的键

聚焦面板后:方向键/PgUp/PgDn 滚动该面板;`Tab` 切下一个标签;`Enter` 在编译面板 = 跳到该诊断;在【输入】面板 = 正常编辑(它内部就是一个 `TextBuffer`,支持撤销);在【AI】面板 = 打开提问输入行(练习模式的问答入口)。`Esc` 交回编辑区。

---

## 9. 关键类型:事件与队列

```cpp
template <class T>
class Mailbox {
public:
  void   push(T v);                          // 加锁 push + notify
  bool   tryPop(T& out);                     // 非阻塞;UI 线程只用这个
  bool   waitPop(T& out, int timeout_ms);    // worker 线程用
  size_t size() const;
  void   close();  bool closed() const;      // 唤醒所有等待者;tryPop 仍可排空残余
private:
  mutable std::mutex m_; std::condition_variable cv_;
  std::deque<T> q_; bool closed_ = false;
};

enum class EvKind {
  AiStarted, AiDelta, AiDone, AiError,
  CompileStarted, CompileDone, RunStarted, RunDone,
  Status,
};

struct AppEvent {
  EvKind   kind;
  uint64_t gen = 0;                          // AI generation 或 Runner::JobId
  AiSink   sink = AiSink::PanelOnly;         // 随请求一路带回,§6 的关键
  std::string text;
  std::shared_ptr<CompileOutcome> compile;   // 重载荷用 shared_ptr,事件保持可廉价 move
  std::shared_ptr<ProcResult>     run;
};
```

临界区都极短(只 move `std::string` / `shared_ptr`),UI 线程不会在锁上被饿死。

## 10. 主循环

```cpp
while (!quit_) {
  if (dirty_) { ui_.draw(model()); dirty_ = false; }
  int k = ui_.getKeyBlockingFor(cfg_.tick_ms);        // 60ms
  if (k == ERR)        { tick(); continue; }
  if (k == KEY_RESIZE) { ui_.handleResize(); relayout(); dirty_ = true; continue; }
  onKey(k);
  tick();
}
// tick(): drainEvents(最多 200 个) -> ai_.maybeAutoTrigger(...) -> 状态栏消息过期
```

启动顺序(`main.cpp`):`setlocale` → `signal(SIGPIPE, SIG_IGN)` → 装 endwin 安全网 → 解析 argv → `ConfigLoader::load` → `HttpChat::globalInit()` → 构造 `App`(内部 `Ui::init`,再按需启动 `AiService` 线程)→ `run()` → `Runner`/`AiService` 析构(close + join)→ `Ui::shutdown` → `HttpChat::globalCleanup`。**线程一律 join,绝不 detach**:detach 的线程去碰已析构的 `Mailbox` 是退出期崩溃的经典来源。

---

## 11. Makefile

```make
BIN      ?= cppide
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -MMD -MP
UNAME_S  := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  NCURSES_PREFIX := $(shell brew --prefix ncurses 2>/dev/null)
  ifneq ($(NCURSES_PREFIX),)
    CPPFLAGS += -I$(NCURSES_PREFIX)/include
    LDFLAGS  += -L$(NCURSES_PREFIX)/lib
  endif
  LDLIBS += -lncurses -lcurl -lpthread
else
  LDLIBS += -lncursesw -lcurl -lpthread
endif

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)
DEP := $(OBJ:.o=.d)
HDR := $(wildcard src/*.h)

all: $(BIN)
$(BIN): $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)
%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

# Wave 1 的验收门:每个头文件必须自给自足
check-headers:
	@for h in $(HDR); do echo "  $$h"; \
	  $(CXX) $(CPPFLAGS) $(CXXFLAGS) -fsyntax-only -x c++ $$h || exit 1; done

debug: CXXFLAGS += -O0 -g -fsanitize=address,undefined
debug: LDFLAGS  += -fsanitize=address,undefined
debug: clean all

tests: $(filter-out src/main.o,$(OBJ))
	@for t in tests/test_*.cpp; do \
	  $(CXX) $(CPPFLAGS) $(CXXFLAGS) -o /tmp/$$(basename $$t .cpp) $$t \
	    $(filter-out src/main.o,$(OBJ)) $(LDLIBS) && /tmp/$$(basename $$t .cpp) || exit 1; done

install: $(BIN)
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
clean:
	rm -f $(OBJ) $(DEP) $(BIN)
-include $(DEP)
.PHONY: all clean debug tests install check-headers
```

`PREFIX ?= /usr/local`。Linux 开发容器前置:`apt-get install build-essential libncursesw5-dev libcurl4-openssl-dev`。

---

## 12. 实现顺序与并行切分

**并行不变式**:每个 wave 内的文件集两两不相交;**只有 Wave 1 碰头文件**。Wave 2 之后若发现头需要改,必须提交给协调者统一修改,不允许 agent 自行改头——否则并行的 agent 之间会产生看不见的耦合。

### Wave 1 — 1 个 agent(串行,是所有并行的前提)
`Makefile` + 全部 14 个头文件 + `src/mailbox.h`(header-only,本 wave 即完成实现)。
门:`make check-headers` 通过(证明每个头自给自足、包含齐全)。

### Wave 2 — 6 路并行(叶子模块,互不依赖)
| Agent | 文件 | 单测 |
|---|---|---|
| A | `src/json.cpp` | `tests/test_json.cpp` |
| B | `src/util.cpp` | — |
| C | `src/highlight.cpp` | `tests/test_highlight.cpp` |
| D | `src/textbuf.cpp` | — |
| E | `src/proc.cpp` | `tests/test_proc.cpp`(死循环 / 巨量输出 / 提前关 stdin / 无视 SIGTERM) |
| F | `src/config.cpp` | — |
门:各 `.o` 编译通过 + 单测通过。

### Wave 3 — 4 路并行
| Agent | 文件 |
|---|---|
| G | `src/editor.cpp` |
| H | `src/build.cpp` + `src/keys.cpp` + `tests/test_diag.cpp` |
| I | `src/aihttp.cpp` + `tests/test_sse.cpp` |
| J | `src/panel.cpp` |

### Wave 4 — 3 路并行
| Agent | 文件 |
|---|---|
| K | `src/ui.cpp` |
| L | `src/ai.cpp` |
| M | `src/app.cpp` + `src/main.cpp` |
三者只依赖 Wave 1 的头文件,所以可以真正并行。门:完整链接 + 手动 smoke(打开文件、编辑、保存、编译报错跳行、运行喂 stdin、切模式)。

### Wave 5 — 2 路并行
| Agent | 内容 |
|---|---|
| N | `README.md` + 示例 config.json |
| O | 打磨:查找、帮助浮层、80x24 布局、`make debug`(ASan/UBSan)跑一轮、错误路径实测(无网络 / 错 key / 死循环程序 / 100MB 输出 / 窗口猛拉缩放) |

---

## 13. 已知限制(写进 README)

1. 原始字符串字面量 `R"delim(...)"` 跨行时高亮为近似(见 §3)。
2. 只编译当前打开的单个文件(需求边界)。
3. 无选区;块操作用行级快捷键;复制到系统剪贴板请用终端自身的选择 + Cmd-C。
4. DeepSeek 的流式格式假定与 OpenAI 兼容;若不兼容,只需改 `src/aihttp.cpp`。
5. 不做会话持久化,退出即清空(需求假设)。

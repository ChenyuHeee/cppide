#!/bin/sh
# desc: Wave 5 最后一轮 5 项小修的专项验证(帮助列宽 / 扩展名 / 死代码 / F1 翻页 / 禁 core)
#
# 用法:  .flower/scripts/wave5-finalfix-verify.sh [子集...]
#   不给参数 = 全跑。子集名:build cols ext dead page core
# ★ 每条外部命令都带 timeout;不联网、不使用任何 API key。
set -u

ROOT=${ROOT:-/work}
BIN=${BIN:-$ROOT/cppide}
S=$ROOT/.flower/scripts
W=${W:-/tmp/wave5ff}
PTY=$W/pty
FAIL=0
mkdir -p "$W"

say()  { printf '\n==================== %s ====================\n' "$*"; }
ok()   { printf '  PASS  %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*"; FAIL=$((FAIL + 1)); }
want() { if [ "$1" = 0 ]; then ok "$2"; else bad "$2 (rc=$1)"; fi; }
wants() { [ $# -eq 0 ] && return 0; for a in $ARGS; do [ "$a" = "$1" ] && return 0; done; return 1; }
ARGS=${*:-}
[ -z "$ARGS" ] && ARGS="build cols ext dead page core"

# ------------------------------------------------------------------ build
if wants build; then
  say "构建 + 零 warning + check-headers"
  timeout 900 make -C "$ROOT" > "$W/build.log" 2>&1
  want $? "make"
  if grep -qi "warning" "$W/build.log"; then bad "make 有 warning"; else ok "make 零 warning"; fi
  timeout 900 make -C "$ROOT" check-headers > "$W/hdr.log" 2>&1
  want $? "make check-headers"
fi
[ -x "$PTY" ] || timeout 300 g++ -std=c++17 -O2 -Wall -Wextra -o "$PTY" "$S/wave5-pty-drive.cpp" -lutil

# ------------------------------------------------------------------ 1) 帮助列宽
# keys.cpp::helpRow 原来写死 padRight(...,22),而 BufferStart 的 label
# "Alt-< / Alt-Home / Shift-Home" 显示宽 28 -> 补齐失效 -> 键名和说明粘成
# "Shift-Home跳到文件开头"。现在按表里最长 label 算宽度 + 2 列间隔。
if wants cols; then
  say "① --help / F1 的键名列宽(不许与说明粘连)"
  timeout 30 "$BIN" --help > "$W/help.txt" 2>&1
  want $? "cppide --help"
  timeout 60 python3 - "$W/help.txt" <<'PYEOF'
import re, sys, unicodedata
def dw(s):
    return sum(2 if unicodedata.east_asian_width(c) in ('W', 'F') else 1 for c in s)
lines = open(sys.argv[1], encoding='utf-8').read().split('\n')
# helpRow 出来的行:helpLines 里以 2 空格缩进,--help 再加 2 -> 恰好 4 个空格
rows = [l for l in lines if l.startswith('    ') and not l.startswith('     ')]
assert len(rows) >= 40, 'helpRow 行数太少:%d' % len(rows)
bad = [l for l in rows if re.search(r'\S\s\s+\S', l[4:]) is None]
if bad:
    print('  FAIL 这些行的键名与说明之间不足 2 个空格:')
    for b in bad[:8]:
        print('       ' + repr(b))
    sys.exit(1)
# 最长键名那两行必须在(它们就是原来粘住的那两行)
joined = '\n'.join(rows)
for probe in ('Alt-< / Alt-Home / Shift-Home', 'Alt-> / Alt-End / Shift-End'):
    if probe not in joined:
        print('  FAIL 找不到最长键名行:' + probe); sys.exit(1)
    hit = [l for l in rows if probe in l][0]
    tail = hit[4:].split(probe, 1)[1]
    if not tail.startswith('  '):
        print('  FAIL 最长键名行仍然粘连:' + repr(hit)); sys.exit(1)
print('  %d 行 helpRow,全部有 >=2 空格间隔;最长键名行宽 %d' %
      (len(rows), max(dw(l) for l in rows)))
PYEOF
  want $? "45 行键名列全部与说明隔开 >=2 空格(含 28 宽的 Shift-Home 行)"
fi

# ------------------------------------------------------------------ 2) 扩展名
if wants ext; then
  say "② 扩展名大小写(.c 与 .C 仍然区分,其余不敏感)"
  D=$W/ext; mkdir -p "$D"
  # 门槛只看扩展名,文件存不存在都一样(不存在 = 当新文件),所以不用真造文件。
  # 非交互环境下 cppide 会以 rc=3 + "不是交互式终端" 退出 —— 那正好说明
  # **扩展名门槛已经放行**;被拒绝时打印的是"不支持的文件类型"。
  pass_ext() {
    out=$(timeout 15 "$BIN" "$D/probe$1" < /dev/null 2>&1)
    case "$out" in
      *"不支持的文件类型"*) bad "扩展名 $1 应当被接受,却被拒绝" ;;
      *"不是交互式终端"*)   ok  "扩展名 $1 被接受" ;;
      *)                    bad "扩展名 $1:看不懂的输出 [$out]" ;;
    esac
  }
  rej_ext() {
    out=$(timeout 15 "$BIN" "$D/probe$1" < /dev/null 2>&1)
    case "$out" in
      *"不支持的文件类型"*) ok  "扩展名 $1 被拒绝" ;;
      *)                    bad "扩展名 $1 应当被拒绝,却放行了 [$out]" ;;
    esac
  }
  for e in .c .C .cpp .CPP .Cpp .cc .CC .cxx .CXX .c++ .C++ \
           .h .H .hpp .Hpp .HPP .hh .HH .hxx .HXX .h++ .hp .HP; do
    pass_ext "$e"
  done
  for e in .inc .INC .txt .py .cs .cp .hs .o; do rej_ext "$e"; done
fi

# ------------------------------------------------------------------ 3) 死代码
if wants dead; then
  say "③ syntaxOnly 与 langFromPath 用同一份清单(没有死分支)"
  # 单元测试里已经有断言(test_diag.cpp / test_textbuf.cpp),这里做源码层面的看门:
  # build.cpp 里不许再出现第二份扩展名清单。
  if grep -q '"\.inc"' "$ROOT/src/build.cpp" "$ROOT/src/textbuf.cpp"; then
    bad "还有 .inc 的残留"
  else ok "两个文件里都没有 .inc 了"; fi
  n=$(grep -c 'ext == "\.h' "$ROOT/src/build.cpp" || true)
  if [ "$n" = 0 ]; then ok "build.cpp 里没有第二份头扩展名清单"
  else bad "build.cpp 里还有 $n 处硬编码头扩展名"; fi
  if grep -q 'langFromPath' "$ROOT/src/build.cpp"; then
    ok "syntaxOnly 走 TextBuffer::langFromPath(唯一清单)"
  else bad "syntaxOnly 没有复用 langFromPath"; fi
fi

# ------------------------------------------------------------------ 4) F1 翻页
if wants page; then
  say "④ F1 帮助浮层 PgDn/PgUp/↓/↑ 翻页(Ui::scrollHelp)+ 退化尺寸"
  printf '// PAGEPROBE_MARKER\nint main(){ return 0; }\n' > "$W/pageprobe.cpp"
  CPPIDE_API_KEY= DEEPSEEK_API_KEY= \
    timeout 300 "$PTY" 24 80 "$S/wave5-finalfix-help-page.txt" "$W/page.out" \
      -- "$BIN" "$W/pageprobe.cpp"
  want $? "F1 翻页 pty 脚本(详见 $W/page.out)"
fi

# ------------------------------------------------------------------ 5) 禁 core
if wants core; then
  say "⑤ 子进程段错误不留 core(proc.cpp::childMain 的 setrlimit)"
  pat=$(cat /proc/sys/kernel/core_pattern 2>/dev/null || echo '?')
  printf '  core_pattern = %s  /  ulimit -c = %s\n' "$pat" "$(ulimit -c)"
  case "$pat" in
    '|'*|/*) printf '  跳过:core_pattern 不是"落在 cwd 的普通文件名",本机无法构成证据\n' ;;
    *)
      # 反证:不经过 proc.cpp 的话,同样的段错误**会**在 cwd 里留下 core
      B=$(mktemp -d "$W/base.XXXXXX")
      ( cd "$B" && timeout 15 sh -c 'kill -SEGV $$' ) 2>/dev/null
      sleep 1
      if ls "$B" 2>/dev/null | grep -q '^core'; then
        ok "反证成立:不禁 core 时确实会留下 core 文件"
      else
        printf '  跳过:本机连反证都留不下 core,断言不构成证据\n'
      fi
      # 真身:走 runProcess 的那条路(由 test_proc.cpp::t_no_core_dump 断言)
      # 注意 `grep -v '\.dbg\.o$'`:`make debug` 会在 src/ 下留一堆 `*.dbg.o`(ASan/UBSan 版),
      # `ls src/*.o` 会把它们一起收进来 -> 一片 "multiple definition" +
      # "undefined reference to __ubsan_handle_*",看起来像单测挂了,其实是选错了 .o。
      timeout 600 sh -c "cd '$ROOT' && g++ -Isrc -std=c++17 -O2 -Wall -Wextra \
        -Wno-unused-parameter -o $W/test_proc tests/test_proc.cpp \
        \$(ls src/*.o | grep -v main.o | grep -v '\.dbg\.o\$') -lncursesw -lcurl -lpthread" > "$W/tp.log" 2>&1
      want $? "编译 test_proc"
      timeout 300 "$W/test_proc" > "$W/tp.run.log" 2>&1
      want $? "test_proc 全过(含 t_no_core_dump / t_signaled)"
      if grep -q "段错误不留 core 文件" "$W/tp.run.log"; then
        ok "t_no_core_dump 真的跑到了"
      else bad "t_no_core_dump 没跑"; fi
      if grep -q "被信号 SIGSEGV 终止\|被信号杀死" "$W/tp.run.log"; then
        ok "禁 core 之后 SIGSEGV 仍然被正确汇报"
      else bad "SIGSEGV 汇报用例没跑到"; fi
      ;;
  esac
  # 仓库根目录不许因为跑测试而多出 core
  if [ -e "$ROOT/core" ]; then bad "$ROOT/core 又出现了"; else ok "$ROOT 下没有 core 文件"; fi
fi

say "wave5-finalfix 汇总:失败 $FAIL 条"
[ "$FAIL" = 0 ] || exit 1

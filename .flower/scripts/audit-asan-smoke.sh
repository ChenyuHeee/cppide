#!/usr/bin/env bash
# desc: 验收 12 —— ASan/UBSan/LSan 下的 pty 冒烟:编辑保存、编译跳诊断、喂 stdin 运行、AI ghost 接受/丢弃、练习模式、面板与缩放各跑一轮
#
# 前置: 先 `make debug`。新的 Makefile 把 sanitizer 产物放在 ./cppide-debug
#       (与 release 的 ./cppide 并存,互不覆盖),本脚本默认就用它;
#       也可用环境变量 CPPIDE_BIN 指定别的二进制。
# 用法: bash audit-asan-smoke.sh [端口]

set -u
PORT="${1:-18966}"
WORK=/work
TMP=/tmp/audit-asan
DRIVE=/tmp/ptydrive
mkdir -p "$TMP"; cd "$TMP" || exit 2
rm -f "$TMP"/san.*

BIN="${CPPIDE_BIN:-$WORK/cppide-debug}"
[ -x "$BIN" ] || BIN="$WORK/cppide"      # 兼容老流程(debug 覆盖式重建)
if ! grep -q 'libasan\|__asan' <(nm -D "$BIN" 2>/dev/null; ldd "$BIN" 2>/dev/null); then
  echo "  跳过:$BIN 不是 sanitizer 版本。先跑 make debug。"
  exit 2
fi
echo "  使用二进制: $BIN"

export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:log_path=$TMP/san:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="print_stacktrace=1:log_path=$TMP/san"
export LSAN_OPTIONS="log_path=$TMP/san"

pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/req.jsonl" \
  > "$TMP/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1.5

cat > "$TMP/ai.json" <<EOF
{ "api_key": "FAKEKEY_ASAN", "model": "m", "base_url": "http://127.0.0.1:$PORT",
  "chat_path": "/v1/chat/completions", "ghost_min_interval_ms": 60000, "tick_ms": 40,
  "run_timeout_ms": 8000, "compile_timeout_ms": 60000 }
EOF
cat > "$TMP/pr.json" <<EOF
{ "api_key": "FAKEKEY_ASAN", "model": "m", "base_url": "http://127.0.0.1:$PORT",
  "chat_path": "/practice", "ghost_min_interval_ms": 60000, "tick_ms": 40,
  "run_timeout_ms": 8000, "compile_timeout_ms": 60000 }
EOF
cat > "$TMP/no.json" <<'EOF'
{ "api_key": "", "tick_ms": 40, "run_timeout_ms": 8000, "compile_timeout_ms": 60000 }
EOF

src() { cat > "$1" <<'EOF'
#include <cstdio>
int main() {
  long long s = 0, v;
  while (scanf("%lld", &v) == 1) s += v;
  printf("ASAN_SUM=%lld\n", s);
  return 0;
}
EOF
}

scen() {  # scen <名字> <cfg> <脚本>
  local name="$1" cfg="$2" script="$3"
  src "$TMP/$name.cpp"
  printf '%s\n' "$script" > "$TMP/$name.txt"
  timeout 300 "$DRIVE" 34 120 "$TMP/$name.txt" "$TMP/$name.out" \
    -- "$BIN" --config "$cfg" "$TMP/$name.cpp" > "$TMP/$name.log" 2>&1
  local rc=$?
  if grep -qE '被信号 (6|11)' "$TMP/$name.log"; then
    bad "[$name] 进程被 SIGABRT/SIGSEGV 杀死"
  else
    ok "[$name] 跑完未崩溃"
  fi
}

echo "== 场景 1:编辑 + 保存 + 撤销重做 =="
scen s1 "$TMP/no.json" 'expect s1.cpp
wait 400
send ZZEDIT
wait 300
send \x15
wait 300
send \x17
wait 300
send \x0f
wait 800
send \x18
wait 2500'

echo "== 场景 2:编译错误 + 诊断跳转 =="
cat > "$TMP/bad.cpp" <<'EOF'
#include <cstdio>
int a = 1;
zzz_bad_token @@@ ;
int main() { return 0; }
EOF
printf 'expect bad.cpp\nwait 400\nsend \\x02\nwait 8000\nsend \\x0e\nwait 800\nsend \\x10\nwait 800\nsend \\x18\nwait 2500\n' > "$TMP/s2.txt"
timeout 300 "$DRIVE" 34 120 "$TMP/s2.txt" "$TMP/s2.out" \
  -- "$BIN" --config "$TMP/no.json" "$TMP/bad.cpp" > "$TMP/s2.log" 2>&1
grep -qE '被信号 (6|11)' "$TMP/s2.log" && bad "[s2] 崩溃" || ok "[s2] 编译+跳诊断未崩溃"

echo "== 场景 3:喂 stdin 运行 =="
scen s3 "$TMP/no.json" 'expect s3.cpp
wait 400
send \x1b4
wait 500
send 5 6 7
wait 300
send \x1b
wait 300
send \x05
expect ASAN_SUM=18
wait 800
send \x18
wait 2500'
grep -q 'ASAN_SUM=18' "$TMP/s3.out" && ok "[s3] stdin 求和正确" || bad "[s3] stdin 结果不对"

echo "== 场景 4:AI ghost 接受(写代码模式)=="
scen s4 "$TMP/ai.json" 'expect s4.cpp
send \x14
expect 写代码模式
wait 300
send \sq
expect GHOSTMARK_Alpha
send \t
wait 700
send \x0f
wait 900
send \x18
wait 2500'
grep -q 'GHOSTMARK_Alpha' "$TMP/s4.cpp" && ok "[s4] Tab 接受落盘" || bad "[s4] Tab 接受失败"

echo "== 场景 5:AI ghost 丢弃(Esc)=="
scen s5 "$TMP/ai.json" 'expect s5.cpp
send \x14
expect 写代码模式
wait 300
send \sq
expect GHOSTMARK_Alpha
send \x1b
wait 700
send \x0f
wait 900
send \x18
wait 2500'
grep -q 'GHOSTMARK' "$TMP/s5.cpp" && bad "[s5] Esc 后仍插入" || ok "[s5] Esc 丢弃,缓冲区干净"

echo "== 场景 6:练习模式(回复含代码块)=="
scen s6 "$TMP/pr.json" 'expect s6.cpp
expect 练习模式
wait 300
send \x01
expect 问 AI
send ceshi\r
expect PRACTICE_LEAK_CODE
wait 800
send \x1b
wait 400
send \t\t
wait 500
send \x0f
wait 900
send \x18
wait 2500'
grep -q 'PRACTICE_LEAK_CODE' "$TMP/s6.cpp" && bad "[s6] 练习模式代码进了缓冲区" \
  || ok "[s6] 练习模式缓冲区无 AI 代码"

echo "== 场景 7:面板切换 + 缩放风暴 + 帮助浮层 =="
src "$TMP/s7.cpp"
cat > "$TMP/s7.txt" <<'EOF'
expect s7.cpp
wait 400
send \x1b1
wait 200
send \x1b2
wait 200
send \x1b3
wait 200
send \x1b4
wait 200
send \x1b
wait 200
send \x1b0
wait 300
send \x1b0
wait 300
send \eOP
wait 600
send \x1b
wait 300
resizestorm 60 20x5 200x60
wait 500
resize 34x120
wait 500
send \x0c
wait 500
send \x18
wait 2500
EOF
timeout 300 "$DRIVE" 34 120 "$TMP/s7.txt" "$TMP/s7.out" \
  -- "$BIN" --config "$TMP/no.json" "$TMP/s7.cpp" > "$TMP/s7.log" 2>&1
grep -qE '被信号 (6|11)' "$TMP/s7.log" && bad "[s7] 崩溃" || ok "[s7] 面板/缩放/帮助未崩溃"

echo "== 场景 8:AI 网络错误路径(500 / 断连)=="
for p in err500 drop badjson; do
  cat > "$TMP/e_$p.json" <<EOF
{ "api_key": "FAKEKEY_ASAN", "model": "m", "base_url": "http://127.0.0.1:$PORT",
  "chat_path": "/$p", "ai_connect_timeout_ms": 2000, "ai_timeout_ms": 4000,
  "ghost_min_interval_ms": 60000, "tick_ms": 40 }
EOF
  scen "e_$p" "$TMP/e_$p.json" 'expect e_'"$p"'.cpp
send \x14
expect 写代码模式
wait 300
send \x01
wait 6000
send \x1b3
wait 800
send \x18
wait 2500'
done

echo
echo "== sanitizer 报告 =="
shopt -s nullglob
reports=("$TMP"/san.*)
if [ ${#reports[@]} -eq 0 ]; then
  ok "ASan/UBSan/LSan 全程零报告"
else
  bad "有 ${#reports[@]} 份 sanitizer 报告"
  for r in "${reports[@]}"; do
    echo "  ---- $r ----"
    head -25 "$r" | sed 's/^/      /'
  done
fi

echo
echo "通过 $pass 条,失败 $fail 条"
[ "$fail" -eq 0 ]

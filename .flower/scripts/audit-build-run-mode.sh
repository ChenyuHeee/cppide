#!/usr/bin/env bash
# desc: 验收 3/4/5 —— 编译错误跳行(多个不同行号各测一次)、运行喂 stdin 看 stdout 与退出码(面板与 stdin_file 两条路)、模式开关一眼可见
#
# 用法: bash audit-build-run-mode.sh
# 依赖: /tmp/ptydrive、/work/cppide

set -u
WORK=/work
TMP=/tmp/audit-brm
DRIVE=/tmp/ptydrive
mkdir -p "$TMP"; cd "$TMP" || exit 2

pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

cat > "$TMP/nocfg.json" <<'EOF'
{ "api_key": "", "tick_ms": 40, "run_timeout_ms": 8000, "compile_timeout_ms": 40000 }
EOF

# ===================================================================
# 验收 3:编译错误可见 + 跳到对应行号
# 造一个已知行号有错的文件。errline 变量控制错误落在第几行。
# 断言:①【编译】面板里出现该行号的诊断;②按 Ctrl-N 跳转后状态栏「行:列」落在该行。
# ===================================================================
mk_err_file() {  # mk_err_file <路径> <错误行号>
  local f="$1" n="$2" i
  : > "$f"
  echo '#include <cstdio>' >> "$f"
  i=2
  while [ "$i" -lt "$n" ]; do echo "int pad_$i = $i;" >> "$f"; i=$((i+1)); done
  echo 'this_is_not_valid_cpp @@@ ;' >> "$f"          # 第 n 行:故意语法错
  echo 'int main() { return 0; }' >> "$f"
}

for N in 3 7 12 20 31; do
  f="$TMP/err$N.cpp"
  mk_err_file "$f" "$N"
  # 先确认宿主编译器自己确实把错报在第 N 行(否则断言的是编译器不是编辑器)
  real=$(c++ -std=c++17 -fsyntax-only "$f" 2>&1 | grep -oE ':[0-9]+:' | head -1 | tr -d ':')
  if [ "$real" != "$N" ]; then
    echo "  (跳过 N=$N:宿主编译器把错报在第 $real 行)"; continue
  fi
  cat > "$TMP/s_err$N.txt" <<EOF
expect err$N.cpp
wait 400
send \\x02
expect 错误
wait 600
send \\x0e
wait 800
send \\x1b
wait 400
send ZZHERE
wait 400
send \\x0f
wait 900
send \\x18
wait 1500
EOF
  timeout 180 "$DRIVE" 34 120 "$TMP/s_err$N.txt" "$TMP/err$N.out" \
    -- "$WORK/cppide" --config "$TMP/nocfg.json" "$f" > "$TMP/err$N.log" 2>&1
  # ① 面板里出现该行号的诊断(诊断行形如 "err3.cpp:3:1: error: ...",过滤后冒号仍在)
  if grep -qE "err$N\.cpp *: *$N *:" "$TMP/err$N.out"; then
    ok "编译错误可见:第 $N 行的诊断出现在【编译】面板"
  else
    bad "第 $N 行的诊断没出现在面板"
    grep -oE 'err[0-9]+\.cpp *: *[0-9]+ *:' "$TMP/err$N.out" | head -3 | sed 's/^/        实际: /'
  fi
  # ② 跳行断言走**文件系统**,不看屏幕:
  #    Ctrl-N 跳转后原地敲入 ZZHERE 并存盘,ZZHERE 落在第几行就是光标真的在第几行。
  #    (不能读状态栏的「行:列」—— ncurses 是差量重绘,"1:1"->"12:23" 只会重写
  #     变化的那几个格子,过滤后的流里会残留成 "2:23",看起来像跳错了行。)
  landed=$(grep -n 'ZZHERE' "$f" | head -1 | cut -d: -f1)
  if [ "$landed" = "$N" ]; then
    ok "跳行生效:Ctrl-N 后光标真的落在第 $N 行(ZZHERE 落在该行)"
  else
    bad "跳行没落到第 $N 行(ZZHERE 落在第 ${landed:-无} 行)"
  fi
done

# ===================================================================
# 验收 4:运行编译产物 + 从【输入】面板喂 stdin + 看到 stdout 与退出码
# 程序:读 stdin 求和,打印 SUM=<和>,return 3
# ===================================================================
cat > "$TMP/sum.cpp" <<'EOF'
#include <cstdio>
int main() {
  long long s = 0, v;
  while (scanf("%lld", &v) == 1) s += v;
  printf("SUM=%lld\n", s);
  return 3;
}
EOF

# --- 4a:从【输入】面板(Alt-4)敲入数据,再 Ctrl-E 编译并运行 ---
cat > "$TMP/s_run.txt" <<'EOF'
expect sum.cpp
wait 400
send \x1b4
expect 输入
wait 400
send 11 22 33
wait 300
send \x1b
wait 300
send \x05
expect SUM=66
wait 500
EOF
timeout 240 "$DRIVE" 34 120 "$TMP/s_run.txt" "$TMP/run.out" \
  -- "$WORK/cppide" --config "$TMP/nocfg.json" "$TMP/sum.cpp" > "$TMP/run.log" 2>&1
if grep -q 'SUM=66' "$TMP/run.out"; then
  ok "【输入】面板喂 stdin:看到正确 stdout(SUM=66)"
else
  bad "没看到 SUM=66"; grep -o 'SUM=[0-9]*' "$TMP/run.out" | head -3 | sed 's/^/        /'
fi
if grep -qE '退出码 *[:：]? *3|退出码 3' "$TMP/run.out"; then
  ok "退出码 3 在面板里可见"
else
  bad "没看到退出码 3"; grep -o '退出码[^│]*' "$TMP/run.out" | head -3 | sed 's/^/        /'
fi

# --- 4b:走 stdin_file 配置项 ---
printf '100\n200\n300\n' > "$TMP/in.txt"
cat > "$TMP/cfg_stdinfile.json" <<EOF
{ "api_key": "", "tick_ms": 40, "run_timeout_ms": 8000,
  "compile_timeout_ms": 40000, "stdin_file": "$TMP/in.txt" }
EOF
cat > "$TMP/s_run2.txt" <<'EOF'
expect sum.cpp
wait 400
send \x05
expect SUM=600
wait 500
EOF
timeout 240 "$DRIVE" 34 120 "$TMP/s_run2.txt" "$TMP/run2.out" \
  -- "$WORK/cppide" --config "$TMP/cfg_stdinfile.json" "$TMP/sum.cpp" > "$TMP/run2.log" 2>&1
if grep -q 'SUM=600' "$TMP/run2.out"; then
  ok "stdin_file 配置项生效(SUM=600)"
else
  bad "stdin_file 没生效"; grep -o 'SUM=[0-9]*' "$TMP/run2.out" | head -3 | sed 's/^/        /'
fi
if grep -qE '退出码 *[:：]? *3|退出码 3' "$TMP/run2.out"; then
  ok "stdin_file 路径下退出码 3 也可见"
else
  bad "stdin_file 路径下没看到退出码 3"
fi

# ===================================================================
# 验收 5:模式开关可见,切换后确实变了
# ===================================================================
cat > "$TMP/m.cpp" <<'EOF'
int main() { return 0; }
EOF
cat > "$TMP/s_mode.txt" <<'EOF'
expect m.cpp
expect 练习模式
mark m1
send \x14
wait 700
expectfrom 写代码模式
mark m2
send \x14
wait 700
expectfrom 练习模式
wait 300
send \x18
wait 1500
EOF
timeout 120 "$DRIVE" 34 120 "$TMP/s_mode.txt" "$TMP/mode.out" \
  -- "$WORK/cppide" --config "$TMP/nocfg.json" "$TMP/m.cpp" > "$TMP/mode.log" 2>&1
if [ "$(grep -cE '^FAIL' "$TMP/mode.log")" = "0" ]; then
  ok "模式标识可见且 Ctrl-T 切换后确实改变(练习模式 <-> 写代码模式)"
else
  bad "模式切换断言失败"; grep -E '^FAIL' "$TMP/mode.log" | sed 's/^/        /'
fi

echo
echo "通过 $pass 条,失败 $fail 条"
[ "$fail" -eq 0 ]

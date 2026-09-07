#!/usr/bin/env bash
# desc: 验修审计问题 3(config.h 写死默认模型名):model 默认空串;三种配置组合下编辑/编译/运行照常、AI 状态提示正确、不崩;src/ 下无模型名字面量。
#
# 用法: bash fix-p3-model-verify.sh [端口]
# 全程假 key(FAKEKEY_P3_*)+ 127.0.0.1 假 SSE 端点,不联网。

set -u
PORT="${1:-19081}"
WORK=/work
TMP=/tmp/fix-p3-model
DRIVE=/tmp/ptydrive
# 可用 CPPIDE_BIN 指向别的二进制(例:make debug 的 ./cppide-debug,跑 ASan/UBSan)
BIN="${CPPIDE_BIN:-$WORK/cppide}"
mkdir -p "$TMP"; cd "$TMP" || exit 2
pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

[ -x "$DRIVE" ] || c++ -std=c++17 -O1 -o "$DRIVE" "$WORK/.flower/scripts/wave5-pty-drive.cpp" || exit 2

cat > "$TMP/p.cpp" <<'EOF'
#include <cstdio>
int main() { printf("P3_RUN_OK\n"); return 5; }
EOF

# ---------------------------------------------------------------- 静态断言
echo "== 3.0 src/ 下不再有写死的模型名 =="
if grep -rniE 'deepseek-(chat|coder|reasoner|v[0-9])|"gpt-[0-9a-z.]+"|claude-[0-9]|qwen[0-9-]|glm-[0-9]|moonshot-' --include='*.cpp' --include='*.h' "$WORK/src" ; then
  bad "src/ 下仍有形似模型名的字面量"
else
  ok "src/ 下没有任何形似模型名的字面量"
fi
if grep -qE '^\s*std::string model;\s*$' "$WORK/src/config.h"; then
  ok "config.h 的 model 无内置默认值(空串)"
else
  bad "config.h 的 model 仍带默认值"
fi
if grep -q '"model": ""' "$WORK/config.sample.json"; then
  ok "config.sample.json 里 model 留空"
else
  bad "config.sample.json 的 model 没留空"
fi

# 组合 A:完全没有配置文件
echo "== 3.A 配置文件不存在 =="
MISSING="$TMP/no-such-dir-$$/config.json"
cat > "$TMP/sA.txt" <<'EOF'
expect p.cpp
wait 500
send // zzA\n
wait 300
send \x0f
wait 700
send \x05
expect P3_RUN_OK
wait 500
send \x18
wait 1500
EOF
timeout 240 "$DRIVE" 34 120 "$TMP/sA.txt" "$TMP/A.out" \
  -- "$BIN" --config "$MISSING" "$TMP/p.cpp" > "$TMP/A.log" 2>&1
grep -q 'P3_RUN_OK' "$TMP/A.out" && ok "A:编译 + 运行照常" || bad "A:编译/运行不可用"
grep -q 'zzA' "$TMP/p.cpp" && ok "A:编辑 + 保存照常(磁盘有改动)" || bad "A:编辑/保存不可用"
grep -qE '退出码 *[:：]? *5' "$TMP/A.out" && ok "A:退出码 5 可见" || bad "A:退出码不可见"
grep -q 'AI 未配置' "$TMP/A.out" && ok "A:状态栏显示「AI 未配置」" || bad "A:状态栏没有「AI 未配置」"
grep -qE '被信号 (6|11)' "$TMP/A.log" && bad "A:崩溃" || ok "A:未崩溃"

# 组合 B:只填 key,不填 model(端点必须一次都不被请求)
echo "== 3.B 只填 api_key,不填 model =="
: > "$TMP/reqB.jsonl"
python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/reqB.jsonl" \
  > "$TMP/serverB.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1.5
cat > "$TMP/cfgB.json" <<EOF
{ "api_key": "FAKEKEY_P3_NOMODEL", "base_url": "http://127.0.0.1:$PORT",
  "chat_path": "/v1/chat/completions", "tick_ms": 60 }
EOF
# 切写代码模式 + 打字 + 停 6 秒(足够触发若干次自动请求)+ Ctrl-A 手动请求 + 看 AI 面板
cat > "$TMP/sB.txt" <<'EOF'
expect p.cpp
send \x14
expect 写代码模式
wait 400
send \sb
wait 6000
send \x01
wait 3000
send \x1b3
wait 1000
send \x05
expect P3_RUN_OK
wait 600
send \x18
wait 500
send y
wait 1200
EOF
timeout 240 "$DRIVE" 34 120 "$TMP/sB.txt" "$TMP/B.out" \
  -- "$BIN" --config "$TMP/cfgB.json" "$TMP/p.cpp" > "$TMP/B.log" 2>&1
nB=$(grep -c . "$TMP/reqB.jsonl")
[ "$nB" -eq 0 ] && ok "B:一次网络请求都没发(实测 $nB 次)" || bad "B:model 为空却发了 $nB 次请求"
grep -q 'AI 未配置' "$TMP/B.out" && ok "B:状态栏显示「AI 未配置」" || bad "B:状态栏没有「AI 未配置」"
grep -q 'model' "$TMP/B.out" && ok "B:界面提示里点明缺的是 model" || bad "B:界面没说缺 model"
grep -q 'P3_RUN_OK' "$TMP/B.out" && ok "B:编译 + 运行照常" || bad "B:编译/运行不可用"
grep -qE '被信号 (6|11)' "$TMP/B.log" && bad "B:崩溃" || ok "B:未崩溃"
# --doctor / --print-config 也要说清楚
outB=$(timeout 60 "$BIN" --config "$TMP/cfgB.json" --doctor 2>&1)
printf '%s' "$outB" | grep -q 'model 为空' && ok "B:--doctor 指出 model 为空" || bad "B:--doctor 没指出 model 为空"
printf '%s' "$outB" | grep -qF 'FAKEKEY_P3_NOMODEL' && bad "B:--doctor 泄漏了 key" || ok "B:--doctor 未泄漏 key"

# 组合 C:key + model 都填(假值 + 本地端点)
echo "== 3.C key 与 model 都填(假值 + 本地假端点)=="
: > "$TMP/reqC.jsonl"
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/reqC.jsonl" \
  > "$TMP/serverC.log" 2>&1 &
SRV=$!
sleep 1.5
cat > "$TMP/cfgC.json" <<EOF
{ "api_key": "FAKEKEY_P3_FULL", "model": "fake-model-P3-C",
  "base_url": "http://127.0.0.1:$PORT", "chat_path": "/v1/chat/completions", "tick_ms": 60 }
EOF
cat > "$TMP/sC.txt" <<'EOF'
expect p.cpp
expect AI 就绪
send \x14
expect 写代码模式
wait 400
send \sc
expect GHOSTMARK
wait 400
send \x09
wait 600
send \x0f
wait 900
send \x05
expect P3_RUN_OK
wait 600
send \x18
wait 500
send y
wait 1200
EOF
timeout 240 "$DRIVE" 34 120 "$TMP/sC.txt" "$TMP/C.out" \
  -- "$BIN" --config "$TMP/cfgC.json" "$TMP/p.cpp" > "$TMP/C.log" 2>&1
nC=$(grep -c . "$TMP/reqC.jsonl")
[ "$nC" -ge 1 ] && ok "C:端点收到 $nC 次请求(AI 真的在工作)" || bad "C:端点没收到请求"
grep -q 'fake-model-P3-C' "$TMP/reqC.jsonl" && ok "C:请求体里的 model 来自配置" || bad "C:请求体 model 不是配置值"
grep -q 'AI 就绪' "$TMP/C.out" && ok "C:状态栏显示「AI 就绪」" || bad "C:状态栏不是「AI 就绪」"
grep -q 'GHOSTMARK' "$TMP/C.out" && ok "C:ghost 显示出来了" || bad "C:ghost 没显示"
grep -q 'GHOSTMARK' "$TMP/p.cpp" && ok "C:Tab 接受后 ghost 落到磁盘(文件系统断言)" || bad "C:Tab 没接受 ghost"
grep -q 'P3_RUN_OK' "$TMP/C.out" && ok "C:编译 + 运行照常" || bad "C:编译/运行不可用"
grep -qE '被信号 (6|11)' "$TMP/C.log" && bad "C:崩溃" || ok "C:未崩溃"
# 复原样例文件(方便重复跑)
cat > "$TMP/p.cpp" <<'EOF'
#include <cstdio>
int main() { printf("P3_RUN_OK\n"); return 5; }
EOF

echo
echo "通过 $pass 条,失败 $fail 条"
[ "$fail" -eq 0 ]

#!/usr/bin/env bash
# desc: 验收 8 —— 无硬编码 key/模型名;配置缺失时编辑+编译+运行照常;key 的值绝不出现在 doctor/print-config/屏幕/错误信息里
#
# 用法: bash audit-config-key.sh [端口]
# 说明:全程只用假 key(UNIQUEKEY_...),不联网,base_url 指向本地假服务器。

set -u
PORT="${1:-18944}"
WORK=/work
TMP=/tmp/audit-key
DRIVE=/tmp/ptydrive
KEY='UNIQUEKEY_qX7vN2mZ8tR4wL9pJ3hB6sD1'
mkdir -p "$TMP"; cd "$TMP" || exit 2

pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

# ---------------------------------------------------------------- 静态扫描
echo "== 8.1 源码/二进制里没有硬编码 key =="
if grep -rnE '"sk-[A-Za-z0-9_-]{8,}"|"Bearer [A-Za-z0-9]' "$WORK/src"/*.cpp "$WORK/src"/*.h >/dev/null 2>&1; then
  bad "源码里出现疑似硬编码 key 字面量"
  grep -rnE '"sk-[A-Za-z0-9_-]{8,}"|"Bearer [A-Za-z0-9]' "$WORK/src"/*.cpp "$WORK/src"/*.h | sed 's/^/        /'
else
  ok "源码里没有 sk-* / Bearer <literal> 形态的 key 字面量"
fi
if strings "$WORK/cppide" | grep -qE '^sk-[A-Za-z0-9]{8,}'; then
  bad "二进制里有 sk-* 字符串"
else
  ok "二进制里没有 sk-* 字符串"
fi
# 模型名:必须完全来自配置,代码里一个字面量都不许有
if grep -rniE 'deepseek-(chat|coder|reasoner|v[0-9])|"gpt-[0-9a-z.]+"|claude-[0-9]|qwen[0-9-]|glm-[0-9]|moonshot-' \
     --include='*.cpp' --include='*.h' "$WORK/src" >/dev/null 2>&1; then
  bad "src/ 下出现了形似模型名的字面量(需求:代码里不许写死模型名)"
else
  ok "src/ 下没有任何形似模型名的字面量(model 默认空串,只从配置读)"
fi

# ---------------------------------------------------------------- 配置缺失
echo "== 8.2 配置缺失(指向不存在的路径)时编辑/编译/运行照常 =="
MISSING="$TMP/definitely-does-not-exist-$$/config.json"
cat > "$TMP/e.cpp" <<'EOF'
#include <cstdio>
int main() { printf("NOCFG_RUN_OK\n"); return 7; }
EOF
cat > "$TMP/s_nocfg.txt" <<'EOF'
expect e.cpp
wait 500
send // zzedit\n
wait 400
send \x05
expect NOCFG_RUN_OK
wait 600
send \x0f
wait 900
send \x18
wait 1800
EOF
timeout 240 "$DRIVE" 34 120 "$TMP/s_nocfg.txt" "$TMP/nocfg.out" \
  -- "$WORK/cppide" --config "$MISSING" "$TMP/e.cpp" > "$TMP/nocfg.log" 2>&1
grep -q 'NOCFG_RUN_OK' "$TMP/nocfg.out" \
  && ok "配置缺失:编译 + 运行照常(看到程序 stdout)" \
  || { bad "配置缺失时编译/运行不可用"; grep -E '^FAIL' "$TMP/nocfg.log" | sed 's/^/        /'; }
grep -q 'zzedit' "$TMP/e.cpp" \
  && ok "配置缺失:编辑 + 保存照常(磁盘有改动)" \
  || bad "配置缺失时编辑/保存不可用"
grep -qE '退出码 *[:：]? *7' "$TMP/nocfg.out" \
  && ok "配置缺失:退出码 7 仍然可见" || bad "配置缺失:退出码不可见"
if grep -qE '被信号 (6|11)' "$TMP/nocfg.log"; then bad "配置缺失时崩溃"; else ok "配置缺失时未崩溃"; fi
# AI 应当只提示未配置
grep -qE '未配置|api_key' "$TMP/nocfg.out" \
  && ok "配置缺失:界面上给出「AI 未配置」类提示" \
  || echo "  (注:本轮没主动触发 AI,未强制要求出现提示)"

# ---------------------------------------------------------------- key 泄漏
echo "== 8.3 key 的值绝不出现在任何输出里 =="
cat > "$TMP/withkey.json" <<EOF
{ "api_key": "$KEY", "model": "audit-model-xyz",
  "base_url": "http://127.0.0.1:$PORT", "chat_path": "/err401",
  "ai_connect_timeout_ms": 2000, "ai_timeout_ms": 4000,
  "ghost_min_interval_ms": 60000, "tick_ms": 40 }
EOF

for mode in --doctor --print-config; do
  out=$(timeout 60 "$WORK/cppide" --config "$TMP/withkey.json" $mode 2>&1)
  if printf '%s' "$out" | grep -qF "$KEY"; then
    bad "$mode 输出里出现了 key 的值"
  else
    ok "$mode 输出里没有 key 的值"
  fi
done
# 环境变量注入路径也测一遍
out=$(CPPIDE_API_KEY="$KEY" timeout 60 "$WORK/cppide" --doctor 2>&1)
printf '%s' "$out" | grep -qF "$KEY" && bad "CPPIDE_API_KEY 经 --doctor 泄漏" || ok "环境变量注入的 key 未经 --doctor 泄漏"

# 真跑一轮 AI 失败(401),检查屏幕/面板/错误提示里不出现 key
python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/req.jsonl" \
  > "$TMP/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1.5
cat > "$TMP/k.cpp" <<'EOF'
int main() { return 0; }
EOF
cat > "$TMP/s_key.txt" <<'EOF'
expect k.cpp
send \x14
expect 写代码模式
wait 300
send \x01
wait 5000
send \x1b3
wait 1000
send \x18
wait 1500
EOF
timeout 180 "$DRIVE" 34 120 "$TMP/s_key.txt" "$TMP/key.out" \
  -- "$WORK/cppide" --config "$TMP/withkey.json" "$TMP/k.cpp" > "$TMP/key.log" 2>&1
grep -qF "$KEY" "$TMP/key.out" "$TMP/key.out.raw" \
  && bad "AI 错误路径把 key 打到了屏幕上" \
  || ok "AI 错误路径(401)屏幕与面板里都没有 key 的值"
grep -q 'API key 无效' "$TMP/key.out" \
  && ok "401 时给出中文提示「API key 无效或已过期」" || bad "401 没有中文提示"
# key 确实被送出去了(证明它是从配置读的,不是写死的)
if grep -qF "Bearer $KEY" "$TMP/req.jsonl" 2>/dev/null; then
  ok "服务端收到的 Authorization 正是配置里的 key(证明 key 来自配置)"
else
  bad "服务端没收到配置里的 key"
fi
# model 也来自配置
if grep -q 'audit-model-xyz' "$TMP/req.jsonl" 2>/dev/null; then
  ok "请求体里的 model 正是配置里的 audit-model-xyz(证明模型名来自配置)"
else
  bad "请求体里的 model 不是配置值"
fi

echo
echo "通过 $pass 条,失败 $fail 条"
[ "$fail" -eq 0 ]

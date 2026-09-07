#!/usr/bin/env bash
# desc: 复现「写代码模式空转烧钱」—— 切到写代码模式后完全不动键盘,统计单位时间内自动发出的 AI 请求数
#
# 用法: bash audit-idle-cost.sh [端口] [空转秒数]
# 背景:AiService::maybeAutoTrigger(src/ai.cpp:203)只看"距上次活动 >= ghost_delay_ms"
#       与"距上次自动请求 >= ghost_min_interval_ms"两个时间阈值,**没有**任何
#       "这次停顿已经请求过了"的状态位。于是光标停着不动时,每隔
#       ghost_min_interval_ms 就会再发一次,永不停止。

set -u
PORT="${1:-19010}"
IDLE="${2:-20}"
WORK=/work
TMP=/tmp/audit-idle
DRIVE=/tmp/ptydrive
mkdir -p "$TMP"; cd "$TMP" || exit 2
: > "$TMP/req.jsonl"

python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/req.jsonl" \
  > "$TMP/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1.5

# 全部用默认值:ghost_delay_ms=500, ghost_min_interval_ms=1200
cat > "$TMP/cfg.json" <<EOF
{ "api_key": "FAKEKEY_IDLE", "model": "m", "base_url": "http://127.0.0.1:$PORT",
  "chat_path": "/v1/chat/completions", "tick_ms": 60 }
EOF
cat > "$TMP/s.cpp" <<'EOF'
#include <cstdio>
int main() { int x = 1; return 0; }
EOF

cat > "$TMP/s.txt" <<EOF
expect s.cpp
send \\x14
expect 写代码模式
wait 300
send \\sy
wait $((IDLE * 1000))
send \\x18
wait 1500
EOF

timeout $((IDLE + 60)) "$DRIVE" 30 110 "$TMP/s.txt" "$TMP/s.out" \
  -- "$WORK/cppide" --config "$TMP/cfg.json" "$TMP/s.cpp" > "$TMP/s.log" 2>&1

n=$(grep -c . "$TMP/req.jsonl")
echo
echo "配置:全默认(ghost_delay_ms=500,ghost_min_interval_ms=1200)"
echo "操作:切到写代码模式 -> 敲 1 个字符 -> 之后 ${IDLE} 秒完全不碰键盘"
echo "结果:服务端收到 $n 次 chat/completions 请求"
echo "折算:约 $(python3 -c "print(round($n*60.0/$IDLE,1))") 次/分钟,$(python3 -c "print(round($n*3600.0/$IDLE))") 次/小时"
echo
if [ "$n" -le 2 ]; then
  echo "PASS  空转不会持续发请求"
  exit 0
else
  echo "FAIL  空转持续发请求 —— 挂着编辑器不动就会一直计费"
  echo "      根因:src/ai.cpp:203 maybeAutoTrigger 只有两个时间阈值,"
  echo "            缺少"本次停顿已请求过"的状态位。"
  exit 1
fi

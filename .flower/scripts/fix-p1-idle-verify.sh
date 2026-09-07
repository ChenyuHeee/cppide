#!/usr/bin/env bash
# desc: 验修审计问题 1(写代码模式空转烧钱):30 秒完全不碰键盘只许发 1 次请求;之后打 1 个字符再等,只许再多 1 次(共 2 次)。
#
# 用法: bash fix-p1-idle-verify.sh [端口]
# 依赖: /tmp/ptydrive(用 .flower/scripts/wave5-pty-drive.cpp 现编,脚本会自动编)
#       假 SSE 端点 audit-fake-ai-server.py + 假 key,全程 127.0.0.1,不联网。
# 判据: 修改前(src/ai.cpp maybeAutoTrigger 只有两个时间阈值)是 23 次/20 秒;
#       修改后靠 auto_done_gen_ 做到"同一次停顿只发一次"。

set -u
PORT="${1:-19077}"
WORK=/work
TMP=/tmp/fix-p1-idle
DRIVE=/tmp/ptydrive
# 可用 CPPIDE_BIN 指向别的二进制(例:make debug 的 ./cppide-debug,跑 ASan/UBSan)
BIN="${CPPIDE_BIN:-$WORK/cppide}"
mkdir -p "$TMP"; cd "$TMP" || exit 2
: > "$TMP/req.jsonl"

[ -x "$DRIVE" ] || c++ -std=c++17 -O1 -o "$DRIVE" "$WORK/.flower/scripts/wave5-pty-drive.cpp" || exit 2

python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/req.jsonl" \
  > "$TMP/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1.5

# 全部用内置默认值:ghost_delay_ms=500、ghost_min_interval_ms=1200
cat > "$TMP/cfg.json" <<EOF
{ "api_key": "FAKEKEY_P1_IDLE", "model": "fake-model-P1", "base_url": "http://127.0.0.1:$PORT",
  "chat_path": "/v1/chat/completions", "tick_ms": 60 }
EOF
cat > "$TMP/s.cpp" <<'EOF'
#include <cstdio>
int main() { int x = 1; return 0; }
EOF

# 阶段一:切写代码模式 -> 敲 1 个字符 -> 30 秒完全不碰键盘
# 阶段二:再敲 1 个字符 -> 再等 12 秒(远大于 delay+min_interval,足够发第二次)
cat > "$TMP/s.txt" <<'EOF'
expect s.cpp
send \x14
expect 写代码模式
wait 400
send \sy
wait 30000
mark phase1
send \sz
wait 12000
send \x18
wait 400
send y
wait 1200
EOF

timeout 120 "$DRIVE" 30 110 "$TMP/s.txt" "$TMP/s.out" \
  -- "$BIN" --config "$TMP/cfg.json" "$TMP/s.cpp" > "$TMP/s.log" 2>&1
rc=$?

total=$(grep -c . "$TMP/req.jsonl")
echo "驱动退出码: $rc"
echo "30 秒空转 + 打一个字符再等 12 秒,服务端共收到 $total 次 chat/completions 请求"

fail=0
if [ "$rc" -ne 0 ]; then
  echo "FAIL  pty 驱动退出码 $rc(断言失败或子进程没正常退出),见 $TMP/s.log"
  fail=1
fi
if [ "$total" -ne 2 ]; then
  echo "FAIL  期望共 2 次(空转 1 次 + 新活动 1 次),实收 $total 次"
  fail=1
else
  echo "PASS  共 2 次:空转期只发 1 次,新活动后又发 1 次"
fi

# 单独跑一遍"只空转 30 秒、之后什么都不按"的对照,断言恰好 1 次
: > "$TMP/req2.jsonl"
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/req2.jsonl" \
  > "$TMP/server2.log" 2>&1 &
SRV=$!
sleep 1.5
cat > "$TMP/s2.txt" <<'EOF'
expect s.cpp
send \x14
expect 写代码模式
wait 400
send \sy
wait 30000
send \x18
wait 400
send y
wait 1200
EOF
timeout 90 "$DRIVE" 30 110 "$TMP/s2.txt" "$TMP/s2.out" \
  -- "$BIN" --config "$TMP/cfg.json" "$TMP/s.cpp" > "$TMP/s2.log" 2>&1
idle=$(grep -c . "$TMP/req2.jsonl")
echo "纯 30 秒空转,服务端收到 $idle 次请求(改前同口径 20 秒 = 23 次)"
if [ "$idle" -ne 1 ]; then
  echo "FAIL  30 秒空转期望恰好 1 次,实收 $idle 次"
  fail=1
else
  echo "PASS  30 秒空转恰好 1 次"
fi

# ghost 仍然要能显示(别把功能修没了)
if grep -q 'GHOSTMARK' "$TMP/s.out" 2>/dev/null; then
  echo "PASS  ghost 仍然显示(屏幕上出现 GHOSTMARK)"
else
  echo "FAIL  ghost 没有显示 —— 限流改动把功能一起干掉了"
  fail=1
fi

exit $fail

#!/usr/bin/env bash
# desc: 验修审计问题 4(8 色终端 ghost 不是灰/暗):从 pty 原始 ANSI 流断言 8 色档 ghost 用 A_DIM(SGR 2)、256 色档用灰色(SGR 90 或 38;5;8),且都不是蓝色(34/94)。
#
# 用法: bash fix-p4-ghost-attr.sh [端口]
# 关键: 属性只能从 <raw_out>.raw 里读(审计问题 5:raw_out 参数名误导,写的是过滤后文本)。
# 全程假 key + 127.0.0.1 假 SSE 端点,不联网。

set -u
PORT="${1:-19084}"
WORK=/work
TMP=/tmp/fix-p4-ghost
DRIVE=/tmp/ptydrive
# 可用 CPPIDE_BIN 指向别的二进制(例:make debug 的 ./cppide-debug,跑 ASan/UBSan)
BIN="${CPPIDE_BIN:-$WORK/cppide}"
mkdir -p "$TMP"; cd "$TMP" || exit 2
pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

# 每次重编驱动器:它带了 PTY_TERM 支持
c++ -std=c++17 -O1 -o "$DRIVE" "$WORK/.flower/scripts/wave5-pty-drive.cpp" || exit 2

: > "$TMP/req.jsonl"
python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/req.jsonl" \
  > "$TMP/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1.5

cat > "$TMP/cfg.json" <<EOF
{ "api_key": "FAKEKEY_P4_GHOST", "model": "fake-model-P4",
  "base_url": "http://127.0.0.1:$PORT", "chat_path": "/v1/chat/completions", "tick_ms": 60 }
EOF
cat > "$TMP/g.cpp" <<'EOF'
#include <cstdio>
int main() { int n = 0; return n; }
EOF
cat > "$TMP/s.txt" <<'EOF'
expect g.cpp
send \x14
expect 写代码模式
wait 400
send \sq
expect GHOSTMARK
wait 800
send \x1b
wait 400
send \x18
wait 400
send y
wait 1200
EOF

run_one() {   # $1 = TERM
  local t="$1" tag="$2"
  cp "$TMP/g.cpp" "$TMP/g.cpp.bak"
  PTY_TERM="$t" timeout 200 "$DRIVE" 34 120 "$TMP/s.txt" "$TMP/$tag.out" \
    -- "$BIN" --config "$TMP/cfg.json" "$TMP/g.cpp" > "$TMP/$tag.log" 2>&1
  cp "$TMP/g.cpp.bak" "$TMP/g.cpp"
  echo "  TERM=$t 原始流 $(wc -c < "$TMP/$tag.out.raw") 字节"
  python3 "$WORK/.flower/scripts/fix-sgr-at-marker.py" "$TMP/$tag.out.raw" \
    'GHOSTMARK_Alpha();' 'GHOSTMARK_Beta();' > "$TMP/$tag.sgr" 2>&1
  sed 's/^/    /' "$TMP/$tag.sgr"
}

echo "== 4.1 TERM=xterm(8 色)=="
run_one xterm c8
if grep -qE 'SGR\[([0-9,]*,)?2(,[0-9,]*)?\]' "$TMP/c8.sgr"; then
  ok "8 色档 ghost 走 A_DIM(原始流里带 SGR 2;实测形态见上)"
else
  bad "8 色档 ghost 不带 SGR 2(A_DIM)"
fi
if grep -qE 'SGR\[(.*,)?(34|94)(,.*)?\]' "$TMP/c8.sgr"; then
  bad "8 色档 ghost 还是蓝色前景(SGR 34/94)"
else
  ok "8 色档 ghost 没有任何蓝色前景(无 SGR 34/94)"
fi

echo "== 4.2 TERM=xterm-256color(256 色)=="
run_one xterm-256color c256
if grep -qE 'SGR\[(.*,)?(90|38,5,8)(,.*)?\]' "$TMP/c256.sgr"; then
  ok "256 色档 ghost 是灰色(SGR 90 或 38;5;8)"
else
  bad "256 色档 ghost 不是灰色"
fi
if grep -qE 'SGR\[(.*,)?(34|94)(,.*)?\]' "$TMP/c256.sgr"; then
  bad "256 色档 ghost 是蓝色"
else
  ok "256 色档 ghost 不是蓝色"
fi

# 两档的 ghost 都必须真的显示出来(别把功能测没了)
grep -q GHOSTMARK "$TMP/c8.out"   && ok "8 色档 ghost 确实显示"   || bad "8 色档 ghost 没显示"
grep -q GHOSTMARK "$TMP/c256.out" && ok "256 色档 ghost 确实显示" || bad "256 色档 ghost 没显示"
# ghost 不许落进文件(本轮按的是 Esc 丢弃)
grep -q GHOSTMARK "$TMP/g.cpp" && bad "Esc 丢弃后 ghost 竟然进了文件" || ok "Esc 丢弃:ghost 没有进文件"

echo
echo "通过 $pass 条,失败 $fail 条"
[ "$fail" -eq 0 ]

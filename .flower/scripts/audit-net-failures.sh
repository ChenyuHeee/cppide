#!/usr/bin/env bash
# desc: 验收 9 —— 五种 AI 异常路径(连不上/秒断连/HTTP 500/畸形 JSON/超慢)各走一次,断言有可见中文提示且键盘不卡死
#
# 用法: bash audit-net-failures.sh [端口]
# 依赖: /tmp/ptydrive(由 wave5-pty-drive.cpp 编好)、audit-fake-ai-server.py、/work/cppide
# 不联网:除"不可路由地址"一项走 TEST-NET-3(203.0.113.0/24,RFC5737 保留)外,全部指向 127.0.0.1。

set -u
PORT="${1:-18931}"
WORK=/work
TMP=/tmp/audit-net
DRIVE=/tmp/ptydrive
mkdir -p "$TMP"
cd "$TMP" || exit 2

pass=0; fail=0
ok()   { echo "  PASS  $*"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $*"; fail=$((fail+1)); }

# ---------------------------------------------------------------- 假服务器
python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/req.jsonl" \
  > "$TMP/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1.5
kill -0 $SRV 2>/dev/null || { echo "假服务器起不来"; exit 2; }

mkcfg() {  # mkcfg <文件> <base_url> <chat_path> <connect_ms> <total_ms>
  cat > "$1" <<EOF
{
  "api_key": "AUDITFAKEKEY_ZZZ9",
  "model": "audit-model-xyz",
  "base_url": "$2",
  "chat_path": "$3",
  "stream": true,
  "ai_connect_timeout_ms": $4,
  "ai_timeout_ms": $5,
  "ghost_min_interval_ms": 60000,
  "tick_ms": 40
}
EOF
}

mksrc() { cat > "$1" <<'EOF'
#include <cstdio>
int main() {
  int x = 1;
  return 0;
}
EOF
}

# 场景 A:触发 AI -> 不碰键盘 -> 断言出现中文错误提示
# (刻意不打字:任何输入都会 noteActivity -> gen++ -> 在飞请求作废,错误事件会被正确丢弃)
run_err() {  # run_err <名字> <cfg> <期望中文片段> <等待ms>
  local name="$1" cfg="$2" want="$3" ms="$4"
  mksrc "$TMP/$name.cpp"
  cat > "$TMP/$name.txt" <<EOF
expect $name.cpp
send \\x14
expect 写代码模式
wait 300
send \\x01
wait $ms
send \\x1b3
wait 600
EOF
  timeout 180 "$DRIVE" 34 120 "$TMP/$name.txt" "$TMP/$name.raw" \
    -- "$WORK/cppide" --config "$cfg" "$TMP/$name.cpp" > "$TMP/$name.log" 2>&1
  if grep -q "$want" "$TMP/$name.raw"; then
    ok "[$name] 屏幕上出现中文提示「$want」"
  else
    bad "[$name] 没找到「$want」"
    grep -o '错误[^ ]*\|失败[^ ]*\|超时[^ ]*' "$TMP/$name.raw" | sort -u | head -5 | sed 's/^/        实际: /'
  fi
  # 不崩:进程必须是被驱动器 SIGKILL 或正常退出,不能是 SIGSEGV/SIGABRT
  if grep -qE '被信号 (6|11)|Segmentation|Aborted' "$TMP/$name.log"; then
    bad "[$name] 进程崩溃(SIGSEGV/SIGABRT)"
  else
    ok "[$name] 未崩溃"
  fi
}

# 场景 B:请求在飞期间狂敲键盘 -> 断言字符真的被吃进缓冲区(输入没被阻塞)
# 断言走**文件系统**:在飞期间敲的字符,存盘后必须出现在磁盘上。
# 不能 expect 屏幕上的连续字符串,也不能 expect 状态栏的「行:列」——
# ncurses 是差量重绘,"1:8"->"1:9" 只重写变化的那一个格子,
# 过滤后的流里根本不会出现完整的 "1:9"(这是驱动器的固有限制,不是产品问题)。
run_alive() {  # run_alive <名字> <cfg>
  local name="$1" cfg="$2"
  mksrc "$TMP/$name.cpp"
  cat > "$TMP/$name.txt" <<EOF
expect $name.cpp
send \\x14
expect 写代码模式
wait 300
send \\x01
wait 200
send ZZALIVE1
wait 400
send ZZALIVE2
wait 400
send \\x0f
wait 1000
send \\x18
wait 2000
EOF
  timeout 180 "$DRIVE" 34 120 "$TMP/$name.txt" "$TMP/$name.raw" \
    -- "$WORK/cppide" --config "$cfg" "$TMP/$name.cpp" > "$TMP/$name.log" 2>&1
  if grep -q 'ZZALIVE1ZZALIVE2' "$TMP/$name.cpp"; then
    ok "[$name] 在飞期间键盘输入仍然生效(敲的字符全部进了缓冲区并存盘)"
  else
    bad "[$name] 在飞期间输入被阻塞或丢字"
    head -2 "$TMP/$name.cpp" | sed 's/^/        磁盘: /'
  fi
}

echo "== 1) 不可路由地址(连接超时)=="
mkcfg "$TMP/c1.json" "http://203.0.113.1:9" "/v1/chat/completions" 2000 4000
run_err net1 "$TMP/c1.json" "超时" 9000

echo "== 2) 本地端口立即断连 =="
mkcfg "$TMP/c2.json" "http://127.0.0.1:$PORT" "/drop" 3000 8000
run_err net2 "$TMP/c2.json" "没有返回任何数据" 6000

echo "== 3) HTTP 500 =="
mkcfg "$TMP/c3.json" "http://127.0.0.1:$PORT" "/err500" 3000 8000
run_err net3 "$TMP/c3.json" "服务端错误" 6000

echo "== 4) 畸形 JSON =="
mkcfg "$TMP/c4.json" "http://127.0.0.1:$PORT" "/badjson" 3000 8000
run_err net4 "$TMP/c4.json" "AI" 6000

echo "== 4b) 完全不是 JSON 的正文 =="
mkcfg "$TMP/c4b.json" "http://127.0.0.1:$PORT" "/notjson" 3000 8000
run_err net4b "$TMP/c4b.json" "AI" 6000

echo "== 5) 超慢响应(读超时)=="
mkcfg "$TMP/c5.json" "http://127.0.0.1:$PORT" "/slow" 3000 4000
run_err net5 "$TMP/c5.json" "超时" 10000

echo "== 6) 无效 key(HTTP 401,真实形态)=="
mkcfg "$TMP/c6.json" "http://127.0.0.1:$PORT" "/err401" 3000 8000
run_err net6 "$TMP/c6.json" "API key 无效" 6000

echo "== 键盘存活(在飞期间)=="
run_alive alive_slow "$TMP/c5.json"
mkcfg "$TMP/c1b.json" "http://203.0.113.1:9" "/v1/chat/completions" 8000 12000
run_alive alive_unroutable "$TMP/c1b.json"

echo
echo "通过 $pass 条,失败 $fail 条"
[ "$fail" -eq 0 ]

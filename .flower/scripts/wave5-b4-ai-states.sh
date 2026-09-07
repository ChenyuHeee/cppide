#!/bin/sh
# desc: B4 —— 三种 AI 异常态实测:无配置 / 错 key(本地假服务器回 401)/ 无网络(连不上)
# 每种都跑同一份 pty 脚本(wave5-b4-ai-common.txt),断言:
#   * 【AI】面板里有可见中文提示
#   * 编辑照常(打字可见)、编译+运行照常(能看到程序输出与退出码)
#   * 输入不卡(bench 的单帧耗时打印出来供人核对)
# ★ 全程只用假 key,绝不接触任何真实 API key / 真实 api.deepseek.com。
set -u

BIN=${BIN:-/work/cppide}
PTY=${PTY:-/tmp/pty5}
SCRIPT=${SCRIPT:-/work/.flower/scripts/wave5-b4-ai-common.txt}
WORK=${WORK:-/tmp/w5b4}
FAKE_KEY='sk-FAKE-0000000000000000000000000000'

rm -rf "$WORK"; mkdir -p "$WORK"

# 被编辑/编译/运行的样本文件。
# ★ 输出串在运行期拼出来("AI_RUN" + "_" + 42),源码里没有 AI_RUN_42 这个字面量 ——
#   否则 pty 断言会把"编辑区里显示的源码"当成"程序真的跑过了"(踩过)。
cat > "$WORK/probe.cpp" <<'EOF'
#include <cstdio>
int main() {
  const char* tag = "AI_RUN";
  std::printf("%s_%d\n", tag, 42);
  return 0;
}
EOF

# ---------------------------------------------------------------- 假 401 服务器
# 用 python3 起一个只会回 401 + DeepSeek 风格错误体的 HTTP 服务器。
cat > "$WORK/fake401.py" <<'EOF'
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        self.rfile.read(n)
        body = json.dumps({"error": {"message": "Authentication Fails, Your api key is invalid",
                                     "type": "authentication_error", "code": "invalid_request_error"}}).encode()
        self.send_response(401)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a):
        pass
srv = HTTPServer(('127.0.0.1', int(sys.argv[1])), H)
srv.serve_forever()
EOF

PORT=18731
python3 "$WORK/fake401.py" "$PORT" &
FAKE_PID=$!
sleep 1

cleanup() { kill "$FAKE_PID" 2>/dev/null; }
trap cleanup EXIT INT TERM

# ------------------------------------------------------------------ 三份配置
# (1) 无配置:--config 指一个不存在的文件 => api_key 空 => AI 关闭
cat > "$WORK/none.json" <<'EOF'
{ "cc": "cc", "cxx": "c++", "run_timeout_ms": 20000, "tick_ms": 60 }
EOF
# 真正的"无配置"用一个不存在的路径,下面单独处理

# (2) 错 key:打本地假服务器,必然 401
cat > "$WORK/badkey.json" <<EOF
{
  "api_key": "$FAKE_KEY",
  "model": "fake-model",
  "base_url": "http://127.0.0.1:$PORT",
  "chat_path": "/v1/chat/completions",
  "stream": true,
  "ghost_delay_ms": 300, "ghost_min_interval_ms": 300,
  "ai_connect_timeout_ms": 2000, "ai_timeout_ms": 5000,
  "cc": "cc", "cxx": "c++", "run_timeout_ms": 20000, "tick_ms": 60
}
EOF

# (3) 无网络:不可路由地址 + 短连接超时
cat > "$WORK/nonet.json" <<EOF
{
  "api_key": "$FAKE_KEY",
  "model": "fake-model",
  "base_url": "http://10.255.255.1:9",
  "chat_path": "/v1/chat/completions",
  "stream": true,
  "ghost_delay_ms": 300, "ghost_min_interval_ms": 300,
  "ai_connect_timeout_ms": 2500, "ai_timeout_ms": 4000,
  "cc": "cc", "cxx": "c++", "run_timeout_ms": 20000, "tick_ms": 60
}
EOF

fails=0

run_case() {
  name=$1; shift
  must=$1; shift
  out="$WORK/$name.out"
  echo ""
  echo "======== [$name] ========"
  cp "$WORK/probe.cpp" "$WORK/$name.cpp"
  # 绝不让子进程读到本机真实 key
  CPPIDE_API_KEY= DEEPSEEK_API_KEY= \
    timeout 240 "$PTY" 24 80 "$SCRIPT" "$out" -- "$BIN" "$@" "$WORK/$name.cpp"
  rc=$?
  if [ "$rc" != 0 ]; then
    echo "  [$name] pty 脚本有失败(rc=$rc)"
    fails=$((fails + 1))
  fi
  # 中文提示断言
  for m in $must; do
    if grep -q -- "$m" "$out"; then
      echo "  PASS  【AI】面板/状态栏里出现中文提示片段: $m"
    else
      echo "  FAIL  找不到中文提示片段: $m"
      fails=$((fails + 1))
    fi
  done
  # 绝不能把 key 原文打到屏幕上
  if grep -q "$FAKE_KEY" "$out"; then
    echo "  FAIL  屏幕上出现了 api_key 原文(必须被 scrubKey 遮掉)"
    fails=$((fails + 1))
  else
    echo "  PASS  屏幕上没有 api_key 原文"
  fi
}

# (1) 无配置文件:路径不存在
run_case nocfg "AI 未配置 api_key" --config "$WORK/does-not-exist.json"
# (1b) 有配置文件但 api_key 缺失
run_case emptykey "AI 未配置 api_key" --config "$WORK/none.json"
# (2) 错 key -> 401
run_case badkey "AI 错误 key 无效或已过期" --config "$WORK/badkey.json"
# (3) 无网络 -> 连不上
run_case nonet "AI 错误 请求超时" --config "$WORK/nonet.json"

echo ""
echo "======== B4 汇总:失败 $fails 条 ========"
exit $([ "$fails" = 0 ] && echo 0 || echo 1)

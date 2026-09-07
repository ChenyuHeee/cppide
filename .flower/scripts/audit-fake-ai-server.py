# desc: 假的 OpenAI 兼容 SSE 端点 —— 审计用。按 URL 路径切换正常/违规/错误/超时/断连等行为,并把收到的请求落盘供断言。
#
# 用法: python3 audit-fake-ai-server.py <port> <reqlog_path>
#
# 路径语义(cppide 的 chat_path 指向哪个就走哪个):
#   /v1/chat/completions  正常 SSE 流,吐一段简短 C++ 补全(写代码模式用)
#   /practice             正常 SSE 流,但**故意违规**吐一整段带 ``` 围栏的完整代码
#   /err500               HTTP 500 + OpenAI 风格错误体
#   /err401               HTTP 401(无效 key 的真实形态)
#   /badjson              200 + SSE 帧里塞畸形 JSON
#   /notjson              200 + 完全不是 JSON 的垃圾正文(非流式路径)
#   /slow                 收到请求后长睡,用于触发客户端超时
#   /drop                 立刻关掉 socket,不回任何字节
#
# 请求日志:每收到一个请求,追加一行 JSON 到 <reqlog_path>,含 path/headers/body。
# 注意:本脚本只监听 127.0.0.1,不联网,不需要任何真实凭据。

import json
import socket
import socketserver
import sys
import threading
import time

PORT = int(sys.argv[1])
REQLOG = sys.argv[2]

_loglock = threading.Lock()


def log_request(path, headers, body):
    rec = {"path": path, "headers": dict(headers), "body": body}
    with _loglock:
        with open(REQLOG, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")


def sse_frames(chunks, reasoning=None):
    """把若干文本片段编码成 OpenAI 兼容的 SSE 帧串。"""
    out = []
    # 首帧:role-only(真实服务端就是这么发的)
    out.append('data: {"choices":[{"delta":{"role":"assistant"},"index":0}]}\n\n')
    if reasoning:
        for r in reasoning:
            out.append("data: " + json.dumps(
                {"choices": [{"delta": {"reasoning_content": r}, "index": 0}]},
                ensure_ascii=False) + "\n\n")
    for c in chunks:
        out.append("data: " + json.dumps(
            {"choices": [{"delta": {"content": c}, "index": 0}]},
            ensure_ascii=False) + "\n\n")
    out.append('data: {"choices":[{"delta":{},"finish_reason":"stop","index":0}]}\n\n')
    out.append("data: [DONE]\n\n")
    return out


# 写代码模式期望的补全:一小段能明确认出来的 C++ 代码
CODE_CHUNKS = ["GHOSTMARK_A", "lpha();\n", "    GHOSTMARK_Beta();"]

# 练习模式的**违规**回复:明确含完整代码块 + 中文思路
PRACTICE_CHUNKS = [
    "思路:这题可以用前缀和。\n",
    "复杂度 O(n)。\n",
    "```cpp\n",
    "int PRACTICE_LEAK_CODE(int n){ return n*2; }\n",
    "```\n",
    "下一步:想清楚边界。\n",
]


class Handler(socketserver.StreamRequestHandler):
    timeout = 120

    def _read_request(self):
        line = self.rfile.readline(65536)
        if not line:
            return None, None, None
        parts = line.decode("latin1").split()
        if len(parts) < 2:
            return None, None, None
        path = parts[1]
        headers = {}
        while True:
            h = self.rfile.readline(65536)
            if not h or h in (b"\r\n", b"\n"):
                break
            t = h.decode("latin1").rstrip("\r\n")
            if ":" in t:
                k, v = t.split(":", 1)
                headers[k.strip()] = v.strip()
        body = ""
        n = int(headers.get("Content-Length", "0") or 0)
        if n > 0:
            body = self.rfile.read(n).decode("utf-8", "replace")
        return path, headers, body

    def _send(self, data):
        self.wfile.write(data.encode("utf-8"))
        self.wfile.flush()

    def _sse_head(self):
        self._send("HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Cache-Control: no-cache\r\n"
                   "Connection: close\r\n\r\n")

    def handle(self):
        try:
            path, headers, body = self._read_request()
        except Exception:
            return
        if path is None:
            return
        log_request(path, headers, body)
        base = path.split("?")[0]

        if base == "/drop":
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            self.connection.close()
            return

        if base == "/slow":
            # 什么都不回,睡到客户端自己超时
            time.sleep(90)
            return

        if base == "/err500":
            b = json.dumps({"error": {"message": "internal server error",
                                      "type": "server_error"}})
            self._send("HTTP/1.1 500 Internal Server Error\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(b), b))
            return

        if base == "/err401":
            b = json.dumps({"error": {"message": "Authentication Fails",
                                      "type": "authentication_error"}})
            self._send("HTTP/1.1 401 Unauthorized\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(b), b))
            return

        if base == "/badjson":
            self._sse_head()
            self._send("data: {\"choices\":[{\"delta\":{\"content\":\n\n")
            self._send("data: {not json at all,,,}\n\n")
            self._send("data: [DONE]\n\n")
            return

        if base == "/notjson":
            b = "<html><body>totally not json</body></html>"
            self._send("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                       "Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(b), b))
            return

        if base == "/practice":
            self._sse_head()
            for f in sse_frames(PRACTICE_CHUNKS, reasoning=["先看数据范围。"]):
                self._send(f)
                time.sleep(0.02)
            return

        # 默认:正常的写代码模式补全
        self._sse_head()
        for f in sse_frames(CODE_CHUNKS):
            self._send(f)
            time.sleep(0.02)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    with Server(("127.0.0.1", PORT), Handler) as srv:
        print("fake-ai listening on 127.0.0.1:%d" % PORT, flush=True)
        srv.serve_forever()

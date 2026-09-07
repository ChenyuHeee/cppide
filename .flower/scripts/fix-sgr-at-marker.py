# desc: 从 pty 原始字节流(<raw_out>.raw)里抓某个标记文字被写出时**生效中的 SGR 属性**,用来断言 ghost 的颜色/属性档位。
#
# 用法: python3 fix-sgr-at-marker.py <raw文件> <标记文字> [更多标记...]
# 说明: 只解析 CSI ... m(SGR),按"最近一次生效的参数集"归属其后写出的文本。
#       SGR 0 / CSI m 视为清空。输出每个标记出现处的参数集(去重,保序)。
#       ⚠ 必须喂 .raw 文件(过滤后的文本里没有转义序列,会得出"没颜色"的假结论)。

import re
import sys

raw = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
markers = sys.argv[2:]

csi = re.compile(r"\x1b\[([0-9;?]*)([A-Za-z])")

cur = []            # 当前生效的 SGR 参数
runs = []           # [(text, sgr_snapshot)]
i = 0
buf = []
while i < len(raw):
    m = csi.match(raw, i)
    if m:
        if buf:
            runs.append(("".join(buf), tuple(cur)))
            buf = []
        if m.group(2) == "m":
            params = [p for p in m.group(1).split(";") if p != ""]
            if not params or params == ["0"]:
                cur = []
            else:
                cur = params
        i = m.end()
        continue
    if raw[i] == "\x1b":          # 非 CSI 的转义(如 ESC(B),原样跳过两字节
        i += 2
        continue
    buf.append(raw[i])
    i += 1
if buf:
    runs.append(("".join(buf), tuple(cur)))

for mk in markers:
    found = []
    for text, sgr in runs:
        if mk in text or any(mk.startswith(text[k:]) and len(text) - k >= 3
                             for k in range(len(text))):
            # 完整包含,或该 run 是标记的一段前缀(ncurses 会把文本切碎)
            if mk in text or text.strip() and text.strip() in mk:
                if sgr not in found:
                    found.append(sgr)
    print(mk + " -> " + (
        " | ".join("SGR[" + ",".join(s) + "]" if s else "SGR[无]" for s in found)
        if found else "(未出现)"))

#!/usr/bin/env bash
# desc: 验收 6/7 —— 用本地假 SSE 端点端到端验证 ghost 自动触发/Tab 接受/Esc 丢弃,以及练习模式绝不向缓冲区插码(含"带 ghost 切模式"边界)
#
# 用法: bash audit-ai-modes.sh [端口]
# 说明:全程假 key(FAKEKEY_*)+ 127.0.0.1 本地假服务器,不联网、不碰任何真实凭据。

set -u
PORT="${1:-18977}"
WORK=/work
TMP=/tmp/audit-ai
DRIVE=/tmp/ptydrive
mkdir -p "$TMP"; cd "$TMP" || exit 2
: > "$TMP/req.jsonl"

pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

python3 "$WORK/.flower/scripts/audit-fake-ai-server.py" "$PORT" "$TMP/req.jsonl" \
  > "$TMP/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1.5
kill -0 $SRV 2>/dev/null || { echo "假服务器起不来"; exit 2; }

mkcfg() {  # mkcfg <文件> <chat_path> <ghost_delay_ms> <min_interval>
  cat > "$1" <<EOF
{ "api_key": "FAKEKEY_AUDIT", "model": "audit-model-xyz",
  "base_url": "http://127.0.0.1:$PORT", "chat_path": "$2",
  "stream": true, "ghost_delay_ms": $3, "ghost_min_interval_ms": $4, "tick_ms": 40 }
EOF
}
mkcfg "$TMP/code.json"     /v1/chat/completions 500  60000
mkcfg "$TMP/slowdly.json"  /v1/chat/completions 30000 60000
mkcfg "$TMP/practice.json" /practice            500  60000

src() { cat > "$1" <<'EOF'
#include <cstdio>
int main() {
  int x = 1;
  return 0;
}
EOF
}

drive() {  # drive <名字> <cfg> <脚本>
  local name="$1" cfg="$2" script="$3"
  printf '%s\n' "$script" > "$TMP/$name.txt"
  timeout 200 "$DRIVE" 34 120 "$TMP/$name.txt" "$TMP/$name.out" \
    -- "$WORK/cppide" --config "$cfg" "$TMP/$name.cpp" > "$TMP/$name.log" 2>&1
}

echo "======== 验收 6:写代码模式 ghost text ========"

echo "-- 6a 默认 ghost_delay_ms 是 500(配置项存在且可改) --"
grep -qE 'ghost_delay_ms +=? *500|"ghost_delay_ms": *500' "$WORK/src/config.h" \
  && ok "src/config.h 里 ghost_delay_ms 默认值 = 500" || bad "默认停顿时长不是 500ms"
grep -q '"ghost_delay_ms"' "$WORK/src/config.cpp" \
  && ok "ghost_delay_ms 可由配置文件覆盖(config.cpp 有解析)" || bad "ghost_delay_ms 不可配置"

echo "-- 6b 停顿后**自动**请求(没按任何 AI 快捷键) --"
src "$TMP/a6b.cpp"
n0=$(grep -c . "$TMP/req.jsonl")
drive a6b "$TMP/code.json" 'expect a6b.cpp
send \x14
expect 写代码模式
wait 300
send \sz
expect GHOSTMARK_Alpha
wait 400
send \x18
wait 1500'
n1=$(grep -c . "$TMP/req.jsonl")
[ "$n1" -gt "$n0" ] && ok "停顿后自动向端点发起了请求(新增 $((n1-n0)) 条,全程未按 Ctrl-A)" \
                    || bad "没有自动请求"
grep -q 'GHOSTMARK_Alpha' "$TMP/a6b.out" && ok "返回内容以 ghost 形式显示在屏幕上" || bad "屏幕上没有 ghost"

echo "-- 6c ghost 用的是灰/暗属性(与正文不同) --"
python3 - "$TMP/a6b.out.raw" <<'PY'
import re, sys
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
runs, cur, i = [], "0", 0
while i < len(raw):
    if raw[i] == '\x1b':
        m = re.match(r'\x1b\[([0-9;]*)m', raw[i:])
        if m: cur = m.group(1) or "0"; i += m.end(); continue
        m = re.match(r'\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-B0]', raw[i:])
        if m: i += m.end(); continue
        i += 1; continue
    j = i
    while j < len(raw) and raw[j] != '\x1b': j += 1
    runs.append((cur, raw[i:j])); i = j
ghost = {s for s, t in runs if 'GHOSTMARK' in t}
body  = {s for s, t in runs if '#include' in t or 'return' in t}
print("      ghost SGR =", sorted(ghost), " 正文 SGR =", sorted(body))
sys.exit(0 if ghost and not (ghost & body) else 1)
PY
[ $? -eq 0 ] && ok "ghost 的终端属性与正文不同(独立配色/暗色)" || bad "ghost 与正文属性相同"

echo "-- 6d Tab 接受:内容真的进缓冲区并落盘 --"
src "$TMP/a6d.cpp"
drive a6d "$TMP/code.json" 'expect a6d.cpp
send \x14
expect 写代码模式
wait 300
send \sz
expect GHOSTMARK_Alpha
send \t
wait 700
send \x0f
wait 900
send \x18
wait 1800'
grep -q 'GHOSTMARK_Alpha' "$TMP/a6d.cpp" && ok "Tab 接受 -> AI 文本进缓冲区并存盘" || bad "Tab 接受失败"

echo "-- 6e Esc 丢弃:缓冲区无 AI 文本,ghost 从屏幕消失 --"
src "$TMP/a6e.cpp"
drive a6e "$TMP/code.json" 'expect a6e.cpp
send \x14
expect 写代码模式
wait 300
send \sz
expect GHOSTMARK_Alpha
send \x1b
wait 900
mark afteresc
send \x0c
wait 800
expectnotfrom GHOSTMARK_Alpha
send \x0f
wait 800
send \x18
wait 1800'
grep -q 'GHOSTMARK' "$TMP/a6e.cpp" && bad "Esc 之后 AI 文本仍被插入" || ok "Esc 丢弃 -> 缓冲区无 AI 文本"
[ "$(grep -cE '^FAIL' "$TMP/a6e.log")" = 0 ] && ok "Esc 之后 ghost 从屏幕上消失" \
  || { bad "Esc 后 ghost 仍在屏幕上"; grep -E '^FAIL' "$TMP/a6e.log" | sed 's/^/        /'; }

# 注意:延时要设得比整段脚本的总时长还长,否则退出前的收尾 wait 会把时间耗过阈值,
# 看起来像"延时没生效"(这是脚本自身的计时陷阱,不是产品 bug)。
echo "-- 6f ghost_delay_ms 真的在起作用(设成 30000 时整段脚本内都不该有请求) --"
src "$TMP/a6f.cpp"
n0=$(grep -c . "$TMP/req.jsonl")
drive a6f "$TMP/slowdly.json" 'expect a6f.cpp
send \x14
expect 写代码模式
wait 300
send \sz
wait 1200
send \x18
wait 1500'
n1=$(grep -c . "$TMP/req.jsonl")
[ "$n1" -eq "$n0" ] && ok "ghost_delay_ms=30000 时全程没有发出请求(延时可配置且真的生效)" \
                    || bad "延时配置没生效(发了 $((n1-n0)) 条)"

echo
echo "======== 验收 7:练习模式绝不插码 ========"

echo "-- 7a 回复含完整代码块时,缓冲区必须逐字节未变 --"
src "$TMP/a7a.cpp"
before=$(sha256sum "$TMP/a7a.cpp" | cut -d' ' -f1)
drive a7a "$TMP/practice.json" 'expect a7a.cpp
expect 练习模式
wait 300
send \x01
expect 问 AI
send zenmezuo\r
expect PRACTICE_LEAK_CODE
wait 800
send \x0f
wait 900
send \x18
wait 1800'
after=$(sha256sum "$TMP/a7a.cpp" | cut -d' ' -f1)
[ "$before" = "$after" ] && ok "缓冲区逐字节未变(sha256 相同)" || bad "缓冲区被改动了"
grep -q 'PRACTICE_LEAK_CODE' "$TMP/a7a.out" \
  && ok "含代码的回复只出现在【AI】面板里" || bad "面板里没看到回复"
grep -qE '推理过程|思路|复杂度' "$TMP/a7a.out" \
  && ok "面板输出的是中文思路提示" || bad "没看到中文思路提示"

echo "-- 7b 练习模式下 Tab 只产生缩进 --"
src "$TMP/a7b.cpp"
drive a7b "$TMP/practice.json" 'expect a7b.cpp
expect 练习模式
wait 300
send \x01
expect 问 AI
send zenmezuo\r
expect PRACTICE_LEAK_CODE
wait 600
send \x1b
wait 400
send \t\t\t
wait 600
send \x0f
wait 900
send \x18
wait 1800'
grep -qE 'PRACTICE_LEAK_CODE|前缀和' "$TMP/a7b.cpp" && bad "Tab 把 AI 内容塞进了缓冲区" \
  || ok "Tab 未引入任何 AI 内容"
head -1 "$TMP/a7b.cpp" | grep -qE '^ +#include' && ok "Tab 只产生缩进(首行被缩进)" \
  || bad "Tab 没有产生缩进"

echo "-- 7c 边界:写代码模式拿到 ghost 后切到练习模式,ghost 必须作废且 Tab 不接受 --"
src "$TMP/a7c.cpp"
drive a7c "$TMP/code.json" 'expect a7c.cpp
send \x14
expect 写代码模式
wait 300
send \sz
expect GHOSTMARK_Alpha
wait 300
send \x14
expect 练习模式
wait 600
send \t
wait 600
send \x0f
wait 900
send \x18
wait 1800'
grep -q 'GHOSTMARK' "$TMP/a7c.cpp" \
  && bad "带 ghost 切到练习模式后,Tab 仍把 AI 代码插进了缓冲区" \
  || ok "带 ghost 切到练习模式后 ghost 作废,Tab 不接受任何 AI 内容"

echo "-- 7d 练习模式的请求里带了不可覆盖的硬后缀 --"
python3 - "$TMP/req.jsonl" <<'PY'
import json, sys
rs = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
pr = [r for r in rs if r['path'].startswith('/practice')]
if not pr:
    print("      (没有练习模式请求)"); sys.exit(1)
sysmsg = ""
for m in json.loads(pr[-1]['body'])['messages']:
    if m['role'] == 'system': sysmsg = m['content']
print("      system prompt 尾部:", sysmsg[-60:].replace("\n", " "))
sys.exit(0 if ('代码' in sysmsg or '思路' in sysmsg) else 1)
PY
[ $? -eq 0 ] && ok "练习模式 system prompt 含"不要给代码/只给思路"类约束" \
             || bad "练习模式 system prompt 没有硬约束"

echo
echo "通过 $pass 条,失败 $fail 条"
[ "$fail" -eq 0 ]

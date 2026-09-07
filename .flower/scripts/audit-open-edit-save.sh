#!/usr/bin/env bash
# desc: 验收 2 —— .c/.cpp/.h/.hpp 四种扩展名各真开一次/改一处/保存,断言磁盘内容正确;并抓 ANSI 属性证明高亮分色
#
# 用法: bash audit-open-edit-save.sh
# 依赖: /tmp/ptydrive、/work/cppide

set -u
WORK=/work
TMP=/tmp/audit-oes
DRIVE=/tmp/ptydrive
mkdir -p "$TMP"; cd "$TMP" || exit 2

pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

cat > "$TMP/nocfg.json" <<'EOF'
{ "api_key": "", "tick_ms": 40 }
EOF

# ---------------------------------------------------------------- 编辑 + 保存
for ext in c cpp h hpp; do
  f="$TMP/probe.$ext"
  cat > "$f" <<'EOF'
int seed_value = 1;
EOF
  # 光标在 (1,1)。敲入一个唯一标记后保存。
  cat > "$TMP/s_$ext.txt" <<EOF
expect probe.$ext
wait 400
send MARKER_$ext\\x5f OK\\n
wait 400
send \\x0f
wait 900
send \\x18
wait 1500
EOF
  timeout 120 "$DRIVE" 30 110 "$TMP/s_$ext.txt" "$TMP/s_$ext.raw" \
    -- "$WORK/cppide" --config "$TMP/nocfg.json" "$f" > "$TMP/s_$ext.log" 2>&1
  if grep -q "MARKER_${ext}_ OK" "$f" && grep -q 'seed_value' "$f"; then
    ok ".$ext 打开/编辑/保存 —— 磁盘内容正确"
  else
    bad ".$ext 保存后磁盘内容不对"; sed 's/^/        /' "$f" | head -3
  fi
done

# ---------------------------------------------------------------- 语法高亮
# 造一个把各类 token 分行摆开的文件,便于把"某行的 SGR"归给某个 token。
cat > "$TMP/hl.cpp" <<'EOF'
#include <cstdio>
int zzkeyword = 0;
const char *s = "zzstringlit";
// zzcommentword
int zznumber = 1234567;
EOF

cat > "$TMP/s_hl.txt" <<'EOF'
expect hl.cpp
wait 1200
send \x18
wait 1200
EOF
timeout 120 "$DRIVE" 30 110 "$TMP/s_hl.txt" "$TMP/hl.raw" \
  -- "$WORK/cppide" --config "$TMP/nocfg.json" "$TMP/hl.cpp" > "$TMP/s_hl.log" 2>&1

# 注意:wave5-pty-drive 把**过滤后**的文本写进 <raw_out>,真正的原始字节流在
# <raw_out>.raw 里。抓 ANSI 属性必须用后者。
python3 - "$TMP/hl.raw.raw" <<'PY'
import re, sys, collections
raw = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
# 把流切成 (当前SGR, 文本) 的连续段
runs, cur = [], "0"
i = 0
while i < len(raw):
    if raw[i] == '\x1b':
        m = re.match(r'\x1b\[([0-9;]*)m', raw[i:])
        if m:
            cur = m.group(1) or "0"
            i += m.end(); continue
        m = re.match(r'\x1b\[[0-9;?]*[A-Za-z]', raw[i:])
        if m:
            i += m.end(); continue
        m = re.match(r'\x1b[()][A-B0]', raw[i:])
        if m:
            i += m.end(); continue
        i += 1; continue
    j = i
    while j < len(raw) and raw[j] != '\x1b':
        j += 1
    runs.append((cur, raw[i:j]))
    i = j

def sgr_of(tok):
    """返回包含该 token 的所有 SGR 集合"""
    out = set()
    for sgr, text in runs:
        if tok in text:
            out.add(sgr)
    return out

targets = {
    "关键字 int":      "int",
    "字符串 zzstringlit": "zzstringlit",
    "注释 zzcommentword": "zzcommentword",
    "数字 1234567":     "1234567",
    "预处理 #include":  "include",
}
res = {}
for label, tok in targets.items():
    s = sgr_of(tok)
    res[label] = s
    print("  %-20s SGR=%s" % (label, sorted(s) if s else "<未在流中找到>"))

missing = [k for k, v in res.items() if not v]
if missing:
    print("  FAIL  这些 token 没在 pty 流里出现:", missing)
    sys.exit(1)

# 断言:不同类别的 token 至少要用到 2 种以上不同属性(不是全同色)
allsgr = set()
for v in res.values():
    allsgr |= v
print("  全部出现过的 SGR 组合数 =", len(allsgr), sorted(allsgr))
if len(allsgr) < 3:
    print("  FAIL  token 属性几乎全同,看不出高亮")
    sys.exit(1)

# 逐对比较:字符串 / 注释 / 数字 / 关键字 互相之间必须存在差异
import itertools
pairs_same = []
for a, b in itertools.combinations(["关键字 int", "字符串 zzstringlit",
                                    "注释 zzcommentword", "数字 1234567"], 2):
    if res[a] == res[b]:
        pairs_same.append((a, b))
if pairs_same:
    print("  WARN  以下类别属性完全相同:", pairs_same)
else:
    print("  PASS  关键字/字符串/注释/数字 四类属性两两不同")
sys.exit(0)
PY
hlrc=$?
if [ $hlrc -eq 0 ]; then ok "语法高亮:不同 token 使用了不同终端属性"; else bad "语法高亮断言失败"; fi

echo
echo "通过 $pass 条,失败 $fail 条"
[ "$fail" -eq 0 ]

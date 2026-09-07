#!/usr/bin/env bash
# desc: 验收 10 —— 逐条核对 README 的编译步骤/配置路径/字段表,并在 pty 里真按 14 个快捷键断言行为与 README 描述相符
#
# 用法: bash audit-readme-keys.sh
# 依赖: /tmp/ptydrive、/work/cppide

set -u
WORK=/work
TMP=/tmp/audit-rk
DRIVE=/tmp/ptydrive
mkdir -p "$TMP"; cd "$TMP" || exit 2

pass=0; fail=0
ok()  { echo "  PASS  $*"; pass=$((pass+1)); }
bad() { echo "  FAIL  $*"; fail=$((fail+1)); }

cat > "$TMP/cfg.json" <<'EOF'
{ "api_key": "", "tick_ms": 40, "run_timeout_ms": 8000, "compile_timeout_ms": 40000 }
EOF

base() { cat > "$1" <<'EOF'
#include <cstdio>
int main() {
  printf("RUNMARK_OK\n");
  return 5;
}
EOF
}

# drive <名字> <源文件> <脚本内容>  -> 产出 $TMP/<名字>.out
drive() {
  local name="$1" src="$2"; shift 2
  printf '%s\n' "$1" > "$TMP/$name.txt"
  timeout 200 "$DRIVE" 34 120 "$TMP/$name.txt" "$TMP/$name.out" \
    -- "$WORK/cppide" --config "$TMP/cfg.json" "$src" > "$TMP/$name.log" 2>&1
  return $?
}

echo "== 10.1 README 的编译步骤与 make 目标 =="
for t in all check-headers tests debug clean install; do
  if grep -qE "^$t:" "$WORK/Makefile"; then ok "Makefile 有 README 提到的目标 '$t'"
  else bad "Makefile 缺少目标 '$t'"; fi
done

echo "== 10.2 README 的默认配置路径与程序实际一致 =="
readme_path=$(grep -oE '\$HOME/\.config/cppide/config\.json|~/\.config/cppide/config\.json' "$WORK/README.md" | head -1)
prog_path=$(timeout 30 "$WORK/cppide" --help 2>&1 | grep -oE '/[^ ]*/\.config/cppide/config\.json' | head -1)
if [ -n "$readme_path" ] && [ -n "$prog_path" ]; then
  ok "README 写的默认路径($readme_path)与程序输出($prog_path)同构"
else
  bad "默认配置路径对不上(README='$readme_path' 程序='$prog_path')"
fi

echo "== 10.3 README 字段表 vs 程序认识的字段 =="
timeout 30 "$WORK/cppide" --print-config > "$TMP/pc.json" 2>/dev/null
miss=0
for k in $(python3 -c "
import json,sys
o=json.load(open('$TMP/pc.json'))
print(' '.join(k for k in o if not k.startswith('_')))
"); do
  grep -q "\`$k\`" "$WORK/README.md" || { echo "        README 字段表缺: $k"; miss=$((miss+1)); }
done
[ "$miss" = 0 ] && ok "--print-config 输出的每个字段 README 都有记载" \
                || bad "README 字段表漏了 $miss 个字段"
# 反向:README 写了但程序不认的字段
# 只取 §2.3「字段全表」那一节,否则会把 §1.3 的 make 目标表(| `make` | …)也算进来。
extra=0
for k in $(awk '/^### 2\.3/,/^### 2\.4/' "$WORK/README.md" \
             | grep -oE '^\| `[a-z_]+` \|' | tr -d '|` '); do
  python3 -c "
import json,sys
o=json.load(open('$TMP/pc.json'))
sys.exit(0 if '$k' in o else 1)
" || { echo "        README 写了但 --print-config 里没有: $k"; extra=$((extra+1)); }
done
[ "$extra" = 0 ] && ok "README 字段表里没有程序不认识的字段" \
                 || bad "README 有 $extra 个字段程序不认识"

echo "== 10.4 真按快捷键(14 个)=="

# (1) Ctrl-O 保存
base "$TMP/k1.cpp"
drive k1 "$TMP/k1.cpp" 'expect k1.cpp
wait 400
send // ZZSAVE\n
wait 300
send \x0f
wait 900
send \x18
wait 1500'
grep -q 'ZZSAVE' "$TMP/k1.cpp" && ok "Ctrl-O 保存(README 4.3)" || bad "Ctrl-O 没保存"

# (2) Ctrl-X 退出(无修改时直接退出,进程正常结束)
base "$TMP/k2.cpp"
drive k2 "$TMP/k2.cpp" 'expect k2.cpp
wait 500
send \x18
wait 2000'
grep -q '退出码 = 0' "$TMP/k2.log" && ok "Ctrl-X 退出(README 4.3)" || bad "Ctrl-X 没退出"

# (3) Ctrl-X 有未保存修改时先问 y/n/s,且 Enter 不等于确认(README 4.3 明确承诺)
#     "Enter 不等于确认" 走**文件系统**断言:Enter 之后编辑器若还活着,
#     后续敲的标记就能被 Ctrl-O 存进磁盘。不能靠 expect 屏幕上的连续字符串 ——
#     ncurses 逐字符重绘,过滤后的文本里字符之间会被插入空格,永远匹配不上。
base "$TMP/k3.cpp"
drive k3 "$TMP/k3.cpp" 'expect k3.cpp
wait 500
send ZZD
wait 400
send \x18
wait 900
send \r
wait 1200
send n
wait 800
send ZZALIVEMARK
wait 600
send \x0f
wait 1000
send \x18
wait 800
send y
wait 2000'
if grep -qE '未保存|y=|退出\?|放弃' "$TMP/k3.out"; then
  ok "Ctrl-X 有未保存修改时给出 y/n/s 确认(README 4.3)"
else
  bad "Ctrl-X 没有确认提示"
fi
grep -q 'ZZALIVEMARK' "$TMP/k3.cpp" \
  && ok "确认框里 Enter 不等于确认,编辑器仍存活(README 4.3:默认不退出)" \
  || bad "Enter 似乎直接退出了(与 README 4.3 矛盾)"

# (4) Ctrl-B 编译 + 脏缓冲区先自动保存(README 4.3)
base "$TMP/k4.cpp"
drive k4 "$TMP/k4.cpp" 'expect k4.cpp
wait 400
send // ZZAUTOSAVE\n
wait 300
send \x02
wait 4000
send \x18
wait 2000'
grep -q 'ZZAUTOSAVE' "$TMP/k4.cpp" \
  && ok "Ctrl-B 编译前自动保存脏缓冲区(README 4.3)" || bad "Ctrl-B 没有自动保存"
grep -qE '编译|成功' "$TMP/k4.out" && ok "Ctrl-B 触发编译且面板有反馈" || bad "Ctrl-B 无编译反馈"

# (5) Ctrl-R 运行
base "$TMP/k5.cpp"
drive k5 "$TMP/k5.cpp" 'expect k5.cpp
wait 400
send \x12
expect RUNMARK_OK
wait 800
send \x18
wait 2000'
grep -q 'RUNMARK_OK' "$TMP/k5.out" && ok "Ctrl-R 运行(README 4.3)" || bad "Ctrl-R 没跑起来"
grep -qE '退出码 *[:：]? *5' "$TMP/k5.out" && ok "Ctrl-R 显示退出码 5" || bad "Ctrl-R 没显示退出码"

# (6) Ctrl-E 编译并运行
base "$TMP/k6.cpp"
drive k6 "$TMP/k6.cpp" 'expect k6.cpp
wait 400
send \x05
expect RUNMARK_OK
wait 800
send \x18
wait 2000'
grep -q 'RUNMARK_OK' "$TMP/k6.out" && ok "Ctrl-E 编译并运行(README 4.3)" || bad "Ctrl-E 失败"

# (7) Ctrl-T 切模式
base "$TMP/k7.cpp"
drive k7 "$TMP/k7.cpp" 'expect k7.cpp
expect 练习模式
mark a
send \x14
wait 700
expectfrom 写代码模式
wait 300
send \x18
wait 1500'
[ "$(grep -cE '^FAIL' "$TMP/k7.log")" = 0 ] && ok "Ctrl-T 切换 AI 模式(README 4.4)" || bad "Ctrl-T 无效"

# (8) Ctrl-G 跳到行号(跳到第 3 行后敲标记,存盘验证落点)
base "$TMP/k8.cpp"
drive k8 "$TMP/k8.cpp" 'expect k8.cpp
wait 400
send \x07
wait 600
send 3\r
wait 600
send ZZGOTO
wait 300
send \x0f
wait 900
send \x18
wait 1500'
land=$(grep -n 'ZZGOTO' "$TMP/k8.cpp" | head -1 | cut -d: -f1)
[ "$land" = "3" ] && ok "Ctrl-G 跳到行号(README 4.5,落在第 3 行)" \
                  || bad "Ctrl-G 没跳到第 3 行(落在第 ${land:-无} 行)"

# (9) Ctrl-F 查找(打开输入行)
base "$TMP/k9.cpp"
drive k9 "$TMP/k9.cpp" 'expect k9.cpp
wait 400
send \x06
wait 800
send \x18
wait 1500'
grep -qE '查找' "$TMP/k9.out" && ok "Ctrl-F 打开查找输入行(README 4.5)" || bad "Ctrl-F 无反应"

# (10) F1 帮助浮层
base "$TMP/k10.cpp"
drive k10 "$TMP/k10.cpp" 'expect k10.cpp
wait 500
send \eOP
wait 1000
send \x1b
wait 500
send \x18
wait 1500'
grep -qE '快捷键|帮助' "$TMP/k10.out" && ok "F1 打开帮助浮层(README 4.5)" || bad "F1 没打开帮助"

# (11) Ctrl-U 撤销 / (12) Ctrl-W 重做
base "$TMP/k11.cpp"
drive k11 "$TMP/k11.cpp" 'expect k11.cpp
wait 400
send ZZUNDOME
wait 500
send \x15
wait 700
send \x0f
wait 900
send \x18
wait 1800'
grep -q 'ZZUNDOME' "$TMP/k11.cpp" && bad "Ctrl-U 撤销无效(文字还在)" || ok "Ctrl-U 撤销(README 4.2)"

base "$TMP/k12.cpp"
drive k12 "$TMP/k12.cpp" 'expect k12.cpp
wait 400
send ZZREDOME
wait 500
send \x15
wait 600
send \x17
wait 700
send \x0f
wait 900
send \x18
wait 1800'
grep -q 'ZZREDOME' "$TMP/k12.cpp" && ok "Ctrl-W 重做(README 4.2)" || bad "Ctrl-W 重做无效"

# (13) Ctrl-K 剪切当前行
base "$TMP/k13.cpp"
drive k13 "$TMP/k13.cpp" 'expect k13.cpp
wait 400
send \x0b
wait 600
send \x0f
wait 900
send \x18
wait 1800'
head -1 "$TMP/k13.cpp" | grep -q '#include' \
  && bad "Ctrl-K 没剪掉第一行" || ok "Ctrl-K 剪切当前行(README 4.2)"

# (14) Ctrl-D 复制当前行 + Ctrl-V 粘贴行
base "$TMP/k14.cpp"
drive k14 "$TMP/k14.cpp" 'expect k14.cpp
wait 400
send \x04
wait 500
send \x16
wait 700
send \x0f
wait 900
send \x18
wait 1800'
n=$(grep -c '#include <cstdio>' "$TMP/k14.cpp")
[ "$n" -ge 2 ] && ok "Ctrl-D 复制 + Ctrl-V 粘贴行(README 4.2,出现 $n 次)" \
               || bad "Ctrl-D/Ctrl-V 没有复制出第二行(出现 $n 次)"

# (15) Alt-1..4 聚焦四个面板
base "$TMP/k15.cpp"
drive k15 "$TMP/k15.cpp" 'expect k15.cpp
wait 400
send \x1b1
wait 400
send \x1b2
wait 400
send \x1b3
wait 400
send \x1b4
expect 输入
wait 600
send \x1b
wait 400
send \x18
wait 1500'
grep -qE '输入' "$TMP/k15.out" && ok "Alt-1/2/3/4 聚焦面板(README 4.6)" || bad "Alt-n 聚焦失败"

# (16) Alt-0 折叠 / 展开面板区
base "$TMP/k16.cpp"
drive k16 "$TMP/k16.cpp" 'expect k16.cpp
expect [编译]
wait 400
send \x1b0
wait 800
mark folded
send \x0c
wait 800
expectnotfrom [编译]
wait 300
send \x18
wait 1500'
[ "$(grep -cE '^FAIL' "$TMP/k16.log")" = 0 ] \
  && ok "Alt-0 折叠面板区(README 4.6)" \
  || { bad "Alt-0 折叠无效"; grep -E '^FAIL' "$TMP/k16.log" | sed 's/^/        /'; }

echo
echo "通过 $pass 条,失败 $fail 条"
[ "$fail" -eq 0 ]

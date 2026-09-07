#!/bin/sh
# desc: Wave4 app/main 全量验收 —— 非交互命令 + pty 端到端冒烟(可选 ASan/UBSan)
# 用法: wave4-app-verify.sh [--asan]
#   不带参数:用 /work/cppide(release);--asan:把全部 .cpp 编到 /tmp/asan-obj 再链 /tmp/cppide-asan
# 产出:/tmp/out-smoke.txt /tmp/out-loop.txt(过滤后的 pty 屏幕文本)+ .raw(原始字节)
set -u
cd /work

BIN=/work/cppide
ASAN=0
[ "${1:-}" = "--asan" ] && ASAN=1

DRV=/tmp/pty-drive
g++ -std=c++17 -O1 -Wall -Wextra -o "$DRV" /work/.flower/scripts/wave4-pty-drive.cpp -lutil

if [ "$ASAN" = 1 ]; then
  # 刻意不用 `make debug`:那会 clean 掉共享的 src/*.o,干扰并行 agent。
  mkdir -p /tmp/asan-obj
  SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
  for f in src/*.cpp; do
    o=/tmp/asan-obj/$(basename "$f" .cpp).o
    [ -f "$o" ] && [ "$o" -nt "$f" ] || \
      g++ -Isrc -std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter $SAN -c -o "$o" "$f"
  done
  g++ $SAN -o /tmp/cppide-asan /tmp/asan-obj/*.o -lncursesw -lcurl -lpthread
  BIN=/tmp/cppide-asan
fi
echo "== 被测二进制: $BIN =="

fail=0
ok()   { echo "PASS  $*"; }
bad()  { echo "FAIL  $*"; fail=$((fail+1)); }
code() { timeout 30 "$BIN" "$@" >/tmp/v.out 2>/tmp/v.err </dev/null; echo $?; }

# ---- 1) 非交互命令:退出码 0 + 有输出 ----
for opt in --help --version --print-config --doctor; do
  c=$(code $opt)
  s=$(wc -c </tmp/v.out)
  if [ "$c" = 0 ] && [ "$s" -gt 10 ]; then ok "$opt 退出码 0,输出 $s 字节"; else bad "$opt 退出码=$c 输出=$s 字节"; fi
done
timeout 30 "$BIN" --print-config >/tmp/v.json </dev/null
python3 -c "import json,sys;json.load(open('/tmp/v.json'))" && ok "--print-config 是合法 JSON" || bad "--print-config 不是合法 JSON"

# ---- 1b) 假 key 不得出现在 --doctor / --print-config 输出里 ----
FAKE=sk-DEADBEEFdeadbeef1234567890abcdefFAKEKEY
printf '{ "api_key": "%s" }\n' "$FAKE" > /tmp/fakecfg.json
timeout 30 "$BIN" --config /tmp/fakecfg.json --doctor >/tmp/v.doctor </dev/null
grep -q "$FAKE" /tmp/v.doctor && bad "--doctor 泄露了 api_key" || ok "--doctor 不含 api_key 值"
grep -q "已配置(已隐去)" /tmp/v.doctor && ok "--doctor 有 key 时显示 已配置(已隐去)" || bad "--doctor 未显示隐去提示"
timeout 30 "$BIN" --config /tmp/fakecfg.json --print-config >/tmp/v.pc </dev/null
grep -q "$FAKE" /tmp/v.pc && bad "--print-config 泄露了 api_key" || ok "--print-config 不含 api_key 值"

# ---- 2) 配置缺失 ----
c=$(code --config /nonexistent/x.json --doctor)
if [ "$c" = 0 ] && grep -q "未找到配置文件 /nonexistent/x.json" /tmp/v.out; then
  ok "缺配置时 --doctor 不崩且 warnings 说明缺失"
else bad "缺配置 --doctor 退出码=$c"; fi

# ---- 3) 非法入参:友好报错 + 非 0 退出 + 不崩 ----
check_err() { # <期望关键字> <参数...>
  want=$1; shift
  c=$(code "$@")
  if [ "$c" != 0 ] && [ "$c" -lt 100 ] && grep -q "$want" /tmp/v.err; then
    ok "[$*] 退出码 $c + 中文报错"
  else bad "[$*] 退出码=$c err=$(head -c 120 /tmp/v.err)"; fi
}
check_err "找不到文件"   /tmp/definitely-missing-file.cpp
check_err "是一个目录"   /tmp
check_err "未知选项"     --bogus
check_err "缺少参数"     --config
check_err "只能打开一个" a.cpp b.cpp

# ---- 4) 无 tty ----
printf 'int main(){}\n' > /tmp/t_exists.cpp
c=$(code /tmp/t_exists.cpp)
if [ "$c" != 0 ] && [ "$c" -lt 100 ] && grep -q "不是交互式终端" /tmp/v.err; then
  ok "无 tty 时清晰报错并退出(码 $c)"
else bad "无 tty 行为不对:退出码=$c"; fi

# ---- 5) pty 端到端冒烟 ----
cat > /tmp/t.cpp <<'EOF'
#include <iostream>
int main() {
    int y = zzz_undeclared;
    std::cout << y << "\n";
    return 0;
}
EOF
if timeout 180 "$DRV" 24 80 /work/.flower/scripts/wave4-pty-smoke.txt /tmp/out-smoke.txt -- \
     "$BIN" --config /tmp/nocfg-missing.json /tmp/t.cpp; then
  ok "pty 冒烟(输入中文/保存/编译/跳诊断/切模式/退出)"
else bad "pty 冒烟失败"; fi
head -1 /tmp/t.cpp | grep -q "// 中文注释" && ok "文件确实被保存(首行是中文注释)" || bad "文件没保存对"

# ---- 6) pty 死循环:不挂死 + Esc 终止 ----
cat > /tmp/loop.cpp <<'EOF'
#include <cstdio>
int main() {
    std::printf("start\n");
    std::fflush(stdout);
    volatile long x = 0;
    for (;;) { x = x + 1; }
}
EOF
printf '{ "run_timeout_ms": 60000 }\n' > /tmp/cfg-run.json
if timeout 240 "$DRV" 24 80 /work/.flower/scripts/wave4-pty-loop.txt /tmp/out-loop.txt -- \
     "$BIN" --config /tmp/cfg-run.json /tmp/loop.cpp; then
  ok "pty 死循环(运行中仍响应按键 + Esc 终止 + 正常退出)"
else bad "pty 死循环用例失败"; fi

# ---- 7) pty:【输入】面板喂 stdin + Ctrl-E 编译并运行 + AI 未配置只给面板消息 ----
cat > /tmp/io.cpp <<'EOF'
#include <iostream>
int main() {
    int n;
    if (!(std::cin >> n)) { std::cout << "no input\n"; return 3; }
    std::cout << "n=" << n * 2 << "\n";
    return 0;
}
EOF
if timeout 180 "$DRV" 24 80 /work/.flower/scripts/wave4-pty-stdin.txt /tmp/out-stdin.txt -- \
     "$BIN" --config /tmp/nocfg-missing.json /tmp/io.cpp; then
  ok "pty stdin(【输入】面板 21 -> n=42 + 退出码 0 + Ctrl-A 面板消息)"
else bad "pty stdin 用例失败"; fi

# ---- 8) pty:提示行 / 撤销 / 帮助浮层 / 面板折叠 ----
if timeout 180 "$DRV" 24 80 /work/.flower/scripts/wave4-pty-edit.txt /tmp/out-edit.txt -- \
     "$BIN" --config /tmp/nocfg-missing.json /tmp/t.cpp; then
  ok "pty 交互(Ctrl-G / Ctrl-F / Ctrl-U / F1 / Alt-0 / Ctrl-L)"
else bad "pty 交互用例失败"; fi

echo "================================"
[ "$fail" = 0 ] && echo "全部通过" || echo "有 $fail 项失败"
exit $fail

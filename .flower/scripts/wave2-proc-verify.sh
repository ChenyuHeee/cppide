#!/usr/bin/env bash
# desc: 验证 src/proc.cpp:零 warning 编译 + test_proc 在 -O2 与 ASan/UBSan 下反复跑(TSan 本机不可用)
set -u
cd "$(dirname "$0")/../.." || exit 1
ROUNDS="${1:-20}"
fail() { echo "FAIL: $*"; exit 1; }

echo "== 1) 零 warning 编译门 =="
out=$(g++ -std=c++17 -O2 -Wall -Wextra -c src/proc.cpp -o /tmp/proc_warn.o 2>&1) || fail "编译失败: $out"
[ -z "$out" ] || { echo "$out"; fail "有 warning"; }
echo "OK: g++ -std=c++17 -O2 -Wall -Wextra -c src/proc.cpp 零输出"

echo "== 2) 构建测试(-O2 / ASan+UBSan) =="
g++ -std=c++17 -O2 -Wall -Wextra -o /tmp/test_proc \
    tests/test_proc.cpp src/proc.cpp -lpthread || fail "-O2 构建失败"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o /tmp/test_proc_asan tests/test_proc.cpp src/proc.cpp -lpthread || fail "ASan 构建失败"

echo "== 3) -O2 跑 1 轮(完整输出) =="
( cd /tmp && timeout 150 ./test_proc ) || fail "-O2 单轮失败"

echo "== 4) -O2 连跑 $ROUNDS 轮 =="
for i in $(seq 1 "$ROUNDS"); do
  ( cd /tmp && timeout 150 ./test_proc >/tmp/proc_r.log 2>&1 ) \
    || { tail -25 /tmp/proc_r.log; fail "-O2 第 $i 轮失败"; }
  printf '.'
done; echo " OK"

echo "== 5) ASan+UBSan 连跑 $ROUNDS 轮(TSan 在本机不可用,用它替代) =="
export ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
for i in $(seq 1 "$ROUNDS"); do
  ( cd /tmp && timeout 200 ./test_proc_asan >/tmp/proc_a.log 2>&1 ) \
    || { tail -25 /tmp/proc_a.log; fail "ASan 第 $i 轮失败"; }
  printf '.'
done; echo " OK"

echo "== 6) 僵尸/残留进程巡检 =="
z=$(ps -eo stat= 2>/dev/null | grep -c '^Z' || true)
echo "当前系统僵尸数: $z(孤儿孙子进程被容器 PID1 收养后可能短暂为 Z,与本模块无关)"
echo
echo "全部通过:$ROUNDS 轮 -O2 + $ROUNDS 轮 ASan/UBSan"

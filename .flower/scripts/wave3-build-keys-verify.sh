#!/usr/bin/env bash
# desc: 验证 Wave 3 的 build.cpp + keys.cpp:零 warning 编译 + test_diag/test_keys 在 -O2 与 ASan/UBSan 下反复跑 + make 两个门
set -u
cd "$(dirname "$0")/../.." || exit 1
ROUNDS="${1:-10}"
fail() { echo "FAIL: $*"; exit 1; }

echo "== 1) 零 warning 编译门 =="
for f in src/build.cpp src/keys.cpp; do
  out=$(g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c "$f" -o /tmp/w3_$(basename "$f" .cpp).o 2>&1) \
    || fail "编译失败: $f"
  [ -z "$out" ] || { echo "$out"; fail "$f 有 warning"; }
  echo "OK: $f 零输出"
done

echo "== 2) make check-headers(头文件仍自给自足)=="
make check-headers >/tmp/w3_ch.log 2>&1 || { tail -20 /tmp/w3_ch.log; fail "check-headers"; }
tail -1 /tmp/w3_ch.log

echo "== 3) 构建测试(-O2 / ASan+UBSan) =="
LINK_DIAG="src/build.cpp src/util.cpp src/proc.cpp"
LINK_KEYS="src/keys.cpp src/util.cpp"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc -o /tmp/w3_test_diag tests/test_diag.cpp $LINK_DIAG -lpthread || fail "test_diag -O2"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc -o /tmp/w3_test_keys tests/test_keys.cpp $LINK_KEYS || fail "test_keys -O2"
g++ -std=c++17 -O1 -g -Wall -Wextra -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o /tmp/w3_test_diag_asan tests/test_diag.cpp $LINK_DIAG -lpthread || fail "test_diag ASan"
g++ -std=c++17 -O1 -g -Wall -Wextra -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o /tmp/w3_test_keys_asan tests/test_keys.cpp $LINK_KEYS || fail "test_keys ASan"

echo "== 4) -O2 跑 1 轮(完整输出) =="
( cd /tmp && timeout 240 ./w3_test_diag ) || fail "test_diag -O2"
( cd /tmp && timeout 240 ./w3_test_keys ) || fail "test_keys -O2"

echo "== 5) 连跑 $ROUNDS 轮(-O2 + ASan/UBSan) =="
export ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
for i in $(seq 1 "$ROUNDS"); do
  for t in w3_test_diag w3_test_keys w3_test_diag_asan w3_test_keys_asan; do
    ( cd /tmp && timeout 300 ./$t >/tmp/w3_r.log 2>&1 ) || { tail -25 /tmp/w3_r.log; fail "$t 第 $i 轮"; }
  done
  printf '.'
done; echo " OK"

echo "== 6) make tests(全工程所有测试)=="
timeout 900 make tests >/tmp/w3_tests.log 2>&1 || { tail -30 /tmp/w3_tests.log; fail "make tests"; }
grep -E "全部通过|OK" /tmp/w3_tests.log | tail -12

echo
echo "全部通过:$ROUNDS 轮 x (test_diag + test_keys) x (-O2 / ASan+UBSan)"

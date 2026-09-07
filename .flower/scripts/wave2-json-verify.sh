#!/usr/bin/env bash
# desc: 验证 src/json.cpp + tests/test_json.cpp:零 warning 编译 + 普通/ASan+UBSan 双跑
set -u
cd /work
fail=0
echo "== 1. 验收编译(必须零 warning) =="
out=$(g++ -std=c++17 -O2 -Wall -Wextra -c src/json.cpp -o /tmp/json_acc.o 2>&1)
if [ -n "$out" ]; then echo "$out"; echo "!! 有 warning/error"; fail=1; else echo "OK 零输出"; fi

echo "== 2. 严格 warning 附加检查(-Wpedantic -Wshadow -Wconversion 仅供参考) =="
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -fsyntax-only src/json.cpp 2>&1 | head -20

echo "== 3. -O2 跑测试 =="
g++ -std=c++17 -O2 -Wall -Wextra -o /tmp/tj tests/test_json.cpp src/json.cpp || fail=1
/tmp/tj || fail=1

echo "== 4. ASan + UBSan + LSan 跑测试 =="
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -fno-omit-frame-pointer -o /tmp/tj_san tests/test_json.cpp src/json.cpp || fail=1
ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 /tmp/tj_san || fail=1

echo "== 5. -O0 -g 跑测试(断言全开) =="
g++ -std=c++17 -O0 -g -Wall -Wextra -o /tmp/tj_o0 tests/test_json.cpp src/json.cpp || fail=1
/tmp/tj_o0 || fail=1

[ $fail -eq 0 ] && echo "ALL GREEN" || echo "FAILED"
exit $fail

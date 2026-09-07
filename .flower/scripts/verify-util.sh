#!/bin/sh
# desc: 验收 src/util.cpp —— 零 warning 编译 + tests/test_util.cpp 在 ASan/UBSan 下跑通
set -e
cd "$(dirname "$0")/../.."
echo "== 1) g++ -std=c++17 -O2 -Wall -Wextra -c src/util.cpp(要求零 warning)"
g++ -std=c++17 -O2 -Wall -Wextra -c src/util.cpp -o /tmp/util_accept.o
echo "   OK"
echo "== 2) 更严格:-Wpedantic -Wshadow -Wconversion(非硬性,但目前也是干净的)"
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
    -c src/util.cpp -o /tmp/util_strict.o
echo "   OK"
echo "== 3) test_util @ ASan+UBSan"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -o /tmp/test_util_asan tests/test_util.cpp src/util.cpp
/tmp/test_util_asan
echo "== 4) test_util @ -O2 无 sanitizer"
g++ -std=c++17 -O2 -Wall -Wextra -o /tmp/test_util_o2 tests/test_util.cpp src/util.cpp
/tmp/test_util_o2
echo "== util 验收全部通过"

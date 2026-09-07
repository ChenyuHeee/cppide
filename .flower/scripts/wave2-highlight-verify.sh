#!/usr/bin/env bash
# desc: Wave2 语法高亮验收:highlight.cpp 零 warning 编译 + test_highlight 在 ASan/UBSan 与 -O2 下通过(含真 textbuf 链接)
set -u
cd "$(dirname "$0")/../.." || exit 1
fail=0
run() { echo "=== $1"; shift; "$@" || { echo "  ^^ FAILED"; fail=1; }; }

echo "=== 1. -Wall -Wextra 零 warning 编译 src/highlight.cpp"
out=$(g++ -std=c++17 -O2 -Wall -Wextra -c src/highlight.cpp -o /tmp/wave2_hl.o 2>&1)
if [ -n "$out" ]; then echo "$out"; echo "  ^^ 有 warning/error"; fail=1; else echo "  OK(无任何输出)"; fi

echo "=== 2. 测试:-O2 无消毒器"
g++ -std=c++17 -O2 -Wall -Wextra -o /tmp/wave2_th_o2 tests/test_highlight.cpp src/highlight.cpp \
  && /tmp/wave2_th_o2 || fail=1

echo "=== 3. 测试:-O1 -g -fsanitize=address,undefined(硬要求)"
g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -o /tmp/wave2_th_san tests/test_highlight.cpp src/highlight.cpp \
  && ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 /tmp/wave2_th_san || fail=1

echo "=== 4. 测试:与真实 textbuf.cpp 一起链接(弱符号被强符号覆盖,make tests 的场景)"
if [ -f src/textbuf.cpp ]; then
  g++ -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined \
    -o /tmp/wave2_th_real tests/test_highlight.cpp src/highlight.cpp src/textbuf.cpp src/util.cpp \
    && /tmp/wave2_th_real \
    && nm -C /tmp/wave2_th_real | grep -q "^[0-9a-f]* T TextBuffer::lineCount" \
    && echo "  OK:用的是真实现(强符号 T)" || fail=1
else
  echo "  跳过:src/textbuf.cpp 尚不存在"
fi

[ $fail -eq 0 ] && echo "ALL GREEN" || echo "SOME CHECK FAILED"
exit $fail

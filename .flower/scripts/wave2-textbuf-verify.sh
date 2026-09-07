#!/usr/bin/env bash
# desc: Wave 2 textbuf 自证:零 warning 编译 + ASan/UBSan 下跑 test_textbuf(全部产物落 /tmp)
set -u
cd /work || exit 1
CXX=${CXX:-g++}
STD="-std=c++17"
fail=0

echo "== 1) 验收编译:-O2 -Wall -Wextra 必须零 warning =="
out=$($CXX $STD -O2 -Wall -Wextra -c src/textbuf.cpp -o /tmp/tb-acc.o 2>&1)
if [ -n "$out" ]; then echo "$out"; echo "FAIL: 有 warning/error"; fail=1; else echo "OK 零 warning"; fi

echo "== 2) 更严格的编译:+ -Wpedantic -Wshadow -Wconversion =="
out=$($CXX $STD -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
        -c src/textbuf.cpp -o /tmp/tb-strict.o 2>&1)
if [ -n "$out" ]; then echo "$out"; echo "WARN: 严格模式有输出(非硬性)"; else echo "OK 零 warning"; fi

echo "== 3) 不依赖 util.cpp:目标文件里不得有 util:: 未定义符号 =="
if nm -uC /tmp/tb-acc.o | grep -q 'util::'; then
  nm -uC /tmp/tb-acc.o | grep 'util::'; echo "FAIL: 引入了 util 依赖"; fail=1
else echo "OK 无 util:: 依赖"; fi

echo "== 4) ASan + UBSan 下运行单测 =="
$CXX $STD -O1 -g -Wall -Wextra -fsanitize=address,undefined \
  -o /tmp/test_textbuf_san tests/test_textbuf.cpp src/textbuf.cpp || { echo "FAIL: 编译失败"; exit 1; }
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
  /tmp/test_textbuf_san || { echo "FAIL: sanitizer 下测试失败"; fail=1; }

echo "== 5) -O2 优化版再跑一遍(优化差异也要过) =="
$CXX $STD -O2 -Wall -Wextra -o /tmp/test_textbuf_o2 tests/test_textbuf.cpp src/textbuf.cpp \
  && /tmp/test_textbuf_o2 >/dev/null && echo "OK" || { echo "FAIL: -O2 下测试失败"; fail=1; }

echo "== 6) 残留临时文件检查(/work 下不得留测试产物) =="
leftover=$(git -C /work status --porcelain 2>/dev/null | grep -c 'cppide-tb-' || true)
ls /work | grep -c 'cppide-tb-' >/dev/null 2>&1
if ls /work/cppide-tb-* >/dev/null 2>&1 || ls /work/.cppide-save-* >/dev/null 2>&1; then
  echo "FAIL: /work 有残留"; fail=1
else echo "OK 无残留 (git: ${leftover:-n/a})"; fi

[ "$fail" = 0 ] && echo "ALL GREEN" || echo "有失败项"
exit $fail

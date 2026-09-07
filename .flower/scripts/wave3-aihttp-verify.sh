#!/bin/sh
# desc: 验 aihttp/SSE —— 零 warning 编译 + ASan/UBSan/LSan 跑 test_sse 与 test_aihttp(ROUNDS 轮)
set -e
ROOT=/work
OUT=/tmp/wave3-aihttp-verify
ROUNDS=${ROUNDS:-1}
mkdir -p "$OUT"

echo "== 1) 验收编译命令(必须零 warning)"
cd "$ROOT"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/aihttp.cpp -o "$OUT/aihttp.o" 2>"$OUT/warn.txt"
if [ -s "$OUT/warn.txt" ]; then echo "FAIL: 有 warning"; cat "$OUT/warn.txt"; exit 1; fi
echo "   zero warning OK"

echo "== 2) 额外严格 warning 档"
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
    -Wold-style-cast -Isrc -fsyntax-only src/aihttp.cpp 2>"$OUT/warn-strict.txt" || true
echo "   strict warnings: $(wc -l < "$OUT/warn-strict.txt") 行(仅供参考,不作为门槛)"

echo "== 3) ASan+UBSan 构建"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
g++ -std=c++17 -Wall -Wextra $SAN -o "$OUT/test_sse" \
    tests/test_sse.cpp src/aihttp.cpp src/json.cpp src/util.cpp -lcurl -lpthread
g++ -std=c++17 -Wall -Wextra $SAN -o "$OUT/test_aihttp" \
    tests/test_aihttp.cpp src/aihttp.cpp src/json.cpp src/util.cpp -lcurl -lpthread

export ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export LSAN_OPTIONS=suppressions=/dev/null

echo "== 4) 跑 $ROUNDS 轮"
i=1
while [ "$i" -le "$ROUNDS" ]; do
  timeout 300 "$OUT/test_sse"    >"$OUT/sse.$i.log"    2>&1 || { echo "FAIL test_sse 第 $i 轮"; tail -30 "$OUT/sse.$i.log"; exit 1; }
  timeout 300 "$OUT/test_aihttp" >"$OUT/aihttp.$i.log" 2>&1 || { echo "FAIL test_aihttp 第 $i 轮"; tail -40 "$OUT/aihttp.$i.log"; exit 1; }
  grep -qi "runtime error\|AddressSanitizer\|LeakSanitizer\|SUMMARY:" "$OUT/sse.$i.log" "$OUT/aihttp.$i.log" && { echo "FAIL: sanitizer 报错(第 $i 轮)"; exit 1; }
  echo "   round $i OK"
  i=$((i+1))
done
tail -2 "$OUT/aihttp.$ROUNDS.log"
tail -1 "$OUT/sse.$ROUNDS.log"
echo "ALL OK ($ROUNDS 轮)"

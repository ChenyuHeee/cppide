#!/usr/bin/env bash
# desc: Wave 4 ai.cpp 验收:零 warning 编译 + check-headers + test_ai(ASan/UBSan/LSan,ROUNDS 轮)
set -uo pipefail
cd /work

ROUNDS="${ROUNDS:-1}"
SRC="src/ai.cpp src/aihttp.cpp src/config.cpp src/json.cpp src/textbuf.cpp src/util.cpp"
LIBS="-lcurl -lpthread"
export CPPIDE_AI_CPP=/work/src/ai.cpp
fail=0
step() { printf '\n=== %s ===\n' "$*"; }

step "1) ai.cpp 零 warning(-O2 -Wall -Wextra)"
if timeout 300 g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/ai.cpp -o /tmp/ai.o 2> /tmp/w1.txt; then
  if [ -s /tmp/w1.txt ]; then echo "有 warning:"; cat /tmp/w1.txt; fail=1; else echo "OK 零输出"; fi
else cat /tmp/w1.txt; fail=1; fi

step "2) 额外严格档(-Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast)"
if timeout 300 g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
     -Wsign-conversion -Wold-style-cast -Isrc -c src/ai.cpp -o /tmp/ai_s.o 2> /tmp/w2.txt; then
  if [ -s /tmp/w2.txt ]; then echo "有 warning:"; cat /tmp/w2.txt; fail=1; else echo "OK 零输出"; fi
else cat /tmp/w2.txt; fail=1; fi

step "3) make check-headers"
timeout 300 make check-headers > /tmp/ch.txt 2>&1 && tail -1 /tmp/ch.txt || { cat /tmp/ch.txt; fail=1; }

step "4) 测试:ASan+UBSan 构建"
if ! timeout 600 g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Isrc -o /tmp/test_ai_asan tests/test_ai.cpp $SRC $LIBS 2> /tmp/b1.txt; then
  cat /tmp/b1.txt; exit 1
fi
[ -s /tmp/b1.txt ] && { echo "构建有 warning:"; cat /tmp/b1.txt; fail=1; }

step "5) 测试:-DNDEBUG 构建(§6 反证必须在 NDEBUG 下也通过)"
timeout 600 g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Isrc -o /tmp/test_ai_ndebug \
  tests/test_ai.cpp $SRC $LIBS 2>&1 | head -20

step "6) 跑 test_ai(NDEBUG,O2)"
timeout 300 /tmp/test_ai_ndebug > /tmp/ndebug.log 2>&1
rc=$?
tail -4 /tmp/ndebug.log
[ $rc -ne 0 ] && { echo "NDEBUG 轮失败 rc=$rc"; cat /tmp/ndebug.log; fail=1; }

step "7) 跑 test_ai × $ROUNDS 轮(ASan+UBSan+LSan;TSan 本机不可用,用连跑替代)"
export ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:abort_on_error=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
pass=0
for i in $(seq 1 "$ROUNDS"); do
  if timeout 300 /tmp/test_ai_asan > "/tmp/ai_round_$i.log" 2>&1; then
    pass=$((pass+1)); printf '  round %2d OK   %s\n' "$i" "$(grep -o '全部通过.*' "/tmp/ai_round_$i.log")"
  else
    echo "  round $i FAIL(rc=$?),日志 /tmp/ai_round_$i.log"; tail -25 "/tmp/ai_round_$i.log"; fail=1
  fi
done
echo "ASan 轮次:$pass/$ROUNDS 通过"
grep -h "UI 线程调用最坏" /tmp/ai_round_*.log | sort -t' ' -k4 -n | tail -1

printf '\n=== 结果:%s ===\n' "$([ $fail -eq 0 ] && echo 全部通过 || echo 有失败项)"
exit $fail

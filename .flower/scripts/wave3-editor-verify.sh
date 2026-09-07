#!/usr/bin/env bash
# desc: Wave3 editor 验收:editor.cpp 零 warning + test_editor 在 ASan/UBSan(assert 开/关)与 -O2/NDEBUG 下通过
set -u
cd /work || exit 1
FAIL=0
SRC="src/editor.cpp src/textbuf.cpp src/util.cpp src/highlight.cpp"

step() { printf '\n=== %s ===\n' "$1"; }

step "1. 零 warning 编译 (-O2 -Wall -Wextra)"
out=$(g++ -std=c++17 -O2 -Wall -Wextra -Isrc -c src/editor.cpp -o /tmp/ed.o 2>&1)
if [ -n "$out" ]; then echo "$out"; echo "-> 有 warning/error"; FAIL=1; else echo "OK 无输出"; fi

step "2. 更严格的一轮 (-Wshadow -Wold-style-cast 之外的常用项)"
out=$(g++ -std=c++17 -O2 -Wall -Wextra -Wshadow -Wsign-conversion -Isrc \
        -fsyntax-only src/editor.cpp 2>&1 | head -30)
[ -n "$out" ] && { echo "$out"; echo "(仅供参考,不计入验收)"; } || echo "OK"

# $1 = 标签, 其余 = 编译选项
build_run() {
  label="$1"; shift
  bin="/tmp/test_editor_$label"
  if ! g++ -std=c++17 -Isrc "$@" -o "$bin" tests/test_editor.cpp $SRC 2>/tmp/build_$label.log; then
    echo "编译失败:"; cat /tmp/build_$label.log; FAIL=1; return
  fi
  w=$(grep -c 'warning:' /tmp/build_$label.log)
  [ "$w" != 0 ] && { echo "有 $w 条 warning:"; grep 'warning:' /tmp/build_$label.log | head; FAIL=1; }
  if ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 "$bin"; then
    echo "-> $label PASS"
  else
    echo "-> $label FAIL (exit $?)"; FAIL=1
  fi
}

step "3. ASan+UBSan,assert 开启(§6 反证走 fork death test)"
build_run asan -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer

step "4. ASan+UBSan,-DNDEBUG(★ §6 反证测试在这里真正跑到 setGhost 运行期防线)"
build_run asan_ndebug -O1 -g -Wall -Wextra -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer

step "5. -O2 -DNDEBUG 普通构建"
build_run o2 -O2 -DNDEBUG -Wall -Wextra

step "6. -O0 无 sanitizer,assert 开启"
build_run o0 -O0 -g -Wall -Wextra

printf '\n===== %s =====\n' "$([ $FAIL = 0 ] && echo 'ALL PASS' || echo 'FAILED')"
exit $FAIL

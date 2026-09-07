#!/bin/sh
# desc: 验证 src/config.cpp 零 warning 编译 + 在 ASan/UBSan 与 -O2/NDEBUG 下跑 tests/test_config.cpp
set -e
cd /work
CXX=${CXX:-g++}
OUT=${OUT:-/tmp/cppide-cfgtest}
mkdir -p "$OUT"

# 真的 json.cpp / util.cpp(由别的 agent 提供)。Wave 2 期间曾用临时桩,现已删除。
DEPS="src/json.cpp src/util.cpp"
for f in $DEPS; do
  [ -f "$f" ] || { echo "FAIL: 缺少依赖 $f(它由别的 agent 负责)"; exit 1; }
done
echo "依赖: $DEPS"

echo "== 1. 验收编译:-O2 -Wall -Wextra,必须零 warning =="
W=$("$CXX" -std=c++17 -O2 -Wall -Wextra -c src/config.cpp -o "$OUT/config.o" 2>&1 | tee /dev/stderr | wc -l)
[ "$W" -eq 0 ] || { echo "FAIL: 编译有 $W 行 warning/error"; exit 1; }
echo "OK"

echo "== 2. ASan+UBSan 下编译并运行 tests/test_config.cpp =="
# shellcheck disable=SC2086
"$CXX" -std=c++17 -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 \
  -Isrc -DCONFIG_CPP_PATH='"/work/src/config.cpp"' -o "$OUT/test_config" \
  tests/test_config.cpp src/config.cpp $DEPS
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  "$OUT/test_config"

echo "== 3. -O2 -DNDEBUG 再跑一遍(断言不许被优化掉)=="
# shellcheck disable=SC2086
"$CXX" -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Isrc \
  -DCONFIG_CPP_PATH='"/work/src/config.cpp"' -o "$OUT/test_config_o2" \
  tests/test_config.cpp src/config.cpp $DEPS
"$OUT/test_config_o2"

echo "== 4. 源码硬性检查:不许有 sk- 字面量 =="
if grep -n 'sk-' src/config.cpp; then echo "FAIL: 源码里有疑似 key"; exit 1; fi
echo "OK"
echo "ALL PASS"

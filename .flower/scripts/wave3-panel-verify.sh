#!/usr/bin/env bash
# desc: Wave 3 panel 自证:零 warning 编译 + ASan/UBSan/LSan 跑 test_panel + 变异测试(产物全落 /tmp)
set -u
cd /work || exit 1
ulimit -c 0 2>/dev/null || true   # 变异体会 abort:别在 /work 里留 core
CXX=${CXX:-g++}
STD="-std=c++17"
SRCS="src/panel.cpp src/textbuf.cpp src/util.cpp"
fail=0

echo "== 1) 验收编译:-O2 -Wall -Wextra -Isrc 必须零 warning =="
out=$($CXX $STD -O2 -Wall -Wextra -Isrc -c src/panel.cpp -o /tmp/panel-acc.o 2>&1)
if [ -n "$out" ]; then echo "$out"; echo "FAIL: 有 warning/error"; fail=1; else echo "OK 零 warning"; fi

echo "== 2) 更严格:+ -Wpedantic -Wshadow -Wconversion =="
out=$($CXX $STD -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Isrc \
        -c src/panel.cpp -o /tmp/panel-strict.o 2>&1)
if [ -n "$out" ]; then echo "$out"; echo "FAIL: 严格模式有输出"; fail=1; else echo "OK 零 warning"; fi

echo "== 3) panel.cpp 不得碰 curses(panel.h 的 scroll() 会被宏打坏) =="
if grep -n '#include *[<"]curses\|#include *[<"]ncurses' src/panel.cpp; then
  echo "FAIL: include 了 curses"; fail=1
else echo "OK 无 curses 依赖"; fi

echo "== 4) ASan + UBSan + LSan 下跑单测 =="
$CXX $STD -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Isrc -o /tmp/test_panel_san tests/test_panel.cpp $SRCS || { echo "FAIL: 编译失败"; exit 1; }
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
  /tmp/test_panel_san || { echo "FAIL: sanitizer 下测试失败"; fail=1; }

echo "== 5) -O2 优化版再跑一遍(顺便量峰值内存) =="
$CXX $STD -O2 -Wall -Wextra -Isrc -o /tmp/test_panel_o2 tests/test_panel.cpp $SRCS \
  && /tmp/test_panel_o2 || { echo "FAIL: -O2 下测试失败"; fail=1; }

echo "== 6) 变异测试:单测必须能抓住这 4 个真实退化 =="
mkdir -p /tmp/panel-mut
python3 - <<'PY'
import sys
base = open('/work/src/panel.cpp').read()
muts = {
 # 朴素流式实现:丢掉 delta 尾部残缺的多字节序列(不做隐式 carry)
 'm1': ("  size_t i = 0;\n  const size_t n = delta.size();",
        "  size_t i = 0;\n  size_t n = delta.size();\n"
        "  while (n > 0 && util::utf8IsContinuation(static_cast<unsigned char>(delta[n-1]))) --n;\n"),
 # 按字节折行:会把多字节字符切两半
 'm2': ("""      const std::vector<util::WrapSeg> segs =
          util::wrapDisplay(lines_[i].text, view_w_, tab_width_);
      for (const util::WrapSeg& s : segs) {
        layout_.push_back(VisualLine{static_cast<int>(i), s.start, s.len});
      }""",
        """      const int len = static_cast<int>(lines_[i].text.size());
      int off = 0;
      do {
        const int take = std::min(view_w_, len - off);
        layout_.push_back(VisualLine{static_cast<int>(i), off, take});
        off += (take > 0) ? take : 1;
      } while (off < len);"""),
 # 宽度变化时不保持滚动锚点
 'm3': ("""  if (anchor_logical >= 0) {
    scroll_ = logicalToVisual(anchor_logical);     // 内部会按新宽度重算折行
  }""", "  // no anchor\n"),
 # 不做环形裁剪
 'm4': ("  if (lines.size() <= cap) return 0;", "  return 0;\n"),
}
for k, (old, new) in muts.items():
    if old not in base:
        print("MUTGEN-FAIL", k); sys.exit(1)
    open('/tmp/panel-mut/%s.cpp' % k, 'w').write(base.replace(old, new, 1))
PY
for m in m1 m2 m3 m4; do
  $CXX $STD -O1 -g -Isrc -o /tmp/test_panel_$m tests/test_panel.cpp \
    /tmp/panel-mut/$m.cpp src/textbuf.cpp src/util.cpp 2>/dev/null
  if /tmp/test_panel_$m >/dev/null 2>&1; then
    echo "FAIL: 变异 $m 没被单测抓住"; fail=1
  else
    echo "OK 变异 $m 被抓住"
  fi
done

[ "$fail" = 0 ] && echo "ALL GREEN" || echo "有失败项"
exit $fail

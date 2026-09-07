#!/usr/bin/env bash
# desc: 验修审计问题 2(make debug -j 竞态 + 删掉 ./cppide):release 与 debug 并行构建各自成功、互不删对方产物、tests 全过。
#
# 用法: bash fix-p2-make-verify.sh
# 背景: 老写法 `debug: clean all` 在 -j 下 clean 与 all 并发,间歇性
#       "cannot find src/ai.o",且会删掉 ./cppide 打挂正在跑的验收。
#       新写法:debug 产物是 cppide-debug + src/*.dbg.o,与 release 完全分离。

set -u
cd /work || exit 2
fail=0
say() { echo "$1"; }
ck() { if [ "$1" -eq 0 ]; then say "PASS  $2"; else say "FAIL  $2"; fail=1; fi; }

# 0) 干净起点
timeout 300 make clean > /tmp/p2-clean.log 2>&1
[ -e /work/cppide ] && { say "FAIL  make clean 之后 ./cppide 还在"; fail=1; }

# 1) release 并行构建
timeout 900 make -j4 > /tmp/p2-rel.log 2>&1; rc=$?
ck $rc "make -j4 成功(exit=$rc)"
w=$(grep -c 'warning:' /tmp/p2-rel.log)
ck $([ "$w" -eq 0 ] && echo 0 || echo 1) "release 构建零 warning(实测 $w 条)"
[ -x /work/cppide ]; ck $? "./cppide 已产出"
relstamp=$(stat -c %Y /work/cppide 2>/dev/null || stat -f %m /work/cppide)

# 2) debug 并行构建(竞态是间歇的,连跑 3 次;其中一次从干净的 dbg 目标开始)
rm -f /work/src/*.dbg.o /work/src/*.dbg.d /work/cppide-debug
for i in 1 2 3; do
  timeout 900 make debug -j4 > "/tmp/p2-dbg$i.log" 2>&1; rc=$?
  ck $rc "第 $i 次 make debug -j4 成功(exit=$rc)"
  w=$(grep -c 'warning:' "/tmp/p2-dbg$i.log")
  ck $([ "$w" -eq 0 ] && echo 0 || echo 1) "第 $i 次 debug 构建零 warning(实测 $w 条)"
  [ -x /work/cppide-debug ]; ck $? "第 $i 次:./cppide-debug 已产出"
  [ -x /work/cppide ]; ck $? "第 $i 次:./cppide 没有被 debug 构建删掉"
done

# 3) release 产物没被重建/覆盖(mtime 不变 => 确实互不干扰)
now=$(stat -c %Y /work/cppide 2>/dev/null || stat -f %m /work/cppide)
ck $([ "$now" = "$relstamp" ] && echo 0 || echo 1) "./cppide 的 mtime 未被 debug 构建改动"

# 4) debug 产物真的带 sanitizer
if nm -C /work/cppide-debug 2>/dev/null | grep -q '__asan\|__ubsan' || \
   ldd /work/cppide-debug 2>/dev/null | grep -q 'libasan\|libubsan'; then
  say "PASS  cppide-debug 里有 ASan/UBSan 符号"
else
  say "FAIL  cppide-debug 没链上 sanitizer"; fail=1
fi
# release 产物**不**带 sanitizer
if nm -C /work/cppide 2>/dev/null | grep -q '__asan_report' ; then
  say "FAIL  release 产物混进了 sanitizer"; fail=1
else
  say "PASS  release 产物干净(无 sanitizer)"
fi

# 5) 两者反复交替:再各跑一次,断言都是 "Nothing to be done"(增量正确、不互相触发重编)
timeout 300 make -j4 > /tmp/p2-rel2.log 2>&1
grep -q "Nothing to be done" /tmp/p2-rel2.log; ck $? "release 增量:无需重编"
timeout 300 make debug -j4 > /tmp/p2-dbg-inc.log 2>&1
grep -q "Nothing to be done" /tmp/p2-dbg-inc.log; ck $? "debug 增量:无需重编"

# 6) 其余目标不受影响
timeout 300 make check-headers > /tmp/p2-hdr.log 2>&1; ck $? "make check-headers 通过"
grep -c '  src/' /tmp/p2-hdr.log | grep -qx 15; ck $? "check-headers 覆盖 15 个头文件"
timeout 900 make tests > /tmp/p2-tests.log 2>&1; ck $? "make tests 全过"
[ -x /work/cppide ]; ck $? "跑完全部目标后 ./cppide 仍在"

echo
[ "$fail" -eq 0 ] && echo "== 全部通过 ==" || echo "== 有失败 =="
exit $fail

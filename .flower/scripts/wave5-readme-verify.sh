#!/bin/sh
# desc: 逐条真跑 README.md 里承诺的命令,并校验 config.sample.json
set -u
cd /work || exit 1
FAIL=0
run() { # run <label> <timeout_s> <cmd...>
  label="$1"; shift; t="$1"; shift
  out=$(timeout "$t" "$@" 2>&1); rc=$?
  if [ $rc -eq 0 ]; then printf 'OK   %-28s rc=0\n' "$label"
  else printf 'FAIL %-28s rc=%s\n' "$label" "$rc"; echo "$out" | tail -5; FAIL=1; fi
}

run "make"                 600 make
run "make check-headers"   300 make check-headers
run "make tests"           600 make tests
run "--help"                20 ./cppide --help
run "--version"             20 ./cppide --version
run "--print-config"        20 ./cppide --print-config
run "--doctor"              30 ./cppide --doctor
run "--doctor+sample"       30 ./cppide --config config.sample.json --doctor

# config.sample.json 必须是合法 JSON、api_key 为空、与 --print-config 一致
timeout 20 ./cppide --print-config > /tmp/_pc.json 2>/dev/null
if diff -q /tmp/_pc.json /work/config.sample.json >/dev/null; then
  echo "OK   sample==print-config"
else echo "FAIL sample!=print-config"; FAIL=1; fi
python3 - <<'PY' || FAIL=1
import json,sys
d=json.load(open('/work/config.sample.json'))
assert d['api_key']=='', 'api_key 非空!'
known=[k for k in d if not k.startswith('_')]
assert len(known)==31, f'可配字段数 {len(known)} != 31'
print(f"OK   sample JSON 合法 · 可配字段 {len(known)} · api_key 为空")
PY
# --doctor 必须在 warnings 里说明 AI 未配置
timeout 30 ./cppide --config config.sample.json --doctor 2>&1 | grep -q "AI 未配置" \
  && echo "OK   sample doctor warns AI 未配置" || { echo "FAIL AI 未配置 warning 缺失"; FAIL=1; }
# README 里承诺的非零退出码
timeout 20 ./cppide --bogus >/dev/null 2>&1; [ $? -eq 2 ] \
  && echo "OK   未知选项 rc=2" || { echo "FAIL 未知选项 rc!=2"; FAIL=1; }
timeout 20 ./cppide a.cpp b.cpp >/dev/null 2>&1; [ $? -eq 2 ] \
  && echo "OK   两个文件 rc=2" || { echo "FAIL 两个文件 rc!=2"; FAIL=1; }
timeout 20 ./cppide /tmp/x.txt >/dev/null 2>&1; [ $? -eq 2 ] \
  && echo "OK   不支持的扩展名 rc=2" || { echo "FAIL 不支持的扩展名 rc!=2"; FAIL=1; }
timeout 20 ./cppide /nope/dir/a.cpp >/dev/null 2>&1; [ $? -eq 2 ] \
  && echo "OK   父目录不存在 rc=2" || { echo "FAIL 父目录不存在 rc!=2"; FAIL=1; }
timeout 20 ./cppide /tmp >/dev/null 2>&1; [ $? -eq 2 ] \
  && echo "OK   目录参数 rc=2" || { echo "FAIL 目录参数 rc!=2"; FAIL=1; }
mkfifo /tmp/_fifo.cpp 2>/dev/null
timeout 20 ./cppide /tmp/_fifo.cpp >/dev/null 2>&1; [ $? -eq 2 ] \
  && echo "OK   命名管道 rc=2" || { echo "FAIL 命名管道 rc!=2"; FAIL=1; }
rm -f /tmp/_fifo.cpp
# 非交互环境:合法的新文件路径也应以 rc=3 拒绝启动 TUI
timeout 20 ./cppide /tmp/_newfile.cpp >/dev/null 2>&1; [ $? -eq 3 ] \
  && echo "OK   非交互环境 rc=3" || { echo "FAIL 非交互环境 rc!=3"; FAIL=1; }

[ $FAIL -eq 0 ] && echo "=== wave5-readme-verify: ALL OK ===" || echo "=== wave5-readme-verify: 有失败 ==="
exit $FAIL

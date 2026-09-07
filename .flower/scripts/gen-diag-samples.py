#!/usr/bin/env python3
# desc: 用真实 g++/gcc 的 stderr 生成 tests/test_diag.cpp 的样本数据块(替换 //@@SAMPLES@@ 标记)
#
# 用法:  python3 .flower/scripts/gen-diag-samples.py            # 重新编样本并回填测试
#         python3 .flower/scripts/gen-diag-samples.py --dump     # 只打印样本
#
# 为什么要有这个脚本:test_diag.cpp 的验收要求是"用真实编译器输出做样本"。
# 手抄 30 行模板报错必然抄错,所以现场编、现场生成 C++ raw string。
import os
import subprocess
import sys
import tempfile

WORK = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TEST = os.path.join(WORK, "tests", "test_diag.cpp")
MARK = "//@@SAMPLES@@"

# (变量名, 说明, 文件名 -> 内容, 编译命令模板)
CASES = []


def case(var, zh, files, argv):
    CASES.append((var, zh, files, argv))


case("kMultiError", "多个 error + 多个 warning(最常见的一屏)",
     {"a.cpp": '#include <vector>\nint main() {\n  int x = ;\n  undeclared_fn(1);\n  int y;\n  return z;\n}\n'},
     ["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-c", "a.cpp", "-o", "/dev/null"])

case("kIncludedFrom", "In file included from(错误在被包含的头里)",
     {"inc.h": '#pragma once\nstruct S { int a; };\nvoid f(S s) { s.nonexistent = 1; }\n',
      "b.cpp": '#include "inc.h"\nint main() { return 0; }\n'},
     ["g++", "-std=c++17", "-Wall", "-c", "b.cpp", "-o", "/dev/null"])

case("kFatalError", "fatal error: 找不到头文件 + compilation terminated.",
     {"c.cpp": '#include "no_such_header_here.h"\nint main(){}\n'},
     ["g++", "-std=c++17", "-c", "c.cpp", "-o", "/dev/null"])

case("kWithNote", "error + note: declared here",
     {"d.cpp": 'void g(int a, int b) { (void)a; (void)b; }\nint main() { g(1); return 0; }\n'},
     ["g++", "-std=c++17", "-c", "d.cpp", "-o", "/dev/null"])

case("kNestedInclude", "两级包含链(from 续行)+ static_assert + note",
     {"lvl2.h": '#pragma once\nstatic_assert(sizeof(int) == 99, "bad int size");\n',
      "lvl1.h": '#pragma once\n#include "lvl2.h"\n',
      "e.cpp": '#include "lvl1.h"\nint main(){}\n'},
     ["g++", "-std=c++17", "-c", "e.cpp", "-o", "/dev/null"])

case("kTemplateBlowup", "模板展开的超长错误(required from / 一堆 candidate note)",
     {"f.cpp": '#include <vector>\n#include <map>\n#include <string>\n#include <algorithm>\n'
               'int main() {\n  std::map<std::string, std::vector<int>> m;\n'
               '  std::sort(m.begin(), m.end());\n  return 0;\n}\n'},
     ["g++", "-std=c++17", "-c", "f.cpp", "-o", "/dev/null"])

case("kWeirdPath", "路径含 ':' 和空格 —— 行号解析的头号陷阱",
     {"dir:with space/my file:2.cpp":
          '#include <vector>\nint main() {\n  int x = ;\n  undeclared_fn(1);\n  int y;\n  return z;\n}\n'},
     ["g++", "-std=c++17", "-Wall", "-Wextra", "-c", "dir:with space/my file:2.cpp", "-o", "/dev/null"])

case("kCLang", "C 编译器(gcc)的输出:error/note/warning 混排",
     {"h.c": '#include <stdio.h>\nint main(void) {\n  int a = "str";\n'
             '  printf("%d\\n", undefined_thing);\n  return 0\n}\n'},
     ["gcc", "-std=c11", "-O2", "-Wall", "-c", "h.c", "-o", "/dev/null"])

case("kLinkError", "链接期错误(undefined reference + collect2)",
     {"i.cpp": 'void missing_symbol();\nint main() { missing_symbol(); return 0; }\n'},
     ["g++", "-std=c++17", "i.cpp", "-o", "i.bin"])

case("kDriverError", "驱动层错误:没有行号的 'g++: fatal error: ...'",
     {},
     ["g++", "-std=c++17", "--nonexistent-option"])

case("kHeaderDirect", "直接 -fsyntax-only 一个头文件(会带那条无法关掉的假警告)",
     {"bad.h": '#pragma once\nint f() { return notdefined; }\n'},
     ["g++", "-std=c++17", "-O2", "-Wall", "-fsyntax-only", "bad.h"])


def collect():
    out = []
    for var, zh, files, argv in CASES:
        with tempfile.TemporaryDirectory(prefix="diagsample-") as d:
            for name, body in files.items():
                p = os.path.join(d, name)
                os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, "w") as f:
                    f.write(body)
            env = dict(os.environ, LC_ALL="C")
            r = subprocess.run(argv, cwd=d, env=env, capture_output=True, text=True)
            out.append((var, zh, r.stderr))
    return out


def render(samples):
    lines = ["// ==== 以下样本由 .flower/scripts/gen-diag-samples.py 现场编译真实的 g++/gcc 采集 ====",
             "// 不要手改:重新生成用 `python3 .flower/scripts/gen-diag-samples.py`。",
             "// 采集环境:g++ (Debian) 14.2.0 / LC_ALL=C。",
             ""]
    for var, zh, text in samples:
        assert ')DIAG"' not in text, var
        lines.append("// %s" % zh)
        lines.append('static const char %s[] = R"DIAG(%s)DIAG";' % (var, text))
        lines.append("")
    return "\n".join(lines)


def main():
    samples = collect()
    block = render(samples)
    if "--dump" in sys.argv:
        print(block)
        return
    with open(TEST) as f:
        src = f.read()
    if MARK not in src:
        sys.exit("找不到标记 %s(是否已经回填过?)" % MARK)
    with open(TEST, "w") as f:
        f.write(src.replace(MARK, block))
    print("已回填 %d 个样本到 %s" % (len(samples), TEST))


main()

# desc: 扫描 src/*.h,报告"声明了 getter/setter 但 private 区找不到对应存储成员"的疑似缺口
import re, glob, os, sys

def snake(n):
    s = re.sub(r'(?<!^)(?=[A-Z])', '_', n).lower()
    return s

for h in sorted(glob.glob('/work/src/*.h')):
    txt = open(h).read()
    # 所有 name_ 形式的标识符(成员)
    members = set(re.findall(r'\b([a-z][A-Za-z0-9_]*_)\b', txt))
    # getter: `Type name() const;`  setter: `void setName(...)`
    getters = re.findall(r'\b([a-z][A-Za-z0-9]*)\s*\(\s*\)\s*const\s*;', txt)
    setters = re.findall(r'void\s+set([A-Z][A-Za-z0-9]*)\s*\(', txt)
    miss = []
    for g in set(getters):
        cands = {g + '_', snake(g) + '_'}
        if not (cands & members):
            miss.append('get:' + g)
    for s in set(setters):
        base = s[0].lower() + s[1:]
        cands = {base + '_', snake(base) + '_'}
        if not (cands & members):
            miss.append('set:' + s)
    if miss:
        print(os.path.basename(h) + ': ' + ', '.join(sorted(miss)))

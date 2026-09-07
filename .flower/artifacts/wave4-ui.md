# Wave 4 · src/ui.cpp 验收记录

- 代码:`/work/src/ui.cpp`(渲染层,全工程唯一 include curses 的 .cpp)
- 单测:`/work/tests/test_ui.cpp`(纯逻辑)
- 复跑脚本:`/work/.flower/scripts/wave4-ui-verify.sh`(一条命令跑完下面全部 20 项,内含 pty 驱动源码)
- 产物目录:`/tmp/wave4-ui/`(9 张屏幕快照、ANSI 转义流、按键记录、各步日志)

## 1. 结论

零 warning;`make check-headers` 通过;test_ui 在 O2 与 ASan/UBSan 下均通过;
pty 真渲染 80x24 / 60x16 / 20x5 三档 × 3 帧 = 9 帧全部成功(不崩、无越界写、回读 bad_cells=0);
终端状态恢复三条网(atexit / SIGSEGV / std::terminate)实测均把 pty 的 ICANON+ECHO 复原。

## 2. 验收输出(原样)

```

=== 1. 编译 src/ui.cpp(-Wall -Wextra 必须零 warning) ===
  [ ok ] ui.cpp 零 warning

=== 2. make check-headers ===
  [ ok ] check-headers: OK (15 个头文件)

=== 3. 依赖对象 ===
  [ ok ] src/ai.cpp 可编译,链接真实 aiModeZh

=== 4. tests/test_ui.cpp(纯逻辑) —— O2 ===
  [ ok ] test_ui: 全部通过(6 个用例 / 833467 个断言)

=== 5. tests/test_ui.cpp —— ASan + UBSan ===
  [ ok ] ASan/UBSan 通过:test_ui: 全部通过(6 个用例 / 833467 个断言)

=== 6. pty 真渲染 + 终端状态恢复 ===
  [ ok ] pty 驱动编译通过
  [ ok ] 渲染 80x24 frame=0 -> snap-80x24-f0.txt
  [ ok ] 渲染 80x24 frame=1 -> snap-80x24-f1.txt
  [ ok ] 渲染 80x24 frame=2 -> snap-80x24-f2.txt
  [ ok ] 渲染 60x16 frame=0 -> snap-60x16-f0.txt
  [ ok ] 渲染 60x16 frame=1 -> snap-60x16-f1.txt
  [ ok ] 渲染 60x16 frame=2 -> snap-60x16-f2.txt
  [ ok ] 渲染 20x5 frame=0 -> snap-20x5-f0.txt
  [ ok ] 渲染 20x5 frame=1 -> snap-20x5-f1.txt
  [ ok ] 渲染 20x5 frame=2 -> snap-20x5-f2.txt

=== 7. pty 里真按键(raw+keypad+escdelay+Alt 合成 全路径) ===
  [ ok ] 9 个键全部归一化正确(见 /tmp/wave4-ui/keys.txt)

=== 8. 终端状态恢复(pty 里读 termios 的 ICANON/ECHO) ===
  [ ok ] restore raw-exit   exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=0 ECHO=0
  [ ok ] restore atexit     exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=1 ECHO=1
  [ ok ] restore segv       exited=0 code=-1 signaled=1 sig=11  tcgetattr=0 ICANON=1 ECHO=1
  [ ok ] restore terminate  exited=0 code=-1 signaled=1 sig=6  tcgetattr=0 ICANON=1 ECHO=1

=== SUMMARY ===
  passed=20 failed=0   产物目录:/tmp/wave4-ui
```

## 3. pty 真渲染快照(人工核对用)

回读方式:渲染完成后、`endwin()` 之前用 ncurses 自己的 `win_wch()` 逐格读回
"终端上真正显示的字符 + 颜色对",比解析 ANSI 转义序列可靠;宽字符占两格,
第二格与第一格回读到同一个字符,快照里已跳过重复。
颜色对图用 `0-9a-z` 表示 `Theme::Pair` 编号(`.` = 默认对 0),对照:
`1`=Normal `2`=Keyword `3`=Type `4`=Preproc `5`=String `6`=Char `7`=Number
`8`=Comment `9`=Operator `a`=Func `b`=Todo `c`=Ghost `d`=LineNo `e`=LineNoPractice
`f`=StatusCode `g`=StatusPractice `h`=StatusBar `i`=TabActive `j`=TabInactive
`k`=TabUnread `l`=Error `m`=Warn `n`=Meta `o`=Help

三帧的内容:
- frame 0:**练习模式** + 面板聚焦【编译】+ 运行中(状态栏应出现"运行中… Esc 终止")
- frame 1:**写代码模式** + 4 行 ghost(锚点在第 5 行行尾)+ 提示行(跳到行号)
- frame 2:F1 帮助浮层

机器断言(不只看图):frame 0/1 会逐格校验
`状态栏最左格颜色对 == P_StatusPractice / P_StatusCode`、
`行号槽每一格 == P_LineNoPractice / P_LineNo`(ghost 覆盖行除外)。
这就是"当前处于哪个模式一眼可见"这条验收标准的机器化证明;
第三重冗余(文字)见快照里的"练习模式 / 写代码模式"字样。

### 80x24 · frame 0

```
### frame=0  80x24  bad_cells=0
--- 屏幕文本 ---
   1 #include <bits/stdc++.h>                                                   
   2 using namespace std;                                                       
   3 // 读入 n 个整数并求和 —— 这一行是中文注释,用来测宽字符排版                
   4 int main() {                                                               
   5     int n; cin >> n;                                                       
   6     vector<long long> a(n);   // TODO: 处理 n == 0 的情况                  
   7     for (int i = 0; i < n; i++) cin >> a[i];                               
   8     long long s = 0;                                                       
   9     /* 块注释:累加                                                         
  10        第二行 */                                                           
  11     for (long long v : a) s += v;                                          
  12     cout << "总和 = " << s << '\n';                                        
  13     return 0;                                                              
  14 }                                                                          
─[编译 2*]─[运行*]─[AI*]─[输入]────────────────────────────────────Esc 回编辑区─
主程序.cpp:6:5: error: 'x' 未声明(注意:中文诊断需 LC_ALL=C 才不会出现)          
主程序.cpp:9:3: warning: unused variable 'y'                                    
-- 编译命令:c++ -O2 -std=c++17 -Wall 主程序.cpp -o /tmp/a.out                   
                                                                                
                                                                                
                                                                                
                                                                                
                                                                                
 练习模式  │ 主程序.cpp* │ 5:21 │ 2 错误… │ AI 思考中… │ 运行中… Esc 终止 F1帮助
--- 颜色对(0-9a-z,'.'=默认对)---
eeeee444444441555555555555555...................................................
eeeee22222122222222211119.......................................................
eeeee88888888888888888888888888888888888888888888888888888888888................
eeeee3331aaaa9919...............................................................
eeeee11113331191111199119.......................................................
eeeee1111333333933331333391a9199111888bbbb88888888888888888888..................
eeeee11112221933311191791119119119991111199119199...............................
eeeee11113333133331119179.......................................................
eeeee111188888888888888.........................................................
eeeee8888888888888888...........................................................
eeeee111122219333313333111911911199119..........................................
eeeee11111111199155555555519911199166669........................................
eeeee1111222222179..............................................................
eeeee9..........................................................................
niiiiiiiiinkkkkkkknkkkkknjjjjjjnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn
llllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllll
llllllllllllllllllllllllllllllllllllllllllll....................................
................................................................................
................................................................................
................................................................................
................................................................................
................................................................................
................................................................................
gggggggggghhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh
--- 布局 ---
gutter y=0 x=0 h=14 w=5
editor y=0 x=5 h=14 w=75
tabs   y=14 x=0 h=1 w=80
panel  y=15 x=0 h=8 w=80
prompt y=0 x=0 h=0 w=0
status y=23 x=0 h=1 w=80
theme  has_color=1 colors=256 pairs=65536 wide_ok=1 status_row=23
```

### 80x24 · frame 1

```
### frame=1  80x24  bad_cells=0
--- 屏幕文本 ---
   2 using namespace std;                                                       
   3 // 读入 n 个整数并求和 —— 这一行是中文注释,用来测宽字符排版                
   4 int main() {                                                               
   5     int n; cin >> n;for (int i = 0; i < n; i++) {                          
~            sum += a[i];                                                       
~            cnt++;                                                             
~        }                                                                      
   9     /* 块注释:累加                                                         
  10        第二行 */                                                           
  11     for (long long v : a) s += v;                                          
  12     cout << "总和 = " << s << '\n';                                        
  13     return 0;                                                              
  14 }                                                                          
─[编译 2*]─[运行*]─[AI*]─[输入]─────────────────────────────────────────────────
主程序.cpp:6:5: error: 'x' 未声明(注意:中文诊断需 LC_ALL=C 才不会出现)          
主程序.cpp:9:3: warning: unused variable 'y'                                    
-- 编译命令:c++ -O2 -std=c++17 -Wall 主程序.cpp -o /tmp/a.out                   
                                                                                
                                                                                
                                                                                
                                                                                
                                                                                
跳到行号: 128                                                                   
 写代码模式  │ 主程序.cpp* │ 5:21 │ 已保存 主程序.cpp(中文文件名) │ AI …  F1帮助
--- 颜色对(0-9a-z,'.'=默认对)---
ddddd22222122222222211119.......................................................
ddddd88888888888888888888888888888888888888888888888888888888888................
ddddd3331aaaa9919...............................................................
ddddd11113331191111199119ccccccccccccccccccccccccccccc..........................
cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
ddddd111188888888888888.........................................................
ddddd8888888888888888...........................................................
ddddd111122219333313333111911911199119..........................................
ddddd11111111199155555555519911199166669........................................
ddddd1111222222179..............................................................
ddddd9..........................................................................
niiiiiiiiinkkkkkkknkkkkknjjjjjjnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn
llllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllllll..........
llllllllllllllllllllllllllllllllllllllllllll....................................
................................................................................
................................................................................
................................................................................
................................................................................
................................................................................
................................................................................
nnnnnnnnnn......................................................................
ffffffffffffhhhhhhhhhhhhhhhhhhhhhhhhlllllllllllllllllllllllllllllhhhhhhhhhhhhhhh
--- 布局 ---
gutter y=0 x=0 h=13 w=5
editor y=0 x=5 h=13 w=75
tabs   y=13 x=0 h=1 w=80
panel  y=14 x=0 h=8 w=80
prompt y=22 x=0 h=1 w=80
status y=23 x=0 h=1 w=80
theme  has_color=1 colors=256 pairs=65536 wide_ok=1 status_row=23
```

### 80x24 · frame 2

```
### frame=2  80x24  bad_cells=0
--- 屏幕文本 ---
   1 #include <bits/stdc++.h>                                                   
    ─ cppide 帮助(Esc 或 F1 关闭)──────────────────────────────────────────     
      cppide 快捷键                        F1 / Esc 关闭本浮层                  
                                                                                
      ── 移动 ──                                                                
        ←                     左移一个字符                                      
        →                     右移一个字符                                      
        ↑                     上移一行                                          
        ↓                     下移一行                                          
  1     Alt-←                 按词左移                                          
  1     Alt-→                 按词右移                                          
  1     Home                  行首                                              
  1     End                   行尾                                              
  1     PgUp                  上翻一页                                          
─[编    PgDn                  下翻一页                                      ────
主程    Alt-< / Alt-Home / Shift-Home跳到文件开头                               
主程    Alt-> / Alt-End / Shift-End跳到文件末尾                                 
--                                                                              
      ── 编辑 ──                                                                
        Enter                 换行(带自动缩进)                                  
        Backspace             向左删除                                          
      …(终端太矮,帮助未显示完)                                                  
    ───────────────────────────────────────────────────────────────────────     
 写代码模式  │ 主程序.cpp* │ 5:21 │ C++ │ 2 错误 1 警告 │ AI 错误         F1帮助
--- 颜色对(0-9a-z,'.'=默认对)---
ddddd444444441555555555555555...................................................
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ddd.ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
niiiooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.nnnn
llllooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
llllooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
....ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
....ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
....ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
....ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
....ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
....ooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.....
ffffffffffffhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh
--- 布局 ---
gutter y=0 x=0 h=14 w=5
editor y=0 x=5 h=14 w=75
tabs   y=14 x=0 h=1 w=80
panel  y=15 x=0 h=8 w=80
prompt y=0 x=0 h=0 w=0
status y=23 x=0 h=1 w=80
theme  has_color=1 colors=256 pairs=65536 wide_ok=1 status_row=23
```

### 60x16 · frame 0

```
### frame=0  60x16  bad_cells=0
--- 屏幕文本 ---
   3 // 读入 n 个整数并求和 —— 这一行是中文注释,用来测宽字符
   4 int main() {                                           
   5     int n; cin >> n;                                   
   6     vector<long long> a(n);   // TODO: 处理 n == 0 的情
   7     for (int i = 0; i < n; i++) cin >> a[i];           
   8     long long s = 0;                                   
─[编译 2*]─[运行*]─[AI*]─[输入]────────────────Esc 回编辑区─
主程序.cpp:6:5: error: 'x' 未声明(注意:中文诊断需 LC_ALL=C  
才不会出现)                                                 
主程序.cpp:9:3: warning: unused variable 'y'                
-- 编译命令:c++ -O2 -std=c++17 -Wall 主程序.cpp -o          
/tmp/a.out                                                  
                                                            
                                                            
                                                            
 练习模式  │ 主程序.cpp* │ 5:21 │ 运行中… Esc 终止    F1帮助
--- 颜色对(0-9a-z,'.'=默认对)---
eeeee8888888888888888888888888888888888888888888888888888888
eeeee3331aaaa9919...........................................
eeeee11113331191111199119...................................
eeeee1111333333933331333391a9199111888bbbb888888888888888888
eeeee11112221933311191791119119119991111199119199...........
eeeee11113333133331119179...................................
niiiiiiiiinkkkkkkknkkkkknjjjjjjnnnnnnnnnnnnnnnnnnnnnnnnnnnnn
llllllllllllllllllllllllllllllllllllllllllllllllllllllllllll
llllllllllllllllllllllllllllllllllllllllllllllllllllllllllll
llllllllllllllllllllllllllllllllllllllllllll................
............................................................
............................................................
............................................................
............................................................
............................................................
gggggggggghhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh
--- 布局 ---
gutter y=0 x=0 h=6 w=5
editor y=0 x=5 h=6 w=55
tabs   y=6 x=0 h=1 w=60
panel  y=7 x=0 h=8 w=60
prompt y=0 x=0 h=0 w=0
status y=15 x=0 h=1 w=60
theme  has_color=1 colors=256 pairs=65536 wide_ok=1 status_row=15
```

### 60x16 · frame 1

```
### frame=1  60x16  bad_cells=0
--- 屏幕文本 ---
   3 // 读入 n 个整数并求和 —— 这一行是中文注释,用来测宽字符
   4 int main() {                                           
   5     int n; cin >> n;for (int i = 0; i < n; i++) {      
~            sum += a[i];                                   
~            cnt++;                                         
─[编译 2*]─[运行*]─[AI*]─[输入]─────────────────────────────
主程序.cpp:6:5: error: 'x' 未声明(注意:中文诊断需 LC_ALL=C  
才不会出现)                                                 
主程序.cpp:9:3: warning: unused variable 'y'                
-- 编译命令:c++ -O2 -std=c++17 -Wall 主程序.cpp -o          
/tmp/a.out                                                  
                                                            
                                                            
                                                            
跳到行号: 128                                               
 写代码模式  │ 主程序.cpp* │ 5:21 │ 已保存 主程序.cp… F1帮助
--- 颜色对(0-9a-z,'.'=默认对)---
ddddd8888888888888888888888888888888888888888888888888888888
ddddd3331aaaa9919...........................................
ddddd11113331191111199119ccccccccccccccccccccccccccccc......
cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
niiiiiiiiinkkkkkkknkkkkknjjjjjjnnnnnnnnnnnnnnnnnnnnnnnnnnnnn
llllllllllllllllllllllllllllllllllllllllllllllllllllllllll..
lllllllllll.................................................
llllllllllllllllllllllllllllllllllllllllllll................
............................................................
............................................................
............................................................
............................................................
............................................................
nnnnnnnnnn..................................................
ffffffffffffhhhhhhhhhhhhhhhhhhhhhhhhlllllllllllllllllhhhhhhh
--- 布局 ---
gutter y=0 x=0 h=5 w=5
editor y=0 x=5 h=5 w=55
tabs   y=5 x=0 h=1 w=60
panel  y=6 x=0 h=8 w=60
prompt y=14 x=0 h=1 w=60
status y=15 x=0 h=1 w=60
theme  has_color=1 colors=256 pairs=65536 wide_ok=1 status_row=15
```

### 60x16 · frame 2

```
### frame=2  60x16  bad_cells=0
--- 屏幕文本 ---
   3 // 读入 n 个整数并求和 —— 这一行是中文注释,用来测宽字符
 ─ cppide 帮助(Esc 或 F1 关闭)───────────────────────────── 
   cppide 快捷键                        F1 / Esc 关闭本…    
                                                            
   ── 移动 ──                                               
     ←                     左移一个字符                     
     →                     右移一个字符                     
     ↑                     上移一行                         
     ↓                     下移一行                         
     Alt-←                 按词左移                         
     Alt-→                 按词右移                         
     Home                  行首                             
     End                   行尾                             
   …(终端太矮,帮助未显示完)                                 
 ────────────────────────────────────────────────────────── 
 写代码模式  │ 主程序.cpp* │ 5:21 │ 2 错误… │ AI 错误 F1帮助
--- 颜色对(0-9a-z,'.'=默认对)---
ddddd8888888888888888888888888888888888888888888888888888888
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
.oooooooooooooooooooooooooooooooooooooooooooooooooooooooooo.
ffffffffffffhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhhh
--- 布局 ---
gutter y=0 x=0 h=6 w=5
editor y=0 x=5 h=6 w=55
tabs   y=6 x=0 h=1 w=60
panel  y=7 x=0 h=8 w=60
prompt y=0 x=0 h=0 w=0
status y=15 x=0 h=1 w=60
theme  has_color=1 colors=256 pairs=65536 wide_ok=1 status_row=15
```

### 20x5 · frame 0

```
### frame=0  20x5  bad_cells=0
--- 屏幕文本 ---
终端太小:当前 20x5,…
                    
                    
                    
                    
--- 颜色对(0-9a-z,'.'=默认对)---
....................
....................
....................
....................
....................
--- 布局 ---
gutter y=0 x=0 h=0 w=0
editor y=0 x=0 h=0 w=0
tabs   y=0 x=0 h=0 w=0
panel  y=0 x=0 h=0 w=0
prompt y=0 x=0 h=0 w=0
status y=0 x=0 h=0 w=0
theme  has_color=1 colors=256 pairs=65536 wide_ok=1 status_row=4
```

### 20x5 · frame 1

```
### frame=1  20x5  bad_cells=0
--- 屏幕文本 ---
终端太小:当前 20x5,…
                    
                    
                    
                    
--- 颜色对(0-9a-z,'.'=默认对)---
....................
....................
....................
....................
....................
--- 布局 ---
gutter y=0 x=0 h=0 w=0
editor y=0 x=0 h=0 w=0
tabs   y=0 x=0 h=0 w=0
panel  y=0 x=0 h=0 w=0
prompt y=0 x=0 h=0 w=0
status y=0 x=0 h=0 w=0
theme  has_color=1 colors=256 pairs=65536 wide_ok=1 status_row=4
```

### 20x5 · frame 2

```
### frame=2  20x5  bad_cells=0
--- 屏幕文本 ---
终端太小:当前 20x5,…
                    
                    
                    
                    
--- 颜色对(0-9a-z,'.'=默认对)---
....................
....................
....................
....................
....................
--- 布局 ---
gutter y=0 x=0 h=0 w=0
editor y=0 x=0 h=0 w=0
tabs   y=0 x=0 h=0 w=0
panel  y=0 x=0 h=0 w=0
prompt y=0 x=0 h=0 w=0
status y=0 x=0 h=0 w=0
theme  has_color=1 colors=256 pairs=65536 wide_ok=1 status_row=4
```

## 4. 实测抓到的真 bug:Enter 会以 `10` 到达 → 回车全失灵

pty 里真按键的那一步(第 7 项)一开始报:

```
Enter      期望=0x0000000D 实到=0x0000000A MISMATCH  | Ctrl-J  动作=None((无))
```

原因链:
1. tty 默认 `ICRNL` 打开,终端发的 CR(13)被内核翻成 LF(10);
2. ncurses 的 `raw()` **不清 ICRNL**(它只关 `ICANON|ISIG|IEXTEN` 和 `IXON|BRKINT|PARMRK`);
3. 现代 ncurses 的 `nonl()` **只影响输出**(只置 `SP->_nl`,根本不碰 termios),
   所以"nonl 让 Enter 保持 13"这个直觉是错的;
4. 于是 Enter 以 10 到达,而按跨模块约定 §18,`Ctrl-J`(10)是"一个都不许绑"的键
   (`test_keys.cpp` 会断言 `lookupAction(10) == Action::None`)—— 结果就是**回车完全没反应**。

修法(在归一化层,`ui_detail::normalizeKey`):`cp == 10 -> 13`。
与 §18 同向 —— §18 里 Ctrl-J 之所以不绑,给出的理由正是"它就是 Enter 的别名";
`lookupAction(10)` 仍然是 `None`(约定不变),只是 ui 层永不再把 10 交给上层。
`tests/test_ui.cpp::case_normalize_keys` 已钉住这条(含 `lookupAction(normalizeKey(OK,10)) == Action::Enter`)。

**建议协调者把这条追加进 `.flower/notes/跨模块约定.md`**(我没有改共享笔记的权限)。

## 5. 另一条容易踩的坑(测试侧,不是代码 bug)

方向键的字节序列必须用 **SS3** 形式 `ESC O D`,不是 CSI 形式 `ESC [ D`:
`keypad(stdscr, TRUE)` 会让 ncurses 送出 `smkx`,把终端切进 application cursor key 模式,
此时 xterm 系终端发的是 `ESC O D`(`xterm-256color` 的 `kcub1` 就是 `\EOD`)。
用 CSI 形式喂进去 ncurses 匹配不上,会退化成"裸 Esc + `[` + `D`"。
**残留风险**:若某个终端仿真器忽略 `smkx` 仍发 CSI 形式,方向键会退化成 `Alt-[` + `D`。
Terminal.app / iTerm2 / tmux / Linux 控制台都honor smkx,故按 §8.1 不额外补 CSI 表;
真遇到可用 `--doctor` 一眼看出(它会回显实际键码)。

## 6. 按键归一化实测(pty 里真敲)

```
a          期望=0x01000061 实到=0x01000061 OK  | a  键码=0x1000061  字符 U+0061  动作=None((无))
<-         期望=0x00000104 实到=0x00000104 OK  | ←  键码=0x104  动作=MoveLeft(左移一个字符)
Alt-<-     期望=0x02000104 实到=0x02000104 OK  | Alt-←  键码=0x2000104  [Alt 位]  动作=MoveWordLeft(按词左移)
Alt-1      期望=0x03000031 实到=0x03000031 OK  | Alt-1  键码=0x3000031  [Alt 位]  动作=FocusCompile(聚焦【编译】面板)
Ctrl-O     期望=0x0000000F 实到=0x0000000F OK  | Ctrl-O  键码=0xF  动作=Save(保存文件)
你        期望=0x01004F60 实到=0x01004F60 OK  | 你  键码=0x1004F60  字符 U+4F60  动作=None((无))
Enter      期望=0x0000000D 实到=0x0000000D OK  | Enter  键码=0xD  动作=Enter(换行(带自动缩进))
bare-Esc   期望=0x0000001B 实到=0x0000001B OK  | Esc  键码=0x1B  动作=Escape(依次取消:补全 / 提示行 / 面板焦点)
z          期望=0x0100007A 实到=0x0100007A OK  | z  键码=0x100007A  字符 U+007A  动作=None((无))
```

## 7. 终端状态恢复实测

在 pty 里初始化 ui(raw+noecho)后,分四种方式结束子进程,
父进程在子进程死掉之后对 **master fd** 做 `tcgetattr`,读 slave 的 `c_lflag`:

```
restore raw-exit   exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=0 ECHO=0
restore atexit     exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=1 ECHO=1
restore segv       exited=0 code=-1 signaled=1 sig=11  tcgetattr=0 ICANON=1 ECHO=1
restore terminate  exited=0 code=-1 signaled=1 sig=6  tcgetattr=0 ICANON=1 ECHO=1
restore raw-exit   exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=0 ECHO=0
restore atexit     exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=1 ECHO=1
restore segv       exited=0 code=-1 signaled=1 sig=11  tcgetattr=0 ICANON=1 ECHO=1
restore terminate  exited=0 code=-1 signaled=1 sig=6  tcgetattr=0 ICANON=1 ECHO=1
restore raw-exit   exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=0 ECHO=0
restore atexit     exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=1 ECHO=1
restore segv       exited=0 code=-1 signaled=1 sig=11  tcgetattr=0 ICANON=1 ECHO=1
restore terminate  exited=0 code=-1 signaled=1 sig=6  tcgetattr=0 ICANON=1 ECHO=1
restore raw-exit   exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=0 ECHO=0
restore atexit     exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=1 ECHO=1
restore segv       exited=0 code=-1 signaled=1 sig=11  tcgetattr=0 ICANON=1 ECHO=1
restore terminate  exited=0 code=-1 signaled=1 sig=6  tcgetattr=0 ICANON=1 ECHO=1
restore raw-exit   exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=0 ECHO=0
restore atexit     exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=1 ECHO=1
restore segv       exited=0 code=-1 signaled=1 sig=11  tcgetattr=0 ICANON=1 ECHO=1
restore terminate  exited=0 code=-1 signaled=1 sig=6  tcgetattr=0 ICANON=1 ECHO=1
restore raw-exit   exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=0 ECHO=0
restore atexit     exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=1 ECHO=1
restore segv       exited=0 code=-1 signaled=1 sig=11  tcgetattr=0 ICANON=1 ECHO=1
restore terminate  exited=0 code=-1 signaled=1 sig=6  tcgetattr=0 ICANON=1 ECHO=1
restore raw-exit   exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=0 ECHO=0
restore atexit     exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=1 ECHO=1
restore segv       exited=0 code=-1 signaled=1 sig=11  tcgetattr=0 ICANON=1 ECHO=1
restore terminate  exited=0 code=-1 signaled=1 sig=6  tcgetattr=0 ICANON=1 ECHO=1
restore raw-exit   exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=0 ECHO=0
restore atexit     exited=1 code=0 signaled=0 sig=0  tcgetattr=0 ICANON=1 ECHO=1
restore segv       exited=0 code=-1 signaled=1 sig=11  tcgetattr=0 ICANON=1 ECHO=1
restore terminate  exited=0 code=-1 signaled=1 sig=6  tcgetattr=0 ICANON=1 ECHO=1
```

- `raw-exit` 是**对照组**:故意 `_exit(0)`(既不 endwin 也绕过 atexit),
  结果 `ICANON=0 ECHO=0` —— 证明这个检测手段真的能发现"终端被留在 raw 状态"。
- 其余三条(atexit / SIGSEGV / 未捕获异常)全部 `ICANON=1 ECHO=1`,
  且 SIGSEGV 的子进程确实以 `signaled=1 sig=11` 死亡(handler 里 `endwin()` 后重新 raise,
  `SA_RESETHAND` 保证退出状态与 core dump 不被吞掉);未捕获异常经 `std::set_terminate`
  → `abort()` → `sig=6`。

## 8. 设计取舍与已知限制(需要写进 README / 交给协调者知晓)

1. **`ui.h` 只加了 4 个 private 成员变量,没改任何已有签名。**
   `HighlightCache hl_cache_` / `int hl_rev_` / `int hl_prev_cursor_line_` / `bool pending_clear_`。
   为什么要影子高亮缓存:`AppModel` 只给 `const Editor*`,而 `Editor::hlCache()` 是**非 const**
   成员(签名冻结,不能改),渲染层拿不到它,只能自备一份。
   失效判据是 `TextBuffer::revision()`,失效**起点**近似取"本帧与上一帧光标行的较小者"
   (编辑总发生在光标处)。近似失败的最坏后果只是某几行颜色暂时不对,
   `Ctrl-L`(`forceFullRedraw`)会把缓存整个丢掉重算 —— 用户手上有确定的修复手段。
2. **面板区显示哪个标签**:`AppModel` 里只有 `focus`(-1 = 编辑区),没有"面板区当前标签"这一项。
   ui 的约定:聚焦面板时画该面板;聚焦编辑区时画**第一个有未读输出**的面板,都没有未读则画【编译】。
   这样后台编译/运行完成的输出可见,又不抢焦点(§8.1 的"永不抢焦点")。
   如果协调者希望"面板区标签独立于焦点"(比如编辑时仍固定看着【运行】),
   需要往 `AppModel` 加一个 `int active_tab` —— **那要改冻结的头,我没动,留给裁决**。
3. **状态栏字段挤不下时的取舍**:优先级 徽章 > 运行中 > 文件名 > 行:列 > `F1帮助` > AI 状态 >
   构建结果/临时消息 > 语言。丢完若还剩空间,会把"最后被丢掉的那个"截断后按原次序放回
   (80 列终端上常见的效果是"上次构建结果"显示成 `2 错误…`)。`F1帮助` 固定右对齐,永不被挤掉。
4. **ghost 首行是覆盖绘制**:内联在锚点之后,会盖住该行右侧的真实文本(插入后就是那个样子);
   §8.1 要求的"不推挤任何真实内容"落在**后续行**上 —— 它们覆盖下方屏幕行,行号槽画 `~`,
   下面的真实行号/内容位置一格不动(见 frame 1 快照)。
5. **F1 浮层的边界**:浮层左右各多抹一列空白。否则边界正好落在宽字符中间时,
   那个字符的另一半会留在屏幕上变成半个乱码(ncurses 只保证被写的那一格被清掉)。
6. **`setlocale(LC_ALL, "")` 由 `main.cpp` 负责**,必须早于 `Ui::init()` 里的 `initscr()`。
   ui.cpp 不重复调,只在文件头注释里写明这是前置条件;单测与 pty 驱动自己设 locale。
7. **不自装 SIGWINCH handler**:靠 ncurses 的 `KEY_RESIZE`;`handleResize()` 只重取尺寸 +
   置 `pending_clear_`(下一帧 `clearok`)。最小可用尺寸 60x16,更小只画一行中文提示(见 20x5 快照)。
8. **`doctorReport()` 在 `initscr()` 之前调用时**拿不到 `COLORS`/`COLOR_PAIRS`
   (它们要有 SCREEN 才有效),此时退化为 `tigetnum("colors")` 并标注"尚未 initscr"。
   没有 include `term.h`:它把 `lines`/`columns` 定义成对象式宏,会打坏 `Panel::lines()`。
9. `getKeyBlockingFor` 用 `wtimeout(stdscr, ms)`,**不用 `halfdelay()`**(后者只有 100ms 粒度,
   撑不起 60ms tick 与 500ms 停顿判定)。空闲返回 `kKeyNone`(= ncurses 的 `ERR`)。

## 9. 集成观察(不是 ui.cpp 的问题,但协调者需要知道)

`make` 现在能把完整的 `./cppide` 链出来(ui.o 与 app.o/main.o/ai.o 一起链接通过)。
在 pty 里真跑 `./cppide /tmp/smoke.cpp`(80x24, TERM=xterm-256color):

- 首帧正常,字节流里能看到状态栏的"练习模式 / 写代码模式"与右端"F1帮助";
- 打字 `abc` → 状态栏出现 `*` 脏标记与 `1:2` 光标位置,渲染跟随;
- `Ctrl-X` → 提示行出现"有未保存的修改。退出?(y=退出 / n=取消 / s=保存并退出)"
  —— 说明键码归一化与提示行绘制这条链路端到端通;
- 按 `y` 之后:**master fd 上 `tcgetattr` 显示 ICANON=1 / ECHO=1**,即 `endwin()` 已经跑过、
  终端已归还(ui 这一侧的责任已完成),但**进程没有退出**:
  `/proc/<pid>/stat` 状态 `S`,单线程,`wchan = request_wait_answer`(FUSE 请求等待)。

结论:退出流程在 **Ui::shutdown 之后**卡住,卡点在文件系统调用上(本沙箱是 FUSE 挂载),
不在 ui.cpp 里。建议交给 app.cpp/main.cpp 的负责人排一下"退出前还在写什么文件"
(候选:配置/会话落盘、临时二进制清理、`curl_global_cleanup`)。
这条不影响本模块的验收标准 —— 相反,它正好又证了一次"绝不把终端留在 raw 状态":
即使进程之后挂住了,终端也已经是可用状态。

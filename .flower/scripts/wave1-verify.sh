#!/usr/bin/env bash
# desc: Wave 1 自证:头文件自给自足 + 全头合并 TU + curses 宏共存 + Mailbox 并发测试
#
# 用法:bash /work/.flower/scripts/wave1-verify.sh [--tsan]
# 全部临时文件落在 /tmp,不污染仓库。退出码 0 = 全绿。
set -u
cd /work || exit 1

CXX=${CXX:-g++}
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc"
FAIL=0
step() { printf '\n=== %s ===\n' "$1"; }
ok()   { echo "  [OK]   $1"; }
bad()  { echo "  [FAIL] $1"; FAIL=1; }

step "1. make check-headers(每个头 -fsyntax-only)"
if make check-headers; then ok "15 个头文件各自自给自足"; else bad "check-headers"; fi

step "2. 合并 TU:一次 include 全部头文件(正序 + 倒序)"
cat > /tmp/cppide_all_headers.cpp <<'EOF'
#include "app.h"
#include "ai.h"
#include "aihttp.h"
#include "build.h"
#include "config.h"
#include "editor.h"
#include "highlight.h"
#include "json.h"
#include "keys.h"
#include "mailbox.h"
#include "panel.h"
#include "proc.h"
#include "textbuf.h"
#include "ui.h"
#include "util.h"
// 倒序再来一遍:验证包含顺序无关、无循环依赖
#include "util.h"
#include "ui.h"
#include "textbuf.h"
#include "proc.h"
#include "panel.h"
#include "mailbox.h"
#include "keys.h"
#include "json.h"
#include "highlight.h"
#include "editor.h"
#include "config.h"
#include "build.h"
#include "aihttp.h"
#include "ai.h"
#include "app.h"
#include <cassert>
#include <type_traits>

// 归一化键码空间不得重叠
static_assert(Ctrl('o') == 15, "Ctrl");
static_assert(kCharBase > 0x1FF, "字符基址必须高于 ncurses KEY_* 空间");
static_assert(isChar(Char(0x4E2D)), "中文码点是可打印字符");
static_assert(charOf(Char(0x4E2D)) == 0x4E2D, "码点往返");
static_assert(!isChar(kKeyResize), "kKeyResize 不是字符");
static_assert(!isChar(Alt(Char(0x4E2D))), "Alt+字符 不是裸字符");
static_assert(isAlt(Alt(0x101)) && altBase(Alt(0x101)) == 0x101, "Alt 往返");
// 事件必须可廉价 move(Mailbox 的前提)
static_assert(std::is_move_constructible<AppEvent>::value, "AppEvent move ctor");
static_assert(std::is_move_assignable<AppEvent>::value, "AppEvent move assign");
static_assert(std::is_move_constructible<AiRequest>::value, "AiRequest move");
// §6:默认 sink 是 PanelOnly,即“默认不可被接受”
static_assert(static_cast<int>(AiSink::PanelOnly) == 0, "默认 sink");

int main() {
  Ghost g;            assert(g.sink == AiSink::PanelOnly && !g.complete);
  AppEvent ev;        assert(ev.kind == EvKind::Status && ev.sink == AiSink::PanelOnly);
  ProcResult pr;      assert(!pr.ok());
  Range r{Pos{2, 0}, Pos{1, 0}};  assert(r.normalized().a.line == 1);
  Mailbox<AppEvent> mb;           assert(mb.size() == 0 && !mb.closed());
  Config c;           assert(!c.aiEnabled());
  Layout L;           assert(L.editor.empty());
  (void)sizeof(AppModel); (void)sizeof(Theme); (void)sizeof(Span);
  return 0;
}
EOF
if $CXX $FLAGS -c -o /tmp/cppide_all_headers.o /tmp/cppide_all_headers.cpp; then
  ok "全头合并 TU 编译通过(无冲突、无循环)"
else
  bad "全头合并 TU 编译失败"
fi

step "3. curses 宏共存:ui.cpp 的写法(NCURSES_NOMACROS + curses.h + 全部头)"
cat > /tmp/cppide_curses_mix.cpp <<'EOF'
// ui.cpp 将采用的写法:必须先 NCURSES_NOMACROS,否则 curses 的函数式宏
// (clear/erase/move/refresh/timeout/scroll/border...)会把成员调用打坏。
#define NCURSES_NOMACROS 1
#include <curses.h>
#include "app.h"
#include "ui.h"
#include "panel.h"
#include "textbuf.h"
int main() {
  TextBuffer b;
  Panel p(PanelId::Compile);
  p.clear();                                  // 若 clear() 宏未被抑制,这里会炸
  (void)b.textRange(Range{Pos{0,0}, Pos{0,0}});
  (void)ERR; (void)KEY_RESIZE; (void)A_DIM;
  return 0;
}
EOF
if $CXX $FLAGS -fsyntax-only /tmp/cppide_curses_mix.cpp 2>/tmp/cppide_curses_mix.log; then
  ok "NCURSES_NOMACROS 下 curses.h 与项目头共存"
else
  bad "curses 宏冲突(见 /tmp/cppide_curses_mix.log)"; sed -n 1,20p /tmp/cppide_curses_mix.log
fi
# 反证:不加 NCURSES_NOMACROS 应当失败(说明这条注意事项是真的必要)
grep -v NCURSES_NOMACROS /tmp/cppide_curses_mix.cpp > /tmp/cppide_curses_bad.cpp
if $CXX $FLAGS -fsyntax-only /tmp/cppide_curses_bad.cpp 2>/dev/null; then
  echo "  [注意] 本机不加 NCURSES_NOMACROS 也能编过(但仍不要这么写)"
else
  ok "反证成立:不加 NCURSES_NOMACROS 确实编不过(scroll/clear/erase 被宏打坏)"
fi

step "4. Mailbox 并发测试(两生产者 push / 消费者 tryPop / close 后排空)"
cat > /tmp/cppide_mailbox_test.cpp <<'EOF'
#include "mailbox.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

struct Msg { int who; int seq; std::string payload; };

int main() {
  const int kPerThread = 20000;
  Mailbox<Msg> mb;
  std::atomic<int> pushed{0};

  auto producer = [&](int who) {
    for (int i = 0; i < kPerThread; ++i) {
      mb.push(Msg{who, i, std::string(8, 'a' + who)});
      pushed.fetch_add(1, std::memory_order_relaxed);
    }
  };
  std::thread p1(producer, 0), p2(producer, 1);

  // 模拟 UI 线程:只允许 tryPop,永不阻塞
  int got = 0, last[2] = {-1, -1};
  auto consume_available = [&] {
    Msg m;
    while (mb.tryPop(m)) {
      assert(m.who == 0 || m.who == 1);
      assert(m.seq == last[m.who] + 1);   // 同一生产者的 FIFO 顺序必须保持
      last[m.who] = m.seq;
      ++got;
    }
  };
  while (got < 2 * kPerThread) {
    consume_available();
    if (got < 2 * kPerThread) std::this_thread::yield();
  }
  p1.join(); p2.join();
  assert(pushed.load() == 2 * kPerThread);
  assert(got == 2 * kPerThread);
  printf("  tryPop 收全 %d 条,且两条流各自 FIFO 有序\n", got);

  // waitPop 的超时语义:空队列 + 未 close -> 等满超时返回 false
  Mailbox<Msg> mb2;
  Msg m;
  auto t0 = std::chrono::steady_clock::now();
  bool r = mb2.waitPop(m, 120);
  int waited = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  assert(!r && waited >= 100);
  printf("  waitPop 空队列超时返回 false(等了 %dms)\n", waited);

  // close() 必须立刻唤醒阻塞中的 waitPop
  std::thread waiter([&] {
    Msg x;
    bool ok = mb2.waitPop(x, -1);     // 无限等
    assert(!ok);                      // 被 close 唤醒,返回 false
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  mb2.close();
  waiter.join();
  printf("  close() 唤醒了无限等待的 waitPop\n");

  // close 后:push 被丢弃,但残余仍可被 tryPop 排空
  Mailbox<Msg> mb3;
  for (int i = 0; i < 5; ++i) mb3.push(Msg{0, i, "x"});
  mb3.close();
  mb3.push(Msg{0, 99, "dropped"});
  int drained = 0;
  while (mb3.tryPop(m)) { assert(m.seq == drained); ++drained; }
  assert(drained == 5);
  assert(mb3.closed() && mb3.empty());
  printf("  close 后排空到 %d 条,且 close 后的 push 被丢弃\n", drained);

  // 多消费者 + 生产者混跑一轮,交给 TSan 判有没有数据竞争
  {
    Mailbox<int> q;
    std::atomic<int> sum{0}, taken{0};
    const int N = 5000;
    std::thread prod([&] { for (int i = 1; i <= N; ++i) q.push(i); q.close(); });
    std::vector<std::thread> cons;
    for (int c = 0; c < 3; ++c)
      cons.emplace_back([&] {
        int v;
        while (true) {
          if (q.waitPop(v, 50)) { sum += v; taken++; }
          else if (q.closed() && q.empty()) break;
        }
      });
    prod.join();
    for (auto& t : cons) t.join();
    assert(taken.load() == N);
    assert(sum.load() == N * (N + 1) / 2);
    printf("  3 消费者 waitPop 全取 %d 条,和校验通过\n", taken.load());
  }
  printf("mailbox: ALL PASS\n");
  return 0;
}
EOF
if $CXX $FLAGS -pthread -o /tmp/cppide_mailbox_test /tmp/cppide_mailbox_test.cpp; then
  if /tmp/cppide_mailbox_test; then ok "Mailbox 功能测试通过"; else bad "Mailbox 功能测试失败"; fi
else
  bad "Mailbox 测试编译失败"
fi

step "5. Mailbox 消毒器复跑(TSan 优先,不可用则退回 ASan+UBSan 并压测)"
TSAN_OK=0
if $CXX -std=c++17 -O1 -g -fsanitize=thread -Isrc -pthread \
      -o /tmp/cppide_mailbox_tsan /tmp/cppide_mailbox_test.cpp 2>/tmp/cppide_tsan_build.log; then
  if /tmp/cppide_mailbox_tsan > /tmp/cppide_tsan_run.log 2>&1; then
    if grep -q "WARNING: ThreadSanitizer" /tmp/cppide_tsan_run.log; then
      bad "TSan 报告数据竞争(见 /tmp/cppide_tsan_run.log)"
    else
      ok "TSan 干净(无数据竞争)"; TSAN_OK=1
    fi
  elif grep -q "unexpected memory mapping" /tmp/cppide_tsan_run.log; then
    # 容器里 vm.mmap_rnd_bits=33 超出 TSan 的影子内存假设,且无权 sysctl / personality。
    echo "  [跳过] 本机 TSan 无法运行(ASLR 位数过大:$(cat /proc/sys/vm/mmap_rnd_bits 2>/dev/null))"
  else
    bad "TSan 运行失败(见 /tmp/cppide_tsan_run.log)"; tail -5 /tmp/cppide_tsan_run.log
  fi
else
  echo "  [跳过] 本机无 TSan(见 /tmp/cppide_tsan_build.log)"
fi
if [ $TSAN_OK -eq 0 ]; then
  if $CXX -std=c++17 -O1 -g -fsanitize=address,undefined -Isrc -pthread \
        -o /tmp/cppide_mailbox_asan /tmp/cppide_mailbox_test.cpp 2>/dev/null; then
    n=0
    while [ $n -lt 20 ]; do
      if ! /tmp/cppide_mailbox_asan > /tmp/cppide_asan_run.log 2>&1; then
        bad "ASan/UBSan 第 $((n+1)) 轮失败(见 /tmp/cppide_asan_run.log)"; break
      fi
      n=$((n+1))
    done
    [ $n -eq 20 ] && ok "ASan+UBSan 连跑 20 轮干净(替代 TSan)"
  else
    bad "ASan 版本编译失败"
  fi
fi

step "汇总"
if [ $FAIL -eq 0 ]; then echo "全部通过"; else echo "有失败项"; fi
exit $FAIL

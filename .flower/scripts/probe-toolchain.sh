# desc: 探测/验证 ncurses+libcurl+c++17 工具链是否可用
#!/usr/bin/env bash
# Probe & verify that this container can compile+link a C++17 program using
# ncurses (wide-char) + libcurl. Safe to re-run. Never blocks: all runs use timeout.
#
# Usage:  bash /work/.flower/scripts/probe-toolchain.sh [--install]
#   --install : apt-get install missing dev packages (needs network + root)
#
# Exit 0 = full toolchain verified (compile, link, and runtime smoke test).

set -uo pipefail

SRC=/tmp/probe-toolchain-src
OUT=/tmp/probe-toolchain-out
mkdir -p "$SRC" "$OUT"

PASS=0; FAIL=0
ok()   { echo "  [ OK ] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }
hdr()  { echo; echo "=== $* ==="; }

# ---------------------------------------------------------------- 0. install
if [ "${1:-}" = "--install" ]; then
  hdr "apt-get install toolchain"
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    g++ make pkg-config libncurses-dev libcurl4-gnutls-dev curl
fi

# ---------------------------------------------------------------- 1. tools
hdr "1. compilers / build tools"
for t in g++ clang++ make pkg-config curl-config curl; do
  if command -v "$t" >/dev/null 2>&1; then
    ok "$t -> $(command -v "$t")"
  else
    echo "  [ -- ] $t not present"
  fi
done
command -v g++ >/dev/null 2>&1 || { echo "FATAL: no g++"; exit 1; }
echo "  g++ version: $(g++ -dumpfullversion 2>/dev/null || g++ -dumpversion)"
echo "  target arch: $(g++ -dumpmachine)"

hdr "2. -std=c++17 support"
cat > "$SRC/cxx17.cpp" <<'EOF'
#include <optional>
#include <string_view>
#include <variant>
static_assert(__cplusplus >= 201703L, "not C++17");
int main() {
    std::optional<int> o = 42;
    std::string_view sv = "hi";
    if (auto [a, b] = std::pair{1, 2}; a < b) return o.value() > 0 && !sv.empty() ? 0 : 1;
    return 1;
}
EOF
if g++ -std=c++17 -Wall -Wextra "$SRC/cxx17.cpp" -o "$OUT/cxx17" 2>"$OUT/cxx17.log"; then
  timeout 10 "$OUT/cxx17" && ok "-std=c++17 compiles and runs" || bad "c++17 binary bad exit"
else
  bad "-std=c++17 compile failed (see $OUT/cxx17.log)"
fi

# ---------------------------------------------------------------- 3. pkg-config
hdr "3. pkg-config / *-config discovery"
for m in ncurses ncursesw tinfo libcurl; do
  if pkg-config --exists "$m" 2>/dev/null; then
    ok "$m $(pkg-config --modversion "$m") :: $(pkg-config --cflags --libs "$m")"
  else
    echo "  [ -- ] pkg-config module '$m' absent"
  fi
done
command -v curl-config >/dev/null 2>&1 && \
  echo "  curl-config: $(curl-config --version) | cflags='$(curl-config --cflags)' libs='$(curl-config --libs)'"

hdr "4. headers"
for h in ncurses.h curses.h ncursesw/curses.h curl/curl.h; do
  printf '#include <%s>\nint main(){return 0;}\n' "$h" > "$SRC/h.cpp"
  if g++ -std=c++17 -fsyntax-only "$SRC/h.cpp" 2>/dev/null; then
    ok "<$h> found on default include path"
  else
    echo "  [ -- ] <$h> NOT on default path"
  fi
done

# ---------------------------------------------------------------- 5. sources
# Two separate translation units, deliberately, to prove real multi-TU linking.
cat > "$SRC/tu_ui.cpp" <<'EOF'
// TU 1: ncurses (wide-char capable). Non-interactive: never calls getch().
#define _XOPEN_SOURCE_EXTENDED 1
#include <ncursesw/curses.h>
#include <clocale>
#include <cstdio>
#include <cwchar>

const char* ui_curses_version() { return curses_version(); }

// Link-only probe: references the symbols we care about without needing a tty.
int ui_symbol_probe() {
    void* syms[] = {
        (void*)&initscr,  (void*)&endwin,   (void*)&newterm,
        (void*)&printw,   (void*)&refresh,  (void*)&cbreak,
        (void*)&noecho,   (void*)&keypad,   (void*)&start_color,
        (void*)&init_pair,(void*)&mvaddwstr,(void*)&get_wch,
        (void*)&wattr_on, (void*)&resizeterm,
    };
    int n = 0;
    for (void* s : syms) if (s) ++n;
    return n;
}

// Real ncurses session. Only reached when a terminal is available.
int ui_render_once() {
    std::setlocale(LC_ALL, "");
    SCREEN* sc = newterm(nullptr, stdout, stdin);
    if (!sc) return -1;
    set_term(sc);
    cbreak(); noecho(); curs_set(0);
    printw("ncurses ok: %s  %dx%d\n", curses_version(), LINES, COLS);
    const wchar_t* wide = L"wide-char: 你好 ─│┌ ✓";
    mvaddwstr(1, 0, wide);
    refresh();
#ifndef NDEBUG
    getch();          // interactive only in debug builds
#endif
    endwin();
    delscreen(sc);
    return 0;
}
EOF

cat > "$SRC/tu_net.cpp" <<'EOF'
// TU 2: libcurl. Does no network I/O -- init/cleanup only.
#include <curl/curl.h>
#include <cstdio>
#include <string>

std::string net_curl_version() { return curl_version(); }

int net_probe() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 1;
    CURL* h = curl_easy_init();
    if (!h) { curl_global_cleanup(); return 2; }
    int rc = 0;
    if (curl_easy_setopt(h, CURLOPT_URL, "https://example.invalid") != CURLE_OK) rc = 3;
    if (curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 1L) != CURLE_OK) rc = 4;
    if (curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, nullptr) != CURLE_OK) rc = 5;
    curl_easy_cleanup(h);
    curl_global_cleanup();
    return rc;
}
EOF

cat > "$SRC/main.cpp" <<'EOF'
#include <cstdio>
#include <cstring>
#include <string>
const char* ui_curses_version();
int ui_symbol_probe();
int ui_render_once();
std::string net_curl_version();
int net_probe();

int main(int argc, char** argv) {
    bool render = (argc > 1 && std::strcmp(argv[1], "--render") == 0);
    std::printf("curses_version = %s\n", ui_curses_version());
    std::printf("curl_version   = %s\n", net_curl_version().c_str());
    int n = ui_symbol_probe();
    std::printf("ncurses symbols resolved = %d\n", n);
    int rc = net_probe();
    std::printf("libcurl easy init/cleanup rc = %d\n", rc);
    if (n < 14 || rc != 0) return 1;
    if (render) {
        int r = ui_render_once();
        std::printf("render rc = %d\n", r);
        return r == 0 ? 0 : 2;
    }
    return 0;
}
EOF

# ---------------------------------------------------------------- 6. link
hdr "5. compile + link (multi-TU, ncursesw + libcurl)"
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -DNDEBUG $(pkg-config --cflags ncursesw libcurl 2>/dev/null)"
LDLIBS="$(pkg-config --libs ncursesw libcurl 2>/dev/null)"
[ -n "$LDLIBS" ] || LDLIBS="-lncursesw -ltinfo -lcurl"
echo "  CXXFLAGS = $CXXFLAGS"
echo "  LDLIBS   = $LDLIBS"
if g++ $CXXFLAGS "$SRC/tu_ui.cpp" "$SRC/tu_net.cpp" "$SRC/main.cpp" \
       -o "$OUT/probe" $LDLIBS 2>"$OUT/link.log"; then
  ok "linked -> $OUT/probe"
  if [ -s "$OUT/link.log" ]; then echo "  (warnings in $OUT/link.log)"; fi
else
  bad "link failed (see $OUT/link.log)"; sed -n '1,40p' "$OUT/link.log"
fi

hdr "6. alternate link names"
for L in "-lncurses -ltinfo" "-lncursesw -ltinfo" "-lncurses" "-lncursesw"; do
  if g++ -std=c++17 -DNDEBUG -w "$SRC/tu_ui.cpp" "$SRC/tu_net.cpp" "$SRC/main.cpp" \
        -o "$OUT/alt" $L -lcurl 2>/dev/null; then
    ok "links with: $L -lcurl"
  else
    echo "  [ -- ] does NOT link with: $L -lcurl"
  fi
done

# ---------------------------------------------------------------- 6b. sublibs
hdr "6b. ncurses sublibraries (menuw/formw/panelw)"
cat > "$SRC/sub.cpp" <<'EOF'
#define _XOPEN_SOURCE_EXTENDED 1
#include <ncursesw/curses.h>
#include <ncursesw/panel.h>
#include <ncursesw/menu.h>
#include <ncursesw/form.h>
int main() {
    void* s[] = {(void*)&new_panel, (void*)&new_menu, (void*)&new_form, (void*)&update_panels};
    int n = 0; for (void* p : s) if (p) ++n;
    return n == 4 ? 0 : 1;
}
EOF
SUBC="$(pkg-config --cflags ncursesw menuw formw panelw 2>/dev/null)"
SUBL="$(pkg-config --libs menuw formw panelw ncursesw 2>/dev/null)"
[ -n "$SUBL" ] || SUBL="-lmenuw -lformw -lpanelw -lncursesw -ltinfo"
if g++ -std=c++17 -DNDEBUG $SUBC "$SRC/sub.cpp" -o "$OUT/sub" $SUBL 2>"$OUT/sub.log"; then
  if TERM=dumb timeout 10 "$OUT/sub"; then ok "menu/form/panel link+run OK :: $SUBL"
  else bad "sublib binary bad exit"; fi
else
  echo "  [ -- ] menu/form/panel not linkable (see $OUT/sub.log)"
fi

# ---------------------------------------------------------------- 7. run
hdr "7. runtime smoke test (non-interactive, TERM=dumb, timeout 10)"
if [ -x "$OUT/probe" ]; then
  if TERM=dumb timeout 10 "$OUT/probe" > "$OUT/run.log" 2>&1; then
    ok "ran clean (exit 0)"
  else
    bad "run exit=$? -- see $OUT/run.log"
  fi
  sed 's/^/  | /' "$OUT/run.log"
fi

hdr "8. runtime ncurses render under a real pty (timeout 10)"
if [ -x "$OUT/probe" ] && command -v python3 >/dev/null 2>&1; then
  TERM=xterm-256color timeout 10 python3 - "$OUT/probe" <<'PY' > "$OUT/pty.log" 2>&1
import os, pty, sys
rc = pty.spawn([sys.argv[1], "--render"])
sys.exit(os.waitstatus_to_exitcode(rc) if hasattr(os, "waitstatus_to_exitcode") else rc)
PY
  if [ $? -eq 0 ]; then ok "ncurses newterm/render works under pty"
  else echo "  [ -- ] pty render non-zero (see $OUT/pty.log) -- link is still proven"; fi
  sed -n '1,10p' "$OUT/pty.log" | cat -v | sed 's/^/  | /'
fi

# ---------------------------------------------------------------- 9. make
hdr "9. make"
cat > "$SRC/Makefile" <<'EOF'
CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -DNDEBUG $(shell pkg-config --cflags ncursesw libcurl)
LDLIBS   := $(shell pkg-config --libs ncursesw libcurl)
OBJS     := tu_ui.o tu_net.o main.o
probe-make: $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@ $(LDLIBS)
clean:
	rm -f $(OBJS) probe-make
EOF
if ( cd "$SRC" && timeout 120 make -B >"$OUT/make.log" 2>&1 ); then
  ok "make build works ($SRC/probe-make)"
else
  bad "make failed (see $OUT/make.log)"
fi

# ---------------------------------------------------------------- 10. network
hdr "10. network reachability (probe only, no credentials)"
for host in https://api.deepseek.com https://api.openai.com https://github.com; do
  if command -v curl >/dev/null 2>&1; then
    code=$(timeout 10 curl -s -m 5 -o /dev/null -w '%{http_code}' "$host" 2>/dev/null)
    [ -n "$code" ] && [ "$code" != "000" ] && ok "$host -> HTTP $code" || echo "  [ -- ] $host unreachable"
  fi
done

hdr "SUMMARY"
echo "  passed=$PASS failed=$FAIL"
echo "  sources: $SRC    binaries/logs: $OUT"
[ "$FAIL" -eq 0 ] && echo "  RESULT: ncurses + libcurl + C++17 toolchain VERIFIED" \
                  || echo "  RESULT: $FAIL check(s) failed"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)

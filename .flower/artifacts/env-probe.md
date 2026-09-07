# 构建环境探测报告 — ncurses + libcurl + C++17

- 探测日期: 2026-09-06
- 容器: Debian GNU/Linux 13 (trixie), **aarch64 / arm64**, root
- 复现脚本: `/work/.flower/scripts/probe-toolchain.sh`(可重复运行,幂等)
- 本次运行结果: **passed=22 failed=0 → 工具链已验证**(脚本重跑两次均 exit 0)

---

## 1. 结论(TL;DR)

**能。** 容器内可以完整编译并链接一个 ncurses + libcurl 的 C++17 程序,
多 TU 分开编译再链接、`make` 构建、运行期 `initscr`/`newterm` 渲染(pty 下)、
`curl_easy_init/cleanup` 全部通过。

| 项目 | 结论 |
|---|---|
| 编译器 | `g++` (Debian 14.2.0-19) 14.2.0,target `aarch64-linux-gnu`。**无 clang++** |
| `-std=c++17` | 支持(`__cplusplus >= 201703L` static_assert 通过,含结构化绑定/optional/string_view) |
| ncurses | 6.5.20250216,头文件 + 库齐备 |
| ncurses 链接名 | **必须 `-lncursesw`**(见 §4,`-lncurses` 无宽字符符号) |
| libcurl | 8.14.1,**GnuTLS** flavour |
| libcurl 链接参数 | `-lcurl`(pkg-config 与 curl-config 都给这个) |
| `make` | GNU Make 4.4.1,可用 |
| `pkg-config` | 1.8.1,`ncurses` / `ncursesw` / `tinfo` / `libcurl` 全部有 `.pc` |
| 联网 | **api.deepseek.com 可达(HTTP 401)**;api.openai.com / github.com 不可达 |

### 初始状态 vs 安装后

探测开始时容器里**什么都没有**:无 `g++`/`gcc`/`clang++`/`make`/`pkg-config`/`curl`,
无任何开发头文件。只有运行时共享库(`libncursesw.so.6`、`libcurl-gnutls.so.4`、`libtinfo.so.6`)。

本次通过 apt 安装(`apt-get update` 走 mirrors.ustc.edu.cn,成功):

```
apt-get install -y --no-install-recommends \
    g++ make pkg-config libncurses-dev libcurl4-gnutls-dev curl
```

带入的关键包:`g++ 4:14.2.0-1`、`g++-14`、`gcc-14`、`libc6-dev`、`make`、`pkg-config`、
`libncurses-dev 6.5+20250216-2`、`libcurl4-gnutls-dev 8.14.1-2+deb13u4`、`curl 8.14.1-2+deb13u4`,
以及依赖 `libgnutls28-dev`、`libkrb5-dev`、`libssh2-1-dev`、`librtmp-dev`、`zlib1g-dev`、`libidn2-dev` 等。

> 注意:如果容器被重建/重置,需要**重新跑一次安装**:
> `bash /work/.flower/scripts/probe-toolchain.sh --install`

---

## 2. 推荐的编译参数

pkg-config 实测输出(照抄即可):

```
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra $(pkg-config --cflags ncursesw libcurl)
         # 展开为: -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600
         #          -I/usr/include/aarch64-linux-gnu
         #          -isystem /usr/include/mit-krb5 -I/usr/include/p11-kit-1
LDLIBS   = $(pkg-config --libs ncursesw libcurl)
         # 展开为: -lncursesw -ltinfo -lcurl
```

不用 pkg-config 的等价写法:`-lncursesw -ltinfo -lcurl`(`-ltinfo` 可省,见 §4)。

头文件全部在**默认搜索路径**上,实测 `-fsyntax-only` 通过:
`<ncurses.h>`(→ `curses.h` 符号链接)、`<curses.h>`、`<ncursesw/curses.h>`、`<curl/curl.h>`。

> `curl/curl.h` 的物理位置是 `/usr/include/aarch64-linux-gnu/curl/curl.h`(Debian multiarch),
> 但 gcc 默认已包含该 multiarch 目录,所以**不加 `-I` 也能 include**。
> 尽管如此仍建议用 `pkg-config --cflags libcurl`,别的发行版不一定这样。

---

## 3. 宽字符 (ncursesw) 可用性

**可用,已实测渲染成功。** pty 下运行的验证程序里:

```cpp
#define _XOPEN_SOURCE_EXTENDED 1
#include <ncursesw/curses.h>
std::setlocale(LC_ALL, "");
const wchar_t* wide = L"wide-char: 你好 ─│┌ ✓";
mvaddwstr(1, 0, wide);
```

pty 输出中确认了 CJK 与制表符/勾号的 UTF-8 字节序列被正确写出
(`M-dM-=M- M-eM-%M-=` = 你好,`M-bM-^TM-^@` = ─,`M-bM-^\M-^S` = ✓),`render rc = 0`。

`ncursesw` 的 `.pc` 存在,版本 6.5.20250216。

---

## 4. 关键坑:`-lncurses` vs `-lncursesw`

**在这个容器里必须用 `-lncursesw`。** 实测矩阵(同一份宽字符 TU):

| 链接参数 | 结果 |
|---|---|
| `-lncurses -ltinfo -lcurl` | **链接失败** |
| `-lncurses -lcurl` | **链接失败** |
| `-lncursesw -ltinfo -lcurl` | OK |
| `-lncursesw -lcurl` | OK |

原因已用 `nm -D --defined-only` 定位 —— 两个库都存在,但宽字符符号只在 w 版里:

```
libncurses.so.6 : add_wch=0 mvaddwstr=0 get_wch=0 wget_wch=0
libncursesw.so.6: add_wch=1 mvaddwstr=1 get_wch=1 wget_wch=1
```

补充事实:
- 窄字符程序(只用 `initscr/printw/refresh/endwin`)用 `-lncurses` **是能链接的**,
  所以"`-lncurses` 完全不可用"的说法不对 —— 是**宽字符 API 不可用**。
- `-ltinfo` 可省:`libncursesw.so` 是个 linker script,内容为
  `INPUT(libncursesw.so.6 -ltinfo)`,已自动带上 tinfo。显式写上更保险。
- `libncurses-dev` 一个包同时提供 `libncurses.so` 和 `libncursesw.so`,不需要额外装 `libncursesw-dev`。

---

## 5. 验证程序结构

源码在 `/tmp/probe-toolchain-src/`(由脚本每次重新生成,**不留在 /work**):

- `tu_ui.cpp` — TU1,ncursesw。`ui_symbol_probe()` 取 14 个函数地址(含 `mvaddwstr`/`get_wch`/`resizeterm`)
  证明符号真被解析;`ui_render_once()` 用 `newterm(nullptr, stdout, stdin)` 起真 session。
  `getch()` 包在 `#ifndef NDEBUG` 里 —— **`-DNDEBUG` 构建绝不进入交互**。
- `tu_net.cpp` — TU2,libcurl。`curl_global_init` → `curl_easy_init` → 三个 `setopt` → `cleanup`,
  **不发任何网络请求**,URL 写死 `https://example.invalid` 且 `CURLOPT_TIMEOUT_MS=1`。
- `main.cpp` — 链接两者,默认非交互;`--render` 才进 ncurses 渲染。

产物与日志在 `/tmp/probe-toolchain-out/`。

运行期实测输出:

```
curses_version = ncurses 6.5.20250216
curl_version   = libcurl/8.14.1 GnuTLS/3.8.9 zlib/1.3.1 brotli/1.1.0 zstd/1.5.7
                 libidn2/2.3.8 libpsl/0.21.2 libssh2/1.11.1 nghttp2/1.64.0
                 ngtcp2/1.11.0 nghttp3/1.8.0 librtmp/2.3 OpenLDAP/2.6.10
ncurses symbols resolved = 14
libcurl easy init/cleanup rc = 0
```

`make -B`(pkg-config 驱动的 Makefile,3 个 .o 分开编译再链接)同样通过。

### 防挂死措施
- 所有运行都套 `timeout 10`(make 用 `timeout 120`,apt 用更长)。
- 默认路径不调 `getch()`;交互只在 debug 构建 + 显式 `--render` 下发生。
- 无 tty 的场景用 `TERM=dumb` 跑非交互路径(exit 0)。
- 需要真 tty 的渲染测试走 `python3 -c "pty.spawn(...)"` + `TERM=xterm-256color`,外层仍有 timeout。

---

## 6. 联网

`curl -s -m 5 -o /dev/null -w '%{http_code}'`,**未携带任何 key**:

| 目标 | 结果 |
|---|---|
| `https://api.deepseek.com` | **HTTP 401** → DNS + TLS + HTTP 全通,可达 |
| `https://api.openai.com` | 不可达(`000`) |
| `https://github.com` | 不可达(`000`) |
| Debian 镜像 mirrors.ustc.edu.cn | 通(apt 成功拉了 10.1 MB) |

401 = 服务端正常应答但缺鉴权,正是"网络通"的证据。
另外 `python3 urllib` 独立复现了同一个 401(curl 装好之前用它探的)。

---

## 7. 对 macOS 目标平台的影响(重要,**未在本容器验证**)

本容器是 **aarch64 Linux**,**不能**产出 macOS 二进制:没有 clang、没有 macOS SDK、
没有 osxcross。所以容器的作用只能是:

1. **语法 / 类型 / C++17 合规性检查** —— 有效。
2. **ncurses 与 libcurl API 用法是否正确** —— 大体有效,但注意版本差异。
3. **最终 macOS 产物验证** —— 必须在真 mac 上做,容器给不了。

已知的 Linux↔macOS 差异(基于既有知识,**本次没有 mac 可测,属未验证**):

- macOS 的 `libncurses.dylib` 是**宽字符编译的**,且 `libncursesw.dylib` 通常是它的符号链接;
  因此 mac 上 `-lncurses` 一般就够,而本容器必须 `-lncursesw`。
  → **建议 Makefile 按平台分支**,或用 `pkg-config --libs ncursesw`(mac 上若无 .pc 则 fallback)。
- macOS 系统 ncurses 版本较旧(5.7 时代),某些较新 API / 扩展可能缺失;
  容器里是 6.5,**容器能编过不等于 mac 能编过**。这是最主要的风险点。
- macOS 有 `curl-config` 但通常**没有** `libcurl.pc`;`pkg-config libcurl` 在裸 mac 上可能失败。
  → 构建脚本应优先 `curl-config --cflags/--libs`,再退回 pkg-config。
- 本容器 libcurl 是 **GnuTLS** flavour;macOS 是 Secure Transport / LibreSSL。
  证书与 TLS 后端相关的行为(CA 路径、`CURLOPT_CAINFO`)不可照搬。
- `<ncursesw/curses.h>` 这个路径是 Debian 特有;macOS 上应写 `<curses.h>` 或 `<ncurses.h>`。
  → **代码里统一 `#include <curses.h>`**,靠 `-I$(pkg-config --cflags ncursesw)` 提供正确路径,可移植性最好。

---

## 8. 未验证 / 风险清单

- 未验证任何 macOS 行为(容器内无 mac 工具链)。§7 全部是知识推断。
- 未验证 `libcurl` 的实际 HTTPS 传输(只做 init/cleanup;仅用命令行 curl 探了可达性)。
- ~~未验证 ncurses 子库~~ → **已验证**:`menuw` / `formw` / `panelw` 三个 `.pc` 都存在(6.5.20250216),
  且实测链接 + 运行通过(`new_panel`/`new_menu`/`new_form`/`update_panels` 符号全部解析,exit 0)。
  参数:`$(pkg-config --libs menuw formw panelw ncursesw)` = `-lmenuw -lformw -lpanelw -lncursesw -ltinfo`
  (顺序重要:子库必须在 `-lncursesw` **之前**)。头文件路径 `<ncursesw/panel.h>` 等。
  注意 `tinfow` 的 `.pc` **不存在**,只有 `tinfo`。
- 未安装 clang++。若产品要求 clang 兼容性(macOS 用 Apple clang),
  建议 `apt-get install clang` 后再跑一遍 —— **g++ 编过不代表 clang 编过**(尤其模板与两阶段查找)。
- 容器非持久假设:重建后需 `--install` 重跑。
- `apt-get update` 依赖 mirrors.ustc.edu.cn 可达;若换网络环境可能失败。

---

## 附录:完整版本输出

```
### g++ --version
g++ (Debian 14.2.0-19) 14.2.0
Copyright (C) 2024 Free Software Foundation, Inc.
This is free software; see the source for copying conditions.  There is NO
warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.


### g++ -v (full config)
Using built-in specs.
COLLECT_GCC=g++
COLLECT_LTO_WRAPPER=/usr/libexec/gcc/aarch64-linux-gnu/14/lto-wrapper
OFFLOAD_TARGET_NAMES=nvptx-none
OFFLOAD_TARGET_DEFAULT=1
Target: aarch64-linux-gnu
Configured with: ../src/configure -v --with-pkgversion='Debian 14.2.0-19' --with-bugurl=file:///usr/share/doc/gcc-14/README.Bugs --enable-languages=c,ada,c++,go,d,fortran,objc,obj-c++,m2,rust --prefix=/usr --with-gcc-major-version-only --program-suffix=-14 --program-prefix=aarch64-linux-gnu- --enable-shared --enable-linker-build-id --libexecdir=/usr/libexec --without-included-gettext --enable-threads=posix --libdir=/usr/lib --enable-nls --enable-bootstrap --enable-clocale=gnu --enable-libstdcxx-debug --enable-libstdcxx-time=yes --with-default-libstdcxx-abi=new --enable-libstdcxx-backtrace --enable-gnu-unique-object --disable-libquadmath --disable-libquadmath-support --enable-plugin --enable-default-pie --with-system-zlib --enable-libphobos-checking=release --with-target-system-zlib=auto --enable-objc-gc=auto --enable-multiarch --enable-fix-cortex-a53-843419 --disable-werror --enable-offload-targets=nvptx-none=/build/reproducible-path/gcc-14-14.2.0/debian/tmp-nvptx/usr --enable-offload-defaulted --without-cuda-driver --enable-checking=release --build=aarch64-linux-gnu --host=aarch64-linux-gnu --target=aarch64-linux-gnu --with-build-config=bootstrap-lto-lean --enable-link-serialization=2
Thread model: posix
Supported LTO compression algorithms: zlib zstd
gcc version 14.2.0 (Debian 14.2.0-19) 

### g++ -dumpmachine
aarch64-linux-gnu

### make --version
GNU Make 4.4.1
Built for aarch64-unknown-linux-gnu
Copyright (C) 1988-2023 Free Software Foundation, Inc.
License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>
This is free software: you are free to change and redistribute it.
There is NO WARRANTY, to the extent permitted by law.

### pkg-config --version
1.8.1

### curl-config --version / --cflags / --libs / --static-libs
libcurl 8.14.1
cflags: []
libs: [-lcurl]
static-libs: [-Wl,-Bstatic -lcurl -Wl,-Bdynamic -lnghttp3 -lngtcp2_crypto_gnutls -lngtcp2 -lnghttp2 -lidn2 -lrtmp -lssh2 -lssh2 -lpsl -lnettle -lgnutls -lgssapi_krb5 -llber -lldap -llber -lzstd -lbrotlidec -lz]

### curl-config --feature
alt-svc
AsynchDNS
brotli
GSS-API
HSTS
HTTP2
HTTP3
HTTPS-proxy
IDN
IPv6
Kerberos
Largefile
libz
NTLM
PSL
SPNEGO
SSL
threadsafe
TLS-SRP
UnixSockets
zstd

### curl-config --protocols
DICT FILE FTP FTPS GOPHER GOPHERS HTTP HTTPS IMAP IMAPS IPFS IPNS LDAP LDAPS MQTT POP3 POP3S RTMP RTSP SCP SFTP SMB SMBS SMTP SMTPS TELNET TFTP WS WSS 

### curl --version
curl 8.14.1 (aarch64-unknown-linux-gnu) libcurl/8.14.1 OpenSSL/3.5.7 zlib/1.3.1 brotli/1.1.0 zstd/1.5.7 libidn2/2.3.8 libpsl/0.21.2 libssh2/1.11.1 nghttp2/1.64.0 nghttp3/1.8.0 librtmp/2.3 OpenLDAP/2.6.10
Release-Date: 2025-06-04, security patched: 8.14.1-2+deb13u4
Protocols: dict file ftp ftps gopher gophers http https imap imaps ipfs ipns ldap ldaps mqtt pop3 pop3s rtmp rtsp scp sftp smb smbs smtp smtps telnet tftp ws wss
Features: alt-svc AsynchDNS brotli GSS-API HSTS HTTP2 HTTP3 HTTPS-proxy IDN IPv6 Kerberos Largefile libz NTLM PSL SPNEGO SSL threadsafe TLS-SRP UnixSockets zstd

### pkg-config modules
ncurses  6.5.20250216  cflags=[-D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 ] libs=[-lncurses -ltinfo ]
ncursesw  6.5.20250216  cflags=[-D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 ] libs=[-lncursesw -ltinfo ]
tinfo  6.5.20250216  cflags=[-D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 ] libs=[-ltinfo ]
tinfow  ABSENT
menuw  6.5.20250216  cflags=[-D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 ] libs=[-lmenuw ]
formw  6.5.20250216  cflags=[-D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 ] libs=[-lformw ]
panelw  6.5.20250216  cflags=[-D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600 ] libs=[-lpanelw ]
libcurl  8.14.1  cflags=[-I/usr/include/aarch64-linux-gnu -isystem /usr/include/mit-krb5 -I/usr/include/p11-kit-1 ] libs=[-lcurl ]

### dpkg -l relevant
ii curl 8.14.1-2+deb13u4
ii g++ 4:14.2.0-1
ii g++-14 14.2.0-19
ii g++-14-aarch64-linux-gnu 14.2.0-19
ii g++-aarch64-linux-gnu 4:14.2.0-1
ii gcc 4:14.2.0-1
ii gcc-14 14.2.0-19
ii gcc-14-aarch64-linux-gnu 14.2.0-19
ii gcc-14-base:arm64 14.2.0-19
ii gcc-aarch64-linux-gnu 4:14.2.0-1
ii libc6-dev:arm64 2.41-12+deb13u3
ii libcurl3t64-gnutls:arm64 8.14.1-2+deb13u4
ii libcurl4-gnutls-dev:arm64 8.14.1-2+deb13u4
ii libcurl4t64:arm64 8.14.1-2+deb13u4
ii libncurses-dev:arm64 6.5+20250216-2
ii libncurses6:arm64 6.5+20250216-2
ii libncursesw6:arm64 6.5+20250216-2
ii make 4.4.1-2
ii ncurses-base 6.5+20250216-2
ii ncurses-bin 6.5+20250216-2
ii pkg-config:arm64 1.8.1-4

### uname -a
Linux 79f3e72c34c1 6.8.0-64-generic #67-Ubuntu SMP PREEMPT_DYNAMIC Sun Jun 15 20:23:40 UTC 2025 aarch64 GNU/Linux

### /etc/os-release
PRETTY_NAME="Debian GNU/Linux 13 (trixie)"
NAME="Debian GNU/Linux"
VERSION_ID="13"
VERSION="13 (trixie)"
VERSION_CODENAME=trixie
DEBIAN_VERSION_FULL=13.6
ID=debian
HOME_URL="https://www.debian.org/"
SUPPORT_URL="https://www.debian.org/support"
BUG_REPORT_URL="https://bugs.debian.org/"

### ncurses libs present
lrwxrwxrwx 1 root root     23 May  8 01:36 /usr/lib/aarch64-linux-gnu/libcurl-gnutls.so -> libcurl-gnutls.so.4.8.0
lrwxrwxrwx 1 root root     19 May  8 01:36 /usr/lib/aarch64-linux-gnu/libcurl-gnutls.so.3 -> libcurl-gnutls.so.4
lrwxrwxrwx 1 root root     23 May  8 01:36 /usr/lib/aarch64-linux-gnu/libcurl-gnutls.so.4 -> libcurl-gnutls.so.4.8.0
-rw-r--r-- 1 root root 988200 May  8 01:36 /usr/lib/aarch64-linux-gnu/libcurl-gnutls.so.4.8.0
lrwxrwxrwx 1 root root     17 May  8 01:36 /usr/lib/aarch64-linux-gnu/libcurl.so -> libcurl-gnutls.so
lrwxrwxrwx 1 root root     16 May  8 01:36 /usr/lib/aarch64-linux-gnu/libcurl.so.4 -> libcurl.so.4.8.0
-rw-r--r-- 1 root root 987880 May  8 01:36 /usr/lib/aarch64-linux-gnu/libcurl.so.4.8.0
-rw-r--r-- 1 root root     31 Mar  6  2025 /usr/lib/aarch64-linux-gnu/libncurses.so
lrwxrwxrwx 1 root root     17 Mar  6  2025 /usr/lib/aarch64-linux-gnu/libncurses.so.6 -> libncurses.so.6.5
-rw-r--r-- 1 root root 198592 Mar  6  2025 /usr/lib/aarch64-linux-gnu/libncurses.so.6.5
-rw-r--r-- 1 root root     32 Mar  6  2025 /usr/lib/aarch64-linux-gnu/libncursesw.so
lrwxrwxrwx 1 root root     18 Mar  6  2025 /usr/lib/aarch64-linux-gnu/libncursesw.so.6 -> libncursesw.so.6.5
-rw-r--r-- 1 root root 264128 Mar  6  2025 /usr/lib/aarch64-linux-gnu/libncursesw.so.6.5
lrwxrwxrwx 1 root root     13 Mar  6  2025 /usr/lib/aarch64-linux-gnu/libtinfo.so -> libtinfo.so.6
lrwxrwxrwx 1 root root     15 Mar  6  2025 /usr/lib/aarch64-linux-gnu/libtinfo.so.6 -> libtinfo.so.6.5
-rw-r--r-- 1 root root 265504 Mar  6  2025 /usr/lib/aarch64-linux-gnu/libtinfo.so.6.5

### libncurses.so / libncursesw.so linker scripts
libncurses.so:  INPUT(libncurses.so.6 -ltinfo)
libncursesw.so: INPUT(libncursesw.so.6 -ltinfo)

### wide-char symbol presence (nm -D --defined-only, grep -w)
/usr/lib/aarch64-linux-gnu/libncurses.so.6: add_wch=0 mvaddwstr=0 get_wch=0 wget_wch=0
/usr/lib/aarch64-linux-gnu/libncursesw.so.6: add_wch=1 mvaddwstr=1 get_wch=1 wget_wch=1

### TERM entries available
xterm
xterm-256color
xterm-color
xterm-debian
xterm-mono
dumb: /usr/share/terminfo/d/dumb
```

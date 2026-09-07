# cppide —— C/C++ 终端 IDE(ncurses + libcurl,C++17)
#
# 依赖:ncurses(宽字符版)+ libcurl。
#   macOS  : 系统自带(brew 的 ncurses 若存在则优先)
#   Debian : apt-get install build-essential libncursesw5-dev libcurl4-openssl-dev
#            (trixie 上包名为 libncurses-dev + libcurl4-gnutls-dev 亦可)
#
# 常用目标:
#   make                 编译出 ./cppide
#   make check-headers   Wave 1 验收门:每个头文件必须自给自足
#   make tests           编译并运行 tests/test_*.cpp
#   make debug           -O0 -g + ASan/UBSan,产物是 ./cppide-debug(与 release 互不干扰)

BIN      ?= cppide
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -MMD -MP
PREFIX   ?= /usr/local
UNAME_S  := $(shell uname -s)

# tests/*.cpp 直接写 #include "config.h"(不带 ../src/),所以搜索路径必须带 -Isrc。
# 放在 CPPFLAGS 而不是 CXXFLAGS:check-headers 的 SYNFLAGS 会过滤 CXXFLAGS,
# 而 CPPFLAGS 三个目标(%.o / check-headers / tests)都会用到。
CPPFLAGS += -Isrc

ifeq ($(UNAME_S),Darwin)
  NCURSES_PREFIX := $(shell brew --prefix ncurses 2>/dev/null)
  ifneq ($(NCURSES_PREFIX),)
    CPPFLAGS += -I$(NCURSES_PREFIX)/include
    LDFLAGS  += -L$(NCURSES_PREFIX)/lib
  endif
  LDLIBS += -lncurses -lcurl -lpthread
  # 若 macOS 自带的 ncurses 头不肯给出宽字符原型(get_wch / add_wch / cchar_t 未声明),
  # 打开下面这行;Debian 的 ncursesw 头默认已 NCURSES_WIDECHAR=1,不需要它。
  # CPPFLAGS += -D_XOPEN_SOURCE_EXTENDED
else
  # Linux:宽字符符号(add_wch/get_wch)只在 ncursesw 里,必须链 -lncursesw
  LDLIBS += -lncursesw -lcurl -lpthread
endif

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)
DEP := $(OBJ:.o=.d)
HDR := $(wildcard src/*.h)

# debug(ASan/UBSan)走**完全独立**的产物名:目标文件 src/*.dbg.o、可执行 cppide-debug。
# 为什么不是老写法 `debug: clean all`:clean 与 all 是两个平级先决条件,`make debug -j4`
# 下 GNU make 会并发跑它们,clean 把已经编出的 .o 删掉,链接阶段报
# "cannot find src/ai.o";而且它会连 ./cppide 一起删,把正在跑的验收脚本打挂。
# 现在 debug 与 release 共存,可并行、互不删对方产物。
DBGBIN   := $(BIN)-debug
DBGOBJ   := $(SRC:.cpp=.dbg.o)
DBGDEP   := $(DBGOBJ:.o=.d)
SANFLAGS := -fsanitize=address,undefined
DBGCXXFLAGS := $(filter-out -O2,$(CXXFLAGS)) -O0 -g $(SANFLAGS)

# 语法检查时不要生成 .d(会与 .o 的依赖文件同名互相覆盖)
SYNFLAGS := $(filter-out -MMD -MP,$(CXXFLAGS))

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

# Wave 1 的验收门:每个头文件必须自给自足。
# 用“生成一个只 include 该头的 TU”而不是直接 -x c++ 头文件本身:
# 后者会让 GCC 无条件报 "#pragma once in main file"(该警告无法用 -Wno-* 关掉),
# 而且“被 include”才是头文件真实的使用方式。
check-headers:
	@for h in $(HDR); do echo "  $$h"; \
	  printf '#include "%s"\n' "$$h" | \
	  $(CXX) $(CPPFLAGS) $(SYNFLAGS) -fsyntax-only -x c++ - || exit 1; done
	@echo "check-headers: OK ($(words $(HDR)) 个头文件)"

debug: $(DBGBIN)

$(DBGBIN): $(DBGOBJ)
	$(CXX) $(LDFLAGS) $(SANFLAGS) -o $@ $^ $(LDLIBS)

src/%.dbg.o: src/%.cpp
	$(CXX) $(CPPFLAGS) $(DBGCXXFLAGS) -c -o $@ $<

tests: $(filter-out src/main.o,$(OBJ))
	@for t in tests/test_*.cpp; do \
	  $(CXX) $(CPPFLAGS) $(CXXFLAGS) -o /tmp/$$(basename $$t .cpp) $$t \
	    $(filter-out src/main.o,$(OBJ)) $(LDLIBS) && /tmp/$$(basename $$t .cpp) || exit 1; done

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(DEP) $(BIN) $(DBGOBJ) $(DBGDEP) $(DBGBIN)

-include $(DEP)
-include $(DBGDEP)
.PHONY: all clean debug tests install check-headers

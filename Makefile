TOPDIR     := .
SRCDIR     := $(TOPDIR)/src

CC         := gcc
#CFLAGS    := -std=c++11 -O2 -Wall -Wextra -pipe -I./include -fno-rtti -fpic
#V8_LIBS   := -L./lib -lv8_libplatform -lv8_base -lv8_init -lv8_initializers -lv8_libbase -lv8_libsampler -lv8_nosnapshot -lv8_snapshot
#LDFLAGS   := -lstdc++ -lboost_system -pthread -lm -lcriu $(V8_LIBS)

LUACFLAGS  := -I./vendor/lua-5.3.5/src
LUALDFLAGS := -L./vendor/lua-5.3.5/src -llua

CFLAGS     := -pie -fpie -g -std=c99 -Wall -Wextra -pipe -lm $(shell pkg-config --cflags jansson)
LDFLAGS    := $(shell pkg-config --libs jansson)

SRCS       := $(shell find $(SRCDIR) -type f -name "*.c")
OBJS       := $(patsubst %.c,%.o,$(SRCS))

EVALERS    := luaval

.PHONY: all clean

all: $(EVALERS)

clean:
	$(RM) $(EVALERS) $(OBJS)
	 cd ./vendor/lua-5.3.5 && $(MAKE) clean

luaval: $(OBJS) ./vendor/lua-5.3.5/src/liblua.a
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

./vendor/lua-5.3.5/src/liblua.a:
	 cd ./vendor/lua-5.3.5 && $(MAKE) linux

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

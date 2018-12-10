TOPDIR     := .
SRCDIR     := $(TOPDIR)/src

CC         := gcc
#CFLAGS    := -std=c++11 -O2 -Wall -Wextra -pipe -I./include -fno-rtti -fpic
#V8_LIBS   := -L./lib -lv8_libplatform -lv8_base -lv8_init -lv8_initializers -lv8_libbase -lv8_libsampler -lv8_nosnapshot -lv8_snapshot
#LDFLAGS   := -lstdc++ -lboost_system -pthread -lm -lcriu $(V8_LIBS)

LUACFLAGS  := -I./vendor/lua-5.3.5/src $(shell pkg-config --cflags jansson)
LUALDFLAGS := -L./vendor/lua-5.3.5/src -llua $(shell pkg-config --libs jansson)

CFLAGS     := -pie -fpie -g -Wall -Wextra -pipe -lm -Wno-unused-parameter
LDFLAGS    :=

SRCS       := $(shell find $(SRCDIR) -type f -name "*.c")
OBJS       := $(patsubst %.c,%.o,$(SRCS))

EVALERS    := luaval
SERVER     := evalserver

.PHONY: all clean

all: $(EVALERS) $(SERVER) memgraph

evalserver: ./cmd/evalserver/*
	go build ./cmd/evalserver

clean:
	$(RM) $(EVALERS) $(OBJS)
	 cd ./vendor/lua-5.3.5 && $(MAKE) clean

memgraph: src/alloc.o src/memgraph/main.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

luaval: src/alloc.o src/luaval/main.o ./vendor/lua-5.3.5/src/liblua.a
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LUALDFLAGS) -o $@

./vendor/lua-5.3.5/src/liblua.a:
	 cd ./vendor/lua-5.3.5 && $(MAKE) linux

src/luaval/main.o: src/luaval/main.c
	$(CC) $(CFLAGS) $(LUACFLAGS) -o $@ -c $<

%.o: %.c %.h
	$(CC) $(CFLAGS) -o $@ -c $<

TOPDIR     := .
SRCDIR     := $(TOPDIR)/src

CC         := gcc -std=c99 -pedantic-errors

LUACFLAGS  := -I./vendor/lua-5.3.5/src
LUALDFLAGS := -L./vendor/lua-5.3.5/src -llua

DUKTAPECFLAGS  := -I./vendor/duktape-2.3.0/src
DUKTAPELDFLAGS :=

CFLAGS     := -g -Wall -Wextra -pedantic -pipe -fpie -fpic -Wno-unused-parameter $(shell pkg-config --cflags jansson)
LDFLAGS    :=  $(shell pkg-config --libs jansson) -lm

SRCS       := $(shell find $(SRCDIR) -type f -name "*.c")
OBJS       := $(patsubst %.c,%.o,$(SRCS))

CBINS      := luaval duktape memtest memgraph
SERVER     := evalserver

.PHONY: all clean test

all: $(CBINS) $(SERVER)

test:
	go test -v ./test

valgrind:
	echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
	./test/valgrind-tests

cppcheck:
	cppcheck \
		--inconclusive \
		--std=c99 \
		--suppress=missingInclude \
		--enable=all \
		--error-exitcode=1 \
		--template "{file}({line}): {severity} ({id}): {message}" \
		-i cmdline.c \
		./src

clangtidy:
	clang-tidy ./src/*.c ./src/*/*.c

evalserver: ./cmd/evalserver/*
	go build ./cmd/evalserver

gengetopt:
	(cd src/driver && gengetopt < getopt)
	(cd src/memgraph && gengetopt < getopt)

clean:
	$(RM) $(CBINS) $(OBJS)
	 cd ./vendor/lua-5.3.5 && $(MAKE) clean

memgraph: src/alloc.o src/memgraph/main.o src/memgraph/cmdline.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

luaval: src/driver/cmdline.o src/driver/evaler.o  src/alloc.o src/luaval/main.o ./vendor/lua-5.3.5/src/liblua.a
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LUALDFLAGS) -o $@

duktape: src/driver/cmdline.o src/driver/evaler.o src/alloc.o src/duktape/main.o ./vendor/duktape-2.3.0/src/duktape.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(DUKTAPELDFLAGS) -o $@

./vendor/lua-5.3.5/src/liblua.a:
	 cd ./vendor/lua-5.3.5 && $(MAKE) linux

src/duktape/main.o: src/duktape/main.c
	$(CC) $(CFLAGS) $(DUKTAPECFLAGS) -o $@ -c $<

src/luaval/main.o: src/luaval/main.c
	$(CC) $(CFLAGS) $(LUACFLAGS) -o $@ -c $<

memtest: src/alloc.o src/memtest/main.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

%.o: %.c %.h
	$(CC) $(CFLAGS) -o $@ -c $<

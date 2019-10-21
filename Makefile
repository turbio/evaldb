TOPDIR     := .
SRCDIR     := $(TOPDIR)/src

CC         := cc -std=c99 -pedantic-errors -g

LUACFLAGS  := -I./vendor/lua-5.3.5/src
LUALDFLAGS := -L./vendor/lua-5.3.5/src

DUKTAPECFLAGS  := -I./vendor/duktape-2.3.0/src
DUKTAPELDFLAGS :=

CFLAGS     := -Wall -Wextra -pedantic -pipe -fpie -fpic -Wno-unused-parameter $(shell pkg-config --cflags jansson)
LDFLAGS    :=  $(shell pkg-config --libs jansson) -lm

SRCS       := $(shell find $(SRCDIR) -type f -name "*.c")
OBJS       := $(patsubst %.c,%.o,$(SRCS))

LANG_OBJS := src/driver/cmdline.o src/driver/evaler.o src/alloc.o

CBINS      := luaval duktape memtest memgraph testcounter

.PHONY: all clean test

all: $(CBINS) gateway

test:
	go test -v ./test

# TODO(turbio): valgrind doesn't like our use fo mremap
# valgrind:
# 	echo 0 | sudo tee /proc/sys/kernel/randomize_va_space
# 	./test/valgrind-tests

cppcheck:
	cppcheck \
		--inconclusive \
		--std=c99 \
		--suppress=missingInclude \
		--enable=all \
		--inline-suppr \
		--error-exitcode=1 \
		--template "{file}({line}): {severity} ({id}): {message}" \
		-i cmdline.c \
		./src

clangtidy:
	clang-tidy ./src/*.c ./src/*/*.c

gateway: ./cmd/gateway/*
	go build ./cmd/gateway

gengetopt:
	(cd src/driver && gengetopt < getopt)
	(cd src/memgraph && gengetopt < getopt)

clean:
	$(RM) $(CBINS) $(OBJS)
	 cd ./vendor/lua-5.3.5 && $(MAKE) clean

memgraph: src/alloc.o src/memgraph/main.o src/memgraph/cmdline.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

luaval: $(LANG_OBJS) src/luaval/main.o ./vendor/lua-5.3.5/src/liblua.a
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LUALDFLAGS) -o $@

testcounter: $(LANG_OBJS) src/testcounter/main.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

duktape: $(LANG_OBJS) src/duktape/main.o ./vendor/duktape-2.3.0/src/duktape.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(DUKTAPELDFLAGS) -o $@

./vendor/lua-5.3.5/src/liblua.a:
	 cd ./vendor/lua-5.3.5 && $(MAKE) linux

src/duktape/main.o: src/duktape/main.c
	$(CC) $(CFLAGS) $(DUKTAPECFLAGS) -o $@ -c $<

src/luaval/main.o: src/luaval/main.c
	$(CC) $(CFLAGS) $(LUACFLAGS) -o $@ -c $<

src/testcounter/main.o: src/testcounter/main.c
	$(CC) $(CFLAGS) -o $@ -c $<

memtest: src/alloc.o src/memtest/main.o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

%.o: %.c %.h src/config.h
	$(CC) $(CFLAGS) -o $@ -c $<

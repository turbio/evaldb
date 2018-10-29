TOPDIR   := .
SRCDIR   := $(TOPDIR)/src

CC       := gcc
#CFLAGS   := -std=c++11 -O2 -Wall -Wextra -pipe -I./include -fno-rtti -fpic
#V8_LIBS  := -L./lib -lv8_libplatform -lv8_base -lv8_init -lv8_initializers -lv8_libbase -lv8_libsampler -lv8_nosnapshot -lv8_snapshot
#LDFLAGS  := -lstdc++ -lboost_system -pthread -lm -lcriu $(V8_LIBS)

CFLAGS   := -std=c99 -O2 -Wall -Wextra -pipe -I./include $(shell pkg-config --cflags lua53)
LDFLAGS  := $(shell pkg-config --libs lua53)

SRCS     := $(shell find $(SRCDIR) -type f -name "*.c")
OBJS     := $(patsubst %.c,%.o,$(SRCS))

TARGET   := server

.PHONY: all clean

all: $(TARGET)

clean:
	$(RM) $(TARGET) $(OBJS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

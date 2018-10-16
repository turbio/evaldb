TOPDIR   := .
SRCDIR   := $(TOPDIR)/src

CC       := gcc
CFLAGS   := -std=c++11 -O2 -Wall -Wextra -pipe -I./include -fno-rtti -fpic
V8_LIBS  := -L./lib -lv8_libplatform -lv8_base -lv8_init -lv8_initializers -lv8_libbase -lv8_libsampler -lv8_nosnapshot -lv8_snapshot
LDFLAGS  := -lstdc++ -lboost_system -pthread -lm -lcriu $(V8_LIBS)

SRCS     := $(shell find $(SRCDIR) -type f -name "*.cpp")
OBJS     := $(patsubst %.cpp,%.o,$(SRCS))

TARGET   := server

.PHONY: all clean

all: $(TARGET)

clean:
	$(RM) $(TARGET) $(OBJS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

%.o: %.cpp
	$(CC) $(CFLAGS) -o $@ -c $<

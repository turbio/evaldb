#pragma once

#include <stddef.h>

#define NODE_CHILDREN 5

struct heap_header {
  uint16_t v;
  uint64_t size; // size includes self

  struct heap_frame *root;

  uintptr_t lua_state;
};

enum frame_type {
  EMPTY = 0,
  FREE_LEAF = (1 << 0),
  USED_LEAF = (1 << 1),
  FRAME = (1 << 2),
};

struct heap_leaf {
  size_t size;
};

struct heap_frame {
  size_t size; // size does not include self

  // each type corresponds the the child at the same index
  enum frame_type ctype[NODE_CHILDREN];
  void *c[NODE_CHILDREN];
};

void *snap_malloc(struct heap_header *heap, size_t n);
void snap_free(struct heap_header *heap, void *ptr);
void *snap_realloc(struct heap_header *heap, void *ptr, size_t n);
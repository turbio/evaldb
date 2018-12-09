#pragma once

#include <stddef.h>
#include <stdint.h>

#define NODE_CHILDREN 5

#define _1_KB 1024
#define _1_MB (_1_KB * 1024)
#define _1_GB (_1_MB * 1024)

#define ALLOC_BLOCK_SIZE (_1_MB * 1)

#define MAP_START_ADDR ((void *)0x600000000000)

#define NUM_REVISIONS 100

struct heap_header {
  uint16_t v;
  size_t size; // size includes self

  void *user_ptr;

  int rev;
  struct heap_frame *revs[NUM_REVISIONS];
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

struct heap_header *init_alloc(char *argv[], char *db_path);

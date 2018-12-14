#pragma once

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define NODE_CHILDREN 16

#define PSIZE sysconf(_SC_PAGESIZE)

#define INITIAL_PAGES 1000

#define ALLOC_BLOCK_SIZE (INITIAL_PAGES * PSIZE)

#define MAP_START_ADDR ((void *)0x600000000000)

#define USER_DATA_DIST (PSIZE * 10)

#define NUM_REVISIONS 100

struct heap_header {
  uint16_t v;
  size_t size; // size includes self

  void *user_ptr;

  int working;
  int committed;

  struct heap_frame *revs[NUM_REVISIONS];

  struct heap_frame *last_frame;
  struct heap_leaf *last_leaf;
};

enum frame_type {
  EMPTY = 0,
  FREE_LEAF = (1 << 0),
  USED_LEAF = (1 << 1),
  FRAME = (1 << 2),
};

struct heap_leaf {
  char committed;
  size_t size; // size does not include self
};

struct heap_frame {
  char committed;

  // each type corresponds the the child at the same index
  enum frame_type ctype[NODE_CHILDREN];
  void *c[NODE_CHILDREN];
};

void *snap_malloc(struct heap_header *heap, size_t n);
void snap_free(struct heap_header *heap, void *ptr);
void *snap_realloc(struct heap_header *heap, void *ptr, size_t n);

struct heap_frame *root(struct heap_header *heap);

struct heap_header *init_alloc(char *argv[], char *db_path);

void commit(struct heap_header *heap);
void begin_mut(struct heap_header *heap);

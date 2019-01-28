#pragma once

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define PAGE_SIZE sysconf(_SC_PAGESIZE)

// should be at least two, half will be used for the initial generation
// and the second half for the intial page.
#define INITIAL_PAGES 8

#define ALLOC_BLOCK_SIZE (INITIAL_PAGES * PAGE_SIZE)

#define MAP_START_ADDR ((void *)0x600000000000)

#define GENERATION_CHILDREN 16

#define MAX_MAPS 65530

struct heap_header {
  uint16_t v;

  size_t page_size; // catastrophic things happen if the page size changes
  void *map_start;

  size_t size; // size includes self

  void *user_ptr;

  struct snap_generation *working;
  struct snap_generation *committed;

  struct snap_generation *root;

  struct snap_page *last_page;
  struct snap_generation *last_gen;
  int last_gen_index;
};

enum snap_node_type {
  SNAP_NODE_GENERATION = 1 << 0,
  SNAP_NODE_PAGE = 1 << 1,
};

struct snap_node {
  char type;
  char committed;
};

struct snap_generation {
  struct snap_node i;

  int gen;
  struct snap_node *c[GENERATION_CHILDREN];
};

struct snap_page {
  struct snap_node i;

  void *real_addr;

  int pages;
  int len;

  struct snap_segment *c[]; // TODO(turbi): should be relative
};

struct snap_segment {
  char used;
  size_t size; // size does not include self
};

void *snap_malloc(struct heap_header *heap, size_t size);
void snap_free(struct heap_header *heap, void *ptr);
void *snap_realloc(struct heap_header *heap, void *ptr, size_t size);

struct heap_header *snap_init(char *db_path);

int snap_commit(struct heap_header *heap);
int snap_begin_mut(struct heap_header *heap);
void snap_checkout(struct heap_header *heap, int genid);

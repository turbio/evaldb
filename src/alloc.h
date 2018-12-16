#pragma once

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define PAGE_SIZE sysconf(_SC_PAGESIZE)

#define INITIAL_PAGES 0x1000

#define ALLOC_BLOCK_SIZE (INITIAL_PAGES * PAGE_SIZE)

#define MAP_START_ADDR ((void *)0x600000000000)

#define USER_DATA_START_ADDR ((char *)MAP_START_ADDR + (PAGE_SIZE * 8))

#define GENERATION_CHILDREN 0x10

struct heap_header {
  uint16_t v;
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

  int pages;
  int len;
  struct snap_segment *c[];
};

struct snap_segment {
  char used;
  size_t size; // size does not include self
};

void *snap_malloc(struct heap_header *heap, size_t n);
void snap_free(struct heap_header *heap, void *ptr);
void *snap_realloc(struct heap_header *heap, void *ptr, size_t n);

struct snap_page *root(struct heap_header *heap);

struct heap_header *snap_init(char *argv[], char *db_path);

void snap_commit(struct heap_header *heap);
void snap_begin_mut(struct heap_header *heap);

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define PAGE_SIZE sysconf(_SC_PAGESIZE)

#define INITIAL_PAGES 0x1000

#define ALLOC_BLOCK_SIZE (INITIAL_PAGES * PAGE_SIZE)

#define MAP_START_ADDR ((void *)0x600000000000)

#define NUM_REVISIONS 100

struct heap_header {
  uint16_t v;
  size_t size; // size includes self

  void *user_ptr;

  int working;
  int committed;

  struct frame_node *revs[NUM_REVISIONS];
};

struct frame_leaf {
  char used;
  size_t size; // size does not include self
};

struct frame_node {
  char committed;

  struct frame_node *next;

  int pages;

  int len;
  struct frame_leaf *c[];
};

void *snap_malloc(struct heap_header *heap, size_t n);
void snap_free(struct heap_header *heap, void *ptr);
void *snap_realloc(struct heap_header *heap, void *ptr, size_t n);

struct frame_node *root(struct heap_header *heap);

struct heap_header *init_alloc(char *argv[], char *db_path);

void commit(struct heap_header *heap);
void begin_mut(struct heap_header *heap);

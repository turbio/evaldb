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

  struct snap_page *revs[NUM_REVISIONS];

  // struct snap_gen *working;
  // struct snap_gen *committed;

  // struct snap_gen *root;

  struct snap_page *last_frame;
};

struct snap_gen {};

struct snap_segment {
  char used;
  size_t size; // size does not include self
};

struct snap_page {
  char committed;
  int pages;
  int len;
  struct snap_page *next;
  struct snap_segment *c[];
};

void *snap_malloc(struct heap_header *heap, size_t n);
void snap_free(struct heap_header *heap, void *ptr);
void *snap_realloc(struct heap_header *heap, void *ptr, size_t n);

struct snap_page *root(struct heap_header *heap);

struct heap_header *snap_init(char *argv[], char *db_path);

void snap_commit(struct heap_header *heap);
void snap_begin_mut(struct heap_header *heap);

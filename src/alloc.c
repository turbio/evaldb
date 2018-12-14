#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/types.h>
#include <unistd.h>

#include "alloc.h"

void *open_db(const char *path, size_t size) {
  int fd = open(path, O_CREAT | O_RDWR, 0660);
  if (fd == -1) {
    fprintf(stderr, "couldn't open db %s\n", strerror(errno));
    return NULL;
  }

  int err = ftruncate(fd, size);
  if (err == -1) {
    fprintf(stderr, "could not increase db size  %s\n", strerror(errno));
    return NULL;
  }

  void *mem = mmap(
      MAP_START_ADDR,
      size,
      PROT_READ | PROT_WRITE,
      MAP_FIXED | MAP_SHARED,
      fd,
      0);
  if (mem == MAP_FAILED) {
    fprintf(stderr, "db map failed  %s\n", strerror(errno));
    return NULL;
  }

  return mem;
}

struct frame_node *root(struct heap_header *heap) {
  return heap->revs[heap->working];
}

void *round_page_down(void *addr) {
  return (void *)((uintptr_t)addr & ~(PAGE_SIZE - 1));
}

void *round_page_up(void *addr) {
  if ((uintptr_t)addr & (PAGE_SIZE - 1)) {
    return round_page_down(addr + PAGE_SIZE);
  }

  return addr;
}

struct heap_header *segv_handle_heap;

void *handling_segv = NULL;

void handle_segv(int signum, siginfo_t *i, void *d) {
  void *addr = i->si_addr;

  if (handling_segv) {
    fprintf(
        stderr,
        "SEGFAULT at %p while already handling SEGV for %p\n",
        addr,
        handling_segv);
    exit(2);
  }

  handling_segv = addr;

  struct heap_header *heap = segv_handle_heap;

  if (addr < MAP_START_ADDR ||
      MAP_START_ADDR >= MAP_START_ADDR + ALLOC_BLOCK_SIZE) {
    fprintf(stderr, "SEGFAULT trying to read %p\n", addr);
    exit(2);
  }

  if (segv_handle_heap->committed == segv_handle_heap->working) {
    fprintf(stderr, "SEGFAULT write outside of mutation %p\n", addr);
    exit(2);
  }

  assert(heap->working != heap->committed);

  void *page = round_page_down(addr);

  int err = mprotect(page, PAGE_SIZE, PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  fprintf(stderr, "! hit %p, marked %p-%p\n", addr, page, page + PAGE_SIZE - 1);

  handling_segv = NULL;
}

struct heap_header *init_alloc(char *argv[], char *db_path) {
  int pers = personality(0xffffffff);
  if (pers == -1) {
    fprintf(stderr, "could not get personality %s\n", strerror(errno));
    exit(1);
  }

  if (!(pers & ADDR_NO_RANDOMIZE)) {
    if (personality(ADDR_NO_RANDOMIZE) == -1) {
      fprintf(stderr, "could not set personality %s\n", strerror(errno));
      exit(1);
    }

    execve("/proc/self/exe", argv, NULL);
  }

  void *mem = open_db(db_path, ALLOC_BLOCK_SIZE);

  struct heap_header *heap = mem;

  if (heap->v == 0) {
    heap->v = 0xffca;
    heap->size = ALLOC_BLOCK_SIZE;

    int initial_rev = 0;

    heap->working = initial_rev;
    heap->committed = initial_rev;

    heap->revs[initial_rev] = round_page_up(heap + sizeof(struct heap_header));
    *heap->revs[initial_rev] = (struct frame_node){
        .committed = 0,
        .pages = 1,
        .len = 0,
    };
  } else if (heap->v != 0xffca) {
    fprintf(
        stderr,
        "got a bad snapshot, %d (expected) != %d (actual)",
        0xffca,
        heap->v);
    exit(1);
  }

  segv_handle_heap = heap;

  struct sigaction segv_action;

  segv_action.sa_flags = SA_SIGINFO | SA_NODEFER;
  segv_action.sa_sigaction = handle_segv;
  sigaction(SIGSEGV, &segv_action, NULL);

  return heap;
}

void walk_frames(
    struct frame_node *f, void (*cb)(struct frame_node *, void *), void *d) {

  cb(f, d);

  if (f->next) {
    walk_frames(f->next, cb, d);
  }
}

struct non_commit_status {
  void *addr;
  int count;
};

void flag_non_commit(struct frame_node *f, void *d) {
  struct non_commit_status *status = (struct non_commit_status *)d;
  if (!f->committed) {
    status->count++;
    if (!status->addr) {
      status->addr = f;
    }
  }
}

void verify(struct heap_header *heap) {
  struct non_commit_status status = {
      .addr = NULL,
      .count = 0,
  };
  walk_frames(
      (struct frame_node *)root(heap), flag_non_commit, (void *)&status);

  assert(status.addr == 0);
}

void set_committed(struct frame_node *f, void *d) {
  int *flagged = (int *)d;
  (*flagged)++;
  f->committed = 1;
}

void commit(struct heap_header *heap) {
  fprintf(stderr, "BEGINNING COMMIT\n");

  int flagged = 0;
  walk_frames((struct frame_node *)root(heap), set_committed, &flagged);

  fprintf(stderr, "committed %d nodes\n", flagged);

  fprintf(stderr, "SETTING READONLY\n");

  void *start_addr = round_page_up((void *)heap + sizeof(struct heap_header));
  size_t len = (INITIAL_PAGES - 1) * PAGE_SIZE;

  int err = mprotect(start_addr, len, PROT_READ);
  if (err != 0) {
    fprintf(stderr, "failed to unmark write pages %s\n", strerror(errno));
    fprintf(stderr, "%p - %p", start_addr, start_addr + len - 1);
    exit(3);
  }

  verify(heap);
  fprintf(stderr, "UPDATING COMMIT BIT\n");

  heap->committed = heap->working;

  fprintf(stderr, "COMMIT COMPLETE\n");
}

void begin_mut(struct heap_header *heap) {
  fprintf(stderr, "BEGINNING MUT\n");

  assert(heap->working == heap->committed);
  verify(heap);

  fprintf(
      stderr,
      "rev: %d, frame: %p\n",
      heap->working,
      heap->revs[heap->committed]);

  heap->working++;
  heap->revs[heap->working] = heap->revs[heap->committed];

  fprintf(stderr, "working: %d\n", heap->working);

  assert(heap->working != heap->committed);
  assert(heap->revs[heap->working] == heap->revs[heap->committed]);
}

size_t nsize(int c) {
  return sizeof(struct frame_node) + (sizeof(struct frame_node *) * c);
}

void *n_last_free(struct frame_node *node) {
  if (!node->len) {
    return (void *)node + (PAGE_SIZE * node->pages) - 1;
  }

  return (void *)node->c[node->len - 1] - 1;
}

struct frame_leaf *n_new_alloc(struct frame_node *node, size_t n) {
  void *last_free = n_last_free(node);

  if ((last_free - n) - ((void *)node + nsize(node->len + 1)) <= 0) {
    return NULL;
  }

  struct frame_leaf *addr = last_free - n;

  node->c[node->len] = addr;
  node->len++;

  addr->size = n;
  addr->used = 1;

  return addr;
}

struct frame_node *n_new_child(struct frame_node *node, size_t min) {
  fprintf(stderr, "creating new child...\n");

  struct frame_node *node2 = (void *)node + PAGE_SIZE;

  node->next = (struct frame_node *)node2;

  *node2 = (struct frame_node){
      .committed = 0,
      .pages = (min / PAGE_SIZE) + 1,
      .len = 0,
  };

  return node2;
}

void *snap_malloc(struct heap_header *heap, size_t n) {
  struct frame_node *node = root(heap);

  while (node->next) {
    node = node->next;
  }

  struct frame_leaf *l = n_new_alloc(node, n + sizeof(struct frame_leaf));
  if (l == NULL) {
    node = n_new_child(node, n);
    l = n_new_alloc(node, n + sizeof(struct frame_leaf));
  }

  assert(l != NULL);

  return (void *)l + sizeof(struct frame_leaf);
}

void snap_free(struct heap_header *heap, void *ptr) {
  struct frame_leaf *l = ptr - sizeof(struct frame_leaf);

  assert(l->used);
  l->used = 0;
}

void *snap_realloc(struct heap_header *heap, void *ptr, size_t n) {
  void *result = snap_malloc(heap, n);
  memmove(result, ptr, n);
  snap_free(heap, ptr);
  return result;
}

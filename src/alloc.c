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

struct heap_frame *root(struct heap_header *heap) {
  return heap->revs[heap->working];
}

struct heap_header *segv_handle_heap;

void *round_page(void *addr) {
  return (void *)((uintptr_t)addr & ~((uintptr_t)getpagesize() - 1));
}

void handle_segv(int signum, siginfo_t *i, void *d) {
  void *addr = i->si_addr;

  if (addr < MAP_START_ADDR ||
      MAP_START_ADDR >= MAP_START_ADDR + ALLOC_BLOCK_SIZE) {
    fprintf(stderr, "SEGFAULT trying to read %p\n", i->si_addr);
    exit(2);
  }

  if (segv_handle_heap->committed == segv_handle_heap->working) {
    fprintf(stderr, "SEGFAULT write outside of mutation %p\n", i->si_addr);
    exit(2);
  }

  void *page = round_page(addr);

  fprintf(stderr, "! hit %p, marked %p-%p\n", addr, page, page + getpagesize());

  int err = mprotect(page, getpagesize(), PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }
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

    heap->working = 0;
    heap->committed = 0;

    heap->revs[heap->working] = mem + sizeof(struct heap_header);
    root(heap)->size =
        heap->size - sizeof(struct heap_header) - sizeof(struct heap_frame);
    root(heap)->commit = 1;
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

void walk_heap_frames(
    struct heap_frame *frame,
    void (*cb)(struct heap_frame *, void *),
    void *data) {
  cb(frame, data);

  for (int i = 0; i < NODE_CHILDREN; i++) {
    if (frame->ctype[i] == FRAME) {
      walk_heap_frames((struct heap_frame *)frame->c[i], cb, data);
    }
  }
}

struct non_commit_status {
  void *addr;
};

void flag_non_commit(struct heap_frame *frame, void *d) {
  struct non_commit_status *status = (struct non_commit_status *)d;
  if (!frame->commit && !status->addr) {
    status->addr = frame;
  }
}

void verify(struct heap_header *heap) {
  struct non_commit_status status = {
      .addr = NULL,
  };
  walk_heap_frames(root(heap), flag_non_commit, (void *)&status);

  assert(status.addr == 0);
}

void set_committed(struct heap_frame *frame, void *d) {
  fprintf(stderr, "committing - %p\n", frame);
  frame->commit = 1;
}

void commit(struct heap_header *heap) {
  fprintf(stderr, "BEGINNING COMMIT\n");
  walk_heap_frames(root(heap), set_committed, NULL);

  fprintf(stderr, "SETTING RW\n");

  int err = mprotect(
      MAP_START_ADDR + sizeof(struct heap_header),
      heap->size - sizeof(struct heap_header),
      PROT_READ);
  if (err != 0) {
    fprintf(stderr, "failed to unmark write pages %s\n", strerror(errno));
    exit(3);
  }

  fprintf(stderr, "COMMIT COMPLETE\n");
  verify(heap);
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

int next_free_index(struct heap_frame *f) {
  for (int i = 0; i < NODE_CHILDREN; i++) {
    if (f->ctype[i] == EMPTY /*|| f->ctype[i] == FREE_LEAF*/) {
      return i;
    }
  }

  return -1;
}

// frame pointer + used frame size = next available address
void *next_free_addr(struct heap_frame *f, int *pindex) {
  int next_child = next_free_index(f);

  assert(next_child != -1);

  if (next_child == 0) {
    *pindex = 0;
    return (void *)f + sizeof(struct heap_frame);
  }

  int prev_child = next_child - 1;

  if (f->ctype[prev_child] == USED_LEAF || f->ctype[prev_child] == FREE_LEAF) {
    *pindex = next_child;
    struct heap_leaf *l = f->c[prev_child];
    return (void *)l + l->size + sizeof(struct heap_leaf);
  }

  return next_free_addr(f->c[prev_child], pindex);
}

void *create_next_child(struct heap_frame *f, enum frame_type t) {
  int pindex;
  void *n = next_free_addr(f, &pindex);

  f->ctype[pindex] = t;
  f->c[pindex] = n;

  return f->c[pindex];
}

struct heap_frame *find_parent(struct heap_frame *r, void *f, int *cindex) {
  int i = 0;

  for (;;) {
    if (r->c[i] == f) {
      *cindex = i;
      return r;
    }

    if (f < r->c[i] || i == NODE_CHILDREN - 1) {
      r = r->c[i];
      i = 0;
      continue;
    }

    i++;
  }
}

void *snap_malloc(struct heap_header *heap, size_t n) {
  struct heap_frame *parent = root(heap);

  int free_index = next_free_index(parent);

  while (free_index == -1) {
    parent = parent->c[NODE_CHILDREN - 1];
    free_index = next_free_index(parent);
  }

  if (free_index == NODE_CHILDREN - 1) {
    struct heap_frame *link = create_next_child(parent, FRAME);
    parent = link;
  }

  struct heap_leaf *l = create_next_child(parent, USED_LEAF);

  l->size = n;

  return (void *)l + sizeof(struct heap_leaf);
}

void snap_free(struct heap_header *heap, void *ptr) {
  struct heap_leaf *l = (struct heap_leaf *)(ptr - sizeof(struct heap_leaf));

  int i;
  struct heap_frame *p = find_parent(root(heap), l, &i);
  assert(p->ctype[i] == USED_LEAF);

  p->ctype[i] = FREE_LEAF;

  memset(ptr, '-', l->size);
}

void *snap_realloc(struct heap_header *heap, void *ptr, size_t n) {
  void *result = snap_malloc(heap, n);
  memmove(result, ptr, n);
  snap_free(heap, ptr);
  return result;
}

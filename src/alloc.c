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

  void *mem = mmap(MAP_START_ADDR, size, PROT_READ | PROT_WRITE,
                   MAP_FIXED | MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED) {
    fprintf(stderr, "db map failed  %s\n", strerror(errno));
    return NULL;
  }

  return mem;
}

struct heap_frame *root(struct heap_header *heap) {
  return heap->revs[heap->working];
}

size_t round_page_up(size_t size) {
  if (size % PSIZE) {
    return size + PSIZE - (size % PSIZE);
  }

  return size;
}

uintptr_t round_page(uintptr_t addr) { return addr & ~(PSIZE - 1); }

void find_frames_inside(struct heap_frame *frame, void *page) {
  fprintf(stderr, "MARKING in %p\n", page);

  for (int i = 0; i < NODE_CHILDREN; i++) {
    if (frame->ctype[i] == USED_LEAF) {
      if (frame->c[i] >= page && frame->c[i] < page + PSIZE) {
        fprintf(stderr, "\thit: %p\n", frame->c[i]);
      }
    } else if (frame->ctype[i] == FRAME) {
      find_frames_inside(frame->c[i], page);
    }
  }
}

struct heap_header *segv_handle_heap;

void handle_segv(int signum, siginfo_t *i, void *d) {
  struct heap_header *heap = segv_handle_heap;

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

  void *page = (void *)round_page((uintptr_t)addr);

  fprintf(stderr, "! hit %p, marked %p-%p\n", addr, page, page + PSIZE - 1);

  find_frames_inside(root(heap), page);

  int err = mprotect(page, PSIZE, PROT_READ | PROT_WRITE);
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

    heap->revs[0] = mem + sizeof(struct heap_header);
    heap->revs[0]->ctype[0] = USED_LEAF;
    heap->revs[0]->c[0] = (void *)heap + USER_DATA_DIST;
    ((struct heap_leaf *)heap->revs[0]->c[0])->committed = 1;
    ((struct heap_leaf *)heap->revs[0]->c[0])->size = 0;
    root(heap)->committed = 1;
  } else if (heap->v != 0xffca) {
    fprintf(stderr, "got a bad snapshot, %d (expected) != %d (actual)", 0xffca,
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

void walk_heap(struct heap_frame *frame, void (*cb)(struct heap_leaf *, void *),
               void *d) {
  cb((struct heap_leaf *)frame, d);

  for (int i = 0; i < NODE_CHILDREN; i++) {
    if (frame->ctype[i] == FRAME) {
      walk_heap((struct heap_frame *)frame->c[i], cb, d);
    } else if (frame->ctype[i] == USED_LEAF) {
      cb((struct heap_leaf *)frame->c[i], d);
    }
  }
}

struct non_commit_status {
  void *addr;
  int count;
};

void flag_non_commit(struct heap_leaf *frame, void *d) {
  struct non_commit_status *status = (struct non_commit_status *)d;
  if (!frame->committed) {
    status->count++;
    if (!status->addr) {
      status->addr = frame;
    }
  }
}

void verify(struct heap_header *heap) {
  struct non_commit_status status = {
      .addr = NULL,
      .count = 0,
  };
  walk_heap(root(heap), flag_non_commit, (void *)&status);

  assert(status.addr == 0);
}

void set_committed(struct heap_leaf *frame, void *d) {
  // fprintf(stderr, "committing - %p\n", frame);
  int *flagged = (int *)d;
  (*flagged)++;
  frame->committed = 1;
}

void commit(struct heap_header *heap) {
  fprintf(stderr, "BEGINNING COMMIT\n");
  int flagged = 0;
  walk_heap(root(heap), set_committed, &flagged);

  fprintf(stderr, "committed %d nodes\n", flagged);

  fprintf(stderr, "SETTING READONLY\n");

  void *start_addr = (void *)heap + USER_DATA_DIST;
  size_t len = heap->size - USER_DATA_DIST;

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

  fprintf(stderr, "rev: %d, frame: %p\n", heap->working,
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

void *create_next_frame(struct heap_frame *frame) {
  int pindex = NODE_CHILDREN - 1;

  frame->ctype[pindex] = FRAME;
  frame->c[pindex] = (void *)frame + sizeof(struct heap_frame);

  return frame->c[pindex];
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

  void *left;

  if (free_index == NODE_CHILDREN - 1) {
    left = parent->c[free_index - 1];
    parent = create_next_frame(parent);
    free_index = 0;
  } else {
    left = parent->c[free_index - 1];
  }

  struct heap_leaf *l =
      left + sizeof(struct heap_leaf) + ((struct heap_leaf *)left)->size;

  parent->ctype[free_index] = USED_LEAF;
  parent->c[free_index] = l;

  l->size = n;
  l->committed = 0;

  return (void *)l + sizeof(struct heap_leaf);
}

void snap_free(struct heap_header *heap, void *ptr) {
  struct heap_leaf *l = (struct heap_leaf *)(ptr - sizeof(struct heap_leaf));

  int i;
  struct heap_frame *p = find_parent(root(heap), l, &i);
  assert(p->ctype[i] == USED_LEAF);

  p->ctype[i] = FREE_LEAF;
}

void *snap_realloc(struct heap_header *heap, void *ptr, size_t n) {
  void *result = snap_malloc(heap, n);
  memmove(result, ptr, n);
  snap_free(heap, ptr);
  return result;
}

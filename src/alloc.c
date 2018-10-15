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

void handle_segv(int signum, siginfo_t *i, void *d) {
  void *addr = i->si_addr;

  if (addr < MAP_START_ADDR ||
      MAP_START_ADDR >= MAP_START_ADDR + ALLOC_BLOCK_SIZE) {
    fprintf(stderr, "SEGFAULT %p\n", i->si_addr);
    exit(2);
  }

  int page_size = getpagesize();

  void *page = (void *)((uintptr_t)addr & ~((uintptr_t)page_size - 1));

  fprintf(stderr, "! hit %p, marked %p-%p\n", addr, page, page + page_size);

  int err = mprotect(page, page_size, PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }
}

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

  void *mem =
      mmap(MAP_START_ADDR, size, PROT_READ, MAP_FIXED | MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED) {
    fprintf(stderr, "db map failed  %s\n", strerror(errno));
    return NULL;
  }

  return mem;
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

  struct sigaction sa;

  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sa.sa_sigaction = handle_segv;
  sigaction(SIGSEGV, &sa, NULL);

  void *mem = open_db(db_path, ALLOC_BLOCK_SIZE);

  struct heap_header *heap = mem;

  if (heap->v == 0) {
    heap->v = 0xffca;
    heap->size = ALLOC_BLOCK_SIZE;

    heap->root = mem + sizeof(struct heap_header);
    heap->root->size =
        heap->size - sizeof(struct heap_header) - sizeof(struct heap_frame);
  } else if (heap->v != 0xffca) {
    fprintf(
        stderr,
        "got a bad snapshot, expected version %d (expected) != %d (actual)",
        0xffca,
        heap->v);
    exit(1);
  }

  return heap;
}

void mem_tree_traverse(struct heap_frame *frame, int depth) {
  char pad[1024];
  memset(pad, ' ', depth * 2);
  pad[depth * 2] = '\0';

  printf("%s┌", pad);
  for (int i = 0; i < 27; i++) {
    printf("─");
  }
  printf("\n%s│", pad);
  printf(" N %p %08ld\n", frame, frame->size);

  for (int i = 0; i < NODE_CHILDREN; i++) {
    char pad[1024];
    memset(pad, ' ', (depth + 1) * 2);
    pad[(depth + 1) * 2] = '\0';

    if (frame->ctype[i] == USED_LEAF) {
      printf("%s┌", pad);
      for (int i = 0; i < 27; i++) {
        printf("─");
      }
      printf("\n%s│", pad);
      printf(
          " L %p %ld b\n",
          frame->c[i],
          ((struct heap_leaf *)frame->c[i])->size);

    } else if (frame->ctype[i] == FREE_LEAF) {
      printf("%s┌", pad);
      for (int i = 0; i < 27; i++) {
        printf("─");
      }
      printf("\n%s│", pad);
      printf(" FREE %ld b\n", ((struct heap_leaf *)frame->c[i])->size);
    } else if (frame->ctype[i] == EMPTY) {
      printf("%s┌", pad);
      for (int i = 0; i < 27; i++) {
        printf("─");
      }
      printf("\n%s│", pad);
      printf(" EMPTY\n");
    } else if (frame->ctype[i] == FRAME) {
      mem_tree_traverse(frame->c[i], depth + 1);
    }
  }
}

void print_mem_tree(struct heap_header *heap) {
  mem_tree_traverse(heap->root, 0);
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
  struct heap_frame *parent = heap->root;

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
  struct heap_frame *p = find_parent(heap->root, l, &i);
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

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

// lol namespaces
#define page snap_page
#define segment snap_segment
#define generation snap_generation
#define node snap_node

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

void *round_page_down(void *addr) {
  return (void *)((uintptr_t)addr & ~(PAGE_SIZE - 1));
}

void *round_page_up(void *addr) {
  if ((uintptr_t)addr & (PAGE_SIZE - 1)) {
    return round_page_down((char *)addr + PAGE_SIZE);
  }

  return addr;
}

struct page *new_page(struct heap_header *heap, size_t pages) {
  struct page *f = heap->last_page =
      (struct page
           *)((char *)heap->last_page + (heap->last_page->pages * PAGE_SIZE));

  *f = (struct page){
      .i = {.type = NODE_PAGE, .committed = 0},
      .pages = pages,
      .len = 0,
  };

  return f;
}

struct page *copy_generation(struct heap_header *h, struct page *p) {
  struct page *new = new_page(h, p->pages);
  memcpy(new, p, p->pages * PAGE_SIZE);
  return new;
}

void copy_dirty_pages(struct heap_header *heap, struct page *p) {}

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
      (char *)MAP_START_ADDR >= (char *)MAP_START_ADDR + ALLOC_BLOCK_SIZE) {
    fprintf(stderr, "SEGFAULT trying to read %p\n", addr);
    exit(2);
  }

  if (segv_handle_heap->committed == segv_handle_heap->working) {
    fprintf(stderr, "SEGFAULT write outside of mutation %p\n", addr);
    exit(2);
  }

  assert(heap->working != heap->committed);

  void *page = round_page_down(addr);

  struct page *f = page;
  // assert(f->committed == 1);

  int err = mprotect(page, PAGE_SIZE, PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  copy_dirty_pages(heap, f);

  fprintf(
      stderr,
      "! hit %p, marked %p-%p\n",
      addr,
      page,
      (char *)page + PAGE_SIZE - 1);

  handling_segv = NULL;
}

struct heap_header *snap_init(char *argv[], char *db_path) {
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
    heap->last_gen_index = 0;

    struct page *initial_page = heap->last_page =
        (struct page *)USER_DATA_START_ADDR;

    *initial_page = (struct page){
        .i = {.type = NODE_PAGE, .committed = 0},
        .pages = 1,
        .len = 0,
    };

    struct generation *initial_gen = heap->last_gen =
        (struct generation *)((char *)heap + sizeof(struct heap_header));

    *initial_gen = (struct generation){
        .i = {.type = NODE_GENERATION, .committed = 0},
        .gen = heap->last_gen_index,
        .c = {(struct node *)initial_page},
    };

    heap->working = initial_gen;
    heap->committed = initial_gen;
    heap->root = initial_gen;

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

int walk_nodes(struct node *n, int (*cb)(struct node *, void *), void *d) {
  if (cb(n, d)) {
    return 1;
  }

  if (n->type == NODE_GENERATION) {
    struct generation *g = (struct generation *)n;
    for (int i = 0; i < GENERATION_CHILDREN; i++) {
      if (!g->c[i]) {
        continue;
      }

      if (walk_nodes(g->c[i], cb, d)) {
        return 1;
      }
    }
  }

  return 0;
}

struct non_commit_status {
  void *addr;
  int count;
};

int flag_non_commit(struct node *n, void *d) {
  struct non_commit_status *status = (struct non_commit_status *)d;
  if (!n->committed) {
    status->count++;
    if (!status->addr) {
      status->addr = n;
    }
  }

  return 0;
}

void verify_all_committed(struct heap_header *heap) {
  struct non_commit_status status = {
      .addr = NULL,
      .count = 0,
  };
  walk_nodes((struct node *)heap->root, flag_non_commit, (void *)&status);

  assert(status.addr == 0);
}

int set_committed(struct node *n, void *d) {
  int *flagged = (int *)d;
  (*flagged)++;
  n->committed = 1;

  return 0;
}

void snap_commit(struct heap_header *heap) {
  fprintf(stderr, "BEGINNING COMMIT\n");
  int flagged = 0;
  walk_nodes((struct node *)heap->root, set_committed, &flagged);

  fprintf(stderr, "committed %d pages\n", flagged);

  fprintf(stderr, "SETTING READONLY\n");

  void *start_addr = round_page_up((char *)heap + sizeof(struct heap_header));
  size_t len = (INITIAL_PAGES - 1) * PAGE_SIZE;

  int err = mprotect(start_addr, len, PROT_READ);
  if (err != 0) {
    fprintf(stderr, "failed to unmark write pages %s\n", strerror(errno));
    fprintf(stderr, "%p - %p", start_addr, (char *)start_addr + len - 1);
    exit(3);
  }

  verify_all_committed(heap);
  fprintf(stderr, "UPDATING COMMIT BIT\n");

  heap->committed = heap->working;

  fprintf(stderr, "COMMIT COMPLETE\n");
}

int first_free(struct generation *gen) {
  for (int i = 0; i < GENERATION_CHILDREN; i++) {
    if (!gen->c[i]) {
      return i;
    }
  }

  return -1;
}

struct generation *new_gen(struct heap_header *h, int index) {
  struct generation *new = ++h->last_gen;

  *new = (struct generation){
      .i = {.type = NODE_GENERATION, .committed = 0},
      .gen = index,
      .c = {0},
  };

  return new;
}

void snap_begin_mut(struct heap_header *heap) {
  fprintf(stderr, "BEGINNING MUT\n");

  assert(heap->working == heap->committed);
  verify_all_committed(heap);

  fprintf(
      stderr,
      "rev: %d, generation at: %p\n",
      heap->working->gen,
      (void *)heap->working);

  struct generation *last = heap->committed;

  int nexti = first_free(last);
  assert(nexti != -1);

  struct generation *next = new_gen(heap, ++heap->last_gen_index);

  last->c[nexti] = (struct node *)next;

  fprintf(stderr, "working now: %d\n", heap->working->gen);

  assert(heap->working != heap->committed);
  assert(heap->committed->gen != heap->working->gen);
}

size_t page_head_size(int ch) {
  return sizeof(struct page) + (sizeof(struct page *) * ch);
}

void *page_data_start(struct page *p) {
  if (!p->len) {
    return (char *)p + (PAGE_SIZE * p->pages) - 1;
  }

  return (char *)p->c[p->len - 1] - 1;
}

int page_can_fit(struct page *p, size_t b) {
  return ((char *)page_data_start(p) - b) -
             ((char *)p + page_head_size(p->len + 1)) >=
         0;
}

struct segment *page_new_segment(struct page *p, size_t bytes) {
  assert(page_can_fit(p, bytes));

  void *last_free = page_data_start(p);
  struct segment *seg =
      (struct segment
           *)(((char *)last_free + 1) - (bytes + sizeof(struct segment)));

  p->c[p->len] = seg;
  p->len++;

  seg->size = bytes;
  seg->used = 1;

  return seg;
}

struct gen_match_info {
  int target;
  struct generation *mismatch;
};

int all_gen_match(struct node *n, void *d) {
  if (n->type == NODE_GENERATION) {
    struct gen_match_info *info = d;
    struct generation *g = (struct generation *)n;
    if (info->target != g->gen) {
      info->mismatch = g;
      return 1;
    }
  }

  return 0;
}

struct first_fit {
  size_t size;
  struct page *p;
};

int find_first_fit(struct node *n, void *d) {
  if (n->type == NODE_PAGE) {
    struct page *p = (struct page *)n;
    struct first_fit *ff = d;
    if (page_can_fit(p, ff->size)) {
      ff->p = p;
      return 1;
    }
  }

  return 0;
}

void *snap_malloc(struct heap_header *heap, size_t bytes) {
  struct generation *g = heap->working;

  struct gen_match_info m = {.target = g->gen, .mismatch = NULL};
  walk_nodes((struct node *)g, all_gen_match, &m);
  assert(m.mismatch == NULL);

  struct first_fit fit = {.size = bytes, .p = NULL};
  walk_nodes((struct node *)g, find_first_fit, &fit);
  assert(fit.p != NULL);

  struct segment *s = page_new_segment(fit.p, bytes);

  return (char *)s + sizeof(struct segment);
}

void snap_free(struct heap_header *heap, void *ptr) {
  struct segment *l = (struct segment *)((char *)ptr - sizeof(struct segment));

  assert(l->used);
  l->used = 0;
}

void *snap_realloc(struct heap_header *heap, void *ptr, size_t bytes) {
  void *result = snap_malloc(heap, bytes);
  memmove(result, ptr, bytes);
  snap_free(heap, ptr);
  return result;
}

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
#define gen snap_snapgen

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

struct page *root(struct heap_header *heap) {
  return heap->revs[heap->working];
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
  struct page *f = heap->last_frame =
      (struct page
           *)((char *)heap->last_frame + (heap->last_frame->pages * PAGE_SIZE));

  *f = (struct page){
      .committed = 0,
      .pages = pages,
      .len = 0,
  };

  return f;
}

struct page *copy_frame(struct heap_header *h, struct page *frame) {
  struct page *new = new_page(h, frame->pages);
  memcpy(new, frame, frame->pages * PAGE_SIZE);
  return new;
}

void copy_dirty_pages(struct heap_header *heap, struct page *frame) {}

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

    int initial_rev = 0;

    heap->working = initial_rev;
    heap->committed = initial_rev;

    heap->revs[initial_rev] =
        round_page_up((char *)heap + sizeof(struct heap_header));
    heap->last_frame = heap->revs[initial_rev];
    *heap->revs[initial_rev] = (struct page){
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

void walk_frames(struct page *f, void (*cb)(struct page *, void *), void *d) {

  cb(f, d);

  if (f->next) {
    walk_frames(f->next, cb, d);
  }
}

struct non_commit_status {
  void *addr;
  int count;
};

void flag_non_commit(struct page *f, void *d) {
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
  walk_frames((struct page *)root(heap), flag_non_commit, (void *)&status);

  assert(status.addr == 0);
}

void set_committed(struct page *f, void *d) {
  int *flagged = (int *)d;
  (*flagged)++;
  f->committed = 1;
}

void snap_commit(struct heap_header *heap) {
  fprintf(stderr, "BEGINNING COMMIT\n");
  int flagged = 0;
  walk_frames((struct page *)root(heap), set_committed, &flagged);

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

  verify(heap);
  fprintf(stderr, "UPDATING COMMIT BIT\n");

  heap->committed = heap->working;

  fprintf(stderr, "COMMIT COMPLETE\n");
}

void snap_begin_mut(struct heap_header *heap) {
  fprintf(stderr, "BEGINNING MUT\n");

  assert(heap->working == heap->committed);
  verify(heap);

  fprintf(
      stderr,
      "rev: %d, frame: %p\n",
      heap->working,
      (void *)heap->revs[heap->committed]);

  heap->working++;
  heap->revs[heap->working] = heap->revs[heap->committed];

  fprintf(stderr, "working: %d\n", heap->working);

  assert(heap->working != heap->committed);
  assert(heap->revs[heap->working] == heap->revs[heap->committed]);
}

size_t nsize(int c) {
  return sizeof(struct page) + (sizeof(struct page *) * c);
}

void *n_last_free(struct page *p) {
  if (!p->len) {
    return (char *)p + (PAGE_SIZE * p->pages) - 1;
  }

  return (char *)p->c[p->len - 1] - 1;
}

struct segment *page_new_segment(struct page *p, size_t bytes) {
  void *last_free = n_last_free(p);

  if (((char *)last_free - bytes) - ((char *)p + nsize(p->len + 1)) <= 0) {
    return NULL;
  }

  struct segment *addr = (struct segment *)((char *)last_free - bytes);

  p->c[p->len] = addr;
  p->len++;

  addr->size = bytes;
  addr->used = 1;

  return addr;
}

struct page *n_new_child(struct heap_header *heap, struct page *p, size_t min) {
  struct page *page = new_page(heap, (min / PAGE_SIZE) + 1);

  p->next = (struct page *)page;

  return page;
}

void *snap_malloc(struct heap_header *heap, size_t bytes) {
  struct page *p = root(heap);

  while (p->next) {
    p = p->next;
  }

  struct segment *l = page_new_segment(p, bytes + sizeof(struct segment));
  if (l == NULL) {
    p = n_new_child(heap, p, bytes);
    l = page_new_segment(p, bytes + sizeof(struct segment));
  }

  assert(l != NULL);

  return (char *)l + sizeof(struct segment);
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

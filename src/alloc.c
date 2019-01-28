#define _XOPEN_SOURCE 600
#define _GNU_SOURCE

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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "alloc.h"

// lol namespaces
#define page snap_page
#define segment snap_segment
#define generation snap_generation
#define node snap_node

#ifdef DEBUG_LOGGING
#define SNAP_EVENT_LOG_PRECOMMITED
#define SNAP_EVENT_LOG_FILE "/dev/stderr"
#endif

#define DEBUG_LOGGING

#ifdef SNAP_EVENT_LOG_FILE
FILE *event_log;
#endif

struct mapping {
  char w;
  void *start;
  size_t len;
};

struct runtime_state {
  char *db_path;
  struct heap_header *heap;
  void *handling_segv;
};

static struct runtime_state rstate;

#define H (rstate.heap)

#ifdef DEBUG_LOGGING
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...)
#endif

int open_db(const char *path, struct runtime_state *state) {
  LOG("opening db at %s\n", path);

  struct stat stat_info;

  int err = stat(path, &stat_info);
  if (err) {
    fprintf(stderr, "could not stat db: %s\n", strerror(errno));
    return -1;
  }

  if ((stat_info.st_mode & S_IFMT) != S_IFREG) {
    fprintf(stderr, "unexpected file type, wanted regular\n");
    return -1;
  }

  int fd = open(path, O_RDWR, 0660);
  if (fd == -1) {
    fprintf(stderr, "couldn't open db %s\n", strerror(errno));
    return -1;
  }

  // we'll initially just map the first page just to grab info
  void *mem = mmap(
      MAP_START_ADDR,
      PAGE_SIZE,
      PROT_READ | PROT_WRITE,
      MAP_FIXED | MAP_SHARED,
      fd,
      0);
  if (mem == MAP_FAILED) {
    fprintf(stderr, "db map failed  %s\n", strerror(errno));
    return -1;
  }

  struct heap_header *heap = (struct heap_header *)mem;

  if (heap->v != 0xffca) {
    fprintf(
        stderr,
        "got a bad snapshot, invalid magic seq 0x%x (expected) != 0x%x "
        "(actual)\n",
        0xffca,
        heap->v);
    return -1;
  }

  if (heap->map_start != MAP_START_ADDR) {
    assert(0 && "TODO");
  }

  LOG("resizing initial map from %lx to %lx\n", PAGE_SIZE, heap->size);

  void *new_addr = mremap(MAP_START_ADDR, PAGE_SIZE, heap->size, 0);
  if (new_addr == (void *)-1) {
    fprintf(stderr, "db remap failed  %s\n", strerror(errno));
    return -1;
  }
  assert(new_addr == heap->map_start && new_addr == MAP_START_ADDR);

  state->heap = mem;

  return 0;
}

int new_db(const char *path) {
  LOG("creating db at %s\n", path);

  int fd = open(path, O_CREAT | O_RDWR, 0660);
  if (fd == -1) {
    fprintf(stderr, "couldn't open db %s\n", strerror(errno));
    return -1;
  }

  size_t initial_size = INITIAL_PAGES * PAGE_SIZE;
  void *map_start = MAP_START_ADDR;

  int err = ftruncate(fd, initial_size);
  if (err) {
    fprintf(stderr, "could not increase db size  %s\n", strerror(errno));
    return -1;
  }

  // we'll initially just map the first page to figure out the real size
  void *mem = mmap(
      map_start,
      initial_size,
      PROT_READ | PROT_WRITE,
      MAP_FIXED | MAP_SHARED,
      fd,
      0);
  if (mem == MAP_FAILED) {
    fprintf(stderr, "db map failed  %s\n", strerror(errno));
    return -1;
  }

  struct heap_header *heap = mem;

  heap->v = 0xffca;
  heap->size = initial_size;
  heap->last_gen_index = 0;
  heap->page_size = PAGE_SIZE;
  heap->map_start = map_start;

  struct page *initial_page = heap->last_page =
      (struct page *)((char *)mem + (PAGE_SIZE * (INITIAL_PAGES / 2)));

  *initial_page = (struct page){
      .i = {.type = SNAP_NODE_PAGE, .committed = 0},
      .pages = 1,
      .len = 0,
      .real_addr = initial_page,
  };

  struct generation *initial_gen = heap->last_gen =
      (struct generation *)((char *)heap + sizeof(struct heap_header));

  *initial_gen = (struct generation){
      .i = {.type = SNAP_NODE_GENERATION, .committed = 0},
      .gen = heap->last_gen_index,
      .c = {(struct node *)initial_page},
  };

  heap->working = initial_gen;
  heap->committed = NULL;
  heap->root = initial_gen;

  munmap(heap->map_start, initial_size);

  return 0;
}

uintptr_t round_page_down(uintptr_t addr) { return (addr & ~(PAGE_SIZE - 1)); }

uintptr_t round_page_up(uintptr_t addr) {
  if (addr & (PAGE_SIZE - 1)) {
    return round_page_down(addr + PAGE_SIZE);
  }

  return addr;
}

enum walk_action {
  WALK_CONTINUE = 0,
  WALK_EXIT = 1,
  WALK_SKIP = 2,
};

int walk_nodes(struct node *n, int (*cb)(struct node *, void *), void *d) {
  int action = cb(n, d);
  if (action) {
    return action;
  }

  if (n->type == SNAP_NODE_GENERATION) {
    struct generation *g = (struct generation *)n;
    for (int i = 0; i < GENERATION_CHILDREN; i++) {
      if (!g->c[i]) {
        continue;
      }

      if (walk_nodes(g->c[i], cb, d) == WALK_EXIT) {
        return WALK_EXIT;
      }
    }
  }

  return WALK_CONTINUE;
}

struct page_from_hit {
  void *hit;
  struct page *p;
  int index;
};
int find_page(struct node *n, void *d) {
  if (n->type == SNAP_NODE_PAGE) {
    struct page *p = (struct page *)n;
    struct page_from_hit *phit = (struct page_from_hit *)d;

    if (phit->hit >= (void *)((char *)p + sizeof(struct page)) &&
        phit->hit < (void *)((char *)p + (PAGE_SIZE * p->pages))) {
      phit->p = p;

      for (int i = 0; i < p->len; i++) {
        struct segment *s = p->c[i];
        if (phit->hit >= (void *)((char *)s) &&
            phit->hit <
                (void *)((char *)s + sizeof(struct segment) + s->size)) {
          phit->index = i;
          break;
        }
      }

      return WALK_EXIT;
    }
  }

  return WALK_CONTINUE;
}

struct node_rel {
  struct node *child;
  struct node *parent;
  int index;
};
int find_parent(struct node *n, void *d) {
  if (n->type == SNAP_NODE_GENERATION) {
    struct generation *g = (struct generation *)n;
    struct node_rel *l = d;
    for (int i = 0; i < GENERATION_CHILDREN; i++) {
      if (l->child == g->c[i]) {
        l->parent = n;
        l->index = i;
        return WALK_EXIT;
      }
    }
  }

  return WALK_CONTINUE;
}

struct tree_slot {
  int index;
  struct generation *target;
};
int first_free_slot(struct node *n, void *d) {
  if (n->type == SNAP_NODE_GENERATION) {
    struct generation *g = (struct generation *)n;
    struct tree_slot *slot = d;

    for (int i = 0; i < GENERATION_CHILDREN; i++) {
      if (!g->c[i]) {
        slot->target = g;
        slot->index = i;
        return WALK_EXIT;
      }
    }
  }

  return WALK_CONTINUE;
}

int verify_all_committed(struct node *n, void *d) {
  if (!n->committed) {
    fprintf(stderr, "node %p was no committed\n", (void *)n);
    assert(0);
  }

  return WALK_CONTINUE;
}

void expand_heap(struct heap_header *heap, size_t min_expand) {
  size_t new_size = heap->size * 2;

  if (new_size - heap->size < min_expand * 2) {
    new_size = round_page_up(min_expand * 2);
  }

  int err = truncate(rstate.db_path, new_size);
  if (err) {
    fprintf(stderr, "could not increase db file size %s\n", strerror(errno));
    exit(1);
  }

  size_t expand_by = new_size - heap->size;

  LOG("resizing map from %lx to %lx (+%lx) to accomadate %lx\n",
      heap->size,
      new_size,
      expand_by,
      min_expand);

  // fprintf(stderr, "taking last map %p from %lx to %lx\n", start, old_len,
  // new_len);

  void *new_addr = mremap(heap->map_start, heap->size, new_size, 0);
  if (new_addr == (void *)-1) {
    fprintf(stderr, "db expand failed %s\n", strerror(errno));
    exit(1);
  }
  assert(new_addr == heap->map_start);

  heap->size = new_size;
}

struct page *new_page(struct heap_header *heap, size_t pages) {
  struct page *f = heap->last_page =
      (struct page
           *)((char *)heap->last_page + (heap->last_page->pages * PAGE_SIZE));

  char *npage_end = (char *)f + (pages * PAGE_SIZE);
  char *heap_end = (char *)heap->map_start + heap->size;

  if (npage_end >= heap_end) {
    expand_heap(heap, pages * PAGE_SIZE);
  }

  assert(npage_end < (char *)heap->map_start + heap->size);

  *f = (struct page){
      .i = {.type = SNAP_NODE_PAGE, .committed = 0},
      .pages = pages,
      .len = 0,
      .real_addr = f,
  };

  return f;
}

int set_committed(struct node *n, void *d) {
  int *flagged = (int *)d;
  (*flagged)++;
  n->committed = 1;

  return WALK_CONTINUE;
}

struct page *page_copy(struct heap_header *h, struct page *p) {
  struct page *new = new_page(h, p->pages);
  memcpy(new, p, p->pages * PAGE_SIZE);

  // offset all the segment pointers so they're still correct when the
  // page is moved. Useful for analyzing the situation without remapping.
  for (int i = 0; i < p->len; i++) {
    new->c[i] =
        (struct segment *)((char *)new->c[i] + ((char *)new - (char *)p));
  }

  return new;
}

// TODO(turbio): just do some remapping
void page_swap(struct heap_header *heap, struct page *p1, struct page *p2) {
  assert(p1->pages == p2->pages);

  LOG("swapping pages %p <-> %p\n", (void *)p1, (void *)p2);

  int err = mprotect((void *)p1, PAGE_SIZE * p1->pages, PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  err = mprotect((void *)p2, PAGE_SIZE * p1->pages, PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  void *tmp = (char *)heap->last_page + (heap->last_page->pages * PAGE_SIZE);

  memcpy(tmp, p1, p1->pages * PAGE_SIZE);
  memcpy(p1, p2, p1->pages * PAGE_SIZE);
  memcpy(p2, tmp, p1->pages * PAGE_SIZE);

  for (int i = 0; i < p2->len; i++) {
    p2->c[i] = (struct segment *)((char *)p2->c[i] + ((char *)p2 - (char *)p1));
    p1->c[i] = (struct segment *)((char *)p1->c[i] + ((char *)p1 - (char *)p2));
  }

  err = mprotect((void *)p1, PAGE_SIZE * p1->pages, PROT_READ);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  err = mprotect((void *)p2, PAGE_SIZE * p1->pages, PROT_READ);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }
}

struct generation *new_gen(struct heap_header *h, int index) {
  struct generation *new = ++h->last_gen;

  *new = (struct generation){
      .i = {.type = SNAP_NODE_GENERATION, .committed = 0},
      .gen = index,
      .c = {0},
  };

  return new;
}

// new_gen_between will free up space on a generation by creating a child and
// moving half its children to said child.
struct generation *
new_gen_between(struct heap_header *heap, struct generation *parent) {
  struct generation *child = new_gen(heap, parent->gen);
  child->i.committed = parent->i.committed;

  for (int i = 0; i < GENERATION_CHILDREN; i++) {
    child->c[i] = parent->c[i];
    parent->c[i] = NULL;
  }

  parent->c[0] = (struct node *)child;

  return child;
}

void handle_segv(int signum, siginfo_t *i, void *d) {
  void *addr = i->si_addr;

  if (rstate.handling_segv) {
    fprintf(
        stderr,
        "SEGFAULT at %p while already handling SEGV for %p\n",
        addr,
        rstate.handling_segv);
    exit(2);
  }

  rstate.handling_segv = addr;

  if (addr < H->map_start ||
      (char *)H->map_start >= (char *)H->map_start + H->size) {
    fprintf(stderr, "SEGFAULT trying to read %p\n", addr);
    exit(2);
  }

  if (H->committed == H->working) {
    fprintf(stderr, "SEGFAULT write outside of mutation %p\n", addr);
    exit(2);
  }

  LOG("! hit %p\n", addr);

  assert(H->working != H->committed);

  struct page_from_hit phit = {.hit = addr, .p = NULL, .index = -1};
  walk_nodes((struct node *)H->root, find_page, (void *)&phit);

  LOG("! turns out the page is %p\n", (void *)phit.p);

  assert(phit.p != NULL);
  assert(phit.index != -1);

  struct page *hit_page = phit.p;

  assert(hit_page->i.type == SNAP_NODE_PAGE);
  assert(hit_page->i.committed);

  int err = mprotect(
      (void *)hit_page, hit_page->pages * PAGE_SIZE, PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  struct node_rel rel = {
      .parent = NULL,
      .index = -1,
      .child = (struct node *)hit_page,
  };
  walk_nodes((struct node *)H->root, find_parent, &rel);
  assert(rel.parent != NULL);
  assert(rel.index != -1);
  assert(rel.parent->type == SNAP_NODE_GENERATION);

  struct page *fresh_page = page_copy(H, hit_page);

  struct tree_slot slot = {.index = -1, .target = NULL};
  walk_nodes((struct node *)H->working, first_free_slot, &slot);

  if (slot.target == NULL) {
    LOG("did one of those fancy moves\n");

    new_gen_between(H, H->working);

    slot = (struct tree_slot){.index = -1, .target = NULL};
    walk_nodes((struct node *)H->working, first_free_slot, &slot);
  }

  assert(slot.target != NULL);
  assert(slot.index != -1);

  fresh_page->i.committed = 1;
  ((struct generation *)rel.parent)->c[rel.index] = (struct node *)fresh_page;

  hit_page->i.committed = 0;
  slot.target->c[slot.index] = (struct node *)hit_page;

  LOG("! duplicated %p-%p to %p\n",
      (void *)hit_page,
      (char *)hit_page + PAGE_SIZE - 1,
      (void *)fresh_page);

  rstate.handling_segv = NULL;
}

struct heap_header *snap_init(char *path) {
#ifdef SNAP_EVENT_LOG_FILE
  int logfd = open(SNAP_EVENT_LOG_FILE, O_CREAT | O_RDWR, 0600);
  if (logfd == -1) {
    fprintf(stderr, "couldn't open event log %s\n", strerror(errno));
    return NULL;
  }

  event_log = fdopen(logfd, "a");
#endif

  rstate.db_path = path;

  struct stat stat_info;

  int err = stat(path, &stat_info);
  if (err && errno == ENOENT) {
    if (new_db(path)) {
      fprintf(stderr, "couldn't creat initial db\n");
      return NULL;
    }
  }

  int notok = open_db(path, &rstate);
  if (notok) {
    return NULL;
  }

  static struct sigaction segv_action;

  segv_action.sa_flags = SA_SIGINFO | SA_NODEFER;
  segv_action.sa_sigaction = handle_segv;
  sigaction(SIGSEGV, &segv_action, NULL);

  return rstate.heap;
}

int snap_commit(struct heap_header *heap) {
#ifdef SNAP_EVENT_LOG_FILE
  fprintf(event_log, "snap_commit\n");
  fflush(event_log);
#endif

  LOG("BEGINNING COMMIT\n");

  int flagged = 0;
  walk_nodes((struct node *)heap->working, set_committed, &flagged);

  LOG("committed %d nodes\n", flagged);
  LOG("SETTING READONLY\n");

  walk_nodes((struct node *)heap->root, verify_all_committed, NULL);

  LOG("UPDATING COMMIT BIT\n");

  heap->committed = heap->working;

  LOG("COMMIT COMPLETE\n");

  return heap->committed->gen;
}

struct up_to_gen_state {
  int max_gen;
  struct page **p;
  int count;
};
int pages_up_to_gen(struct node *n, void *d) {
  struct up_to_gen_state *s = (struct up_to_gen_state *)d;

  if (n->type == SNAP_NODE_GENERATION) {
    struct generation *g = (struct generation *)n;

    if (g->gen > s->max_gen) {
      return WALK_SKIP;
    }

    return WALK_CONTINUE;
  }

  struct page *page = (struct page *)n;

  if (s->p) {
    for (int i = 0; i < s->count; i++) {
      if (s->p[i]->real_addr == page->real_addr) {
        s->p[i] = page;
        goto exit;
      }
    }

    s->p[s->count] = page;
  }

  s->count++;

exit:
  return WALK_CONTINUE;
}

struct gen_from_id {
  int id;
  struct generation *g;
};
int first_gen_for_id(struct node *n, void *d) {
  if (n->type == SNAP_NODE_GENERATION) {
    struct generation *g = (struct generation *)n;
    struct gen_from_id *f = (struct gen_from_id *)d;

    if (g->gen == f->id) {
      f->g = g;
      return WALK_EXIT;
    }
  }

  return WALK_CONTINUE;
}

void snap_checkout(struct heap_header *heap, int genid) {

  LOG("BEGINNING CHECKOUT\n");

  assert(heap->committed == heap->working);

  struct gen_from_id fid = {.id = genid, .g = NULL};
  walk_nodes((struct node *)heap->root, first_gen_for_id, &fid);
  assert(fid.g != NULL);
  assert(fid.g->gen == genid);

  if (heap->committed->gen == genid) {
    LOG("CHECKOUT DONE\n");
    return;
  }

  struct up_to_gen_state s = {
      .max_gen = genid,
      .p = NULL,
      .count = 0,
  };
  walk_nodes((struct node *)heap->root, pages_up_to_gen, &s);

  struct page *to_swap[s.count];

  s = (struct up_to_gen_state){
      .max_gen = genid,
      .p = to_swap,
      .count = 0,
  };
  walk_nodes((struct node *)heap->root, pages_up_to_gen, &s);

  LOG("swapping %d pages\n", s.count);

  for (int i = 0; i < s.count; i++) {
    if (s.p[i] != s.p[i]->real_addr) {
      struct node_rel rel1 = {
          .parent = NULL,
          .index = -1,
          .child = (struct node *)s.p[i],
      };
      walk_nodes((struct node *)heap->root, find_parent, &rel1);
      assert(rel1.parent != NULL);
      assert(rel1.index != -1);
      assert(rel1.parent->type == SNAP_NODE_GENERATION);

      struct node_rel rel2 = {
          .parent = NULL,
          .index = -1,
          .child = (struct node *)s.p[i]->real_addr,
      };
      walk_nodes((struct node *)heap->root, find_parent, &rel2);
      assert(rel2.parent != NULL);
      assert(rel2.index != -1);
      assert(rel2.parent->type == SNAP_NODE_GENERATION);

      ((struct generation *)rel1.parent)->c[rel1.index] =
          (struct node *)s.p[i]->real_addr;
      ((struct generation *)rel2.parent)->c[rel2.index] = (struct node *)s.p[i];

      page_swap(heap, s.p[i], s.p[i]->real_addr);
    }
  }

  heap->committed = fid.g;
  heap->working = fid.g;

  LOG("COMPLETED CHECKOUT\n");
}

int snap_begin_mut(struct heap_header *heap) {
#ifdef SNAP_EVENT_LOG_FILE
  fprintf(event_log, "snap_begin_mut\n");
  fflush(event_log);
#endif

  LOG("BEGINNING MUT\n");

  assert(heap->working == heap->committed);
  walk_nodes((struct node *)heap->root, verify_all_committed, NULL);

  LOG("rev: %d, generation at: %p\n",
      heap->working->gen,
      (void *)heap->working);

  struct tree_slot slot = {.index = -1, .target = NULL};
  walk_nodes((struct node *)heap->committed, first_free_slot, &slot);

  if (slot.target == NULL) {
    new_gen_between(heap, heap->committed);
    slot = (struct tree_slot){.index = -1, .target = NULL};
    walk_nodes((struct node *)heap->committed, first_free_slot, &slot);
  }

  assert(slot.target != NULL);
  assert(slot.index != -1);

  struct generation *next = new_gen(heap, ++heap->last_gen_index);

  slot.target->c[slot.index] = (struct node *)next;
  heap->working = next;

  LOG("working now: %d\n", heap->working->gen);

  assert(heap->working != heap->committed);
  assert(heap->committed->gen != heap->working->gen);

  walk_nodes((struct node *)heap->committed, set_readonly, NULL);

  return heap->working->gen;
}

void *page_head_end(struct page *p) {
  return (char *)p + sizeof(struct page) + (sizeof(struct segment *) * p->len);
}

void *page_data_start(struct page *p) {
  if (!p->len) {
    return (char *)p + (PAGE_SIZE * p->pages) - 1;
  }

  return (char *)p->c[p->len - 1] - 1;
}

// can the page fit an additional segment of size n
int page_can_fit(struct page *p, size_t n) {
  return (char *)((char *)page_data_start(p) - (char *)page_head_end(p)) >
         (char *)((n + sizeof(void *) + sizeof(struct segment)));
}

struct segment *page_new_segment(struct page *p, size_t bytes) {
  assert(page_can_fit(p, bytes));

  void *last_free = page_data_start(p);
  struct segment *seg =
      (struct segment
           *)(((char *)last_free + 1) - (bytes + sizeof(struct segment)));

  *seg = (struct segment){
      .used = 1,
      .size = bytes,
  };

  p->c[p->len] = seg;
  p->len++;

  return seg;
}

struct gen_match_info {
  int target;
  struct generation *mismatch;
};
int all_gen_match(struct node *n, void *d) {
  if (n->type == SNAP_NODE_GENERATION) {
    struct gen_match_info *info = d;
    struct generation *g = (struct generation *)n;
    if (info->target != g->gen) {
      info->mismatch = g;
      return WALK_EXIT;
    }
  }

  return WALK_CONTINUE;
}

struct page_fit {
  size_t size;
  struct page *p;
};
int first_page_fit(struct node *n, void *d) {
  if (n->type == SNAP_NODE_PAGE) {
    struct page *p = (struct page *)n;
    struct page_fit *ff = d;
    if (page_can_fit(p, ff->size)) {
      ff->p = p;
      return WALK_EXIT;
    }
  }

  return WALK_CONTINUE;
}

void *_snap_malloc(struct heap_header *heap, size_t size) {
  if (!size) {
    return NULL;
  }

  assert(heap->working != heap->committed);

  struct generation *g = heap->working;

  struct gen_match_info m = {.target = g->gen, .mismatch = NULL};
  walk_nodes((struct node *)g, all_gen_match, &m);
  assert(m.mismatch == NULL);

  struct page_fit fit = {.size = size, .p = NULL};
  walk_nodes((struct node *)g, first_page_fit, &fit);

  if (fit.p == NULL) {
    struct tree_slot slot = {.index = -1, .target = NULL};
    walk_nodes((struct node *)g, first_free_slot, &slot);

    if (slot.target == NULL) {
      new_gen_between(heap, g);
      slot = (struct tree_slot){.index = -1, .target = NULL};
      walk_nodes((struct node *)heap->working, first_free_slot, &slot);
    }

    assert(slot.target != NULL);
    assert(slot.index != -1);

    slot.target->c[slot.index] =
        (struct node *)new_page(heap, (size / PAGE_SIZE) + 1);

    fit = (struct page_fit){.size = size, .p = NULL};
    walk_nodes((struct node *)g, first_page_fit, &fit);
  }

  assert(fit.p != NULL);
  assert(fit.p->i.type == SNAP_NODE_PAGE);

  struct segment *s = page_new_segment(fit.p, size);

  return (char *)s + sizeof(struct segment);
}

void _snap_free(struct heap_header *heap, void *ptr) {
  if (!ptr) {
    return;
  }

  assert(heap->working != heap->committed);

  struct segment *s = (struct segment *)((char *)ptr - sizeof(struct segment));
  assert(s->used);

  struct page_from_hit phit = {.hit = ptr, .p = NULL, .index = -1};
  walk_nodes((struct node *)heap->root, find_page, (void *)&phit);
  assert(phit.p != NULL);
  assert(phit.index != -1);

  s->used = 0;
}

void *snap_malloc(struct heap_header *heap, size_t size) {
#ifdef SNAP_EVENT_LOG_PRECOMMITED
  fprintf(event_log, "snap_malloc %lu\n", (unsigned long)size);
  fflush(event_log);
#endif

  if (size == 0) {
    return NULL;
  }

  void *result = _snap_malloc(heap, size);

#ifdef SNAP_EVENT_LOG_FILE
  fprintf(event_log, "snap_malloc %lu -> %p\n", (unsigned long)size, result);
  fflush(event_log);
#endif

  return result;
}

void snap_free(struct heap_header *heap, void *ptr) {
#ifdef SNAP_EVENT_LOG_FILE
  fprintf(event_log, "snap_free %p\n", ptr);
  fflush(event_log);
#endif

  _snap_free(heap, ptr);
}

void *snap_realloc(struct heap_header *heap, void *ptr, size_t size) {
#ifdef SNAP_EVENT_LOG_PRECOMMITED
  fprintf(event_log, "snap_realloc %p %lu\n", ptr, (unsigned long)size);
  fflush(event_log);
#endif

  if (!ptr) {
    return _snap_malloc(heap, size);
  }

  assert(heap->working != heap->committed);

  struct page_from_hit phit = {.hit = ptr, .p = NULL, .index = -1};
  walk_nodes((struct node *)heap->root, find_page, (void *)&phit);
  assert(phit.p != NULL);
  assert(phit.index != -1);

  if (size == 0) {
    _snap_free(heap, ptr);
    return NULL;
  }

  void *result = _snap_malloc(heap, size);
  memmove(result, ptr, size);
  _snap_free(heap, ptr);

#ifdef SNAP_EVENT_LOG_FILE
  fprintf(
      event_log,
      "snap_realloc %p %lu -> %p\n",
      ptr,
      (unsigned long)size,
      result);
  fflush(event_log);
#endif

  return result;
}

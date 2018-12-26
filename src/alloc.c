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

#ifdef SNAP_EVENT_LOG_FILE
FILE *event_log;
#endif

void *open_db(const char *path, size_t size) {
#ifdef SNAP_EVENT_LOG_FILE
  int logfd = open(SNAP_EVENT_LOG_FILE, O_CREAT | O_RDWR, 0600);
  if (logfd == -1) {
    fprintf(stderr, "couldn't open event log %s\n", strerror(errno));
    return NULL;
  }

  event_log = fdopen(logfd, "a");
#endif

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
        // TODO(mason): revisit this
        // if ((void *)phit->hit >= (void *)((char *)s + sizeof(struct segment))
        // && (void *)phit->hit < (void *)((char *)s + sizeof(struct segment) +
        // s->size)) {

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

struct page *new_page(struct heap_header *heap, size_t pages) {
  struct page *f = heap->last_page =
      (struct page
           *)((char *)heap->last_page + (heap->last_page->pages * PAGE_SIZE));

  *f = (struct page){
      .i = {.type = SNAP_NODE_PAGE, .committed = 0},
      .pages = pages,
      .len = 0,
      .real_addr = f,
  };

  return f;
}

int set_readonly(struct node *n, void *d) {
  if (n->type != SNAP_NODE_PAGE || !n->committed) {
    return WALK_CONTINUE;
  }

  struct page *p = (struct page *)n;

  if (p->real_addr != p) {
    return WALK_CONTINUE;
  }

  fprintf(stderr, "setting RONLY on %p\n", (void *)n);

  int err = mprotect((void *)p, PAGE_SIZE * p->pages, PROT_READ);
  if (err != 0) {
    fprintf(stderr, "failed to unmark write pages %s\n", strerror(errno));
    fprintf(
        stderr,
        "%p - %p",
        (void *)p,
        (void *)((char *)p + (PAGE_SIZE * p->pages)));
    exit(3);
  }

  return WALK_CONTINUE;
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
  // page is moved
  for (int i = 0; i < p->len; i++) {
    new->c[i] =
        (struct segment *)((char *)new->c[i] + ((char *)new - (char *)p));
  }

  return new;
}

// TODO(turbio): just do some remapping
void page_swap(struct heap_header *heap, struct page *p1, struct page *p2) {
  fprintf(stderr, "swapping pages %p <-> %p\n", (void *)p1, (void *)p2);

  int err = mprotect((void *)p1, PAGE_SIZE, PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  err = mprotect((void *)p2, PAGE_SIZE, PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  void *tmp = (char *)heap->last_page + (heap->last_page->pages * PAGE_SIZE);

  memcpy(tmp, p1, p1->pages * PAGE_SIZE);
  for (int i = 0; i < p1->len; i++) {
    p1->c[i] = (struct segment *)((char *)p1->c[i] + ((char *)p2 - (char *)p1));
  }

  memcpy(p1, p2, p2->pages * PAGE_SIZE);
  for (int i = 0; i < p2->len; i++) {
    p2->c[i] = (struct segment *)((char *)p1->c[i] + ((char *)p1 - (char *)p2));
  }

  memcpy(p2, tmp, p1->pages * PAGE_SIZE);

  err = mprotect((void *)p1, PAGE_SIZE, PROT_READ);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  err = mprotect((void *)p2, PAGE_SIZE, PROT_READ);
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

  fprintf(stderr, "! hit %p\n", addr);

  assert(heap->working != heap->committed);

  struct page_from_hit phit = {.hit = addr, .p = NULL, .index = -1};
  walk_nodes((struct node *)heap->root, find_page, (void *)&phit);

  fprintf(stderr, "! turns out the is %p\n", (void *)phit.p);

  assert(phit.p != NULL);
  assert(phit.index != -1);

  struct page *hit_page = phit.p;

  assert(hit_page->i.type == SNAP_NODE_PAGE);
  assert(hit_page->i.committed);

  int err = mprotect((void *)hit_page, PAGE_SIZE, PROT_READ | PROT_WRITE);
  if (err != 0) {
    fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
    exit(3);
  }

  struct node_rel rel = {
      .parent = NULL,
      .index = -1,
      .child = (struct node *)hit_page,
  };
  walk_nodes((struct node *)heap->root, find_parent, &rel);
  assert(rel.parent != NULL);
  assert(rel.index != -1);
  assert(rel.parent->type == SNAP_NODE_GENERATION);

  struct page *new_page = page_copy(heap, hit_page);

  struct tree_slot slot = {.index = -1, .target = NULL};
  walk_nodes((struct node *)heap->working, first_free_slot, &slot);

  if (slot.target == NULL) {
    fprintf(stderr, "did one of those fancy moves\n");

    new_gen_between(heap, heap->working);

    slot = (struct tree_slot){.index = -1, .target = NULL};
    walk_nodes((struct node *)heap->working, first_free_slot, &slot);
  }

  // TODO(turbio): this needs to create children sometimes
  assert(slot.target != NULL);
  assert(slot.index != -1);

  new_page->i.committed = 1;
  ((struct generation *)rel.parent)->c[rel.index] = (struct node *)new_page;

  hit_page->i.committed = 0;
  slot.target->c[slot.index] = (struct node *)hit_page;

  fprintf(
      stderr,
      "! duplicated %p-%p to %p\n",
      (void *)hit_page,
      (char *)hit_page + PAGE_SIZE - 1,
      (void *)new_page);

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

  } else if (heap->v != 0xffca) {
    fprintf(
        stderr,
        "got a bad snapshot, %d (expected) != %d (actual)",
        0xffca,
        heap->v);
    exit(1);
  }

  walk_nodes((struct node *)heap->root, set_readonly, NULL);

  segv_handle_heap = heap;

  struct sigaction segv_action;

  segv_action.sa_flags = SA_SIGINFO | SA_NODEFER;
  segv_action.sa_sigaction = handle_segv;
  sigaction(SIGSEGV, &segv_action, NULL);

  return heap;
}

int snap_commit(struct heap_header *heap) {
#ifdef SNAP_EVENT_LOG_FILE
  fprintf(event_log, "snap_commit\n");
  fflush(event_log);
#endif

  fprintf(stderr, "BEGINNING COMMIT\n");
  int flagged = 0;
  walk_nodes((struct node *)heap->working, set_committed, &flagged);
  walk_nodes((struct node *)heap->working, set_readonly, NULL);

  fprintf(stderr, "committed %d nodes\n", flagged);

  fprintf(stderr, "SETTING READONLY\n");

  walk_nodes((struct node *)heap->root, verify_all_committed, NULL);
  fprintf(stderr, "UPDATING COMMIT BIT\n");

  heap->committed = heap->working;

  fprintf(stderr, "COMMIT COMPLETE\n");

  return heap->committed->gen;
}

struct up_to_gen_state {
  int max_gen;
  struct page *p[100];
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

  int i = 0;
  while (s->p[i]) {
    if (s->p[i]->real_addr == page->real_addr) {
      break;
    }

    i++;

    assert(i < 100);
  }

  s->p[i] = page;

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

void snap_checkout(struct heap_header *heap, int generation) {
  fprintf(stderr, "BEGINNING CHECKOUT\n");

  assert(heap->committed == heap->working);

  struct gen_from_id fid = {.id = generation, .g = NULL};
  walk_nodes((struct node *)heap->root, first_gen_for_id, &fid);
  assert(fid.g != NULL);
  assert(fid.g->gen == generation);

  if (heap->committed->gen == generation) {
    return;
  }

  struct up_to_gen_state s = {
      .max_gen = generation,
      .p = {NULL},
  };

  walk_nodes((struct node *)heap->root, pages_up_to_gen, &s);

  int i = 0;
  while (s.p[i]) {
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

    i++;
    assert(i < 100);
  }

  heap->committed = fid.g;
  heap->working = fid.g;

  fprintf(stderr, "COMPLETED CHECKOUT\n");
}

int snap_begin_mut(struct heap_header *heap) {
#ifdef SNAP_EVENT_LOG_FILE
  fprintf(event_log, "snap_begin_mut\n");
  fflush(event_log);
#endif

  fprintf(stderr, "BEGINNING MUT\n");

  assert(heap->working == heap->committed);
  walk_nodes((struct node *)heap->root, verify_all_committed, NULL);

  fprintf(
      stderr,
      "rev: %d, generation at: %p\n",
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

  fprintf(stderr, "working now: %d\n", heap->working->gen);

  assert(heap->working != heap->committed);
  assert(heap->committed->gen != heap->working->gen);

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

void *_snap_malloc(struct heap_header *heap, size_t bytes) {
  assert(heap->working != heap->committed);

  struct generation *g = heap->working;

  struct gen_match_info m = {.target = g->gen, .mismatch = NULL};
  walk_nodes((struct node *)g, all_gen_match, &m);
  assert(m.mismatch == NULL);

  struct page_fit fit = {.size = bytes, .p = NULL};
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
        (struct node *)new_page(heap, (bytes / PAGE_SIZE) + 1);

    fit = (struct page_fit){.size = bytes, .p = NULL};
    walk_nodes((struct node *)g, first_page_fit, &fit);
  }

  assert(fit.p != NULL);
  assert(fit.p->i.type == SNAP_NODE_PAGE);

  struct segment *s = page_new_segment(fit.p, bytes);

  return (char *)s + sizeof(struct segment);
}

void _snap_free(struct heap_header *heap, void *ptr) {
  assert(heap->working != heap->committed);

  struct segment *s = (struct segment *)((char *)ptr - sizeof(struct segment));
  assert(s->used);

  struct page_from_hit phit = {.hit = ptr, .p = NULL, .index = -1};
  walk_nodes((struct node *)heap->root, find_page, (void *)&phit);
  assert(phit.p != NULL);
  assert(phit.index != -1);

  s->used = 0;
}

void *snap_malloc(struct heap_header *heap, size_t bytes) {
  if (bytes == 0) {
    return NULL;
  }

  void *result = _snap_malloc(heap, bytes);

#ifdef SNAP_EVENT_LOG_FILE
  fprintf(event_log, "snap_malloc %ld -> %p\n", bytes, result);
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

void *snap_realloc(struct heap_header *heap, void *ptr, size_t bytes) {
  assert(heap->working != heap->committed);

  struct page_from_hit phit = {.hit = ptr, .p = NULL, .index = -1};
  walk_nodes((struct node *)heap->root, find_page, (void *)&phit);
  assert(phit.p != NULL);
  assert(phit.index != -1);

  if (bytes == 0) {
    _snap_free(heap, ptr);
    return NULL;
  }

  void *result = _snap_malloc(heap, bytes);
  memmove(result, ptr, bytes);
  _snap_free(heap, ptr);

#ifdef SNAP_EVENT_LOG_FILE
  fprintf(event_log, "snap_realloc %p %ld -> %p\n", ptr, bytes, result);
  fflush(event_log);
#endif

  return result;
}

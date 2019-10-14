// All sorts of fun stuff with mmap and signal handling so we need
// these bois.
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
#include "config.h"

// lol namespaces
#define page snap_page
#define segment snap_segment
#define generation snap_generation
#define node snap_node

#ifdef DEBUG_ALL
#define DEBUG_LOGGING
#define LOG_MAP_MODS
#define FULL_VERIFY
#endif

#ifdef DEBUG_LOGGING
#define SNAP_EVENT_LOG_PRECOMMITED
#define SNAP_EVENT_LOG_FILE "/dev/stderr"
#endif

#ifdef SNAP_EVENT_LOG_FILE
FILE *event_log;
#endif

void full_verify(int committed);

struct map {
  char w;
  void *start;
  size_t len;
};

struct table {
  size_t len;
  struct map m[MAX_MAPS];
};

struct runtime_state {
  char *db_path;
  struct heap_header *heap;
  void *handling_segv;

  struct table active_map;
};

static struct runtime_state rstate = {0};

#define H (rstate.heap)

#ifdef DEBUG_LOGGING
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOG(...)
#endif

uintptr_t round_page_down(uintptr_t addr) { return (addr & ~(PAGE_SIZE - 1)); }

uintptr_t round_page_up(uintptr_t addr) {
  if (addr & (PAGE_SIZE - 1)) {
    return round_page_down(addr + PAGE_SIZE);
  }

  return addr;
}

void print_map(struct map m) {
  LOG("\t%p-%p r", m.start, (void *)((char *)m.start + m.len));
  if (m.w) {
    LOG("w");
  }
  LOG("\n");
}

void print_table(struct table *pm) {
  for (size_t i = 0; i < pm->len; i++) {
    print_map(pm->m[i]);
  }
}

void insert_map(struct table *pm, struct map m, size_t index) {
  assert(index <= pm->len);

  if (pm->len == index) {
    pm->m[index] = m;
    pm->len++;
    return;
  }

  for (size_t i = pm->len - 1; i >= index; i--) {
    pm->m[i + 1] = pm->m[i];
  }

  pm->m[index] = m;
  pm->len++;
}

void remove_map(struct table *pm, size_t index) {
  assert(index < pm->len);

  for (size_t i = index; i < pm->len; i++) {
    pm->m[i] = pm->m[i + 1];
  }

  pm->len--;
  pm->m[pm->len] = (struct map){0};
}

void overlaps(struct map m, struct table *t, struct table *hits) {
  assert(hits->len == 0);

  for (size_t i = 0; i < t->len; i++) {
    if ((char *)m.start >= (char *)t->m[i].start + t->m[i].len) {
      continue;
    } else if ((char *)m.start + m.len <= (char *)t->m[i].start) {
      break;
    }

    hits->m[hits->len++] = t->m[i];
  }
}

size_t map_index(struct table *t, struct map m) {
  for (size_t i = 0; i < t->len; i++) {
    if (t->m[i].len == m.len && t->m[i].start == m.start) {
      return i;
    }
  }

  return (size_t)-1;
}

struct map *find_map(struct table *t, struct map m) {
  size_t i = map_index(t, m);
  if (i == (size_t)-1) {
    return NULL;
  }

  return &t->m[i];
}

struct map *find_map_before(struct table *t, struct map m) {
  size_t i = map_index(t, m);
  if (i == (size_t)-1 || i == 0) {
    return NULL;
  }

  return &t->m[i - 1];
}

struct map *find_map_after(struct table *t, struct map m) {
  size_t i = map_index(t, m);
  if (i == (size_t)-1 || i == t->len - 1) {
    return NULL;
  }

  return &t->m[i + 1];
}

void insert_after(struct table *t, struct map new, struct map after) {
  size_t i = map_index(t, after);
  assert(i != (size_t)-1);

  insert_map(t, new, i + 1);
}

int w_to_prot(char w) {
  if (w) {
    return PROT_READ | PROT_WRITE;
  }

  return PROT_READ;
}

// taken from linux mmap.c:
//
// The following mprotect cases have to be considered, where AAAA is
// the area passed down from mprotect_fixup, never extending beyond one
// vma, PPPPPP is the prev vma specified, and NNNNNN the next vma after:
//
//     AAAA             AAAA                AAAA          AAAA
//    PPPPPPNNNNNN    PPPPPPNNNNNN    PPPPPPNNNNNN    PPPPNNNNXXXX
//    cannot merge    might become    might become    might become
//                    PPNNNNNNNNNN    PPPPPPPPPPNN    PPPPPPPPPPPP 6 or
//    mmap, brk or    case 4 below    case 5 below    PPPPPPPPXXXX 7 or
//    mremap move:                                    PPPPXXXXXXXX 8
//        AAAA
//    PPPP    NNNN    PPPPPPPPPPPP    PPPPPPPPNNNN    PPPPNNNNNNNN
//    might become    case 1 below    case 2 below    case 3 below
//
void merge_in_map(struct map newmap) {
  full_verify(-1);

#ifdef LOG_MAP_MODS
  LOG("+");
  print_map(newmap);
#endif

  struct table hits = {0};
  overlaps(newmap, &rstate.active_map, &hits);
  assert(hits.len != 0);

#ifdef LOG_MAP_MODS
  LOG("\tfound %lu overlaps:\n", (unsigned long)hits.len);
  print_table(&hits);
  LOG("\t");
#endif

  while (hits.len > 2 || (hits.len == 2 && hits.m[0].w == hits.m[1].w)) {
    struct map sec = hits.m[1];

    struct map *abefore = find_map_before(&rstate.active_map, sec);
    assert(abefore);

    struct map *hbefore = find_map_before(&hits, sec);
    assert(hbefore);

    remove_map(&hits, map_index(&hits, sec));
    remove_map(&rstate.active_map, map_index(&rstate.active_map, sec));

    abefore->len += sec.len;
    hbefore->len += sec.len;
  }

  if (hits.len == 1) {
    struct map hit = hits.m[0];

    if (hit.w == newmap.w) {
#ifdef LOG_MAP_MODS
      LOG("skip because equal\n");
#endif
    } else if (hit.start == newmap.start && hit.len == newmap.len) {
#ifdef LOG_MAP_MODS
      LOG("exact match\n");
#endif

      struct map *mbefore = find_map_before(&rstate.active_map, hit);
      struct map *mafter = find_map_after(&rstate.active_map, hit);
      assert(mbefore || mafter);

      if (mbefore && mafter) {
        // exact hit on surrounded node
        //   AAAAAAAA
        // LLRRRRRRRRLL
        // becomes
        // LLLLLLLLLLLL

        const struct map mafterv = *mafter;

#ifdef LOG_MAP_MODS
        LOG("surrounded, join left and right\n");
#endif

        assert(mbefore->w == mafter->w && "should be alternating");

        remove_map(&rstate.active_map, map_index(&rstate.active_map, hit));
        remove_map(&rstate.active_map, map_index(&rstate.active_map, mafterv));

        mbefore->len += mafterv.len + hit.len;

        int err = mprotect(newmap.start, newmap.len, w_to_prot(newmap.w));
        if (err != 0) {
          fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
          exit(3);
        }
      } else if (mbefore) {
        assert(0 && "oh no");
      } else if (mafter) {
        assert(0 && "oh no");
      } else {
        assert(0);
      }

    } else if (hit.start == newmap.start) {
#ifdef LOG_MAP_MODS
      LOG("share start addr\n");
#endif

      // maps share a start address
      //       AAAA
      // LLLLLLRRRRRR
      // becomes
      // LLLLLLLLLLRR
      //
      // shrink the map being hit and extend the map before

      struct map *mbefore = find_map_before(&rstate.active_map, hit);
      assert(mbefore && "unimplemented: hitting left most map");

      struct map *hitptr = find_map(&rstate.active_map, hit);
      assert(hitptr);

      hitptr->len -= newmap.len;
      mbefore->len += newmap.len;

      hitptr->start = (char *)newmap.start + newmap.len;

      int err = mprotect(newmap.start, newmap.len, w_to_prot(newmap.w));
      if (err != 0) {
        fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
        exit(3);
      }
    } else if (
        (char *)hit.start + hit.len == (char *)newmap.start + newmap.len) {
#ifdef LOG_MAP_MODS
      LOG("share end addr\n");
#endif

      // maps share an end address
      //   AAAA
      // LLLLLLRRRRRR
      // becomes
      // LLRRRRRRRRRR

      struct map *mafter = find_map_after(&rstate.active_map, hit);

      if (!mafter) {
        struct map *hitptr = find_map(&rstate.active_map, hit);
        assert(hitptr);

        insert_after(&rstate.active_map, newmap, hit);

        hitptr->len -= newmap.len;

        int err = mprotect(newmap.start, newmap.len, w_to_prot(newmap.w));
        if (err != 0) {
          fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
          exit(3);
        }
      } else {
        struct map *hitptr = find_map(&rstate.active_map, hit);
        assert(hitptr);

        mafter->len += newmap.len;
        mafter->start = (char *)mafter->start - newmap.len;

        hitptr->len -= newmap.len;

        int err = mprotect(newmap.start, newmap.len, w_to_prot(newmap.w));
        if (err != 0) {
          fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
          exit(3);
        }
      }
    } else {
#ifdef LOG_MAP_MODS
      LOG("inside\n");
#endif

      struct map *hitptr = find_map(&rstate.active_map, hit);
      assert(hitptr);

      hitptr->len = (char *)newmap.start - (char *)hitptr->start;

      insert_after(&rstate.active_map, newmap, *hitptr);

      struct map m2 = {
          .start = (char *)newmap.start + newmap.len,
          .len = hit.len - hitptr->len - newmap.len,
          .w = hitptr->w,
      };
      insert_after(&rstate.active_map, m2, newmap);

      int err = mprotect(newmap.start, newmap.len, w_to_prot(newmap.w));
      if (err != 0) {
        fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
        exit(3);
      }
    }
  } else if (hits.len == 2) {
    struct map hl = hits.m[0];
    struct map hr = hits.m[1];

    struct map *lptr = find_map(&rstate.active_map, hl);
    assert(lptr);

    struct map *rptr = find_map(&rstate.active_map, hr);
    assert(rptr);

    if (newmap.w == hl.w) {
      LOG("expanding left\n");
      size_t ldiff =
          ((char *)newmap.start + newmap.len) - ((char *)hl.start + hl.len);

      lptr->len += ldiff;

      rptr->start = (char *)rptr->start + ldiff;
      rptr->len -= ldiff;

      if (rptr->len == 0) {
        // the map probably aligned with the right side, we'll end up killing
        // two maps

        size_t emptyi = map_index(&rstate.active_map, *rptr);
        remove_map(&rstate.active_map, emptyi);

        lptr->len += rstate.active_map.m[emptyi].len;

        remove_map(&rstate.active_map, emptyi);
      }
    } else if (newmap.w == hr.w) {
      LOG("expanding right\n");

      size_t rdiff = (char *)hr.start - (char *)newmap.start;

      lptr->len -= rdiff;

      assert(lptr->len != 0);

      rptr->start = (char *)rptr->start - rdiff;
      rptr->len += rdiff;
    }

    int err = mprotect(newmap.start, newmap.len, w_to_prot(newmap.w));
    if (err != 0) {
      fprintf(stderr, "failed to mark write pages %s\n", strerror(errno));
      exit(3);
    }
  } else {
    assert(0 && "hits should have been culled to 1 or 2");
  }

#ifdef LOG_MAP_MODS
  LOG("resulting map:\n");
  print_table(&rstate.active_map);
  LOG("\n");
#endif

  full_verify(-1);
}

void merge_in_table(struct table newmaps) {
  // can't merge in no maps
  assert(newmaps.len > 0);

  // lowest map can't start before the existing lowest map
  assert(newmaps.m[0].start >= rstate.active_map.m[0].start);

  // highest map can't go beyond the existing highest map
  assert(
      (char *)newmaps.m[newmaps.len - 1].start +
          newmaps.m[newmaps.len - 1].len <=

      (char *)rstate.active_map.m[rstate.active_map.len - 1].start +
          rstate.active_map.m[rstate.active_map.len - 1].len);

  struct table merged = {
      .len = 1,
      .m =
          {
              newmaps.m[0],
          },
  };

  for (size_t i = 1; i < newmaps.len; i++) {
    // merge em if they're contiguous and have the same protection
    if (merged.m[merged.len - 1].w == newmaps.m[i].w &&
        (char *)merged.m[merged.len - 1].start + merged.m[merged.len - 1].len ==
            newmaps.m[i].start) {
      merged.m[merged.len - 1].len += newmaps.m[i].len;
    } else {
      merged.m[merged.len] = newmaps.m[i];
      merged.len++;
    }
  }

#ifdef LOG_MAP_MODS
  LOG(">>>>>>\n");
#endif

  for (size_t i = 0; i < merged.len; i++) {
    struct map newmap = merged.m[i];
    merge_in_map(newmap);
  }

#ifdef LOG_MAP_MODS
  LOG("<<<<<<\n");
#endif
}

enum address_in {
  ADDR_IN_INVALID = -1,
  ADDR_IN_USER_DATA = 1,
  ADDR_IN_META_DATA = 2,
};

int object_in_range(uintptr_t addr, size_t size) {
  if (addr < (uintptr_t)H->map_start) {
    return ADDR_IN_INVALID;
  }

  if (addr >= (uintptr_t)H->map_start + H->size) {
    return ADDR_IN_INVALID;
  }

  if (addr + size <
      (uintptr_t)H->map_start + (PAGE_SIZE * (INITIAL_PAGES / 2))) {
    return ADDR_IN_META_DATA;
  }

  return ADDR_IN_USER_DATA;
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
    assert(
        object_in_range((uintptr_t)g, sizeof(struct generation)) ==
        ADDR_IN_META_DATA);

    for (int i = 0; i < GENERATION_CHILDREN; i++) {
      if (!g->c[i]) {
        continue;
      }

      if (walk_nodes(g->c[i], cb, d) == WALK_EXIT) {
        return WALK_EXIT;
      }
    }
  } else if (n->type == SNAP_NODE_PAGE) {
    assert(
        object_in_range((uintptr_t)n, sizeof(struct node)) ==
        ADDR_IN_USER_DATA);
  } else {
    assert(0 && "invalid node type while talking, probably walked off the end");
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

  LOG("mapping initial heap from %p-%p\n",
      MAP_START_ADDR,
      (void *)((char *)MAP_START_ADDR + PAGE_SIZE));

  // we'll initially map the first page to grab db header info
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
    assert(0 && "cannot handle alternate map_start");
  }

  state->heap = mem;

  LOG("mapping in entire heap %p-%p\n",
      MAP_START_ADDR,
      (void *)((char *)MAP_START_ADDR + heap->size));

  void *new_addr = mremap(MAP_START_ADDR, PAGE_SIZE, heap->size, 0);
  if (new_addr == (void *)-1) {
    fprintf(stderr, "db remap failed at open: %s\n", strerror(errno));
    return -1;
  }
  assert(new_addr == heap->map_start);
  assert(new_addr == MAP_START_ADDR);

  state->active_map = (struct table){
      .len = 1,
      .m =
          {
              {
                  .start = MAP_START_ADDR,
                  .len = heap->size,
                  .w = 1,
              },
          },
  };

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
      .i = {.type = SNAP_NODE_PAGE, .committed = 1},
      .pages = 1,
      .len = 0,
      .real_addr = initial_page,
  };

  struct generation *initial_gen = heap->last_gen =
      (struct generation *)((char *)heap + sizeof(struct heap_header));

  *initial_gen = (struct generation){
      .i = {.type = SNAP_NODE_GENERATION, .committed = 1},
      .gen = heap->last_gen_index,
      .c = {(struct node *)initial_page},
  };

  heap->committed = NULL;
  heap->working = initial_gen;
  heap->root = initial_gen;

  munmap(heap->map_start, initial_size);

  return 0;
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

int pages_in_gen(struct node *n, void *d) {
  if (n->type != SNAP_NODE_PAGE || !n->committed) {
    return WALK_CONTINUE;
  }

  struct page *p = (struct page *)n;

  if (p->real_addr != p) {
    return WALK_CONTINUE;
  }

  struct table *mm = (struct table *)d;

  mm->m[mm->len] = (struct map){
      .w = 0,
      .start = p,
      .len = p->pages * PAGE_SIZE,
  };
  mm->len++;

  assert(mm->len < MAX_MAPS);

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

  LOG("resizing map from %lx to %lx (+%lx) to accommodate %lx\n",
      (unsigned long)heap->size,
      (unsigned long)new_size,
      (unsigned long)expand_by,
      (unsigned long)min_expand);

  struct map lastmap = rstate.active_map.m[rstate.active_map.len - 1];

  LOG("resizing map:");
  print_map(lastmap);

  void *new_addr =
      mremap(lastmap.start, lastmap.len, lastmap.len + expand_by, 0);
  if (new_addr == (void *)-1) {
    fprintf(stderr, "db remap failed at expand: %s\n", strerror(errno));
    exit(3);
  }

  rstate.active_map.m[rstate.active_map.len - 1].len += expand_by;

  LOG("layout is now:\n");
  print_table(&rstate.active_map);

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

  assert(object_in_range((uintptr_t)f, pages * PAGE_SIZE) == ADDR_IN_USER_DATA);

  return f;
}

int set_committed(struct node *n, void *d) {
  int *flagged = (int *)d;
  (*flagged)++;
  n->committed = 1;

  return WALK_CONTINUE;
}

struct page *page_copy(struct heap_header *h, struct page *p) {
  assert(
      object_in_range((uintptr_t)p, p->pages * PAGE_SIZE) == ADDR_IN_USER_DATA);

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
  full_verify(1);

  assert(
      object_in_range((uintptr_t)p1, p1->pages * PAGE_SIZE) ==
      ADDR_IN_USER_DATA);
  assert(
      object_in_range((uintptr_t)p2, p2->pages * PAGE_SIZE) ==
      ADDR_IN_USER_DATA);

  assert(p1->pages > 0);
  assert(p1->pages == p2->pages);
  assert(p1->real_addr == p2->real_addr);
  assert(p1->real_addr == p1 || p2->real_addr == p2);

  int pages = p1->pages;
  size_t chlen = pages * PAGE_SIZE;

  LOG("swapping pages %p <-> %p\n", (void *)p1, (void *)p2);

  struct table rw = {
      .len = 2,
      .m =
          {
              {
                  .w = 1,
                  .start = p1,
                  .len = chlen,
              },
              {
                  .w = 1,
                  .start = p2,
                  .len = chlen,
              },
          },
  };
  merge_in_table(rw);

  char tmp[chlen];

  memcpy(tmp, p1, chlen);
  memcpy(p1, p2, chlen);
  memcpy(p2, tmp, chlen);

  size_t dist = (char *)p1 - (char *)p2;

  for (int i = 0; i < p1->len; i++) {
    p1->c[i] = (struct segment *)((char *)p1->c[i] + dist);
  }

  for (int i = 0; i < p2->len; i++) {
    p2->c[i] = (struct segment *)((char *)p2->c[i] - dist);
  }

  full_verify(1);
}

struct generation *new_gen(struct heap_header *h, int index) {
  struct generation *new = ++h->last_gen;

  *new = (struct generation){
      .i = {.type = SNAP_NODE_GENERATION, .committed = 0},
      .gen = index,
      .c = {0},
  };

  assert(
      object_in_range((uintptr_t) new, sizeof(struct generation)) ==
      ADDR_IN_META_DATA);

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

  if (addr < H->map_start || (char *)addr >= (char *)H->map_start + H->size) {
    LOG("legal zone: %p-%p\n",
        H->map_start,
        (void *)((char *)H->map_start + H->size));

    LOG("in transaction: %s\n", H->committed == H->working ? "false" : "true");

    fprintf(stderr, "SEGFAULT trying to read %p\n", addr);
    exit(2);
  }

  assert(object_in_range((uintptr_t)addr, 1) == ADDR_IN_USER_DATA);

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

  struct table pm = {
      .len = 1,
      .m =
          {
              {
                  .w = 1,
                  .start = hit_page,
                  .len = hit_page->pages * PAGE_SIZE,
              },
          },
  };
  merge_in_table(pm);

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
      (void *)((char *)hit_page + PAGE_SIZE - 1),
      (void *)fresh_page);

  rstate.handling_segv = NULL;
}

struct heap_header *snap_init(char *db_path) {
#ifdef SNAP_EVENT_LOG_FILE
  int logfd = open(SNAP_EVENT_LOG_FILE, O_CREAT | O_RDWR, 0600);
  if (logfd == -1) {
    fprintf(stderr, "couldn't open event log %s\n", strerror(errno));
    return NULL;
  }

  event_log = fdopen(logfd, "a");
#endif

  rstate.db_path = db_path;

  struct stat stat_info;

  int created = 0;

  int err = stat(db_path, &stat_info);
  if (err && errno == ENOENT) {
    if (new_db(db_path)) {
      fprintf(stderr, "couldn't creat initial db\n");
      return NULL;
    }

    created = 1;
  }

  int notok = open_db(db_path, &rstate);
  if (notok) {
    return NULL;
  }

  if (created) {
    full_verify(0);
  } else {
    // if we're loading an existing heap lets handle some invariant and try to
    // correct for them.

    // TODO(turbio): cleanup some of the other state!
    if (H->committed != H->working) {
      fprintf(stderr, "WARNING: heap is uncommitted, rolling back...\n");
      H->working = H->committed;
    }

    full_verify(1);
  }

  static struct sigaction segv_action;

  segv_action.sa_flags = SA_SIGINFO | SA_NODEFER;
  segv_action.sa_sigaction = handle_segv;
  sigaction(SIGSEGV, &segv_action, NULL);

  LOG("snap allocator initialized\n");

  return rstate.heap;
}

int snap_commit(struct heap_header *heap) {
#ifdef SNAP_EVENT_LOG_FILE
  fprintf(event_log, "snap_commit\n");
  fflush(event_log);
#endif

  full_verify(0);

  LOG("BEGINNING COMMIT\n");

  LOG("update commit bit\n");

  int flagged = 0;
  walk_nodes((struct node *)heap->working, set_committed, &flagged);

  LOG("committed %d nodes\n", flagged);

  heap->committed = heap->working;

  full_verify(1);

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

  full_verify(1);

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

  full_verify(1);

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

  full_verify(1);

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

  struct table mm = {0};

  walk_nodes((struct node *)heap->root, pages_in_gen, &mm);
  merge_in_table(mm);

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

  full_verify(0);

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

  full_verify(0);

  return (char *)s + sizeof(struct segment);
}

void _snap_free(struct heap_header *heap, void *ptr) {
  if (!ptr) {
    return;
  }

  full_verify(0);

  assert(heap->working != heap->committed);

  struct segment *s = (struct segment *)((char *)ptr - sizeof(struct segment));
  assert(s->used);

  struct page_from_hit phit = {.hit = ptr, .p = NULL, .index = -1};
  walk_nodes((struct node *)heap->root, find_page, (void *)&phit);
  assert(phit.p != NULL);
  assert(phit.index != -1);

  s->used = 0;

  full_verify(0);
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

// cppcheck-suppress unusedFunction
int segments_inside_pages(struct node *n, void *d) {
  if (n->type != SNAP_NODE_PAGE) {
    return WALK_CONTINUE;
  }

  struct page *p = (struct page *)n;

  for (int i = 0; i < p->len; i++) {
    if (!((char *)p->c[i] > (char *)p) ||
        !((char *)p->c[i] < (char *)p + (p->pages * PAGE_SIZE))) {
      LOG("page verification failed!\n");
      LOG("segment %d is bad, %p should be inside %p-%p\n",
          i,
          (void *)p->c[i],
          (void *)p,
          (void *)((char *)p + (p->pages * PAGE_SIZE)));
      exit(1);
    }
  }

  return WALK_CONTINUE;
}

// cppcheck-suppress unusedFunction
int verify_all_committed(struct node *n, void *d) {
  if (!n->committed) {
    if (n->type == SNAP_NODE_GENERATION) {
      fprintf(stderr, "gen %p was no committed\n", (void *)n);
    } else if (n->type == SNAP_NODE_PAGE) {
      fprintf(stderr, "page %p was no committed\n", (void *)n);
    }
    assert(0);
  }

  return WALK_CONTINUE;
}

#ifdef FULL_VERIFY
void full_verify(int committed) {
  if (committed == 1) {
    assert(H->working == H->committed && "expected to be committed");
    walk_nodes((struct node *)H->root, verify_all_committed, NULL);
  } else if (committed == 0) {
    assert(H->working != H->committed && "expected to be uncommitted");
  }

  walk_nodes((struct node *)H->root, segments_inside_pages, NULL);

  for (size_t i = 1; i < rstate.active_map.len; i++) {
    assert(
        rstate.active_map.m[i].len > 0 &&
        "shouldn't have any zero length maps");
  }

  for (size_t i = 1; i < rstate.active_map.len; i++) {
    assert(
        (rstate.active_map.m[0].start && (PAGE_SIZE - 1)) != 0 &&
        "start address should be page aligned");
    assert(
        (rstate.active_map.m[0].start && (PAGE_SIZE - 1)) != 0 &&
        "end address should be page aligned");
  }

  for (size_t i = 1; i < rstate.active_map.len; i++) {
    assert(
        rstate.active_map.m[i].w != rstate.active_map.m[i - 1].w &&
        "table entries must be alternating");
  }

  for (size_t i = 1; i < rstate.active_map.len; i++) {
    assert(
        (char *)rstate.active_map.m[i].start ==
            ((char *)rstate.active_map.m[i - 1].start +
             rstate.active_map.m[i - 1].len) &&
        "table entries must be contiguous");
  }

  FILE *f = fopen("/proc/self/maps", "r");

  uintptr_t start;
  uintptr_t end;
  char r, w, x, s;

  int found_start = 0;
  int err = 0;

  for (size_t i = 0; i < rstate.active_map.len; i++) {
    assert(
        fscanf(f, "%lx-%lx %c%c%c%c %*[^\n]", &start, &end, &r, &w, &x, &s) ==
        6);

    while (!found_start) {
      if ((void *)start == H->map_start) {
        found_start = 1;
        break;
      }
      assert(
          fscanf(f, "%lx-%lx %c%c%c%c %*[^\n]", &start, &end, &r, &w, &x, &s) ==
          6);
    }

    if (rstate.active_map.m[i].start != (void *)start ||
        (char *)rstate.active_map.m[i].start + rstate.active_map.m[i].len !=
            (char *)end ||
        rstate.active_map.m[i].w != (w == 'w')) {
      err = 1;
      break;
    }
  }

  if (err) {
    LOG("linux page table mismatch!\n");
    LOG("expected:\n");
    print_table(&rstate.active_map);
    LOG("but linux says we're actually:\n");
    rewind(f);

    int ch;
    while ((ch = fgetc(f)) != EOF) {
      putchar(ch);
    }

    assert(0);
  }

  fclose(f);
}
#else
void full_verify(int committed) {}
#endif

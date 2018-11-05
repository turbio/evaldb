#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lua.h>

#include <jansson.h>

#include <assert.h>

#include <sys/personality.h>

#define ALLOC_BLOCK_SIZE (1000 * 1000)
#define NODE_CHILDREN 5

int log_alloc;

void *open_db(const char *path, int size) {
  int fd = open(path, O_CREAT | O_RDWR, 0660);
  if (fd == -1) {
    printf("couldn't open db %s\n", strerror(errno));
    return NULL;
  }

  int err = ftruncate(fd, size);
  if (err == -1) {
    printf("could not increase db size  %s\n", strerror(errno));
    return NULL;
  }

  void *mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED) {
    printf("db map failed  %s\n", strerror(errno));
    return NULL;
  }

  return mem;
}

struct heap_header {
  uint16_t v;

  uint64_t size; // size includes self
  uint64_t used;

  struct heap_frame *first;

  uintptr_t lua_state;
};

enum frame_type {
  EMPTY = 0,
  NODE = (1 << 0),
  LEAF = (1 << 1),
};

struct heap_frame {
  size_t size; // size does not include self

  // each type corresponds the the child at the same index
  enum frame_type ctype[NODE_CHILDREN];
  void *c[NODE_CHILDREN];
};

void memTreeTraverse(struct heap_frame *frame, int depth) {
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

    if (frame->ctype[i] == LEAF) {
      printf("%s┌", pad);
      for (int i = 0; i < 27; i++) {
        printf("─");
      }
      printf("\n%s│", pad);
      printf(" EMPTY\n");

    } else if (frame->ctype[i] == NODE) {
      struct heap_frame *c = frame->c[i];
      memTreeTraverse(c, depth + 1);
    }
  }
}

void print_mem_tree(struct heap_header *heap) {
  memTreeTraverse(heap->first, 0);
}

int next_free_index(struct heap_frame *f) {
  for (int i = 0; i < NODE_CHILDREN; i++) {
    if (f->ctype[i] == EMPTY) {
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

  int last_child = next_child - 1;

  struct heap_frame *rframe = f->c[last_child];

  if (f->ctype[last_child] == LEAF) {
    *pindex = next_child;
    return (void *)rframe + rframe->size + sizeof(struct heap_frame);
  }

  return next_free_addr(rframe, pindex);
}

struct heap_frame *create_next_child(struct heap_frame *f, enum frame_type t) {
  int pindex;
  struct heap_frame *newf = (struct heap_frame *)next_free_addr(f, &pindex);
  f->c[pindex] = newf;
  f->ctype[pindex] = t;
  return newf;
}

struct heap_frame *
find_parent(struct heap_frame *r, struct heap_frame *f, int *cindex) {
  int i = 0;

  for (;;) {
    if (r->c[i] == f) {
      *cindex = i;
      return r;
    }

    if ((void *)f < r->c[i] || i == NODE_CHILDREN - 1) {
      r = r->c[i];
      i = 0;
      continue;
    }

    i++;
  }
}

void *snap_malloc(struct heap_header *heap, size_t n) {
  struct heap_frame *parent = heap->first;

  int free_index = next_free_index(parent);

  while (free_index == -1) {
    parent = parent->c[NODE_CHILDREN - 1];
    free_index = next_free_index(parent);
  }

  if (free_index == NODE_CHILDREN - 1) {
    struct heap_frame *link = create_next_child(parent, NODE);
    heap->used += sizeof(struct heap_frame);
    parent = link;
  }

  struct heap_frame *target = create_next_child(parent, LEAF);
  heap->used += sizeof(struct heap_frame);

  target->size = n;

  heap->used += n;

  return (void *)target + sizeof(struct heap_frame);
}

void *snap_realloc(struct heap_header *heap, void *ptr, size_t n) {
  struct heap_frame *frame =
      (struct heap_frame *)(ptr - sizeof(struct heap_frame));

  heap->used -= frame->size;
  heap->used += n;

  if (n < frame->size) {
    memset(ptr + n, '-', frame->size - n);
    return ptr;
  }

  void *result = snap_malloc(heap, n);
  memmove(result, ptr, n);
  return result;
}

void snap_free(struct heap_header *heap, void *ptr) {
  struct heap_frame *frame =
      (struct heap_frame *)(ptr - sizeof(struct heap_frame));

  heap->used -= frame->size + sizeof(struct heap_frame);

  frame->type = NODE;
  for (int i = 0; i < NODE_CHILDREN; i++) {
    frame->child[i] = NULL;
  }

  /*memset(frame, NULL, frame->size + sizeof(struct heap_frame));*/
}

void *lua_allocr(void *ud, void *ptr, size_t osize, size_t nsize) {
  struct heap_header *heap = (struct heap_header *)ud;

  if (nsize == 0) {
    if (ptr == NULL) {
      return NULL;
    }

    if (log_alloc) {
      printf("   FREE %p %ld -> %ld\n", ptr, osize, nsize);
    }

    snap_free(heap, ptr);
    return NULL;
  }

  if (ptr) {
    if (log_alloc) {
      printf("REALLOC %p %ld -> %ld\n", ptr, osize, nsize);
    }

    return snap_realloc(heap, ptr, nsize);
  }

  if (log_alloc) {
    printf("  ALLOC %ld\n", nsize);
  }

  void *addr = snap_malloc(heap, nsize);

  return addr;
}

void asJSON(lua_State *L, int index, char *str, size_t *len) {
  json_t *v = NULL;

  switch (lua_type(L, index)) {
  case LUA_TNUMBER:
    if (lua_isinteger(L, index)) {
      lua_Integer n = lua_tointeger(L, index);
      v = json_integer(n);
    } else {
      lua_Number n = lua_tonumber(L, index);
      v = json_real(n);
    }
    break;
  case LUA_TSTRING:
    v = json_string(lua_tostring(L, index));
    break;
  case LUA_TNIL:
    v = json_null();
    break;
  case LUA_TTABLE:
    strncpy(str, "\"TODO\"", strlen("\"TODO\"") + 1);
    break;
  }

  if (v == NULL) {
    strncpy(str, "\"bad type\"", strlen("\"bad type\"") + 1);
    return;
  }

  size_t s = json_dumpb(v, str, 1024, JSON_ENCODE_ANY);
  str[s] = 0;
  *len = s;
}

int did_read = 0;

const char *lreader(lua_State *L, void *data, size_t *size) {
  if (did_read) {
    *size = 0;
    return NULL;
  }

  did_read = 1;

  *size = strlen(data);
  return data;
}

void run_for(
    struct heap_header *heap, lua_State *L, const char *code, char *result) {
  assert(lua_gettop(L) == 0);

  int error;

  printf("evaling: \"%s\"\n", code);

  did_read = 0;
  error = lua_load(L, lreader, (void *)code, "eval", "t");
  if (error) {
    sprintf(result, "load err: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    goto abort;
  }

  error = lua_pcall(L, 0, 1, 0);
  if (error) {
    sprintf(result, "call err: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    goto abort;
  }

  size_t l = 0;

  asJSON(L, 1, result, &l);
  lua_pop(L, 1);
  assert(lua_gettop(L) == 0);

abort:
  lua_gc(L, LUA_GCCOLLECT, 0);
}

int main(int argc, char *argv[]) {
  int pers = personality(0xffffffff);
  if (pers == -1) {
    printf("could not get personality %s", strerror(errno));
    return 1;
  }

  if (!(pers & ADDR_NO_RANDOMIZE)) {
    if (personality(ADDR_NO_RANDOMIZE) == -1) {
      printf("could not set personality %s", strerror(errno));
      return 1;
    }

    execve("/proc/self/exe", argv, NULL);
  }

  printf("sizeof(struct heap_header): %ld\n", sizeof(struct heap_header));
  printf("sizeof(struct heap_frame): %ld\n", sizeof(struct heap_frame));

  void *mem = open_db("./_db", ALLOC_BLOCK_SIZE);

  struct heap_header *heap = (struct heap_header *)mem;

  lua_State *L;

  log_alloc = 1;

  if (heap->v != 0xffca) {
    heap->v = 0xffca;
    heap->size = ALLOC_BLOCK_SIZE;
    heap->used = sizeof(struct heap_header);
    heap->first = mem + sizeof(struct heap_header);

    heap->first->size = heap->size - heap->used - sizeof(struct heap_frame);
    heap->first->type = NODE;

    printf("CREATING LUA STATE\n");
    L = lua_newstate(lua_allocr, heap);
    heap->lua_state = (uintptr_t)L;
    printf("CREATED LUA STATE at %p\n", L);
  } else {
    printf("LOADING LUA STATE\n");
    L = (lua_State *)(heap->lua_state);
    printf("LOADED LUA STATE at %p\n", L);
  }

  lua_setallocf(L, lua_allocr, heap);

  char buff[1024];

  int usedBefore = heap->used;

  if (argc == 2) {
    run_for(heap, L, argv[1], buff);
    printf("=> %s\n", buff);
  }

  print_mem_tree(heap);

  printf("%ld/%ld (%ld)\n", heap->used, heap->size, heap->used - usedBefore);

  return 0;

  run_for(heap, L, "return 1+1", buff);
  assert(strcmp(buff, "2") == 0);

  run_for(heap, L, "return 1+1.0", buff);
  assert(strcmp(buff, "2.0") == 0);

  run_for(heap, L, "return 'hello\\nworld!'", buff);
  assert(strcmp(buff, "\"hello\\nworld!\"") == 0);

  run_for(heap, L, "v = 1", buff);
  run_for(heap, L, "return v", buff);
  assert(strcmp(buff, "1") == 0);
  run_for(heap, L, "v = v + 1", buff);
  run_for(heap, L, "v = v + 1", buff);
  run_for(heap, L, "return v", buff);
  assert(strcmp(buff, "3") == 0);

  return 0;
}

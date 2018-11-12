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

#include "alloc.h"

#define ALLOC_BLOCK_SIZE (1000 * 1000)

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
  printf("sizeof(struct heap_leaf): %ld\n", sizeof(struct heap_leaf));

  void *mem = open_db("./_db", ALLOC_BLOCK_SIZE);

  struct heap_header *heap = (struct heap_header *)mem;

  lua_State *L;

  log_alloc = 1;

  if (heap->v != 0xffca) {
    heap->v = 0xffca;
    heap->size = ALLOC_BLOCK_SIZE;

    heap->root = mem + sizeof(struct heap_header);
    heap->root->size =
        heap->size - sizeof(struct heap_header) - sizeof(struct heap_frame);

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

  if (argc == 2) {
    run_for(heap, L, argv[1], buff);
    printf("=> %s\n", buff);
  }

  // print_mem_tree(heap);

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

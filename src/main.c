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

#define BLOCK_SIZE 4024
#define MIN_BLOCKS 50

int log_alloc;

void *open_db(const char *path) {
  int fd = open(path, O_CREAT | O_RDWR, 0660);
  if (fd == -1) {
    printf("couldn't open db %s\n", strerror(errno));
    return NULL;
  }

  int err = ftruncate(fd, BLOCK_SIZE * MIN_BLOCKS);
  if (err == -1) {
    printf("could not increase db size  %s\n", strerror(errno));
    return NULL;
  }

  void *mem = mmap(0x7f646374e000, BLOCK_SIZE * MIN_BLOCKS,
                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED) {
    printf("db map failed  %s\n", strerror(errno));
    return NULL;
  }

  return mem;
}

struct heap_meta {
  uint8_t v;
  uintptr_t beginning;
  uintptr_t last_malloc;
  uintptr_t head;
};

void *snap_malloc(struct heap_meta *heap, size_t n) {
  heap->last_malloc = heap->head;
  heap->head += n;
  return (void *)heap->last_malloc;
}

void *snap_realloc(struct heap_meta *heap, void *ptr, size_t n) {
  if (ptr == heap->last_malloc) {
    heap->head = heap->last_malloc + n;
    return ptr;
  }

  void *result = snap_malloc(heap, n);
  memmove(result, ptr, n);
  return result;
}

void *lua_allocr(void *ud, void *ptr, size_t osize, size_t nsize) {
  struct heap_meta *heap = (struct heap_meta *)ud;

  if (nsize == 0) {
    if (log_alloc) {
      printf("   FREE %p %ld -> %ld\n", ptr, osize, nsize);
    }
    /*free(ptr);*/
    return NULL;
  }

  if (ptr) {
    if (log_alloc) {
      printf("REALLOC %p %ld -> %ld\n", ptr, osize, nsize);
    }
    return snap_realloc(heap, ptr, nsize);
  }

  /*void *addr = malloc(nsize);*/
  void *addr = snap_malloc(heap, nsize);

  if (log_alloc) {
    printf("  ALLOC %p %ld (r: %ld)\n", addr, nsize,
           (BLOCK_SIZE * MIN_BLOCKS) - (heap->head - heap->beginning));
  }

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

void run_for(lua_State *L, const char *code, char *result) {
  lua_settop(L, 0);

  int error;

  printf("evaling: \"%s\"\n", code);

  did_read = 0;
  error = lua_load(L, lreader, (void *)code, "eval", "t");
  if (error) {
    printf("load err: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return;
  }

  error = lua_pcall(L, 0, 1, 0);
  if (error) {
    printf("call err: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return;
  }

  size_t l = 0;

  asJSON(L, 1, result, &l);
  lua_pop(L, 1);
  printf("top: %d\n", lua_gettop(L));
}

int main(int argc, char *argv[]) {
  int fresh = 0;
  void *mem = open_db("./_db");

  struct heap_meta *heap = (struct heap_meta *)mem;
  if (heap->v != 0x13) {
    fresh = 1;
    heap->v = 0x13;
    heap->last_malloc = 0;
    heap->head = heap->beginning = (uintptr_t)(mem + sizeof(struct heap_meta));
  }

  log_alloc = 1;

  lua_State *L;

  if (fresh) {
    printf("CREATING LUA STATE\n");
    L = lua_newstate(lua_allocr, heap);
    printf("CREATED LUA STATE at %p\n", L);
  } else {
    printf("LOADING LUA STATE\n");
    L = (lua_State *)(heap->beginning + 8);
    lua_setallocf(L, lua_allocr, heap);
    printf("LOADED LUA STATE at %p\n", L);
  }

  char buff[1024];

  if (fresh) {
    run_for(L, "v = 1", buff);
    lua_gc(L, LUA_GCCOLLECT, 0);
  } else {
    run_for(L, "v = v + 1", buff);
    run_for(L, "return v", buff);
    printf("result: %s\n", buff);
    lua_gc(L, LUA_GCCOLLECT, 0);
  }

  /*run_for(L, "return 1+1", buff);*/
  /*assert(strcmp(buff, "2") == 0);*/

  /*run_for(L, "return 1+1.0", buff);*/
  /*assert(strcmp(buff, "2.0") == 0);*/

  return 0;

  run_for(L, "return 'hello\\nworld!'", buff);
  assert(strcmp(buff, "\"hello\\nworld!\"") == 0);

  run_for(L, "v = 1", buff);
  run_for(L, "return v", buff);
  assert(strcmp(buff, "1") == 0);
  run_for(L, "v = v + 1", buff);
  run_for(L, "v = v + 1", buff);
  run_for(L, "return v", buff);
  assert(strcmp(buff, "3") == 0);

  lua_close(L);
  return 0;
}

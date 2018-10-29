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
#include <lualib.h>

#include <jansson.h>

#include <assert.h>

#define BLOCK_SIZE 4096
#define MIN_BLOCKS 1

int log_alloc;

void *open_db(const char *path) {
  struct stat s;

  int err = stat(path, &s);
  int fresh = err == -1;

  if (err == -1) {
    if (errno != ENOENT) {
      printf("couldn't open db %s\n", strerror(errno));
      return NULL;
    }
  }

  int fd = open(path, O_CREAT | O_RDWR, 0660);
  if (fd == -1) {
    printf("couldn't open db %s\n", strerror(errno));
    return NULL;
  }

  err = ftruncate(fd, BLOCK_SIZE * MIN_BLOCKS);
  if (err == -1) {
    printf("could not increase db size  %s\n", strerror(errno));
    return NULL;
  }

  void *mem = mmap(NULL, BLOCK_SIZE * MIN_BLOCKS, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED) {
    printf("db map failed  %s\n", strerror(errno));
    return NULL;
  }

  return mem;
}

void *lua_allocr(void *ud, void *ptr, size_t osize, size_t nsize) {
  if (nsize == 0) {
    if (log_alloc) {
      printf("   FREE %p\n", ptr);
    }
    free(ptr);
    return NULL;
  }

  if (ptr) {
    if (log_alloc) {
      printf("REALLOC %p %ld -> %ld\n", ptr, osize, nsize);
    }
    return realloc(ptr, nsize);
  }

  void *addr = malloc(nsize);

  if (log_alloc) {
    printf("  ALLOC %p %ld\n", addr, nsize);
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
    strncpy(str, "UNKNOWN TYPE", strlen("UNKNOWN TYPE"));
    return;
  }

  size_t s = json_dumpb(v, str, 1024, JSON_ENCODE_ANY);
  str[s] = 0;
  *len = s;
}

void run_for(lua_State *L, const char *code, char *result) {
  lua_settop(L, 0);

  int error;

  printf("evaling: \"%s\"\n", code);

  error = luaL_loadbufferx(L, code, strlen(code), "eval", "t");
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

  printf("as json(%ld): %s\n", l, result);

  lua_gc(L, LUA_GCCOLLECT, 0);
}

int main(int argc, char *argv[]) {
  void *mem = open_db("./_db");

  char buff[1024];

  log_alloc = 1;
  printf("CREATING LUA STATE\n");
  lua_State *L = lua_newstate(lua_allocr, mem);
  printf("CREATED LUA STATE at %p\n", L);

  run_for(L, "return 1+1", buff);
  assert(strcmp(buff, "2") == 0);

  return 0;

  run_for(L, "return 1+1.0", buff);
  assert(strcmp(buff, "2.0") == 0);

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

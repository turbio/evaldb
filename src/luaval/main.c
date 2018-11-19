#include <stdio.h>
#include <string.h>

#include <sys/wait.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <jansson.h>

#include <assert.h>

#include "../alloc.h"

#include <ucontext.h>

int log_alloc;

void *lua_allocr(void *ud, void *ptr, size_t osize, size_t nsize) {
  struct heap_header *heap = (struct heap_header *)ud;

  if (nsize == 0) {
    if (ptr == NULL) {
      return NULL;
    }

    if (log_alloc) {
      fprintf(stderr, "   FREE %p %ld -> %ld\n", ptr, osize, nsize);
    }

    snap_free(heap, ptr);
    return NULL;
  }

  if (ptr) {
    if (log_alloc) {
      fprintf(stderr, "REALLOC %p %ld -> %ld\n", ptr, osize, nsize);
    }

    return snap_realloc(heap, ptr, nsize);
  }

  if (log_alloc) {
    fprintf(stderr, "  ALLOC %ld\n", nsize);
  }

  void *addr = snap_malloc(heap, nsize);

  return addr;
}

void as_json(lua_State *L, int index, char *str, size_t *len) {
  json_t *v = NULL;

  switch (lua_type(L, index)) {
  case LUA_TBOOLEAN:
    v = json_boolean(lua_toboolean(L, index));
    break;

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

int run_for(
    struct heap_header *heap,
    lua_State *L,
    const char *code,
    char *result,
    char *errmsg) {
  assert(lua_gettop(L) == 0);

  int error;

  fprintf(stderr, "evaling: \"%s\"\n", code);

  did_read = 0;
  error = lua_load(L, lreader, (void *)code, "eval", "t");
  if (error) {
    sprintf(errmsg, "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    goto abort;
  }

  error = lua_pcall(L, 0, 1, 0);
  if (error) {
    sprintf(errmsg, "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    goto abort;
  }

  size_t l = 0;

  as_json(L, 1, result, &l);
  lua_pop(L, 1);
  assert(lua_gettop(L) == 0);

abort:
  lua_gc(L, LUA_GCCOLLECT, 0);

  return !!error;
}

void run_tests(struct heap_header *heap, lua_State *L) {
  char buff[1024];

  run_for(heap, L, "return 1+1", buff, buff);
  assert(strcmp(buff, "2") == 0);

  run_for(heap, L, "return 1+1.0", buff, buff);
  assert(strcmp(buff, "2.0") == 0);

  run_for(heap, L, "return 'hello\\nworld!'", buff, buff);
  assert(strcmp(buff, "\"hello\\nworld!\"") == 0);

  run_for(heap, L, "v = 1", buff, buff);
  run_for(heap, L, "return v", buff, buff);
  assert(strcmp(buff, "1") == 0);
  run_for(heap, L, "v = v + 1", buff, buff);
  run_for(heap, L, "v = v + 1", buff, buff);
  run_for(heap, L, "return v", buff, buff);
  assert(strcmp(buff, "3") == 0);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <db file> [code]\n", *argv);
    exit(1);
  }

  struct heap_header *heap = init_alloc(argv, argv[1]);

  lua_State *L;

  log_alloc = 1;

  if (heap->user_ptr) {
    fprintf(stderr, "LOADING LUA STATE\n");
    L = heap->user_ptr;
    fprintf(stderr, "LOADED LUA STATE at %p\n", L);
  } else {
    fprintf(stderr, "CREATING LUA STATE\n");
    L = lua_newstate(lua_allocr, heap);
    heap->user_ptr = L;

    // be careful what we expose from the standard library, it has all sorts
    // of unsafe stuff built right in.

    // the base library has some unsafe stuff
    /*luaL_requiref(L, "_G", luaopen_base, 1);*/
    /*lua_pop(L, 1);*/

    luaL_requiref(L, "string", luaopen_string, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "table", luaopen_table, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "math", luaopen_math, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "coroutine", luaopen_coroutine, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "bit32", luaopen_bit32, 1);
    lua_pop(L, 1);

    luaL_requiref(L, "utf8", luaopen_utf8, 1);
    lua_pop(L, 1);

    fprintf(stderr, "CREATED LUA STATE at %p\n", L);
  }

  if (lua_getallocf(L, NULL) != lua_allocr) {
    lua_setallocf(L, lua_allocr, heap);
  }

  if (argc < 3) {
    return 0;
  }

  if (argv[2][0] != '-') {
    char buff[4096];

    run_for(heap, L, argv[2], buff, buff);
    fputs(buff, stdout);
    fputs("\n", stdout);
    return 0;
  }

  for (;;) {
    char inbuff[4096];

    if (fgets(inbuff, 4096, stdin) == NULL) {
      fprintf(stderr, "unexpected read error\n");
      return 1;
    }

    if (inbuff[strlen(inbuff) - 1] != '\n') {
      fprintf(stderr, "input should be \\n delimited\n");
      return 1;
    }

    json_t *q = json_loads(inbuff, 0, NULL);
    if (!json_is_object(q)) {
      fprintf(stderr, "input should be an object\n");
      return 1;
    }

    json_t *code = json_object_get(q, "code");
    if (!json_is_string(code)) {
      fprintf(stderr, "code should be a string\n");
      return 1;
    }

    const char *code_str = json_string_value(code);

    char outbuff[4096];

    int err = run_for(heap, L, code_str, outbuff, outbuff);

    json_t *qr = json_object();

    if (err) {
      json_object_set(qr, "error", json_string(outbuff));
    } else {
      json_object_set(qr, "object", json_string(outbuff));
    }

    const char *r_str = json_dumps(qr, 0);

    fputs(r_str, stdout);
    fputs("\n", stdout);
    fflush(stdout);
  }

  return 0;
}

#include <stdio.h>
#include <string.h>

#include <jansson.h>

#include <assert.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "../alloc.h"
#include "./cmdline.h"

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

    /*free(ptr);*/
    snap_free(heap, ptr);
    return NULL;
  }

  if (ptr) {
    if (log_alloc) {
      fprintf(stderr, "REALLOC %p %ld -> %ld\n", ptr, osize, nsize);
    }

    /*return realloc(ptr, nsize);*/
    return snap_realloc(heap, ptr, nsize);
  }

  if (log_alloc) {
    fprintf(stderr, "  ALLOC %ld\n", nsize);
  }

  /*return malloc(nsize);*/
  return snap_malloc(heap, nsize);
}

void unmarshal(lua_State *L, json_t *v) {
  lua_checkstack(L, 1);
  switch (json_typeof(v)) {
  case JSON_STRING:
    lua_pushstring(L, json_string_value(v));
    break;
  case JSON_INTEGER:
    lua_pushinteger(L, json_integer_value(v));
    break;
  case JSON_REAL:
    lua_pushnumber(L, json_real_value(v));
    break;
  case JSON_TRUE:
    lua_pushboolean(L, 1);
    break;
  case JSON_FALSE:
    lua_pushboolean(L, 0);
    break;
  case JSON_ARRAY:
    lua_checkstack(L, 2);
    int len = json_array_size(v);
    lua_createtable(L, len, 0);
    for (int i = 0; i < len; i++) {
      lua_pushinteger(L, i + 1);
      unmarshal(L, json_array_get(v, i));
      lua_settable(L, -3);
    }
    break;
  case JSON_OBJECT:
    lua_checkstack(L, 2);
    lua_createtable(L, 0, json_object_size(v));
    for (void *i = json_object_iter(v); i; i = json_object_iter_next(v, i)) {
      lua_pushstring(L, json_object_iter_key(i));
      unmarshal(L, json_object_iter_value(i));
      lua_settable(L, -3);
    }
    break;
  case JSON_NULL:
    lua_pushnil(L);
    break;
  default:
    lua_pushstring(L, "idk what this is");
    break;
  }
}

json_t *marshal(lua_State *L, int index, int depth) {
  if (depth > 5) {
    return json_string("!max depth!");
  }

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

  case LUA_TTABLE: {
    int ordered = 1;

    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
      if (lua_type(L, -2) != LUA_TNUMBER || lua_tointeger(L, -2) != ordered) {
        ordered = -1;
        lua_pop(L, 2);
        break;
      }

      ordered++;

      lua_pop(L, 1);
    }

    if (ordered == -1 || ordered == 1) {
      v = json_object();

      lua_pushnil(L);
      while (lua_next(L, index) != 0) {
        fprintf(
            stderr,
            "%s - %s\n",
            lua_typename(L, lua_type(L, -2)),
            lua_typename(L, lua_type(L, -1)));

        const char *key;

        if (lua_type(L, -2) == LUA_TSTRING) {
          key = lua_tostring(L, -2);
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
          lua_pushvalue(L, -2);
          key = lua_tostring(L, -1);
          lua_pop(L, 1);
        } else {
          lua_pop(L, 1);
          continue;
        }

        json_object_set(v, key, marshal(L, lua_gettop(L), depth + 1));
        lua_pop(L, 1);
      }
    } else {
      v = json_array();

      lua_pushnil(L);
      while (lua_next(L, index) != 0) {
        json_array_append_new(v, marshal(L, lua_gettop(L), depth + 1));
        lua_pop(L, 1);
      }
    }
    break;
  }
  default: {
    char b[] = "!type \0        ";
    strncat(b, lua_typename(L, lua_type(L, -1)), 8);
    strcat(b, "!");
    v = json_string(b);
    break;
  }
  }

  return v;
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
    json_t *args,
    json_t **result,
    char *errmsg) {
  assert(lua_gettop(L) == 0);

  int error;

  int argc = json_object_size(args);

  char preamble[1000] = {'\0'};

  if (argc) {
    strcat(preamble, "local ");
    int first = 1;
    for (void *i = json_object_iter(args); i;
         i = json_object_iter_next(args, i)) {
      if (!first) {
        strcat(preamble, ",");
      }
      strcat(preamble, json_object_iter_key(i));
      first = 0;
    }
    strcat(preamble, "=...\n");
  }

  fprintf(stderr, "preamble \"%s\"\n", preamble);

  char *code_gen = (char *)malloc(strlen(code) + strlen(preamble) + 1);

  strcat(code_gen, preamble);
  strcat(code_gen, code);

  did_read = 0;

  fprintf(stderr, "evaling: \"%s\"\n", code_gen);
  error = lua_load(L, lreader, (void *)code_gen, "eval", "t");
  if (error) {
    sprintf(errmsg, "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    goto abort;
  }

  free(code_gen);

  if (argc) {
    for (void *i = json_object_iter(args); i;
         i = json_object_iter_next(args, i)) {
      unmarshal(L, json_object_iter_value(i));
    }
  }

  assert(argc + 1 == lua_gettop(L));

  error = lua_pcall(L, argc, 1, 0);
  if (error) {
    sprintf(errmsg, "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    goto abort;
  }

  *result = marshal(L, 1, 0);
  lua_pop(L, 1);
  assert(lua_gettop(L) == 0);

abort:
  // lua_gc(L, LUA_GCCOLLECT, 0);

  return !!error;
}

void walk_generations(struct snap_generation *g) {
  printf("%d\n", g->gen);

  for (int i = 0; i < GENERATION_CHILDREN; i++) {
    if (g->c[i] && g->c[i]->type == SNAP_NODE_GENERATION) {
      walk_generations((struct snap_generation *)g->c[i]);
    }
  }
}

void list_generations(struct heap_header *heap) {
  walk_generations(heap->root);
  printf("working: %d\n", heap->working->gen);
  printf("committed: %d\n", heap->committed->gen);
}

int main(int argc, char *argv[]) {
  struct gengetopt_args_info args;
  cmdline_parser(argc, argv, &args);

  struct heap_header *heap = snap_init(argv, args.db_arg);

  if (args.list_flag) {
    list_generations(heap);
    return 0;
  }

  if (args.checkout_given) {
    snap_checkout(heap, args.checkout_arg);
  }

  lua_State *L;

  /*log_alloc = 1;*/

  if (heap->user_ptr) {
    fprintf(stderr, "LOADING LUA STATE\n");
    L = heap->user_ptr;
    fprintf(stderr, "LOADED LUA STATE at %p\n", (void *)L);
  } else {
    fprintf(stderr, "CREATING LUA STATE\n");
    L = lua_newstate(lua_allocr, heap);

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

    fprintf(stderr, "CREATED LUA STATE at %p\n", (void *)L);

    heap->user_ptr = L;

    snap_commit(heap);
  }

  if (args.eval_given) {
    json_t *interp_args = json_object();

    for (int i = 0; i < args.arg_given; i++) {
      char *eql = strchr(args.arg_arg[i], '=');
      *eql = '\0';

      json_t *v = json_loads(eql + 1, JSON_DECODE_ANY, NULL);

      if (!v) {
        fprintf(
            stderr, "could not parse arg: %s = %s\n", args.arg_arg[i], eql + 1);
        exit(1);
      }

      json_object_set(interp_args, args.arg_arg[i], v);
    }

    char buff[4096];

    json_t *r;

    snap_begin_mut(heap);

    if (run_for(heap, L, args.eval_arg, interp_args, &r, buff)) {
      fputs("error:", stdout);
      fputs(buff, stdout);
      fputs("\n", stdout);
    } else {
      fputs(json_dumps(r, JSON_ENCODE_ANY), stdout);
      fputs("\n", stdout);
    }

    snap_commit(heap);

    return 0;
  }

  while (args.server_flag) {
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

    json_t *args = json_object_get(q, "args");
    if (!json_is_object(args)) {
      fprintf(stderr, "args should be an object\n");
      return 1;
    }

    json_t *code = json_object_get(q, "code");
    if (!json_is_string(code)) {
      fprintf(stderr, "code should be a string\n");
      return 1;
    }

    const char *code_str = json_string_value(code);

    char errbuff[4096];

    json_t *result;

    snap_begin_mut(heap);

    int err = run_for(heap, L, code_str, args, &result, errbuff);

    snap_commit(heap);

    json_t *qr = json_object();

    if (err) {
      json_object_set(qr, "error", json_string(errbuff));
    } else {
      json_object_set(qr, "object", result);
    }

    const char *r_str = json_dumps(qr, 0);

    fputs(r_str, stdout);
    fputs("\n", stdout);
    fflush(stdout);
  }

  return 0;
}

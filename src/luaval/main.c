#include <jansson.h>
#include <stdio.h>
#include <string.h>

#include <assert.h>
#include <stdlib.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include "../alloc.h"
#include "../driver/evaler.h"

void *lua_allocr(void *ud, void *ptr, size_t osize, size_t nsize) {
  struct heap_header *heap = (struct heap_header *)ud;

  if (nsize == 0) {
    if (ptr == NULL) {
      return NULL;
    }
    snap_free(heap, ptr);
    return NULL;
  }

  if (ptr) {
    return snap_realloc(heap, ptr, nsize);
  }

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

    const char *key;
    json_t *value;
    // cppcheck-suppress uninitvar
    json_object_foreach(v, key, value) {
      lua_pushstring(L, key);
      unmarshal(L, value);
      lua_settable(L, -3);
    }

    break;
  case JSON_NULL:
    lua_pushnil(L);
    break;
  default:
    assert(0 && "unknown type in json");
    break;
  }
}

json_t *marshal(lua_State *L, int index, int depth) {
  if (depth > 5) {
    return json_string("max depth!");
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

        json_object_set_new(v, key, marshal(L, lua_gettop(L), depth + 1));
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
    char b[] = "type \0        ";
    strncat(b, lua_typename(L, lua_type(L, -1)), 8);
    v = json_string(b);
    break;
  }
  }

  return v;
}

struct lread_status {
  int did_read;
  const char *str;
};

const char *lreader(lua_State *L, void *data, size_t *size) {
  struct lread_status *s = (struct lread_status *)data;

  if (s->did_read) {
    *size = 0;
    return NULL;
  }

  s->did_read = 1;

  *size = strlen(s->str);
  return s->str;
}

enum evaler_status create_init(struct heap_header *heap) {
  lua_State *L = lua_newstate(lua_allocr, heap);

  if (!L) {
    return INTERNAL_ERROR;
  }

  // be careful what we expose from the standard library, it has all sorts
  // of unsafe stuff built right in. Specifically, base and os need to be
  // handled safely.
  luaL_requiref(L, "_G", luaopen_base, 1);
  lua_pop(L, 1);

  luaL_requiref(L, "os", luaopen_os, 1);
  lua_pop(L, 1);

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

  heap->user_ptr = L;

  return OK;
}

int arg_len(json_t *args) {
  int preamble_len = 0;

  preamble_len = strlen("local ");

  const char *key;
  json_t *value;

  int start = 1;
  // cppcheck-suppress uninitvar
  json_object_foreach(args, key, value) {
    if (!start) {
      preamble_len += strlen(", ");
    }
    start = 0;
    preamble_len += strlen(key);
  }
  preamble_len += strlen(" =...\n");

  return preamble_len;
}

json_t *do_eval(
    struct heap_header *heap,
    const char *code,
    json_t *args,
    enum evaler_status *status) {

  json_t *result = NULL;

  lua_State *L = heap->user_ptr;

  assert(lua_gettop(L) == 0);

  int error;

  int argc = json_object_size(args);

  struct lread_status reader = {
      .did_read = 0,
      .str = NULL,
  };

  char *ambled = NULL;

  if (argc) {
    int preamble_len = arg_len(args);

    char preamble[preamble_len];
    preamble[0] = 0;

    strcat(preamble, "local ");

    const char *key;
    json_t *value;

    int start = 1;
    // cppcheck-suppress uninitvar
    json_object_foreach(args, key, value) {
      if (!start) {
        strcat(preamble, ",");
      }
      start = 0;
      strcat(preamble, key);
    }

    strcat(preamble, "=...\n");

    ambled = (char *)malloc(strlen(code) + strlen(preamble) + 1);
    strcpy(ambled, preamble);
    strcat(ambled, code);
    reader.str = ambled;
  } else {
    reader.str = code;
  }

  lua_checkstack(L, 1);
  error = lua_load(L, lreader, (void *)&reader, "eval", "t");
  if (error) {
    result = json_string(lua_tostring(L, -1));
    lua_pop(L, 1);

    *status = USER_ERROR;

    goto abort;
  }

  if (argc) {
    const char *key;
    json_t *value;
    // cppcheck-suppress uninitvar
    json_object_foreach(args, key, value) { unmarshal(L, value); }
  }

  assert(argc + 1 == lua_gettop(L));

  error = lua_pcall(L, argc, 1, 0);
  if (error) {
    result = marshal(L, 1, 0);

    lua_pop(L, 1);

    *status = USER_ERROR;

    goto abort;
  }

  result = marshal(L, 1, 0);

  lua_pop(L, 1);

  *status = OK;

abort:
  lua_gc(L, LUA_GCCOLLECT, 0);

  if (ambled) {
    free(ambled);
  }

  assert(lua_gettop(L) == 0);

  return result;
}

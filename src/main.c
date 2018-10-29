#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

void *lua_allocr(void *ud, void *ptr, size_t osize, size_t nsize) {
  if (nsize == 0) {
    // printf("LUA FREE\n");
    free(ptr);
    return NULL;
  }

  // printf("LUA ALLOC %ld -> %ld\n", osize, nsize);
  return realloc(ptr, nsize);
}

void asJSON(lua_State *L, int index, char *str) {
  int t = lua_type(L, index);

  switch (t) {
  case LUA_TNUMBER:
    lua_Number n = lua_tonumber(L, index);
    strcpy(str, "UNKNOWN TYPE");
    break;
  default:
    strcpy(str, "UNKNOWN TYPE");
    break;
  }

  /*sprintf(str, "%s", lua_typename(L, t));*/
}

int main(int argc, char *argv[]) {
  printf("BEGIN LUA STUFF\n");

  char buff[1024];
  int error;

  lua_State *L = lua_newstate(lua_allocr, NULL);

  while (fgets(buff, sizeof(buff), stdin) != NULL) {
    lua_settop(L, 0);

    printf("evaling: \"%s\"\n", buff);

    error = luaL_loadbufferx(L, buff, strlen(buff), "eval", "t");
    if (error) {
      printf("load err: %s\n", lua_tostring(L, -1));
      lua_pop(L, 1);
      continue;
    }

    error = lua_pcall(L, 0, 1, 0);
    if (error) {
      printf("call err: %s\n", lua_tostring(L, -1));
      lua_pop(L, 1);
      continue;
    }

    char result[1024];
    asJSON(L, 1, result);
    lua_pop(L, 1);
    printf("as json: %s\n", result);

    lua_gc(L, LUA_GCCOLLECT, 0);
  }

  lua_close(L);
  return 0;
}

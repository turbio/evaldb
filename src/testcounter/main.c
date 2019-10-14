#include <jansson.h>
#include <stdio.h>
#include <string.h>

#include <assert.h>
#include <stdlib.h>

#include "../alloc.h"
#include "../driver/evaler.h"

enum evaler_status create_init(struct heap_header *heap) {
  heap->user_ptr = snap_malloc(heap, sizeof(uint64_t));

  return OK;
}

json_t *do_eval(
    struct heap_header *heap,
    const char *code,
    json_t *args,
    enum evaler_status *status) {

  uint64_t v = ++(*(uint64_t *)heap->user_ptr);

  json_t *result = json_integer(v);

  *status = OK;
  if (!strcmp(code, "FAIL")) {
    *status = USER_ERROR;
  }

  return result;
}

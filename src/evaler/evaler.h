#pragma once

#include <jansson.h>

#include "../alloc.h"

enum evaler_status {
  OK = 0,
  INTERNAL_ERROR = 1,
  USER_ERROR = 2,
};

enum evaler_status create_init(struct heap_header *heap);
json_t *do_eval(
    struct heap_header *heap,
    const char *code,
    json_t *args,
    enum evaler_status *status);

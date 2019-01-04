#include <stdio.h>

#include <duktape.h>
#include <jansson.h>

#include "../alloc.h"
#include "../driver/evaler.h"

void handle_fatal(void *d, const char *msg) {
  fprintf(stderr, "fatal error: %s\n", msg);
}

json_t *do_eval(
    struct heap_header *heap,
    const char *code,
    json_t *args,
    enum evaler_status *status) {

  int argc = json_object_size(args);

  int premable_len = strlen(
      "function("
      "){");

  if (argc) {
    const char *key;
    json_t *value;
    json_object_foreach(args, key, value) {
      premable_len += strlen(key) + strlen(",");
    }
  }

  char preamble[premable_len];
  strcpy(preamble, "function(");

  if (argc) {
    const char *key;
    json_t *value;
    json_object_foreach(args, key, value) {
      strcat(preamble, key);
      strcat(preamble, ",");
    }
  }

  strcat(preamble, "){");

  const char *end = "}";

  char *wrapped =
      (char *)malloc(strlen(preamble) + strlen(code) + strlen(end) + 1);
  wrapped[0] = '\0';

  strcat(wrapped, preamble);
  strcat(wrapped, code);
  strcat(wrapped, end);

  duk_context *ctx = heap->user_ptr;

  duk_require_stack(ctx, 1);

  duk_push_string(ctx, "eval");
  int err = duk_pcompile_string_filename(ctx, DUK_COMPILE_FUNCTION, wrapped);
  free(wrapped);
  if (err) {
    const char *msg = duk_safe_to_string(ctx, -1);
    json_t *jmsg = json_string(msg);
    duk_pop(ctx);
    *status = USER_ERROR;
    return jmsg;
  }

  if (argc) {
    const char *key;
    json_t *value;
    json_object_foreach(args, key, value) {
      char *argstr = json_dumps(value, JSON_ENCODE_ANY);
      duk_push_string(ctx, argstr);
      duk_json_decode(ctx, -1);
      free(argstr);
    }
  }

  err = duk_pcall(ctx, argc);
  if (err) {
    const char *msg = duk_safe_to_string(ctx, -1);
    json_t *jmsg = json_string(msg);
    duk_pop(ctx);
    *status = USER_ERROR;
    return jmsg;
  }

  const char *resultstr = duk_json_encode(ctx, -1);
  json_t *result = json_loads(resultstr, JSON_DECODE_ANY, NULL);
  if (!result) {
    result = json_null();
  }

  duk_pop(ctx);

  *status = OK;

  return result;
}

enum evaler_status create_init(struct heap_header *heap) {
  duk_context *ctx = duk_create_heap(
      (void *(*)(void *, size_t))snap_malloc,
      (void *(*)(void *, void *, size_t))snap_realloc,
      (void (*)(void *, void *))snap_free,
      heap,
      handle_fatal);
  if (!ctx) {
    fprintf(stderr, "could not initialize context\n");
    return 1;
  }

  heap->user_ptr = ctx;

  return OK;
}

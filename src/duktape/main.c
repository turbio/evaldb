#include <stdio.h>

#include <duktape.h>
#include <jansson.h>

#include "../evaler/evaler.h"

void *_malloc(void *d, size_t size) { return malloc(size); }
void *_realloc(void *d, void *ptr, size_t size) { return realloc(ptr, size); }
void _free(void *d, void *ptr) { free(ptr); }

void fatal(void *d, const char *msg) {
  fprintf(stderr, "fatal error: %s\n", msg);
}

json_t *do_eval(
    struct heap_header *heap,
    const char *code,
    json_t *args,
    enum evaler_status *status) {

  const char *before = "function () {";
  const char *after = "}";

  char *wrapped =
      (char *)malloc(strlen(code) + strlen(before) + strlen(after) + 1);
  wrapped[0] = '\0';

  strcat(wrapped, before);
  strcat(wrapped, code);
  strcat(wrapped, after);

  duk_context *ctx = heap->user_ptr;

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

  err = duk_pcall(ctx, 0);
  if (err) {
    const char *msg = duk_safe_to_string(ctx, -1);
    json_t *jmsg = json_string(msg);
    duk_pop(ctx);
    *status = USER_ERROR;
    return jmsg;
  }

  const char *strr = duk_json_encode(ctx, -1);
  fprintf(stderr, "hmm: %s\n", strr);
  json_t *result = json_loads(strr, JSON_DECODE_ANY, NULL);

  duk_pop(ctx);

  *status = OK;

  return result;
}

enum evaler_status create_init(struct heap_header *heap) {

  duk_context *ctx = duk_create_heap(_malloc, _realloc, _free, NULL, fatal);
  if (!ctx) {
    fprintf(stderr, "could not initialize context\n");
    return 1;
  }

  heap->user_ptr = ctx;

  return OK;
}

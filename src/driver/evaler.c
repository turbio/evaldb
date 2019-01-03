#include <assert.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmdline.h"
#include "evaler.h"

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

int from_eval_arg(struct gengetopt_args_info args, struct heap_header *heap) {
  json_t *interp_args = json_object();

  for (unsigned int i = 0; i < args.arg_given; i++) {
    const char *key = args.arg_arg[i];
    char *eql = strchr(key, '=');
    *eql = '\0';

    json_t *val = json_loads(eql + 1, JSON_DECODE_ANY, NULL);

    if (!val) {
      fprintf(stderr, "could not parse arg: %s = %s\n", key, eql + 1);
      exit(1);
    }

    json_object_set_new(interp_args, key, val);
  }

  snap_begin_mut(heap);

  enum evaler_status status;

  json_t *result = do_eval(heap, args.eval_arg, interp_args, &status);

  if (!result) {
    fprintf(stdout, "internal error\n");
    return 0;
  }

  if (status) {
    fputs("error: ", stdout);
  }

  char *rstr = json_dumps(result, JSON_ENCODE_ANY);
  fprintf(stdout, "%s\n", rstr);

  free(rstr);
  json_decref(result);
  json_decref(interp_args);

  snap_commit(heap);

  return 0;
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

  if (!heap->user_ptr) {
    fprintf(stderr, "CREATING INITIAL STATE\n");

    if (create_init(heap)) {
      fprintf(stderr, "fatal, unable to create evaler\n");
      return 1;
    }

    assert(heap->user_ptr != NULL);

    snap_commit(heap);
    fprintf(stderr, "CREATED INITIAL STATE\n");
  } else {
    assert(heap->user_ptr != NULL);
  }

  if (args.eval_given) {
    return from_eval_arg(args, heap);
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
    if (!q) {
      fprintf(stderr, "unable to parse input\n");
      return 1;
    }

    if (!json_is_object(q)) {
      fprintf(stderr, "input should be an object\n");
      json_decref(q);
      return 1;
    }

    json_t *args = json_object_get(q, "args");
    if (!json_is_object(args)) {
      fprintf(stderr, "args should be an object\n");
      json_decref(q);
      return 1;
    }

    json_t *code = json_object_get(q, "code");
    if (!json_is_string(code)) {
      fprintf(stderr, "code should be a string\n");
      json_decref(q);
      return 1;
    }

    const char *code_str = json_string_value(code);
    enum evaler_status status;

    snap_begin_mut(heap);

    json_t *result = do_eval(heap, code_str, args, &status);

    json_decref(q);

    json_t *qr = json_object();

    if (status) {
      json_object_set_new(qr, "error", result);
    } else {
      json_object_set_new(qr, "object", result);
    }

    char *r_str = json_dumps(qr, 0);

    fputs(r_str, stdout);
    fputs("\n", stdout);
    fflush(stdout);

    json_decref(qr);
    free(r_str);

    snap_commit(heap);
  }

  return 0;
}

#include <assert.h>
#include <errno.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>

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
  printf("  working: %d\n", heap->working->gen);
  printf("committed: %d\n", heap->committed->gen);
  walk_generations(heap->root);
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

  int parent_gen = heap->committed->gen;

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

  char rstrc[strlen(rstr)];
  strcpy(rstrc, rstr);

  free(rstr);
  json_decref(result);
  json_decref(interp_args);

  snap_commit(heap);

  if (status) {
    snap_checkout(heap, parent_gen);
  }

  fprintf(stdout, "%s\n", rstrc);

  return 0;
}

void server_loop(struct heap_header *heap) {
  for (;;) {
    char inbuff[4096];

    if (!fgets(inbuff, 4096, stdin)) {
      return;
    }

    if (inbuff[strlen(inbuff) - 1] != '\n') {
      fprintf(stderr, "input should be \\n delimited\n");
      return;
    }

    json_t *q = json_loads(inbuff, 0, NULL);
    if (!q) {
      fprintf(stderr, "unable to parse input\n");
      return;
    }

    if (!json_is_object(q)) {
      fprintf(stderr, "input should be an object\n");
      json_decref(q);
      return;
    }

    json_t *gen = json_object_get(q, "gen");
    if (gen) {
      if (!json_is_integer(gen)) {
        fprintf(stderr, "gen should be an integer\n");
        json_decref(q);
        return;
      }

      snap_checkout(heap, json_integer_value(gen));
    }

    json_t *args = json_object_get(q, "args");
    if (!json_is_object(args)) {
      fprintf(stderr, "args should be an object\n");
      json_decref(q);
      return;
    }

    json_t *code = json_object_get(q, "code");
    if (!json_is_string(code)) {
      fprintf(stderr, "code should be a string\n");
      json_decref(q);
      return;
    }

    int readonly = 0;

    json_t *ronly = json_object_get(q, "readonly");
    readonly = json_is_true(ronly);

    const char *code_str = json_string_value(code);
    enum evaler_status status;

    int parent_gen = heap->committed->gen;

    snap_begin_mut(heap);

    json_t *result = do_eval(heap, code_str, args, &status);

    int result_gen = snap_commit(heap);

    json_decref(q);

    json_t *qr = json_object();

    if (status) {
      snap_checkout(heap, parent_gen);
      json_object_set_new(qr, "error", result);
    } else if (readonly) {
      snap_checkout(heap, parent_gen);
      json_object_set_new(qr, "object", result);
    } else {
      json_object_set_new(qr, "object", result);
    }

    json_object_set_new(qr, "gen", json_integer(result_gen));
    json_object_set_new(qr, "parent", json_integer(parent_gen));

    char *r_str = json_dumps(qr, 0);

    fputs(r_str, stdout);
    fputs("\n", stdout);
    fflush(stdout);

    json_decref(qr);
    free(r_str);
  }
}

int main(int argc, char *argv[]) {
  struct gengetopt_args_info args;
  cmdline_parser(argc, argv, &args);

  if (!args.noaslr_flag) {
    int pers = personality(0xffffffff);
    if (pers == -1) {
      fprintf(stderr, "could not get personality %s\n", strerror(errno));
      exit(1);
    }

    if (!(pers & ADDR_NO_RANDOMIZE)) {
      if (personality(ADDR_NO_RANDOMIZE) == -1) {
        fprintf(stderr, "could not set personality %s\n", strerror(errno));
        exit(1);
      }

      execve("/proc/self/exe", argv, NULL);
    }
  }

  struct heap_header *heap = snap_init(args.db_arg);
  if (heap == NULL) {
    fprintf(stderr, "fatal, unable to create heap\n");
    return 1;
  }

  if (args.list_flag) {
    list_generations(heap);
    goto cleanup;
  }

  if (args.checkout_given) {
    snap_checkout(heap, args.checkout_arg);
  }

  if (!heap->user_ptr) {
#ifdef DEBUG_LOGGING
    fprintf(stderr, "CREATING INITIAL STATE\n");
#endif

    if (create_init(heap)) {
      fprintf(stderr, "fatal, unable to create evaler\n");
      goto cleanup;
    }

    assert(heap->user_ptr != NULL);

    snap_commit(heap);
#ifdef DEBUG_LOGGING
    fprintf(stderr, "CREATED INITIAL STATE\n");
#endif
  }

  if (args.eval_given) {
    from_eval_arg(args, heap);
    goto cleanup;
  }

  if (args.server_flag) {
    server_loop(heap);
  }

cleanup:

  cmdline_parser_free(&args);

  return 0;
}

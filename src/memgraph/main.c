#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sys/personality.h>

#include "../alloc.h"

int n = 0;

void print_node(void *addr, long size, const char *state, int committed) {
  printf("\"%p\" ", addr);
  printf("[");
  printf(" tooltip=\"%p\\nsize: %ld\\nstate: %s\"", addr, size, state);
  /*printf(" label=\"%p\\nsize: %ld\\nstate: %s\"", addr, size, state);*/
  printf(" label=\"\"");
  printf(" width=.1");
  printf(" height=.1");
  printf(" style=filled");
  printf(" fontsize=9");
  printf(" shape=box");
  printf(" penwidth=.5");
  if (committed) {
    printf(" color=\"#888888\"");
  } else {
    printf(" color=\"#ff0000\"");
  }
  if (!strcmp(state, "NODE")) {
    printf(" fillcolor=\"#aaaaff\"");
  } else if (!strcmp(state, "FREE")) {
    printf(" fillcolor=\"#ffaaaa\"");
  } else if (!strcmp(state, "USED")) {
    printf(" fillcolor=\"#aaffaa\"");
  } else {
    printf(" fillcolor=\"#888888\"");
  }
  printf("]\n");
  n++;
}

void print_node_connection(void *from, void *to) {
  printf("\"%p\" -> \"%p\" ", from, to);
  printf("[arrowsize=.25]\n");
}

void print_tree_nodes(struct heap_frame *frame) {
  print_node(frame, frame->size, "NODE", frame->committed);

  for (int i = 0; i < NODE_CHILDREN; i++) {
    print_node_connection(frame, frame->c[i]);

    if (frame->ctype[i] == USED_LEAF) {
      struct heap_leaf *leaf = (struct heap_leaf *)frame->c[i];
      print_node(leaf, leaf->size, "USED", leaf->committed);
    } else if (frame->ctype[i] == FREE_LEAF) {
      struct heap_leaf *leaf = (struct heap_leaf *)frame->c[i];
      print_node(leaf, leaf->size, "FREE", leaf->committed);
    } else if (frame->ctype[i] == EMPTY) {
      print_node(frame->c[i], 0, "EMPTY", 0);
    } else if (frame->ctype[i] == FRAME) {
      print_tree_nodes(frame->c[i]);
    } else {
      print_node(frame->c[i], 0, "UNKNOWN", 0);
    }
  }
}

void print_table_node(void *addr, long size, const char *state) {
  printf("<tr>\n");
  printf("<td ");
  printf("port=\"%p\" ", addr);

  if (!strcmp(state, "NODE")) {
    printf("bgcolor=\"#aaaaff\" ");
    printf("height=\"%ld\" ", sizeof(struct heap_frame) / 10);
    printf(">%p +%ld</td>\n", addr, sizeof(struct heap_frame));
  } else if (!strcmp(state, "FREE")) {
    printf("bgcolor=\"#ffaaaa\" ");
    printf("height=\"%ld\" ", (size + sizeof(struct heap_leaf)) / 10);
    printf(">%p %ld + %ld</td>\n", addr, size, sizeof(struct heap_leaf));
  } else if (!strcmp(state, "USED")) {
    printf("bgcolor=\"#aaffaa\" ");
    printf("height=\"%ld\" ", (size + sizeof(struct heap_leaf)) / 10);
    printf(">%p %ld + %ld</td>\n", addr, size, sizeof(struct heap_leaf));
  } else {
    printf(">%p</td>\n", addr);
  }
  printf("</tr>\n");
}

void table_nodes(struct heap_frame *frame) {
  print_table_node(frame, frame->size, "NODE");

  for (int i = 0; i < NODE_CHILDREN; i++) {
    if (frame->ctype[i] == USED_LEAF) {
      struct heap_leaf *leaf = (struct heap_leaf *)frame->c[i];
      print_table_node(leaf, leaf->size, "USED");
    } else if (frame->ctype[i] == FREE_LEAF) {
      struct heap_leaf *leaf = (struct heap_leaf *)frame->c[i];
      print_table_node(leaf, leaf->size, "FREE");
    } else if (frame->ctype[i] == EMPTY) {
      print_table_node(0, 0, "EMPTY");
    } else if (frame->ctype[i] == FRAME) {
      table_nodes(frame->c[i]);
    }
  }
}

void table_node_connections(struct heap_frame *frame) {
  printf("\"%p\" -> table:\"%p\"", frame, frame);
  printf("[arrowsize=.5]\n");

  for (int i = 0; i < NODE_CHILDREN; i++) {
    if (frame->ctype[i] == USED_LEAF) {
      printf("\"%p\" -> table:\"%p\"", frame->c[i], frame->c[i]);
      printf("[arrowsize=.5]\n");
    } else if (frame->ctype[i] == FREE_LEAF) {
      printf("\"%p\" -> table:\"%p\"", frame->c[i], frame->c[i]);
      printf("[arrowsize=.5]\n");
    } else if (frame->ctype[i] == FRAME) {
      table_node_connections(frame->c[i]);
    }
  }
}

void render_tree(struct heap_header *heap) {
  printf("nodesep=0\n");
  printf("ranksep=0.1\n");

  printf("\"committed\" [shape=box fontsize=9]\n");
  printf("\"working\" [shape=box fontsize=9]\n");

  printf("\"committed\" -> \"revision %d\" [arrowsize=.25]\n", heap->committed);
  printf("\"working\" -> \"revision %d\" [arrowsize=.25]\n", heap->working);

  for (int i = 0; i < NUM_REVISIONS; i++) {
    if (!heap->revs[i]) {
      continue;
    }

    printf("\"revision %d\" [shape=box fontsize=9]\n", i);
    printf("\"revision %d\" -> \"%p\" [arrowsize=.25]\n", i, heap->revs[i]);

    print_tree_nodes(heap->revs[i]);
  }
}

void render_mem_table(struct heap_header *heap) {
  printf("table [label=<\n");
  printf("<table border=\"0\" cellborder=\"1\" cellspacing=\"0\">\n");
  table_nodes(root(heap));
  printf("</table>\n");
  printf("> style=filled fontsize=9 shape=plain]\n");
}

void find_bounds(struct heap_frame *frame, size_t *min, size_t *max) {
  if (frame->size > *max) {
    *max = frame->size;
  }
  if (frame->size < *min) {
    *min = frame->size;
  }

  for (int i = 0; i < NODE_CHILDREN; i++) {
    if (frame->ctype[i] == USED_LEAF || frame->ctype[i] == FREE_LEAF) {
      struct heap_leaf *l = frame->c[i];
      if (l->size > *max) {
        *max = l->size;
      }
      if (l->size < *min) {
        *min = l->size;
      }
    } else if (frame->ctype[i] == FRAME) {
      find_bounds(frame->c[i], min, max);
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("usage: %s <db file>\n", *argv);
    exit(1);
  }

  struct heap_header *heap = init_alloc(argv, argv[1]);

  if (heap->v != 0xffca) {
    printf("got a bad heap! %d != %d (expected)", heap->v, 0xffca);
    exit(1);
  }

  size_t min, max;
  min = SIZE_MAX;
  max = 0;
  find_bounds(root(heap), &min, &max);

  printf("digraph \"memory\" {\n");
  /*printf("rankdir=LR\n");*/
  /*render_mem_table(heap);*/
  render_tree(heap);

  printf("}\n");

  return 0;
}

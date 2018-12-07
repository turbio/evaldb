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

void print_node(void *addr, long size, const char *state) {
  printf("\"%p\" ", addr);
  printf("[");
  printf(" tooltip=\"%p\\nsize: %ld\\nstate: %s\"", addr, size, state);
  printf(" label=\"\"");
  printf(" width=.1");
  printf(" height=.1");
  printf(" style=filled");
  printf(" fontsize=9");
  printf(" shape=box");
  printf(" color=\"#888888\"");
  if (!strcmp(state, "NODE")) {
    printf(" fillcolor=\"#aaaaff\"");
  } else if (!strcmp(state, "FREE")) {
    printf(" fillcolor=\"#ffaaaa\"");
  } else if (!strcmp(state, "USED")) {
    printf(" fillcolor=\"#aaffaa\"");
  }
  printf("]");
  n++;
}

void print_tree_nodes(struct heap_frame *frame) {
  print_node(frame, frame->size, "NODE");

  for (int i = 0; i < NODE_CHILDREN; i++) {
    printf("\"%p\" -> \"%p\"", frame, frame->c[i]);
    printf("[arrowsize=.25]\n");

    if (frame->ctype[i] == USED_LEAF) {
      struct heap_leaf *leaf = (struct heap_leaf *)frame->c[i];
      print_node(leaf, leaf->size, "USED");
    } else if (frame->ctype[i] == FREE_LEAF) {
      struct heap_leaf *leaf = (struct heap_leaf *)frame->c[i];
      print_node(leaf, leaf->size, "FREE");
    } else if (frame->ctype[i] == EMPTY) {
      print_node(0, 0, "EMPTY");
    } else if (frame->ctype[i] == FRAME) {
      print_tree_nodes(frame->c[i]);
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

  print_tree_nodes(heap->root);
}

void render_mem_table(struct heap_header *heap) {
  printf("table [label=<\n");
  printf("<table border=\"0\" cellborder=\"1\" cellspacing=\"0\">\n");
  table_nodes(heap->root);
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
  find_bounds(heap->root, &min, &max);

  printf("digraph \"memory\" {\n");
  // printf("rankdir=LR\n");
  // render_mem_table(heap);
  render_tree(heap);

  printf("}\n");

  return 0;
}

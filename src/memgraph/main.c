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

void print_segment(struct snap_segment *s, int labels) {
  printf("\"%p\" ", (void *)s);
  printf("[");
  if (labels) {
    printf(" label=\"%p\\nsize: %ld\"", (void *)s, s->size);
    printf(" tooltip=\"%p\\nsize: %ld\"", (void *)s, s->size);
  } else {
    printf(" label=\"\"");
  }
  printf(" width=.1");
  printf(" height=.1");
  printf(" style=filled");
  printf(" fontsize=9");
  printf(" shape=box");
  printf(" penwidth=.5");
  if (s->used) {
    printf(" fillcolor=\"#aaffaa\"");
  } else {
    printf(" fillcolor=\"#ffaaaa\"");
  }
  printf("]\n");
  n++;
}

void print_node(struct snap_node *n, int labels) {
  printf("\"%p\" ", (void *)n);
  printf("[");
  if (n->type == SNAP_NODE_GENERATION) {
    struct snap_generation *g = (struct snap_generation *)n;
    printf(" label=\"%p\\ngen: %d\"", (void *)n, g->gen);
    printf(" tooltip=\"%p\\ngen: %d\"", (void *)n, g->gen);
    printf(" fillcolor=\"#aaaaff\"");
  } else if (n->type == SNAP_NODE_PAGE) {
    struct snap_page *p = (struct snap_page *)n;
    printf(" label=\"%p\\nlen: %d\\npages: %d\"", (void *)n, p->len, p->pages);
    printf(
        " tooltip=\"%p\\nlen: %d\\npages: %d\"", (void *)n, p->len, p->pages);
    printf(" fillcolor=\"#ffffaa\"");
  } else {
    printf(" label=\"%p\\nUNKNOWN\"", (void *)n);
    printf(" tooltip=\"%p\\nUNKNOWN\"", (void *)n);
    printf(" fillcolor=\"#888888\"");
  }
  if (!labels) {
    printf(" label=\"\"");
  }
  printf(" width=.1");
  printf(" height=.1");
  printf(" style=filled");
  printf(" fontsize=9");
  printf(" shape=box");
  printf(" penwidth=.5");
  if (n->committed) {
    printf(" color=\"#888888\"");
  } else {
    printf(" color=\"#ff0000\"");
  }
  printf("]\n");
  n++;
}

void print_node_connection(void *from, void *to) {
  printf("\"%p\" -> \"%p\" ", from, to);
  printf("[arrowsize=.25]\n");
}

void print_tree_nodes(struct snap_node *n, int labels) {
  print_node(n, labels);

  if (n->type == SNAP_NODE_GENERATION) {
    struct snap_generation *g = (struct snap_generation *)n;
    for (int i = 0; i < GENERATION_CHILDREN; i++) {
      if (!g->c[i]) {
        continue;
      }

      print_node_connection(g, g->c[i]);
      print_tree_nodes(g->c[i], labels);
    }
  } else if (n->type == SNAP_NODE_PAGE) {
    struct snap_page *p = (struct snap_page *)n;
    for (int i = 0; i < p->len; i++) {
      print_node_connection(p, p->c[i]);
      print_segment(p->c[i], labels);
    }
  } else {
    printf("unknown node type %d at %p", n->type, (void *)n);
    exit(1);
  }
}

void render_tree(struct heap_header *heap, int labels) {
  printf("nodesep=0.0\n");
  printf("ranksep=0.1\n");
  /*printf("overlap=false\n");*/

  printf("\"committed\" [shape=box fontsize=9 width=.1 height=.1]\n");
  printf("\"working\" [shape=box fontsize=9 width=.1 height=.1]\n");
  printf("\"root\" [shape=box fontsize=9 width=.1 height=.1]\n");

  printf("\"committed\" -> \"%p\" [arrowsize=.25]\n", (void *)heap->committed);
  printf("\"working\" -> \"%p\" [arrowsize=.25]\n", (void *)heap->working);
  printf("\"root\" -> \"%p\" [arrowsize=.25]\n", (void *)heap->root);

  print_tree_nodes((struct snap_node *)heap->root, labels);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("usage: %s <db file>\n", *argv);
    exit(1);
  }

  int labels = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--labels")) {
      labels = 1;
      break;
    }
  }

  struct heap_header *heap = snap_init(argv, argv[argc - 1]);

  if (heap->v != 0xffca) {
    printf("got a bad heap! %d != %d (expected)", heap->v, 0xffca);
    exit(1);
  }

  printf("digraph \"memory\" {\n");
  printf("rankdir=LR\n");
  render_tree(heap, labels);

  printf("}\n");

  return 0;
}

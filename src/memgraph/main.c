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

void print_leaf(struct snap_segment *f) {
  printf("\"%p\" ", f);
  printf("[");
  printf(" tooltip=\"%p\\nsize: %ld\"", f, f->size);
  printf(" label=\"%p\\nsize: %ld\"", f, f->size);
  /*printf(" label=\"\"");*/
  printf(" width=.1");
  printf(" height=.1");
  printf(" style=filled");
  printf(" fontsize=9");
  printf(" shape=box");
  printf(" penwidth=.5");
  if (f->used) {
    printf(" fillcolor=\"#aaffaa\"");
  } else {
    printf(" fillcolor=\"#ffaaaa\"");
  }
  printf("]\n");
  n++;
}

void print_node(struct snap_page *f) {
  printf("\"%p\" ", f);
  printf("[");
  printf(" tooltip=\"%p\\nlen: %d\"", f, 0);
  printf(" label=\"%p\\nlen: %d\\npages: %d\"", f, f->len, f->pages);
  /*printf(" label=\"\"");*/
  printf(" width=.1");
  printf(" height=.1");
  printf(" style=filled");
  printf(" fontsize=9");
  printf(" shape=box");
  printf(" penwidth=.5");
  if (f->committed) {
    printf(" color=\"#888888\"");
  } else {
    printf(" color=\"#ff0000\"");
  }
  printf(" fillcolor=\"#aaaaff\"");
  printf("]\n");
  n++;
}

void print_node_connection(void *from, void *to) {
  printf("\"%p\" -> \"%p\" ", from, to);
  printf("[arrowsize=.25]\n");
}

void print_tree_nodes(struct snap_page *frame) {
  print_node(frame);

  for (int i = 0; i < frame->len; i++) {
    print_node_connection(frame, frame->c[i]);

    print_leaf(frame->c[i]);
  }

  if (frame->next) {
    print_node_connection(frame, frame->next);
    print_tree_nodes(frame->next);
  }
}

void render_tree(struct heap_header *heap) {
  printf("nodesep=0\n");
  printf("ranksep=0.1\n");

  printf("\"committed\" [shape=box fontsize=9 width=.1 height=.1]\n");
  printf("\"working\" [shape=box fontsize=9 width=.1 height=.1]\n");

  printf("\"committed\" -> \"revision %d\" [arrowsize=.25]\n", heap->committed);
  printf("\"working\" -> \"revision %d\" [arrowsize=.25]\n", heap->working);

  for (int i = 0; i < NUM_REVISIONS; i++) {
    if (!heap->revs[i]) {
      continue;
    }

    printf("\"revision %d\" [shape=box fontsize=9 width=.1 height=.1]\n", i);
    printf("\"revision %d\" -> \"%p\" [arrowsize=.25]\n", i, heap->revs[i]);

    print_tree_nodes(heap->revs[i]);
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("usage: %s <db file>\n", *argv);
    exit(1);
  }

  struct heap_header *heap = snap_init(argv, argv[1]);

  if (heap->v != 0xffca) {
    printf("got a bad heap! %d != %d (expected)", heap->v, 0xffca);
    exit(1);
  }

  printf("digraph \"memory\" {\n");
  printf("rankdir=LR\n");
  render_tree(heap);

  printf("}\n");

  return 0;
}

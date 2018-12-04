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

void print_node(void *addr, long size, const char *state) {
  printf("\"%p\" ", addr);
  printf("[");
  printf(" label=\"%p\\nsize: %ld\\nstate: %s\"", addr, size, state);
  printf(" shape=box");
  printf(" fontsize=9");
  printf(" color=\"#888888\"");
  if (!strcmp(state, "NODE")) {
    printf(" fillcolor=\"#aaaaff\"");
  } else if (!strcmp(state, "FREE")) {
    printf(" fillcolor=\"#ffaaaa\"");
  } else if (!strcmp(state, "USED")) {
    printf(" fillcolor=\"#aaffaa\"");
  }
  printf("]");
}

void mem_tree_traverse(struct heap_frame *frame, int depth) {
  /*printf("N %p %08ld\n", frame, frame->size);*/
  print_node(frame, frame->size, "NODE");

  for (int i = 0; i < NODE_CHILDREN; i++) {
    printf("\"%p\" -> \"%p\"\n", frame, frame->c[i]);

    if (frame->ctype[i] == USED_LEAF) {
      struct heap_leaf *leaf = (struct heap_leaf *)frame->c[i];
      print_node(leaf, leaf->size, "USED");
    } else if (frame->ctype[i] == FREE_LEAF) {
      struct heap_leaf *leaf = (struct heap_leaf *)frame->c[i];
      print_node(leaf, leaf->size, "FREE");
    } else if (frame->ctype[i] == EMPTY) {
      print_node(0, 0, "EMPTY");
    } else if (frame->ctype[i] == FRAME) {
      mem_tree_traverse(frame->c[i], depth + 1);
    }
  }
}

void print_mem_tree(struct heap_header *heap) {
  printf("digraph \"memory\" {\n");
  printf("node [style=filled]\n");
  mem_tree_traverse(heap->root, 0);
  printf("}\n");
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

  print_mem_tree(heap);

  // int fd = open("/proc/self/maps", O_RDONLY);

  // char buff[102400];
  // read(fd, buff, 102400);

  // printf("buff:\n%s", buff);

  return 0;
}

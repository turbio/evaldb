#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../alloc.h"

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <db file>\n", *argv);
    exit(1);
  }

  struct heap_header *heap = init_alloc(argv, argv[1]);

  for (int i = 0; i < 10; i++) {
    char *d = snap_malloc(heap, 32);
    for (int i = 0; i < 31; i++) {
      d[i] = '+';
    }
    d[31] = 0;
    printf("%s\n", d);
  }
}

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

  struct heap_header *heap = snap_init(argv, argv[1]);

  char *addrs[10];

  for (int i = 0; i < 10; i++) {
    addrs[i] = snap_malloc(heap, 32);
  }

  for (int i = 0; i < 10; i++) {
    for (int j = 0; j < 31; j++) {
      addrs[i][j] = '+';
    }
    addrs[i][31] = 0;
  }

  for (int i = 0; i < 10; i++) {
    printf("%s\n", addrs[i]);
  }

  snap_commit(heap);

  snap_begin_mut(heap);

  for (int i = 0; i < 10; i++) {
    for (int j = 0; j < 31; j++) {
      addrs[i][j] = '-';
    }
    addrs[i][31] = 0;
  }

  for (int i = 0; i < 10; i++) {
    printf("%s\n", addrs[i]);
  }

  snap_commit(heap);

  snap_checkout(heap, 0);

  for (int i = 0; i < 10; i++) {
    printf("%s\n", addrs[i]);
  }

  snap_begin_mut(heap);

  for (int i = 0; i < 10; i++) {
    for (int j = 0; j < 31; j++) {
      addrs[i][j] = '/';
    }
    addrs[i][31] = 0;
  }

  snap_commit(heap);

  for (int i = 0; i < 10; i++) {
    printf("%s\n", addrs[i]);
  }

  snap_checkout(heap, 0);

  for (int i = 0; i < 10; i++) {
    printf("%s\n", addrs[i]);
  }
}

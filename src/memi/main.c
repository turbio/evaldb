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

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("usage: %s <db file>\n", *argv);
    exit(1);
  }

  struct heap_header *heap = init_alloc(argv, argv[1]);

  printf("sizeof(struct heap_header): %ld\n", sizeof(struct heap_header));
  printf("sizeof(struct heap_frame): %ld\n", sizeof(struct heap_frame));
  printf("sizeof(struct heap_leaf): %ld\n", sizeof(struct heap_leaf));

  if (heap->v != 0xffca) {
    printf("got a bad heap! %d != %d (expected)", heap->v, 0xffca);
    exit(1);
  }

  print_mem_tree(heap);

  int fd = open("/proc/self/maps", O_RDONLY);

  char buff[102400];
  read(fd, buff, 102400);

  printf("buff:\n%s", buff);

  return 0;
}

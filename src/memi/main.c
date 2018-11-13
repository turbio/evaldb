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

#define ALLOC_BLOCK_SIZE (1000 * 1000)

int log_alloc;
const uintptr_t addr = 0x600000000000;

void *open_db(const char *path, int size) {
  int fd = open(path, O_CREAT | O_RDWR, 0660);
  if (fd == -1) {
    printf("couldn't open db %s\n", strerror(errno));
    return NULL;
  }

  int err = ftruncate(fd, size);
  if (err == -1) {
    printf("could not increase db size:  %s\n", strerror(errno));
    return NULL;
  }

  void *mem = mmap((void *)addr, size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED, fd, 0);
  if (mem == MAP_FAILED) {
    printf("db map failed:  %s\n", strerror(errno));
    return NULL;
  }

  return mem;
}

int main(int argc, char *argv[]) {
  int pers = personality(0xffffffff);
  if (pers == -1) {
    printf("could not get personality: %s", strerror(errno));
    return 1;
  }

  if (!(pers & ADDR_NO_RANDOMIZE)) {
    if (personality(ADDR_NO_RANDOMIZE) == -1) {
      printf("could not set personality: %s", strerror(errno));
      return 1;
    }

    execve("/proc/self/exe", argv, NULL);
  }

  printf("sizeof(struct heap_header): %ld\n", sizeof(struct heap_header));
  printf("sizeof(struct heap_frame): %ld\n", sizeof(struct heap_frame));
  printf("sizeof(struct heap_leaf): %ld\n", sizeof(struct heap_leaf));

  void *mem = open_db("./_db", ALLOC_BLOCK_SIZE);

  struct heap_header *heap = (struct heap_header *)mem;

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

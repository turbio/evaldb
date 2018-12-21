#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../alloc.h"

#define used_ch (char)(0xff)
#define free_ch (char)(0xff)

struct tracked_alloc {
  char *ptr;
  int size;
  char used;
};

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <db file> <log>\n", *argv);
    exit(1);
  }

  FILE *log = fopen(argv[2], "r");

  struct heap_header *heap = snap_init(argv, argv[1]);

  struct tracked_alloc allocs[1000] = {};
  int n = 0;

  char buff[1000];
  while (fgets(buff, 1000, log)) {

    buff[strlen(buff) - 1] = '\0';

    char *cmd_end = strchr(buff, ' ');
    if (cmd_end) {
      *cmd_end = '\0';
    }

    if (!strcmp(buff, "snap_malloc")) {
      long size = strtol(cmd_end + 1, NULL, 10);

      printf("%s(%ld)\n", buff, size);

      char *ptr = (char *)snap_malloc(heap, size);

      memset(ptr, used_ch, size);

      allocs[n++] = (struct tracked_alloc){
          .size = size,
          .ptr = ptr,
          .used = 1,
      };

    } else if (!strcmp(buff, "snap_free")) {
      char *addr = (char *)strtoll(cmd_end + 1, NULL, 16);

      printf("%s(%p)\n", buff, (void *)addr);

      snap_free(heap, addr);

      int found = 0;
      for (int i = 0; i < n; i++) {
        if (allocs[i].ptr == addr) {
          found = 1;
          allocs[i].used = 0;
          memset(addr, free_ch, allocs[i].size);
          break;
        }
      }
      assert(found);
    } else if (!strcmp(buff, "snap_realloc")) {
      char *next;
      char *addr = (char *)strtoll(cmd_end + 1, &next, 16);
      long new_size = strtol(next, NULL, 10);

      printf("%s(%p, %ld)\n", buff, (void *)addr, new_size);

      char *new_addr = (char *)snap_realloc(heap, addr, new_size);

      int found = 0;
      for (int i = 0; i < n; i++) {
        if (allocs[i].ptr == addr) {
          found = 1;
          allocs[i].size = new_size;
          memset(new_addr, used_ch, allocs[i].size);
          break;
        }
      }
      assert(found);

    } else if (!strcmp(buff, "snap_begin_mut")) {
    } else if (!strcmp(buff, "snap_commit")) {
    } else {
      printf("unknown command %s\n", buff);
      break;
    }

    for (int i = 0; i < n; i++) {
      struct tracked_alloc a = allocs[i];

      for (int j = 0; j < a.size; j++) {
        if (a.used && a.ptr[j] != used_ch) {
          printf("unused check failed %d != %d", a.ptr[j], used_ch);
          exit(1);
        } else if (a.ptr[j] != free_ch) {
          printf("unused check failed %d != %d", a.ptr[j], free_ch);
          exit(1);
        }
      }
    }
  }
}

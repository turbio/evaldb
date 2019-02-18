#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../alloc.h"

#define used_ch (uint8_t)(0xff)
#define free_ch (uint8_t)(0xff)

struct tracked_alloc {
  uint8_t *ptr;
  int size;
  char used;
};

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <db file> <log>\n", *argv);
    exit(1);
  }

  FILE *log = fopen(argv[2], "r");

  struct heap_header *heap = snap_init(argv[1]);

  struct tracked_alloc allocs[1000];
  int n = 0;

  int index = 0;

  char buff[1000];
  while (fgets(buff, 1000, log)) {

    buff[strlen(buff) - 1] = '\0';

    char *cmd_end = strchr(buff, ' ');
    if (cmd_end) {
      *cmd_end = '\0';
    }

    if (!strcmp(buff, "snap_malloc")) {
      char *at = NULL;
      long size = strtol(cmd_end + 1, &at, 10);

      at = strstr(at, "-> ") + strlen("-> ");

      void *expected_addr = (void *)strtoll(at, NULL, 16);

      fprintf(
          stderr,
          "%3d. expect: %s(%ld) -> %p\n",
          index,
          buff,
          size,
          expected_addr);

      uint8_t *ptr = (uint8_t *)snap_malloc(heap, size);

      fprintf(stderr, "     receiv: %s(%ld) -> %p\n", buff, size, (void *)ptr);

      if (expected_addr != ptr) {
        fprintf(
            stderr,
            "bad result: %p != %p\n",
            (void *)ptr,
            (void *)expected_addr);
      }

      assert(expected_addr == ptr);

      memset(ptr, used_ch, size);

      allocs[n++] = (struct tracked_alloc){
          .size = size,
          .ptr = ptr,
          .used = 1,
      };

    } else if (!strcmp(buff, "snap_free")) {
      uint8_t *addr = (uint8_t *)strtoll(cmd_end + 1, NULL, 16);

      fprintf(stderr, "%s(%p)\n", buff, (void *)addr);

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

      snap_free(heap, addr);
    } else if (!strcmp(buff, "snap_realloc")) {
      char *next;
      uint8_t *addr = (uint8_t *)strtoll(cmd_end + 1, &next, 16);
      long new_size = strtol(next, NULL, 10);

      uint8_t *new_addr = (uint8_t *)snap_realloc(heap, addr, new_size);

      fprintf(
          stderr,
          "%s(%p, %ld) -> %p\n",
          buff,
          (void *)addr,
          new_size,
          (void *)new_addr);

      int found = 0;
      for (int i = 0; i < n; i++) {
        if (allocs[i].ptr == addr) {
          found = 1;
          allocs[i].size = new_size;
          allocs[i].ptr = new_addr;
          memset(new_addr, used_ch, allocs[i].size);
          break;
        }
      }
      assert(found);

    } else if (!strcmp(buff, "snap_begin_mut")) {
      fprintf(stderr, "%s()\n", buff);
      snap_begin_mut(heap);
    } else if (!strcmp(buff, "snap_commit")) {
      fprintf(stderr, "%s()\n", buff);
      snap_commit(heap);
    } else {
      fprintf(stderr, "unknown command %s\n", buff);
      break;
    }

    for (int i = 0; i < n; i++) {
      struct tracked_alloc a = allocs[i];

      for (int j = 0; j < a.size; j++) {
        if (a.used && a.ptr[j] != used_ch) {
          fprintf(
              stderr,
              "section %p - %p (%d)\n",
              (void *)a.ptr,
              (void *)((char *)a.ptr + a.size),
              a.size);
          fprintf(stderr, "at %p\n", (void *)((char *)a.ptr + j));
          fprintf(stderr, "used check failed %d != %d", a.ptr[j], used_ch);
          exit(1);
        }
      }
    }

    index++;
  }

  fclose(log);
}

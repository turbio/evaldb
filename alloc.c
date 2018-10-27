#include <dlfcn.h>
#include <stdio.h>

extern void abort();

static char heap[10000];

void *malloc(int size) {
  printf("alloc %d\n", size);
  return heap;

  /*void *(*ptr)(int);*/
  /*void *handle = (void *)-1;*/
  /*ptr = (void *)dlsym(handle, "malloc");*/
  /*if (ptr == NULL) {*/
  /*printf("Opps\n");*/
  /*abort();*/
  /*}*/
  /*void *alloc = (*ptr)(size);*/
  /*printf("Alloc = %p Size: %d\n", alloc, size);*/
  /*return alloc;*/
}

void *realloc(void *alloc, int size) {
  printf("realloc %p %d\n", alloc, size);
  return heap;

  /*void *(*ptr)(void *, int);*/
  /*void *handle = (void *)-1;*/
  /*ptr = (void *)dlsym(handle, "malloc");*/
  /*if (ptr == NULL) {*/
  /*printf("Opps\n");*/
  /*abort();*/
  /*}*/
  /*alloc = (*ptr)(alloc, size);*/
  /*printf("Realloc = %p Size: %d\n", alloc, size);*/
  /*return alloc;*/
}

void free(void *alloc) { printf("free %p\n", alloc); }

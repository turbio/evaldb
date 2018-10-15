#include <fcntl.h>
#include <iostream>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.hpp"

#define BLOCK_SIZE 4096
#define MIN_BLOCKS 100

DB::DB(std::string path) {
  struct stat s {};
  int err = stat(path.c_str(), &s);
  bool fresh = err == -1;
  if (err == -1) {
    if (errno != ENOENT) {
      std::cout << "couldn't open db " << strerror(errno) << std::endl;
      return;
    }
  }

  int fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0660);
  if (fd == -1) {
    std::cout << "couldn't open db " << strerror(errno) << std::endl;
    return;
  }

  err = ftruncate(fd, BLOCK_SIZE * MIN_BLOCKS);
  if (err == -1) {
    std::cout << "could not increase db size " << strerror(errno) << std::endl;
    return;
  }

  head = (struct header *)mmap(
      nullptr,
      BLOCK_SIZE * MIN_BLOCKS,
      PROT_READ | PROT_WRITE,
      MAP_SHARED,
      fd,
      0);
  if (head == MAP_FAILED) {
    std::cout << "db map failed" << strerror(errno) << std::endl;
    return;
  }

  if (fresh) {
    head->top = sizeof(header);
  }
}

DB::frame *DB::nextFree() {
  auto top = (struct frame *)(uintptr_t(head) + head->top);

  if (top->len) {
    head->top += sizeof(frame) + top->len;
    top = (struct frame *)(uintptr_t(head) + head->top);
  }

  return top;
}

void DB::commit(const std::string &s) {
  auto top = nextFree();

  top->len = s.length();
  top->sep = '|';
  memcpy((char *)top + sizeof(frame), s.c_str(), s.length());

  if (msync(head, BLOCK_SIZE * MIN_BLOCKS, MS_SYNC) == -1) {
    std::cout << "db sync err " << strerror(errno) << std::endl;
    return;
  }
}

const char *DB::top() {
  struct frame *top =
      (struct frame *)(uintptr_t(head) + head->top + sizeof(frame));
  return (char *)(top);
}

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

//#include <chrono>
//#include <criu/criu.h>
//#include <iostream>

//#include "db.hpp"
//#include "evaler.hpp"

// void do_checkpoint(int pid, int inc) {
// auto start = std::chrono::high_resolution_clock::now();

// auto proc_dump_dir = (std::string("./imgs/") + std::to_string(pid));
// if (inc == 0) {
// auto err = mkdir(proc_dump_dir.c_str(), 0777);
// if (err != 0) {
// std::cout << "dir create failed for " << proc_dump_dir << " "
//<< strerror(errno) << std::endl;
// return;
//}
//}

// auto dump_dir = proc_dump_dir + "/" + std::to_string(inc);
// auto err = mkdir(dump_dir.c_str(), 0777);
// if (err != 0) {
// std::cout << "dir create failed for " << dump_dir << " " << strerror(errno)
//<< std::endl;
// return;
//}

//// if (inc != 0) {
////   auto parent = (std::string("../") + std::to_string(inc - 1) +
////   "/").c_str(); char p[80]; strcpy(p, parent); std::cout << "parent dir "
////   << parent << std::endl; criu_set_parent_images(p);
//// }

// int fd = open(dump_dir.c_str(), O_DIRECTORY);
// if (fd == -1) {
// std::cout << "dir open failed" << std::endl;
// return;
//}

// criu_set_pid(pid);
// criu_set_images_dir_fd(fd);

// err = criu_dump();
// if (err != 0) {
// std::cout << "err: " << err << " errno: " << strerror(errno) << std::endl;
// return;
//}

// auto stop = std::chrono::high_resolution_clock::now();
// auto duration =
// std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
// std::cout << "checkpoint took: " << duration.count() << "ms" << std::endl;
//}

int main(int argc, char *argv[]) {
  printf("oofio\n");
  return 0;

  if (argc < 3) {
    printf("Usage: %s <address> <port> [db file]\n", argv[0]);
    return 1;
  }

  return 1;

  int stdinp[2];
  int stdoutp[2];
  int stderrp[2];

  if (pipe(stdinp) == -1) {
    printf("pipe failed! %s\n", strerror(errno));
    return 1;
  }

  if (pipe(stdoutp) == -1) {
    printf("pipe failed! %s", strerror(errno));
    return 1;
  }

  if (pipe(stderrp) == -1) {
    printf("pipe failed! %s", strerror(errno));
    return 1;
  }

  int child = fork();
  if (child == -1) {
    printf("fork failed! %s", strerror(errno));
    return 1;
  }

  if (child == 0) {
    close(stdinp[1]);
    close(stdoutp[0]);
    dup2(stdinp[0], 0);
    dup2(stdoutp[1], 1);

    sleep(1000);
  } else {
    sleep(10);
  }

  // std::cout << "child is at " << child << std::endl;
  // if (criu_init_opts() == -1) {
  // std::cout << "criu setup failed!" << std::endl;
  // return 1;
  //}

  // criu_set_log_level(4);
  // criu_set_leave_running(true);
  //// criu_set_shell_job(true);
  // criu_set_log_file("restore.log");
  //// criu_set_track_mem(true);

  // for (int i = 0; i < 1000; i++) {
  // do_checkpoint(child, i);
  //}

  // kill(child, 9);

  // while (1) {
  // sleep(1);
  //}

  // waitpid(child, nullptr, 0);
  // std::cout << "child is donzo" << std::endl;

  // err = criu_restore_child();
  // if (err < 0) {
  //   std::cout << "err: " << err << " errno: " << strerror(errno) <<
  //   std::endl; kill(child, 9); return 1;
  // }

  // DB d = DB("./evaldb_store");
  // std::cout << "top: \"" << d.top() << "\"" << std::endl;

  // write(stdinp[1], "123\n", 4);

  // char buf;
  // while (read(stdoutp[0], &buf, 1) > 0) {
  //   std::cout << buf << std::endl;
  // }
  //}
}

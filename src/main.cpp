#include <criu/criu.h>
#include <fcntl.h>
#include <iostream>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "db.hpp"
#include "evaler.hpp"

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <address> <port> [db file]"
              << std::endl;
    return 1;
  }

  int ipipe[2];
  int opipe[2];

  if (pipe(ipipe) == -1) {
    std::cout << "pipe failed!" << std::endl;
    return 1;
  }

  if (pipe(opipe) == -1) {
    std::cout << "pipe failed!" << std::endl;
    return 1;
  }

  auto child = fork();
  if (child == -1) {
    std::cout << "fork failed!" << std::endl;
    return 1;
  }

  if (child == 0) {
    close(ipipe[1]);
    close(opipe[0]);
    dup2(ipipe[0], 0);
    dup2(opipe[1], 1);

    sleep(1000);

    run();
  } else {
    std::cout << "child is at " << child << std::endl;
    if (criu_init_opts() == -1) {
      std::cout << "criu setup failed!" << std::endl;
      return 1;
    }

    int fd = open("./imgs/", O_DIRECTORY);
    if (fd == -1) {
      std::cout << "dir open failed" << std::endl;
      return 1;
    }

    criu_set_pid(child);
    criu_set_images_dir_fd(fd);
    criu_set_log_level(4);
    // criu_set_leave_running(true);
    criu_set_leave_running(false);
    criu_set_shell_job(true);
    // criu_set_ext_sharing(true);
    criu_set_log_file("restore.log");

    int err = criu_dump();
    if (err) {
      std::cout << "err: " << err << " errno: " << strerror(errno) << std::endl;
      std::cout << 0 << " Success" << std::endl;
      std::cout << -EBADE << " RPC has returned fail." << std::endl;
      std::cout << -ECONNREFUSED << " Unable to connect to CRIU." << std::endl;
      std::cout << -ECOMM << " Unable to send/recv msg to/from CRIU."
                << std::endl;
      std::cout << -EINVAL
                << " CRIU doesn't support this type of request. You should "
                   "probably update CRIU."
                << std::endl;
      std::cout
          << -EBADMSG
          << " Unexpected response from CRIU. You should probably update CRIU."
          << std::endl;

      kill(child, 9);
      return 1;
    }

    waitpid(child, nullptr, 0);
    std::cout << "child is donzo" << std::endl;

    err = criu_restore();
    if (err < 0) {
      std::cout << "err: " << err << " errno: " << strerror(errno) << std::endl;
      std::cout << 0 << " Success" << std::endl;
      std::cout << -EBADE << " RPC has returned fail." << std::endl;
      std::cout << -ECONNREFUSED << " Unable to connect to CRIU." << std::endl;
      std::cout << -ECOMM << " Unable to send/recv msg to/from CRIU."
                << std::endl;
      std::cout << -EINVAL
                << " CRIU doesn't support this type of request. You should "
                   "probably update CRIU."
                << std::endl;
      std::cout
          << -EBADMSG
          << " Unexpected response from CRIU. You should probably update CRIU."
          << std::endl;

      kill(child, 9);
      return 1;
    }

    sleep(10);

    DB d = DB("./evaldb_store");
    std::cout << "top: \"" << d.top() << "\"" << std::endl;

    write(ipipe[1], "123\n", 4);

    char buf;
    while (read(opipe[0], &buf, 1) > 0) {
      std::cout << buf << std::endl;
    }
  }
}

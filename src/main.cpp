#include <iostream>

#include "db.hpp"
#include "evaler.hpp"

int main(int argc, char *argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: " << argv[0] << " <address> <port> [db file]"
              << std::endl;
    return 1;
  }

  run();

  DB d = DB("./evaldb_store");
  std::cout << "top: \"" << d.top() << "\"" << std::endl;

  for (;;) {
    std::string in;
    std::cin >> in;
    d.commit(in);
    std::cout << "top: \"" << d.top() << "\"" << std::endl;
  }
}

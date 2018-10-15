#include <string>

class DB {
public:
  explicit DB(std::string);
  void commit(const std::string &);
  const char *top();

private:
  struct frame {
    int len;
    char sep;
  };

  struct header {
    int top;
  };

  struct header *head;

  DB::frame *nextFree();
};

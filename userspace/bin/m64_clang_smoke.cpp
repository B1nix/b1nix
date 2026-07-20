#include <stdexcept>
#include <string>
#include <vector>

#include <string.h>
#include <unistd.h>

struct Base {
  virtual ~Base() {}
};
struct Derived : Base {};

extern "C" int main() {
  std::vector<std::string> values{"clang", "libstdc++"};
  Base *value = new Derived;
  int ok = values.size() == 2 && dynamic_cast<Derived *>(value);
  try {
    throw std::runtime_error("ok");
  } catch (const std::exception &e) {
    ok = ok && strcmp(e.what(), "ok") == 0;
  }
  delete value;
  const char *marker = ok ? "M64-CLANG: ok\n" : "M64-CLANG: fail\n";
  write(1, marker, strlen(marker));
  return ok ? 0 : 1;
}

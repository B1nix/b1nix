/* Hosted C++ runtime smoke: proves b1nix can RUN (not just link) C++ built
 * against the cross GCC's libstdc++ — STL containers, a global constructor via
 * crt0's .init_array, and exception throw/catch across a frame (DWARF unwind
 * registered by crt0). Every marker is gated on a real, verified result. */
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <string.h>
#include <unistd.h>

/* Global object with a constructor — fires only if crt0 runs .init_array. */
struct Global {
  int v;
  Global() : v(0x1234) {}
};
static Global g_global;

static void mark(const char *s) { write(1, s, strlen(s)); }

static int throwing(bool b) {
  if (b)
    throw std::runtime_error("boom");
  return 7;
}

int main() {
  if (g_global.v != 0x1234)
    return (mark("CXX-SMOKE: fail ctor\n"), 1);

  /* STL containers + iteration. */
  std::map<std::string, int> m;
  m["a"] = 3;
  m["b"] = 4;
  std::vector<int> v;
  for (auto &kv : m)
    v.push_back(kv.second);
  int sum = 0;
  for (int x : v)
    sum += x;
  if (sum != 7)
    return (mark("CXX-SMOKE: fail stl\n"), 1);

  /* Throw across a frame and catch by reference. */
  int caught = 0;
  try {
    throwing(true);
  } catch (const std::exception &e) {
    caught = (strcmp(e.what(), "boom") == 0);
  }
  if (!caught)
    return (mark("CXX-SMOKE: fail exceptions\n"), 1);
  if (throwing(false) != 7)
    return (mark("CXX-SMOKE: fail nothrow\n"), 1);

  mark("CXX-SMOKE: ok ctors\n");
  mark("CXX-SMOKE: ok stl\n");
  mark("CXX-SMOKE: ok exceptions\n");
  return 0;
}

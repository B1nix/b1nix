/* Hosted C++ runtime smoke: proves b1nix can RUN (not just link) C++ built
 * against the cross GCC's libstdc++ — STL containers, a global constructor via
 * crt0's .init_array, exception throw/catch across a frame (DWARF unwind
 * registered by crt0), RTTI (dynamic_cast/typeid), thread-safe function-local
 * statics (__cxa_guard over the kernel futex), and std::thread/mutex/atomic
 * over the M29 pthread layer. Every marker is gated on a real, verified
 * result. This is the M55 "C++ runtime" acceptance test. */
#include <atomic>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
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

/* Polymorphic hierarchy for RTTI (vtable + type_info). */
struct Base {
  virtual ~Base() {}
  virtual int kind() const { return 1; }
};
struct Derived : Base {
  int kind() const override { return 2; }
};

/* Thread-safe function-local static: the first caller runs the constructor
 * under __cxa_guard_acquire/release (which uses the kernel futex on b1nix);
 * the side-effecting counter proves it constructs exactly once. */
struct OnceCounter {
  int n;
  OnceCounter() : n(0) { ++ctor_calls; }
  static int ctor_calls;
};
int OnceCounter::ctor_calls = 0;

static OnceCounter &local_static() {
  static OnceCounter c;
  return c;
}

extern "C" int main() {
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

  /* RTTI: dynamic_cast down a polymorphic hierarchy + typeid identity. A
   * failed dynamic_cast to a reference also throws std::bad_cast (RTTI+EH). */
  Derived d;
  Base *bp = &d;
  Derived *dp = dynamic_cast<Derived *>(bp);
  if (!dp || dp->kind() != 2)
    return (mark("CXX-SMOKE: fail rtti\n"), 1);
  if (typeid(*bp) != typeid(Derived))
    return (mark("CXX-SMOKE: fail rtti\n"), 1);
  int bad_cast = 0;
  try {
    Base b2;
    (void)dynamic_cast<Derived &>(b2); /* must throw std::bad_cast */
  } catch (const std::bad_cast &) {
    bad_cast = 1;
  }
  if (!bad_cast)
    return (mark("CXX-SMOKE: fail rtti\n"), 1);

  /* Thread-safe function-local static: many calls, one construction. */
  for (int i = 0; i < 5; i++)
    local_static().n += 1;
  if (OnceCounter::ctor_calls != 1 || local_static().n != 5)
    return (mark("CXX-SMOKE: fail static-init\n"), 1);

  /* std::thread + std::mutex + std::atomic over the M29 pthread layer. */
  std::atomic<int> acount{0};
  long long mcount = 0;
  std::mutex mtx;
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; i++) {
    ts.emplace_back([&] {
      for (int j = 0; j < 1000; j++) {
        acount.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mtx);
        mcount += 1;
      }
    });
  }
  for (auto &t : ts)
    t.join();
  if (acount.load() != 4000 || mcount != 4000)
    return (mark("CXX-SMOKE: fail threads\n"), 1);

  mark("CXX-SMOKE: ok ctors\n");
  mark("CXX-SMOKE: ok stl\n");
  mark("CXX-SMOKE: ok exceptions\n");
  mark("CXX-SMOKE: ok rtti\n");
  mark("CXX-SMOKE: ok static-init\n");
  mark("CXX-SMOKE: ok threads\n");
  return 0;
}

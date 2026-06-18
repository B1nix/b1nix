/* M55 acceptance test: std::iostream (std::cout/std::cerr + the iostream locale
 * facets) and std::filesystem run correctly on b1nix, built against the cross
 * GCC's hosted libstdc++ via tools/b1nix-c++.
 *
 * This proves the deferred M55 bullet ("std::filesystem and locale/iostream
 * (std::cout) — thin wrappers over the existing VFS / UTF-8 libc") works
 * end-to-end:
 *   - std::cout / std::cerr stream formatted output (int, string, bool,
 *     hex/dec manipulators) to fd 1/2 through the ios_base::Init static and the
 *     numeric/ctype locale facets.
 *   - std::ostringstream / std::istringstream round-trip values through the
 *     locale machinery (no real fd, exercises just the facets + streambuf).
 *   - std::cin extracts from a real fd 0 (a pipe primed with data and dup2'd
 *     onto stdin), driving the streambuf over a descriptor like interactive
 *     input would, without blocking on an empty terminal.
 *   - std::filesystem creates a directory under /tmp, writes a file via
 *     std::ofstream, iterates the directory with directory_iterator, stats the
 *     entry (is_regular_file + file_size), then removes the tree — all backed
 *     by the b1nix VFS syscalls (mkdir/open/getdents/stat/unlink/rmdir).
 *
 * Every marker is gated on a real, verified result; nothing is printed
 * unconditionally. On any mismatch we print a "fail" line and return non-zero
 * so the kernel WAIT sees a failure. */
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

int main() {
  /* ---- std::cout / std::cerr: formatted output to real fds. ---- */
  std::cout << "M55-IOSTREAM: cout " << 42 << " " << std::hex << 255 << std::dec
            << " " << std::boolalpha << true << "\n";
  std::cout.flush();
  std::cerr << "M55-IOSTREAM: cerr line\n";
  std::cerr.flush();

  /* ---- string streams: round-trip through the locale facets. ---- */
  std::ostringstream oss;
  oss << "v=" << 7 << " s=" << std::string("abc");
  if (oss.str() != "v=7 s=abc") {
    std::cout << "M55-IOSTREAM: fail osstream\n";
    return 1;
  }
  std::istringstream iss("123 456 word");
  int a = 0, b = 0;
  std::string w;
  iss >> a >> b >> w;
  if (a != 123 || b != 456 || w != "word") {
    std::cout << "M55-IOSTREAM: fail isstream\n";
    return 1;
  }

  /* ---- std::cin: extract from a real fd 0 backed by queued data. The kernel
   * spawns us with no interactive stdin, so prime a pipe, redirect its read end
   * onto fd 0, and read through std::cin. Unlike the istringstream above this
   * drives the streambuf over a real descriptor (stdio FILE* stdin -> read(0)),
   * which is the path interactive std::cin would take. ---- */
  {
    int p[2];
    if (pipe(p) != 0) {
      std::cout << "M55-IOSTREAM: fail cin-pipe\n";
      return 1;
    }
    static const char feed[] = "314 hello";
    ssize_t want = (ssize_t)(sizeof(feed) - 1);
    if (write(p[1], feed, (size_t)want) != want) {
      std::cout << "M55-IOSTREAM: fail cin-feed\n";
      return 1;
    }
    close(p[1]); /* EOF after the queued bytes */
    if (dup2(p[0], 0) < 0) {
      std::cout << "M55-IOSTREAM: fail cin-dup2\n";
      return 1;
    }
    close(p[0]);
    int cn = 0;
    std::string cw;
    std::cin >> cn >> cw;
    if (!std::cin || cn != 314 || cw != "hello") {
      std::cout << "M55-IOSTREAM: fail cin\n";
      return 1;
    }
  }

  /* ---- std::filesystem: create / write / iterate / stat / remove. ---- */
  fs::path dir = "/tmp/m55_fs_test";
  std::error_code ec;
  fs::remove_all(dir, ec); /* clean any stale state, ignore errors */

  if (!fs::create_directory(dir, ec) || ec) {
    std::cout << "M55-IOSTREAM: fail create_directory\n";
    return 1;
  }
  if (!fs::is_directory(dir)) {
    std::cout << "M55-IOSTREAM: fail is_directory\n";
    return 1;
  }

  /* Write two files with std::ofstream (iostream -> VFS). */
  const char *names[2] = {"alpha.txt", "beta.txt"};
  const char *bodies[2] = {"hello", "worldworld"};
  for (int i = 0; i < 2; i++) {
    std::ofstream f(dir / names[i]);
    if (!f) {
      std::cout << "M55-IOSTREAM: fail ofstream\n";
      return 1;
    }
    f << bodies[i];
    f.close();
    if (!f) {
      std::cout << "M55-IOSTREAM: fail ofstream-close\n";
      return 1;
    }
  }

  /* Iterate the directory and verify entries + sizes via stat. */
  int found = 0;
  long total = 0;
  for (const auto &e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) {
      std::cout << "M55-IOSTREAM: fail entry-type\n";
      return 1;
    }
    std::string fn = e.path().filename().string();
    if (fn == "alpha.txt") {
      if (fs::file_size(e.path()) != 5) {
        std::cout << "M55-IOSTREAM: fail file_size\n";
        return 1;
      }
      found |= 1;
    } else if (fn == "beta.txt") {
      if (fs::file_size(e.path()) != 10) {
        std::cout << "M55-IOSTREAM: fail file_size\n";
        return 1;
      }
      found |= 2;
    }
    total += (long)fs::file_size(e.path());
  }
  if (found != 3) {
    std::cout << "M55-IOSTREAM: fail iterate\n";
    return 1;
  }
  if (total != 15) {
    std::cout << "M55-IOSTREAM: fail iterate-size\n";
    return 1;
  }

  /* Read one file back through std::ifstream + getline. */
  std::ifstream rf(dir / "alpha.txt");
  std::string content;
  std::getline(rf, content);
  if (content != "hello") {
    std::cout << "M55-IOSTREAM: fail ifstream\n";
    return 1;
  }

  /* Tear down the tree and confirm it is gone. */
  std::uintmax_t removed = fs::remove_all(dir, ec);
  if (ec || removed != 3 /* dir + 2 files */ || fs::exists(dir)) {
    std::cout << "M55-IOSTREAM: fail remove_all\n";
    return 1;
  }

  std::cout << "M55-IOSTREAM: ok cout\n";
  std::cout << "M55-IOSTREAM: ok sstream\n";
  std::cout << "M55-IOSTREAM: ok cin\n";
  std::cout << "M55-IOSTREAM: ok filesystem\n";
  std::cout.flush();
  return 0;
}

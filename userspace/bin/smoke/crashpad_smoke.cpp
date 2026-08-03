/* Crashpad end-to-end smoke: upstream Crashpad, unmodified, running on b1nix.
 *
 * The test forks a child that starts the real /bin/crashpad_handler through
 * crashpad::CrashpadClient and then dereferences a bad pointer. The handler is
 * a separate process: it attaches to the crashing child, walks its threads and
 * memory, and writes a minidump into the report database. The parent then
 * verifies a minidump really landed there — a file that starts with the
 * "MDMP" signature and is large enough to hold the crash it describes.
 *
 * This exercises the whole M80 kernel surface at once, through code b1nix
 * did not write: PR_SET_PTRACER, the AF_UNIX handshake with SO_PEERCRED,
 * PTRACE_ATTACH/GETREGSET/GETSIGINFO, /proc/<pid>/{task,auxv,maps,mem} and
 * process_vm_readv.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "client/crashpad_client.h"
#include "base/files/file_path.h"

namespace {

const char kDbDir[] = "/tmp/crashpad-db";
const char kHandler[] = "/bin/crashpad_handler";

void marker(const char* s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
}

void ok(const char* name) {
  char line[128];
  snprintf(line, sizeof(line), "CRASHPAD-SMOKE: ok %s", name);
  marker(line);
}

void fail(const char* name, long v) {
  char line[160];
  snprintf(line, sizeof(line), "CRASHPAD-SMOKE: FAIL %s (%ld, errno=%d)", name,
           v, errno);
  marker(line);
}

/* Look for a minidump anywhere under the database directory. Crashpad files
 * new reports under "pending"/"new" depending on version, so walk both. */
bool find_minidump(char* out, size_t out_len, long* out_size) {
  static const char* subdirs[] = {"pending", "new", "completed", ""};
  for (const char** sd = subdirs; *sd; ++sd) {
    char dirpath[256];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", kDbDir, *sd);
    DIR* d = opendir(dirpath);
    if (!d)
      continue;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
      if (e->d_name[0] == '.')
        continue;
      size_t n = strlen(e->d_name);
      if (n < 4 || strcmp(e->d_name + n - 4, ".dmp") != 0)
        continue;
      char path[512];
      snprintf(path, sizeof(path), "%s/%s", dirpath, e->d_name);
      int fd = open(path, O_RDONLY);
      if (fd < 0)
        continue;
      char sig[4] = {0, 0, 0, 0};
      long got = read(fd, sig, sizeof(sig));
      off_t size = lseek(fd, 0, SEEK_END);
      close(fd);
      if (got == 4 && memcmp(sig, "MDMP", 4) == 0) {
        snprintf(out, out_len, "%s", path);
        *out_size = (long)size;
        closedir(d);
        return true;
      }
    }
    closedir(d);
  }
  return false;
}

}  // namespace

int main() {
  marker("CRASHPAD-SMOKE: start");

  if (access(kHandler, X_OK) != 0) {
    fail("handler-present", -1);
    marker("CRASHPAD-SMOKE: done");
    return 1;
  }
  ok("handler-present");

  mkdir(kDbDir, 0755);

  int sync_pipe[2];
  if (pipe(sync_pipe) != 0) {
    fail("start-handler", -1);
    marker("CRASHPAD-SMOKE: done");
    return 1;
  }

  pid_t child = fork();
  if (child == 0) {
    close(sync_pipe[0]);
    crashpad::CrashpadClient client;
    base::FilePath handler(kHandler);
    base::FilePath db(kDbDir);
    std::map<std::string, std::string> annotations;
    annotations["b1nix"] = "m80";
    std::vector<std::string> args;
    args.push_back("--no-rate-limit");
    bool started = client.StartHandler(handler, db, db, /*url=*/"", annotations,
                                       args, /*restartable=*/false,
                                       /*asynchronous_start=*/false);
    long v = started ? 1 : 0;
    write(sync_pipe[1], &v, sizeof(v));
    if (!started)
      _exit(2);
    /* Crash. The handler is now responsible for us. */
    *reinterpret_cast<volatile int*>(0xdead0000UL) = 1;
    _exit(3); /* unreachable */
  }
  if (child < 0) {
    fail("start-handler", child);
    marker("CRASHPAD-SMOKE: done");
    return 1;
  }
  close(sync_pipe[1]);

  long started = 0;
  bool got_started = (read(sync_pipe[0], &started, sizeof(started)) ==
                      (long)sizeof(started));
  close(sync_pipe[0]);
  if (!got_started || started != 1) {
    fail("start-handler", started);
    kill(child, SIGKILL);
    int st = 0;
    waitpid(child, &st, 0);
    marker("CRASHPAD-SMOKE: done");
    return 1;
  }
  ok("start-handler");

  int status = 0;
  pid_t w = waitpid(child, &status, 0);
  bool crashed = (w == child) && (WIFSIGNALED(status) || WIFEXITED(status));
  if (!crashed) {
    fail("client-crash", (long)w);
    marker("CRASHPAD-SMOKE: done");
    return 1;
  }
  ok("client-crash");

  /* The handler writes the dump after the client dies; give it a bounded
   * amount of time rather than assuming an ordering between the two. */
  char path[512] = {0};
  long size = 0;
  bool found = false;
  for (int i = 0; i < 300 && !found; i++) {
    found = find_minidump(path, sizeof(path), &size);
    if (!found)
      usleep(100000);
  }
  if (!found || size < 4096) {
    fail("minidump", size);
    marker("CRASHPAD-SMOKE: done");
    return 1;
  }
  {
    char line[256];
    snprintf(line, sizeof(line), "CRASHPAD-SMOKE: dump %s (%ld bytes)", path,
             size);
    marker(line);
  }
  ok("minidump");
  marker("CRASHPAD-SMOKE: done");
  return 0;
}

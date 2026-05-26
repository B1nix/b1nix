#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "syscall.h"

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

static void marker(const char *text) {
  write(1, text, strlen(text));
}

static int run_cmd(const char *path, char *const argv[]) {
  int pid = syscall(SYS_FORK);
  if (pid == 0) {
    execvp(path, argv);
    _exit(127);
  } else if (pid > 0) {
    int status = 0;
    int wr = syscall(SYS_WAITPID, pid, &status, 0);
    if (wr == pid) {
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      }
    }
    return -1;
  }
  return -1;
}

static int run_cmd_stderr_to_file(const char *path, char *const argv[],
                                  const char *stderr_path) {
  int pid = syscall(SYS_FORK);
  if (pid == 0) {
    close(2);
    int err_fd = open(stderr_path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (err_fd != 2) {
      _exit(127);
    }
    execvp(path, argv);
    _exit(127);
  } else if (pid > 0) {
    int status = 0;
    int wr = syscall(SYS_WAITPID, pid, &status, 0);
    if (wr == pid) {
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      }
    }
    return -1;
  }
  return -1;
}

int main(void) {
  marker("M25-SMOKE: start\n");

  // 1. Check if TCC is present
  int tcc_fd = open("/bin/tcc", O_RDONLY);
  if (tcc_fd < 0) {
    marker("M25-SMOKE: fail tcc-launch\n");
    return 1;
  }
  close(tcc_fd);
  marker("M25-SMOKE: ok tcc-launch\n");

  // 2. Write a small hello.c
  int hello_fd = open("/tmp/hello.c", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (hello_fd < 0) {
    marker("M25-SMOKE: fail write-hello\n");
    return 1;
  }
  const char *hello_src =
      "#include <stdio.h>\n"
      "int main(void) {\n"
      "  printf(\"M25-HELLO: hello from native tcc\\n\");\n"
      "  return 0;\n"
      "}\n";
  write(hello_fd, hello_src, strlen(hello_src));
  close(hello_fd);

  // 3. Compile hello.c using /bin/tcc
  char *tcc_hello_argv[] = {"tcc", "/tmp/hello.c", "-o", "/tmp/hello", NULL};
  int compile_rc = run_cmd("/bin/tcc", tcc_hello_argv);
  if (compile_rc != 0) {
    char errbuf[64];
    snprintf(errbuf, sizeof(errbuf), "M25-SMOKE: fail compile-hello (rc=%d)\n", compile_rc);
    marker(errbuf);
    return 1;
  }
  marker("M25-SMOKE: ok compile-hello\n");

  // 4. Run /tmp/hello
  char *hello_argv[] = {"hello", NULL};
  int run_rc = run_cmd("/tmp/hello", hello_argv);
  if (run_rc != 0) {
    marker("M25-SMOKE: fail run-hello\n");
    return 1;
  }
  marker("M25-SMOKE: ok run-hello\n");

  // 5. Compiling a simple utility: mini-echo
  int echo_fd = open("/tmp/mini-echo.c", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (echo_fd < 0) {
    marker("M25-SMOKE: fail write-utility\n");
    return 1;
  }
  const char *echo_src =
      "#include <stdio.h>\n"
      "#include <fcntl.h>\n"
      "#include <unistd.h>\n"
      "#include <string.h>\n"
      "int main(int argc, char **argv) {\n"
      "  int fd = open(\"/tmp/mini-echo.out\", O_CREAT | O_WRONLY | O_TRUNC, 0666);\n"
      "  if (fd < 0) return 2;\n"
      "  for (int i = 1; i < argc; i++) {\n"
      "    write(fd, argv[i], strlen(argv[i]));\n"
      "    if (i != argc - 1) write(fd, \" \", 1);\n"
      "  }\n"
      "  write(fd, \"\\n\", 1);\n"
      "  close(fd);\n"
      "  return 0;\n"
      "}\n";
  write(echo_fd, echo_src, strlen(echo_src));
  close(echo_fd);

  // Compile mini-echo
  char *tcc_echo_argv[] = {"tcc", "/tmp/mini-echo.c", "-o", "/tmp/mini-echo", NULL};
  int compile_echo_rc = run_cmd("/bin/tcc", tcc_echo_argv);
  if (compile_echo_rc != 0) {
    marker("M25-SMOKE: fail compile-utility\n");
    return 1;
  }

  // Run mini-echo
  char *echo_argv[] = {"mini-echo", "alpha", "beta", NULL};
  int run_echo_rc = run_cmd("/tmp/mini-echo", echo_argv);
  if (run_echo_rc != 0) {
    marker("M25-SMOKE: fail run-utility\n");
    return 1;
  }
  int out_fd = open("/tmp/mini-echo.out", O_RDONLY);
  if (out_fd < 0) {
    marker("M25-SMOKE: fail verify-utility-output\n");
    return 1;
  }
  char outbuf[64];
  ssize_t out_n = read(out_fd, outbuf, sizeof(outbuf) - 1);
  close(out_fd);
  if (out_n < 0) {
    marker("M25-SMOKE: fail verify-utility-output\n");
    return 1;
  }
  outbuf[out_n] = '\0';
  if (strcmp(outbuf, "alpha beta\n") != 0) {
    marker("M25-SMOKE: fail verify-utility-output\n");
    return 1;
  }
  marker("M25-SMOKE: ok compile-utility\n");

  // 6. argc/argv propagation in compiled program
  int argv_fd = open("/tmp/argv-check.c", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (argv_fd < 0) {
    marker("M25-SMOKE: fail write-argv-check\n");
    return 1;
  }
  const char *argv_src =
      "#include <fcntl.h>\n"
      "#include <unistd.h>\n"
      "#include <string.h>\n"
      "int main(int argc, char **argv) {\n"
      "  if (argc < 3) return 3;\n"
      "  int fd = open(\"/tmp/argv-check.out\", O_CREAT | O_WRONLY | O_TRUNC, 0666);\n"
      "  if (fd < 0) return 4;\n"
      "  write(fd, argv[1], strlen(argv[1]));\n"
      "  write(fd, \"|\", 1);\n"
      "  write(fd, argv[2], strlen(argv[2]));\n"
      "  write(fd, \"\\n\", 1);\n"
      "  close(fd);\n"
      "  return 0;\n"
      "}\n";
  write(argv_fd, argv_src, strlen(argv_src));
  close(argv_fd);

  char *tcc_argv_check_argv[] = {"tcc", "/tmp/argv-check.c", "-o",
                                 "/tmp/argv-check", NULL};
  if (run_cmd("/bin/tcc", tcc_argv_check_argv) != 0) {
    marker("M25-SMOKE: fail compile-argv-check\n");
    return 1;
  }
  char *argv_check_argv[] = {"argv-check", "one", "two", NULL};
  if (run_cmd("/tmp/argv-check", argv_check_argv) != 0) {
    marker("M25-SMOKE: fail run-argv-check\n");
    return 1;
  }
  int argv_out_fd = open("/tmp/argv-check.out", O_RDONLY);
  if (argv_out_fd < 0) {
    marker("M25-SMOKE: fail verify-argv-check\n");
    return 1;
  }
  char argv_outbuf[64];
  ssize_t argv_out_n = read(argv_out_fd, argv_outbuf, sizeof(argv_outbuf) - 1);
  close(argv_out_fd);
  if (argv_out_n < 0) {
    marker("M25-SMOKE: fail verify-argv-check\n");
    return 1;
  }
  argv_outbuf[argv_out_n] = '\0';
  if (strcmp(argv_outbuf, "one|two\n") != 0) {
    marker("M25-SMOKE: fail verify-argv-check\n");
    return 1;
  }
  marker("M25-SMOKE: ok argv-check\n");

  // 7. stderr behavior through redirection
  int stderr_fd = open("/tmp/stderr-check.c", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (stderr_fd < 0) {
    marker("M25-SMOKE: fail write-stderr-check\n");
    return 1;
  }
  const char *stderr_src =
      "#include <stdio.h>\n"
      "int main(void) {\n"
      "  fprintf(stderr, \"M25-STDERR: native tcc stderr path\\n\");\n"
      "  return 0;\n"
      "}\n";
  write(stderr_fd, stderr_src, strlen(stderr_src));
  close(stderr_fd);

  char *tcc_stderr_argv[] = {"tcc", "/tmp/stderr-check.c", "-o",
                             "/tmp/stderr-check", NULL};
  if (run_cmd("/bin/tcc", tcc_stderr_argv) != 0) {
    marker("M25-SMOKE: fail compile-stderr-check\n");
    return 1;
  }
  char *stderr_argv[] = {"stderr-check", NULL};
  if (run_cmd_stderr_to_file("/tmp/stderr-check", stderr_argv,
                             "/tmp/stderr-check.out") != 0) {
    marker("M25-SMOKE: fail run-stderr-check\n");
    return 1;
  }
  int stderr_out_fd = open("/tmp/stderr-check.out", O_RDONLY);
  if (stderr_out_fd < 0) {
    marker("M25-SMOKE: fail verify-stderr-check\n");
    return 1;
  }
  char stderr_outbuf[96];
  ssize_t stderr_out_n =
      read(stderr_out_fd, stderr_outbuf, sizeof(stderr_outbuf) - 1);
  close(stderr_out_fd);
  if (stderr_out_n < 0) {
    marker("M25-SMOKE: fail verify-stderr-check\n");
    return 1;
  }
  stderr_outbuf[stderr_out_n] = '\0';
  if (strcmp(stderr_outbuf, "M25-STDERR: native tcc stderr path\n") != 0) {
    marker("M25-SMOKE: fail verify-stderr-check\n");
    return 1;
  }
  marker("M25-SMOKE: ok stderr-check\n");

  // 8. non-zero exit propagation
  int exit_fd = open("/tmp/exit37.c", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (exit_fd < 0) {
    marker("M25-SMOKE: fail write-exit-check\n");
    return 1;
  }
  const char *exit_src =
      "int main(void) {\n"
      "  return 37;\n"
      "}\n";
  write(exit_fd, exit_src, strlen(exit_src));
  close(exit_fd);

  char *tcc_exit_argv[] = {"tcc", "/tmp/exit37.c", "-o", "/tmp/exit37", NULL};
  if (run_cmd("/bin/tcc", tcc_exit_argv) != 0) {
    marker("M25-SMOKE: fail compile-exit-check\n");
    return 1;
  }
  char *exit_argv[] = {"exit37", NULL};
  int exit_rc = run_cmd("/tmp/exit37", exit_argv);
  if (exit_rc != 37) {
    marker("M25-SMOKE: fail verify-exit-check\n");
    return 1;
  }
  marker("M25-SMOKE: ok exit-check\n");

  // 9. Floating point parsing and scaling tests
  int float_fd = open("/tmp/float-check.c", O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (float_fd < 0) {
    marker("M25-SMOKE: fail write-float-check\n");
    return 1;
  }
  const char *float_src =
      "#include <stdio.h>\n"
      "#include <stdlib.h>\n"
      "#include <math.h>\n"
      "#include <signal.h>\n"
      "#include <errno.h>\n"
      "int main(void) {\n"
      "  char *end;\n"
      "  double val;\n"
      "  val = strtod(\"  -123.456\", &end);\n"
      "  if (val != -123.456 || *end != '\\0') return 1;\n"
      "  val = strtod(\"1.5e3\", &end);\n"
      "  if (val != 1500.0 || *end != '\\0') return 2;\n"
      "  val = strtod(\"0x1.8p2\", &end);\n"
      "  if (val != 6.0 || *end != '\\0') return 3;\n"
      "  val = ldexp(1.25, 4);\n"
      "  if (val != 20.0) return 4;\n"
      "  sigset_t set;\n"
      "  if (sigemptyset(&set) != 0 || set != 0) return 5;\n"
      "  if (sigaddset(&set, SIGINT) != 0 || set != (1UL << (SIGINT - 1))) return 6;\n"
      "  if (sigismember(&set, SIGINT) != 1) return 7;\n"
      "  if (sigismember(&set, SIGSEGV) != 0) return 8;\n"
      "  if (sigfillset(&set) != 0 || set != ~0UL) return 9;\n"
      "  if (sigdelset(&set, SIGINT) != 0 || sigismember(&set, SIGINT) != 0) return 10;\n"
      "  if (sigismember(&set, SIGSEGV) != 1) return 11;\n"
      "  errno = 0;\n"
      "  if (sigaddset(&set, -1) != -1 || errno != EINVAL) return 12;\n"
      "  errno = 0;\n"
      "  if (sigaddset(&set, 64) != -1 || errno != EINVAL) return 13;\n"
      "  errno = 0;\n"
      "  if (sigismember(&set, 0) != -1 || errno != EINVAL) return 14;\n"
      "  printf(\"M25-FLOAT: all float tests passed\\n\");\n"
      "  return 0;\n"
      "}\n";
  write(float_fd, float_src, strlen(float_src));
  close(float_fd);

  char *tcc_float_argv[] = {"tcc", "/tmp/float-check.c", "-o", "/tmp/float-check", NULL};
  if (run_cmd("/bin/tcc", tcc_float_argv) != 0) {
    marker("M25-SMOKE: fail compile-float-check\n");
    return 1;
  }
  char *float_argv[] = {"float-check", NULL};
  if (run_cmd("/tmp/float-check", float_argv) != 0) {
    marker("M25-SMOKE: fail run-float-check\n");
    return 1;
  }
  marker("M25-SMOKE: ok float-check\n");

  marker("M25-SMOKE: done\n");
  return 0;
}

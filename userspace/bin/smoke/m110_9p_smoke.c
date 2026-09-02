/*
 * m110_9p_smoke — VirtIO-9P (9P2000.L) filesystem smoke test.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static void marker(const char *text) {
  write(1, text, strlen(text));
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  marker("M110-9P: start\n");

  mkdir("/mnt/9p", 0755);

  /* 1. Mount 9p filesystem */
  if (mount("hostshare", "/mnt/9p", "9p", 0, NULL) < 0) {
    char err[64];
    snprintf(err, sizeof(err), "M110-9P: fail mount (errno=%d)\n", errno);
    marker(err);
    return 1;
  }
  marker("M110-9P: ok mount\n");

  /* 2. Read file created by host */
  int fd = open("/mnt/9p/hello_from_host.txt", O_RDONLY);
  if (fd >= 0) {
    char buf[128];
    memset(buf, 0, sizeof(buf));
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0 && strstr(buf, "Hello from Host")) {
      marker("M110-9P: ok read-host-file\n");
    } else {
      marker("M110-9P: fail read-host-file-content\n");
    }
  } else {
    marker("M110-9P: fail open-host-file\n");
  }

  /* 3. Write file from guest */
  int out_fd = open("/mnt/9p/guest_output.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
  if (out_fd >= 0) {
    const char *msg = "Written by b1nix via VirtIO-9P!\n";
    write(out_fd, msg, strlen(msg));
    fsync(out_fd);
    close(out_fd);
    marker("M110-9P: ok write-guest-file\n");
  } else {
    marker("M110-9P: fail write-guest-file\n");
  }

  /* 4. Directory enumeration */
  DIR *d = opendir("/mnt/9p");
  if (d) {
    int found_guest_file = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (strcmp(de->d_name, "guest_output.txt") == 0) {
        found_guest_file = 1;
      }
    }
    closedir(d);
    if (found_guest_file) {
      marker("M110-9P: ok readdir\n");
    } else {
      marker("M110-9P: fail readdir-find-file\n");
    }
  } else {
    marker("M110-9P: fail opendir\n");
  }

  marker("M110-9P: done\n");
  return 0;
}

/*
 * m14_smoke — storage, ext4, swap, block-cache, persistence tests.
 * Rewritten to use POSIX API (no b1nix raw syscalls).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>

static void marker(const char *text) {
  write(1, text, strlen(text));
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  marker("M14-SMOKE: start\n");

  /* 1. Test invalid device mount */
  if (mount("nosuchdevice", "/mnt/ext3", "ext2", 0, NULL) < 0) {
    marker("M14-SMOKE: ok invalid-device\n");
  } else {
    marker("M14-SMOKE: fail invalid-device\n");
  }

  /* 2. Test invalid filesystem type mount */
  if (mount("sata0", "/mnt/ext3", "invalid_fs_type", 0, NULL) < 0) {
    marker("M14-SMOKE: ok invalid-fs\n");
  } else {
    marker("M14-SMOKE: fail invalid-fs\n");
  }

  /* 3. Mount sata0 as ext4 (primary) */
  if (mount("sata0", "/mnt/ext4", "ext4", 0, NULL) == 0) {
    marker("M14-SMOKE: ok mount-ext4-sata\n");
  } else {
    char err[64];
    snprintf(err, sizeof(err), "M14-SMOKE: fail mount-ext4-sata (rc=%d)\n",
             errno);
    marker(err);
  }

  /* 4. Mount nvme0 as ext4 and test read/write persistence */
  if (mount("nvme0", "/mnt/ext4nvme", "ext4", 0, NULL) == 0) {
    int fd_ext4 = open("/mnt/ext4nvme/persist_ext4.txt",
                       O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd_ext4 >= 0) {
      const char *ext4_msg = "B1NIX ext4 persistent validation";
      int wr = write(fd_ext4, ext4_msg, strlen(ext4_msg));
      int f_rc = fsync(fd_ext4);
      sync();
      close(fd_ext4);

      int um_ok = (umount("/mnt/ext4nvme") == 0);
      int m_ok =
          (mount("nvme0", "/mnt/ext4nvme", "ext4", 0, NULL) == 0);

      int fd_read = open("/mnt/ext4nvme/persist_ext4.txt", O_RDONLY);
      char buf[64];
      memset(buf, 0, sizeof(buf));
      int rd = -1;
      if (fd_read >= 0) {
        rd = read(fd_read, buf, sizeof(buf) - 1);
        close(fd_read);
      }

      if (wr == (int)strlen(ext4_msg) && f_rc == 0 && um_ok && m_ok &&
          rd == (int)strlen(ext4_msg) && strcmp(buf, ext4_msg) == 0) {
        marker("M14-SMOKE: ok mount-ext4-nvme\n");
        marker("M14-SMOKE: ok ext4-persistence\n");
      } else {
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "M14-SMOKE: fail ext4-persistence wr=%d frc=%d um=%d m=%d "
                 "rd=%d got=%s\n",
                 wr, f_rc, um_ok, m_ok, rd, buf[0] ? buf : "(empty)");
        marker(dbg);
      }
    } else {
      marker("M14-SMOKE: fail ext4nvme open\n");
    }
  } else {
    char err[64];
    snprintf(err, sizeof(err), "M14-SMOKE: fail mount-ext4-nvme (rc=%d)\n",
             errno);
    marker(err);
  }

  /* 5. Test Block Cache */
  {
    int fd_cache =
        open("/mnt/ext4/cache_test.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd_cache >= 0) {
      const char *test_data = "cached_block_data_validation";
      int wr = write(fd_cache, test_data, strlen(test_data));
      int f_rc = fsync(fd_cache);
      sync();
      close(fd_cache);

      int um_ok = (umount("/mnt/ext4") == 0);
      int m_ok = (mount("sata0", "/mnt/ext4", "ext4", 0, NULL) == 0);

      int fd_read = open("/mnt/ext4/cache_test.txt", O_RDONLY);
      char buf[64];
      memset(buf, 0, sizeof(buf));
      int rd = -1;
      if (fd_read >= 0) {
        rd = read(fd_read, buf, sizeof(buf) - 1);
        close(fd_read);
      }

      if (wr == (int)strlen(test_data) && f_rc == 0 && um_ok && m_ok &&
          rd == (int)strlen(test_data) && strcmp(buf, test_data) == 0) {
        marker("M14-SMOKE: ok block-cache\n");
      } else {
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "M14-SMOKE: fail block-cache wr=%d frc=%d um=%d m=%d rd=%d "
                 "got=%s\n",
                 wr, f_rc, um_ok, m_ok, rd, buf[0] ? buf : "(empty)");
        marker(dbg);
      }
    } else {
      marker("M14-SMOKE: fail block-cache open\n");
    }
  }

  /* 6. Test Persistence */
  {
    int fd_persist =
        open("/mnt/ext4/persist.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd_persist >= 0) {
      const char *persist_msg = "B1NIX persistent block storage validation";
      int wr = write(fd_persist, persist_msg, strlen(persist_msg));
      int f_rc = fsync(fd_persist);
      sync();
      close(fd_persist);

      int um_ok = (umount("/mnt/ext4") == 0);
      int m_ok = (mount("sata0", "/mnt/ext4", "ext4", 0, NULL) == 0);

      int fd_read = open("/mnt/ext4/persist.txt", O_RDONLY);
      char buf[128];
      memset(buf, 0, sizeof(buf));
      int rd = -1;
      if (fd_read >= 0) {
        rd = read(fd_read, buf, sizeof(buf) - 1);
        close(fd_read);
      }

      if (wr == (int)strlen(persist_msg) && f_rc == 0 && um_ok && m_ok &&
          rd == (int)strlen(persist_msg) && strcmp(buf, persist_msg) == 0) {
        marker("M14-SMOKE: ok persistence\n");
      } else {
        char dbg[256];
        snprintf(dbg, sizeof(dbg),
                 "M14-SMOKE: fail persistence wr=%d frc=%d um=%d m=%d rd=%d "
                 "got=%s\n",
                 wr, f_rc, um_ok, m_ok, rd, buf[0] ? buf : "(empty)");
        marker(dbg);
      }
    } else {
      marker("M14-SMOKE: fail persistence open\n");
    }
  }

  /* 7. Stress loop */
  {
    int stress_ok = 1;
    for (int i = 0; i < 10; i++) {
      char path[64];
      snprintf(path, sizeof(path), "/mnt/ext4/stress_%d.txt", i);
      int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0666);
      if (fd < 0) {
        stress_ok = 0;
        break;
      }
      char data[32];
      snprintf(data, sizeof(data), "stress_data_%d", i);
      if (write(fd, data, strlen(data)) != (int)strlen(data)) {
        close(fd);
        stress_ok = 0;
        break;
      }
      lseek(fd, 0, SEEK_SET);
      char r_buf[32];
      memset(r_buf, 0, sizeof(r_buf));
      if (read(fd, r_buf, strlen(data)) != (int)strlen(data) ||
          strcmp(r_buf, data) != 0) {
        close(fd);
        stress_ok = 0;
        break;
      }
      close(fd);
      if (unlink(path) < 0) {
        stress_ok = 0;
        break;
      }
    }
    marker(stress_ok ? "M14-SMOKE: ok stress-loop\n"
                     : "M14-SMOKE: fail stress-loop\n");
  }

  /* 8. Large file boundary */
  {
    int fd_large =
        open("/mnt/ext4/large_file.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd_large >= 0) {
      char *chunk_data = malloc(4096);
      memset(chunk_data, 'A', 4096);
      int write_ok = 1;
      for (int i = 0; i < 32; i++) {
        if (write(fd_large, chunk_data, 4096) != 4096) {
          write_ok = 0;
          break;
        }
      }
      fsync(fd_large);
      close(fd_large);
      free(chunk_data);

      struct stat st;
      int stat_rc = stat("/mnt/ext4/large_file.txt", &st);

      int fd_read_large = open("/mnt/ext4/large_file.txt", O_RDONLY);
      int read_ok = 1;
      if (fd_read_large >= 0) {
        char *read_chunk = malloc(4096);
        for (int i = 0; i < 32; i++) {
          int r = read(fd_read_large, read_chunk, 4096);
          if (r != 4096) {
            read_ok = 0;
            break;
          }
          for (int j = 0; j < 4096; j++) {
            if (read_chunk[j] != 'A') {
              read_ok = 0;
              break;
            }
          }
          if (!read_ok) break;
        }
        close(fd_read_large);
        free(read_chunk);
      } else {
        read_ok = 0;
      }

      if (write_ok && stat_rc == 0 && st.st_size == 128 * 1024 && read_ok) {
        marker("M14-SMOKE: ok large-file\n");
      } else {
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "M14-SMOKE: fail large-file wok=%d s_rc=%d sz=%d rok=%d\n",
                 write_ok, stat_rc, (int)st.st_size, read_ok);
        marker(dbg);
      }
    } else {
      marker("M14-SMOKE: fail large-file open\n");
    }
  }

  /* 9. VFS Path Normalization */
  {
    int fd_norm =
        open("/mnt/ext4/../ext4/./large_file.txt", O_RDONLY);
    if (fd_norm >= 0) {
      struct stat st;
      int stat_rc = fstat(fd_norm, &st);
      close(fd_norm);
      if (stat_rc == 0 && st.st_size == 128 * 1024) {
        marker("M14-SMOKE: ok VFS-normalization\n");
      } else {
        marker("M14-SMOKE: fail VFS-normalization\n");
      }
    } else {
      marker("M14-SMOKE: fail VFS-normalization open\n");
    }
  }

  /* 10. M72: mmap-store durability across page-cache reclaim */
  {
    const char *dp = "/mnt/ext4/mmap_dur.txt";
    int dfd = open(dp, O_CREAT | O_RDWR | O_TRUNC, 0666);
    int ok = 0;
    if (dfd >= 0) {
      char init[4096];
      memset(init, 'a', sizeof(init));
      if (write(dfd, init, sizeof(init)) == 4096) {
        fsync(dfd);
        int sf0 = open("/sys/kernel/mm/drop_caches", O_WRONLY);
        if (sf0 >= 0) {
          write(sf0, "1\n", 2);
          close(sf0);
        }
        void *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
        if (m != MAP_FAILED) {
          memcpy(m, "DURABLE-MMAP-DATA", 17);
          munmap(m, 4096);
          int sf = open("/sys/kernel/mm/drop_caches", O_WRONLY);
          if (sf >= 0) {
            write(sf, "1\n", 2);
            close(sf);
          }
          int rfd = open(dp, O_RDONLY);
          if (rfd >= 0) {
            char rb[17];
            if (read(rfd, rb, 17) == 17 &&
                memcmp(rb, "DURABLE-MMAP-DATA", 17) == 0)
              ok = 1;
            close(rfd);
          }
        }
      }
      close(dfd);
    }
    marker(ok ? "M14-SMOKE: ok mmap-durable\n"
              : "M14-SMOKE: fail mmap-durable\n");
    unlink(dp);
  }

  /* Clean up */
  unlink("/mnt/ext4/large_file.txt");
  unlink("/mnt/ext4/persist.txt");
  unlink("/mnt/ext4/cache_test.txt");
  unlink("/mnt/ext4nvme/persist_ext4.txt");

  umount("/mnt/ext4");
  umount("/mnt/ext4nvme");

  marker("M14-SMOKE: done\n");
  return 0;
}

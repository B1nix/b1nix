/*
 * m14_smoke — storage, ext4, swap, block-cache, persistence tests.
 * Rewritten to use POSIX API (no b1nix raw syscalls).
 */
#include <dirent.h>
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
  if (mount("sda", "/mnt/ext3", "invalid_fs_type", 0, NULL) < 0) {
    marker("M14-SMOKE: ok invalid-fs\n");
  } else {
    marker("M14-SMOKE: fail invalid-fs\n");
  }

  /* 3. Mount sda as ext4 (primary) */
  if (mount("sda", "/mnt/ext4", "ext4", 0, NULL) == 0) {
    marker("M14-SMOKE: ok mount-ext4-sata\n");
  } else {
    char err[64];
    snprintf(err, sizeof(err), "M14-SMOKE: fail mount-ext4-sata (rc=%d)\n",
             errno);
    marker(err);
  }

  /* 4. Mount nvme0n1 as ext4 and test read/write persistence */
  if (mount("nvme0n1", "/mnt/ext4nvme", "ext4", 0, NULL) == 0) {
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
          (mount("nvme0n1", "/mnt/ext4nvme", "ext4", 0, NULL) == 0);

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

    /* 4b. A FIFO is a real on-disk inode (S_IFIFO, no data blocks), so it must
     * survive umount/mount like any other name — that is what lets an init
     * system keep its control FIFO on the root filesystem instead of a
     * RAM-only directory. */
    {
      const char *fifo = "/mnt/ext4nvme/ctl.fifo";
      unlink(fifo);
      struct stat before, after;
      int mk = mkfifo(fifo, 0600);
      int st1 = stat(fifo, &before);
      sync();
      int um_ok = (umount("/mnt/ext4nvme") == 0);
      int m_ok = (mount("nvme0n1", "/mnt/ext4nvme", "ext4", 0, NULL) == 0);
      int st2 = stat(fifo, &after);
      if (mk == 0 && st1 == 0 && S_ISFIFO(before.st_mode) && um_ok && m_ok &&
          st2 == 0 && S_ISFIFO(after.st_mode)) {
        marker("M14-SMOKE: ok ext4-fifo-persistence\n");
      } else {
        char dbg[128];
        snprintf(dbg, sizeof(dbg),
                 "M14-SMOKE: fail ext4-fifo-persistence mk=%d st1=%d um=%d "
                 "m=%d st2=%d mode=%o\n",
                 mk, st1, um_ok, m_ok, st2, (unsigned)after.st_mode);
        marker(dbg);
      }
      unlink(fifo);
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
      int m_ok = (mount("sda", "/mnt/ext4", "ext4", 0, NULL) == 0);

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
      int m_ok = (mount("sda", "/mnt/ext4", "ext4", 0, NULL) == 0);

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

  /* Removable media are identified by what they are, not by what they are
   * called: USB mass storage is a SCSI disk and so shares the sd* sequence with
   * AHCI, which means the name can no longer tell the two apart and
   * /sys/block/<disk>/removable has to carry the fact instead.
   *
   * Only emitted where a removable disk is actually attached (the blk instance
   * has a USB stick behind xHCI); the other instances have none and stay quiet
   * rather than reporting a pass they did not earn. */
  {
    DIR *sysblk = opendir("/sys/block");

    if (sysblk) {
      char removable_name[64];
      int removable_count = 0, fixed_count = 0, read_failures = 0;
      struct dirent *ent;

      removable_name[0] = '\0';
      while ((ent = readdir(sysblk)) != NULL) {
        char path[128];
        char value[8];
        int vfd, n;

        if (ent->d_name[0] == '.')
          continue;
        snprintf(path, sizeof(path), "/sys/block/%s/removable", ent->d_name);
        vfd = open(path, O_RDONLY);
        if (vfd < 0) {
          read_failures++;
          continue;
        }
        n = (int)read(vfd, value, sizeof(value) - 1);
        close(vfd);
        if (n <= 0) {
          read_failures++;
          continue;
        }
        value[n] = '\0';
        if (value[0] == '1') {
          removable_count++;
          snprintf(removable_name, sizeof(removable_name), "%s", ent->d_name);
        } else if (value[0] == '0') {
          fixed_count++;
        } else {
          read_failures++;
        }
      }
      closedir(sysblk);

      if (removable_count > 0) {
        /* The stick must be the only removable disk, must sit beside fixed
         * disks that say so, and must be named sd* — i.e. it joined the SCSI
         * sequence rather than getting a bus-specific name of its own. */
        int ok = removable_count == 1 && fixed_count > 0 && read_failures == 0 &&
                 removable_name[0] == 's' && removable_name[1] == 'd' &&
                 removable_name[2] != '\0';

        if (ok) {
          marker("M14-SMOKE: ok removable-is-a-fact\n");
        } else {
          char err[128];
          snprintf(err, sizeof(err),
                   "M14-SMOKE: fail removable-is-a-fact name=%s rm=%d fixed=%d bad=%d\n",
                   removable_name, removable_count, fixed_count, read_failures);
          marker(err);
        }
      }
    }
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

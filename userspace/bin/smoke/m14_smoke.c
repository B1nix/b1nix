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


/* The file an object-writing compiler leaves behind, generated from its name:
 * a size between 40 and 110 KiB and bytes no other (round, file) pair shares. */
static unsigned churn_size(int round, int i) {
  return 40960u + (unsigned)((round * 7919 + i * 104729) % 71680);
}

static unsigned char churn_byte(int round, int i, unsigned off) {
  unsigned x = (unsigned)round * 2654435761u ^ (unsigned)i * 40503u ^ off * 2246822519u;
  x ^= x >> 13;
  x *= 3266489917u;
  return (unsigned char)(x ^ (x >> 16));
}

static int churn_verify(int round, int i, const char *path) {
  static unsigned char buf[120000];
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  unsigned want = churn_size(round, i), got = 0;
  for (;;) {
    ssize_t r = read(fd, buf + got, sizeof(buf) - got);
    if (r <= 0)
      break;
    got += (unsigned)r;
  }
  close(fd);
  if (got != want)
    return 0;
  for (unsigned o = 0; o < want; o++)
    if (buf[o] != churn_byte(round, i, o))
      return 0;
  return 1;
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
      char seen[256];
      int seen_len = 0;
      int removable_count = 0, fixed_count = 0, read_failures = 0;
      struct dirent *ent;

      removable_name[0] = '\0';
      seen[0] = '\0';
      while ((ent = readdir(sysblk)) != NULL) {
        char path[128];
        char value[8];
        int vfd, n;

        if (ent->d_name[0] == '.')
          continue;
        if (seen_len < (int)sizeof(seen) - 1)
          seen_len += snprintf(seen + seen_len, sizeof(seen) - (size_t)seen_len,
                               "%s%s", seen_len ? "," : "", ent->d_name);
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

      /* Always report the scan itself. The pass marker below is conditional on
       * a removable disk being present, so without this line an instance that
       * sees none is indistinguishable from one where /sys/block could not be
       * read at all. */
      {
        char scan[384];
        snprintf(scan, sizeof(scan),
                 "M14-SMOKE: removable-scan rm=%d fixed=%d bad=%d seen=%s\n",
                 removable_count, fixed_count, read_failures, seen);
        marker(scan);
      }

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

  /* 12. A linker's output: create, ftruncate to size, fill through a shared
   * mapping, unmap. Half-way through, sync -- a writeback that cleans the
   * pages while the mapping is still being written, and rewrites a page that
   * was already cleaned. What is on the disk after a remount must be every
   * byte that was stored, not the zeros of the truncate. */
  {
    enum { MAPSZ = 512 * 1024 };
    int ok = 0;
    long bad_at = -1, got_total = -1;
    int bad_val = -1, want_val = -1;
    int fd = open("/mnt/ext4/linker-out.elf", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0 && ftruncate(fd, MAPSZ) == 0) {
      unsigned char *m = mmap(0, MAPSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (m != MAP_FAILED) {
        for (unsigned o2 = 0; o2 < MAPSZ / 2; o2++)
          m[o2] = churn_byte(99, 1, o2);
        sync();
        for (unsigned o2 = MAPSZ / 2; o2 < MAPSZ; o2++)
          m[o2] = churn_byte(99, 1, o2);
        for (unsigned o2 = 0; o2 < 4096; o2++)   /* a cleaned page, stored again */
          m[o2] = churn_byte(99, 2, o2);
        munmap(m, MAPSZ);
        close(fd);
        fd = -1;
        sync();
        if (umount("/mnt/ext4") == 0 && mount("sda", "/mnt/ext4", "ext4", 0, NULL) == 0) {
          static unsigned char back[MAPSZ];
          int rfd = open("/mnt/ext4/linker-out.elf", O_RDONLY);
          ssize_t got = 0;
          while (rfd >= 0 && got < MAPSZ) {
            ssize_t r = read(rfd, back + got, (size_t)(MAPSZ - got));
            if (r <= 0)
              break;
            got += r;
          }
          if (rfd >= 0)
            close(rfd);
          got_total = (long)got;
          ok = (got == MAPSZ);
          for (unsigned o2 = 0; ok && o2 < MAPSZ; o2++)
            if (back[o2] != churn_byte(99, o2 < 4096 ? 2 : 1, o2)) {
              ok = 0;
              bad_at = (long)o2;
              bad_val = back[o2];
              want_val = churn_byte(99, o2 < 4096 ? 2 : 1, o2);
            }
        }
      }
    }
    if (fd >= 0)
      close(fd);
    unlink("/mnt/ext4/linker-out.elf");
    if (ok) {
      marker("M14-SMOKE: ok ext4-shared-mmap-durable\n");
    } else {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "M14-SMOKE: fail ext4-shared-mmap-durable read=%ld bad_at=%ld got=%d want=%d\n",
               got_total, bad_at, bad_val, want_val);
      marker(msg);
    }
  }

  /* 11. Object-file churn: what a compiler does to a build directory. Each
   * file is written in uneven chunks to a temporary name and renamed over the
   * previous version, so freed inodes are reused while their old pages may
   * still be cached. Checked through the cache after every round, and from
   * the disk after a remount. The in-guest kernel self-host read another
   * object's bytes, and found a truncated one on disk, in exactly this pattern. */
  {
    enum { ROUNDS = 6, FILES = 30 };
    static unsigned char wbuf[120000];
    int cache_bad = -1, disk_bad = -1;
    mkdir("/mnt/ext4/churn", 0755);
    for (int round = 0; round < ROUNDS && cache_bad < 0; round++) {
      /* Two files at a time, their chunks interleaved, so their blocks
       * alternate on disk: each ends up with more extents than fit in the
       * inode, and the extent tree has to grow an index block. */
      for (int i = 0; i < FILES; i += 2) {
        static unsigned char wbuf2[120000];
        unsigned char *bufs[2] = {wbuf, wbuf2};
        unsigned char heads[2][64];
        char tmps[2][64], fins[2][64];
        unsigned sizes[2], offs[2] = {0, 0};
        int fds[2];
        for (int k = 0; k < 2; k++) {
          snprintf(tmps[k], sizeof(tmps[k]), "/mnt/ext4/churn/f%d.o-tmp", i + k);
          snprintf(fins[k], sizeof(fins[k]), "/mnt/ext4/churn/f%d.o", i + k);
          sizes[k] = churn_size(round, i + k);
          for (unsigned o2 = 0; o2 < sizes[k]; o2++)
            bufs[k][o2] = churn_byte(round, i + k, o2);
          /* An object writer puts a placeholder header down, writes the
           * sections, then goes back and fills the header in. */
          memcpy(heads[k], bufs[k], sizeof(heads[k]));
          memset(bufs[k], 0, sizeof(heads[k]));
          fds[k] = open(tmps[k], O_CREAT | O_WRONLY | O_TRUNC, 0644);
        }
        unsigned step = 4096 + 512;
        while (offs[0] < sizes[0] || offs[1] < sizes[1]) {
          for (int k = 0; k < 2; k++) {
            if (fds[k] < 0 || offs[k] >= sizes[k])
              continue;
            unsigned len = sizes[k] - offs[k] < step ? sizes[k] - offs[k] : step;
            if (write(fds[k], bufs[k] + offs[k], len) != (ssize_t)len)
              offs[k] = sizes[k];
            else
              offs[k] += len;
          }
        }
        for (int k = 0; k < 2; k++) {
          if (fds[k] < 0)
            continue;
          (void)!pwrite(fds[k], heads[k], sizeof(heads[k]), 0);
          close(fds[k]);
          rename(tmps[k], fins[k]);
        }
      }
      for (int i = 0; i < FILES; i++) {
        char fin[64];
        snprintf(fin, sizeof(fin), "/mnt/ext4/churn/f%d.o", i);
        if (!churn_verify(round, i, fin)) {
          cache_bad = round * 100 + i;
          break;
        }
      }
    }
    if (cache_bad < 0) {
      sync();
      int um_ok = (umount("/mnt/ext4") == 0);
      int m_ok = (mount("sda", "/mnt/ext4", "ext4", 0, NULL) == 0);
      for (int i = 0; i < FILES && um_ok && m_ok; i++) {
        char fin[64];
        snprintf(fin, sizeof(fin), "/mnt/ext4/churn/f%d.o", i);
        if (!churn_verify(ROUNDS - 1, i, fin)) {
          disk_bad = i;
          break;
        }
      }
      if (!um_ok || !m_ok)
        disk_bad = 1000;
    }
    char msg[128];
    if (cache_bad < 0)
      marker("M14-SMOKE: ok ext4-churn-cache\n");
    else {
      snprintf(msg, sizeof(msg), "M14-SMOKE: fail ext4-churn-cache round=%d file=%d\n",
               cache_bad / 100, cache_bad % 100);
      marker(msg);
    }
    if (cache_bad < 0 && disk_bad < 0)
      marker("M14-SMOKE: ok ext4-churn-disk\n");
    else if (cache_bad < 0) {
      snprintf(msg, sizeof(msg), "M14-SMOKE: fail ext4-churn-disk file=%d\n", disk_bad);
      marker(msg);
    }
    for (int i = 0; i < FILES; i++) {
      char fin[64];
      snprintf(fin, sizeof(fin), "/mnt/ext4/churn/f%d.o", i);
      unlink(fin);
    }
    rmdir("/mnt/ext4/churn");
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

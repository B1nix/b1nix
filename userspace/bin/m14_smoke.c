#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "syscall.h"
#include "types.h"

static void marker(const char *text) {
  write(1, text, strlen(text));
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  marker("M14-SMOKE: start\n");

  /* 1. Test invalid device mount */
  isize rc_nodev = syscall(SYS_MOUNT, "nosuchdevice", "/ext3", "ext2", 0);
  if (rc_nodev < 0) {
    marker("M14-SMOKE: ok invalid-device\n");
  } else {
    marker("M14-SMOKE: fail invalid-device\n");
  }

  /* 2. Test invalid filesystem type mount */
  isize rc_badfs = syscall(SYS_MOUNT, "sata0", "/ext3", "invalid_fs_type", 0);
  if (rc_badfs < 0) {
    marker("M14-SMOKE: ok invalid-fs\n");
  } else {
    marker("M14-SMOKE: fail invalid-fs\n");
  }

  /* 3. Mount sata0 as ext3 */
  isize rc_ext3 = syscall(SYS_MOUNT, "sata0", "/ext3", "ext3", 0);
  if (rc_ext3 == 0) {
    marker("M14-SMOKE: ok mount-ext3\n");
  } else {
    char err[64];
    snprintf(err, sizeof(err), "M14-SMOKE: fail mount-ext3 (rc=%d)\n", (int)rc_ext3);
    marker(err);
  }

  /* 4. Mount nvme0 as ext4 and test read/write persistence */
  isize rc_ext4 = syscall(SYS_MOUNT, "nvme0", "/ext4", "ext4", 0);
  if (rc_ext4 == 0) {
    int fd_ext4 = open("/ext4/persist_ext4.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd_ext4 >= 0) {
      const char *ext4_msg = "B1NIX ext4 persistent validation";
      int wr = write(fd_ext4, ext4_msg, strlen(ext4_msg));
      int f_rc = syscall(SYS_FSYNC, fd_ext4);
      syscall(SYS_SYNC);
      close(fd_ext4);

      isize um_rc = syscall(SYS_UMOUNT, "/ext4");
      isize m_rc = syscall(SYS_MOUNT, "nvme0", "/ext4", "ext4", 0);

      int fd_read = open("/ext4/persist_ext4.txt", O_RDONLY);
      char buf[64];
      memset(buf, 0, sizeof(buf));
      int rd = -1;
      if (fd_read >= 0) {
        rd = read(fd_read, buf, sizeof(buf) - 1);
        close(fd_read);
      }

      if (wr == (int)strlen(ext4_msg) && f_rc == 0 && um_rc == 0 && m_rc == 0 &&
          rd == (int)strlen(ext4_msg) && strcmp(buf, ext4_msg) == 0) {
        marker("M14-SMOKE: ok mount-ext4\n");
        marker("M14-SMOKE: ok ext4-persistence\n");
      } else {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "M14-SMOKE: fail ext4-persistence wr=%d frc=%d um=%d m=%d rd=%d got=%s\n",
                 wr, (int)f_rc, (int)um_rc, (int)m_rc, rd, buf[0] ? buf : "(empty)");
        marker(dbg);
      }
    } else {
      marker("M14-SMOKE: fail ext4 open\n");
    }
  } else {
    char err[64];
    snprintf(err, sizeof(err), "M14-SMOKE: fail mount-ext4 (rc=%d)\n", (int)rc_ext4);
    marker(err);
  }

  /* 5. Test Block Cache (Cached read and verify through flush & remount) */
  int fd_cache = open("/ext3/cache_test.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd_cache >= 0) {
    const char *test_data = "cached_block_data_validation";
    int wr = write(fd_cache, test_data, strlen(test_data));
    int f_rc = syscall(SYS_FSYNC, fd_cache);
    syscall(SYS_SYNC);
    close(fd_cache);

    isize um_rc = syscall(SYS_UMOUNT, "/ext3");
    isize m_rc = syscall(SYS_MOUNT, "sata0", "/ext3", "ext3", 0);

    int fd_read = open("/ext3/cache_test.txt", O_RDONLY);
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int rd = -1;
    if (fd_read >= 0) {
      rd = read(fd_read, buf, sizeof(buf) - 1);
      close(fd_read);
    }

    if (wr == (int)strlen(test_data) && f_rc == 0 && um_rc == 0 && m_rc == 0 &&
        rd == (int)strlen(test_data) && strcmp(buf, test_data) == 0) {
      marker("M14-SMOKE: ok block-cache\n");
    } else {
      char dbg[128];
      snprintf(dbg, sizeof(dbg), "M14-SMOKE: fail block-cache wr=%d frc=%d um=%d m=%d rd=%d got=%s\n",
               wr, (int)f_rc, (int)um_rc, (int)m_rc, rd, buf[0] ? buf : "(empty)");
      marker(dbg);
    }
  } else {
    marker("M14-SMOKE: fail block-cache open\n");
  }

  /* 6. Test Persistence (sync, umount, remount, read back) */
  int fd_persist = open("/ext3/persist.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd_persist >= 0) {
    const char *persist_msg = "B1NIX persistent block storage validation";
    int wr = write(fd_persist, persist_msg, strlen(persist_msg));
    int f_rc = syscall(SYS_FSYNC, fd_persist);
    syscall(SYS_SYNC);
    close(fd_persist);

    isize um_rc = syscall(SYS_UMOUNT, "/ext3");
    isize m_rc = syscall(SYS_MOUNT, "sata0", "/ext3", "ext3", 0);

    int fd_read = open("/ext3/persist.txt", O_RDONLY);
    char buf[128];
    memset(buf, 0, sizeof(buf));
    int rd = -1;
    if (fd_read >= 0) {
      rd = read(fd_read, buf, sizeof(buf) - 1);
      close(fd_read);
    }

    if (wr == (int)strlen(persist_msg) && f_rc == 0 && um_rc == 0 && m_rc == 0 &&
        rd == (int)strlen(persist_msg) && strcmp(buf, persist_msg) == 0) {
      marker("M14-SMOKE: ok persistence\n");
    } else {
      char dbg[256];
      snprintf(dbg, sizeof(dbg), "M14-SMOKE: fail persistence wr=%d frc=%d um=%d m=%d rd=%d got=%s\n",
               wr, (int)f_rc, (int)um_rc, (int)m_rc, rd, buf[0] ? buf : "(empty)");
      marker(dbg);
    }
  } else {
    marker("M14-SMOKE: fail persistence open\n");
  }

  /* 7. Stress loop (repeated create, write, read, delete loop) */
  int stress_ok = 1;
  for (int i = 0; i < 10; i++) {
    char path[64];
    snprintf(path, sizeof(path), "/ext3/stress_%d.txt", i);
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
    if (read(fd, r_buf, strlen(data)) != (int)strlen(data) || strcmp(r_buf, data) != 0) {
      close(fd);
      stress_ok = 0;
      break;
    }
    close(fd);
    if (syscall(SYS_UNLINK, path) < 0) {
      stress_ok = 0;
      break;
    }
  }
  if (stress_ok) {
    marker("M14-SMOKE: ok stress-loop\n");
  } else {
    marker("M14-SMOKE: fail stress-loop\n");
  }

  /* 8. Large file boundary (within safe QEMU memory limits) */
  int fd_large = open("/ext3/large_file.txt", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd_large >= 0) {
    /* Write 128KB in chunks of 4KB */
    char *chunk_data = malloc(4096);
    memset(chunk_data, 'A', 4096);
    int write_ok = 1;
    for (int i = 0; i < 32; i++) {
      if (write(fd_large, chunk_data, 4096) != 4096) {
        write_ok = 0;
        break;
      }
    }
    syscall(SYS_FSYNC, fd_large);
    close(fd_large);
    free(chunk_data);

    /* Read back and verify size and pattern */
    struct stat st;
    isize stat_rc = syscall(SYS_STAT, "/ext3/large_file.txt", &st);
    
    int fd_read_large = open("/ext3/large_file.txt", O_RDONLY);
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
      snprintf(dbg, sizeof(dbg), "M14-SMOKE: fail large-file wok=%d s_rc=%d sz=%d rok=%d\n",
               write_ok, (int)stat_rc, (int)st.st_size, read_ok);
      marker(dbg);
    }
  } else {
    marker("M14-SMOKE: fail large-file open\n");
  }

  /* 9. VFS Path Normalization */
  int fd_norm = open("/ext3/../ext3/./large_file.txt", O_RDONLY);
  if (fd_norm >= 0) {
    struct stat st;
    isize stat_rc = syscall(SYS_FSTAT, fd_norm, &st);
    close(fd_norm);
    if (stat_rc == 0 && st.st_size == 128 * 1024) {
      marker("M14-SMOKE: ok VFS-normalization\n");
    } else {
      marker("M14-SMOKE: fail VFS-normalization\n");
    }
  } else {
    marker("M14-SMOKE: fail VFS-normalization open\n");
  }

  /* Clean up files and unmount filesystems */
  syscall(SYS_UNLINK, "/ext3/large_file.txt");
  syscall(SYS_UNLINK, "/ext3/persist.txt");
  syscall(SYS_UNLINK, "/ext3/cache_test.txt");
  syscall(SYS_UNLINK, "/ext4/persist_ext4.txt");

  syscall(SYS_UMOUNT, "/ext3");
  syscall(SYS_UMOUNT, "/ext4");

  marker("M14-SMOKE: done\n");
  return 0;
}

#include <utmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <errno.h>

static char utmp_file[256] = _PATH_UTMP;
static int utmp_fd = -1;
static struct utmp utmp_buf;

void utmpname(const char *file) {
  strncpy(utmp_file, file, sizeof(utmp_file) - 1);
  utmp_file[sizeof(utmp_file) - 1] = '\0';
  if (utmp_fd >= 0) {
    close(utmp_fd);
    utmp_fd = -1;
  }
}

void setutent(void) {
  if (utmp_fd < 0) {
    utmp_fd = open(utmp_file, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (utmp_fd < 0) {
      utmp_fd = open(utmp_file, O_RDONLY | O_CLOEXEC);
    }
  } else {
    lseek(utmp_fd, 0, SEEK_SET);
  }
}

void endutent(void) {
  if (utmp_fd >= 0) {
    close(utmp_fd);
    utmp_fd = -1;
  }
}

struct utmp *getutent(void) {
  if (utmp_fd < 0) {
    setutent();
    if (utmp_fd < 0) return NULL;
  }
  if (read(utmp_fd, &utmp_buf, sizeof(struct utmp)) == sizeof(struct utmp)) {
    return &utmp_buf;
  }
  return NULL;
}

struct utmp *getutid(const struct utmp *ut) {
  struct utmp *u;
  while ((u = getutent()) != NULL) {
    if (ut->ut_type == RUN_LVL || ut->ut_type == BOOT_TIME ||
        ut->ut_type == NEW_TIME || ut->ut_type == OLD_TIME) {
      if (u->ut_type == ut->ut_type) return u;
    } else if (ut->ut_type == INIT_PROCESS || ut->ut_type == LOGIN_PROCESS ||
               ut->ut_type == USER_PROCESS || ut->ut_type == DEAD_PROCESS) {
      if (u->ut_type == INIT_PROCESS || u->ut_type == LOGIN_PROCESS ||
          u->ut_type == USER_PROCESS || u->ut_type == DEAD_PROCESS) {
        if (strncmp(u->ut_id, ut->ut_id, sizeof(u->ut_id)) == 0) return u;
      }
    }
  }
  return NULL;
}

struct utmp *getutline(const struct utmp *ut) {
  struct utmp *u;
  while ((u = getutent()) != NULL) {
    if (u->ut_type == LOGIN_PROCESS || u->ut_type == USER_PROCESS) {
      if (strncmp(u->ut_line, ut->ut_line, sizeof(u->ut_line)) == 0) return u;
    }
  }
  return NULL;
}

struct utmp *pututline(const struct utmp *ut) {
  if (utmp_fd < 0) {
    setutent();
    if (utmp_fd < 0) return NULL;
  }
  
  struct utmp u;
  off_t pos = 0;
  int found = 0;
  lseek(utmp_fd, 0, SEEK_SET);
  while (read(utmp_fd, &u, sizeof(struct utmp)) == sizeof(struct utmp)) {
    if ((ut->ut_type == LOGIN_PROCESS || ut->ut_type == USER_PROCESS ||
         ut->ut_type == DEAD_PROCESS || ut->ut_type == INIT_PROCESS) &&
        (u.ut_type == LOGIN_PROCESS || u.ut_type == USER_PROCESS ||
         u.ut_type == DEAD_PROCESS || u.ut_type == INIT_PROCESS)) {
      if (strncmp(u.ut_id, ut->ut_id, sizeof(u.ut_id)) == 0) {
        lseek(utmp_fd, pos, SEEK_SET);
        found = 1;
        break;
      }
    }
    pos = lseek(utmp_fd, 0, SEEK_CUR);
  }
  
  if (!found) {
    lseek(utmp_fd, 0, SEEK_END);
  }
  
  if (write(utmp_fd, ut, sizeof(struct utmp)) == sizeof(struct utmp)) {
    memcpy(&utmp_buf, ut, sizeof(struct utmp));
    return &utmp_buf;
  }
  return NULL;
}

void updwtmp(const char *wtmp_file, const struct utmp *ut) {
  int fd = open(wtmp_file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd >= 0) {
    write(fd, ut, sizeof(struct utmp));
    close(fd);
  }
}

void logwtmp(const char *line, const char *name, const char *host) {
  struct utmp ut;
  memset(&ut, 0, sizeof(ut));
  ut.ut_type = name[0] ? USER_PROCESS : DEAD_PROCESS;
  ut.ut_pid = getpid();
  strncpy(ut.ut_line, line, sizeof(ut.ut_line));
  strncpy(ut.ut_user, name, sizeof(ut.ut_user));
  strncpy(ut.ut_host, host, sizeof(ut.ut_host));
  struct timeval tv;
  gettimeofday(&tv, NULL);
  ut.ut_tv.tv_sec = tv.tv_sec;
  ut.ut_tv.tv_usec = tv.tv_usec;
  
  updwtmp("/var/log/wtmp", &ut);
}

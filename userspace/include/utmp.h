#ifndef B1NIX_U_UTMP_H
#define B1NIX_U_UTMP_H

#include <sys/types.h>
#include <time.h>
#include <paths.h>

#define UT_LINESIZE 32
#define UT_NAMESIZE 32
#define UT_HOSTSIZE 256

/* ut_type values */
#define EMPTY         0
#define RUN_LVL       1
#define BOOT_TIME     2
#define NEW_TIME      3
#define OLD_TIME      4
#define INIT_PROCESS  5
#define LOGIN_PROCESS 6
#define USER_PROCESS  7
#define DEAD_PROCESS  8
#define ACCOUNTING    9

struct exit_status {
  short e_termination;
  short e_exit;
};

struct utmp {
  short ut_type;
  pid_t ut_pid;
  char ut_line[UT_LINESIZE];
  char ut_id[4];
  char ut_user[UT_NAMESIZE];
  char ut_host[UT_HOSTSIZE];
  struct exit_status ut_exit;
  long ut_session;
  struct {
    int tv_sec;
    int tv_usec;
  } ut_tv;
  int32_t ut_addr_v6[4];
  char __unused[20];
};

#ifdef __cplusplus
extern "C" {
#endif

void utmpname(const char *file);
struct utmp *getutent(void);
struct utmp *getutid(const struct utmp *ut);
struct utmp *getutline(const struct utmp *ut);
struct utmp *pututline(const struct utmp *ut);
void setutent(void);
void endutent(void);
void updwtmp(const char *wtmp_file, const struct utmp *ut);
void logwtmp(const char *line, const char *name, const char *host);

#ifdef __cplusplus
}
#endif

#endif

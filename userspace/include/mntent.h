#ifndef B1NIX_U_MNTENT_H
#define B1NIX_U_MNTENT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOUNTED     "/proc/mounts"
#define MNTTAB      "/etc/fstab"

/* Standard filesystem mount option names (subset used by common tools). */
#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RO       "ro"
#define MNTOPT_RW       "rw"
#define MNTOPT_SUID     "suid"
#define MNTOPT_NOSUID   "nosuid"
#define MNTOPT_NOAUTO   "noauto"

struct mntent {
  char *mnt_fsname; /* device or remote filesystem */
  char *mnt_dir;    /* mount point */
  char *mnt_type;   /* filesystem type */
  char *mnt_opts;   /* mount options */
  int mnt_freq;     /* dump frequency (days) */
  int mnt_passno;   /* fsck pass number */
};

FILE *setmntent(const char *filename, const char *type);
struct mntent *getmntent(FILE *stream);
struct mntent *getmntent_r(FILE *stream, struct mntent *result, char *buffer,
                           int bufsize);
int addmntent(FILE *stream, const struct mntent *mnt);
int endmntent(FILE *stream);
char *hasmntopt(const struct mntent *mnt, const char *opt);

#ifdef __cplusplus
}
#endif

#endif /* B1NIX_U_MNTENT_H */

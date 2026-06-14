#ifndef _SYS_FILE_H
#define _SYS_FILE_H

/* flock(2) advisory whole-file locks. b1nix backs these with fcntl-style
 * locking in the kernel; the operation constants match Linux. */
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int fd, int operation);

#endif

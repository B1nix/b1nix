#ifndef B1NIX_U_SYS_KLOG_H
#define B1NIX_U_SYS_KLOG_H

/* SYSLOG_ACTION_* command codes accepted by klogctl(), matching the Linux
 * syslog(2) ABI that BusyBox dmesg drives. b1nix only implements the subset
 * dmesg needs: READ_ALL/READ_CLEAR (drain the ring), SIZE_BUFFER (ring size),
 * and CONSOLE_LEVEL (accepted but a no-op — b1nix has no console loglevel). */
#define SYSLOG_ACTION_CLOSE         0
#define SYSLOG_ACTION_OPEN          1
#define SYSLOG_ACTION_READ          2
#define SYSLOG_ACTION_READ_ALL      3
#define SYSLOG_ACTION_READ_CLEAR    4
#define SYSLOG_ACTION_CLEAR         5
#define SYSLOG_ACTION_CONSOLE_OFF   6
#define SYSLOG_ACTION_CONSOLE_ON    7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD   9
#define SYSLOG_ACTION_SIZE_BUFFER   10

int klogctl(int type, char *bufp, int len);

#endif

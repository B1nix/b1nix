#ifndef B1NIX_SYS_IOCTL_H
#define B1NIX_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

/* Linux _IOC ioctl-number encoding, so headers can compute request numbers
 * (e.g. BLKGETSIZE64 = _IOR(0x12,114,size_t)). The b1nix kernel decodes the
 * type/nr fields and ignores the size/dir bits, so per-arch size_t differences
 * are harmless. */
#define _IOC_NRBITS    8
#define _IOC_TYPEBITS  8
#define _IOC_SIZEBITS  14
#define _IOC_DIRBITS   2
#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U
#define _IOC(dir, type, nr, size)                                              \
  (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) |                      \
   ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IO(type, nr)        _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, size) _IOC(_IOC_READ, (type), (nr), (sizeof(size)))
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), (sizeof(size)))
#define _IOWR(type, nr, size)                                                  \
  _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (sizeof(size)))

/* Socket ioctl numbers (Linux linux/sockios.h ABI). Interface queries
 * (SIOCGIF*) are served by the b1nix socket ioctl handler from netdev state;
 * route mutation (SIOCADDRT/DELRT) is accepted as a no-op. */
#define SIOCADDRT      0x890B
#define SIOCDELRT      0x890C
#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCSIFFLAGS   0x8914
#define SIOCGIFADDR    0x8915
#define SIOCSIFADDR    0x8916
#define SIOCGIFDSTADDR 0x8917
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFNETMASK 0x891B
#define SIOCSIFNETMASK 0x891C
#define SIOCGIFMETRIC  0x891D
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933
#define SIOCGIFTXQLEN  0x8942
#define SIOCSIFDSTADDR 0x8918
#define SIOCSIFBRDADDR 0x891A
#define SIOCSIFMETRIC  0x891E
#define SIOCSIFMTU     0x8922
#define SIOCSIFHWADDR  0x8924
#define SIOCSIFTXQLEN  0x8943
#define SIOCADDMULTI        0x8931
#define SIOCDELMULTI        0x8932
#define SIOCSIFHWBROADCAST  0x8937

/* Window-size ioctl + struct (glibc exposes these via <sys/ioctl.h>). Guarded so
 * including both <sys/ioctl.h> and <termios.h> is safe. */
#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#endif
#ifndef TIOCSWINSZ
#define TIOCSWINSZ 0x5414
#endif
#ifndef _STRUCT_WINSIZE_DEFINED
#define _STRUCT_WINSIZE_DEFINED 1
struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};
#endif

#ifdef __cplusplus
}
#endif

#endif

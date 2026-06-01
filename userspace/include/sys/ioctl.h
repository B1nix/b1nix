#ifndef B1NIX_SYS_IOCTL_H
#define B1NIX_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif

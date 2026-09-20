#ifndef B1NIX_U_LINUX_VERSION_H
#define B1NIX_U_LINUX_VERSION_H

/* Claim a modern kernel so BusyBox uses the 64-bit loop API path. */
#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#define LINUX_VERSION_CODE KERNEL_VERSION(5, 10, 0)

#endif

#ifndef _LINUX_RANDOM_H
#define _LINUX_RANDOM_H

/* Minimal <linux/random.h> for the b1nix libc (added for the Chromium port,
 * M60-62). b1nix has a getrandom() syscall; ports such as boringssl only need
 * the GRND_* flag values from this Linux UAPI header. Values match Linux. */

#define GRND_NONBLOCK 0x0001
#define GRND_RANDOM   0x0002
#define GRND_INSECURE 0x0004

#endif /* _LINUX_RANDOM_H */

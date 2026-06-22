#ifndef _LINUX_MEMFD_H
#define _LINUX_MEMFD_H
/* <linux/memfd.h>: memfd_create flags. b1nix defines MFD_CLOEXEC /
 * MFD_ALLOW_SEALING in <sys/mman.h>; mirror them here (plus the hugetlb flags,
 * which b1nix ignores) for code that includes the Linux path. Added for the
 * Chromium port (M60-62). */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC       0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#define MFD_HUGETLB       0x0004U
#define MFD_NOEXEC_SEAL   0x0008U
#define MFD_EXEC          0x0010U
#endif /* _LINUX_MEMFD_H */

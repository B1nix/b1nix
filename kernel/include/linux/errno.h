/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_ERRNO_H
#define LKPI_LINUX_ERRNO_H

/*
 * Error numbers, defined here rather than pulled from <b1nix/errno.h>.
 *
 * This is the boundary rule the import rests on: a translation unit compiling
 * imported source must not see b1nix's own headers. b1nix/errno.h also defines
 * ERR_PTR, PTR_ERR and IS_ERR as macros, which collide with the typed functions
 * <linux/err.h> is supposed to provide; every such collision so far has been
 * patched with a macro whose correctness depended on include order, and that is
 * a class of bug rather than a list of them.
 *
 * The numbers are the same on both sides because they are ABI — userspace reads
 * them out of errno — so duplicating them here duplicates a constant, not a
 * decision. b1nix's own errno.h remains the authority; if the two ever disagree
 * the ABI is already broken.
 */

#define EPERM            1
#define ENOENT           2
#define ESRCH            3
#define EINTR            4
#define EIO              5
#define ENXIO            6
#define E2BIG            7
#define ENOEXEC          8
#define EBADF            9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define ENOTBLK         15
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define ETXTBSY         26
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define EDOM            33
#define ERANGE          34
#define EDEADLK         35
#define ENAMETOOLONG    36
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define ENOMSG          42
#define EIDRM           43
#define ENODATA         61
#define EPROTO          71
#define EOVERFLOW       75
#define EBADMSG         74
#define ENOTSUPP       524
#define EOPNOTSUPP      95
#define ETIME           62
#define ETIMEDOUT      110
#define EALREADY       114
#define EINPROGRESS    115
#define ESTALE         116
#define EREMOTEIO      121
#define ENOTSUP        EOPNOTSUPP
#define EDEADLOCK      EDEADLK
/*
 * Filesystem errors. EUCLEAN is the one that matters: btrfs returns it for a
 * checksum mismatch and ext4 for a corrupt structure, and userspace maps it to
 * "Structure needs cleaning" — a distinct answer from EIO, which means the
 * device failed. Collapsing the two tells a user their disk is dying when their
 * filesystem needs fsck, or the reverse.
 */
#define ENOTCONN       107
#define ESHUTDOWN      108
/* "Invalid request descriptor" — what fiemap returns for a flag it does not
 * support, which userspace distinguishes from EINVAL. */
#define EBADR           53
#define ECANCELED      125
#define EUCLEAN        117
/* "This parameter is not mine." The mount-option parser returns it for a name
 * that is not in the filesystem's table, and every caller treats it as "ask
 * the next parser", not as a failure. */
#define ENOPARAM       519
#define ENOTNAM        118
#define ENAVAIL        119
#define EISNAM         120
#define EDQUOT          122
#define ENOKEY          126
#define EKEYEXPIRED     127
#define EKEYREVOKED     128
#define EKEYREJECTED    129
#define EOWNERDEAD      130
#define ENOTRECOVERABLE 131
#define ERFKILL         132
#define EHWPOISON       133
#define EXDEV            18
#define EMLINK           31
#define ETXTBSY          26
#define EFBIG            27
#define ENOSPC           28
#define ESPIPE           29
#define EROFS            30

/* Kernel-internal, never returned to userspace. Linux keeps these above the
 * POSIX range for exactly that reason. */
#define ERESTARTSYS    512
#define ENOIOCTLCMD    515
#define EPROBE_DEFER   517


/* The rarer codes imported drivers return. Values are Linux's, because they
 * cross to userspace and a driver's errno must mean there what it means here. */
#ifndef ECHRNG
#define ECHRNG   44  /* channel number out of range */
#define EBADSLT  57  /* invalid slot */
#define EL3RST   47
#define ENOTUNIQ 76
#endif


/* The rest of the codes imported drivers return. Values are Linux's, because a
 * driver's errno crosses to userspace and has to mean there what it means
 * here. */
#ifndef ENOBUFS
#define ENOBUFS  105
#endif
#ifndef ENOLINK
#define ENOLINK  67
#define EMULTIHOP 72
#define EREMOTE  66
#endif
#ifndef ENOKEY
#define ENOKEY   126
#endif


#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif


#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif


#ifndef EADDRINUSE
#define EADDRINUSE 98
#endif
#ifndef ENOPKG
#define ENOPKG 65
#endif

#endif

/* SPDX-License-Identifier: MIT */
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

/* Kernel-internal, never returned to userspace. Linux keeps these above the
 * POSIX range for exactly that reason. */
#define ERESTARTSYS    512
#define ENOIOCTLCMD    515
#define EPROBE_DEFER   517

#endif

#ifndef _LINUX_FUTEX_H
#define _LINUX_FUTEX_H

/* Minimal <linux/futex.h>: the op constants ports pass to syscall(SYS_futex).
 * b1nix's futex syscall uses the Linux WAIT/WAKE op encoding. */
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_FD             2
#define FUTEX_REQUEUE        3
#define FUTEX_CMP_REQUEUE    4
#define FUTEX_WAKE_OP        5
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10

#define FUTEX_PRIVATE_FLAG   128
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_WAIT_PRIVATE   (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE   (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)

#endif

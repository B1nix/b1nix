#ifndef _LINUX_PERF_EVENT_H
#define _LINUX_PERF_EVENT_H

/* Minimal <linux/perf_event.h> for b1nix.
 *
 * b1nix has no perf subsystem. This header exists so google_benchmark (pulled
 * into content_shell via base/test:test_support → perf_test_suite, but never run
 * on b1nix) compiles. google_benchmark's real perf-counter code is behind
 * `#if defined HAVE_LIBPFM` (off on b1nix), so perf_event_open is never called;
 * the struct/enums below just satisfy the compile. Real perf = a kernel feature
 * (not planned). */

#include <stdint.h>

enum perf_type_id {
    PERF_TYPE_HARDWARE = 0,
};

enum perf_hw_id {
    PERF_COUNT_HW_MAX = 10,
};

enum perf_event_read_format {
    PERF_FORMAT_TOTAL_TIME_ENABLED = 1U << 0,
    PERF_FORMAT_TOTAL_TIME_RUNNING = 1U << 1,
    PERF_FORMAT_ID                 = 1U << 2,
    PERF_FORMAT_GROUP              = 1U << 3,
};

/* PMU type is packed into the high bits of perf_event_attr.config. */
#define PERF_PMU_TYPE_SHIFT 32

/* perf ioctl()s (canonical Linux _IO('$', n) values). Used by google_benchmark's
 * counter control; never actually issued on b1nix (no perf fd to ioctl). */
#define PERF_EVENT_IOC_ENABLE  0x2400
#define PERF_EVENT_IOC_DISABLE 0x2401
#define PERF_EVENT_IOC_REFRESH 0x2402
#define PERF_EVENT_IOC_RESET   0x2403
#define PERF_EVENT_IOC_ID      0x80082407

struct perf_event_attr {
    uint32_t type;
    uint32_t size;
    uint64_t config;
    uint64_t read_format;
    uint32_t disabled : 1;
    uint32_t inherit : 1;
    uint32_t pinned : 1;
    uint32_t exclude_user : 1;
    uint32_t exclude_kernel : 1;
    uint32_t exclude_hv : 1;
    uint32_t __reserved_bits : 26;
    uint64_t __reserved[8];
};

#endif /* _LINUX_PERF_EVENT_H */

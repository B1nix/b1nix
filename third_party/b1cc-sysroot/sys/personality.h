#ifndef _SYS_PERSONALITY_H
#define _SYS_PERSONALITY_H

/* Minimal <sys/personality.h> for b1nix.
 *
 * b1nix has no personality(2) — there is no per-process execution-domain or
 * ASLR-persona control. personality() returns -1 so callers (e.g. the
 * google_benchmark test-infra, which queries/sets ADDR_NO_RANDOMIZE to stabilise
 * benchmarks) gracefully skip ASLR control. Nothing on b1nix runs benchmarks; if
 * real persona control is ever wanted it becomes a kernel feature. */

#define ADDR_NO_RANDOMIZE 0x0040000

static inline int personality(unsigned long persona) {
    (void)persona;
    return -1;
}

#endif /* _SYS_PERSONALITY_H */

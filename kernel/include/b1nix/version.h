#ifndef B1NIX_VERSION_H
#define B1NIX_VERSION_H

#define B1NIX_VERSION_STR "0.114.83"

/* The Linux kernel version b1nix's Linux ABI layer claims to implement.
 *
 * glibc's dynamic loader compares this against the minimum recorded in a
 * binary's NT_GNU_ABI_TAG note and calls _dl_fatal_printf("FATAL: kernel too
 * old") if it loses, so a distribution's binaries cannot start under a
 * release string that does not parse as a Linux version. Raise it when the
 * ABI layer genuinely gains what a newer release implies. */
#define B1NIX_LINUX_ABI_RELEASE "6.6.0"

/* The kernel release, as uname(2) reports it and as everything keyed on it
 * spells it: /lib/modules/<release>, a module's vermagic, /proc/version,
 * /proc/sys/kernel/osrelease and /sys/kernel/osrelease. It has to parse as a
 * Linux version (see above) and it has to name b1nix, so it is both. */
#define B1NIX_RELEASE_STR B1NIX_LINUX_ABI_RELEASE "-b1nix-" B1NIX_VERSION_STR

#endif

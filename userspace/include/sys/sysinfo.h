#ifndef B1NIX_U_SYS_SYSINFO_H
#define B1NIX_U_SYS_SYSINFO_H

/* C-only: some builds force-include this header globally (e.g. the busybox port,
 * whose procps applets need struct sysinfo but whose libbb.h refuses to pull it
 * in). A -include that also reaches .S files must be a no-op for the assembler. */
#ifndef __ASSEMBLER__

/* Linux struct sysinfo ABI (see sysinfo(2)). The kernel SYS_SYSINFO handler
 * copies a byte-identical layout out, so the field widths here MUST mirror the
 * kernel's: both sides use native `unsigned long`, which is 32-bit on i686 and
 * 64-bit on x86_64 — identical per arch, so no padding mismatch. The trailing
 * _f[] pad keeps the struct at the historical 64-bit/32-bit size. */
struct sysinfo {
  long uptime;             /* seconds since boot */
  unsigned long loads[3];  /* 1, 5, 15 minute load averages (<<16 fixed point) */
  unsigned long totalram;  /* total usable main memory size */
  unsigned long freeram;   /* available memory size */
  unsigned long sharedram; /* amount of shared memory */
  unsigned long bufferram; /* memory used by buffers */
  unsigned long totalswap; /* total swap space size */
  unsigned long freeswap;  /* swap space still available */
  unsigned short procs;    /* number of current processes */
  unsigned short pad;      /* explicit padding */
  unsigned long totalhigh; /* total high memory size */
  unsigned long freehigh;  /* available high memory size */
  unsigned int mem_unit;   /* memory unit size in bytes */
  char _f[20 - 2 * sizeof(long) - sizeof(int)]; /* pad to 64 bytes */
};

int sysinfo(struct sysinfo *info);

#endif /* !__ASSEMBLER__ */

#endif

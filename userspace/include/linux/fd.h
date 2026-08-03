/* b1nix: uapi <linux/fd.h>. b1nix drives no floppy controller, and this header
 * exists for the callers that ask whether a device is one — mkfs.vfat issues
 * FDGETPRM and treats a failure as "not a floppy", which is the true answer
 * here. The struct is Linux's, so the failing call is a well-formed request
 * rather than a guess at a layout.
 */
#ifndef _B1NIX_LINUX_FD_H
#define _B1NIX_LINUX_FD_H

struct floppy_struct {
  unsigned int size;    /* nr of sectors total */
  unsigned int sect;    /* sectors per track */
  unsigned int head;    /* nr of heads */
  unsigned int track;   /* nr of tracks */
  unsigned int stretch; /* bit 0: double track steps */
  unsigned char gap;    /* gap 1 size */
  unsigned char rate;   /* data rate, |= 0x40 for perpendicular */
  unsigned char spec1;  /* stepping rate, head unload time */
  unsigned char fmt_gap;/* gap2 size for formatting */
  const char *name;
};

#define FDGETPRM _IOR(2, 0x04, struct floppy_struct)

#endif /* _B1NIX_LINUX_FD_H */

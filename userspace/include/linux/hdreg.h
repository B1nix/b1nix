/* b1nix: uapi <linux/hdreg.h> — only HDIO_GETGEO, which is the one thing
 * mkfs.vfat and fdisk ask a block device for that b1nix can answer.
 *
 * The geometry is synthesized (255 heads, 63 sectors, cylinders derived from
 * the capacity), exactly as Linux does for every device that has no real CHS
 * addressing left. It exists because the FAT boot sector has fields for it,
 * not because anything seeks by cylinder.
 */
#ifndef _B1NIX_LINUX_HDREG_H
#define _B1NIX_LINUX_HDREG_H

struct hd_geometry {
  unsigned char heads;
  unsigned char sectors;
  unsigned short cylinders;
  unsigned long start;
};

#define HDIO_GETGEO 0x0301

#endif /* _B1NIX_LINUX_HDREG_H */

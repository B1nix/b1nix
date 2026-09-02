#ifndef B1NIX_BOOTMARK_H
#define B1NIX_BOOTMARK_H

/*
 * Bring-up probe for a board with no serial console.
 *
 * BOOTMARK(n) paints the number n on the framebuffer the bootloader left
 * scanning, overwriting whatever number was there before — so the display
 * always shows the last milestone reached, and a boot that stops says exactly
 * where. See fb_boot_mark() in kernel/arch/aarch64/arch.c.
 *
 * Compiles to nothing on every build except the one that defines
 * B1NIX_FB_BOOT_MARKERS (the `bahamut` target), and to nothing at all on
 * architectures that have a serial port worth using.
 */
#if defined(__aarch64__)
void fb_boot_mark(int slot);
void fb_boot_num(int row, int value);
#define BOOTMARK(slot) fb_boot_mark((slot))
#else
#define BOOTMARK(slot) ((void)0)
#define fb_boot_num(row, val) ((void)0)
#endif

#endif /* B1NIX_BOOTMARK_H */

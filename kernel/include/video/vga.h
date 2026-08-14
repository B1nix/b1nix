/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_VIDEO_VGA_H
#define LKPI_VIDEO_VGA_H
/* The legacy VGA register window. b1nix's own video code owns it; imported
 * drivers reach it only through vgaarb, which is where the arbitration lives. */
#define VGA_MIS_W 0x3c2
#define VGA_MIS_R 0x3cc
#define VGA_SR_INDEX 0x3c4
#define VGA_SR_DATA  0x3c5
#define VGA_SEQ_CLOCK_MODE 0x01
#define VGA_SR01_SCREEN_OFF 0x20

/* The sequencer's index and data ports, under the names the imported code uses
 * for them. Same two registers as VGA_SR_INDEX/VGA_SR_DATA above. */
#define VGA_SEQ_I 0x3c4
#define VGA_SEQ_D 0x3c5

#endif

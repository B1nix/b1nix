#ifndef B1NIX_U_LINUX_LOOP_H
#define B1NIX_U_LINUX_LOOP_H

#include <stdint.h>

/* Loop-device ioctl ABI (Linux linux/loop.h), enough for BusyBox losetup. The
 * b1nix kernel implements LOOP_CTL_GET_FREE, LOOP_SET_FD, LOOP_CLR_FD and the
 * status ioctls over its loop_device backing. */

#define LO_NAME_SIZE 64
#define LO_KEY_SIZE  32

struct loop_info64 {
  uint64_t lo_device;
  uint64_t lo_inode;
  uint64_t lo_rdevice;
  uint64_t lo_offset;
  uint64_t lo_sizelimit;
  uint32_t lo_number;
  uint32_t lo_encrypt_type;
  uint32_t lo_encrypt_key_size;
  uint32_t lo_flags;
  uint8_t  lo_file_name[LO_NAME_SIZE];
  uint8_t  lo_crypt_name[LO_NAME_SIZE];
  uint8_t  lo_encrypt_key[LO_KEY_SIZE];
  uint64_t lo_init[2];
};

#define LOOP_SET_FD        0x4C00
#define LOOP_CLR_FD        0x4C01
#define LOOP_SET_STATUS    0x4C02
#define LOOP_GET_STATUS    0x4C03
#define LOOP_SET_STATUS64  0x4C04
#define LOOP_GET_STATUS64  0x4C05
#define LOOP_CHANGE_FD     0x4C06
#define LOOP_SET_CAPACITY  0x4C07
#define LOOP_SET_DIRECT_IO 0x4C08
#define LOOP_SET_BLOCK_SIZE 0x4C09
#define LOOP_CONFIGURE     0x4C0A

/* /dev/loop-control interface */
#define LOOP_CTL_ADD      0x4C80
#define LOOP_CTL_REMOVE   0x4C81
#define LOOP_CTL_GET_FREE 0x4C82

#endif

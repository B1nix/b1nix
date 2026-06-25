#ifndef B1NIX_U_LINUX_SYNC_FILE_H
#define B1NIX_U_LINUX_SYNC_FILE_H
/* Linux sync-fence file UAPI. b1nix has no DRM sync fences; provided so the
 * graphics sync-fence helper (third_party/libsync) compiles. */
#include <linux/types.h>
#include <sys/ioctl.h>

struct sync_merge_data {
  char name[32];
  __s32 fd2;
  __s32 fence;
  __u32 flags;
  __u32 pad;
};

struct sync_fence_info {
  char obj_name[32];
  char driver_name[32];
  __s32 status;
  __u32 flags;
  __u64 timestamp_ns;
};

struct sync_file_info {
  char name[32];
  __s32 status;
  __u32 flags;
  __u32 num_fences;
  __u32 pad;
  __u64 sync_fence_info;
};

#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_MERGE     _IOWR(SYNC_IOC_MAGIC, 3, struct sync_merge_data)
#define SYNC_IOC_FILE_INFO _IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)

#endif /* B1NIX_U_LINUX_SYNC_FILE_H */

#ifndef _LINUX_MAGIC_H
#define _LINUX_MAGIC_H

/* <linux/magic.h>: filesystem superblock magic numbers, as returned by
 * statfs(2)/fstatfs(2) in f_type. Values match the Linux UAPI so ports that
 * classify a filesystem by its magic (e.g. Chromium sys_info / process)
 * behave identically. Added for the Chromium port (M60-62). */

#define ADFS_SUPER_MAGIC      0xadf5
#define AFFS_SUPER_MAGIC      0xadff
#define AFS_SUPER_MAGIC       0x5346414f
#define AUTOFS_SUPER_MAGIC    0x0187
#define CEPH_SUPER_MAGIC      0x00c36400
#define CODA_SUPER_MAGIC      0x73757245
#define CRAMFS_MAGIC          0x28cd3d45
#define DEBUGFS_MAGIC         0x64626720
#define SECURITYFS_MAGIC      0x73636673
#define SELINUX_MAGIC         0xf97cff8c
#define SMACK_MAGIC           0x43415d53
#define RAMFS_MAGIC           0x858458f6
#define TMPFS_MAGIC           0x01021994
#define HUGETLBFS_MAGIC       0x958458f6
#define SQUASHFS_MAGIC        0x73717368
#define ECRYPTFS_SUPER_MAGIC  0xf15f
#define EFS_SUPER_MAGIC       0x414a53
#define EROFS_SUPER_MAGIC_V1  0xe0f5e1e2
#define EXT2_SUPER_MAGIC      0xef53
#define EXT3_SUPER_MAGIC      0xef53
#define EXT4_SUPER_MAGIC      0xef53
#define XENFS_SUPER_MAGIC     0xabba1974
#define BTRFS_SUPER_MAGIC     0x9123683e
#define NILFS_SUPER_MAGIC     0x3434
#define F2FS_SUPER_MAGIC      0xf2f52010
#define HPFS_SUPER_MAGIC      0xf995e849
#define ISOFS_SUPER_MAGIC     0x9660
#define JFFS2_SUPER_MAGIC     0x72b6
#define XFS_SUPER_MAGIC       0x58465342
#define PSTOREFS_MAGIC        0x6165676c
#define EFIVARFS_MAGIC        0xde5e81e4
#define HOSTFS_SUPER_MAGIC    0x00c0ffee
#define OVERLAYFS_SUPER_MAGIC 0x794c7630
#define FUSE_SUPER_MAGIC      0x65735546

#define MINIX_SUPER_MAGIC     0x137f
#define MINIX_SUPER_MAGIC2    0x138f
#define MINIX2_SUPER_MAGIC    0x2468
#define MINIX2_SUPER_MAGIC2   0x2478
#define MINIX3_SUPER_MAGIC    0x4d5a

#define MSDOS_SUPER_MAGIC     0x4d44
#define NCP_SUPER_MAGIC       0x564c
#define NFS_SUPER_MAGIC       0x6969
#define OCFS2_SUPER_MAGIC     0x7461636f
#define OPENPROM_SUPER_MAGIC  0x9fa1
#define QNX4_SUPER_MAGIC      0x002f
#define QNX6_SUPER_MAGIC      0x68191122
#define AFS_FS_MAGIC          0x6b414653

#define REISERFS_SUPER_MAGIC  0x52654973
#define SMB_SUPER_MAGIC       0x517b
#define CGROUP_SUPER_MAGIC    0x27e0eb
#define CGROUP2_SUPER_MAGIC   0x63677270

#define RDTGROUP_SUPER_MAGIC  0x7655821
#define STACK_END_MAGIC       0x57AC6E9D
#define TRACEFS_MAGIC         0x74726163
#define V9FS_MAGIC            0x01021997
#define BDEVFS_MAGIC          0x62646576
#define DAXFS_MAGIC           0x64646178
#define BINFMTFS_MAGIC        0x42494e4d
#define DEVPTS_SUPER_MAGIC    0x1cd1
#define BINDERFS_SUPER_MAGIC  0x6c6f6f70
#define FUTEXFS_SUPER_MAGIC   0xBAD1DEA
#define PIPEFS_MAGIC          0x50495045
#define PROC_SUPER_MAGIC      0x9fa0
#define SOCKFS_MAGIC          0x534f434b
#define SYSFS_MAGIC           0x62656572
#define USBDEVICE_SUPER_MAGIC 0x9fa2
#define MTD_INODE_FS_MAGIC    0x11307854
#define ANON_INODE_FS_MAGIC   0x09041934
#define BTRFS_TEST_MAGIC      0x73727279
#define NSFS_MAGIC            0x6e736673
#define BPF_FS_MAGIC          0xcafe4a11
#define AAFS_MAGIC            0x5a3c69f0
#define ZONEFS_MAGIC          0x5a4f4653
#define UDF_SUPER_MAGIC       0x15013346
#define DMA_BUF_MAGIC         0x444d4142
#define DEVMEM_MAGIC          0x454d444d
#define SECRETMEM_MAGIC       0x5345434d

#endif /* _LINUX_MAGIC_H */

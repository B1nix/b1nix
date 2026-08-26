/*
 * mdcreate — write b1nix RAID superblocks onto member disks.
 *
 * There is no mdadm here, and the kernel deliberately reads its own superblock
 * format rather than claiming Linux's (see kernel/include/b1nix/md.h). This is
 * the tool that writes it: given a level and a list of block devices, it
 * stamps each one with its place in the array. `raidautorun` then finds them
 * and the kernel brings the array up.
 *
 *   mdcreate stripe|mirror <chunk-blocks> /dev/sdb /dev/sdc ...
 *
 * The chunk size is ignored for a mirror. Every member is stamped with the
 * same array id, which is what ties them together at assembly time.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MD_MAGIC 0x4231494e444d3031ULL
#define MD_LEVEL_STRIPE 0
#define MD_LEVEL_MIRROR 1

struct md_superblock {
	unsigned long long magic;
	unsigned int level;
	unsigned int members;
	unsigned int member_index;
	unsigned int chunk_blocks;
	unsigned long long data_blocks;
	unsigned long long data_offset;
	unsigned long long array_uuid;
	unsigned int sb_csum;
	unsigned int pad;
};

static unsigned int md_csum(const struct md_superblock *sb)
{
	unsigned long long acc = sb->magic + sb->level + sb->members +
				 sb->member_index + sb->chunk_blocks +
				 sb->data_blocks + sb->data_offset +
				 sb->array_uuid;
	return (unsigned int)(acc ^ (acc >> 32));
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr,
			"usage: mdcreate stripe|mirror <chunk-blocks> <device>...\n");
		return 2;
	}

	unsigned int level;
	if (strcmp(argv[1], "stripe") == 0)
		level = MD_LEVEL_STRIPE;
	else if (strcmp(argv[1], "mirror") == 0)
		level = MD_LEVEL_MIRROR;
	else {
		fprintf(stderr, "mdcreate: level must be stripe or mirror\n");
		return 2;
	}

	unsigned int chunk = (unsigned int)strtoul(argv[2], 0, 0);
	if (level == MD_LEVEL_STRIPE && chunk == 0) {
		fprintf(stderr, "mdcreate: a stripe needs a chunk size\n");
		return 2;
	}

	unsigned int members = (unsigned int)(argc - 3);
	/* The array id has to differ between arrays created in one run and be the
	 * same across the members of one array; the clock supplies both. */
	unsigned long long uuid = (unsigned long long)time(0) * 1000003ull +
				  (unsigned long long)getpid();

	for (unsigned int i = 0; i < members; i++) {
		const char *path = argv[3 + i];
		int fd = open(path, O_RDWR);

		if (fd < 0) {
			fprintf(stderr, "mdcreate: %s: cannot open\n", path);
			return 1;
		}

		/* How much this member can hold, minus the block the superblock
		 * itself occupies. BLKGETSIZE64 reports bytes. */
		unsigned long long bytes = 0;
		if (ioctl(fd, 0x80081272 /* BLKGETSIZE64 */, &bytes) != 0 ||
		    bytes == 0) {
			fprintf(stderr, "mdcreate: %s: cannot size\n", path);
			close(fd);
			return 1;
		}

		struct md_superblock sb;
		memset(&sb, 0, sizeof(sb));
		sb.magic = MD_MAGIC;
		sb.level = level;
		sb.members = members;
		sb.member_index = i;
		sb.chunk_blocks = (level == MD_LEVEL_STRIPE) ? chunk : 0;
		sb.data_offset = 1; /* block 0 is the superblock */
		sb.data_blocks = bytes / 512 - 1;
		sb.array_uuid = uuid;
		sb.sb_csum = md_csum(&sb);

		char block[512];
		memset(block, 0, sizeof(block));
		memcpy(block, &sb, sizeof(sb));
		if (lseek(fd, 0, SEEK_SET) != 0 ||
		    write(fd, block, sizeof(block)) != (long)sizeof(block)) {
			fprintf(stderr, "mdcreate: %s: cannot write superblock\n",
				path);
			close(fd);
			return 1;
		}
		fsync(fd);
		close(fd);
		printf("mdcreate: %s member %u/%u of array %llu\n", path, i + 1,
		       members, uuid);
	}

	printf("mdcreate: ok %s %u members\n", argv[1], members);
	return 0;
}

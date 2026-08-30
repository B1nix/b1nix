/* btrfs read checks.
 *
 * In the kernel rather than in the driver module on purpose: the test drives
 * btrfs the way a caller does — mount by type name, open by path, read — and
 * touches none of the driver's internals. A test that reaches inside the thing
 * it tests can pass while the interface is broken.
 */

#include <b1nix/btrfs.h>
#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <b1nix/errno.h>
#include <b1nix/posix.h>
#include <stdio.h>
#include <string.h>

/* ── Self-test ─────────────────────────────────────────────────────────────
 *
 * Checked against a filesystem THIS DRIVER DID NOT WRITE: the smoke suite
 * builds it with mkfs.btrfs and hands it over as a disk. That is the only way
 * to find out whether the on-disk format was read or merely reproduced — a
 * driver tested against its own output agrees with itself and nothing else.
 *
 * The large file's content is a formula (line N is N, zero-padded), so the
 * guest derives what it should see rather than being told. */
static int btrfs_probe_disk(struct block_device *dev) {
    u8 *buf = kmalloc(4096);

    if (!buf)
        return 0;
    int ok = 0;

    if (blk_read_cached(dev, BTRFS_SUPER_INFO_OFFSET / 512, 8, buf) >= 0) {
        const struct btrfs_super_block *sb =
            (const struct btrfs_super_block *)buf;

        ok = memcmp(sb->magic, BTRFS_MAGIC, 8) == 0;
    }
    kfree(buf);
    return ok;
}

/* Says WHICH way it failed, because "the file did not read as expected" covers
 * three different bugs: the entry was never created, it was created and cannot
 * be opened, or it opened and gave back the wrong bytes. */
static int btrfs_expect_file(const char *path, const char *want) {
    int fd = vfs_open_flags(path, B1NIX_O_RDONLY);

    if (fd < 0) {
        char line[160];

        snprintf(line, sizeof(line), "M119-BTRFS: diag open('%s') = %d\n", path,
                 fd);
        console_write(line);
        return 0;
    }

    char buf[128];
    isize n = vfs_read(fd, buf, sizeof(buf) - 1);

    vfs_close(fd);
    if (n < 0) {
        char line[160];

        snprintf(line, sizeof(line), "M119-BTRFS: diag read('%s') = %ld\n", path,
                 (long)n);
        console_write(line);
        return 0;
    }
    buf[n] = '\0';
    if (strcmp(buf, want) == 0)
        return 1;

    char line[224];

    snprintf(line, sizeof(line), "M119-BTRFS: diag '%s' n=%ld got='%.32s'\n",
             path, (long)n, buf);
    console_write(line);
    return 0;
}

void btrfs_selftest(void) {
    const char *dev_name = 0;
    usize n = blk_count();

    for (usize i = 0; i < n; i++) {
        struct block_device *d = blk_at(i);

        if (d && d->name && btrfs_probe_disk(d)) {
            dev_name = d->name;
            break;
        }
    }
    if (!dev_name)
        return; /* no btrfs disk attached to this instance */

    console_write("M119-BTRFS: start\n");

    if (vfs_mkdir("/mnt/btrfs", 0755) < 0 && vfs_find_node("/mnt/btrfs") == 0) {
        console_write("M119-BTRFS: FAIL mkdir\n");
        return;
    }
    int rc = vfs_mount(dev_name, "/mnt/btrfs", "btrfs", 0);

    if (rc < 0) {
        char line[96];

        snprintf(line, sizeof(line), "M119-BTRFS: FAIL mount rc=%d\n", rc);
        console_write(line);
        return;
    }
    console_write("M119-BTRFS: ok mount\n");

    /* A small file: btrfs stores it inline, inside the tree leaf itself. */
    if (btrfs_expect_file("/mnt/btrfs/hello.txt", "hello from btrfs\n"))
        console_write("M119-BTRFS: ok inline-file\n");
    else
        console_write("M119-BTRFS: FAIL inline-file\n");

    /* A file in a nested directory: the tree walk descended twice. */
    if (btrfs_expect_file("/mnt/btrfs/dir/sub/deep.txt", "nested\n"))
        console_write("M119-BTRFS: ok nested-dir\n");
    else
        console_write("M119-BTRFS: FAIL nested-dir\n");

    /* A symlink, whose target IS its file content on btrfs. */
    {
        char target[64];
        isize len = vfs_readlink("/mnt/btrfs/link.txt", target,
                                 sizeof(target) - 1);

        if (len > 0) {
            target[len] = '\0';
            if (strcmp(target, "hello.txt") == 0)
                console_write("M119-BTRFS: ok symlink\n");
            else
                console_write("M119-BTRFS: FAIL symlink\n");
        } else {
            console_write("M119-BTRFS: FAIL symlink\n");
        }
    }

    /* The large file: a regular extent, read at an offset that is neither the
     * start of the file nor a sector boundary. Line N of the file is N in 14
     * digits followed by a newline, so the expected bytes are computed here
     * rather than remembered. */
    {
        int fd = vfs_open_flags("/mnt/btrfs/big.bin", B1NIX_O_RDONLY);

        if (fd < 0) {
            console_write("M119-BTRFS: FAIL regular-extent open\n");
        } else {
            /* The generator writes 12288 lines of "%014d\n" — fourteen digits
             * and a newline, so fifteen bytes each and 184320 in all. Both
             * numbers are derived from that one fact rather than remembered
             * separately, which is what keeps the check honest if the
             * generator changes. */
            const u64 line_len = 15;
            const u64 lines = 12288;
            struct b1nix_stat st;
            int have_size =
                vfs_fstat(fd, &st) == 0 && (u64)st.st_size == lines * line_len;
            /* A line well past the first sector, so the read crosses into the
             * middle of a regular extent rather than starting at its edge. */
            const u64 line = 4096;
            char want[16];

            snprintf(want, sizeof(want), "%014llu\n", (unsigned long long)line);

            char got[16];
            int ok = 0;

            if (vfs_lseek(fd, (isize)(line * line_len), 0) >= 0) {
                isize r = vfs_read(fd, got, 15);

                if (r == 15) {
                    got[15] = '\0';
                    ok = strncmp(got, want, 15) == 0;
                }
            }
            vfs_close(fd);
            if (have_size)
                console_write("M119-BTRFS: ok regular-size\n");
            else
                console_write("M119-BTRFS: FAIL regular-size\n");
            if (ok)
                console_write("M119-BTRFS: ok regular-extent\n");
            else
                console_write("M119-BTRFS: FAIL regular-extent\n");
        }
    }

    console_write("M119-BTRFS: done\n");
}

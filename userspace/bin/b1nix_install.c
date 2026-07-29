/* b1nix-install — install b1nix onto a disk (RPi/cloud-image model).
 *
 * Copies a pre-baked, standalone-bootable disk image (MBR + Limine + ext4
 * root, built on the host by tools/images/mk-disk-image.sh, V8/Chromium excluded) onto
 * a target block device. No mkfs/bootloader/partitioning in-guest — the image already
 * has it all. After this, the target disk boots b1nix on its own.
 *
 *   b1nix-install [-y] [--no-packages] [<source-image>] <target-disk>
 *
 * <source-image> defaults to the live medium's image if omitted.
 * Example: b1nix-install /mnt/iso/boot/b1nix-disk.img /dev/sda
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>

#define CHUNK (1024 * 1024)           /* 1 MiB copy buffer */

static const char *default_sources[] = {
    "/mnt/iso/boot/b1nix-disk.img",   /* live ISO/USB */
    "/boot/b1nix-disk.img",
    NULL,
};

/* Size of a regular file OR a block device. fstat() reports st_size==0 for
 * b1nix block-device nodes (the size lives in the inode, not surfaced via
 * st_size), so prefer lseek(SEEK_END); fall back to fstat for plain files.
 * Returns -1 if the size can't be determined (caller then copies until EOF). */
static long file_size(int fd) {
    long end = lseek(fd, 0, SEEK_END);
    if (end > 0) { lseek(fd, 0, SEEK_SET); return end; }
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) return (long)st.st_size;
    return -1;
}

int main(int argc, char **argv) {
    int force = 0, install_packages = 1, ai = 1;
    while (ai < argc) {
        if (strcmp(argv[ai], "-y") == 0) force = 1;
        else if (strcmp(argv[ai], "--no-packages") == 0) install_packages = 0;
        else break;
        ai++;
    }

    const char *src = NULL, *dst = NULL;
    int rest = argc - ai;
    if (rest == 1) {                  /* only target → default source */
        dst = argv[ai];
        for (int i = 0; default_sources[i]; i++) {
            if (access(default_sources[i], R_OK) == 0) { src = default_sources[i]; break; }
        }
        if (!src) {
            fprintf(stderr, "b1nix-install: no disk image found; pass one explicitly\n");
            return 1;
        }
    } else if (rest == 2) {
        src = argv[ai]; dst = argv[ai + 1];
    } else {
        fprintf(stderr, "Usage: b1nix-install [-y] [--no-packages] [<source-image>] <target-disk>\n");
        return 1;
    }

    int sfd = open(src, O_RDONLY);
    if (sfd < 0) { fprintf(stderr, "b1nix-install: cannot open source %s\n", src); return 1; }
    long ssize = file_size(sfd);   /* -1 if unknown → copy until EOF */

    int dfd = open(dst, O_WRONLY);
    if (dfd < 0) { fprintf(stderr, "b1nix-install: cannot open target %s\n", dst); close(sfd); return 1; }
    long dsize = file_size(dfd);
    if (dsize > 0 && dsize < ssize) {
        fprintf(stderr, "b1nix-install: target %s (%ld MiB) smaller than image (%ld MiB)\n",
                dst, dsize / (1024 * 1024), ssize / (1024 * 1024));
        close(sfd); close(dfd); return 1;
    }

    if (ssize > 0) printf("Installing %s -> %s (%ld MiB)\n", src, dst, ssize / (1024 * 1024));
    else           printf("Installing %s -> %s (size unknown, copying to EOF)\n", src, dst);
    printf("This ERASES all data on %s.\n", dst);
    if (!force) {
        printf("Type 'yes' to continue: ");
        fflush(stdout);
        char line[16];
        if (!fgets(line, sizeof(line), stdin) || strncmp(line, "yes", 3) != 0) {
            printf("Aborted.\n");
            close(sfd); close(dfd); return 1;
        }
    }

    char *buf = malloc(CHUNK);
    if (!buf) { fprintf(stderr, "b1nix-install: out of memory\n"); close(sfd); close(dfd); return 1; }

    long copied = 0, next_report = 0;
    for (;;) {
        long n = read(sfd, buf, CHUNK);
        if (n < 0) { fprintf(stderr, "\nb1nix-install: read error at %ld\n", copied); free(buf); close(sfd); close(dfd); return 1; }
        if (n == 0) break;
        long off = 0;
        while (off < n) {
            long w = write(dfd, buf + off, (size_t)(n - off));
            if (w <= 0) { fprintf(stderr, "\nb1nix-install: write error at %ld\n", copied + off); free(buf); close(sfd); close(dfd); return 1; }
            off += w;
        }
        copied += n;
        if (copied >= next_report) {
            if (ssize > 0) printf("\r  %ld / %ld MiB", copied / (1024 * 1024), ssize / (1024 * 1024));
            else           printf("\r  %ld MiB", copied / (1024 * 1024));
            fflush(stdout);
            next_report += 16 * 1024 * 1024;
        }
    }

    free(buf);
    close(sfd);
    fsync(dfd);
    close(dfd);
    sync();
    printf("\r  %ld MiB copied\n", copied / (1024 * 1024));

    if (install_packages) {
        int fd = open(dst, O_RDONLY);
        int dummy = 0;
        if (fd < 0 || ioctl(fd, BLKRRPART, &dummy) != 0) {
            fprintf(stderr, "b1nix-install: cannot rescan target partitions\n");
            if (fd >= 0) close(fd);
            return 1;
        }
        close(fd);

        const char *base = strrchr(dst, '/');
        base = base ? base + 1 : dst;
        char partition[64];
        snprintf(partition, sizeof(partition), "%sp1", base);
        mkdir("/mnt/install", 0755);
        if (mount(partition, "/mnt/install", "ext4", 0, NULL) != 0) {
            fprintf(stderr, "b1nix-install: cannot mount %s\n", partition);
            return 1;
        }

        /* install-all pulls every arch-matching package for this target, which
         * includes the 'dev' sysroot (static libs + crt0 + headers
         * runtime headers). With it on disk, the installed system can compile
         * and link programs with the on-target b1cc/make. */
        printf("Installing packages from b1nix-pkgs (incl. dev sysroot)...\n");
        int rc = system("ROOT=/mnt/install bpkg update && ROOT=/mnt/install bpkg install-all");
        sync();
        umount("/mnt/install");
        if (rc != 0) {
            fprintf(stderr, "b1nix-install: package installation failed; base system remains bootable\n");
            return 1;
        }
    }

    printf("Done. Remove the install medium and reboot.\n");
    return 0;
}

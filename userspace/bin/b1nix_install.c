/* b1nix-install — install b1nix onto a disk (RPi/cloud-image model).
 *
 * Copies a pre-baked, standalone-bootable disk image (MBR + real GRUB + ext4
 * root, built on the host by tools/mk-disk-image.sh, V8/Chromium excluded) onto
 * a target block device. No mkfs/grub/partitioning in-guest — the image already
 * has it all. After this, the target disk boots b1nix on its own.
 *
 *   b1nix-install [-y] [<source-image>] <target-disk>
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
    int force = 0, ai = 1;
    if (ai < argc && strcmp(argv[ai], "-y") == 0) { force = 1; ai++; }

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
        fprintf(stderr, "Usage: b1nix-install [-y] [<source-image>] <target-disk>\n");
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
    printf("\r  %ld MiB copied\nDone. Remove the install medium and reboot.\n",
           copied / (1024 * 1024));
    return 0;
}

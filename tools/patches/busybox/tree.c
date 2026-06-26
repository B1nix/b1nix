/* __b1nix__: scandir/alphasort-free reimplementation of BusyBox tree. */
//config:config TREE
//config:	bool "tree (2.5 kb)"
//config:	default y
//config:	help
//config:	List files and directories in a tree structure.
//config:
//applet:IF_TREE(APPLET(tree, BB_DIR_USR_BIN, BB_SUID_DROP))
//kbuild:lib-$(CONFIG_TREE) += tree.o
//usage:#define tree_trivial_usage NOUSAGE_STR
//usage:#define tree_full_usage ""

#include "libbb.h"
#include "common_bufsiz.h"
#include "unicode.h"

#define prefix_buf bb_common_bufsiz1

static void tree_print(unsigned count[2], const char *directory_name, char *prefix_pos) {
    DIR *d;
    struct dirent **sorted = NULL;
    int n = 0, cap = 0, i;
    const char *bar = "|   ";
    const char *mid = "|-- ";
    const char *end = "`-- ";
    if (ENABLE_UNICODE_SUPPORT && unicode_status == UNICODE_ON) {
        bar = "│   ";
        mid = "├── ";
        end = "└── ";
    }
    d = opendir(directory_name);
    fputs_stdout(directory_name);
    if (!d) { puts(" [error opening dir]"); return; }
    puts("");
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (n >= cap) { cap = cap ? cap * 2 : 64; sorted = xrealloc(sorted, cap * sizeof(sorted[0])); }
        sorted[n] = xstrdup(de->d_name);
        n++;
    }
    closedir(d);
    for (i = 0; i < n; i++) {
        int j;
        for (j = i; j > 0 && strcmp(sorted[j - 1], sorted[j]) > 0; j--) {
            char *tmp = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = tmp;
        }
    }
    xchdir(directory_name);
    for (i = 0; i < n; i++) {
        const char *name = sorted[i];
        int is_last = (i == n - 1);
        struct stat statBuf;
        strcpy(prefix_pos, is_last ? end : mid);
        fputs_stdout(prefix_buf);
        int status = lstat(name, &statBuf);
        if (status == 0 && S_ISLNK(statBuf.st_mode)) {
            char *symlink_path = xmalloc_readlink(name);
            printf("%s -> %s\n", name, symlink_path);
            free(symlink_path);
        } else {
            puts(name);
        }
        if (status == 0 && S_ISDIR(statBuf.st_mode)) {
            char *sub_prefix = prefix_pos + strlen(prefix_pos);
            strcpy(sub_prefix, is_last ? "    " : bar);
            tree_print(count, name, sub_prefix);
        }
        free(sorted[i]);
    }
    free(sorted);
    xchdir("..");
}

int tree_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int tree_main(int argc, char **argv) {
    unsigned count[2] = {0, 0};
    const char *dir = ".";
    if (argc > 1) dir = argv[1];
    char *prefix = prefix_buf;
    tree_print(count, dir, prefix);
    return 0;
}

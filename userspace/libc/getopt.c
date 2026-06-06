#include <unistd.h>
#include <string.h>
#include <stdio.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

static int optpos = 1;

int getopt(int argc, char *const argv[], const char *optstring) {
    optarg = NULL;
    if (optind == 0) {
        optind = 1;
        optpos = 1;
    }
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || argv[optind][1] == '\0') {
        return -1;
    }

    if (strcmp(argv[optind], "--") == 0) {
        optind++;
        return -1;
    }

    char c = argv[optind][optpos];
    const char *p = strchr(optstring, c);

    if (c == '-' || !p) {
        if (opterr && optstring[0] != ':') {
            fprintf(stderr, "%s: illegal option -- %c\n", argv[0], c);
        }
        optopt = c;
        optpos++;
        if (argv[optind][optpos] == '\0') {
            optind++;
            optpos = 1;
        }
        return '?';
    }

    optopt = c;
    if (p[1] == ':') {
        if (argv[optind][optpos + 1] != '\0') {
            optarg = &argv[optind][optpos + 1];
            optind++;
            optpos = 1;
        } else {
            optind++;
            if (optind >= argc) {
                if (opterr && optstring[0] != ':') {
                    fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], c);
                }
                optpos = 1;
                return (optstring[0] == ':') ? ':' : '?';
            }
            optarg = argv[optind];
            optind++;
            optpos = 1;
        }
    } else {
        optpos++;
        if (argv[optind][optpos] == '\0') {
            optind++;
            optpos = 1;
        }
    }

    return c;
}

#include <unistd.h>
#include <getopt.h>
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

/* Shared "--name" / "--name=arg" parser for getopt_long{,_only}. `arg` points
 * just past the leading dashes of argv[optind]. Advances optind and returns the
 * value to hand back to the caller (or 0 when the matched option uses `flag`).
 * Mirrors GNU semantics: exact names win, otherwise a single unambiguous prefix
 * is accepted; missing/extra arguments are diagnosed like the short-option path. */
static int parse_long_option(int argc, char *const argv[], const char *optstring,
                             const struct option *longopts, int *longindex,
                             const char *arg) {
    const char *eq = strchr(arg, '=');
    size_t namelen = eq ? (size_t)(eq - arg) : strlen(arg);

    int match = -1, nmatch = 0, exact = 0;
    for (int i = 0; longopts && longopts[i].name; i++) {
        if (strncmp(longopts[i].name, arg, namelen) == 0) {
            if (strlen(longopts[i].name) == namelen) {
                match = i;
                exact = 1;
                break;
            }
            match = i;
            nmatch++;
        }
    }

    if (!exact && nmatch != 1) {
        if (opterr) {
            fprintf(stderr, "%s: %s option '--%.*s'\n", argv[0],
                    nmatch == 0 ? "unrecognized" : "ambiguous",
                    (int)namelen, arg);
        }
        optind++;
        optopt = 0;
        return '?';
    }

    const struct option *o = &longopts[match];
    optind++;
    optpos = 1;
    if (longindex)
        *longindex = match;

    optarg = NULL;
    if (o->has_arg == required_argument || o->has_arg == optional_argument) {
        if (eq) {
            optarg = (char *)eq + 1;
        } else if (o->has_arg == required_argument) {
            if (optind >= argc) {
                if (opterr) {
                    fprintf(stderr, "%s: option '--%s' requires an argument\n",
                            argv[0], o->name);
                }
                optopt = o->val;
                return (optstring[0] == ':') ? ':' : '?';
            }
            optarg = argv[optind++];
        }
    } else if (eq) { /* no_argument but "--name=arg" given */
        if (opterr) {
            fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                    argv[0], o->name);
        }
        optopt = o->val;
        return '?';
    }

    if (o->flag) {
        *o->flag = o->val;
        return 0;
    }
    return o->val;
}

/* Like getopt(), but also recognises "--name"/"--name=arg" from longopts. When
 * `long_only` is set, a single-dash "-name" is also tried as a long option
 * before falling back to short-option processing. */
static int getopt_long_impl(int argc, char *const argv[], const char *optstring,
                            const struct option *longopts, int *longindex,
                            int long_only) {
    optarg = NULL;
    if (optind == 0) {
        optind = 1;
        optpos = 1;
    }
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' ||
        argv[optind][1] == '\0') {
        return -1;
    }
    if (strcmp(argv[optind], "--") == 0) {
        optind++;
        return -1;
    }

    /* Only consider a fresh argument as a long option (optpos==1); mid-cluster
     * short options keep going through getopt(). */
    if (optpos == 1) {
        if (argv[optind][1] == '-') {
            return parse_long_option(argc, argv, optstring, longopts, longindex,
                                     argv[optind] + 2);
        }
        if (long_only && longopts) {
            const char *arg = argv[optind] + 1;
            const char *eq = strchr(arg, '=');
            size_t namelen = eq ? (size_t)(eq - arg) : strlen(arg);
            /* Treat "-name" as long only when it is not a lone known short
             * option char (so "-h" stays a short option unless ambiguous). */
            int looks_long = namelen > 1 || !strchr(optstring, arg[0]);
            if (looks_long) {
                for (int i = 0; longopts[i].name; i++) {
                    if (strncmp(longopts[i].name, arg, namelen) == 0) {
                        return parse_long_option(argc, argv, optstring, longopts,
                                                 longindex, arg);
                    }
                }
            }
        }
    }

    return getopt(argc, argv, optstring);
}

int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex) {
    return getopt_long_impl(argc, argv, optstring, longopts, longindex, 0);
}

int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex) {
    return getopt_long_impl(argc, argv, optstring, longopts, longindex, 1);
}

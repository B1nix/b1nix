#ifndef B1NIX_U_GETOPT_H
#define B1NIX_U_GETOPT_H

/* GNU-style long-option parsing. The short-option variables (optarg, optind,
 * opterr, optopt) and getopt() itself are declared in <unistd.h>; pull them in
 * so code that includes only <getopt.h> still sees them, matching glibc. */
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* has_arg values for struct option. */
#define no_argument        0
#define required_argument  1
#define optional_argument  2

struct option {
  const char *name; /* long option name, without the leading "--" */
  int has_arg;      /* no_argument / required_argument / optional_argument */
  int *flag;        /* if non-NULL, *flag is set to val and 0 is returned */
  int val;          /* value to return (or store via flag) on a match */
};

/* Like getopt(), but also recognises "--name" / "--name=arg" long options from
 * longopts. On a long match, returns val (or stores it via flag and returns 0)
 * and sets *longindex (if non-NULL) to the matched index. Exact names and
 * unambiguous prefixes are accepted, mirroring GNU getopt_long. */
int getopt_long(int argc, char *const argv[], const char *optstring,
                const struct option *longopts, int *longindex);

/* As getopt_long, but a single leading '-' may also introduce a long option. */
int getopt_long_only(int argc, char *const argv[], const char *optstring,
                     const struct option *longopts, int *longindex);

#ifdef __cplusplus
}
#endif

#endif /* B1NIX_U_GETOPT_H */

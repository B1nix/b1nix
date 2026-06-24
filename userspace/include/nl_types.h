#ifndef B1NIX_U_NL_TYPES_H
#define B1NIX_U_NL_TYPES_H

typedef void *nl_catd;
typedef int nl_item;

#define NL_SETD 1
#define NL_CAT_LOCALE 1

/* b1nix has no message catalogs: catopen always fails and catgets returns the
 * caller-supplied default string. Used by libc++'s message_facet. */
static inline nl_catd catopen(const char *name, int flag) {
    (void)name; (void)flag; return (nl_catd)-1;
}
static inline char *catgets(nl_catd catd, int set, int msg, const char *s) {
    (void)catd; (void)set; (void)msg; return (char *)s;
}
static inline int catclose(nl_catd catd) { (void)catd; return 0; }

#endif

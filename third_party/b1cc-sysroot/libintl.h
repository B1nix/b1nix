/* No-op <libintl.h> (GNU gettext) for b1nix.
 *
 * b1nix is C-locale only and ships no message catalogs, so every gettext
 * variant returns its input string unchanged (the standard behaviour when no
 * translation is installed). bindtextdomain/textdomain are accepted and report
 * success without doing anything. This is honest: there is no translation
 * machinery to defer to, so the untranslated string IS the correct result.
 */
#ifndef _LIBINTL_H
#define _LIBINTL_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline char *gettext(const char *__msgid) {
	return (char *)__msgid;
}

static inline char *dgettext(const char *__domainname, const char *__msgid) {
	(void)__domainname;
	return (char *)__msgid;
}

static inline char *dcgettext(const char *__domainname, const char *__msgid,
			      int __category) {
	(void)__domainname; (void)__category;
	return (char *)__msgid;
}

static inline char *ngettext(const char *__msgid1, const char *__msgid2,
			     unsigned long int __n) {
	return (char *)(__n == 1 ? __msgid1 : __msgid2);
}

static inline char *dngettext(const char *__domainname, const char *__msgid1,
			      const char *__msgid2, unsigned long int __n) {
	(void)__domainname;
	return (char *)(__n == 1 ? __msgid1 : __msgid2);
}

static inline char *dcngettext(const char *__domainname, const char *__msgid1,
			       const char *__msgid2, unsigned long int __n,
			       int __category) {
	(void)__domainname; (void)__category;
	return (char *)(__n == 1 ? __msgid1 : __msgid2);
}

static inline char *bindtextdomain(const char *__domainname,
				   const char *__dirname) {
	(void)__domainname;
	return (char *)__dirname;
}

static inline char *bind_textdomain_codeset(const char *__domainname,
					    const char *__codeset) {
	(void)__domainname;
	return (char *)__codeset;
}

static inline char *textdomain(const char *__domainname) {
	return (char *)__domainname;
}

#ifdef __cplusplus
}
#endif

#endif /* _LIBINTL_H */

#include <wctype.h>
#include <string.h>
#include <ctype.h>

wctype_t wctype(const char *name) {
    if (strcmp(name, "alnum") == 0) return 1;
    if (strcmp(name, "alpha") == 0) return 2;
    if (strcmp(name, "cntrl") == 0) return 3;
    if (strcmp(name, "digit") == 0) return 4;
    if (strcmp(name, "graph") == 0) return 5;
    if (strcmp(name, "lower") == 0) return 6;
    if (strcmp(name, "print") == 0) return 7;
    if (strcmp(name, "punct") == 0) return 8;
    if (strcmp(name, "space") == 0) return 9;
    if (strcmp(name, "upper") == 0) return 10;
    if (strcmp(name, "xdigit") == 0) return 11;
    if (strcmp(name, "blank") == 0) return 12;
    return 0;
}

int iswctype(wint_t wc, wctype_t desc) {
    switch (desc) {
        case 1: return isalnum(wc);
        case 2: return isalpha(wc);
        case 3: return iscntrl(wc);
        case 4: return isdigit(wc);
        case 5: return isgraph(wc);
        case 6: return islower(wc);
        case 7: return isprint(wc);
        case 8: return ispunct(wc);
        case 9: return isspace(wc);
        case 10: return isupper(wc);
        case 11: return isxdigit(wc);
        case 12: return isblank(wc);
        default: return 0;
    }
}

int iswalnum(wint_t wc) { return isalnum(wc); }
int iswalpha(wint_t wc) { return isalpha(wc); }
int iswcntrl(wint_t wc) { return iscntrl(wc); }
int iswdigit(wint_t wc) { return isdigit(wc); }
int iswgraph(wint_t wc) { return isgraph(wc); }
int iswlower(wint_t wc) { return islower(wc); }
int iswprint(wint_t wc) { return isprint(wc); }
int iswpunct(wint_t wc) { return ispunct(wc); }
int iswspace(wint_t wc) { return isspace(wc); }
int iswupper(wint_t wc) { return isupper(wc); }
int iswxdigit(wint_t wc) { return isxdigit(wc); }
int iswblank(wint_t wc) { return isblank(wc); }

wint_t towlower(wint_t wc) { return tolower(wc); }
wint_t towupper(wint_t wc) { return toupper(wc); }

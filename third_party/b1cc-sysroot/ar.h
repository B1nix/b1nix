#ifndef B1NIX_U_AR_H
#define B1NIX_U_AR_H

/* System V / GNU `ar` archive format (matches the output of the ported
 * binutils ar). Used by GNU Make's arscan.c to read .a member timestamps. */

#define ARMAG  "!<arch>\n"  /* magic at start of an archive */
#define SARMAG 8            /* length of ARMAG (without NUL) */
#define ARFMAG "`\n"        /* header terminator */

struct ar_hdr {
    char ar_name[16];  /* member name */
    char ar_date[12];  /* mtime, decimal seconds */
    char ar_uid[6];    /* owner uid, decimal */
    char ar_gid[6];    /* owner gid, decimal */
    char ar_mode[8];   /* mode, octal */
    char ar_size[10];  /* member size, decimal */
    char ar_fmag[2];   /* must equal ARFMAG */
};

#endif

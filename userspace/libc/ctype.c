/* libc/ctype.c — character classification table for B1NIX userspace.
 *
 * Provides the newlib-compatible `_ctype_` table that libstdc++'s generic
 * (newlib) locale config expects: ctype<char>::classic_table() returns
 * `_ctype_ + 1`, and is(mask, c) tests `_ctype_[c + 1] & mask`. Index 0 is the
 * EOF slot; indices 1..256 classify bytes 0..255. Bit masks come from ctype.h
 * and match newlib (and the libstdc++ ctype_base::mask layout). */

#include <ctype.h>

const char _ctype_[257] = {
    0, /* index 0: EOF slot (_ctype_[-1] via classic_table()) */

    /* 0x00-0x0F */
    _C, _C, _C, _C, _C, _C, _C, _C,
    _C, _C | _S, _C | _S, _C | _S, _C | _S, _C | _S, _C, _C,
    /* 0x10-0x1F */
    _C, _C, _C, _C, _C, _C, _C, _C,
    _C, _C, _C, _C, _C, _C, _C, _C,
    /* 0x20-0x2F : space, then punctuation */
    _S | _B, _P, _P, _P, _P, _P, _P, _P,
    _P, _P, _P, _P, _P, _P, _P, _P,
    /* 0x30-0x3F : '0'-'9' then punctuation */
    _N, _N, _N, _N, _N, _N, _N, _N,
    _N, _N, _P, _P, _P, _P, _P, _P,
    /* 0x40-0x4F : '@' 'A'-'O' */
    _P, _U | _X, _U | _X, _U | _X, _U | _X, _U | _X, _U | _X, _U,
    _U, _U, _U, _U, _U, _U, _U, _U,
    /* 0x50-0x5F : 'P'-'Z' then '[' '\' ']' '^' '_' */
    _U, _U, _U, _U, _U, _U, _U, _U,
    _U, _U, _U, _P, _P, _P, _P, _P,
    /* 0x60-0x6F : '`' 'a'-'o' */
    _P, _L | _X, _L | _X, _L | _X, _L | _X, _L | _X, _L | _X, _L,
    _L, _L, _L, _L, _L, _L, _L, _L,
    /* 0x70-0x7F : 'p'-'z' then '{' '|' '}' '~' DEL */
    _L, _L, _L, _L, _L, _L, _L, _L,
    _L, _L, _L, _P, _P, _P, _P, _C,

    /* 0x80-0xFF : non-ASCII, unclassified — remaining entries are zero. */
};

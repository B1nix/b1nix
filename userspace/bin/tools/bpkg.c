/* bpkg -- b1nix's native package manager.
 *
 * Speaks two repository formats:
 *
 *   1. "flat" -- the house format already produced by tools/packages/
 *      bpkg-publish.sh and consumed at image-build time by
 *      tools/packages/install-ports.sh: a plain-text index
 *      (name version arch sha256 url [deps]) where each url points at a
 *      single gzip-compressed tar of the package's files.
 *
 *   2. "apk" -- a real Alpine Linux repository: APKINDEX.tar.gz (a gzipped
 *      tar containing one text file, APKINDEX, with one blank-line-
 *      separated record per package in Alpine's "C:value" key/value form)
 *      plus per-package .apk files, each of which is a concatenation of
 *      gzip members (signature tar, control tar with .PKGINFO, then the
 *      data tar with the actual files). The RSA signature and the control
 *      member's datahash are both verified when mbedTLS is linked in, and a
 *      signed package is refused outright when it is not. The per-file Q1
 *      checksums in APKINDEX stay unread -- the signature chain already
 *      covers the same bytes -- which is a documented gap, not a silently
 *      skipped check.
 *
 * gzip/deflate and tar are both implemented locally (RFC 1951/1952, POSIX
 * ustar + GNU longname) so this binary has no dependency on a ported zlib
 * or libarchive -- exactly the kind of third-party dependency M104+ is
 * trying to get OUT of the boot-critical path.
 *
 * State on disk (shared with tools/packages/install-ports.sh's "download"
 * mode, so a rootfs seeded at image-build time is a rootfs bpkg can keep
 * managing at runtime):
 *   /etc/bpkg.conf                 INDEX_URL=... (one KEY=VALUE per line)
 *   /var/lib/bpkg/index            cached copy of the last-fetched index
 *   /var/lib/bpkg/installed/NAME.list   newline-separated installed paths
 *   /var/lib/bpkg/installed/NAME.ver    installed version string
 *   /var/lib/bpkg/scripts/NAME.SCRIPT   the package's own install scripts,
 *                                       kept so deinstall and triggers can
 *                                       still run after the .apk is gone
 *   /var/lib/bpkg/scripts/NAME.triggers the trigger's path patterns
 *   /etc/apk/world                      explicitly requested packages
 *
 * Usage:
 *   bpkg update                    refresh /var/lib/bpkg/index from INDEX_URL
 *   bpkg list                      list installed packages
 *   bpkg search TERM               search the cached index
 *   bpkg install NAME [NAME...]    resolve deps, download, extract; over an
 *                                  older version this is an upgrade
 *   bpkg remove NAME                delete NAME's recorded files
 *   bpkg info NAME                  show cached index metadata for NAME
 *   bpkg world                      list the explicitly requested packages
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* growable buffer                                                     */
/* ------------------------------------------------------------------ */

struct buf {
    uint8_t *p;
    size_t len, cap;
};

static void buf_init(struct buf *b) { b->p = NULL; b->len = 0; b->cap = 0; }

static void buf_reserve(struct buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 4096;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t *np = realloc(b->p, ncap);
    if (!np) { fprintf(stderr, "bpkg: out of memory\n"); exit(1); }
    b->p = np; b->cap = ncap;
}

static void buf_append(struct buf *b, const void *data, size_t n) {
    buf_reserve(b, n);
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void buf_free(struct buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* ------------------------------------------------------------------ */
/* CRC32 (gzip trailer verification)                                   */
/* ------------------------------------------------------------------ */

static uint32_t crc32_table[256];
static int crc32_ready = 0;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t n) {
    if (!crc32_ready) crc32_init();
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4) -- flat-index integrity verification            */
/* ------------------------------------------------------------------ */

struct sha256_ctx {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t buflen;
};

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t sha256_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(struct sha256_ctx *c, const uint8_t *block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_rotr(w[i-15], 7) ^ sha256_rotr(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = sha256_rotr(w[i-2], 17) ^ sha256_rotr(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->h[0],b=c->h[1],cc=c->h[2],d=c->h[3],e=c->h[4],f=c->h[5],g=c->h[6],h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha256_rotr(e,6) ^ sha256_rotr(e,11) ^ sha256_rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha256_rotr(a,2) ^ sha256_rotr(a,13) ^ sha256_rotr(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_init(struct sha256_ctx *c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv));
    c->total = 0; c->buflen = 0;
}

static void sha256_update(struct sha256_ctx *c, const uint8_t *data, size_t len) {
    c->total += len;
    while (len > 0) {
        size_t take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, data, take);
        c->buflen += take; data += take; len -= take;
        if (c->buflen == 64) { sha256_transform(c, c->buf); c->buflen = 0; }
    }
}

static void sha256_final_hex(struct sha256_ctx *c, char out[65]) {
    uint64_t bitlen = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) sha256_update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bitlen >> (56 - i * 8));
    /* bypass the total+= in sha256_update for the length bytes */
    memcpy(c->buf + c->buflen, lenbuf, 8);
    sha256_transform(c, c->buf);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(c->h[i] >> (24 - j * 8));
            out[(i*4+j)*2] = hex[byte >> 4];
            out[(i*4+j)*2+1] = hex[byte & 0xF];
        }
    }
    out[64] = 0;
}

static void sha256_hex(const uint8_t *data, size_t len, char out[65]) {
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final_hex(&c, out);
}

/* ------------------------------------------------------------------ */
/* raw DEFLATE decoder (RFC 1951)                                      */
/* ------------------------------------------------------------------ */

struct inflate_state {
    const uint8_t *in;
    size_t inlen, inpos;
    uint32_t bitbuf;
    int bitcnt;
    struct buf out;
};

static int inf_getbit(struct inflate_state *s) {
    if (s->bitcnt == 0) {
        if (s->inpos >= s->inlen) return -1;
        s->bitbuf = s->in[s->inpos++];
        s->bitcnt = 8;
    }
    int bit = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return bit;
}

static int32_t inf_getbits(struct inflate_state *s, int n) {
    int32_t v = 0;
    for (int i = 0; i < n; i++) {
        int b = inf_getbit(s);
        if (b < 0) return -1;
        v |= (b << i);
    }
    return v;
}

/* canonical huffman decode table: counts[len] + sorted symbol list */
struct huff {
    uint16_t counts[16];
    uint16_t symbols[288];
    int nsym;
};

static void huff_build(struct huff *h, const uint8_t *lengths, int n) {
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; i++) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    uint16_t offs[16];
    offs[0] = 0; offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->counts[len];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbols[offs[lengths[i]]++] = (uint16_t)i;
    h->nsym = n;
}

static int huff_decode(struct inflate_state *s, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = inf_getbit(s);
        if (b < 0) return -1;
        code |= b;
        int count = h->counts[len];
        if (code - first < count) return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inf_block_data(struct inflate_state *s, const struct huff *lit, const struct huff *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            uint8_t c = (uint8_t)sym;
            buf_append(&s->out, &c, 1);
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int32_t extra = inf_getbits(s, len_extra[sym]);
            if (extra < 0) return -1;
            int length = len_base[sym] + extra;
            int dsym = huff_decode(s, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            int32_t dextra = inf_getbits(s, dist_extra[dsym]);
            if (dextra < 0) return -1;
            int distance = dist_base[dsym] + dextra;
            if ((size_t)distance > s->out.len) return -1;
            size_t start = s->out.len - distance;
            buf_reserve(&s->out, (size_t)length);
            for (int i = 0; i < length; i++) {
                uint8_t c = s->out.p[start + i];
                s->out.p[s->out.len++] = c;
            }
        }
    }
}

static void huff_fixed(struct huff *lit, struct huff *dist) {
    uint8_t litlens[288], distlens[30];
    int i = 0;
    for (; i < 144; i++) litlens[i] = 8;
    for (; i < 256; i++) litlens[i] = 9;
    for (; i < 280; i++) litlens[i] = 7;
    for (; i < 288; i++) litlens[i] = 8;
    for (i = 0; i < 30; i++) distlens[i] = 5;
    huff_build(lit, litlens, 288);
    huff_build(dist, distlens, 30);
}

static const uint8_t clc_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

static int inf_dynamic(struct inflate_state *s, struct huff *lit, struct huff *dist) {
    int32_t hlit = inf_getbits(s, 5); if (hlit < 0) return -1; hlit += 257;
    int32_t hdist = inf_getbits(s, 5); if (hdist < 0) return -1; hdist += 1;
    int32_t hclen = inf_getbits(s, 4); if (hclen < 0) return -1; hclen += 4;

    uint8_t cl_lens[19];
    memset(cl_lens, 0, sizeof(cl_lens));
    for (int i = 0; i < hclen; i++) {
        int32_t v = inf_getbits(s, 3);
        if (v < 0) return -1;
        cl_lens[clc_order[i]] = (uint8_t)v;
    }
    struct huff clh;
    huff_build(&clh, cl_lens, 19);

    uint8_t lens[288 + 32];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = huff_decode(s, &clh);
        if (sym < 0) return -1;
        if (sym < 16) {
            lens[n++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (n == 0) return -1;
            int32_t rep = inf_getbits(s, 2); if (rep < 0) return -1;
            uint8_t prev = lens[n - 1];
            for (int i = 0; i < rep + 3 && n < total; i++) lens[n++] = prev;
        } else if (sym == 17) {
            int32_t rep = inf_getbits(s, 3); if (rep < 0) return -1;
            for (int i = 0; i < rep + 3 && n < total; i++) lens[n++] = 0;
        } else {
            int32_t rep = inf_getbits(s, 7); if (rep < 0) return -1;
            for (int i = 0; i < rep + 11 && n < total; i++) lens[n++] = 0;
        }
    }
    huff_build(lit, lens, hlit);
    huff_build(dist, lens + hlit, hdist);
    return 0;
}

/* Inflate one raw DEFLATE stream starting at s->inpos. Returns 0 on success. */
static int inflate_raw(struct inflate_state *s) {
    int final;
    do {
        int b = inf_getbit(s);
        if (b < 0) return -1;
        final = b;
        int32_t type = inf_getbits(s, 2);
        if (type < 0) return -1;
        if (type == 0) {
            s->bitcnt = 0; /* byte-align */
            if (s->inpos + 4 > s->inlen) return -1;
            uint16_t len = (uint16_t)(s->in[s->inpos] | (s->in[s->inpos + 1] << 8));
            s->inpos += 4; /* len + ~len */
            if (s->inpos + len > s->inlen) return -1;
            buf_append(&s->out, s->in + s->inpos, len);
            s->inpos += len;
        } else if (type == 1) {
            struct huff lit, dist;
            huff_fixed(&lit, &dist);
            if (inf_block_data(s, &lit, &dist) < 0) return -1;
        } else if (type == 2) {
            struct huff lit, dist;
            if (inf_dynamic(s, &lit, &dist) < 0) return -1;
            if (inf_block_data(s, &lit, &dist) < 0) return -1;
        } else {
            return -1;
        }
    } while (!final);
    return 0;
}

/* ------------------------------------------------------------------ */
/* gzip container: decompress ALL concatenated members into one buffer */
/* ------------------------------------------------------------------ */

static int gunzip_all(const uint8_t *in, size_t inlen, struct buf *out) {
    size_t pos = 0;
    buf_init(out);
    while (pos < inlen) {
        if (pos + 10 > inlen || in[pos] != 0x1F || in[pos + 1] != 0x8B) {
            fprintf(stderr, "bpkg: not a gzip member at offset %zu\n", pos);
            return -1;
        }
        uint8_t flg = in[pos + 3];
        size_t hpos = pos + 10;
        if (flg & 0x04) { /* FEXTRA */
            if (hpos + 2 > inlen) return -1;
            uint16_t xlen = (uint16_t)(in[hpos] | (in[hpos + 1] << 8));
            hpos += 2 + xlen;
        }
        if (flg & 0x08) { while (hpos < inlen && in[hpos]) hpos++; hpos++; } /* FNAME */
        if (flg & 0x10) { while (hpos < inlen && in[hpos]) hpos++; hpos++; } /* FCOMMENT */
        if (flg & 0x02) hpos += 2; /* FHCRC */
        if (hpos > inlen) return -1;

        struct inflate_state s;
        s.in = in; s.inlen = inlen; s.inpos = hpos;
        s.bitbuf = 0; s.bitcnt = 0;
        size_t member_start = out->len;
        s.out = *out; /* decode straight into the accumulating buffer */
        if (inflate_raw(&s) < 0) {
            fprintf(stderr, "bpkg: corrupt deflate stream at offset %zu\n", pos);
            buf_free(&s.out);
            return -1;
        }
        *out = s.out;

        /* trailer: CRC32 + ISIZE, byte-aligned after the deflate stream */
        size_t tpos = s.inpos;
        if (tpos + 8 > inlen) { fprintf(stderr, "bpkg: truncated gzip trailer\n"); return -1; }
        uint32_t want_crc = (uint32_t)in[tpos] | ((uint32_t)in[tpos+1] << 8) |
                             ((uint32_t)in[tpos+2] << 16) | ((uint32_t)in[tpos+3] << 24);
        uint32_t got_crc = crc32_update(0, out->p + member_start, out->len - member_start);
        if (want_crc != got_crc) {
            fprintf(stderr, "bpkg: gzip CRC mismatch (want %08x got %08x)\n", want_crc, got_crc);
            return -1;
        }
        pos = tpos + 8;
    }
    return 0;
}

/* Decompress exactly the FIRST gzip member starting at *pos, advance *pos
 * past its trailer. Used for .apk's concatenated-members layout, where we
 * need member boundaries (signature / control / data), not one merged blob. */
static int gunzip_one(const uint8_t *in, size_t inlen, size_t *pos, struct buf *out) {
    size_t p = *pos;
    if (p + 10 > inlen || in[p] != 0x1F || in[p + 1] != 0x8B) return -1;
    uint8_t flg = in[p + 3];
    size_t hpos = p + 10;
    if (flg & 0x04) { uint16_t xlen = (uint16_t)(in[hpos] | (in[hpos+1] << 8)); hpos += 2 + xlen; }
    if (flg & 0x08) { while (hpos < inlen && in[hpos]) hpos++; hpos++; }
    if (flg & 0x10) { while (hpos < inlen && in[hpos]) hpos++; hpos++; }
    if (flg & 0x02) hpos += 2;
    if (hpos > inlen) return -1;

    struct inflate_state s;
    s.in = in; s.inlen = inlen; s.inpos = hpos;
    s.bitbuf = 0; s.bitcnt = 0;
    buf_init(&s.out);
    if (inflate_raw(&s) < 0) { buf_free(&s.out); return -1; }
    size_t tpos = s.inpos;
    if (tpos + 8 > inlen) { buf_free(&s.out); return -1; }
    uint32_t want_crc = (uint32_t)in[tpos] | ((uint32_t)in[tpos+1] << 8) |
                         ((uint32_t)in[tpos+2] << 16) | ((uint32_t)in[tpos+3] << 24);
    uint32_t got_crc = crc32_update(0, s.out.p, s.out.len);
    if (want_crc != got_crc) { buf_free(&s.out); return -1; }
    *out = s.out;
    *pos = tpos + 8;
    return 0;
}

/* ------------------------------------------------------------------ */
/* tar (ustar + GNU longname) extraction                               */
/* ------------------------------------------------------------------ */

struct tar_hdr {
    char name[100];
    char mode[8];
    char uid[8], gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32], gname[32];
    char devmajor[8], devminor[8];
    char prefix[155];
    char pad[12];
};

static long tar_octal(const char *field, int n) {
    long v = 0;
    for (int i = 0; i < n && field[i]; i++) {
        if (field[i] < '0' || field[i] > '7') break;
        v = v * 8 + (field[i] - '0');
    }
    return v;
}

/*
 * Create the directories an entry needs, without doing it again for the next
 * entry in the same directory.
 *
 * This runs once per archive member, and it used to issue an mkdir for every
 * component of every path: a package with a thousand files five levels deep
 * asked the kernel to create directories five thousand times, and each of
 * those resolved the path from the root. Archives are written directory by
 * directory, so remembering the last one collapses nearly all of it — the
 * mkdirs that remain are the ones that genuinely name a new directory.
 */
static void safe_mkdirs(const char *root, const char *rel) {
    static char last[1024];
    char path[1024];
    const char *slash = strrchr(rel, '/');
    size_t dirlen;

    if (!slash)
        return; /* no directory part: nothing to create */
    dirlen = (size_t)(slash - rel);
    if (snprintf(path, sizeof(path), "%.*s", (int)dirlen, rel) >= (int)sizeof(path))
        return;
    if (strcmp(path, last) == 0)
        return; /* same directory as the previous entry */
    snprintf(last, sizeof(last), "%s", path);

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    for (char *p = path + strlen(root) + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0755);
            *p = '/';
        }
    }
}

/* Returns 0 and appends every extracted path to *filelist (newline-separated,
 * relative to root) on success. Rejects absolute paths and ".." traversal --
 * same rule install-ports.sh already enforces for the flat format. */
static int tar_extract(const uint8_t *data, size_t len, const char *root, struct buf *filelist) {
    size_t pos = 0;
    char pending_name[1024]; pending_name[0] = 0;
    while (pos + 512 <= len) {
        const struct tar_hdr *h = (const struct tar_hdr *)(data + pos);
        int allzero = 1;
        for (size_t i = 0; i < 512; i++) if (data[pos + i]) { allzero = 0; break; }
        if (allzero) { pos += 512; continue; /* end-of-archive padding block */ }

        long size = tar_octal(h->size, 12);
        char name[1200];
        if (pending_name[0]) {
            snprintf(name, sizeof(name), "%s", pending_name);
            pending_name[0] = 0;
        } else if (h->prefix[0]) {
            snprintf(name, sizeof(name), "%.155s/%.100s", h->prefix, h->name);
        } else {
            snprintf(name, sizeof(name), "%.100s", h->name);
        }
        pos += 512;

        /* strip a leading "./" (very common in these tarballs) */
        char *cleanname = name;
        if (cleanname[0] == '.' && cleanname[1] == '/') cleanname += 2;

        if (cleanname[0] == '/' || strstr(cleanname, "..")) {
            fprintf(stderr, "bpkg: unsafe tar path '%s'\n", cleanname);
            return -1;
        }

        if (h->typeflag == 'L') { /* GNU long name: content is the next entry's name */
            if (pos + (size_t)size > len) return -1;
            size_t n = (size_t)size < sizeof(pending_name) - 1 ? (size_t)size : sizeof(pending_name) - 1;
            memcpy(pending_name, data + pos, n);
            pending_name[n] = 0;
            pos += ((size_t)size + 511) & ~(size_t)511;
            continue;
        }
        if (h->typeflag == 'x' || h->typeflag == 'g') { /* pax extended header: skip */
            pos += ((size_t)size + 511) & ~(size_t)511;
            continue;
        }
        if (!cleanname[0]) { /* bare "." entry */
            if (size > 0) pos += ((size_t)size + 511) & ~(size_t)511;
            continue;
        }

        if (pos + (size_t)size > len && h->typeflag != '5' && h->typeflag != '2') {
            fprintf(stderr, "bpkg: truncated tar entry '%s'\n", cleanname);
            return -1;
        }

        mode_t mode = (mode_t)tar_octal(h->mode, 8);
        char full[1280];
        snprintf(full, sizeof(full), "%s/%s", root, cleanname);

        if (h->typeflag == '5') { /* directory */
            safe_mkdirs(root, cleanname);
            mkdir(full, mode ? mode : 0755);
        } else if (h->typeflag == '2') { /* symlink */
            safe_mkdirs(root, cleanname);
            unlink(full);
            symlink(h->linkname, full);
            if (filelist) { buf_append(filelist, cleanname, strlen(cleanname)); buf_append(filelist, "\n", 1); }
        } else if (h->typeflag == '0' || h->typeflag == 0 || h->typeflag == '7') { /* regular file */
            safe_mkdirs(root, cleanname);
            int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
            if (fd < 0) { fprintf(stderr, "bpkg: cannot create %s: %s\n", full, strerror(errno)); return -1; }
            if (size > 0) {
                ssize_t w = write(fd, data + pos, (size_t)size);
                if (w < 0 || (size_t)w != (size_t)size) {
                    fprintf(stderr, "bpkg: short write to %s\n", full);
                    close(fd); return -1;
                }
            }
            close(fd);
            chmod(full, mode ? mode : 0644);
            if (filelist) { buf_append(filelist, cleanname, strlen(cleanname)); buf_append(filelist, "\n", 1); }
        }
        /* other typeflags (hardlink '1', device nodes, fifos): silently skipped,
         * none of the packages this tool targets ship them. */

        if (size > 0) pos += ((size_t)size + 511) & ~(size_t)511;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTTP/1.1 GET over TCP, with TLS when mbedTLS is available           */
/* ------------------------------------------------------------------ */

#ifdef B1NIX_HAVE_MBEDTLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <psa/crypto.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

/* Trust anchors. Alpine's mirrors and every other real repository are HTTPS,
 * so this file decides whether bpkg can install anything at all -- if it is
 * missing, an https:// fetch fails loudly instead of continuing unverified. */
#define BPKG_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"
#endif

static int connect_host(const char *host, const char *port) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) < 0) { close(fd); fd = -1; }
    freeaddrinfo(res);
    return fd;
}

/* Parses http[s]://host[:port]/path. Without mbedTLS linked in, https:// is
 * rejected with a clear error rather than silently downgraded to plaintext:
 * pretending to fetch an https:// URL would be exactly the kind of fake pass
 * the project's testing rules forbid. */
static int url_parse(const char *url, char *host, size_t hostcap, char *port, size_t portcap, char *path, size_t pathcap, int *is_tls) {
    const char *p;
    const char *defport = "80";
    if (is_tls) *is_tls = 0;
    if (strncmp(url, "http://", 7) == 0) p = url + 7;
    else if (strncmp(url, "https://", 8) == 0) {
#ifdef B1NIX_HAVE_MBEDTLS
        p = url + 8;
        defport = "443";
        if (is_tls) *is_tls = 1;
#else
        fprintf(stderr, "bpkg: https:// not supported (this bpkg was built without mbedTLS) -- use an http mirror or a local file:// index\n");
        return -1;
#endif
    } else { fprintf(stderr, "bpkg: unsupported URL scheme in '%s'\n", url); return -1; }

    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));
    if (colon) {
        size_t hl = (size_t)(colon - p);
        if (hl >= hostcap) return -1;
        memcpy(host, p, hl); host[hl] = 0;
        size_t pl = (size_t)(hostend - colon - 1);
        if (pl >= portcap) return -1;
        memcpy(port, colon + 1, pl); port[pl] = 0;
    } else {
        size_t hl = (size_t)(hostend - p);
        if (hl >= hostcap) return -1;
        memcpy(host, p, hl); host[hl] = 0;
        snprintf(port, portcap, "%s", defport);
    }
    if (slash) snprintf(path, pathcap, "%s", slash);
    else snprintf(path, pathcap, "/");
    return 0;
}

/* memmem() is a GNU/BSD extension; musl provides it via string.h, but keep a
 * fallback so this file has no surprise link-time dependency if it's absent. */
static void *bpkg_memmem(const void *haystack, size_t hlen, const void *needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) return NULL;
    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(h + i, n, nlen) == 0) return (void *)(h + i);
    return NULL;
}

/* One HTTP connection: a plain fd, or the same fd underneath an mbedTLS
 * session. Everything above this point in the file (gzip, tar, sha256, index
 * parsing) stays dependency-free -- TLS is isolated to these three calls. */
struct conn {
    int fd;
#ifdef B1NIX_HAVE_MBEDTLS
    int tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
#endif
};

#ifdef B1NIX_HAVE_MBEDTLS
static int conn_bio_send(void *ctx, const unsigned char *b, size_t n) {
    ssize_t r = write(*(int *)ctx, b, n);
    return r < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : (int)r;
}
static int conn_bio_recv(void *ctx, unsigned char *b, size_t n) {
    ssize_t r = read(*(int *)ctx, b, n);
    return r < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : (int)r;
}
#endif

static int conn_open(struct conn *c, const char *host, const char *port, int use_tls) {
    memset(c, 0, sizeof(*c));
    c->fd = connect_host(host, port);
    if (c->fd < 0) { fprintf(stderr, "bpkg: cannot connect to %s:%s\n", host, port); return -1; }
    if (!use_tls) return 0;

#ifdef B1NIX_HAVE_MBEDTLS
    c->tls = 1;
    /* mbedTLS 3.6 runs TLS 1.3 through PSA, and without psa_crypto_init() the
     * handshake dies with a bare MBEDTLS_ERR_SSL_INTERNAL_ERROR (-0x6c00)
     * that names nothing. */
    psa_crypto_init();
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->drbg);

    int rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
                                   (const unsigned char *)"bpkg", 4);
    if (rc != 0) { fprintf(stderr, "bpkg: TLS RNG seed failed (-0x%04x)\n", -rc); goto fail; }

    rc = mbedtls_x509_crt_parse_file(&c->ca, BPKG_CA_BUNDLE);
    if (rc < 0) {
        fprintf(stderr, "bpkg: cannot load trust anchors from %s (-0x%04x) -- refusing to fetch over an unverified TLS connection\n",
                BPKG_CA_BUNDLE, -rc);
        goto fail;
    }

    rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) { fprintf(stderr, "bpkg: TLS config failed (-0x%04x)\n", -rc); goto fail; }
    /* Verification is REQUIRED: a package manager that accepts any
     * certificate is a package manager that installs anything. */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

    rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc != 0) { fprintf(stderr, "bpkg: TLS setup failed (-0x%04x)\n", -rc); goto fail; }
    /* SNI, and the name the certificate is checked against. */
    rc = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (rc != 0) { fprintf(stderr, "bpkg: TLS hostname failed (-0x%04x)\n", -rc); goto fail; }
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, conn_bio_send, conn_bio_recv, NULL);

    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        fprintf(stderr, "bpkg: TLS handshake with %s failed (-0x%04x)\n", host, -rc);
        goto fail;
    }
    uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
    if (flags != 0) {
        char why[512];
        mbedtls_x509_crt_verify_info(why, sizeof(why), "  ", flags);
        fprintf(stderr, "bpkg: certificate for %s rejected:\n%s", host, why);
        goto fail;
    }
    return 0;
fail:
    close(c->fd);
    c->fd = -1;
    return -1;
#else
    (void)use_tls;
    return -1;
#endif
}

static ssize_t conn_write(struct conn *c, const void *b, size_t n) {
#ifdef B1NIX_HAVE_MBEDTLS
    if (c->tls) {
        size_t done = 0;
        while (done < n) {
            int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)b + done, n - done);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r <= 0) return -1;
            done += (size_t)r;
        }
        return (ssize_t)done;
    }
#endif
    return write(c->fd, b, n);
}

static ssize_t conn_read(struct conn *c, void *b, size_t n) {
#ifdef B1NIX_HAVE_MBEDTLS
    if (c->tls) {
        for (;;) {
            int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)b, n);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            /* TLS 1.3 servers send session tickets mid-stream; mbedTLS reports
             * each one to the caller. It is not data and not an error -- keep
             * reading, or the response looks empty and every fetch fails. */
            if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
            if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            if (r < 0) {
                fprintf(stderr, "bpkg: TLS read failed (-0x%04x)\n", -r);
                return -1;
            }
            return r;
        }
    }
#endif
    return read(c->fd, b, n);
}

static void conn_close(struct conn *c) {
#ifdef B1NIX_HAVE_MBEDTLS
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_x509_crt_free(&c->ca);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->entropy);
        c->tls = 0;
    }
#endif
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

/* Fetches url into *out. Follows simple 30x redirects (a handful of hops). */
static int http_fetch(const char *url, struct buf *out) {
    char cur[2048];
    snprintf(cur, sizeof(cur), "%s", url);
    for (int hop = 0; hop < 5; hop++) {
        char host[256], port[16], path[1536];
        int use_tls = 0;
        if (url_parse(cur, host, sizeof(host), port, sizeof(port), path, sizeof(path), &use_tls) < 0) return -1;

        struct conn c;
        if (conn_open(&c, host, port, use_tls) < 0) return -1;

        char req[1800];
        int n = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: bpkg/1.0 (b1nix)\r\nConnection: close\r\nAccept: */*\r\n\r\n",
                          path, host);
        if (conn_write(&c, req, (size_t)n) != n) { conn_close(&c); return -1; }

        struct buf resp; buf_init(&resp);
        char chunk[4096];
        ssize_t r;
        while ((r = conn_read(&c, chunk, sizeof(chunk))) > 0) buf_append(&resp, chunk, (size_t)r);
        conn_close(&c);

        if (resp.len < 12 || memcmp(resp.p, "HTTP/1.", 7) != 0) {
            fprintf(stderr, "bpkg: bad HTTP response from %s\n", host);
            buf_free(&resp); return -1;
        }
        int status = atoi((char *)resp.p + 9);
        uint8_t *body_start = bpkg_memmem(resp.p, resp.len, "\r\n\r\n", 4);
        if (!body_start) { fprintf(stderr, "bpkg: malformed HTTP headers\n"); buf_free(&resp); return -1; }
        body_start += 4;
        size_t header_len = (size_t)(body_start - resp.p);
        size_t body_len = resp.len - header_len;

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char loc[2048] = "";
            uint8_t *lp = bpkg_memmem(resp.p, header_len, "Location:", 9);
            if (!lp) lp = bpkg_memmem(resp.p, header_len, "location:", 9);
            if (lp) {
                lp += 9;
                while (*lp == ' ') lp++;
                size_t i = 0;
                while (i < sizeof(loc) - 1 && lp[i] != '\r' && lp[i] != '\n') { loc[i] = (char)lp[i]; i++; }
                loc[i] = 0;
            }
            buf_free(&resp);
            if (!loc[0]) { fprintf(stderr, "bpkg: redirect with no Location\n"); return -1; }
            snprintf(cur, sizeof(cur), "%s", loc);
            continue;
        }
        if (status != 200) {
            fprintf(stderr, "bpkg: HTTP %d fetching %s\n", status, cur);
            buf_free(&resp); return -1;
        }

        /* chunked transfer-encoding is not handled; every server we target
         * (jsDelivr, Alpine mirrors) sends Content-Length for static files.
         *
         * That length is checked rather than trusted-by-omission: the body is
         * read until the connection closes, so a transfer cut short arrives as
         * a valid-looking short file. It then fails much later as "not a valid
         * .apk", which sends the reader hunting for a decompression bug that is
         * not there. A truncated download is a download error and says so. */
        {
            uint8_t *lp = bpkg_memmem(resp.p, header_len, "Content-Length:", 15);
            if (!lp) lp = bpkg_memmem(resp.p, header_len, "content-length:", 15);
            if (lp) {
                size_t want = (size_t)strtoul((char *)lp + 15, NULL, 10);
                if (want && body_len < want) {
                    fprintf(stderr,
                            "bpkg: truncated download of %s: %zu of %zu bytes\n",
                            cur, body_len, want);
                    buf_free(&resp);
                    return -1;
                }
            }
        }

        buf_init(out);
        buf_append(out, resp.p + header_len, body_len);
        buf_free(&resp);
        return 0;
    }
    fprintf(stderr, "bpkg: too many redirects fetching %s\n", url);
    return -1;
}

/* file:// support so packages can be staged locally (offline installs,
 * testing) without a network round-trip. */
static int fetch_url(const char *url, struct buf *out) {
    if (strncmp(url, "file://", 7) == 0) {
        const char *path = url + 7;
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "bpkg: cannot open %s: %s\n", path, strerror(errno)); return -1; }
        buf_init(out);
        char chunk[8192];
        size_t r;
        while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) buf_append(out, chunk, r);
        fclose(f);
        return 0;
    }
    /* Retried, because a dropped transfer is worth a second ask: the failure
     * this guards against is a connection cut mid-body, and the next attempt
     * usually completes. Three tries, then the error stands. */
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (http_fetch(url, out) == 0)
            return 0;
        if (attempt < 3)
            fprintf(stderr, "bpkg: retrying %s (attempt %d of 3)\n", url, attempt + 1);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* config + state                                                      */
/* ------------------------------------------------------------------ */

#define BPKG_CONF "/etc/bpkg.conf"
#define BPKG_STATE_DIR "/var/lib/bpkg"
#define BPKG_INSTALLED_DIR "/var/lib/bpkg/installed"
#define BPKG_SCRIPTS_DIR "/var/lib/bpkg/scripts"
/*
 * In the state directory, where the rest of bpkg's state lives.
 *
 * Making it persist is a question about the machine, not about bpkg: a caller
 * that has somewhere durable to keep it can point this path there — see the
 * symlink in /etc/i915-sway.sh. Moving it into the package cache instead would
 * put one kind of state in two places, and the installed-package metadata,
 * which must NOT survive a boot that starts from a fresh ramdisk, sits beside
 * it.
 */
#define BPKG_CACHED_INDEX BPKG_STATE_DIR "/index"
/* Downloaded packages, kept between runs when something is mounted here. */
#define BPKG_CACHE_DIR "/var/cache/bpkg"

/* Read a whole file into `out`. Returns 0 when it existed and was read. */
static int read_whole_file(const char *path, struct buf *out)
{
    FILE *f = fopen(path, "rb");
    char chunk[8192];
    size_t r;

    if (!f)
        return -1;
    buf_init(out);
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf_append(out, chunk, r);
    fclose(f);
    if (out->len == 0) {
        buf_free(out);
        return -1;
    }
    return 0;
}

/*
 * Best-effort: a cache that cannot be written is not an error — but a cache
 * that half-wrote is worse than none, and one that says nothing about it is
 * how a run came to download eighty-six packages into a cache that had kept
 * eight empty names.
 *
 * Written to a temporary name, forced out, then renamed: a reader either finds
 * the whole file or does not find it, and an entry can never be a truncated
 * package that every later run happily reuses.
 */
static void write_whole_file(const char *path, const void *data, size_t len)
{
    char tmp[512];
    FILE *f;
    int fd;

    mkdir("/var/cache", 0755);
    mkdir(BPKG_CACHE_DIR, 0755);
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
        return;
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "bpkg: cache: cannot create %s: %s\n", tmp,
                strerror(errno));
        return;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "bpkg: cache: short write to %s\n", tmp);
        fclose(f);
        unlink(tmp);
        return;
    }
    if (fflush(f) != 0 || (fd = fileno(f)) < 0 || fsync(fd) != 0) {
        fprintf(stderr, "bpkg: cache: cannot flush %s: %s\n", tmp,
                strerror(errno));
        fclose(f);
        unlink(tmp);
        return;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "bpkg: cache: cannot close %s: %s\n", tmp,
                strerror(errno));
        unlink(tmp);
        return;
    }
    if (rename(tmp, path) != 0) {
        fprintf(stderr, "bpkg: cache: cannot rename %s: %s\n", tmp,
                strerror(errno));
        unlink(tmp);
    }
}

/* b1nix is x86_64-only; the 32-bit port is retired. */
#define BPKG_ARCH "x86_64"

#define BPKG_DEFAULT_INDEX_URL "http://dl-cdn.alpinelinux.org/alpine/v3.20/main/" BPKG_ARCH "/APKINDEX.tar.gz"

static void read_conf_index_url(char *out, size_t cap) {
    snprintf(out, cap, "%s", BPKG_DEFAULT_INDEX_URL);
    /* BPKG_INDEX_URL overrides /etc/bpkg.conf -- lets tooling (and the
     * smoke test) point at a second repo without rewriting system config. */
    const char *env = getenv("BPKG_INDEX_URL");
    if (env && env[0]) { snprintf(out, cap, "%s", env); return; }
    FILE *f = fopen(BPKG_CONF, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (strncmp(line, "INDEX_URL=", 10) == 0) {
            char *v = line + 10;
            size_t vlen = strlen(v);
            if (vlen >= 2 && v[0] == '\'' && v[vlen - 1] == '\'') { v[vlen - 1] = 0; v++; }
            snprintf(out, cap, "%s", v);
        }
    }
    fclose(f);
}

/* INDEX_URL may name several repositories, space-separated (Alpine splits
 * its packages across main/ and community/, and a real install pulls from
 * both). Each one is cached in its own file and all of them are parsed into
 * one package set. Repo 0 keeps the historic /var/lib/bpkg/index path so a
 * rootfs seeded by tools/packages/install-ports.sh still loads unchanged. */
#define BPKG_MAX_REPOS 8

static int read_conf_index_urls(char urls[][1536], int max) {
    char all[1536];
    read_conf_index_url(all, sizeof(all));
    int n = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(all, " \t", &saveptr); tok && n < max; tok = strtok_r(NULL, " \t", &saveptr))
        snprintf(urls[n++], 1536, "%s", tok);
    if (n == 0) snprintf(urls[n++], 1536, "%s", BPKG_DEFAULT_INDEX_URL);
    return n;
}

static void repo_cache_path(int i, char *out, size_t cap) {
    if (i == 0) snprintf(out, cap, "%s", BPKG_CACHED_INDEX);
    else snprintf(out, cap, "%s.%d", BPKG_CACHED_INDEX, i);
}

static int is_apkindex_url(const char *url) {
    const char *b = strrchr(url, '/');
    b = b ? b + 1 : url;
    return strcmp(b, "APKINDEX.tar.gz") == 0;
}

static void ensure_state_dirs(void) {
    mkdir(BPKG_STATE_DIR, 0755);
    mkdir(BPKG_INSTALLED_DIR, 0755);
    mkdir(BPKG_SCRIPTS_DIR, 0755);
    /* The cache directory as well: the index lives there now, beside the
     * packages, and `update` writes it with open() rather than through
     * write_whole_file — so nothing else would create it. */
    mkdir("/var/cache", 0755);
    mkdir(BPKG_CACHE_DIR, 0755);
}

/* ------------------------------------------------------------------ */
/* flat-format index: "name version arch sha256 url [deps]"            */
/* ------------------------------------------------------------------ */

struct pkg {
    /* Two full Alpine indexes are ~28k packages, so every byte here is
     * multiplied by 28000: keep the per-package record small enough that
     * `bpkg update` on a real mirror fits in a modest guest. */
    char name[128], version[64], arch[32], sha[80], url[512], deps[1024];
    int is_apk; /* true => url points at a real Alpine .apk (triple-gzip) */
};

/* A "provides" token: the virtual names a package answers to besides its own
 * -- "so:libreadline.so.8", "cmd:bash", "/bin/sh". Alpine's dependencies are
 * mostly stated in these terms (bash depends on so:libreadline.so.8, not on
 * "readline"), so without this table every real package looks unresolvable. */
struct prov {
    char token[160];
    int idx; /* index into pkgset.v */
};

struct pkgset {
    struct pkg *v;
    int n, cap;
    struct prov *p;
    int pn, pcap;
};

static void pkgset_init(struct pkgset *s) { memset(s, 0, sizeof(*s)); }

static void pkgset_add_prov(struct pkgset *s, const char *token, int idx) {
    if (!token[0]) return;
    if (s->pn == s->pcap) {
        s->pcap = s->pcap ? s->pcap * 2 : 256;
        s->p = realloc(s->p, (size_t)s->pcap * sizeof(struct prov));
    }
    snprintf(s->p[s->pn].token, sizeof(s->p[s->pn].token), "%s", token);
    s->p[s->pn].idx = idx;
    s->pn++;
}

static struct pkg *pkgset_add(struct pkgset *s) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->v = realloc(s->v, (size_t)s->cap * sizeof(struct pkg));
    }
    struct pkg *p = &s->v[s->n++];
    memset(p, 0, sizeof(*p));
    return p;
}

/* The flat house format packs every arch into one index file (see the
 * fixture pkgs/index), so a name lookup must prefer the build's own arch --
 * otherwise an image could silently pull down a foreign-arch binary. Real
 * Alpine repos are already arch-scoped by URL (one APKINDEX per arch dir),
 * so their entries either all carry the matching arch or none at all; the
 * fallback to "first match" keeps those working unchanged. */
static struct pkg *pkgset_find(struct pkgset *s, const char *name) {
    struct pkg *fallback = NULL;
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->v[i].name, name) != 0) continue;
        if (!s->v[i].arch[0] || strcmp(s->v[i].arch, BPKG_ARCH) == 0) return &s->v[i];
        if (!fallback) fallback = &s->v[i];
    }
    return fallback;
}

/* Resolves a dependency token to a package: its own name first, then the
 * provides table (so:/cmd:/pc:/path tokens). */
static struct pkg *pkg_resolve(struct pkgset *s, const char *token) {
    struct pkg *pk = pkgset_find(s, token);
    if (pk) return pk;
    for (int i = 0; i < s->pn; i++)
        if (strcmp(s->p[i].token, token) == 0) return &s->v[s->p[i].idx];
    return NULL;
}

/* Names the base image already supplies, one per line in /etc/bpkg.provided
 * (comments with #). Without it, resolving a real Alpine package would drag
 * in Alpine's own musl and busybox and overwrite the running system's libc
 * and shell -- these are dependencies that are genuinely already satisfied,
 * not ones being quietly ignored. */
#define BPKG_PROVIDED_CONF "/etc/bpkg.provided"

static char (*g_provided)[160];
static int g_provided_n;

static void load_base_provided(void) {
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;
    FILE *f = fopen(BPKG_PROVIDED_CONF, "r");
    if (!f) return;
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        if (!*l || *l == '#') continue;
        if (g_provided_n == cap) {
            cap = cap ? cap * 2 : 32;
            g_provided = realloc(g_provided, (size_t)cap * 160);
        }
        snprintf(g_provided[g_provided_n++], 160, "%s", l);
    }
    fclose(f);
}

static int base_provides(const char *token) {
    load_base_provided();
    for (int i = 0; i < g_provided_n; i++)
        if (strcmp(g_provided[i], token) == 0) return 1;
    return 0;
}

static void parse_flat_index(const char *text, size_t len, struct pkgset *out) {
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[2048];
        size_t n = linelen < sizeof(line) - 1 ? linelen : sizeof(line) - 1;
        memcpy(line, p, n); line[n] = 0;
        p = nl ? nl + 1 : end;

        char *l = line;
        while (*l == ' ' || *l == '\t') l++;
        if (!*l || *l == '#') continue;

        char name[128] = "", version[64] = "", arch[32] = "", sha[80] = "", url[512] = "", deps[512] = "";
        int got = sscanf(l, "%127s %63s %31s %79s %511s %511s", name, version, arch, sha, url, deps);
        if (got < 5) continue;
        struct pkg *pk = pkgset_add(out);
        snprintf(pk->name, sizeof(pk->name), "%s", name);
        snprintf(pk->version, sizeof(pk->version), "%s", version);
        snprintf(pk->arch, sizeof(pk->arch), "%s", arch);
        snprintf(pk->sha, sizeof(pk->sha), "%s", sha);
        snprintf(pk->url, sizeof(pk->url), "%s", url);
        snprintf(pk->deps, sizeof(pk->deps), "%s", got >= 6 ? deps : "");
        pk->is_apk = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Alpine APKINDEX parsing                                             */
/* ------------------------------------------------------------------ */

static void parse_apkindex(const char *text, size_t len, const char *base_url, struct pkgset *out) {
    const char *p = text, *end = text + len;
    struct pkg *cur = NULL;

    while (p <= end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char line[1024];
        size_t n = linelen < sizeof(line) - 1 ? linelen : sizeof(line) - 1;
        memcpy(line, p, n); line[n] = 0;
        int last = (nl == NULL);
        p = nl ? nl + 1 : end + 1;

        if (line[0] == 0) { cur = NULL; if (last) break; continue; }
        if (!line[1] || line[1] != ':') { if (last) break; continue; }

        char key = line[0];
        const char *val = line + 2;
        switch (key) {
        case 'P': /* package name */
            cur = pkgset_add(out);
            snprintf(cur->name, sizeof(cur->name), "%s", val);
            cur->is_apk = 1;
            break;
        case 'V':
            if (cur) snprintf(cur->version, sizeof(cur->version), "%s", val);
            break;
        case 'A':
            if (cur) snprintf(cur->arch, sizeof(cur->arch), "%s", val);
            break;
        case 'D': /* space-separated deps, some with version constraints (name>=1.0) */
            if (cur) {
                char tmp[1024] = ""; size_t tl = 0;
                char work[1024];
                snprintf(work, sizeof(work), "%s", val);
                char *saveptr = NULL;
                for (char *tok = strtok_r(work, " ", &saveptr); tok; tok = strtok_r(NULL, " ", &saveptr)) {
                    /* Conflicts (!x) are not dependencies. Everything else is
                     * kept verbatim -- so:/cmd:/pc:/path tokens resolve through
                     * the provides table built from the index's p: lines, or
                     * through /etc/bpkg.provided when the base image already
                     * supplies them (musl, /bin/sh, ...). */
                    if (tok[0] == '!') continue;
                    char depname[160];
                    size_t i = 0;
                    while (tok[i] && tok[i] != '=' && tok[i] != '>' && tok[i] != '<' && tok[i] != '~' && i < sizeof(depname) - 1) {
                        depname[i] = tok[i]; i++;
                    }
                    depname[i] = 0;
                    if (!depname[0]) continue;
                    size_t need = strlen(depname) + (tl ? 1 : 0);
                    if (tl + need >= sizeof(tmp) - 1) continue;
                    if (tl) tmp[tl++] = ',';
                    memcpy(tmp + tl, depname, strlen(depname)); tl += strlen(depname);
                    tmp[tl] = 0;
                }
                snprintf(cur->deps, sizeof(cur->deps), "%s", tmp);
            }
            break;
        case 'p': /* provides: "so:libreadline.so.8=8.2 cmd:bash=5.2.26-r0" */
            if (cur) {
                char work[1024];
                snprintf(work, sizeof(work), "%s", val);
                char *saveptr = NULL;
                for (char *tok = strtok_r(work, " ", &saveptr); tok; tok = strtok_r(NULL, " ", &saveptr)) {
                    char *eq = strchr(tok, '=');
                    if (eq) *eq = 0;
                    if (tok[0]) pkgset_add_prov(out, tok, (int)(cur - out->v));
                }
            }
            break;
        default:
            break;
        }
        if (cur && cur->name[0] && cur->version[0]) {
            snprintf(cur->url, sizeof(cur->url), "%s/%s-%s.apk", base_url, cur->name, cur->version);
        }
        if (last) break;
    }
}

/* strip trailing "/APKINDEX.tar.gz" to get the repo directory */
static void apk_base_url(const char *index_url, char *out, size_t cap) {
    snprintf(out, cap, "%s", index_url);
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
}

/* ------------------------------------------------------------------ */
/* commands                                                             */
/* ------------------------------------------------------------------ */

static int load_one_index(struct pkgset *out, const char *index_url, const char *cache_path) {
    FILE *f = fopen(cache_path, "rb");
    if (!f) {
        fprintf(stderr, "bpkg: no cached index for %s -- run 'bpkg update' first\n", index_url);
        return -1;
    }
    struct buf b; buf_init(&b);
    char chunk[8192]; size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) buf_append(&b, chunk, r);
    fclose(f);

    if (is_apkindex_url(index_url)) {
        struct buf tar; if (gunzip_all(b.p, b.len, &tar) < 0) { buf_free(&b); return -1; }
        /* find the APKINDEX member inside the tar */
        size_t pos = 0; int found = 0;
        while (pos + 512 <= tar.len) {
            const struct tar_hdr *h = (const struct tar_hdr *)(tar.p + pos);
            int allzero = 1;
            for (size_t i = 0; i < 512; i++) if (tar.p[pos + i]) { allzero = 0; break; }
            if (allzero) { pos += 512; continue; }
            long size = tar_octal(h->size, 12);
            pos += 512;
            if (strcmp(h->name, "APKINDEX") == 0) {
                char base[1536]; apk_base_url(index_url, base, sizeof(base));
                parse_apkindex((const char *)(tar.p + pos), (size_t)size, base, out);
                found = 1;
                break;
            }
            pos += ((size_t)size + 511) & ~(size_t)511;
        }
        buf_free(&tar);
        if (!found) { fprintf(stderr, "bpkg: APKINDEX not found inside index tarball\n"); buf_free(&b); return -1; }
    } else {
        parse_flat_index((const char *)b.p, b.len, out);
    }
    buf_free(&b);
    return 0;
}

/* Loads every configured repository into one package set. index_url is
 * filled with the first repo's URL (callers only use it for messages). */
static int load_cached_index(struct pkgset *out, char *index_url, size_t url_cap) {
    char urls[BPKG_MAX_REPOS][1536];
    int nrepo = read_conf_index_urls(urls, BPKG_MAX_REPOS);
    snprintf(index_url, url_cap, "%s", urls[0]);
    pkgset_init(out);
    int loaded = 0;
    for (int i = 0; i < nrepo; i++) {
        char cache[1600];
        repo_cache_path(i, cache, sizeof(cache));
        if (load_one_index(out, urls[i], cache) == 0) loaded++;
    }
    return loaded ? 0 : -1;
}

static int cmd_update(void) {
    ensure_state_dirs();
    char urls[BPKG_MAX_REPOS][1536];
    int nrepo = read_conf_index_urls(urls, BPKG_MAX_REPOS);
    int rc = 0;
    for (int i = 0; i < nrepo; i++) {
        printf("bpkg: fetching index from %s\n", urls[i]);
        struct buf b;
        if (fetch_url(urls[i], &b) < 0) { rc = 1; continue; }
        char cache[1600];
        repo_cache_path(i, cache, sizeof(cache));
        int fd = open(cache, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { fprintf(stderr, "bpkg: cannot write %s: %s\n", cache, strerror(errno)); buf_free(&b); rc = 1; continue; }
        if (write(fd, b.p, b.len) != (ssize_t)b.len) { fprintf(stderr, "bpkg: short write to %s\n", cache); rc = 1; }
        close(fd);
        printf("bpkg: cached %zu bytes\n", b.len);
        buf_free(&b);
    }
    return rc;
}

static int cmd_list(void) {
    DIR *d = opendir(BPKG_INSTALLED_DIR);
    if (!d) { printf("bpkg: no packages installed\n"); return 0; }
    struct dirent *de;
    int any = 0;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l > 4 && strcmp(de->d_name + l - 4, ".ver") == 0) {
            char name[256]; snprintf(name, sizeof(name), "%.*s", (int)(l - 4), de->d_name);
            char verpath[512]; snprintf(verpath, sizeof(verpath), "%s/%s", BPKG_INSTALLED_DIR, de->d_name);
            FILE *f = fopen(verpath, "r");
            char ver[128] = "?";
            if (f) { if (fgets(ver, sizeof(ver), f)) { char *nl = strchr(ver, '\n'); if (nl) *nl = 0; } fclose(f); }
            printf("%s %s\n", name, ver);
            any = 1;
        }
    }
    closedir(d);
    if (!any) printf("bpkg: no packages installed\n");
    return 0;
}

static int cmd_search(const char *term) {
    struct pkgset set; char index_url[1536];
    if (load_cached_index(&set, index_url, sizeof(index_url)) < 0) return 1;
    int found = 0;
    for (int i = 0; i < set.n; i++) {
        if (strstr(set.v[i].name, term)) {
            printf("%-24s %s\n", set.v[i].name, set.v[i].version);
            found++;
        }
    }
    if (!found) printf("bpkg: no matches for '%s'\n", term);
    return 0;
}

static int cmd_info(const char *name) {
    struct pkgset set; char index_url[1536];
    if (load_cached_index(&set, index_url, sizeof(index_url)) < 0) return 1;
    struct pkg *pk = pkgset_find(&set, name);
    if (!pk) { fprintf(stderr, "bpkg: unknown package '%s'\n", name); return 1; }
    printf("name:    %s\nversion: %s\nurl:     %s\ndeps:    %s\nformat:  %s\n",
           pk->name, pk->version, pk->url, pk->deps[0] ? pk->deps : "(none)", pk->is_apk ? "apk" : "flat");
    return 0;
}

/* Install root: "/" for real installs, overridable via BPKG_ROOT so the
 * smoke test can exercise the real pipeline against a scratch directory
 * instead of the live rootfs. Package metadata under /var/lib/bpkg is
 * intentionally NOT root-relative here -- the smoke test symlinks
 * /var/lib/bpkg to its scratch root itself, keeping this binary's on-disk
 * layout identical to a real system. */
static const char *install_root(void) {
    const char *r = getenv("BPKG_ROOT");
    return (r && r[0]) ? r : "/";
}

/* ------------------------------------------------------------------ */
/* .apk integrity: RSA signature + datahash chain                      */
/* ------------------------------------------------------------------ */

/* Returns the first regular file in a decompressed tar: its name, and a
 * pointer into `tar` for its contents. */
static int tar_first_file(const uint8_t *tar, size_t len, char *name, size_t namecap,
                          const uint8_t **content, size_t *clen) {
    size_t pos = 0;
    while (pos + 512 <= len) {
        const struct tar_hdr *h = (const struct tar_hdr *)(tar + pos);
        int allzero = 1;
        for (size_t i = 0; i < 512; i++) if (tar[pos + i]) { allzero = 0; break; }
        if (allzero) { pos += 512; continue; }
        long size = tar_octal(h->size, 12);
        pos += 512;
        if (size < 0 || pos + (size_t)size > len) return -1;
        if (h->typeflag == '0' || h->typeflag == 0) {
            snprintf(name, namecap, "%.100s", h->name);
            *content = tar + pos;
            *clen = (size_t)size;
            return 0;
        }
        pos += ((size_t)size + 511) & ~(size_t)511;
    }
    return -1;
}

/* Finds "key = value" in a .PKGINFO-style text blob. */
static int pkginfo_value(const uint8_t *text, size_t len, const char *key, char *out, size_t cap) {
    size_t klen = strlen(key);
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n') j++;
        if (j - i > klen && memcmp(text + i, key, klen) == 0) {
            size_t v = i + klen;
            while (v < j && (text[v] == ' ' || text[v] == '=')) v++;
            size_t n = j - v;
            if (n >= cap) n = cap - 1;
            memcpy(out, text + v, n); out[n] = 0;
            return 0;
        }
        i = j + 1;
    }
    return -1;
}

#ifdef B1NIX_HAVE_MBEDTLS
#include <mbedtls/md.h>
#include <mbedtls/pk.h>

/* Alpine's own signing keys, shipped with the image (public keys only). */
#define APK_KEYS_DIR "/etc/apk/keys"

/* An .apk carries its integrity in a chain, and this checks the whole chain:
 *
 *   gzip member 1  .SIGN.RSA.<key>  RSA PKCS#1 v1.5 / SHA-1 over the *stored
 *                                   bytes* of member 2
 *   gzip member 2  .PKGINFO         carries datahash = sha256 of the stored
 *                                   bytes of member 3
 *   gzip member 3  the files        what actually gets extracted
 *
 * Verifying only the signature would leave the payload unauthenticated, so
 * both links are required. Returns 0 when the package is trusted. */
static int apk_verify(const uint8_t *sig_tar, size_t sig_tar_len,
                      const uint8_t *ctl_raw, size_t ctl_raw_len,
                      const uint8_t *ctl_tar, size_t ctl_tar_len,
                      const uint8_t *data_raw, size_t data_raw_len) {
    char signame[128];
    const uint8_t *sig = NULL;
    size_t siglen = 0;
    if (tar_first_file(sig_tar, sig_tar_len, signame, sizeof(signame), &sig, &siglen) < 0) {
        fprintf(stderr, "bpkg: .apk signature member is not a readable tar\n");
        return -1;
    }
    /* ".SIGN.RSA.<keyfile>" -- the suffix names the key that signed it. */
    const char *keyname = NULL;
    if (strncmp(signame, ".SIGN.RSA.", 10) == 0) keyname = signame + 10;
    if (!keyname || !keyname[0]) {
        fprintf(stderr, "bpkg: unsupported signature type '%s'\n", signame);
        return -1;
    }

    char keypath[512];
    snprintf(keypath, sizeof(keypath), "%s/%s", APK_KEYS_DIR, keyname);
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_keyfile(&pk, keypath);
    if (rc != 0) {
        fprintf(stderr, "bpkg: package is signed by '%s', which is not a trusted key in %s -- refusing to install\n",
                keyname, APK_KEYS_DIR);
        mbedtls_pk_free(&pk);
        return -1;
    }

    unsigned char sha1[20];
    rc = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), ctl_raw, ctl_raw_len, sha1);
    if (rc == 0)
        rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA1, sha1, sizeof(sha1), sig, siglen);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        fprintf(stderr, "bpkg: signature check FAILED (-0x%04x) -- refusing to install\n", -rc);
        return -1;
    }

    /* The signature covers the control member; the control member commits to
     * the data member through datahash. */
    char pkginfo_name[128];
    const uint8_t *pkginfo = NULL;
    size_t pkginfo_len = 0;
    if (tar_first_file(ctl_tar, ctl_tar_len, pkginfo_name, sizeof(pkginfo_name), &pkginfo, &pkginfo_len) < 0) {
        fprintf(stderr, "bpkg: .apk control member has no .PKGINFO\n");
        return -1;
    }
    char want[80];
    if (pkginfo_value(pkginfo, pkginfo_len, "datahash", want, sizeof(want)) < 0) {
        fprintf(stderr, "bpkg: .PKGINFO carries no datahash -- cannot authenticate the payload\n");
        return -1;
    }
    char got[65];
    sha256_hex(data_raw, data_raw_len, got);
    if (strcasecmp(got, want) != 0) {
        fprintf(stderr, "bpkg: payload does not match the signed datahash (want %s, got %s)\n", want, got);
        return -1;
    }
    printf("bpkg: signature ok (%s), payload matches datahash\n", keyname);
    return 0;
}
#endif /* B1NIX_HAVE_MBEDTLS */

/* ------------------------------------------------------------------ */
/* package scripts, triggers and /etc/apk/world                        */
/* ------------------------------------------------------------------ */

/* Locate a named member in an already-decompressed tar. Alpine's control
 * member is a handful of dot-files (.PKGINFO, .pre-install, .post-install,
 * .trigger, ...), so a linear walk per lookup is cheap and keeps this
 * independent of member order. */
static int tar_find_file(const uint8_t *tar, size_t len, const char *want,
                         const uint8_t **content, size_t *clen) {
    size_t pos = 0;
    while (pos + 512 <= len) {
        const struct tar_hdr *h = (const struct tar_hdr *)(tar + pos);
        int allzero = 1;
        for (size_t i = 0; i < 512; i++) if (tar[pos + i]) { allzero = 0; break; }
        if (allzero) { pos += 512; continue; }
        long size = tar_octal(h->size, 12);
        char name[128];
        snprintf(name, sizeof(name), "%.100s", h->name);
        const char *n = name;
        if (n[0] == '.' && n[1] == '/') n += 2;
        pos += 512;
        if (size < 0 || pos + (size_t)size > len) return -1;
        if ((h->typeflag == '0' || h->typeflag == 0) && strcmp(n, want) == 0) {
            *content = tar + pos;
            *clen = (size_t)size;
            return 0;
        }
        pos += ((size_t)size + 511) & ~(size_t)511;
    }
    return -1;
}

/* Every path installed during this run of bpkg, absolute and newline
 * separated. Triggers are a property of the whole transaction, not of one
 * package: ca-certificates' trigger has to fire when some *other* package
 * drops a certificate into the directory it watches, so what the triggers
 * are matched against is this list, collected across every install. */
static struct buf g_txn_paths;

static void txn_record(const struct buf *filelist) {
    const char *p = (const char *)filelist->p;
    size_t i = 0, start = 0;
    for (; i <= filelist->len; i++) {
        if (i == filelist->len || p[i] == '\n') {
            if (i > start) {
                buf_append(&g_txn_paths, "/", 1);
                buf_append(&g_txn_paths, p + start, i - start);
                buf_append(&g_txn_paths, "\n", 1);
            }
            start = i + 1;
        }
    }
}

/* Write a script out of the control member and make it executable. Returns 0
 * when the script exists and was staged, -1 when the package has none. */
static int stage_script(const uint8_t *ctl_tar, size_t ctl_len, const char *pkgname,
                        const char *which, char *out, size_t outcap) {
    const uint8_t *body = NULL; size_t blen = 0;
    if (!ctl_tar || tar_find_file(ctl_tar, ctl_len, which, &body, &blen) < 0) return -1;
    snprintf(out, outcap, "%s/%s%s", BPKG_SCRIPTS_DIR, pkgname, which);
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        fprintf(stderr, "bpkg: cannot stage %s for %s: %s\n", which, pkgname, strerror(errno));
        return -1;
    }
    if (blen && write(fd, body, blen) != (ssize_t)blen) {
        fprintf(stderr, "bpkg: short write staging %s for %s\n", which, pkgname);
        close(fd);
        unlink(out);
        return -1;
    }
    close(fd);
    chmod(out, 0755);
    return 0;
}

/*
 * Run one staged script.
 *
 * apk chroots into the install root before running these. bpkg deliberately
 * does not: on a real system install_root() is "/", where a chroot would be a
 * no-op anyway, and the scratch root the smoke test installs into holds no
 * shell to chroot to. Instead the script runs with the install root as its
 * working directory and BPKG_ROOT in its environment, which is the same
 * information a chroot would have conveyed and is what the fixture scripts
 * use to place their side effects.
 *
 * A script that fails is reported and its exit status returned; the caller
 * decides. Nothing here is silent.
 */
static int run_script(const char *path, const char *pkgname, const char *which,
                      const char *newver, const char *oldver) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "bpkg: fork for %s%s failed: %s\n", pkgname, which, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        if (chdir(install_root()) != 0) _exit(127);
        setenv("BPKG_ROOT", install_root(), 1);
        char *const argv[] = { (char *)"sh", (char *)path, (char *)newver,
                               (char *)oldver, NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (rc != 0)
        fprintf(stderr, "bpkg: %s%s exited %d\n", pkgname, which, rc);
    else
        printf("bpkg: ran %s%s\n", pkgname, which);
    return rc;
}

/* Stage and immediately run one of the install-time scripts. */
static int run_control_script(const uint8_t *ctl_tar, size_t ctl_len, const char *pkgname,
                              const char *which, const char *newver, const char *oldver) {
    char path[640];
    if (stage_script(ctl_tar, ctl_len, pkgname, which, path, sizeof(path)) < 0)
        return 0; /* no such script: nothing to do, and not an error */
    int rc = run_script(path, pkgname, which, newver, oldver);
    unlink(path);
    return rc;
}

/*
 * Which of the two scripts this phase runs.
 *
 * An upgrade gets its own pair: when the new package ships .pre-upgrade or
 * .post-upgrade, that script runs INSTEAD of the matching install script, and
 * is handed the version being replaced in $2. A package that ships neither --
 * most of them -- has its install scripts run for the upgrade instead, which
 * is why so many Alpine scripts branch on $2 being non-empty.
 *
 * The choice is made per phase, not per package: a package that ships only
 * .post-upgrade gets .pre-install and .post-upgrade, because that is the pair
 * it actually wrote.
 */
static const char *phase_script(const uint8_t *ctl_tar, size_t ctl_len, int upgrading,
                                const char *upgrade_name, const char *install_name) {
    const uint8_t *body = NULL; size_t blen = 0;
    if (!upgrading || !ctl_tar) return install_name;
    if (tar_find_file(ctl_tar, ctl_len, upgrade_name, &body, &blen) < 0) return install_name;
    return upgrade_name;
}

/* Keep .pre-deinstall/.post-deinstall for later: by the time they are needed
 * the .apk they came from is long gone. */
static void keep_deinstall_scripts(const uint8_t *ctl_tar, size_t ctl_len, const char *pkgname) {
    char path[640];
    stage_script(ctl_tar, ctl_len, pkgname, ".pre-deinstall", path, sizeof(path));
    stage_script(ctl_tar, ctl_len, pkgname, ".post-deinstall", path, sizeof(path));
}

/* A package's trigger is a script plus the path patterns that arm it, the
 * patterns coming from .PKGINFO's "triggers = " line. Both are kept on disk
 * so a package installed a month ago still fires when today's install writes
 * into a directory it watches. */
static void record_triggers(const uint8_t *ctl_tar, size_t ctl_len, const char *pkgname) {
    const uint8_t *pkginfo = NULL; size_t plen = 0;
    char patterns[1024];
    char path[640];

    if (!ctl_tar) return;
    if (tar_find_file(ctl_tar, ctl_len, ".PKGINFO", &pkginfo, &plen) < 0) return;
    if (pkginfo_value(pkginfo, plen, "triggers", patterns, sizeof(patterns)) < 0) return;
    if (!patterns[0]) return;
    if (stage_script(ctl_tar, ctl_len, pkgname, ".trigger", path, sizeof(path)) < 0) {
        fprintf(stderr, "bpkg: %s declares triggers but ships no .trigger script\n", pkgname);
        return;
    }
    snprintf(path, sizeof(path), "%s/%s.triggers", BPKG_SCRIPTS_DIR, pkgname);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    dprintf(fd, "%s\n", patterns);
    close(fd);
}

/* True when `abspath`, or the directory holding it, matches `pattern`. Alpine
 * writes its triggers as directory globs -- "/usr/share/applications", or the
 * same with a trailing slash-star -- so both forms have to hit. Returns the
 * directory to hand the trigger script in `dir`. */
static int trigger_matches(const char *pattern, const char *abspath, char *dir, size_t dircap) {
    const char *slash = strrchr(abspath, '/');
    size_t dlen = slash && slash != abspath ? (size_t)(slash - abspath) : 1;
    snprintf(dir, dircap, "%.*s", (int)dlen, abspath);
    if (fnmatch(pattern, abspath, FNM_PATHNAME) == 0) return 1;
    if (fnmatch(pattern, dir, FNM_PATHNAME) == 0) return 1;
    return 0;
}

/* Fire every armed trigger whose patterns match something this transaction
 * installed, once per package, with the matching directories as arguments --
 * which is the interface Alpine's own trigger scripts are written against. */
static void run_pending_triggers(void) {
    if (!g_txn_paths.len) return;

    DIR *d = opendir(BPKG_SCRIPTS_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t nlen = strlen(de->d_name);
        if (nlen <= 9 || strcmp(de->d_name + nlen - 9, ".triggers") != 0) continue;
        char pkgname[128];
        snprintf(pkgname, sizeof(pkgname), "%.*s", (int)(nlen - 9), de->d_name);

        char tpath[640];
        snprintf(tpath, sizeof(tpath), "%s/%s", BPKG_SCRIPTS_DIR, de->d_name);
        struct buf pat;
        if (read_whole_file(tpath, &pat) < 0) continue;
        char patterns[1024];
        size_t n = pat.len < sizeof(patterns) - 1 ? pat.len : sizeof(patterns) - 1;
        memcpy(patterns, pat.p, n); patterns[n] = 0;
        buf_free(&pat);
        char *nl = strchr(patterns, '\n'); if (nl) *nl = 0;

        /* Collect the distinct directories this package's patterns matched. */
        char dirs[16][512]; int ndirs = 0;
        char work[1024]; snprintf(work, sizeof(work), "%s", patterns);
        char *sp = NULL;
        for (char *pt = strtok_r(work, " \t", &sp); pt && ndirs < 16; pt = strtok_r(NULL, " \t", &sp)) {
            const char *p = (const char *)g_txn_paths.p;
            size_t i = 0, start = 0;
            for (; i <= g_txn_paths.len && ndirs < 16; i++) {
                if (i != g_txn_paths.len && p[i] != '\n') continue;
                if (i > start) {
                    char abspath[1024];
                    size_t l = i - start < sizeof(abspath) - 1 ? i - start : sizeof(abspath) - 1;
                    memcpy(abspath, p + start, l); abspath[l] = 0;
                    char dir[512];
                    if (trigger_matches(pt, abspath, dir, sizeof(dir))) {
                        int seen = 0;
                        for (int k = 0; k < ndirs; k++) if (strcmp(dirs[k], dir) == 0) { seen = 1; break; }
                        if (!seen) snprintf(dirs[ndirs++], sizeof(dirs[0]), "%s", dir);
                    }
                }
                start = i + 1;
            }
        }
        if (!ndirs) continue;

        char spath[640];
        snprintf(spath, sizeof(spath), "%s/%s.trigger", BPKG_SCRIPTS_DIR, pkgname);
        if (access(spath, X_OK) != 0) continue;

        pid_t pid = fork();
        if (pid < 0) continue;
        if (pid == 0) {
            char *argv[20];
            int a = 0;
            argv[a++] = (char *)"sh";
            argv[a++] = spath;
            for (int k = 0; k < ndirs && a < 19; k++) argv[a++] = dirs[k];
            argv[a] = NULL;
            if (chdir(install_root()) != 0) _exit(127);
            setenv("BPKG_ROOT", install_root(), 1);
            execv("/bin/sh", argv);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (rc != 0) fprintf(stderr, "bpkg: trigger for %s exited %d\n", pkgname, rc);
        else printf("bpkg: ran trigger for %s (%d path(s))\n", pkgname, ndirs);
    }
    closedir(d);
}

/* ---- /etc/apk/world -------------------------------------------------- */

/* The set of packages somebody asked for by name, as opposed to the ones that
 * came in as dependencies. Alpine keeps it one constraint per line, sorted;
 * so does this, so apk and bpkg can read each other's file. */
static void world_path(char *out, size_t cap) {
    const char *root = install_root();
    if (strcmp(root, "/") == 0) snprintf(out, cap, "/etc/apk/world");
    else snprintf(out, cap, "%s/etc/apk/world", root);
}

static int world_read(char names[][128], int max) {
    char path[640];
    world_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0;
    char line[256];
    while (n < max && fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0]) continue;
        snprintf(names[n++], 128, "%s", line);
    }
    fclose(f);
    return n;
}

static int world_write(char names[][128], int n) {
    /* sort, so the file is stable across runs and diffable */
    for (int i = 1; i < n; i++) {
        char tmp[128]; snprintf(tmp, sizeof(tmp), "%s", names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) { snprintf(names[j + 1], 128, "%s", names[j]); j--; }
        snprintf(names[j + 1], 128, "%s", tmp);
    }
    /* The directories may not exist yet on a fresh root. */
    const char *root = install_root();
    const char *sep = strcmp(root, "/") == 0 ? "" : "/";
    char dir[640];
    snprintf(dir, sizeof(dir), "%s%setc", root, sep);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s%setc/apk", root, sep);
    mkdir(dir, 0755);

    char path[640];
    world_path(path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "bpkg: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    for (int i = 0; i < n; i++) dprintf(fd, "%s\n", names[i]);
    close(fd);
    return 0;
}

static void world_add(const char *name) {
    static char names[512][128];
    int n = world_read(names, 512);
    for (int i = 0; i < n; i++) if (strcmp(names[i], name) == 0) return;
    if (n >= 512) return;
    snprintf(names[n++], 128, "%s", name);
    world_write(names, n);
}

static void world_remove(const char *name) {
    static char names[512][128];
    int n = world_read(names, 512);
    int out = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) continue;
        if (out != i) snprintf(names[out], 128, "%s", names[i]);
        out++;
    }
    if (out != n) world_write(names, out);
}

static int cmd_world(void) {
    static char names[512][128];
    int n = world_read(names, 512);
    for (int i = 0; i < n; i++) printf("%s\n", names[i]);
    return 0;
}

static int already_installed(const char *name, const char *version) {
    char verpath[512]; snprintf(verpath, sizeof(verpath), "%s/%s.ver", BPKG_INSTALLED_DIR, name);
    FILE *f = fopen(verpath, "r");
    if (!f) return 0;
    char cur[128] = "";
    if (fgets(cur, sizeof(cur), f)) { char *nl = strchr(cur, '\n'); if (nl) *nl = 0; }
    fclose(f);
    return strcmp(cur, version) == 0;
}

static int record_install(const char *name, const char *version, struct buf *filelist) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s.list", BPKG_INSTALLED_DIR, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    write(fd, filelist->p, filelist->len);
    close(fd);
    snprintf(p, sizeof(p), "%s/%s.ver", BPKG_INSTALLED_DIR, name);
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    dprintf(fd, "%s\n", version);
    close(fd);
    return 0;
}

/* Install a single package (already resolved in `set`), skipping if the
 * exact version is already recorded. Extracts flat tar.gz directly; for apk,
 * splits the concatenated gzip members and extracts only the LAST member
 * (the data tarball) -- signature and control/.PKGINFO members are decoded
 * (to walk past them) but not installed as files. */
static int install_one(struct pkgset *set, const char *name) {
    struct pkg *pk = pkgset_find(set, name);
    if (!pk) { fprintf(stderr, "bpkg: unknown package '%s'\n", name); return -1; }
    if (already_installed(pk->name, pk->version)) {
        printf("bpkg: %s %s already installed\n", pk->name, pk->version);
        return 0;
    }

    /* Before anything is staged: the scripts directory has to exist by the
     * time a .pre-install is written out, which happens before extraction. */
    ensure_state_dirs();

    /* An install over an older version is an upgrade: the scripts are told so
     * through their second argument, and a package that ships .pre-upgrade or
     * .post-upgrade has those run in place of the install scripts. Empty means
     * a fresh install. The old version's deinstall scripts are deliberately
     * not run -- that is what separates an upgrade from remove-then-install. */
    char oldver[128] = "";
    {
        char vp[512];
        snprintf(vp, sizeof(vp), "%s/%s.ver", BPKG_INSTALLED_DIR, pk->name);
        FILE *vf = fopen(vp, "r");
        if (vf) {
            if (fgets(oldver, sizeof(oldver), vf)) {
                char *nl = strchr(oldver, '\n');
                if (nl) *nl = 0;
            }
            fclose(vf);
        }
    }
    int upgrading = oldver[0] != 0;
    if (upgrading)
        printf("bpkg: upgrading %s %s -> %s\n", pk->name, oldver, pk->version);

    /*
     * A downloaded package is kept.
     *
     * Fetching one costs a TLS handshake and a megabyte over the network, and
     * the same handful gets installed over and over while a system is being
     * brought up — several minutes of every test boot spent re-downloading
     * bytes that have not changed. The cache is keyed by the file name the
     * index gives, which already carries the version, so a new release simply
     * misses and is fetched.
     */
    char cached[512];
    struct buf raw;
    const char *base = strrchr(pk->url, '/');

    snprintf(cached, sizeof(cached), "%s/%s", BPKG_CACHE_DIR,
             base ? base + 1 : pk->name);

    /* Local packages are never cached: they cost nothing to read, and they are
     * how the tamper tests hand bpkg a deliberately corrupted payload — serving
     * an older good copy of the same file name would defeat the check the test
     * exists to make. */
    int cacheable = strncmp(pk->url, "file://", 7) != 0;

    /* Whether these bytes came from the cache decides what a decode failure
     * means: a package that will not decode is either genuinely broken — in
     * which case the mirror will say so too — or a cache entry that was
     * written by a run somebody interrupted. The second is recoverable, and
     * telling them apart costs one re-download. */
    int from_cache = 0;

    if (cacheable && read_whole_file(cached, &raw) == 0) {
        printf("bpkg: using cached %s %s\n", pk->name, pk->version);
        cacheable = 0; /* already there; nothing to write back */
        from_cache = 1;
    } else {
        printf("bpkg: fetching %s %s from %s\n", pk->name, pk->version, pk->url);
        if (fetch_url(pk->url, &raw) < 0) return -1;
    }

    if (!pk->is_apk && pk->sha[0]) {
        char got[65];
        sha256_hex(raw.p, raw.len, got);
        if (strcasecmp(got, pk->sha) != 0) {
            fprintf(stderr, "bpkg: checksum mismatch for %s (index says %s, got %s) -- refusing to install\n",
                    pk->name, pk->sha, got);
            buf_free(&raw);
            return -1;
        }
    }

    struct buf filelist; buf_init(&filelist);
    /* The control member of an .apk, when it has one: where .pre-install,
     * .post-install, the deinstall scripts and .trigger live. */
    const uint8_t *ctl_tar = NULL;
    size_t ctl_len = 0;
    int rc;
    if (!pk->is_apk) {
        struct buf tar;
        rc = gunzip_all(raw.p, raw.len, &tar);
        if (rc == 0) rc = tar_extract(tar.p, tar.len, install_root(), &filelist);
        buf_free(&tar);
    } else {
        /* Split the concatenated gzip members, keeping both the decompressed
         * contents and the stored byte ranges -- the signature is computed
         * over the stored bytes, not the decompressed ones. */
        enum { APK_MAX_MEMBERS = 4 };
        struct buf member[APK_MAX_MEMBERS];
        size_t mstart[APK_MAX_MEMBERS], mend[APK_MAX_MEMBERS];
        int members = 0;
        size_t pos = 0;
        rc = -1;
        while (pos < raw.len && members < APK_MAX_MEMBERS) {
            mstart[members] = pos;
            if (gunzip_one(raw.p, raw.len, &pos, &member[members]) < 0) {
                if (members == 0) fprintf(stderr, "bpkg: %s is not a valid .apk (bad first gzip member)\n", pk->name);
                break;
            }
            mend[members] = pos;
            members++;
        }

        /* Signed .apk files start with a member whose single file is named
         * ".SIGN.<algo>.<key>". Deciding on that name rather than on the
         * member count keeps unsigned local tarballs (the offline fixtures)
         * distinguishable from a real package whose signature went missing. */
        int signed_apk = 0;
        if (members >= 1) {
            char first[128];
            const uint8_t *c = NULL; size_t cl = 0;
            if (tar_first_file(member[0].p, member[0].len, first, sizeof(first), &c, &cl) == 0)
                signed_apk = strncmp(first, ".SIGN.", 6) == 0;
        }

        if (signed_apk) {
            /* signature, control, data — in that order. A signed package that
             * did not decode into all three is corrupt, and must NOT fall back
             * to the unsigned path (that would extract the control tarball and
             * call it success). */
            if (members < 3) {
                fprintf(stderr, "bpkg: %s is signed but only %d of its 3 members decoded -- refusing to install\n",
                        pk->name, members);
                if (from_cache) {
                    /* Almost certainly a truncated cache entry: drop it and
                     * fetch the package again rather than failing the install
                     * over bytes that were never complete. */
                    fprintf(stderr, "bpkg: %s came from the cache; discarding it and refetching\n",
                            pk->name);
                    unlink(cached);
                    for (int mi = 0; mi < members; mi++)
                        buf_free(&member[mi]);
                    buf_free(&raw);
                    buf_free(&filelist);
                    return install_one(set, name);
                }
                rc = -1;
            } else {
#ifdef B1NIX_HAVE_MBEDTLS
                rc = apk_verify(member[0].p, member[0].len,
                                raw.p + mstart[1], mend[1] - mstart[1],
                                member[1].p, member[1].len,
                                raw.p + mstart[2], mend[2] - mstart[2]);
#else
                fprintf(stderr, "bpkg: %s is signed, but this bpkg was built without mbedTLS and cannot check the signature -- refusing to install\n", pk->name);
                rc = -1;
#endif
                if (rc == 0) {
                    ctl_tar = member[1].p;
                    ctl_len = member[1].len;
                    const char *pre = phase_script(ctl_tar, ctl_len, upgrading,
                                                   ".pre-upgrade", ".pre-install");
                    if (run_control_script(ctl_tar, ctl_len, pk->name, pre,
                                           pk->version, oldver) != 0) {
                        fprintf(stderr, "bpkg: %s %s failed -- not installing\n", pk->name, pre);
                        rc = -1;
                    }
                }
                if (rc == 0)
                    rc = tar_extract(member[2].p, member[2].len, install_root(), &filelist);
            }
        } else if (members > 0) {
            /* No signature member. Local (file://) packages are the offline
             * fixtures the smoke test builds with the host's own tar/gzip;
             * anything fetched over the network must be signed. */
            if (strncmp(pk->url, "file://", 7) != 0) {
                fprintf(stderr, "bpkg: %s carries no signature -- refusing to install an unsigned package fetched over the network\n", pk->name);
                rc = -1;
            } else {
                if (members >= 2) {
                    ctl_tar = member[members - 2].p;
                    ctl_len = member[members - 2].len;
                }
                rc = 0;
                const char *pre = phase_script(ctl_tar, ctl_len, upgrading,
                                               ".pre-upgrade", ".pre-install");
                if (run_control_script(ctl_tar, ctl_len, pk->name, pre,
                                       pk->version, oldver) != 0) {
                    fprintf(stderr, "bpkg: %s %s failed -- not installing\n", pk->name, pre);
                    rc = -1;
                }
                if (rc == 0)
                    rc = tar_extract(member[members - 1].p, member[members - 1].len, install_root(), &filelist);
            }
        }
        if (rc == 0 && ctl_tar) {
            /* The package is on disk now, so a failing .post-install is a
             * broken package rather than a reason to unwind: report it, keep
             * the install, and let the exit status carry it. */
            const char *post = phase_script(ctl_tar, ctl_len, upgrading,
                                            ".post-upgrade", ".post-install");
            if (run_control_script(ctl_tar, ctl_len, pk->name, post,
                                   pk->version, oldver) != 0)
                fprintf(stderr, "bpkg: %s installed, but its %s failed\n", pk->name, post);
            keep_deinstall_scripts(ctl_tar, ctl_len, pk->name);
            record_triggers(ctl_tar, ctl_len, pk->name);
        }
        for (int i = 0; i < members; i++) buf_free(&member[i]);
    }
    /* Cached only now: everything above — signature, datahash, extraction — has
     * accepted these bytes, so what lands there is a package that installed
     * cleanly rather than merely one that downloaded. */
    if (cacheable && rc == 0)
        write_whole_file(cached, raw.p, raw.len);
    buf_free(&raw);

    if (rc < 0) { fprintf(stderr, "bpkg: failed to extract %s\n", pk->name); buf_free(&filelist); return -1; }

    record_install(pk->name, pk->version, &filelist);
    txn_record(&filelist);
    buf_free(&filelist);
    printf("bpkg: installed %s %s\n", pk->name, pk->version);
    return 0;
}

static int install_with_deps(struct pkgset *set, const char *name, char visited[][128], int *nvisited, int max_visited) {
    struct pkg *pk = pkg_resolve(set, name);
    if (!pk) { fprintf(stderr, "bpkg: unknown package '%s'\n", name); return -1; }

    for (int i = 0; i < *nvisited; i++) if (strcmp(visited[i], pk->name) == 0) return 0; /* already handled or in progress */
    if (*nvisited >= max_visited) { fprintf(stderr, "bpkg: dependency graph too deep\n"); return -1; }
    snprintf(visited[*nvisited], 128, "%s", pk->name);
    (*nvisited)++;

    if (pk->deps[0]) {
        char deps[1024]; snprintf(deps, sizeof(deps), "%s", pk->deps);
        char *saveptr = NULL;
        for (char *tok = strtok_r(deps, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
            if (base_provides(tok)) continue; /* already in the base image */
            struct pkg *dep = pkg_resolve(set, tok);
            if (!dep) {
                fprintf(stderr, "bpkg: warning: %s depends on unknown package '%s' (skipping dep)\n", name, tok);
                continue;
            }
            if (strcmp(dep->name, pk->name) == 0) continue;
            if (install_with_deps(set, dep->name, visited, nvisited, max_visited) < 0) return -1;
        }
    }
    return install_one(set, pk->name);
}

static int cmd_install(int argc, char **argv) {
    struct pkgset set; char index_url[1536];
    if (load_cached_index(&set, index_url, sizeof(index_url)) < 0) return 1;
    char visited[256][128]; int nvisited = 0;
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        if (install_with_deps(&set, argv[i], visited, &nvisited, 256) < 0) { rc = 1; continue; }
        /* Named on the command line, so it is a member of world -- unlike the
         * dependencies install_with_deps pulled in behind it, which are not. */
        struct pkg *pk = pkg_resolve(&set, argv[i]);
        world_add(pk ? pk->name : argv[i]);
    }
    /* Triggers fire once for the whole transaction, after everything is on
     * disk: a trigger that ran per package would see a half-populated
     * directory and would run again for the next one. */
    run_pending_triggers();
    buf_free(&g_txn_paths);
    return rc;
}

/* Run a deinstall script kept from install time, if the package shipped one. */
static void run_kept_script(const char *name, const char *which) {
    char path[640];
    snprintf(path, sizeof(path), "%s/%s%s", BPKG_SCRIPTS_DIR, name, which);
    if (access(path, X_OK) != 0) return;
    run_script(path, name, which, "", "");
    unlink(path);
}

static int cmd_remove(const char *name) {
    char listpath[512]; snprintf(listpath, sizeof(listpath), "%s/%s.list", BPKG_INSTALLED_DIR, name);
    FILE *f = fopen(listpath, "r");
    if (!f) { fprintf(stderr, "bpkg: %s is not installed\n", name); return 1; }
    run_kept_script(name, ".pre-deinstall");
    char line[1024];
    int removed = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0]) continue;
        char full[1200]; snprintf(full, sizeof(full), "%s/%s", install_root(), line);
        if (unlink(full) == 0) removed++;
    }
    fclose(f);
    unlink(listpath);
    char verpath[512]; snprintf(verpath, sizeof(verpath), "%s/%s.ver", BPKG_INSTALLED_DIR, name);
    unlink(verpath);
    run_kept_script(name, ".post-deinstall");
    /* A removed package's trigger must not keep firing on other packages'
     * installs, and world only lists what is still wanted. */
    char tpath[640];
    snprintf(tpath, sizeof(tpath), "%s/%s.triggers", BPKG_SCRIPTS_DIR, name);
    unlink(tpath);
    snprintf(tpath, sizeof(tpath), "%s/%s.trigger", BPKG_SCRIPTS_DIR, name);
    unlink(tpath);
    world_remove(name);
    printf("bpkg: removed %s (%d files)\n", name, removed);
    return 0;
}

/* ------------------------------------------------------------------ */

static void usage(void) {
    fprintf(stderr,
        "bpkg -- b1nix package manager\n"
        "usage: bpkg update\n"
        "       bpkg list\n"
        "       bpkg search TERM\n"
        "       bpkg info NAME\n"
        "       bpkg install NAME [NAME...]\n"
        "       bpkg remove NAME\n"
        "       bpkg world\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (strcmp(argv[1], "update") == 0) return cmd_update();
    if (strcmp(argv[1], "list") == 0) return cmd_list();
    if (strcmp(argv[1], "search") == 0) { if (argc < 3) { usage(); return 2; } return cmd_search(argv[2]); }
    if (strcmp(argv[1], "info") == 0) { if (argc < 3) { usage(); return 2; } return cmd_info(argv[2]); }
    if (strcmp(argv[1], "install") == 0) { if (argc < 3) { usage(); return 2; } return cmd_install(argc - 2, argv + 2); }
    if (strcmp(argv[1], "remove") == 0) { if (argc < 3) { usage(); return 2; } return cmd_remove(argv[2]); }
    if (strcmp(argv[1], "world") == 0) return cmd_world();
    usage();
    return 2;
}

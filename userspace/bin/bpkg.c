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
 *      data tar with the actual files). We do not verify the RSA
 *      signature (no public-key crypto is linked into this binary) or the
 *      per-file Q1 checksums in APKINDEX -- both are documented gaps, not
 *      silently skipped checks.
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
 *
 * Usage:
 *   bpkg update                    refresh /var/lib/bpkg/index from INDEX_URL
 *   bpkg list                      list installed packages
 *   bpkg search TERM               search the cached index
 *   bpkg install NAME [NAME...]    resolve deps, download, extract
 *   bpkg remove NAME                delete NAME's recorded files
 *   bpkg info NAME                  show cached index metadata for NAME
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

static void safe_mkdirs(const char *root, const char *rel) {
    char path[1024];
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
/* HTTP/1.1 GET over a plain TCP socket (no TLS)                       */
/* ------------------------------------------------------------------ */

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

/* Parses http://host[:port]/path -- that's it. https:// is rejected with a
 * clear error rather than silently falling back to plaintext: this binary
 * has no TLS linked in, so pretending to fetch an https:// URL would be
 * exactly the kind of fake pass the project's testing rules forbid. */
static int url_parse(const char *url, char *host, size_t hostcap, char *port, size_t portcap, char *path, size_t pathcap) {
    const char *p;
    if (strncmp(url, "http://", 7) == 0) p = url + 7;
    else if (strncmp(url, "https://", 8) == 0) {
        fprintf(stderr, "bpkg: https:// not supported (no TLS linked into bpkg) -- use an http mirror or a local file:// index\n");
        return -1;
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
        snprintf(port, portcap, "80");
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

/* Fetches url into *out. Follows simple 30x redirects (a handful of hops). */
static int http_fetch(const char *url, struct buf *out) {
    char cur[2048];
    snprintf(cur, sizeof(cur), "%s", url);
    for (int hop = 0; hop < 5; hop++) {
        char host[256], port[16], path[1536];
        if (url_parse(cur, host, sizeof(host), port, sizeof(port), path, sizeof(path)) < 0) return -1;

        int fd = connect_host(host, port);
        if (fd < 0) { fprintf(stderr, "bpkg: cannot connect to %s:%s\n", host, port); return -1; }

        char req[1800];
        int n = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: bpkg/1.0 (b1nix)\r\nConnection: close\r\nAccept: */*\r\n\r\n",
                          path, host);
        if (write(fd, req, (size_t)n) != n) { close(fd); return -1; }

        struct buf resp; buf_init(&resp);
        char chunk[4096];
        ssize_t r;
        while ((r = read(fd, chunk, sizeof(chunk))) > 0) buf_append(&resp, chunk, (size_t)r);
        close(fd);

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
         * (jsDelivr, Alpine mirrors) sends Content-Length for static files. */
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
    return http_fetch(url, out);
}

/* ------------------------------------------------------------------ */
/* config + state                                                      */
/* ------------------------------------------------------------------ */

#define BPKG_CONF "/etc/bpkg.conf"
#define BPKG_STATE_DIR "/var/lib/bpkg"
#define BPKG_INSTALLED_DIR "/var/lib/bpkg/installed"
#define BPKG_CACHED_INDEX "/var/lib/bpkg/index"

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

static int is_apkindex_url(const char *url) {
    const char *b = strrchr(url, '/');
    b = b ? b + 1 : url;
    return strcmp(b, "APKINDEX.tar.gz") == 0;
}

static void ensure_state_dirs(void) {
    mkdir(BPKG_STATE_DIR, 0755);
    mkdir(BPKG_INSTALLED_DIR, 0755);
}

/* ------------------------------------------------------------------ */
/* flat-format index: "name version arch sha256 url [deps]"            */
/* ------------------------------------------------------------------ */

struct pkg {
    char name[128], version[64], arch[32], sha[80], url[1536], deps[512];
    int is_apk; /* true => url points at a real Alpine .apk (triple-gzip) */
};

struct pkgset {
    struct pkg *v;
    int n, cap;
};

static void pkgset_init(struct pkgset *s) { s->v = NULL; s->n = 0; s->cap = 0; }

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

        char name[128] = "", version[64] = "", arch[32] = "", sha[80] = "", url[1536] = "", deps[512] = "";
        int got = sscanf(l, "%127s %63s %31s %79s %1535s %511s", name, version, arch, sha, url, deps);
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
                char tmp[512] = ""; size_t tl = 0;
                char work[512];
                snprintf(work, sizeof(work), "%s", val);
                char *saveptr = NULL;
                for (char *tok = strtok_r(work, " ", &saveptr); tok; tok = strtok_r(NULL, " ", &saveptr)) {
                    /* skip conflicts (!x), path-provides (/x) and virtual/
                     * soname deps (so:, pc:, cmd:) -- none of those name an
                     * installable package in our index, so treating them as
                     * one would just produce an unresolvable dependency. */
                    if (tok[0] == '!' || tok[0] == '/') continue;
                    if (strncmp(tok, "so:", 3) == 0 || strncmp(tok, "pc:", 3) == 0 || strncmp(tok, "cmd:", 4) == 0) continue;
                    char depname[128];
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

static int load_cached_index(struct pkgset *out, char *index_url, size_t url_cap) {
    read_conf_index_url(index_url, url_cap);
    FILE *f = fopen(BPKG_CACHED_INDEX, "rb");
    if (!f) {
        fprintf(stderr, "bpkg: no cached index -- run 'bpkg update' first\n");
        return -1;
    }
    struct buf b; buf_init(&b);
    char chunk[8192]; size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) buf_append(&b, chunk, r);
    fclose(f);

    pkgset_init(out);
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

static int cmd_update(void) {
    ensure_state_dirs();
    char index_url[1536];
    read_conf_index_url(index_url, sizeof(index_url));
    printf("bpkg: fetching index from %s\n", index_url);
    struct buf b;
    if (fetch_url(index_url, &b) < 0) return 1;
    int fd = open(BPKG_CACHED_INDEX, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "bpkg: cannot write %s: %s\n", BPKG_CACHED_INDEX, strerror(errno)); buf_free(&b); return 1; }
    write(fd, b.p, b.len);
    close(fd);
    printf("bpkg: cached %zu bytes\n", b.len);
    buf_free(&b);
    return 0;
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

    printf("bpkg: fetching %s %s from %s\n", pk->name, pk->version, pk->url);
    struct buf raw;
    if (fetch_url(pk->url, &raw) < 0) return -1;

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
    int rc;
    if (!pk->is_apk) {
        struct buf tar;
        rc = gunzip_all(raw.p, raw.len, &tar);
        if (rc == 0) rc = tar_extract(tar.p, tar.len, install_root(), &filelist);
        buf_free(&tar);
    } else {
        size_t pos = 0;
        struct buf member;
        int members = 0;
        rc = -1;
        while (pos < raw.len) {
            if (gunzip_one(raw.p, raw.len, &pos, &member) < 0) {
                if (members == 0) fprintf(stderr, "bpkg: %s is not a valid .apk (bad first gzip member)\n", pk->name);
                break;
            }
            members++;
            if (pos >= raw.len) /* this was the last member: it's the data tarball */
                rc = tar_extract(member.p, member.len, install_root(), &filelist);
            buf_free(&member);
        }
        if (members == 0) rc = -1;
    }
    buf_free(&raw);

    if (rc < 0) { fprintf(stderr, "bpkg: failed to extract %s\n", pk->name); buf_free(&filelist); return -1; }

    ensure_state_dirs();
    record_install(pk->name, pk->version, &filelist);
    buf_free(&filelist);
    printf("bpkg: installed %s %s\n", pk->name, pk->version);
    return 0;
}

static int install_with_deps(struct pkgset *set, const char *name, char visited[][128], int *nvisited, int max_visited) {
    for (int i = 0; i < *nvisited; i++) if (strcmp(visited[i], name) == 0) return 0; /* already handled or in progress */
    if (*nvisited >= max_visited) { fprintf(stderr, "bpkg: dependency graph too deep\n"); return -1; }
    snprintf(visited[*nvisited], 128, "%s", name);
    (*nvisited)++;

    struct pkg *pk = pkgset_find(set, name);
    if (!pk) { fprintf(stderr, "bpkg: unknown package '%s'\n", name); return -1; }

    if (pk->deps[0]) {
        char deps[512]; snprintf(deps, sizeof(deps), "%s", pk->deps);
        char *saveptr = NULL;
        for (char *tok = strtok_r(deps, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
            if (!pkgset_find(set, tok)) {
                fprintf(stderr, "bpkg: warning: %s depends on unknown package '%s' (skipping dep)\n", name, tok);
                continue;
            }
            if (install_with_deps(set, tok, visited, nvisited, max_visited) < 0) return -1;
        }
    }
    return install_one(set, name);
}

static int cmd_install(int argc, char **argv) {
    struct pkgset set; char index_url[1536];
    if (load_cached_index(&set, index_url, sizeof(index_url)) < 0) return 1;
    char visited[256][128]; int nvisited = 0;
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        if (install_with_deps(&set, argv[i], visited, &nvisited, 256) < 0) rc = 1;
    }
    return rc;
}

static int cmd_remove(const char *name) {
    char listpath[512]; snprintf(listpath, sizeof(listpath), "%s/%s.list", BPKG_INSTALLED_DIR, name);
    FILE *f = fopen(listpath, "r");
    if (!f) { fprintf(stderr, "bpkg: %s is not installed\n", name); return 1; }
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
        "       bpkg remove NAME\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (strcmp(argv[1], "update") == 0) return cmd_update();
    if (strcmp(argv[1], "list") == 0) return cmd_list();
    if (strcmp(argv[1], "search") == 0) { if (argc < 3) { usage(); return 2; } return cmd_search(argv[2]); }
    if (strcmp(argv[1], "info") == 0) { if (argc < 3) { usage(); return 2; } return cmd_info(argv[2]); }
    if (strcmp(argv[1], "install") == 0) { if (argc < 3) { usage(); return 2; } return cmd_install(argc - 2, argv + 2); }
    if (strcmp(argv[1], "remove") == 0) { if (argc < 3) { usage(); return 2; } return cmd_remove(argv[2]); }
    usage();
    return 2;
}

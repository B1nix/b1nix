/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AML interpreter — the bytecode in the DSDT and the SSDTs (M134).
 *
 * See kernel/include/b1nix/aml.h for what this is and is not. In one line:
 * acpi.c reads the tables that are structs, this one runs the table that is a
 * program.
 *
 * Structure, in the order the file is written:
 *
 *   1. objects            — refcounted values (integer, string, buffer,
 *                           package, field unit, region, method, reference)
 *   2. the namespace      — four-character segments in a tree, and the ACPI
 *                           name-resolution rules (root, carats, search-up)
 *   3. the byte stream    — PkgLength, NameString, opcode fetch
 *   4. skipping           — walking past a term without running it, which the
 *                           load pass needs for everything that is not a
 *                           declaration
 *   5. operation regions  — the only place this kernel touches hardware on the
 *                           firmware's instruction, and the only place it
 *                           REFUSES to (PCI config, EC, SMBus, CMOS)
 *   6. the evaluator      — one switch over the opcodes
 *   7. the loader         — DSDT + every SSDT, then the predefined roots
 *   8. the public API
 *
 * Serialization: one spinlock around every evaluation. Nothing here sleeps
 * (Sleep and Stall are bounded busy waits, see op_sleep), nothing here takes
 * another subsystem's lock except the heap's and, once per region, the page
 * tables' — so the lock has no order to violate.
 */

#include <b1nix/acpi.h>
#include <b1nix/aml.h>
#include <b1nix/kprintf.h>
#include <b1nix/ktime.h>
#include <b1nix/mm.h>
#include <b1nix/spinlock.h>
#include <b1nix/types.h>
#include <stdio.h>
#include <string.h>

#if defined(__x86_64__)
#include <b1nix/io.h>
#endif

/* ────────────────────────────────────────────────────────────────────────
 * 1. Objects
 * ──────────────────────────────────────────────────────────────────────── */

struct aml_node;

struct aml_obj {
    u8  type;
    int ref;
    union {
        u64 integer;
        /* String and Buffer share this. A string's bytes are NUL-terminated
         * past `len` so they can be printed; a buffer's are not. */
        struct { u8 *p; u32 len; } buf;
        struct { struct aml_obj **e; u32 n; } pkg;
        struct { const u8 *body; u32 len; u8 nargs; u8 builtin; } method;
        struct { u8 space; u8 dead; u64 base; u64 len; volatile u8 *va; } region;
        struct {
            struct aml_obj *region;     /* AML_T_REGION, owned */
            struct aml_obj *index_fld;  /* IndexField: the index unit, owned */
            struct aml_obj *data_fld;   /* IndexField: the data unit, owned */
            u32 bit_off, bit_len;
            u8  flags;
        } fld;
        struct { struct aml_obj *buf; u32 bit_off, bit_len; } bfld;
        /* A reference: to a namespace node, or into a package or buffer. */
        struct {
            u8 kind;
            struct aml_node *node;
            struct aml_obj *obj;        /* owned when set */
            u32 index;
        } ref;
        struct { int held; u8 level; } mutex;
    } u;
};

#define REF_NODE      1
#define REF_PKG_ELEM  2
#define REF_BUF_BYTE  3

struct aml_node {
    char seg[4];                        /* never NUL-terminated */
    struct aml_node *parent, *child, *next;
    struct aml_obj  *val;               /* owned; never NULL after creation */
};

static struct aml_node *g_root;
static u32   g_objects;
static u32   g_typecount[AML_T_SCOPE + 1];
static u32   g_skipped;
static int   g_ready;
static int   g_int_bits = 64;           /* 32 for a revision-1 DSDT */
static spinlock_t g_aml_lock = SPINLOCK_INIT;

/* Which address spaces this interpreter refused, one bit per space, so the
 * boot log and /proc can say so once instead of per access. */
static u32 g_refused_spaces;

static struct aml_obj *obj_new(u8 type) {
    struct aml_obj *o = kzalloc(sizeof(*o));
    if (!o)
        return 0;
    o->type = type;
    o->ref = 1;
    return o;
}

static void aml_unref(struct aml_obj *o);

static struct aml_obj *aml_ref(struct aml_obj *o) {
    if (o)
        o->ref++;
    return o;
}

static void aml_unref(struct aml_obj *o) {
    if (!o)
        return;
    if (--o->ref > 0)
        return;
    switch (o->type) {
    case AML_T_STRING:
    case AML_T_BUFFER:
        kfree(o->u.buf.p);
        break;
    case AML_T_PACKAGE:
        for (u32 i = 0; i < o->u.pkg.n; i++)
            aml_unref(o->u.pkg.e[i]);
        kfree(o->u.pkg.e);
        break;
    case AML_T_FIELD:
        aml_unref(o->u.fld.region);
        aml_unref(o->u.fld.index_fld);
        aml_unref(o->u.fld.data_fld);
        break;
    case AML_T_BUFFER_FIELD:
        aml_unref(o->u.bfld.buf);
        break;
    case AML_T_REF:
        aml_unref(o->u.ref.obj);
        break;
    default:
        break;
    }
    kfree(o);
}

static struct aml_obj *obj_int(u64 v) {
    struct aml_obj *o = obj_new(AML_T_INTEGER);
    if (o)
        o->u.integer = v;
    return o;
}

static struct aml_obj *obj_buf(u32 len, int string) {
    struct aml_obj *o = obj_new(string ? AML_T_STRING : AML_T_BUFFER);
    if (!o)
        return 0;
    /* One extra byte so a string is always printable and a zero-length
     * buffer still has an allocation to point at. */
    o->u.buf.p = kzalloc(len + 1);
    if (!o->u.buf.p) {
        kfree(o);
        return 0;
    }
    o->u.buf.len = len;
    return o;
}

static struct aml_obj *obj_str(const char *s) {
    u32 n = (u32)strlen(s);
    struct aml_obj *o = obj_buf(n, 1);
    if (o)
        memcpy(o->u.buf.p, s, n);
    return o;
}

static struct aml_obj *obj_pkg(u32 n) {
    struct aml_obj *o = obj_new(AML_T_PACKAGE);
    if (!o)
        return 0;
    o->u.pkg.e = kzalloc(sizeof(struct aml_obj *) * (n ? n : 1));
    if (!o->u.pkg.e) {
        kfree(o);
        return 0;
    }
    o->u.pkg.n = n;
    return o;
}

/* The integer mask this table's revision asks for: a revision-1 DSDT is a
 * 32-bit table, and an interpreter that quietly gave it 64-bit arithmetic
 * would return a different answer than the firmware author tested. */
static u64 int_mask(void) {
    return g_int_bits >= 64 ? ~0ULL : 0xFFFFFFFFULL;
}

static u64 obj_as_int(const struct aml_obj *o) {
    if (!o)
        return 0;
    switch (o->type) {
    case AML_T_INTEGER:
        return o->u.integer;
    case AML_T_BUFFER: {
        u64 v = 0;
        u32 n = o->u.buf.len;
        if (n > 8)
            n = 8;
        for (u32 i = 0; i < n; i++)
            v |= ((u64)o->u.buf.p[i]) << (i * 8);
        return v & int_mask();
    }
    case AML_T_STRING: {
        /* ToInteger's rule: a leading "0x" is hex, otherwise decimal. */
        const char *s = (const char *)o->u.buf.p;
        u64 v = 0;
        u32 i = 0;
        if (o->u.buf.len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            for (i = 2; i < o->u.buf.len; i++) {
                char ch = s[i];
                u64 d;
                if (ch >= '0' && ch <= '9')      d = (u64)(ch - '0');
                else if (ch >= 'a' && ch <= 'f') d = (u64)(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F') d = (u64)(ch - 'A' + 10);
                else break;
                v = v * 16 + d;
            }
        } else {
            for (i = 0; i < o->u.buf.len; i++) {
                if (s[i] < '0' || s[i] > '9')
                    break;
                v = v * 10 + (u64)(s[i] - '0');
            }
        }
        return v & int_mask();
    }
    default:
        return 0;
    }
}

/* ────────────────────────────────────────────────────────────────────────
 * 2. The namespace
 * ──────────────────────────────────────────────────────────────────────── */

static void seg_copy(char *dst, const char *src) {
    for (int i = 0; i < 4; i++)
        dst[i] = src[i];
}

static int seg_eq(const char *a, const char *b) {
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static struct aml_node *ns_child(struct aml_node *p, const char *seg) {
    for (struct aml_node *n = p ? p->child : 0; n; n = n->next)
        if (seg_eq(n->seg, seg))
            return n;
    return 0;
}

static struct aml_node *ns_add(struct aml_node *p, const char *seg, u8 type) {
    struct aml_node *n = ns_child(p, seg);
    if (n)
        return n;
    n = kzalloc(sizeof(*n));
    if (!n)
        return 0;
    seg_copy(n->seg, seg);
    n->val = obj_new(type);
    if (!n->val) {
        kfree(n);
        return 0;
    }
    n->parent = p;
    /* Append at the tail so a walk reports the namespace in the order the
     * firmware declared it — which is what makes a /proc listing comparable
     * with a disassembly. */
    if (p) {
        struct aml_node **tail = &p->child;
        while (*tail)
            tail = &(*tail)->next;
        *tail = n;
    }
    g_objects++;
    if (type <= AML_T_SCOPE)
        g_typecount[type]++;
    return n;
}

static void node_set(struct aml_node *n, struct aml_obj *v) {
    if (!n || !v)
        return;
    if (n->val) {
        if (n->val->type <= AML_T_SCOPE && g_typecount[n->val->type])
            g_typecount[n->val->type]--;
        aml_unref(n->val);
    }
    n->val = v;
    if (v->type <= AML_T_SCOPE)
        g_typecount[v->type]++;
}

/* A parsed NameString. */
struct aml_path {
    u8   root;
    u8   carats;
    u8   nsegs;
    char seg[8][4];
};

static struct aml_node *ns_walk_up(struct aml_node *scope, int carats) {
    while (carats-- > 0 && scope && scope->parent)
        scope = scope->parent;
    return scope;
}

/* Resolve a parsed name. `create` makes every segment that is missing, which
 * is what a declaration needs; without it a missing segment is a miss.
 * The ACPI search-up rule applies only to a single unprefixed segment. */
static struct aml_node *ns_resolve(struct aml_node *scope,
                                   const struct aml_path *np, int create) {
    struct aml_node *base;

    if (np->nsegs == 0)
        return np->root ? g_root : ns_walk_up(scope, np->carats);

    if (np->root)
        base = g_root;
    else
        base = ns_walk_up(scope, np->carats);
    if (!base)
        return 0;

    if (!create && !np->root && np->carats == 0 && np->nsegs == 1) {
        for (struct aml_node *s = base; s; s = s->parent) {
            struct aml_node *n = ns_child(s, np->seg[0]);
            if (n)
                return n;
        }
        return 0;
    }

    struct aml_node *cur = base;
    for (int i = 0; i < np->nsegs; i++) {
        struct aml_node *n = ns_child(cur, np->seg[i]);
        if (!n) {
            if (!create)
                return 0;
            /* An intermediate segment the firmware never declared: make it a
             * scope rather than dropping the declaration on the floor, and
             * count it so /proc can show the namespace was patched up. */
            n = ns_add(cur, np->seg[i], AML_T_SCOPE);
            if (!n)
                return 0;
        }
        cur = n;
    }
    return cur;
}

/* Absolute path of a node, "\\_SB_.PCI0" style. Returns the length written. */
static int ns_path(const struct aml_node *n, char *out, usize cap) {
    const struct aml_node *chain[32];
    int depth = 0;
    while (n && n->parent && depth < 32) {
        chain[depth++] = n;
        n = n->parent;
    }
    usize len = 0;
    if (cap < 2)
        return 0;
    out[len++] = '\\';
    for (int i = depth - 1; i >= 0; i--) {
        if (len + 5 >= cap)
            break;
        if (i != depth - 1)
            out[len++] = '.';
        for (int k = 0; k < 4; k++)
            out[len++] = chain[i]->seg[k];
    }
    out[len] = '\0';
    return (int)len;
}

/* ────────────────────────────────────────────────────────────────────────
 * 3. The byte stream
 * ──────────────────────────────────────────────────────────────────────── */

#define CTRL_NONE      0
#define CTRL_RETURN    1
#define CTRL_BREAK     2
#define CTRL_CONTINUE  3

#define AML_MAX_DEPTH  24

struct aml_ctx {
    const u8 *p, *end;
    struct aml_node *scope;
    struct aml_obj  *arg[7];
    struct aml_obj  *loc[8];
    struct aml_obj  *ret;
    int   ctrl;
    int   depth;
    int   err;
    int   load;                 /* 1 while building the namespace */
    u64  *budget;               /* shared step budget, so a runaway method or
                                 * a While that never falls out cannot hang
                                 * the machine */
};

static int at_end(struct aml_ctx *c) { return c->p >= c->end; }

static u8 u8_at(struct aml_ctx *c) {
    if (at_end(c)) {
        c->err = AML_EBADCODE;
        return 0;
    }
    return *c->p++;
}

static u64 uint_le(struct aml_ctx *c, int bytes) {
    u64 v = 0;
    for (int i = 0; i < bytes; i++)
        v |= ((u64)u8_at(c)) << (i * 8);
    return v;
}

/* PkgLength: the count includes its own encoding bytes. Returns the address
 * one past the package. */
static const u8 *pkg_end(struct aml_ctx *c) {
    const u8 *start = c->p;
    u8 lead = u8_at(c);
    if (c->err)
        return c->end;
    u32 follow = (u32)(lead >> 6);
    u32 len;
    if (follow == 0) {
        len = lead & 0x3F;
    } else {
        len = lead & 0x0F;
        for (u32 i = 0; i < follow; i++)
            len |= ((u32)u8_at(c)) << (4 + i * 8);
    }
    const u8 *e = start + len;
    if (e < c->p || e > c->end) {
        c->err = AML_EBADCODE;
        return c->end;
    }
    return e;
}

static int is_lead_name_char(u8 ch) {
    return (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static int starts_name(u8 ch) {
    return is_lead_name_char(ch) || ch == '\\' || ch == '^' || ch == 0x2E ||
           ch == 0x2F;
}

static int parse_name(struct aml_ctx *c, struct aml_path *np) {
    memset(np, 0, sizeof(*np));
    while (!at_end(c) && *c->p == '^') {
        c->p++;
        if (np->carats < 255)
            np->carats++;
    }
    if (!at_end(c) && *c->p == '\\') {
        c->p++;
        np->root = 1;
    }
    if (at_end(c)) {
        c->err = AML_EBADCODE;
        return c->err;
    }
    u8 lead = *c->p;
    int count;
    if (lead == 0x00) {                 /* NullName */
        c->p++;
        return AML_OK;
    } else if (lead == 0x2E) {          /* DualNamePrefix */
        c->p++;
        count = 2;
    } else if (lead == 0x2F) {          /* MultiNamePrefix */
        c->p++;
        count = (int)u8_at(c);
    } else if (is_lead_name_char(lead)) {
        count = 1;
    } else {
        c->err = AML_EBADCODE;
        return c->err;
    }
    if (count < 0 || count > 8) {
        c->err = AML_EBADCODE;
        return c->err;
    }
    for (int i = 0; i < count; i++) {
        if (c->p + 4 > c->end) {
            c->err = AML_EBADCODE;
            return c->err;
        }
        seg_copy(np->seg[i], (const char *)c->p);
        c->p += 4;
    }
    np->nsegs = (u8)count;
    return AML_OK;
}

/* Opcodes. Extended ones are returned as 0x5B00 | second byte. */
#define EXT(x) (0x5B00 | (x))

static int fetch_op(struct aml_ctx *c) {
    if (at_end(c))
        return -1;
    u8 b = *c->p++;
    if (b == 0x5B) {
        if (at_end(c)) {
            c->err = AML_EBADCODE;
            return -1;
        }
        return (int)EXT(*c->p++);
    }
    return (int)b;
}

/* Does this opcode carry a PkgLength right after itself? Everything that does
 * can be skipped without understanding a byte of its body, which is what
 * makes the load pass safe on firmware full of constructs this interpreter
 * has never seen. */
static int op_has_pkglen(int op) {
    switch (op) {
    case 0x10:            /* Scope */
    case 0x11:            /* Buffer */
    case 0x12:            /* Package */
    case 0x13:            /* VarPackage */
    case 0x14:            /* Method */
    case 0xA0:            /* If */
    case 0xA1:            /* Else */
    case 0xA2:            /* While */
    case EXT(0x81):       /* Field */
    case EXT(0x82):       /* Device */
    case EXT(0x83):       /* Processor */
    case EXT(0x84):       /* PowerResource */
    case EXT(0x85):       /* ThermalZone */
    case EXT(0x86):       /* IndexField */
    case EXT(0x87):       /* BankField */
        return 1;
    default:
        return 0;
    }
}

/* ────────────────────────────────────────────────────────────────────────
 * 4. Skipping a term
 * ──────────────────────────────────────────────────────────────────────── */

static int skip_term(struct aml_ctx *c);

static int skip_target(struct aml_ctx *c) {
    if (!at_end(c) && *c->p == 0x00) {  /* NullName — no target */
        c->p++;
        return AML_OK;
    }
    return skip_term(c);
}

static int method_argc(struct aml_ctx *c, const struct aml_path *np) {
    struct aml_node *n = ns_resolve(c->scope, np, 0);
    if (n && n->val && n->val->type == AML_T_METHOD)
        return n->val->u.method.nargs;
    return n ? 0 : -1;
}

static int skip_args(struct aml_ctx *c, int n) {
    for (int i = 0; i < n && !c->err; i++)
        skip_term(c);
    return c->err;
}

static int skip_term(struct aml_ctx *c) {
    if (c->err || at_end(c))
        return c->err;
    if (++c->depth > AML_MAX_DEPTH) {
        c->depth--;
        c->err = AML_EDEPTH;
        return c->err;
    }
    int op = fetch_op(c);
    if (op < 0) {
        c->depth--;
        return c->err;
    }

    if (op_has_pkglen(op)) {
        const u8 *e = pkg_end(c);
        if (!c->err)
            c->p = e;
        c->depth--;
        return c->err;
    }

    switch (op) {
    case 0x00: case 0x01: case 0xFF:                    /* Zero, One, Ones */
    case 0xA3: case 0xA5: case 0x9F: case 0xCC:         /* Noop Break Cont BP */
    case EXT(0x31):                                     /* Debug */
    case EXT(0x33):                                     /* Timer */
    case EXT(0x30):                                     /* Revision */
        break;
    case 0x0A: c->p += 1; break;
    case 0x0B: c->p += 2; break;
    case 0x0C: c->p += 4; break;
    case 0x0E: c->p += 8; break;
    case 0x0D:                                          /* String */
        while (!at_end(c) && *c->p)
            c->p++;
        if (!at_end(c))
            c->p++;
        break;
    case 0x08: {                                        /* Name */
        struct aml_path np;
        parse_name(c, &np);
        skip_term(c);
        break;
    }
    case 0x06: {                                        /* Alias */
        struct aml_path np;
        parse_name(c, &np);
        parse_name(c, &np);
        break;
    }
    case 0x15: {                                        /* External */
        struct aml_path np;
        parse_name(c, &np);
        c->p += 2;
        break;
    }
    case EXT(0x80): {                                   /* OperationRegion */
        struct aml_path np;
        parse_name(c, &np);
        c->p += 1;
        skip_args(c, 2);
        break;
    }
    case EXT(0x01): {                                   /* Mutex */
        struct aml_path np;
        parse_name(c, &np);
        c->p += 1;
        break;
    }
    case EXT(0x02): {                                   /* Event */
        struct aml_path np;
        parse_name(c, &np);
        break;
    }
    /* Local0-7, Arg0-6 */
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E:
        break;
    /* one term + one target */
    case 0x70:                                          /* Store */
    case 0x80: case 0x81: case 0x82:                    /* Not, FindSetL/R */
    case 0x96: case 0x97: case 0x98: case 0x99:         /* To{Buffer,Dec,Hex,Int} */
    case EXT(0x28): case EXT(0x29):                     /* From/ToBCD */
        skip_args(c, 1);
        skip_target(c);
        break;
    /* two terms + one target */
    case 0x72: case 0x73: case 0x74: case 0x77:
    case 0x79: case 0x7A: case 0x7B: case 0x7C:
    case 0x7D: case 0x7E: case 0x7F: case 0x85:
    case 0x88: case 0x9C: case 0x84:
        skip_args(c, 2);
        skip_target(c);
        break;
    case 0x78:                                          /* Divide */
        skip_args(c, 2);
        skip_target(c);
        skip_target(c);
        break;
    case 0x9E:                                          /* Mid */
        skip_args(c, 3);
        skip_target(c);
        break;
    /* pure expressions */
    case 0x90: case 0x91: case 0x93: case 0x94: case 0x95:
        skip_args(c, 2);
        break;
    case 0x92: case 0x83: case 0xA4:                    /* LNot DerefOf Return */
    case EXT(0x21): case EXT(0x22):                     /* Stall, Sleep */
        skip_args(c, 1);
        break;
    /* SuperName only */
    case 0x71: case 0x75: case 0x76: case 0x87: case 0x8E:
    case EXT(0x26): case EXT(0x27):                     /* Reset, Release */
    case EXT(0x24): case EXT(0x2A):                     /* Signal, Unload */
        skip_term(c);
        break;
    case 0x86:                                          /* Notify */
    case EXT(0x25):                                     /* Wait */
        skip_term(c);
        skip_args(c, 1);
        break;
    case EXT(0x12):                                     /* CondRefOf */
        skip_term(c);
        skip_target(c);
        break;
    case 0x9D:                                          /* CopyObject */
        skip_args(c, 1);
        skip_term(c);
        break;
    case EXT(0x23):                                     /* Acquire */
        skip_term(c);
        c->p += 2;
        break;
    case 0x89:                                          /* Match */
        skip_args(c, 1);
        c->p += 1;
        skip_args(c, 1);
        c->p += 1;
        skip_args(c, 2);
        break;
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F:
    case EXT(0x13): {                                   /* CreateXField */
        struct aml_path np;
        skip_args(c, op == (int)EXT(0x13) ? 3 : 2);
        parse_name(c, &np);
        break;
    }
    case EXT(0x32): {                                   /* Fatal */
        c->p += 5;
        skip_args(c, 1);
        break;
    }
    default:
        if (op < 0x100 && starts_name((u8)op)) {
            /* Back up over the opcode byte: it was the first byte of a name. */
            c->p--;
            struct aml_path np;
            if (parse_name(c, &np) == AML_OK) {
                int n = method_argc(c, &np);
                if (n < 0) {
                    /* Unknown name in a call position. Assume it names an
                     * object, not a method: every real table declares its
                     * methods (or an External for them) before use, so this
                     * is the forward reference to a Name. */
                    n = 0;
                }
                skip_args(c, n);
            }
        } else {
            c->err = AML_EBADCODE;
        }
        break;
    }
    c->depth--;
    return c->err;
}

/* ────────────────────────────────────────────────────────────────────────
 * 5. Operation regions
 * ──────────────────────────────────────────────────────────────────────── */

const char *aml_region_space_name(u8 space) {
    switch (space) {
    case AML_SPACE_MEMORY:  return "SystemMemory";
    case AML_SPACE_IO:      return "SystemIO";
    case AML_SPACE_PCI_CFG: return "PCI_Config";
    case AML_SPACE_EC:      return "EmbeddedControl";
    case AML_SPACE_SMBUS:   return "SMBus";
    case AML_SPACE_CMOS:    return "CMOS";
    default:                return "vendor";
    }
}

/* Map a SystemMemory region once, on first access. Small regions below the
 * direct map are just an offset; anything else gets its own MMIO mapping. */
static int region_map(struct aml_obj *r) {
    if (r->u.region.va || r->u.region.dead)
        return r->u.region.dead ? AML_EREGION : AML_OK;
    u64 base = r->u.region.base, len = r->u.region.len;
    if (len == 0 || len > (1u << 20)) {
        r->u.region.dead = 1;
        return AML_EREGION;
    }
    if (base + len <= DIRECT_MAP_SIZE) {
        r->u.region.va = (volatile u8 *)(usize)(DIRECT_MAP_BASE + base);
        return AML_OK;
    }
    void *va = vmm_map_mmio(base, (usize)len, VMM_PRESENT | VMM_WRITABLE);
    if (!va) {
        r->u.region.dead = 1;
        return AML_EREGION;
    }
    r->u.region.va = (volatile u8 *)va;
    return AML_OK;
}

/*
 * One aligned access of 8, 16, 32 or 64 bits inside a region.
 *
 * Only SystemMemory and SystemIO are honoured. PCI config space needs the
 * device's own _ADR walked back up the namespace to a segment/bus/device, an
 * embedded controller needs its command/data ports and the IBF/OBF handshake
 * with an interrupt behind it, SMBus needs a host controller driver, and CMOS
 * needs the RTC's index/data pair arbitrated with kernel/dev/rtc_dev.c. None
 * of those exist here, so an access to one is REFUSED — it returns an error
 * that propagates out of the evaluation. It is not answered with a zero: a
 * battery that reads 0% because the interpreter invented the number is worse
 * than a battery that is absent.
 */
static int region_access(struct aml_obj *r, u64 off, u32 bits, u64 *val,
                         int write) {
    if (!r || r->type != AML_T_REGION)
        return AML_EREGION;
    if (off + (bits / 8) > r->u.region.len)
        return AML_EREGION;

    switch (r->u.region.space) {
    case AML_SPACE_MEMORY: {
        int e = region_map(r);
        if (e != AML_OK)
            return e;
        volatile u8 *a = r->u.region.va + off;
        switch (bits) {
        case 8:
            if (write) *a = (u8)*val; else *val = *a;
            return AML_OK;
        case 16:
            if (write) *(volatile u16 *)a = (u16)*val; else *val = *(volatile u16 *)a;
            return AML_OK;
        case 32:
            if (write) *(volatile u32 *)a = (u32)*val; else *val = *(volatile u32 *)a;
            return AML_OK;
        case 64:
            if (write) *(volatile u64 *)a = *val; else *val = *(volatile u64 *)a;
            return AML_OK;
        default:
            return AML_EREGION;
        }
    }
#if defined(__x86_64__)
    case AML_SPACE_IO: {
        u16 port = (u16)(r->u.region.base + off);
        switch (bits) {
        case 8:
            if (write) outb(port, (u8)*val); else *val = inb(port);
            return AML_OK;
        case 16:
            if (write) outw(port, (u16)*val); else *val = inw(port);
            return AML_OK;
        case 32:
            if (write) outl(port, (u32)*val); else *val = inl(port);
            return AML_OK;
        default:
            return AML_EREGION;
        }
    }
#endif
    default:
        g_refused_spaces |= 1u << (r->u.region.space & 31);
        return AML_EREGION;
    }
}

/* The access width a field's flags ask for, in bits. AnyAcc picks the
 * narrowest access that still covers the field in one go. */
static u32 field_access_bits(const struct aml_obj *f) {
    switch (f->u.fld.flags & 0x0F) {
    case 1: return 8;
    case 2: return 16;
    case 3: return 32;
    case 4: return 64;
    case 0:
    default: {
        u32 w = f->u.fld.bit_len;
        u32 start = f->u.fld.bit_off;
        if (w <= 8 && (start % 8) + w <= 8) return 8;
        if (w <= 16) return 16;
        if (w <= 32) return 32;
        return 32;
    }
    }
}

static int field_read_int(struct aml_obj *f, u64 *out);
static int field_write_int(struct aml_obj *f, u64 val);

/* An IndexField is two field units on some other region: write the index,
 * then read or write the data. */
static int index_field_io(struct aml_obj *f, u64 *val, int write) {
    u32 acc = field_access_bits(f);
    if (acc == 0)
        acc = 8;
    u32 unit = acc;
    u32 first = f->u.fld.bit_off / unit;
    u32 nunits = (f->u.fld.bit_len + unit - 1) / unit;
    if (f->u.fld.bit_off % unit || f->u.fld.bit_len % unit || nunits == 0 ||
        nunits > 8)
        return AML_EUNSUPP;      /* unaligned index fields are not modelled */
    u64 acc_val = 0;
    for (u32 i = 0; i < nunits; i++) {
        int e = field_write_int(f->u.fld.index_fld, first + i);
        if (e != AML_OK)
            return e;
        if (write) {
            e = field_write_int(f->u.fld.data_fld,
                                (*val >> (i * unit)) & (unit >= 64 ? ~0ULL
                                                        : ((1ULL << unit) - 1)));
        } else {
            u64 piece = 0;
            e = field_read_int(f->u.fld.data_fld, &piece);
            acc_val |= (piece & (unit >= 64 ? ~0ULL : ((1ULL << unit) - 1)))
                       << (i * unit);
        }
        if (e != AML_OK)
            return e;
    }
    if (!write)
        *val = acc_val;
    return AML_OK;
}

static int field_read_int(struct aml_obj *f, u64 *out) {
    if (!f || f->type != AML_T_FIELD)
        return AML_EARG;
    if (f->u.fld.index_fld)
        return index_field_io(f, out, 0);
    if (f->u.fld.bit_len > 64)
        return AML_EUNSUPP;

    u32 acc = field_access_bits(f);
    u32 first = f->u.fld.bit_off / acc;
    u32 last = (f->u.fld.bit_off + f->u.fld.bit_len - 1) / acc;
    u64 v = 0;
    u32 shift = 0;
    for (u32 i = first; i <= last; i++) {
        u64 unit = 0;
        int e = region_access(f->u.fld.region, (u64)i * (acc / 8), acc, &unit, 0);
        if (e != AML_OK)
            return e;
        if (i == first)
            unit >>= (f->u.fld.bit_off % acc);
        if (shift >= 64)
            break;
        v |= unit << shift;
        shift += (i == first) ? (acc - (f->u.fld.bit_off % acc)) : acc;
    }
    if (f->u.fld.bit_len < 64)
        v &= (1ULL << f->u.fld.bit_len) - 1;
    *out = v;
    return AML_OK;
}

static int field_write_int(struct aml_obj *f, u64 val) {
    if (!f || f->type != AML_T_FIELD)
        return AML_EARG;
    if (f->u.fld.index_fld)
        return index_field_io(f, &val, 1);
    if (f->u.fld.bit_len > 64)
        return AML_EUNSUPP;

    u32 acc = field_access_bits(f);
    u32 first = f->u.fld.bit_off / acc;
    u32 last = (f->u.fld.bit_off + f->u.fld.bit_len - 1) / acc;
    u32 update = (u32)((f->u.fld.flags >> 5) & 3);
    u32 consumed = 0;
    for (u32 i = first; i <= last; i++) {
        u32 lo = (i == first) ? (f->u.fld.bit_off % acc) : 0;
        u32 avail = acc - lo;
        u32 take = f->u.fld.bit_len - consumed;
        if (take > avail)
            take = avail;
        u64 mask = (take >= 64) ? ~0ULL : (((1ULL << take) - 1) << lo);
        u64 piece = ((val >> consumed) << lo) & mask;
        u64 unit;
        if (take == acc && lo == 0) {
            unit = piece;
        } else if (update == 1) {               /* WriteAsOnes */
            unit = piece | ~mask;
        } else if (update == 2) {               /* WriteAsZeros */
            unit = piece;
        } else {                                /* Preserve */
            u64 old = 0;
            int e = region_access(f->u.fld.region, (u64)i * (acc / 8), acc,
                                  &old, 0);
            if (e != AML_OK)
                return e;
            unit = (old & ~mask) | piece;
        }
        int e = region_access(f->u.fld.region, (u64)i * (acc / 8), acc, &unit, 1);
        if (e != AML_OK)
            return e;
        consumed += take;
    }
    return AML_OK;
}

/* Buffer fields (CreateByteField and friends) are the same idea over a
 * Buffer object rather than a region. */
static int bfield_read_int(struct aml_obj *f, u64 *out) {
    if (f->u.bfld.bit_len > 64)
        return AML_EUNSUPP;
    struct aml_obj *b = f->u.bfld.buf;
    if (!b || (b->type != AML_T_BUFFER && b->type != AML_T_STRING))
        return AML_EARG;
    u64 v = 0;
    for (u32 i = 0; i < f->u.bfld.bit_len; i++) {
        u32 bit = f->u.bfld.bit_off + i;
        if (bit / 8 >= b->u.buf.len)
            break;
        if (b->u.buf.p[bit / 8] & (1u << (bit % 8)))
            v |= 1ULL << i;
    }
    *out = v;
    return AML_OK;
}

static int bfield_write_int(struct aml_obj *f, u64 val) {
    if (f->u.bfld.bit_len > 64)
        return AML_EUNSUPP;
    struct aml_obj *b = f->u.bfld.buf;
    if (!b || (b->type != AML_T_BUFFER && b->type != AML_T_STRING))
        return AML_EARG;
    for (u32 i = 0; i < f->u.bfld.bit_len; i++) {
        u32 bit = f->u.bfld.bit_off + i;
        if (bit / 8 >= b->u.buf.len)
            break;
        u8 m = (u8)(1u << (bit % 8));
        if (val & (1ULL << i))
            b->u.buf.p[bit / 8] |= m;
        else
            b->u.buf.p[bit / 8] &= (u8)~m;
    }
    return AML_OK;
}

/* ────────────────────────────────────────────────────────────────────────
 * 6. The evaluator
 * ──────────────────────────────────────────────────────────────────────── */

static struct aml_obj *eval_term(struct aml_ctx *c);
static int exec_list(struct aml_ctx *c, const u8 *end);
static int load_list(struct aml_ctx *c, const u8 *end);

/* An lvalue: what a Target or SuperName resolved to. */
#define LV_NONE   0
#define LV_NODE   1
#define LV_LOCAL  2
#define LV_ARG    3
#define LV_OBJ    4
#define LV_DEBUG  5

struct aml_lv {
    int kind;
    struct aml_node *node;
    struct aml_obj  *obj;       /* owned when kind == LV_OBJ */
    int index;
};

static void lv_clear(struct aml_lv *lv) {
    if (lv->kind == LV_OBJ)
        aml_unref(lv->obj);
    lv->kind = LV_NONE;
    lv->obj = 0;
    lv->node = 0;
}

static struct aml_obj *obj_copy(struct aml_obj *src);

/* Deref a value the way an operand wants it: a field unit is read, a named
 * method with no arguments is called, a reference is followed. */
static struct aml_obj *deref_value(struct aml_ctx *c, struct aml_obj *o) {
    if (!o)
        return 0;
    if (o->type == AML_T_FIELD) {
        u64 v = 0;
        int e = field_read_int(o, &v);
        if (e != AML_OK) {
            c->err = e;
            aml_unref(o);
            return 0;
        }
        aml_unref(o);
        return obj_int(v);
    }
    if (o->type == AML_T_BUFFER_FIELD) {
        u64 v = 0;
        int e = bfield_read_int(o, &v);
        if (e != AML_OK) {
            c->err = e;
            aml_unref(o);
            return 0;
        }
        aml_unref(o);
        return obj_int(v);
    }
    return o;
}

static struct aml_obj *obj_copy(struct aml_obj *src) {
    if (!src)
        return 0;
    switch (src->type) {
    case AML_T_INTEGER:
        return obj_int(src->u.integer);
    case AML_T_STRING:
    case AML_T_BUFFER: {
        struct aml_obj *o = obj_buf(src->u.buf.len, src->type == AML_T_STRING);
        if (o)
            memcpy(o->u.buf.p, src->u.buf.p, src->u.buf.len);
        return o;
    }
    case AML_T_PACKAGE: {
        struct aml_obj *o = obj_pkg(src->u.pkg.n);
        if (!o)
            return 0;
        for (u32 i = 0; i < src->u.pkg.n; i++)
            o->u.pkg.e[i] = aml_ref(src->u.pkg.e[i]);
        return o;
    }
    default:
        return aml_ref(src);
    }
}

/* Convert for a Store into a target that already has a type. */
static struct aml_obj *convert_to(struct aml_obj *src, u8 type) {
    if (!src)
        return 0;
    if (src->type == type)
        return obj_copy(src);
    switch (type) {
    case AML_T_INTEGER:
        return obj_int(obj_as_int(src));
    case AML_T_BUFFER: {
        if (src->type == AML_T_INTEGER) {
            u32 n = (u32)(g_int_bits / 8);
            struct aml_obj *o = obj_buf(n, 0);
            if (o)
                for (u32 i = 0; i < n; i++)
                    o->u.buf.p[i] = (u8)(src->u.integer >> (i * 8));
            return o;
        }
        if (src->type == AML_T_STRING) {
            struct aml_obj *o = obj_buf(src->u.buf.len, 0);
            if (o)
                memcpy(o->u.buf.p, src->u.buf.p, src->u.buf.len);
            return o;
        }
        return obj_copy(src);
    }
    case AML_T_STRING: {
        if (src->type == AML_T_INTEGER) {
            char tmp[24];
            int n = snprintf(tmp, sizeof(tmp), "%llX",
                             (unsigned long long)src->u.integer);
            if (n < 0)
                n = 0;
            return obj_str(tmp);
        }
        if (src->type == AML_T_BUFFER) {
            struct aml_obj *o = obj_buf(src->u.buf.len, 1);
            if (o)
                memcpy(o->u.buf.p, src->u.buf.p, src->u.buf.len);
            return o;
        }
        return obj_copy(src);
    }
    default:
        return obj_copy(src);
    }
}

static int store_obj(struct aml_ctx *c, struct aml_obj *src, struct aml_lv *lv,
                     int copy_object);

static int store_to_obj(struct aml_ctx *c, struct aml_obj *src,
                        struct aml_obj *dst) {
    (void)c;
    if (!dst)
        return AML_EARG;
    if (dst->type == AML_T_FIELD)
        return field_write_int(dst, obj_as_int(src));
    if (dst->type == AML_T_BUFFER_FIELD)
        return bfield_write_int(dst, obj_as_int(src));
    if (dst->type == AML_T_REF) {
        if (dst->u.ref.kind == REF_NODE && dst->u.ref.node) {
            struct aml_lv lv = { LV_NODE, dst->u.ref.node, 0, 0 };
            return store_obj(c, src, &lv, 0);
        }
        if (dst->u.ref.kind == REF_PKG_ELEM && dst->u.ref.obj &&
            dst->u.ref.index < dst->u.ref.obj->u.pkg.n) {
            struct aml_obj *cp = obj_copy(src);
            if (!cp)
                return AML_ENOMEM;
            aml_unref(dst->u.ref.obj->u.pkg.e[dst->u.ref.index]);
            dst->u.ref.obj->u.pkg.e[dst->u.ref.index] = cp;
            return AML_OK;
        }
        if (dst->u.ref.kind == REF_BUF_BYTE && dst->u.ref.obj &&
            dst->u.ref.index < dst->u.ref.obj->u.buf.len) {
            dst->u.ref.obj->u.buf.p[dst->u.ref.index] = (u8)obj_as_int(src);
            return AML_OK;
        }
        return AML_EARG;
    }
    return AML_EARG;
}

static int store_obj(struct aml_ctx *c, struct aml_obj *src, struct aml_lv *lv,
                     int copy_object) {
    if (!lv || lv->kind == LV_NONE)
        return AML_OK;
    switch (lv->kind) {
    case LV_DEBUG:
        if (src && src->type == AML_T_INTEGER)
            k_info("aml", "Debug: 0x%llx", (unsigned long long)src->u.integer);
        else if (src && src->type == AML_T_STRING)
            k_info("aml", "Debug: %s", (const char *)src->u.buf.p);
        else
            k_info("aml", "Debug: <%s>", aml_type_name(src ? src->type : 0));
        return AML_OK;
    case LV_LOCAL: {
        aml_unref(c->loc[lv->index]);
        c->loc[lv->index] = obj_copy(src);
        return AML_OK;
    }
    case LV_ARG: {
        aml_unref(c->arg[lv->index]);
        c->arg[lv->index] = obj_copy(src);
        return AML_OK;
    }
    case LV_NODE: {
        struct aml_node *n = lv->node;
        if (!n)
            return AML_EARG;
        if (n->val && (n->val->type == AML_T_FIELD ||
                       n->val->type == AML_T_BUFFER_FIELD))
            return store_to_obj(c, src, n->val);
        struct aml_obj *cp;
        if (copy_object || !n->val || n->val->type == AML_T_UNINIT ||
            n->val->type == AML_T_SCOPE)
            cp = obj_copy(src);
        else
            cp = convert_to(src, n->val->type);
        if (!cp)
            return AML_ENOMEM;
        node_set(n, cp);
        return AML_OK;
    }
    case LV_OBJ:
        return store_to_obj(c, src, lv->obj);
    default:
        return AML_EARG;
    }
}

/* Parse a SuperName / Target into an lvalue. */
static int eval_lvalue(struct aml_ctx *c, struct aml_lv *lv, int allow_null) {
    memset(lv, 0, sizeof(*lv));
    if (at_end(c)) {
        c->err = AML_EBADCODE;
        return c->err;
    }
    u8 b = *c->p;
    if (b == 0x00 && allow_null) {
        c->p++;
        lv->kind = LV_NONE;
        return AML_OK;
    }
    if (b >= 0x60 && b <= 0x67) {
        c->p++;
        lv->kind = LV_LOCAL;
        lv->index = b - 0x60;
        return AML_OK;
    }
    if (b >= 0x68 && b <= 0x6E) {
        c->p++;
        lv->kind = LV_ARG;
        lv->index = b - 0x68;
        return AML_OK;
    }
    if (b == 0x5B && c->p + 1 < c->end && c->p[1] == 0x31) {
        c->p += 2;
        lv->kind = LV_DEBUG;
        return AML_OK;
    }
    if (is_lead_name_char(b) || b == '\\' || b == '^' || b == 0x2E ||
        b == 0x2F) {
        /* A plain name. This is not a method invocation: a SuperName names
         * an object. */
        {
            struct aml_path np;
            if (parse_name(c, &np) != AML_OK)
                return c->err;
            struct aml_node *n = ns_resolve(c->scope, &np, 0);
            if (!n) {
                /* Storing into a name that does not exist yet creates it —
                 * some firmware relies on that for scratch variables. */
                n = ns_resolve(c->scope, &np, 1);
                if (!n) {
                    c->err = AML_ENOENT;
                    return c->err;
                }
            }
            lv->kind = LV_NODE;
            lv->node = n;
            return AML_OK;
        }
    }
    /* Otherwise it must be an expression that yields a reference. */
    struct aml_obj *o = eval_term(c);
    if (c->err) {
        aml_unref(o);
        return c->err;
    }
    lv->kind = LV_OBJ;
    lv->obj = o;
    return AML_OK;
}

static struct aml_obj *eval_operand(struct aml_ctx *c) {
    struct aml_obj *o = eval_term(c);
    if (c->err) {
        aml_unref(o);
        return 0;
    }
    return deref_value(c, o);
}

static u64 eval_int(struct aml_ctx *c) {
    struct aml_obj *o = eval_operand(c);
    u64 v = obj_as_int(o);
    aml_unref(o);
    return v & int_mask();
}

static struct aml_obj *invoke_method(struct aml_ctx *c, struct aml_node *n,
                                     struct aml_obj **args, int nargs);

/* \_OSI — the one predefined method the firmware asks questions of. Linux
 * answers Ones for the Windows strings it chooses to claim and Zero for
 * everything else, including "Linux"; firmware branches on the answer, so
 * claiming nothing takes the untested path on most machines and claiming
 * everything takes paths for features this kernel does not have. The list
 * stops where the ACPI features this kernel implements stop. */
static const char *const g_osi_yes[] = {
    "Windows 2000", "Windows 2001", "Windows 2001 SP1", "Windows 2001 SP2",
    "Windows 2001.1", "Windows 2006", "Windows 2006.1", "Windows 2006 SP1",
    "Windows 2009", "Windows 2012", "Windows 2013", "Windows 2015",
};

static struct aml_obj *builtin_osi(struct aml_obj **args, int nargs) {
    if (nargs < 1 || !args[0] || args[0]->type != AML_T_STRING)
        return obj_int(0);
    const char *want = (const char *)args[0]->u.buf.p;
    for (usize i = 0; i < sizeof(g_osi_yes) / sizeof(g_osi_yes[0]); i++)
        if (strcmp(want, g_osi_yes[i]) == 0)
            return obj_int(int_mask());
    return obj_int(0);
}

static struct aml_obj *invoke_method(struct aml_ctx *c, struct aml_node *n,
                                     struct aml_obj **args, int nargs) {
    if (!n || !n->val || n->val->type != AML_T_METHOD) {
        c->err = AML_EARG;
        return 0;
    }
    if (n->val->u.method.builtin)
        return builtin_osi(args, nargs);
    if (c->depth + 1 > AML_MAX_DEPTH) {
        c->err = AML_EDEPTH;
        return 0;
    }

    struct aml_ctx m;
    memset(&m, 0, sizeof(m));
    m.p = n->val->u.method.body;
    m.end = n->val->u.method.body + n->val->u.method.len;
    m.scope = n;
    m.depth = c->depth + 1;
    m.budget = c->budget;
    for (int i = 0; i < nargs && i < 7; i++)
        m.arg[i] = aml_ref(args[i]);

    exec_list(&m, m.end);

    struct aml_obj *ret = m.ret;
    m.ret = 0;
    for (int i = 0; i < 7; i++)
        aml_unref(m.arg[i]);
    for (int i = 0; i < 8; i++)
        aml_unref(m.loc[i]);
    if (m.err && !c->err)
        c->err = m.err;
    if (!ret)
        ret = obj_int(0);
    return ret;
}

static int exec_list(struct aml_ctx *c, const u8 *end) {
    const u8 *save = c->end;
    c->end = end;
    while (c->p < end && !c->err && c->ctrl == CTRL_NONE) {
        if (c->budget && (*c->budget)-- == 0) {
            c->err = AML_EDEPTH;
            break;
        }
        struct aml_obj *o = eval_term(c);
        aml_unref(o);
    }
    if (!c->err && c->ctrl == CTRL_NONE)
        c->p = end;
    c->end = save;
    return c->err;
}

/* Two integers and a target: the shape most of the arithmetic wears. */
static struct aml_obj *binop(struct aml_ctx *c, int op) {
    u64 a = eval_int(c);
    u64 b = eval_int(c);
    if (c->err)
        return 0;
    u64 r = 0;
    switch (op) {
    case 0x72: r = a + b; break;
    case 0x74: r = a - b; break;
    case 0x77: r = a * b; break;
    case 0x85: r = b ? a % b : 0; break;
    case 0x79: r = (b >= 64) ? 0 : (a << b); break;
    case 0x7A: r = (b >= 64) ? 0 : (a >> b); break;
    case 0x7B: r = a & b; break;
    case 0x7C: r = ~(a & b); break;
    case 0x7D: r = a | b; break;
    case 0x7E: r = ~(a | b); break;
    case 0x7F: r = a ^ b; break;
    default:   r = 0; break;
    }
    if (op == 0x85 && b == 0) {
        c->err = AML_EARG;      /* Mod by zero is a fatal error in ACPI */
        return 0;
    }
    r &= int_mask();
    struct aml_lv lv;
    if (eval_lvalue(c, &lv, 1) != AML_OK)
        return 0;
    struct aml_obj *res = obj_int(r);
    int e = store_obj(c, res, &lv, 0);
    lv_clear(&lv);
    if (e != AML_OK)
        c->err = e;
    return res;
}

static struct aml_obj *unop(struct aml_ctx *c, int op) {
    struct aml_obj *src = eval_operand(c);
    if (c->err) {
        aml_unref(src);
        return 0;
    }
    struct aml_obj *res = 0;
    u64 a = obj_as_int(src);
    switch (op) {
    case 0x80: res = obj_int((~a) & int_mask()); break;
    case 0x81: {                        /* FindSetLeftBit */
        u64 v = a & int_mask();
        int bit = 0;
        for (int i = g_int_bits - 1; i >= 0; i--)
            if (v & (1ULL << i)) { bit = i + 1; break; }
        res = obj_int((u64)bit);
        break;
    }
    case 0x82: {                        /* FindSetRightBit */
        u64 v = a & int_mask();
        int bit = 0;
        for (int i = 0; i < g_int_bits; i++)
            if (v & (1ULL << i)) { bit = i + 1; break; }
        res = obj_int((u64)bit);
        break;
    }
    case 0x96: res = convert_to(src, AML_T_BUFFER); break;
    case 0x99: res = obj_int(obj_as_int(src)); break;
    case 0x98: {                        /* ToHexString */
        char tmp[24];
        int n = snprintf(tmp, sizeof(tmp), "0x%llX", (unsigned long long)a);
        (void)n;
        res = obj_str(tmp);
        break;
    }
    case 0x97: {                        /* ToDecimalString */
        char tmp[24];
        int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)a);
        (void)n;
        res = obj_str(tmp);
        break;
    }
    case EXT(0x28): {                   /* FromBCD */
        u64 v = 0, mul = 1;
        for (int i = 0; i < 16; i++) {
            v += ((a >> (i * 4)) & 0xF) * mul;
            mul *= 10;
        }
        res = obj_int(v & int_mask());
        break;
    }
    case EXT(0x29): {                   /* ToBCD */
        u64 v = 0;
        for (int i = 0; i < 16 && a; i++) {
            v |= (a % 10) << (i * 4);
            a /= 10;
        }
        res = obj_int(v);
        break;
    }
    default: res = obj_int(0); break;
    }
    aml_unref(src);
    if (!res) {
        c->err = AML_ENOMEM;
        return 0;
    }
    struct aml_lv lv;
    if (eval_lvalue(c, &lv, 1) != AML_OK) {
        aml_unref(res);
        return 0;
    }
    int e = store_obj(c, res, &lv, 0);
    lv_clear(&lv);
    if (e != AML_OK)
        c->err = e;
    return res;
}

static int obj_compare(struct aml_obj *a, struct aml_obj *b, int *out) {
    if (!a || !b)
        return AML_EARG;
    if (a->type == AML_T_STRING || a->type == AML_T_BUFFER) {
        if (b->type != a->type) {
            u64 x = obj_as_int(a), y = obj_as_int(b);
            *out = x < y ? -1 : (x > y ? 1 : 0);
            return AML_OK;
        }
        u32 n = a->u.buf.len < b->u.buf.len ? a->u.buf.len : b->u.buf.len;
        for (u32 i = 0; i < n; i++) {
            if (a->u.buf.p[i] != b->u.buf.p[i]) {
                *out = a->u.buf.p[i] < b->u.buf.p[i] ? -1 : 1;
                return AML_OK;
            }
        }
        *out = a->u.buf.len < b->u.buf.len ? -1
               : (a->u.buf.len > b->u.buf.len ? 1 : 0);
        return AML_OK;
    }
    u64 x = obj_as_int(a), y = obj_as_int(b);
    *out = x < y ? -1 : (x > y ? 1 : 0);
    return AML_OK;
}

static struct aml_obj *logic2(struct aml_ctx *c, int op) {
    struct aml_obj *a = eval_operand(c);
    struct aml_obj *b = eval_operand(c);
    struct aml_obj *r = 0;
    if (!c->err) {
        int cmp = 0;
        u64 t = int_mask(), f = 0;
        switch (op) {
        case 0x90: r = obj_int((obj_as_int(a) && obj_as_int(b)) ? t : f); break;
        case 0x91: r = obj_int((obj_as_int(a) || obj_as_int(b)) ? t : f); break;
        case 0x93: obj_compare(a, b, &cmp); r = obj_int(cmp == 0 ? t : f); break;
        case 0x94: obj_compare(a, b, &cmp); r = obj_int(cmp > 0 ? t : f); break;
        case 0x95: obj_compare(a, b, &cmp); r = obj_int(cmp < 0 ? t : f); break;
        default:   r = obj_int(f); break;
        }
    }
    aml_unref(a);
    aml_unref(b);
    return r;
}

/* Sleep and Stall. Nothing here may block: an evaluation runs under the
 * interpreter's spinlock, and both are bounded so a firmware that asks for a
 * long nap costs a bounded busy wait instead of a stalled CPU. */
static void op_delay(u64 usec) {
    if (usec > 20000)
        usec = 20000;
    u64 until = ktime_monotonic_ns() + usec * 1000;
    while (ktime_monotonic_ns() < until)
        ;
}

static struct aml_obj *eval_term(struct aml_ctx *c) {
    if (c->err || at_end(c))
        return 0;
    if (++c->depth > AML_MAX_DEPTH) {
        c->depth--;
        c->err = AML_EDEPTH;
        return 0;
    }
    struct aml_obj *res = 0;
    const u8 *op_at = c->p;
    int op = fetch_op(c);
    if (op < 0) {
        c->depth--;
        return 0;
    }

    switch (op) {
    case 0x00: res = obj_int(0); break;
    case 0x01: res = obj_int(1); break;
    case 0xFF: res = obj_int(int_mask()); break;
    case 0x0A: res = obj_int(uint_le(c, 1)); break;
    case 0x0B: res = obj_int(uint_le(c, 2)); break;
    case 0x0C: res = obj_int(uint_le(c, 4)); break;
    case 0x0E: res = obj_int(uint_le(c, 8)); break;
    case EXT(0x30): res = obj_int(2); break;            /* Revision */
    case EXT(0x33): res = obj_int(ktime_monotonic_ns() / 100); break; /* Timer */
    case 0x0D: {                                        /* String */
        const u8 *s = c->p;
        while (!at_end(c) && *c->p)
            c->p++;
        u32 n = (u32)(c->p - s);
        if (!at_end(c))
            c->p++;
        res = obj_buf(n, 1);
        if (res)
            memcpy(res->u.buf.p, s, n);
        break;
    }
    case 0x11: {                                        /* Buffer */
        const u8 *e = pkg_end(c);
        if (c->err) break;
        u64 size = eval_int(c);
        if (c->err) break;
        if (size > (1u << 20)) { c->err = AML_EBADCODE; break; }
        u32 init = (u32)(e - c->p);
        if ((u64)init > size)
            size = init;
        res = obj_buf((u32)size, 0);
        if (res && init)
            memcpy(res->u.buf.p, c->p, init);
        c->p = e;
        break;
    }
    case 0x12:                                          /* Package */
    case 0x13: {                                        /* VarPackage */
        const u8 *e = pkg_end(c);
        if (c->err) break;
        u64 n;
        if (op == 0x12)
            n = u8_at(c);
        else
            n = eval_int(c);
        if (c->err || n > 512) { c->err = c->err ? c->err : AML_EBADCODE; break; }
        res = obj_pkg((u32)n);
        if (!res) { c->err = AML_ENOMEM; break; }
        for (u32 i = 0; i < (u32)n; i++) {
            if (c->p >= e) {
                res->u.pkg.e[i] = obj_new(AML_T_UNINIT);
                continue;
            }
            /* A package element may be a bare NameString. Resolve it now;
             * a forward reference that is still unknown is kept as the name
             * itself rather than silently becoming zero. */
            u8 b = *c->p;
            if (is_lead_name_char(b) || b == '\\' || b == '^' || b == 0x2E ||
                b == 0x2F) {
                struct aml_path np;
                const u8 *before = c->p;
                if (parse_name(c, &np) != AML_OK) { c->p = before; break; }
                struct aml_node *nd = ns_resolve(c->scope, &np, 0);
                if (nd && nd->val) {
                    res->u.pkg.e[i] = aml_ref(nd->val);
                } else {
                    char buf[40];
                    usize l = 0;
                    for (int s = 0; s < np.nsegs && l + 5 < sizeof(buf); s++) {
                        if (s) buf[l++] = '.';
                        for (int k = 0; k < 4; k++) buf[l++] = np.seg[s][k];
                    }
                    buf[l] = '\0';
                    res->u.pkg.e[i] = obj_str(buf);
                }
            } else {
                const u8 *save_end = c->end;
                c->end = e;
                struct aml_obj *el = eval_term(c);
                c->end = save_end;
                res->u.pkg.e[i] = el ? el : obj_new(AML_T_UNINIT);
            }
            if (c->err)
                break;
        }
        c->p = e;
        break;
    }
    /* Local and Arg */
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
        res = aml_ref(c->loc[op - 0x60]);
        if (!res) res = obj_int(0);
        break;
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E:
        res = aml_ref(c->arg[op - 0x68]);
        if (!res) res = obj_int(0);
        break;
    case EXT(0x31):                                     /* Debug as a source */
        res = obj_new(AML_T_DEBUG);
        break;

    case 0x70: {                                        /* Store */
        struct aml_obj *src = eval_operand(c);
        if (c->err) { aml_unref(src); break; }
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) { aml_unref(src); break; }
        int e = store_obj(c, src, &lv, 0);
        lv_clear(&lv);
        if (e != AML_OK)
            c->err = e;
        res = src;
        break;
    }
    case 0x9D: {                                        /* CopyObject */
        struct aml_obj *src = eval_operand(c);
        if (c->err) { aml_unref(src); break; }
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) { aml_unref(src); break; }
        int e = store_obj(c, src, &lv, 1);
        lv_clear(&lv);
        if (e != AML_OK)
            c->err = e;
        res = src;
        break;
    }
    case 0x72: case 0x74: case 0x77: case 0x85:
    case 0x79: case 0x7A: case 0x7B: case 0x7C:
    case 0x7D: case 0x7E: case 0x7F:
        res = binop(c, op);
        break;
    case 0x80: case 0x81: case 0x82: case 0x96:
    case 0x97: case 0x98: case 0x99:
    case EXT(0x28): case EXT(0x29):
        res = unop(c, op);
        break;
    case 0x78: {                                        /* Divide */
        u64 a = eval_int(c), b = eval_int(c);
        if (c->err) break;
        if (b == 0) { c->err = AML_EARG; break; }
        u64 rem = a % b, quo = a / b;
        struct aml_lv lr, lq;
        if (eval_lvalue(c, &lr, 1) != AML_OK) break;
        if (eval_lvalue(c, &lq, 1) != AML_OK) { lv_clear(&lr); break; }
        struct aml_obj *ro = obj_int(rem);
        store_obj(c, ro, &lr, 0);
        aml_unref(ro);
        res = obj_int(quo);
        store_obj(c, res, &lq, 0);
        lv_clear(&lr);
        lv_clear(&lq);
        break;
    }
    case 0x73: {                                        /* Concat */
        struct aml_obj *a = eval_operand(c);
        struct aml_obj *b = eval_operand(c);
        if (!c->err && a && b) {
            if (a->type == AML_T_STRING || b->type == AML_T_STRING) {
                struct aml_obj *sa = convert_to(a, AML_T_STRING);
                struct aml_obj *sb = convert_to(b, AML_T_STRING);
                if (sa && sb) {
                    res = obj_buf(sa->u.buf.len + sb->u.buf.len, 1);
                    if (res) {
                        memcpy(res->u.buf.p, sa->u.buf.p, sa->u.buf.len);
                        memcpy(res->u.buf.p + sa->u.buf.len, sb->u.buf.p,
                               sb->u.buf.len);
                    }
                }
                aml_unref(sa);
                aml_unref(sb);
            } else {
                struct aml_obj *ba = convert_to(a, AML_T_BUFFER);
                struct aml_obj *bb = convert_to(b, AML_T_BUFFER);
                if (ba && bb) {
                    res = obj_buf(ba->u.buf.len + bb->u.buf.len, 0);
                    if (res) {
                        memcpy(res->u.buf.p, ba->u.buf.p, ba->u.buf.len);
                        memcpy(res->u.buf.p + ba->u.buf.len, bb->u.buf.p,
                               bb->u.buf.len);
                    }
                }
                aml_unref(ba);
                aml_unref(bb);
            }
        }
        aml_unref(a);
        aml_unref(b);
        if (!res && !c->err) res = obj_buf(0, 0);
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 1) == AML_OK) {
            store_obj(c, res, &lv, 0);
            lv_clear(&lv);
        }
        break;
    }
    case 0x84: {                                        /* ConcatRes */
        struct aml_obj *a = eval_operand(c);
        struct aml_obj *b = eval_operand(c);
        /* Two resource templates, each ending in an end tag (0x79 + a
         * checksum byte): the result is both without the first's tag. */
        u32 la = (a && a->type == AML_T_BUFFER && a->u.buf.len >= 2)
                     ? a->u.buf.len - 2 : 0;
        u32 lb = (b && b->type == AML_T_BUFFER) ? b->u.buf.len : 0;
        res = obj_buf(la + lb, 0);
        if (res) {
            if (la) memcpy(res->u.buf.p, a->u.buf.p, la);
            if (lb) memcpy(res->u.buf.p + la, b->u.buf.p, lb);
        }
        aml_unref(a);
        aml_unref(b);
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 1) == AML_OK) {
            store_obj(c, res, &lv, 0);
            lv_clear(&lv);
        }
        break;
    }
    case 0x9C: {                                        /* ToString */
        struct aml_obj *src = eval_operand(c);
        u64 len = eval_int(c);
        u32 n = 0;
        if (src && (src->type == AML_T_BUFFER || src->type == AML_T_STRING)) {
            u32 cap = src->u.buf.len;
            if (len < cap) cap = (u32)len;
            while (n < cap && src->u.buf.p[n])
                n++;
            res = obj_buf(n, 1);
            if (res) memcpy(res->u.buf.p, src->u.buf.p, n);
        } else {
            res = obj_str("");
        }
        aml_unref(src);
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 1) == AML_OK) {
            store_obj(c, res, &lv, 0);
            lv_clear(&lv);
        }
        break;
    }
    case 0x9E: {                                        /* Mid */
        struct aml_obj *src = eval_operand(c);
        u64 idx = eval_int(c);
        u64 len = eval_int(c);
        if (src && (src->type == AML_T_BUFFER || src->type == AML_T_STRING)) {
            u32 start = idx < src->u.buf.len ? (u32)idx : src->u.buf.len;
            u32 n = src->u.buf.len - start;
            if (len < n) n = (u32)len;
            res = obj_buf(n, src->type == AML_T_STRING);
            if (res) memcpy(res->u.buf.p, src->u.buf.p + start, n);
        } else {
            res = obj_buf(0, 0);
        }
        aml_unref(src);
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 1) == AML_OK) {
            store_obj(c, res, &lv, 0);
            lv_clear(&lv);
        }
        break;
    }
    case 0x88: {                                        /* Index */
        struct aml_obj *src = eval_operand(c);
        u64 idx = eval_int(c);
        if (!c->err && src) {
            if (src->type == AML_T_PACKAGE && idx < src->u.pkg.n) {
                res = obj_new(AML_T_REF);
                if (res) {
                    res->u.ref.kind = REF_PKG_ELEM;
                    res->u.ref.obj = aml_ref(src);
                    res->u.ref.index = (u32)idx;
                }
            } else if ((src->type == AML_T_BUFFER ||
                        src->type == AML_T_STRING) && idx < src->u.buf.len) {
                res = obj_new(AML_T_REF);
                if (res) {
                    res->u.ref.kind = REF_BUF_BYTE;
                    res->u.ref.obj = aml_ref(src);
                    res->u.ref.index = (u32)idx;
                }
            } else {
                c->err = AML_EARG;
            }
        }
        aml_unref(src);
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 1) == AML_OK) {
            if (lv.kind != LV_NONE && res)
                store_obj(c, res, &lv, 1);
            lv_clear(&lv);
        }
        break;
    }
    case 0x83: {                                        /* DerefOf */
        struct aml_obj *r = eval_term(c);
        if (c->err) { aml_unref(r); break; }
        if (r && r->type == AML_T_REF) {
            switch (r->u.ref.kind) {
            case REF_NODE:
                if (r->u.ref.node && r->u.ref.node->val)
                    res = aml_ref(r->u.ref.node->val);
                else if (r->u.ref.obj)
                    res = aml_ref(r->u.ref.obj);
                else
                    res = obj_int(0);
                res = deref_value(c, res);
                break;
            case REF_PKG_ELEM:
                res = aml_ref(r->u.ref.obj->u.pkg.e[r->u.ref.index]);
                break;
            case REF_BUF_BYTE:
                res = obj_int(r->u.ref.obj->u.buf.p[r->u.ref.index]);
                break;
            default:
                c->err = AML_EARG;
                break;
            }
        } else {
            res = deref_value(c, aml_ref(r));
        }
        aml_unref(r);
        break;
    }
    case 0x71: {                                        /* RefOf */
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) break;
        res = obj_new(AML_T_REF);
        if (res) {
            res->u.ref.kind = REF_NODE;
            if (lv.kind == LV_NODE) {
                res->u.ref.node = lv.node;
            } else if (lv.kind == LV_OBJ) {
                /* A reference to something with no namespace node of its own:
                 * hold the object itself. */
                res->u.ref.obj = aml_ref(lv.obj);
            } else {
                aml_unref(res);
                res = 0;
                c->err = AML_EUNSUPP;
            }
        }
        lv_clear(&lv);
        break;
    }
    case EXT(0x12): {                                   /* CondRefOf */
        const u8 *before = c->p;
        struct aml_lv lv;
        int found = 1;
        /* Resolve without creating: that is the whole point of CondRefOf. */
        if (!at_end(c) && (is_lead_name_char(*c->p) || *c->p == '\\' ||
                           *c->p == '^' || *c->p == 0x2E || *c->p == 0x2F)) {
            struct aml_path np;
            if (parse_name(c, &np) != AML_OK) break;
            struct aml_node *n = ns_resolve(c->scope, &np, 0);
            memset(&lv, 0, sizeof(lv));
            if (n) {
                lv.kind = LV_NODE;
                lv.node = n;
            } else {
                found = 0;
                lv.kind = LV_NONE;
            }
        } else {
            c->p = before;
            if (eval_lvalue(c, &lv, 0) != AML_OK) break;
        }
        struct aml_lv tgt;
        if (eval_lvalue(c, &tgt, 1) != AML_OK) { lv_clear(&lv); break; }
        if (found) {
            struct aml_obj *r = obj_new(AML_T_REF);
            if (r) {
                r->u.ref.kind = REF_NODE;
                r->u.ref.node = lv.node;
                store_obj(c, r, &tgt, 1);
                aml_unref(r);
            }
        }
        lv_clear(&lv);
        lv_clear(&tgt);
        res = obj_int(found ? int_mask() : 0);
        break;
    }
    case 0x75: case 0x76: {                             /* Increment/Decrement */
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) break;
        u64 v = 0;
        if (lv.kind == LV_NODE && lv.node && lv.node->val) {
            struct aml_obj *cur = aml_ref(lv.node->val);
            cur = deref_value(c, cur);
            v = obj_as_int(cur);
            aml_unref(cur);
        } else if (lv.kind == LV_LOCAL) {
            v = obj_as_int(c->loc[lv.index]);
        } else if (lv.kind == LV_ARG) {
            v = obj_as_int(c->arg[lv.index]);
        }
        v = (op == 0x75 ? v + 1 : v - 1) & int_mask();
        res = obj_int(v);
        store_obj(c, res, &lv, 0);
        lv_clear(&lv);
        break;
    }
    case 0x87: {                                        /* SizeOf */
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) break;
        struct aml_obj *o = 0;
        if (lv.kind == LV_NODE && lv.node) o = aml_ref(lv.node->val);
        else if (lv.kind == LV_LOCAL) o = aml_ref(c->loc[lv.index]);
        else if (lv.kind == LV_ARG) o = aml_ref(c->arg[lv.index]);
        else if (lv.kind == LV_OBJ) o = aml_ref(lv.obj);
        u64 n = 0;
        if (o) {
            if (o->type == AML_T_PACKAGE) n = o->u.pkg.n;
            else if (o->type == AML_T_BUFFER || o->type == AML_T_STRING)
                n = o->u.buf.len;
        }
        aml_unref(o);
        lv_clear(&lv);
        res = obj_int(n);
        break;
    }
    case 0x8E: {                                        /* ObjectType */
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) break;
        int t = AML_T_UNINIT;
        if (lv.kind == LV_NODE && lv.node && lv.node->val) t = lv.node->val->type;
        else if (lv.kind == LV_LOCAL && c->loc[lv.index]) t = c->loc[lv.index]->type;
        else if (lv.kind == LV_ARG && c->arg[lv.index]) t = c->arg[lv.index]->type;
        else if (lv.kind == LV_OBJ && lv.obj) t = lv.obj->type;
        lv_clear(&lv);
        res = obj_int((u64)(t > AML_T_DDB ? AML_T_UNINIT : t));
        break;
    }
    case 0x90: case 0x91: case 0x93: case 0x94: case 0x95:
        res = logic2(c, op);
        break;
    case 0x92: {                                        /* LNot */
        u64 v = eval_int(c);
        res = obj_int(v ? 0 : int_mask());
        break;
    }
    case 0x89: {                                        /* Match */
        struct aml_obj *pkg = eval_operand(c);
        u8 op1 = u8_at(c);
        struct aml_obj *v1 = eval_operand(c);
        u8 op2 = u8_at(c);
        struct aml_obj *v2 = eval_operand(c);
        u64 start = eval_int(c);
        u64 found = int_mask();
        if (!c->err && pkg && pkg->type == AML_T_PACKAGE) {
            for (u32 i = (u32)start; i < pkg->u.pkg.n; i++) {
                struct aml_obj *el = pkg->u.pkg.e[i];
                if (!el || el->type == AML_T_UNINIT)
                    continue;
                int c1 = 0, c2 = 0;
                obj_compare(el, v1, &c1);
                obj_compare(el, v2, &c2);
                int ok1 = (op1 == 0) || (op1 == 1 && c1 == 0) ||
                          (op1 == 2 && c1 <= 0) || (op1 == 3 && c1 < 0) ||
                          (op1 == 4 && c1 >= 0) || (op1 == 5 && c1 > 0);
                int ok2 = (op2 == 0) || (op2 == 1 && c2 == 0) ||
                          (op2 == 2 && c2 <= 0) || (op2 == 3 && c2 < 0) ||
                          (op2 == 4 && c2 >= 0) || (op2 == 5 && c2 > 0);
                if (ok1 && ok2) { found = i; break; }
            }
        }
        aml_unref(pkg);
        aml_unref(v1);
        aml_unref(v2);
        res = obj_int(found);
        break;
    }
    case 0x86: {                                        /* Notify */
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) break;
        u64 v = eval_int(c);
        char path[128];
        if (lv.kind == LV_NODE && lv.node) {
            ns_path(lv.node, path, sizeof(path));
            k_info("aml", "Notify(%s, 0x%llx)", path, (unsigned long long)v);
        }
        lv_clear(&lv);
        break;
    }
    case EXT(0x23): {                                   /* Acquire */
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) break;
        c->p += 2;                                      /* timeout, ignored */
        if (lv.kind == LV_NODE && lv.node && lv.node->val &&
            lv.node->val->type == AML_T_MUTEX)
            lv.node->val->u.mutex.held++;
        lv_clear(&lv);
        /* Zero means acquired. Every evaluation is already serialised by the
         * interpreter's own lock, so a mutex here can only ever be free. */
        res = obj_int(0);
        break;
    }
    case EXT(0x27): case EXT(0x26): case EXT(0x24): {   /* Release/Reset/Signal */
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) break;
        if (op == EXT(0x27) && lv.kind == LV_NODE && lv.node && lv.node->val &&
            lv.node->val->type == AML_T_MUTEX && lv.node->val->u.mutex.held)
            lv.node->val->u.mutex.held--;
        lv_clear(&lv);
        break;
    }
    case EXT(0x25): {                                   /* Wait */
        struct aml_lv lv;
        if (eval_lvalue(c, &lv, 0) != AML_OK) break;
        (void)eval_int(c);
        lv_clear(&lv);
        res = obj_int(int_mask());                      /* timed out */
        break;
    }
    case EXT(0x21): op_delay(eval_int(c)); break;       /* Stall */
    case EXT(0x22): op_delay(eval_int(c) * 1000); break;/* Sleep */
    case 0xA3: break;                                   /* Noop */
    case 0xCC: break;                                   /* BreakPoint */
    case 0xA4: {                                        /* Return */
        struct aml_obj *v = eval_operand(c);
        aml_unref(c->ret);
        c->ret = v;
        c->ctrl = CTRL_RETURN;
        break;
    }
    case 0xA5: c->ctrl = CTRL_BREAK; break;
    case 0x9F: c->ctrl = CTRL_CONTINUE; break;
    case 0xA0: {                                        /* If */
        const u8 *e = pkg_end(c);
        if (c->err) break;
        u64 pred = eval_int(c);
        if (c->err) break;
        if (pred)
            exec_list(c, e);
        else
            c->p = e;
        /* An Else immediately after: run or skip it to match. */
        if (!c->err && c->ctrl == CTRL_NONE && c->p < c->end && *c->p == 0xA1) {
            c->p++;
            const u8 *ee = pkg_end(c);
            if (c->err) break;
            if (!pred)
                exec_list(c, ee);
            else
                c->p = ee;
        }
        break;
    }
    case 0xA1: {                                        /* stray Else */
        const u8 *e = pkg_end(c);
        if (!c->err)
            c->p = e;
        break;
    }
    case 0xA2: {                                        /* While */
        const u8 *e = pkg_end(c);
        if (c->err) break;
        const u8 *body = c->p;
        for (;;) {
            if (c->budget && (*c->budget)-- == 0) { c->err = AML_EDEPTH; break; }
            c->p = body;
            u64 pred = eval_int(c);
            if (c->err || !pred)
                break;
            exec_list(c, e);
            if (c->err)
                break;
            if (c->ctrl == CTRL_BREAK) { c->ctrl = CTRL_NONE; break; }
            if (c->ctrl == CTRL_CONTINUE) c->ctrl = CTRL_NONE;
            if (c->ctrl == CTRL_RETURN)
                break;
        }
        if (c->ctrl == CTRL_NONE)
            c->p = e;
        break;
    }
    case EXT(0x32): {                                   /* Fatal */
        u8 ftype = u8_at(c);
        u64 fcode = uint_le(c, 4);
        u64 farg = eval_int(c);
        k_warn("aml", "Fatal(type=%u code=0x%llx arg=0x%llx) from firmware",
               (unsigned)ftype, (unsigned long long)fcode,
               (unsigned long long)farg);
        c->err = AML_EUNSUPP;
        break;
    }
    case 0x5B00 | 0x20:                                 /* Load */
    case EXT(0x2A):                                     /* Unload */
        c->err = AML_EUNSUPP;
        break;
    /* Declarations can appear inside a method body too. */
    case 0x08: case 0x06: case 0x10: case 0x14: case 0x15:
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F:
    case EXT(0x01): case EXT(0x02): case EXT(0x13):
    case EXT(0x80): case EXT(0x81): case EXT(0x82): case EXT(0x83):
    case EXT(0x84): case EXT(0x85): case EXT(0x86): case EXT(0x87): {
        c->p = op_at;
        const u8 *one_end;
        {
            struct aml_ctx probe = *c;
            skip_term(&probe);
            one_end = probe.p;
            if (probe.err) { c->err = probe.err; break; }
        }
        load_list(c, one_end);
        c->p = one_end;
        break;
    }
    default: {
        if (op < 0x100 && starts_name((u8)op)) {
            c->p = op_at;
            struct aml_path np;
            if (parse_name(c, &np) != AML_OK)
                break;
            struct aml_node *n = ns_resolve(c->scope, &np, 0);
            if (!n) {
                c->err = AML_ENOENT;
                break;
            }
            if (n->val && n->val->type == AML_T_METHOD) {
                int na = n->val->u.method.nargs;
                struct aml_obj *a[7];
                memset(a, 0, sizeof(a));
                for (int i = 0; i < na && !c->err; i++)
                    a[i] = eval_operand(c);
                if (!c->err)
                    res = invoke_method(c, n, a, na);
                for (int i = 0; i < 7; i++)
                    aml_unref(a[i]);
            } else {
                res = aml_ref(n->val);
            }
        } else {
            c->err = AML_EBADCODE;
        }
        break;
    }
    }

    c->depth--;
    return res;
}

/* ────────────────────────────────────────────────────────────────────────
 * 7. The loader
 * ──────────────────────────────────────────────────────────────────────── */

/* A named field's PkgLength is a WIDTH IN BITS, and a reserved field's is a
 * number of bits to skip -- neither is a byte span, so the value itself is
 * what a field list needs rather than pkg_end's pointer. */
static u32 read_pkglen_value(struct aml_ctx *c) {
    u8 lead = u8_at(c);
    u32 follow = (u32)(lead >> 6);
    u32 len;
    if (follow == 0) {
        len = lead & 0x3F;
    } else {
        len = lead & 0x0F;
        for (u32 i = 0; i < follow; i++)
            len |= ((u32)u8_at(c)) << (4 + i * 8);
    }
    return len;
}

static void load_fields(struct aml_ctx *c, const u8 *end,
                        struct aml_obj *region, u8 flags,
                        struct aml_obj *index_fld, struct aml_obj *data_fld) {
    u32 bit = 0;
    u8 acc = (u8)(flags & 0x0F);
    while (c->p < end && !c->err) {
        u8 b = *c->p;
        if (b == 0x00) {
            c->p++;
            bit += read_pkglen_value(c);
        } else if (b == 0x01) {
            c->p++;
            acc = u8_at(c);
            (void)u8_at(c);
        } else if (b == 0x02) {
            c->err = AML_EUNSUPP;       /* GPIO/SerialBus connections */
            return;
        } else if (b == 0x03) {
            c->p++;
            acc = u8_at(c);
            (void)u8_at(c);
            (void)u8_at(c);
        } else {
            if (c->p + 4 > end) { c->err = AML_EBADCODE; return; }
            char seg[4];
            seg_copy(seg, (const char *)c->p);
            c->p += 4;
            u32 width = read_pkglen_value(c);
            if (c->err) return;
            struct aml_node *n = ns_add(c->scope, seg, AML_T_FIELD);
            if (n) {
                struct aml_obj *f = obj_new(AML_T_FIELD);
                if (f) {
                    f->u.fld.region = aml_ref(region);
                    f->u.fld.index_fld = aml_ref(index_fld);
                    f->u.fld.data_fld = aml_ref(data_fld);
                    f->u.fld.bit_off = bit;
                    f->u.fld.bit_len = width;
                    f->u.fld.flags = (u8)((flags & 0xF0) | (acc & 0x0F));
                    node_set(n, f);
                }
            }
            bit += width;
        }
    }
}

static struct aml_obj *field_unit_at(struct aml_node *scope,
                                     const struct aml_path *np) {
    struct aml_node *n = ns_resolve(scope, np, 0);
    if (n && n->val && n->val->type == AML_T_FIELD)
        return n->val;
    return 0;
}

/*
 * One declaration. Returns 1 when the term was a declaration this pass
 * handled, 0 when the caller should skip it.
 */
static int load_one(struct aml_ctx *c) {
    const u8 *op_at = c->p;
    int op = fetch_op(c);
    if (op < 0)
        return 0;

    switch (op) {
    case 0x10: {                                        /* Scope */
        const u8 *e = pkg_end(c);
        if (c->err) return 1;
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        struct aml_node *n = ns_resolve(c->scope, &np, 0);
        if (!n)
            n = ns_resolve(c->scope, &np, 1);
        if (!n) { c->p = e; return 1; }
        struct aml_node *save = c->scope;
        c->scope = n;
        load_list(c, e);
        c->scope = save;
        c->p = e;
        return 1;
    }
    case EXT(0x82):                                     /* Device */
    case EXT(0x85): {                                   /* ThermalZone */
        const u8 *e = pkg_end(c);
        if (c->err) return 1;
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        struct aml_node *n = ns_resolve(c->scope, &np, 1);
        if (!n) { c->p = e; return 1; }
        node_set(n, obj_new(op == EXT(0x82) ? AML_T_DEVICE : AML_T_THERMAL));
        struct aml_node *save = c->scope;
        c->scope = n;
        load_list(c, e);
        c->scope = save;
        c->p = e;
        return 1;
    }
    case EXT(0x83): {                                   /* Processor */
        const u8 *e = pkg_end(c);
        if (c->err) return 1;
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        c->p += 6;                                      /* id, pblk, pblk len */
        struct aml_node *n = ns_resolve(c->scope, &np, 1);
        if (!n) { c->p = e; return 1; }
        node_set(n, obj_new(AML_T_PROCESSOR));
        struct aml_node *save = c->scope;
        c->scope = n;
        load_list(c, e);
        c->scope = save;
        c->p = e;
        return 1;
    }
    case EXT(0x84): {                                   /* PowerResource */
        const u8 *e = pkg_end(c);
        if (c->err) return 1;
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        c->p += 3;                                      /* level, order */
        struct aml_node *n = ns_resolve(c->scope, &np, 1);
        if (!n) { c->p = e; return 1; }
        node_set(n, obj_new(AML_T_POWER));
        struct aml_node *save = c->scope;
        c->scope = n;
        load_list(c, e);
        c->scope = save;
        c->p = e;
        return 1;
    }
    case 0x14: {                                        /* Method */
        const u8 *e = pkg_end(c);
        if (c->err) return 1;
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        u8 flags = u8_at(c);
        struct aml_node *n = ns_resolve(c->scope, &np, 1);
        if (n) {
            struct aml_obj *m = obj_new(AML_T_METHOD);
            if (m) {
                m->u.method.body = c->p;
                m->u.method.len = (u32)(e - c->p);
                m->u.method.nargs = (u8)(flags & 7);
                node_set(n, m);
            }
        }
        c->p = e;
        return 1;
    }
    case 0x08: {                                        /* Name */
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        struct aml_node *n = ns_resolve(c->scope, &np, 1);
        const u8 *data_at = c->p;
        struct aml_obj *v = eval_term(c);
        if (c->err) {
            /* An initialiser this interpreter cannot evaluate must not leave
             * a plausible-looking zero behind, and the stream is now at an
             * unknown offset: rewind and walk past the term instead. */
            aml_unref(v);
            c->err = AML_OK;
            c->p = data_at;
            skip_term(c);
            g_skipped++;
            return 1;
        }
        if (n && v)
            node_set(n, deref_value(c, v));
        else
            aml_unref(v);
        return 1;
    }
    case 0x06: {                                        /* Alias */
        struct aml_path src, dst;
        if (parse_name(c, &src) != AML_OK) return 1;
        if (parse_name(c, &dst) != AML_OK) return 1;
        struct aml_node *s = ns_resolve(c->scope, &src, 0);
        struct aml_node *d = ns_resolve(c->scope, &dst, 1);
        if (s && d && s->val)
            node_set(d, aml_ref(s->val));
        return 1;
    }
    case 0x15: {                                        /* External */
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        u8 type = u8_at(c);
        u8 argc = u8_at(c);
        /* Declaring the arity of a method defined in another table is what
         * lets the loader walk past a call to it without desynchronising. */
        if (type == 8) {
            struct aml_node *n = ns_resolve(c->scope, &np, 0);
            if (!n) {
                n = ns_resolve(c->scope, &np, 1);
                if (n) {
                    struct aml_obj *m = obj_new(AML_T_METHOD);
                    if (m) {
                        m->u.method.body = 0;
                        m->u.method.len = 0;
                        m->u.method.nargs = (u8)(argc & 7);
                        node_set(n, m);
                    }
                }
            }
        }
        return 1;
    }
    case EXT(0x01): {                                   /* Mutex */
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        u8 level = u8_at(c);
        struct aml_node *n = ns_resolve(c->scope, &np, 1);
        if (n) {
            struct aml_obj *m = obj_new(AML_T_MUTEX);
            if (m) {
                m->u.mutex.level = level;
                node_set(n, m);
            }
        }
        return 1;
    }
    case EXT(0x02): {                                   /* Event */
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        struct aml_node *n = ns_resolve(c->scope, &np, 1);
        if (n)
            node_set(n, obj_new(AML_T_EVENT));
        return 1;
    }
    case EXT(0x80): {                                   /* OperationRegion */
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        u8 space = u8_at(c);
        const u8 *data_at = c->p;
        u64 base = eval_int(c);
        u64 len = eval_int(c);
        if (c->err) {
            c->err = AML_OK;
            c->p = data_at;
            skip_term(c);
            skip_term(c);
            g_skipped++;
            return 1;
        }
        struct aml_node *n = ns_resolve(c->scope, &np, 1);
        if (n) {
            struct aml_obj *r = obj_new(AML_T_REGION);
            if (r) {
                r->u.region.space = space;
                r->u.region.base = base;
                r->u.region.len = len;
                node_set(n, r);
            }
        }
        return 1;
    }
    case EXT(0x81): {                                   /* Field */
        const u8 *e = pkg_end(c);
        if (c->err) return 1;
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK) return 1;
        u8 flags = u8_at(c);
        struct aml_node *rn = ns_resolve(c->scope, &np, 0);
        struct aml_obj *region =
            (rn && rn->val && rn->val->type == AML_T_REGION) ? rn->val : 0;
        if (region)
            load_fields(c, e, region, flags, 0, 0);
        else
            g_skipped++;
        c->err = AML_OK;
        c->p = e;
        return 1;
    }
    case EXT(0x86): {                                   /* IndexField */
        const u8 *e = pkg_end(c);
        if (c->err) return 1;
        struct aml_path inp, dnp;
        if (parse_name(c, &inp) != AML_OK) return 1;
        if (parse_name(c, &dnp) != AML_OK) return 1;
        u8 flags = u8_at(c);
        struct aml_obj *ifu = field_unit_at(c->scope, &inp);
        struct aml_obj *dfu = field_unit_at(c->scope, &dnp);
        if (ifu && dfu)
            load_fields(c, e, 0, flags, ifu, dfu);
        else
            g_skipped++;
        c->err = AML_OK;
        c->p = e;
        return 1;
    }
    case EXT(0x87): {                                   /* BankField */
        const u8 *e = pkg_end(c);
        if (!c->err)
            c->p = e;
        /* A bank field needs the bank register written before every access,
         * which nothing this kernel reads uses. Refused rather than modelled
         * as a plain field, which would read the wrong bank. */
        g_skipped++;
        return 1;
    }
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F:
    case EXT(0x13): {                                   /* CreateXField */
        struct aml_obj *src = eval_operand(c);
        u64 idx = eval_int(c);
        u64 width;
        switch (op) {
        case 0x8D: width = 1; break;                    /* CreateBitField */
        case 0x8C: width = 8; break;                    /* CreateByteField */
        case 0x8B: width = 16; break;                   /* CreateWordField */
        case 0x8A: width = 32; break;                   /* CreateDWordField */
        case 0x8F: width = 64; break;                   /* CreateQWordField */
        default:   width = eval_int(c); break;          /* CreateField: bits */
        }
        struct aml_path np;
        if (parse_name(c, &np) != AML_OK || c->err) {
            aml_unref(src);
            c->err = AML_OK;
            g_skipped++;
            return 1;
        }
        struct aml_node *n = ns_resolve(c->scope, &np, 1);
        if (n && src) {
            struct aml_obj *f = obj_new(AML_T_BUFFER_FIELD);
            if (f) {
                f->u.bfld.buf = aml_ref(src);
                f->u.bfld.bit_off = (u32)(op == 0x8D || op == EXT(0x13)
                                              ? idx : idx * 8);
                f->u.bfld.bit_len = (u32)width;
                node_set(n, f);
            }
        }
        aml_unref(src);
        return 1;
    }
    default:
        c->p = op_at;
        return 0;
    }
}

/* Walk a term list building the namespace. Executable terms are skipped, an
 * If whose predicate can be decided here is followed, because firmware does
 * put declarations behind one. */
static int load_list(struct aml_ctx *c, const u8 *end) {
    const u8 *save = c->end;
    c->end = end;
    while (c->p < end && !c->err) {
        if (c->budget && (*c->budget)-- == 0) { c->err = AML_EDEPTH; break; }
        const u8 *before = c->p;
        if (load_one(c)) {
            if (c->p <= before) { c->err = AML_EBADCODE; break; }
            continue;
        }
        if (*c->p == 0xA0) {                            /* If */
            c->p++;
            const u8 *e = pkg_end(c);
            if (c->err) break;
            struct aml_ctx probe = *c;
            probe.load = 0;
            u64 pred = eval_int(&probe);
            int decided = (probe.err == AML_OK);
            if (decided) {
                c->p = probe.p;
                if (pred)
                    load_list(c, e);
            } else {
                g_skipped++;
            }
            c->p = e;
            if (c->p < end && *c->p == 0xA1) {
                c->p++;
                const u8 *ee = pkg_end(c);
                if (c->err) break;
                if (decided && !pred)
                    load_list(c, ee);
                c->p = ee;
            }
            continue;
        }
        /* Anything else at declaration level is an executable statement the
         * load pass has no business running. Walk past it. */
        struct aml_ctx probe = *c;
        skip_term(&probe);
        if (probe.err || probe.p <= c->p) {
            c->err = probe.err ? probe.err : AML_EBADCODE;
            break;
        }
        c->p = probe.p;
        g_skipped++;
    }
    c->end = save;
    return c->err;
}

/* ────────────────────────────────────────────────────────────────────────
 * 8. Tables and the public API
 * ──────────────────────────────────────────────────────────────────────── */

#define AML_MAX_TABLES 16

static struct {
    char sig[5];
    u32  len;
} g_tables[AML_MAX_TABLES];
static int g_ntables;

/* Only the x86_64 build has ACPI tables to load at all: kernel/dev/acpi.c is
 * not compiled on the boards, which have a device tree and no RSDP. The
 * namespace, the evaluator and everything /proc reports are built on both, so
 * an aarch64 kernel answers "no tables, the predefined roots only" rather
 * than not having the interface. */
#if defined(__x86_64__)
static void load_table(const struct acpi_sdt_header *h) {
    if (g_ntables >= AML_MAX_TABLES)
        return;
    if (h->length <= sizeof(*h))
        return;
    const u8 *aml = (const u8 *)h + sizeof(*h);
    u32 len = h->length - (u32)sizeof(*h);

    struct aml_ctx c;
    memset(&c, 0, sizeof(c));
    u64 budget = 2000000;
    c.p = aml;
    c.end = aml + len;
    c.scope = g_root;
    c.load = 1;
    c.budget = &budget;
    load_list(&c, c.end);

    for (int i = 0; i < 4; i++)
        g_tables[g_ntables].sig[i] = h->signature[i];
    g_tables[g_ntables].sig[4] = '\0';
    g_tables[g_ntables].len = len;
    g_ntables++;

    if (c.err)
        k_warn("aml", "%s: decode stopped at +%u (%s)",
               g_tables[g_ntables - 1].sig, (unsigned)(c.p - aml),
               aml_error_name(c.err));
}

#endif /* __x86_64__ */

const char *aml_type_name(int type) {
    switch (type) {
    case AML_T_INTEGER:      return "integer";
    case AML_T_STRING:       return "string";
    case AML_T_BUFFER:       return "buffer";
    case AML_T_PACKAGE:      return "package";
    case AML_T_FIELD:        return "field";
    case AML_T_DEVICE:       return "device";
    case AML_T_EVENT:        return "event";
    case AML_T_METHOD:       return "method";
    case AML_T_MUTEX:        return "mutex";
    case AML_T_REGION:       return "region";
    case AML_T_POWER:        return "power";
    case AML_T_PROCESSOR:    return "processor";
    case AML_T_THERMAL:      return "thermal";
    case AML_T_BUFFER_FIELD: return "bufferfield";
    case AML_T_DEBUG:        return "debug";
    case AML_T_REF:          return "reference";
    case AML_T_SCOPE:        return "scope";
    default:                 return "uninitialised";
    }
}

const char *aml_error_name(int err) {
    switch (err) {
    case AML_OK:       return "ok";
    case AML_ENOENT:   return "no-such-object";
    case AML_EBADCODE: return "bad-bytecode";
    case AML_EUNSUPP:  return "unsupported";
    case AML_ENOMEM:   return "out-of-memory";
    case AML_EARG:     return "bad-argument";
    case AML_EDEPTH:   return "budget-exhausted";
    case AML_EREGION:  return "region-refused";
    default:           return "error";
    }
}

static void make_roots(void) {
    static const char *const roots[] = { "_GPE", "_PR_", "_SB_", "_SI_", "_TZ_" };
    for (usize i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        ns_add(g_root, roots[i], AML_T_SCOPE);

    struct aml_node *n = ns_add(g_root, "_OSI", AML_T_METHOD);
    if (n && n->val) {
        n->val->u.method.builtin = 1;
        n->val->u.method.nargs = 1;
    }
    n = ns_add(g_root, "_OS_", AML_T_STRING);
    if (n) {
        /* What Linux reports, for the same reason: firmware that branches on
         * the name has only ever been tested with this one. */
        struct aml_obj *s = obj_str("Microsoft Windows NT");
        if (s)
            node_set(n, s);
    }
    n = ns_add(g_root, "_REV", AML_T_INTEGER);
    if (n && n->val)
        n->val->u.integer = 2;
    ns_add(g_root, "_GL_", AML_T_MUTEX);
}

int aml_init(void) {
    if (g_root)
        return g_ntables;
    g_root = kzalloc(sizeof(*g_root));
    if (!g_root)
        return 0;
    seg_copy(g_root->seg, "\\___");
    g_root->val = obj_new(AML_T_SCOPE);
    make_roots();
    g_ready = 1;

#if defined(__x86_64__)
    const struct acpi_sdt_header *dsdt = acpi_dsdt();
    if (dsdt) {
        /* A revision-1 table is a 32-bit one, and its author tested it that
         * way: arithmetic that overflows 32 bits must wrap where they
         * expected it to. */
        g_int_bits = dsdt->revision >= 2 ? 64 : 32;
        load_table(dsdt);
    }
    for (int i = 0; i < acpi_table_count(); i++) {
        const struct acpi_sdt_header *t = acpi_table_at(i);
        if (!t)
            continue;
        if (t->signature[0] == 'S' && t->signature[1] == 'S' &&
            t->signature[2] == 'D' && t->signature[3] == 'T')
            load_table(t);
    }
#endif

    if (g_ntables)
        k_info("aml", "%d table(s), %u objects (%u methods, %u devices, "
                      "%u regions, %u fields), %u terms skipped",
               g_ntables, (unsigned)g_objects,
               (unsigned)g_typecount[AML_T_METHOD],
               (unsigned)g_typecount[AML_T_DEVICE],
               (unsigned)g_typecount[AML_T_REGION],
               (unsigned)g_typecount[AML_T_FIELD], (unsigned)g_skipped);
    return g_ntables;
}

int aml_ready(void) { return g_ready; }
int aml_table_count(void) { return g_ntables; }

const char *aml_table_sig(int idx) {
    if (idx < 0 || idx >= g_ntables)
        return 0;
    return g_tables[idx].sig;
}

u32 aml_table_length(int idx) {
    if (idx < 0 || idx >= g_ntables)
        return 0;
    return g_tables[idx].len;
}

u32 aml_object_count(void) { return g_objects; }

u32 aml_type_count(int type) {
    if (type < 0 || type > AML_T_SCOPE)
        return 0;
    return g_typecount[type];
}

u32 aml_skipped_terms(void) { return g_skipped; }

u32 aml_refused_spaces(void) { return g_refused_spaces; }

static void walk_rec(struct aml_node *n, aml_walk_fn fn, void *ctx, char *path,
                     usize cap) {
    for (struct aml_node *ch = n->child; ch; ch = ch->next) {
        ns_path(ch, path, cap);
        fn(ctx, path, ch->val ? ch->val->type : AML_T_UNINIT);
        walk_rec(ch, fn, ctx, path, cap);
    }
}

void aml_walk(aml_walk_fn fn, void *ctx) {
    if (!g_root || !fn)
        return;
    char path[160];
    spin_lock(&g_aml_lock);
    walk_rec(g_root, fn, ctx, path, sizeof(path));
    spin_unlock(&g_aml_lock);
}

/* Parse a textual path ("\\_SB_.PCI0._UID") into the namespace. */
static struct aml_node *lookup_path(const char *path) {
    if (!g_root || !path)
        return 0;
    struct aml_node *cur = g_root;
    const char *p = path;
    while (*p == '\\')
        p++;
    if (!*p)
        return g_root;
    while (*p) {
        char seg[4] = { '_', '_', '_', '_' };
        int i = 0;
        while (*p && *p != '.' && i < 4)
            seg[i++] = *p++;
        while (*p && *p != '.')
            p++;
        if (*p == '.')
            p++;
        cur = ns_child(cur, seg);
        if (!cur)
            return 0;
    }
    return cur;
}

int aml_exists(const char *path) {
    spin_lock(&g_aml_lock);
    int r = lookup_path(path) != 0;
    spin_unlock(&g_aml_lock);
    return r;
}

static void fill_result(struct aml_obj *o, struct aml_result *out) {
    memset(out, 0, sizeof(*out));
    if (!o) {
        out->type = AML_T_UNINIT;
        return;
    }
    out->type = o->type;
    switch (o->type) {
    case AML_T_INTEGER:
        out->integer = o->u.integer;
        break;
    case AML_T_STRING:
    case AML_T_BUFFER: {
        out->length = o->u.buf.len;
        u32 n = o->u.buf.len;
        if (n > AML_RESULT_BYTES)
            n = AML_RESULT_BYTES;
        memcpy(out->bytes, o->u.buf.p, n);
        out->bytes_copied = n;
        break;
    }
    case AML_T_PACKAGE: {
        out->length = o->u.pkg.n;
        u32 n = o->u.pkg.n;
        if (n > AML_RESULT_ELEMS)
            n = AML_RESULT_ELEMS;
        for (u32 i = 0; i < n; i++) {
            struct aml_obj *e = o->u.pkg.e[i];
            out->elem_type[i] = e ? e->type : AML_T_UNINIT;
            out->elem_int[i] = e ? obj_as_int(e) : 0;
        }
        out->elems = n;
        break;
    }
    default:
        break;
    }
}

/* The whole of an evaluation, with one option: `elem` of -1 flattens the
 * object itself, and anything else flattens that element of the package it
 * returned. A package of packages — which is what `_PSS` is, one inner package
 * per P-state — cannot be read any other way through a flat result, and
 * handing the caller the interpreter's own object layout instead would mean
 * handing out a pointer whose lifetime is the interpreter's lock. */
static int aml_evaluate_at(const char *path, const u64 *args, int nargs,
                           int elem, struct aml_result *out) {
    if (!out)
        return AML_EARG;
    memset(out, 0, sizeof(*out));
    if (!g_ready || !g_root)
        return AML_ENOENT;
    if (nargs < 0 || nargs > 7)
        return AML_EARG;

    spin_lock(&g_aml_lock);
    struct aml_node *n = lookup_path(path);
    if (!n) {
        spin_unlock(&g_aml_lock);
        return AML_ENOENT;
    }

    struct aml_ctx c;
    memset(&c, 0, sizeof(c));
    u64 budget = 200000;
    c.budget = &budget;
    c.scope = n->parent ? n->parent : g_root;
    c.p = c.end = 0;

    struct aml_obj *res = 0;
    int err = AML_OK;
    if (n->val && n->val->type == AML_T_METHOD) {
        if (nargs != n->val->u.method.nargs) {
            spin_unlock(&g_aml_lock);
            return AML_EARG;
        }
        struct aml_obj *a[7];
        memset(a, 0, sizeof(a));
        for (int i = 0; i < nargs; i++)
            a[i] = obj_int(args[i]);
        res = invoke_method(&c, n, a, nargs);
        for (int i = 0; i < 7; i++)
            aml_unref(a[i]);
        err = c.err;
    } else if (n->val) {
        res = aml_ref(n->val);
        res = deref_value(&c, res);
        err = c.err;
    } else {
        err = AML_ENOENT;
    }

    if (err == AML_OK) {
        if (elem < 0) {
            fill_result(res, out);
        } else if (res && res->type == AML_T_PACKAGE &&
                   (u32)elem < res->u.pkg.n) {
            fill_result(res->u.pkg.e[elem], out);
        } else {
            err = AML_EARG;
        }
    }
    aml_unref(res);
    spin_unlock(&g_aml_lock);
    return err;
}

int aml_evaluate(const char *path, const u64 *args, int nargs,
                 struct aml_result *out) {
    return aml_evaluate_at(path, args, nargs, -1, out);
}

int aml_evaluate_element(const char *path, const u64 *args, int nargs,
                         u32 index, struct aml_result *out) {
    return aml_evaluate_at(path, args, nargs, (int)index, out);
}


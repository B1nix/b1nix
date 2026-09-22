/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef B1NIX_AML_H
#define B1NIX_AML_H

#include <b1nix/types.h>

/*
 * AML — the bytecode inside the ACPI DSDT and SSDTs (M134).
 *
 * kernel/dev/acpi.c reads the static tables: everything there is a struct with
 * fields at fixed offsets. The DSDT is not that. It carries a compiled program
 * — a namespace of devices and named objects, plus methods that have to be
 * EXECUTED to answer a question. A battery's remaining charge (`_BST`), a
 * thermal zone's temperature (`_TMP`) and the sleep type the machine wants for
 * S5 are all method or package evaluations, not registers, so nothing about
 * them can be read without an interpreter.
 *
 * This is that interpreter: a loader that walks the byte stream and builds the
 * object tree, and an evaluator for the opcodes those objects need. It is not
 * ACPICA — there is no OSPM event model, no GPE dispatch, no table unloading —
 * and the operation-region spaces it cannot honour are REFUSED rather than
 * guessed at (see aml_region_space_name and the refusal in region_access).
 */

/* Object types. The numbering matches ACPI's ObjectType values where one
 * exists, because `ObjectType()` in AML returns exactly this. */
#define AML_T_UNINIT        0
#define AML_T_INTEGER       1
#define AML_T_STRING        2
#define AML_T_BUFFER        3
#define AML_T_PACKAGE       4
#define AML_T_FIELD         5   /* FieldUnit */
#define AML_T_DEVICE        6
#define AML_T_EVENT         7
#define AML_T_METHOD        8
#define AML_T_MUTEX         9
#define AML_T_REGION        10  /* OperationRegion */
#define AML_T_POWER         11
#define AML_T_PROCESSOR     12
#define AML_T_THERMAL       13
#define AML_T_BUFFER_FIELD  14
#define AML_T_DDB           15
#define AML_T_DEBUG         16
#define AML_T_REF           17  /* internal: produced by RefOf/Index */
#define AML_T_SCOPE         18  /* internal: a namespace node with no value */

/* Operation-region address spaces (ACPI 6.x table 5-89). */
#define AML_SPACE_MEMORY    0x00
#define AML_SPACE_IO        0x01
#define AML_SPACE_PCI_CFG   0x02
#define AML_SPACE_EC        0x03
#define AML_SPACE_SMBUS     0x04
#define AML_SPACE_CMOS      0x05

/* Errors are small negatives of their own; they never reach userspace as
 * errno, they are reported as text through /proc/b1nix-acpi-eval. */
#define AML_OK              0
#define AML_ENOENT          1   /* no such object */
#define AML_EBADCODE        2   /* the byte stream does not decode */
#define AML_EUNSUPP         3   /* a construct this interpreter refuses */
#define AML_ENOMEM          4
#define AML_EARG            5   /* wrong argument count or type */
#define AML_EDEPTH          6   /* recursion or loop budget exhausted */
#define AML_EREGION         7   /* an address space this kernel will not touch */

/* Load the DSDT and every SSDT the root table lists, and build the namespace.
 * Returns the number of tables loaded (0 when the machine has no ACPI at all,
 * which is every aarch64 board here). Safe to call once, after acpi_init(). */
int aml_init(void);

/* True once aml_init() built a namespace with at least the predefined roots. */
int aml_ready(void);

/* What was loaded, for /proc/b1nix-acpi and for anyone debugging a machine
 * whose firmware does something unexpected. */
int         aml_table_count(void);
const char *aml_table_sig(int idx);     /* "DSDT" / "SSDT", NULL past the end */
u32         aml_table_length(int idx);  /* bytes of AML in that table */
u32         aml_object_count(void);     /* namespace nodes, roots included */
u32         aml_type_count(int type);   /* how many of one AML_T_* */
u32         aml_skipped_terms(void);    /* terms the loader could not decode */
/* One bit per address space this interpreter was asked for and refused. */
u32         aml_refused_spaces(void);
const char *aml_region_space_name(u8 space);

/* Walk every namespace node in tree order. `path` is the absolute path with
 * the leading backslash ("\\_SB_.PCI0"). The callback runs with the
 * interpreter's lock held, so it must not call aml_evaluate or aml_exists:
 * collect what it needs and ask afterwards. */
typedef void (*aml_walk_fn)(void *ctx, const char *path, int type);
void aml_walk(aml_walk_fn fn, void *ctx);

/* The result of an evaluation, flattened so a caller needs no allocator and
 * no knowledge of the interpreter's own object layout. */
#define AML_RESULT_BYTES    256
#define AML_RESULT_ELEMS    8

struct aml_result {
    int  type;                          /* AML_T_* of the returned object */
    u64  integer;                       /* AML_T_INTEGER */
    u32  length;                        /* string/buffer bytes, or package
                                         * element count */
    u32  bytes_copied;                  /* how much of `bytes` is valid */
    u8   bytes[AML_RESULT_BYTES];       /* string (no NUL) or buffer content */
    u32  elems;                         /* package elements described below */
    u8   elem_type[AML_RESULT_ELEMS];
    u64  elem_int[AML_RESULT_ELEMS];
};

/* Evaluate `path` — a Name, or a Method called with `nargs` integer
 * arguments. Returns AML_OK or one of the AML_E* codes above. */
int aml_evaluate(const char *path, const u64 *args, int nargs,
                 struct aml_result *out);

/* The same, but flattening ONE element of the package the evaluation returned
 * — what reading a package of packages needs. `_PSS` is the case that made this
 * exist: a package with one inner package per P-state, each of six integers.
 * AML_EARG when the object is not a package or has no such element. */
int aml_evaluate_element(const char *path, const u64 *args, int nargs,
                         u32 index, struct aml_result *out);

/* True when the namespace has an object at that path. */
int aml_exists(const char *path);

/* Human-readable names, shared by /proc and the boot log. */
const char *aml_type_name(int type);
const char *aml_error_name(int err);

#endif /* B1NIX_AML_H */

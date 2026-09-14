/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_FS_PARSER_H
#define LKPI_LINUX_FS_PARSER_H

#include <linux/fs_context.h>

/*
 * The typed mount-option parser.
 *
 * A filesystem declares a table of `fs_parameter_spec` — name, type, an
 * enumeration tag it wants back — and the parser turns each option into a
 * `fs_parse_result` with the value already converted and range-checked. ext4's
 * table has around ninety entries.
 *
 * The `fsparam_*` macros are how those tables are written, so their expansion
 * has to match upstream field for field: a table built with a different member
 * order compiles and then parses every option as the wrong one.
 *
 * `fs_param_neg_with_no` is the flag behind option pairs like `barrier` and
 * `nobarrier`: one spec, and `result->negated` says which spelling was used.
 * Treating the two as separate options would silently accept `nobarrier` as
 * `barrier`.
 */

/*
 * The type of an option IS its parser.
 *
 * Upstream made the `type` field a function pointer rather than an enum tag,
 * so a filesystem can supply its own converter without extending a switch. The
 * `fsparam_*` macros below install the right one. There is deliberately no enum
 * of the same names: they are functions, and a tag sharing a name with one is a
 * redefinition rather than a parallel spelling.
 */
struct p_log;
struct fs_parameter_spec;
struct fs_parse_result;

typedef int fs_param_type(struct p_log *log,
                          const struct fs_parameter_spec *spec,
                          struct fs_parameter *param,
                          struct fs_parse_result *result);

struct fs_parameter_spec {
	const char *name;
	fs_param_type *type;
	u8 opt;         /* the filesystem's own tag for this option */
	unsigned short flags;
	const void *data;
};

/* Where a parse error is reported to, and what it is called there. */
struct p_log {
	const char *prefix;
	struct fc_log *log;
};

#define fs_param_neg_with_no  0x0002  /* "no" prefix negates it */
#define fs_param_neg_with_empty 0x0004
#define fs_param_deprecated   0x0008
#define fs_param_can_be_empty 0x0010

struct fs_parameter_enum {
	u8 opt;
	char name[14];
	u8 value;
};

struct constant_table {
	const char *name;
	int value;
};

struct fs_parse_result {
	bool negated;   /* the "no" form was used */
	bool has_value;
	union {
		bool boolean;
		int int_32;
		unsigned int uint_32;
		u64 uint_64;
		kuid_t uid;
		kgid_t gid;
	};
};

int fs_parse(struct fs_context *fc, const struct fs_parameter_spec *desc,
             struct fs_parameter *param, struct fs_parse_result *result);
int fs_lookup_param(struct fs_context *fc, struct fs_parameter *param,
                    bool want_bdev, unsigned int flags, struct path *_path);
int lookup_constant(const struct constant_table *tbl, const char *name,
                    int not_found);

/* The per-type parsers the macros below install as `type`. */
fs_param_type fs_param_is_bool, fs_param_is_u32, fs_param_is_s32,
              fs_param_is_u64, fs_param_is_enum, fs_param_is_string,
              fs_param_is_blob, fs_param_is_blockdev, fs_param_is_path,
              fs_param_is_fd, fs_param_is_uid, fs_param_is_gid;

#define __fsparam(TYPE, NAME, OPT, FLAGS, DATA) \
	{ .name = NAME, .opt = OPT, .type = TYPE, .flags = FLAGS, .data = DATA }

#define fsparam_flag(NAME, OPT)      __fsparam(NULL, NAME, OPT, 0, NULL)
#define fsparam_flag_no(NAME, OPT) \
	__fsparam(NULL, NAME, OPT, fs_param_neg_with_no, NULL)
#define fsparam_bool(NAME, OPT)      __fsparam(fs_param_is_bool, NAME, OPT, 0, NULL)
#define fsparam_u32(NAME, OPT)       __fsparam(fs_param_is_u32, NAME, OPT, 0, NULL)
#define fsparam_u32oct(NAME, OPT) \
	__fsparam(fs_param_is_u32, NAME, OPT, 0, (void *)8)
#define fsparam_u32hex(NAME, OPT) \
	__fsparam(fs_param_is_u32, NAME, OPT, 0, (void *)16)
#define fsparam_s32(NAME, OPT)       __fsparam(fs_param_is_s32, NAME, OPT, 0, NULL)
#define fsparam_u64(NAME, OPT)       __fsparam(fs_param_is_u64, NAME, OPT, 0, NULL)
#define fsparam_enum(NAME, OPT, array) \
	__fsparam(fs_param_is_enum, NAME, OPT, 0, array)
#define fsparam_string(NAME, OPT) \
	__fsparam(fs_param_is_string, NAME, OPT, 0, NULL)
#define fsparam_string_empty(NAME, OPT) \
	__fsparam(fs_param_is_string, NAME, OPT, fs_param_can_be_empty, NULL)
#define fsparam_blob(NAME, OPT)      __fsparam(fs_param_is_blob, NAME, OPT, 0, NULL)
#define fsparam_bdev(NAME, OPT) \
	__fsparam(fs_param_is_blockdev, NAME, OPT, 0, NULL)
#define fsparam_path(NAME, OPT)      __fsparam(fs_param_is_path, NAME, OPT, 0, NULL)
#define fsparam_fd(NAME, OPT)        __fsparam(fs_param_is_fd, NAME, OPT, 0, NULL)
#define fsparam_uid(NAME, OPT)       __fsparam(fs_param_is_uid, NAME, OPT, 0, NULL)
#define fsparam_gid(NAME, OPT)       __fsparam(fs_param_is_gid, NAME, OPT, 0, NULL)

#endif

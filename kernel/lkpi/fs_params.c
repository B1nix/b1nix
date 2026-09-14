/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the mount-parameter parser, and the odds and ends ext4 needs.
 *
 * ext4 describes its mount options as a table of `fs_parameter_spec` and asks
 * fs_parse() to match one and convert its value; the filesystem then switches
 * on the tag. That is the whole interface, and it has to be real: a parser
 * that matched nothing would make every option an error, and one that returned
 * an unconverted value would have ext4 act on a number it never computed.
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/namei.h>
#include <linux/exportfs.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kstrtox.h>
#include <linux/err.h>
#include <linux/printk.h>
#include <linux/utsname.h>
#include <lkpi/env.h>

/* ── the per-type converters ────────────────────────────────────── */

/*
 * Each takes the parameter's string and fills in the result. The `data` field
 * of the spec carries the base for the numeric forms — the fsparam_u32oct and
 * fsparam_u32hex macros put 8 and 16 there — so "0640" is read as octal where
 * the filesystem said octal, and as decimal where it did not.
 */
static const char *param_string(struct fs_parameter *param)
{
	if (!param)
		return NULL;
	if (param->type == fs_value_is_string || param->type == fs_value_is_blob)
		return param->string;
	return NULL;
}

static unsigned int spec_base(const struct fs_parameter_spec *spec)
{
	unsigned long base = (unsigned long)spec->data;

	return base ? (unsigned int)base : 10u;
}

int fs_param_is_bool(struct p_log *log, const struct fs_parameter_spec *spec,
                     struct fs_parameter *param, struct fs_parse_result *result)
{
	const char *s = param_string(param);

	(void)log;
	(void)spec;
	if (!s || !*s) {
		result->boolean = true;
		return 0;
	}
	if (!strcmp(s, "1") || !strcmp(s, "y") || !strcmp(s, "yes") ||
	    !strcmp(s, "true")) {
		result->boolean = true;
		return 0;
	}
	if (!strcmp(s, "0") || !strcmp(s, "n") || !strcmp(s, "no") ||
	    !strcmp(s, "false")) {
		result->boolean = false;
		return 0;
	}
	return -EINVAL;
}

int fs_param_is_u32(struct p_log *log, const struct fs_parameter_spec *spec,
                    struct fs_parameter *param, struct fs_parse_result *result)
{
	const char *s = param_string(param);
	unsigned int v;

	(void)log;
	if (!s || kstrtouint(s, spec_base(spec), &v) != 0)
		return -EINVAL;
	result->uint_32 = v;
	return 0;
}

int fs_param_is_s32(struct p_log *log, const struct fs_parameter_spec *spec,
                    struct fs_parameter *param, struct fs_parse_result *result)
{
	const char *s = param_string(param);
	int v;

	(void)log;
	if (!s || kstrtoint(s, spec_base(spec), &v) != 0)
		return -EINVAL;
	result->int_32 = v;
	return 0;
}

int fs_param_is_u64(struct p_log *log, const struct fs_parameter_spec *spec,
                    struct fs_parameter *param, struct fs_parse_result *result)
{
	const char *s = param_string(param);
	u64 v;

	(void)log;
	if (!s || kstrtoull(s, spec_base(spec), &v) != 0)
		return -EINVAL;
	result->uint_64 = v;
	return 0;
}

int fs_param_is_enum(struct p_log *log, const struct fs_parameter_spec *spec,
                     struct fs_parameter *param, struct fs_parse_result *result)
{
	const struct constant_table *tbl = spec->data;
	const char *s = param_string(param);
	int v;

	(void)log;
	if (!s || !tbl)
		return -EINVAL;
	v = lookup_constant(tbl, s, -1);
	if (v < 0)
		return -EINVAL;
	result->uint_32 = (unsigned int)v;
	return 0;
}

int fs_param_is_string(struct p_log *log, const struct fs_parameter_spec *spec,
                       struct fs_parameter *param,
                       struct fs_parse_result *result)
{
	const char *s = param_string(param);

	(void)log;
	(void)result;
	if (!s)
		return -EINVAL;
	if (!*s && !(spec->flags & fs_param_can_be_empty))
		return -EINVAL;
	return 0;
}

int fs_param_is_blob(struct p_log *log, const struct fs_parameter_spec *spec,
                     struct fs_parameter *param, struct fs_parse_result *result)
{
	(void)log;
	(void)spec;
	(void)result;
	return (param && param->type == fs_value_is_blob) ? 0 : -EINVAL;
}

int fs_param_is_blockdev(struct p_log *log,
                         const struct fs_parameter_spec *spec,
                         struct fs_parameter *param,
                         struct fs_parse_result *result)
{
	return fs_param_is_string(log, spec, param, result);
}

int fs_param_is_path(struct p_log *log, const struct fs_parameter_spec *spec,
                     struct fs_parameter *param, struct fs_parse_result *result)
{
	return fs_param_is_string(log, spec, param, result);
}

int fs_param_is_fd(struct p_log *log, const struct fs_parameter_spec *spec,
                   struct fs_parameter *param, struct fs_parse_result *result)
{
	return fs_param_is_s32(log, spec, param, result);
}

int fs_param_is_uid(struct p_log *log, const struct fs_parameter_spec *spec,
                    struct fs_parameter *param, struct fs_parse_result *result)
{
	return fs_param_is_u32(log, spec, param, result);
}

int fs_param_is_gid(struct p_log *log, const struct fs_parameter_spec *spec,
                    struct fs_parameter *param, struct fs_parse_result *result)
{
	return fs_param_is_u32(log, spec, param, result);
}

int lookup_constant(const struct constant_table *tbl, const char *name,
                    int not_found)
{
	if (!tbl || !name)
		return not_found;
	for (; tbl->name; tbl++) {
		if (strcmp(tbl->name, name) == 0)
			return tbl->value;
	}
	return not_found;
}

/* ── the parser proper ──────────────────────────────────────────── */

/*
 * Match one parameter against the filesystem's table.
 *
 * Returns the option's own tag on success, which is what the caller switches
 * on. -ENOPARAM means "not mine", and the caller then reports the option as
 * unknown — so a table miss must not be reported as an error, or every
 * filesystem would reject the options belonging to the VFS.
 */
int fs_parse(struct fs_context *fc, const struct fs_parameter_spec *desc,
             struct fs_parameter *param, struct fs_parse_result *result)
{
	const struct fs_parameter_spec *spec;
	const char *key = param ? param->key : NULL;
	struct p_log log = { fc && fc->fs_type ? fc->fs_type->name : "fs", NULL };
	int ret;

	if (!desc || !key)
		return -ENOPARAM;

	memset(result, 0, sizeof(*result));
	result->has_value = param->type != fs_value_is_flag &&
	                    param->type != fs_value_is_undefined;

	for (spec = desc; spec->name; spec++) {
		if (strcmp(spec->name, key) == 0)
			break;
		/*
		 * "nofoo" for an option declared with fs_param_neg_with_no. The
		 * negated form is the same option with the answer inverted, which is
		 * why it is matched here rather than being a second table entry.
		 */
		if ((spec->flags & fs_param_neg_with_no) &&
		    key[0] == 'n' && key[1] == 'o' &&
		    strcmp(spec->name, key + 2) == 0) {
			result->negated = true;
			break;
		}
	}
	if (!spec->name)
		return -ENOPARAM;

	if (!spec->type) {
		/* A flag: present or absent, no value to convert. */
		if (result->has_value && !(spec->flags & fs_param_can_be_empty))
			return -EINVAL;
		result->boolean = !result->negated;
		return spec->opt;
	}
	if (!result->has_value) {
		/* A typed option with no value. Only the negated form of a
		 * "no"-negatable option is allowed to have none. */
		if (!result->negated)
			return -EINVAL;
		result->boolean = false;
		return spec->opt;
	}
	ret = spec->type(&log, spec, param, result);
	if (ret < 0)
		return ret;
	return spec->opt;
}

int fs_lookup_param(struct fs_context *fc, struct fs_parameter *param,
                    bool want_bdev, unsigned int flags, struct path *_path)
{
	(void)fc;
	(void)want_bdev;
	(void)flags;
	(void)_path;
	/*
	 * Resolving a path named by a mount option — ext4 uses it for the
	 * external journal device. There is no path walk on this side of the
	 * boundary, and returning a path that resolved to nothing would have the
	 * filesystem open whatever that empty path happened to name.
	 */
	if (param && param->key)
		lkpi_printk("lkpi-fs: mount option '%s' names a path, which is not "
		            "resolvable here\n", param->key);
	return -EOPNOTSUPP;
}

/* ── the odds and ends ──────────────────────────────────────────── */

enum system_states system_state = SYSTEM_RUNNING;

struct new_utsname *init_utsname(void)
{
	static struct new_utsname uts;

	if (!uts.sysname[0]) {
		strncpy(uts.sysname, "Linux", sizeof(uts.sysname) - 1);
		strncpy(uts.nodename, "b1nix", sizeof(uts.nodename) - 1);
		/* The release imported code reports. b1nix's own version header is on
		 * the other side of the boundary, so the string is repeated here
		 * rather than included — and it is the ABI release, which is what a
		 * filesystem checking "which kernel am I" is asking about. */
		strncpy(uts.release, "6.6.0", sizeof(uts.release) - 1);
		strncpy(uts.version, "b1nix", sizeof(uts.version) - 1);
		strncpy(uts.machine, "x86_64", sizeof(uts.machine) - 1);
	}
	return &uts;
}

void nd_terminate_link(void *name, size_t len, size_t maxlen)
{
	char *s = name;

	/* A symlink's target is stored without a terminator, and the buffer it is
	 * read into is a page: this is what stops a reader running past the end of
	 * the name into whatever the rest of the page holds. */
	s[len > maxlen ? maxlen : len] = '\0';
}

int generic_check_addressable(unsigned stblocksize, u64 num_blocks)
{
	u64 last_fs_block = num_blocks - 1;
	u64 last_fs_page = last_fs_block >> (PAGE_SHIFT - stblocksize);

	if (unlikely(num_blocks == 0))
		return 0;
	if ((stblocksize < 9) || (stblocksize > PAGE_SHIFT))
		return -EINVAL;
	/* The page cache is indexed by pgoff_t, which is 64-bit here, so the only
	 * filesystem this refuses is one whose block size is impossible. */
	if (last_fs_page > (pgoff_t)-1)
		return -EFBIG;
	return 0;
}

void fsnotify_sb_error(struct super_block *sb, struct inode *inode, int error)
{
	(void)sb;
	(void)inode;
	(void)error;
	/* fanotify's filesystem-error stream. Nothing subscribes to one here; the
	 * error itself is reported by the filesystem's own log message. */
}

int set_task_ioprio(struct lkpi_task *task, int ioprio)
{
	(void)task;
	(void)ioprio;
	/* I/O priority classes. b1nix's block layer has a priority gate of its own
	 * (see blk_io_prio_of_current) but nothing maps these values onto it yet,
	 * so accepting the request without acting on it is the honest answer: the
	 * caller is a journal thread asking to be scheduled ahead of writers, and
	 * failing the call would make it give up entirely. */
	return 0;
}

struct kmem_cache *kmem_cache_create_usercopy(const char *name,
                                              unsigned int size,
                                              unsigned int align,
                                              unsigned int flags,
                                              unsigned int useroffset,
                                              unsigned int usersize,
                                              void (*ctor)(void *))
{
	(void)useroffset;
	(void)usersize;
	/* The usercopy window is what hardened usercopy checks a copy_to_user
	 * against. b1nix has no such check, so the cache is an ordinary one. */
	return kmem_cache_create_legacy(name, size, align, flags, ctor);
}

/* ── file handles ───────────────────────────────────────────────── */

/*
 * Turn a file handle back into a dentry.
 *
 * The handle carries an inode number and a generation, and the filesystem's
 * own callback turns those into an inode; the generation is what makes a
 * handle to a deleted file report ESTALE instead of opening whatever inode
 * number was reused. Nothing in b1nix serves NFS, but ext4 installs these as
 * its export operations and the open-by-handle syscalls reach them.
 */
static struct dentry *fh_to_dentry_common(struct super_block *sb, u64 ino,
                                          u32 gen,
                                          struct inode *(*get_inode)(struct super_block *sb,
                                                                     u64 ino, u32 gen))
{
	struct inode *inode;

	if (!get_inode)
		return ERR_PTR(-ESTALE);
	inode = get_inode(sb, ino, gen);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	if (!inode)
		return ERR_PTR(-ESTALE);
	return d_obtain_alias(inode);
}

struct dentry *generic_fh_to_dentry(struct super_block *sb, struct fid *fid,
                                    int fh_len, int fh_type,
                                    struct inode *(*get_inode)(struct super_block *sb,
                                                               u64 ino, u32 gen))
{
	if (!fid || fh_len < 2)
		return NULL;
	switch (fh_type) {
	case FILEID_INO32_GEN:
	case FILEID_INO32_GEN_PARENT:
		return fh_to_dentry_common(sb, fid->i32.ino, fid->i32.gen, get_inode);
	default:
		return NULL;
	}
}

struct dentry *generic_fh_to_parent(struct super_block *sb, struct fid *fid,
                                    int fh_len, int fh_type,
                                    struct inode *(*get_inode)(struct super_block *sb,
                                                               u64 ino, u32 gen))
{
	if (!fid || fh_len <= 2 || fh_type != FILEID_INO32_GEN_PARENT)
		return NULL;
	return fh_to_dentry_common(sb, fid->i32.parent_ino,
	                           fid->i32.parent_gen, get_inode);
}

__kernel_fsid_t uuid_to_fsid(const __u8 *uuid)
{
	__kernel_fsid_t fsid;
	u64 f[2];

	/* statfs's f_fsid, folded from the filesystem's UUID exactly as upstream
	 * folds it: two 64-bit halves XORed, so both halves of the UUID matter. */
	memcpy(f, uuid, sizeof(f));
	fsid.val[0] = (int)(f[0] ^ (f[0] >> 32));
	fsid.val[1] = (int)(f[1] ^ (f[1] >> 32));
	return fsid;
}

/* ── whole pages, by address ────────────────────────────────────── */

/*
 * The address-not-page spelling of the page allocator.
 *
 * jbd2 allocates its descriptor blocks this way. Only single pages are served:
 * an order above zero would need physically contiguous pages, and a caller
 * given one page where it asked for four writes past the end of it — so the
 * request fails instead.
 */
unsigned long __get_free_pages(gfp_t gfp, unsigned int order)
{
	struct page *page;

	if (order != 0)
		return 0;
	page = alloc_page(gfp);
	if (!page)
		return 0;
	return (unsigned long)page_address(page);
}

unsigned long get_zeroed_page(gfp_t gfp)
{
	unsigned long addr = __get_free_pages(gfp, 0);

	if (addr)
		memset((void *)addr, 0, PAGE_SIZE);
	return addr;
}

void free_pages(unsigned long addr, unsigned int order)
{
	if (!addr || order != 0)
		return;
	__free_page(virt_to_page((void *)addr));
}

/* Sleep until the deadline. The mode (absolute or relative) is upstream's;
 * both end in the same wait here, and a zero or past deadline returns at once
 * rather than sleeping forever. */
int schedule_hrtimeout(ktime_t *expires, int mode)
{
	u64 now = lkpi_monotonic_ns();
	u64 until;
	u64 jiffies_wanted;

	(void)mode;
	if (!expires)
		return 0;
	until = (u64)*expires;
	if (until <= now)
		return 0;
	/* The scheduler's resolution is the tick, and a jiffy is 10 ms of it, so
	 * a sub-jiffy deadline rounds up to one rather than to none: returning
	 * immediately would turn every short sleep into a spin. */
	jiffies_wanted = ((until - now) + 9999999ull) / 10000000ull;
	lkpi_sleep_jiffies(jiffies_wanted ? jiffies_wanted : 1);
	return 0;
}

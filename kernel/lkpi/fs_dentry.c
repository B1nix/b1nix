/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * linuxkpi: the dentry.
 *
 * A dentry is a name in a directory, and a cache of the answer to "does this
 * name exist, and what inode is it". A filesystem creates them in `lookup` and
 * hands them back through d_splice_alias or d_add.
 *
 * Two properties are load-bearing:
 *
 *   - A NEGATIVE dentry — one with no inode — is an ANSWER, not an absence. It
 *     records that the name does not exist, so a second lookup need not go to
 *     the disk. Treating it as "not cached" makes every miss hit the
 *     filesystem.
 *   - The reference count is the lifetime. `dget` takes one, `dput` drops one,
 *     and the tree holds one for every child, which is why a parent cannot go
 *     away while a child is alive.
 *
 * What is deliberately NOT here: the name hash table. Nothing in this shim
 * looks a dentry up by name — the filesystems reach them through the inode or
 * through the parent's child list, and the path walk that would need a hash
 * lives in b1nix's own VFS above the bridge.
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/namei.h>

static struct dentry *d_alloc_common(struct dentry *parent,
                                     const struct qstr *name)
{
	struct dentry *dentry = kzalloc(sizeof(*dentry), GFP_KERNEL);

	if (!dentry)
		return NULL;

	dentry->d_lockref.count = 1;
	spin_lock_init(&dentry->d_lockref.lock);
	INIT_HLIST_BL_NODE(&dentry->d_hash);
	INIT_LIST_HEAD(&dentry->d_child);
	INIT_LIST_HEAD(&dentry->d_subdirs);
	INIT_HLIST_NODE(&dentry->d_u);

	if (name && name->len) {
		if (name->len < DNAME_INLINE_LEN) {
			memcpy(dentry->d_iname, name->name, name->len);
			dentry->d_iname[name->len] = '\0';
			dentry->d_name.name = dentry->d_iname;
		} else {
			/*
			 * Longer than the inline buffer: a separate allocation, which is
			 * the only reason DNAME_INLINE_LEN exists. Copying into the
			 * inline array regardless would overrun the dentry.
			 */
			unsigned char *buf = kmalloc(name->len + 1, GFP_KERNEL);

			if (!buf) {
				kfree(dentry);
				return NULL;
			}
			memcpy(buf, name->name, name->len);
			buf[name->len] = '\0';
			dentry->d_name.name = buf;
		}
		dentry->d_name.len = name->len;
		dentry->d_name.hash = name->hash;
	} else {
		dentry->d_iname[0] = '/';
		dentry->d_iname[1] = '\0';
		dentry->d_name.name = dentry->d_iname;
		dentry->d_name.len = 1;
	}

	if (parent) {
		dentry->d_parent = parent;
		dentry->d_sb = parent->d_sb;
		dentry->d_op = parent->d_sb ? parent->d_sb->s_d_op : NULL;
		/* The parent holds a reference for as long as this child exists,
		 * which is what stops a directory being freed under its contents. */
		dget(parent);
		list_add(&dentry->d_child, &parent->d_subdirs);
	} else {
		/* Its own parent: that is what makes IS_ROOT true and what stops a
		 * path walk stepping off the top. */
		dentry->d_parent = dentry;
	}
	return dentry;
}

struct dentry *d_alloc(struct dentry *parent, const struct qstr *name)
{
	return d_alloc_common(parent, name);
}

struct dentry *d_alloc_anon(struct super_block *sb)
{
	struct dentry *dentry = d_alloc_common(NULL, NULL);

	if (dentry) {
		dentry->d_sb = sb;
		dentry->d_op = sb ? sb->s_d_op : NULL;
		dentry->d_flags |= DCACHE_DISCONNECTED;
	}
	return dentry;
}

struct dentry *dget(struct dentry *dentry)
{
	if (dentry)
		lockref_get(&dentry->d_lockref);
	return dentry;
}

struct dentry *dget_parent(struct dentry *dentry)
{
	return dentry ? dget(dentry->d_parent) : NULL;
}

void dput(struct dentry *dentry)
{
	while (dentry) {
		struct dentry *parent;

		if (lockref_put_return(&dentry->d_lockref) > 0)
			return;

		/*
		 * The last reference is gone. The inode's reference is dropped, the
		 * child is unlinked from its parent, and then the parent's reference
		 * is dropped — iteratively rather than recursively, because a deep
		 * tree collapsing at once would otherwise recurse as far as it is
		 * deep.
		 */
		if (dentry->d_op && dentry->d_op->d_release)
			dentry->d_op->d_release(dentry);
		if (dentry->d_inode) {
			/*
			 * Leave the inode's alias list first. The dentry is about to be
			 * freed, and an inode that still lists it hands the next walker
			 * a dead pointer — btrfs notices and warns about exactly this
			 * when it destroys the inode.
			 */
			if (!hlist_unhashed(&dentry->d_u))
				hlist_del_init(&dentry->d_u);
			if (dentry->d_op && dentry->d_op->d_iput)
				dentry->d_op->d_iput(dentry, dentry->d_inode);
			else
				iput(dentry->d_inode);
			dentry->d_inode = NULL;
		}
		if (!list_empty(&dentry->d_child))
			list_del_init(&dentry->d_child);
		if (dentry->d_name.name != dentry->d_iname)
			kfree(dentry->d_name.name);

		parent = (dentry->d_parent == dentry) ? NULL : dentry->d_parent;
		kfree(dentry);
		dentry = parent;
	}
}

/* ── attaching an inode ─────────────────────────────────────────── */

void d_instantiate(struct dentry *dentry, struct inode *inode)
{
	/* The dentry takes over the caller's reference on the inode; that is why
	 * a failed d_instantiate path must iput and a successful one must not. */
	dentry->d_inode = inode;
	if (inode)
		hlist_add_head(&dentry->d_u, &inode->i_dentry);
}

void d_instantiate_new(struct dentry *dentry, struct inode *inode)
{
	d_instantiate(dentry, inode);
	/* The inode was created with I_NEW; publishing it here is what lets any
	 * lookup that is waiting on it proceed. */
	unlock_new_inode(inode);
}

void d_add(struct dentry *dentry, struct inode *inode)
{
	d_instantiate(dentry, inode);
}

void d_tmpfile(struct file *file, struct inode *inode)
{
	/* A file with no name in any directory. The dentry exists so the file has
	 * something to point at, and is never hashed — which is what makes the
	 * inode unreachable by name while the descriptor is open. */
	if (file && file->f_path.dentry)
		d_instantiate(file->f_path.dentry, inode);
	else
		iput(inode);
}

/*
 * Attach an inode to a dentry, reusing an existing alias if the inode already
 * has one.
 *
 * The reuse is what makes a directory have exactly one dentry: a second name
 * for the same directory would give a path walk two answers for "where am I",
 * and the loop check would never fire. For a regular file, several aliases are
 * normal (hard links) and a fresh attach is right.
 */
struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry)
{
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	if (inode && S_ISDIR(inode->i_mode)) {
		struct dentry *alias = d_find_any_alias(inode);

		if (alias && alias != dentry) {
			/* The directory is already known by another dentry. The caller's
			 * is discarded and the existing one returned — that is the whole
			 * point of the "splice". */
			iput(inode);
			return alias;
		}
		dput(alias);
	}

	d_instantiate(dentry, inode);
	return NULL;
}

struct dentry *d_obtain_alias(struct inode *inode)
{
	struct dentry *dentry;

	if (IS_ERR(inode))
		return ERR_CAST(inode);
	if (!inode)
		return ERR_PTR(-ESTALE);

	dentry = d_find_any_alias(inode);
	if (dentry) {
		/* Already known by a name: that one is returned, and the caller's
		 * reference on the inode is released because the dentry holds one. */
		iput(inode);
		return dentry;
	}
	dentry = d_alloc_anon(inode->i_sb);
	if (!dentry) {
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	d_instantiate(dentry, inode);
	return dentry;
}

struct dentry *d_obtain_root(struct inode *inode)
{
	return d_obtain_alias(inode);
}

struct dentry *d_make_root(struct inode *root_inode)
{
	struct dentry *res;

	if (!root_inode)
		return NULL;
	res = d_alloc_anon(root_inode->i_sb);
	if (!res) {
		iput(root_inode);
		return NULL;
	}
	d_instantiate(res, root_inode);
	return res;
}

struct dentry *d_find_any_alias(struct inode *inode)
{
	struct dentry *alias;

	if (!inode || hlist_empty(&inode->i_dentry))
		return NULL;
	alias = hlist_entry(inode->i_dentry.first, struct dentry, d_u);
	return dget(alias);
}

struct dentry *d_find_alias(struct inode *inode)
{
	return d_find_any_alias(inode);
}

void d_prune_aliases(struct inode *inode)
{
	/* Drop the dentries nothing else is holding. Only the unreferenced ones:
	 * one somebody has open is not ours to remove. */
	struct hlist_node *n;
	struct dentry *dentry;

	hlist_for_each_entry_safe(dentry, n, &inode->i_dentry, d_u) {
		if (dentry->d_lockref.count == 0)
			dput(dentry);
	}
}

/* ── removing ───────────────────────────────────────────────────── */

void d_drop(struct dentry *dentry)
{
	/* Off the hash: the name is no longer an answer, and the next lookup goes
	 * to the filesystem. The dentry itself lives until its references go. */
	if (!d_unhashed(dentry))
		hlist_bl_del_init(&dentry->d_hash);
}

void d_delete(struct dentry *dentry)
{
	/*
	 * The name is gone from the directory. The dentry becomes negative
	 * rather than being freed, because that IS the cached answer: the name
	 * does not exist.
	 */
	if (dentry->d_inode) {
		iput(dentry->d_inode);
		dentry->d_inode = NULL;
		if (!hlist_unhashed(&dentry->d_u))
			hlist_del_init(&dentry->d_u);
	}
}

void d_invalidate(struct dentry *dentry)
{
	d_drop(dentry);
}

void d_delete_notify(struct inode *dir, struct dentry *dentry)
{
	(void)dir;
	d_delete(dentry);
}

void d_mark_dontcache(struct inode *inode)
{
	inode->i_state |= I_DONTCACHE;
}

void d_move(struct dentry *dentry, struct dentry *target)
{
	/*
	 * A rename: the dentry takes the target's name and place. The name is
	 * swapped rather than copied when both are inline, because the target's
	 * buffer is inside the target's own allocation.
	 */
	struct dentry *old_parent = dentry->d_parent;

	if (target->d_name.name == target->d_iname) {
		memcpy(dentry->d_iname, target->d_iname, DNAME_INLINE_LEN);
		if (dentry->d_name.name != dentry->d_iname)
			kfree(dentry->d_name.name);
		dentry->d_name.name = dentry->d_iname;
	} else {
		if (dentry->d_name.name != dentry->d_iname)
			kfree(dentry->d_name.name);
		dentry->d_name.name = target->d_name.name;
		target->d_name.name = target->d_iname;
	}
	dentry->d_name.len = target->d_name.len;
	dentry->d_name.hash = target->d_name.hash;

	if (dentry->d_parent != target->d_parent) {
		list_del_init(&dentry->d_child);
		dentry->d_parent = target->d_parent;
		dget(target->d_parent);
		list_add(&dentry->d_child, &target->d_parent->d_subdirs);
		dput(old_parent);
	}
	d_drop(target);
}

void d_set_d_op(struct dentry *dentry, const struct dentry_operations *op)
{
	dentry->d_op = op;
	dentry->d_flags &= ~(DCACHE_OP_HASH | DCACHE_OP_COMPARE |
	                     DCACHE_OP_REVALIDATE | DCACHE_OP_DELETE);
	if (!op)
		return;
	if (op->d_hash)
		dentry->d_flags |= DCACHE_OP_HASH;
	if (op->d_compare)
		dentry->d_flags |= DCACHE_OP_COMPARE;
	if (op->d_revalidate)
		dentry->d_flags |= DCACHE_OP_REVALIDATE;
	if (op->d_delete)
		dentry->d_flags |= DCACHE_OP_DELETE;
}

int d_set_mounted(struct dentry *dentry)
{
	(void)dentry;
	/* Mount points are b1nix's VFS's business, above the bridge. */
	return 0;
}

void shrink_dcache_sb(struct super_block *sb)
{
	(void)sb;
	/* Nothing to shrink: this cache holds only what a filesystem explicitly
	 * created, and drops each one when its last reference goes. */
}

void generic_set_encrypted_ci_d_ops(struct dentry *dentry)
{
	/* Case-insensitive and encrypted directories both need their own hash and
	 * compare. Neither is supported (see <linux/unicode.h> and
	 * <linux/fscrypt.h>), so a dentry under one keeps the ordinary
	 * byte-for-byte comparison — which is why such a filesystem is refused at
	 * mount rather than mounted with the wrong comparison. */
	(void)dentry;
}

/*
 * Render a path from a dentry, walking up to the root.
 *
 * Built backwards into the end of the buffer, which is what makes it one pass:
 * the components are discovered leaf-first and printed root-first.
 */
char *dentry_path_raw(const struct dentry *dentry, char *buf, int buflen)
{
	char *end = buf + buflen;
	const struct dentry *d = dentry;

	if (buflen < 2)
		return ERR_PTR(-ENAMETOOLONG);
	*--end = '\0';

	while (d && !IS_ROOT(d)) {
		int len = (int)d->d_name.len;

		if (end - buf < len + 1)
			return ERR_PTR(-ENAMETOOLONG);
		end -= len;
		memcpy(end, d->d_name.name, (size_t)len);
		*--end = '/';
		d = d->d_parent;
	}
	if (end == buf + buflen - 1)
		*--end = '/';
	return end;
}

char *d_path(const struct path *path, char *buf, int buflen)
{
	return dentry_path_raw(path->dentry, buf, buflen);
}

char *file_path(struct file *file, char *buf, int buflen)
{
	return d_path(&file->f_path, buf, buflen);
}

/* ── paths ──────────────────────────────────────────────────────── */

void path_get(const struct path *path)
{
	if (path && path->dentry)
		dget(path->dentry);
}

void path_put(const struct path *path)
{
	if (path && path->dentry)
		dput(path->dentry);
}

/*
 * Resolve a path to a (mount, dentry) pair.
 *
 * Declared and deliberately unimplemented: resolution belongs to b1nix's VFS,
 * which is above this layer and does not produce these objects. btrfs uses it
 * for one thing — turning a device path given as a mount option into a device —
 * and the bridge resolves that itself before the filesystem is entered.
 *
 * Returning -ENOENT rather than pretending: a filesystem that gets a path back
 * would then walk a tree that does not exist.
 */
int kern_path(const char *name, unsigned int flags, struct path *path)
{
	(void)name;
	(void)flags;
	(void)path;
	return -ENOENT;
}

struct dentry *lookup_one(struct mnt_idmap *idmap, const char *name,
                          struct dentry *base, int len)
{
	struct qstr q = { { { .hash = 0, .len = (u32)len } }, .name =
	                      (const unsigned char *)name };
	struct dentry *dentry;

	(void)idmap;
	if (!base || !base->d_inode || !base->d_inode->i_op ||
	    !base->d_inode->i_op->lookup)
		return ERR_PTR(-ENOTDIR);

	dentry = d_alloc(base, &q);
	if (!dentry)
		return ERR_PTR(-ENOMEM);
	/* The filesystem fills it in, and may return a different dentry — which
	 * is then the one to use, and ours is dropped. */
	{
		struct dentry *res = base->d_inode->i_op->lookup(base->d_inode,
		                                                 dentry, 0);

		if (IS_ERR(res)) {
			dput(dentry);
			return res;
		}
		if (res) {
			dput(dentry);
			return res;
		}
	}
	return dentry;
}

struct dentry *lookup_one_len(const char *name, struct dentry *base, int len)
{
	return lookup_one(&nop_mnt_idmap, name, base, len);
}

struct dentry *lookup_one_len_unlocked(const char *name, struct dentry *base,
                                       int len)
{
	return lookup_one(&nop_mnt_idmap, name, base, len);
}

struct dentry *lookup_positive_unlocked(const char *name, struct dentry *base,
                                        int len)
{
	struct dentry *dentry = lookup_one(&nop_mnt_idmap, name, base, len);

	if (!IS_ERR(dentry) && d_is_negative(dentry)) {
		/* "Positive" means the name exists. A negative answer is turned into
		 * ENOENT here so the caller does not have to check twice. */
		dput(dentry);
		return ERR_PTR(-ENOENT);
	}
	return dentry;
}

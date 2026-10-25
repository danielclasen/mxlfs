// SPDX-License-Identifier: GPL-2.0-only
/*
 * mxlfs - a memory filesystem for MXL domains.
 *
 * Regular files keep their data in unevictable page cache, allocated in the
 * largest folios that fit (PMD-sized where alignment allows), so ring buffers
 * map with 2 MiB entries and register with few NIC translation entries.
 * fallocate allocates at once, except for processes that asked for lazy
 * allocation (MXLFS_IOC_LAZY_ALLOC): their fallocate only sets the size, and
 * pages appear on first touch, PMD-sized through the fault path. That makes
 * a flow whose ring takes no memory until something writes or maps it.
 * Such a file faults in 4 KiB pages, so writing a header allocates only
 * that page; a later fallocate from any other process allocates the rest
 * in large folios and folds the pages already there into them.
 * There is no writeback and no page_mkwrite, so shared writable mappings can
 * be long-term pinned for RDMA.
 *
 * A view is a file whose pages are those of several equally sized source
 * files, each page taken from the source selected for it. Selecting another
 * source for a range zaps the view's mappings there; readers refault onto
 * the new source's pages. Replacing a source with another file zaps the
 * pages that select it the same way. No data is copied.
 *
 * With the wait= mount option, a path walk through a missing "*.mxl-flow"
 * name waits up to that many milliseconds for the flow to appear, instead
 * of failing with ENOENT, and a privileged process is told which flow is
 * wanted (MXLFS_IOC_WANTED_NEXT). The walk waits in ->d_manage, outside
 * the directory lock, so the writer that creates the flow is not blocked;
 * once the flow appears the walk is retried and finds it.
 */

#include <linux/backing-dev.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/file.h>
#include <linux/huge_mm.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/uio.h>
#include <linux/falloc.h>
#include <linux/namei.h>
#include <linux/sched/signal.h>

#include "mxlfs_uapi.h"

#ifndef CONFIG_TRANSPARENT_HUGEPAGE
#error "mxlfs needs CONFIG_TRANSPARENT_HUGEPAGE for large page cache folios"
#endif
#ifdef CONFIG_HIGHMEM
#error "mxlfs copies into large folios through the direct map"
#endif

#define MXLFS_DEFAULT_MODE 0755

struct mxlfs_sb_info {
	umode_t mode;
	u64 limit; /* bytes; 0 means unlimited (size=0) */
	unsigned int wait_ms; /* 0 disables waiting for missing flows */
	atomic64_t allocated;
	atomic64_t view_selects;
	atomic64_t view_faults;
	atomic64_t view_replaces;

	spinlock_t lazy_lock;
	struct list_head lazy;	/* struct mxlfs_lazy: thread groups with lazy fallocate */

	spinlock_t wanted_lock;
	struct list_head wanted;	/* struct mxlfs_wanted, oldest first */
	wait_queue_head_t wanted_wq;	/* MXLFS_IOC_WANTED_NEXT callers */
	u64 wanted_seq;
};

struct mxlfs_lazy {
	struct list_head node;
	struct pid *tgid;
};

enum { MXLFS_WANT_PENDING, MXLFS_WANT_APPEARED, MXLFS_WANT_FAILED };

/* A missing flow that path walks are waiting for; under wanted_lock. */
struct mxlfs_wanted {
	struct list_head node;
	struct inode *dir;
	char *name;
	unsigned int len;
	char *path;		/* from the filesystem root */
	u64 id;
	struct pid *pid;	/* first waiter's thread group */
	unsigned int waiters;
	bool reported;
	int state;
	int error;
	wait_queue_head_t wq;
};

struct mxlfs_view {
	unsigned int count;
	/* Inode references, not files: a file would pin the mount. */
	struct inode *sources[MXLFS_VIEW_MAX_SOURCES];
	pgoff_t pages;
	u8 *sel;	/* source per page; under the view's invalidate_lock */
};

struct mxlfs_inode {
	struct inode vfs_inode;
	atomic_long_t charged;	/* pages charged to the superblock */
	bool lazy;		/* sized by a lazy fallocate, not yet allocated */
	atomic_t view_users;	/* views that use this file as a source */
	struct mxlfs_view *view;
};

static struct kmem_cache *mxlfs_inode_cachep;

static inline struct mxlfs_inode *MXLFS_I(struct inode *inode)
{
	return container_of(inode, struct mxlfs_inode, vfs_inode);
}

static inline struct mxlfs_sb_info *MXLFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static const struct super_operations mxlfs_ops;
static const struct address_space_operations mxlfs_aops;
static const struct inode_operations mxlfs_dir_inode_operations;
static const struct inode_operations mxlfs_file_inode_operations;
static const struct file_operations mxlfs_file_operations;
static const struct file_operations mxlfs_dir_operations;

/*
 * Brings the superblock's allocated bytes in line with the inode's page
 * cache. Anything that adds or removes pages calls it afterwards; each call
 * settles whatever changed since the last, so racing callers converge.
 */
static void mxlfs_settle(struct inode *inode)
{
	long now = READ_ONCE(inode->i_mapping->nrpages);
	long was = atomic_long_xchg(&MXLFS_I(inode)->charged, now);

	if (now != was)
		atomic64_add((s64)(now - was) << PAGE_SHIFT,
			     &MXLFS_SB(inode->i_sb)->allocated);
}

/* Whether bytes more would exceed size=. Checked before allocating. */
static bool mxlfs_over_limit(struct mxlfs_sb_info *sbi, u64 bytes)
{
	return sbi->limit && atomic64_read(&sbi->allocated) + bytes > sbi->limit;
}

/* Drops entries of thread groups that have exited; under lazy_lock. */
static void mxlfs_lazy_prune(struct mxlfs_sb_info *sbi)
{
	struct mxlfs_lazy *l, *n;

	list_for_each_entry_safe(l, n, &sbi->lazy, node) {
		if (pid_has_task(l->tgid, PIDTYPE_TGID))
			continue;
		list_del(&l->node);
		put_pid(l->tgid);
		kfree(l);
	}
}

static bool mxlfs_lazy(struct mxlfs_sb_info *sbi)
{
	struct pid *me = task_tgid(current);
	struct mxlfs_lazy *l;
	bool found = false;

	spin_lock(&sbi->lazy_lock);
	list_for_each_entry(l, &sbi->lazy, node)
		if (l->tgid == me)
			found = true;
	spin_unlock(&sbi->lazy_lock);
	return found;
}

/*
 * Needs no privilege: it changes only how the caller's own fallocate calls
 * allocate, and the list cannot grow past the thread groups alive.
 */
static long mxlfs_set_lazy(struct mxlfs_sb_info *sbi, u32 __user *arg)
{
	struct pid *me = task_tgid(current);
	struct mxlfs_lazy *l, *n, *add;
	u32 on;

	if (get_user(on, arg))
		return -EFAULT;
	add = on ? kmalloc(sizeof(*add), GFP_KERNEL) : NULL;
	if (on && !add)
		return -ENOMEM;

	spin_lock(&sbi->lazy_lock);
	mxlfs_lazy_prune(sbi);
	list_for_each_entry_safe(l, n, &sbi->lazy, node) {
		if (l->tgid != me)
			continue;
		if (on)
			goto out;	/* already lazy */
		list_del(&l->node);
		put_pid(l->tgid);
		kfree(l);
		goto out;
	}
	if (on) {
		add->tgid = get_pid(me);
		list_add(&add->node, &sbi->lazy);
		add = NULL;
	}
out:
	spin_unlock(&sbi->lazy_lock);
	kfree(add);
	return 0;
}

static struct inode *mxlfs_get_inode(struct super_block *sb,
				     const struct inode *dir, umode_t mode)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_mapping->a_ops = &mxlfs_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
#ifdef CONFIG_CGROUP_WRITEBACK
	/*
	 * Shrinking a file splits the large folio across the new size, and
	 * the split accounts the dropped part of a dirty folio as cleaned
	 * against inode_to_wb(), which is i_wb. On a bdi without writeback
	 * only marking the inode dirty sets i_wb, and a file that was
	 * created, fallocated and written through a mapping need not have
	 * been, so the split would dereference NULL. This is the wb that
	 * __inode_attach_wb() picks on such a bdi; it holds no reference,
	 * and eviction detaches it.
	 */
	inode->i_wb = &inode_to_bdi(inode)->wb;
#endif
	simple_inode_init_ts(inode);

	switch (mode & S_IFMT) {
	case S_IFREG:
		mapping_set_large_folios(inode->i_mapping);
		inode->i_op = &mxlfs_file_inode_operations;
		inode->i_fop = &mxlfs_file_operations;
		break;
	case S_IFDIR:
		inode->i_op = &mxlfs_dir_inode_operations;
		inode->i_fop = &mxlfs_dir_operations;
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		break;
	default:
		init_special_inode(inode, mode, 0);
		break;
	}
	return inode;
}

/* Waiting for missing flows */

static bool mxlfs_is_flow_name(const struct qstr *name)
{
	static const char suffix[] = ".mxl-flow";
	const unsigned int n = sizeof(suffix) - 1;

	return name->len > n && name->name[0] != '.' &&
	       !memcmp(name->name + name->len - n, suffix, n);
}

static struct mxlfs_wanted *mxlfs_wanted_find(struct mxlfs_sb_info *sbi,
					      struct inode *dir,
					      const struct qstr *name)
{
	struct mxlfs_wanted *w;

	lockdep_assert_held(&sbi->wanted_lock);
	list_for_each_entry(w, &sbi->wanted, node)
		if (w->dir == dir && w->len == name->len &&
		    !memcmp(w->name, name->name, name->len))
			return w;
	return NULL;
}

static void mxlfs_wanted_free(struct mxlfs_wanted *w)
{
	iput(w->dir);
	put_pid(w->pid);
	kfree(w->name);
	kfree(w->path);
	kfree(w);
}

/* Wakes the walks waiting for name in dir, which now exists. */
static void mxlfs_wanted_appeared(struct inode *dir, const struct qstr *name)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(dir->i_sb);
	struct mxlfs_wanted *w;

	if (!READ_ONCE(sbi->wait_ms) || !mxlfs_is_flow_name(name))
		return;
	spin_lock(&sbi->wanted_lock);
	w = mxlfs_wanted_find(sbi, dir, name);
	if (w && w->state == MXLFS_WANT_PENDING) {
		w->state = MXLFS_WANT_APPEARED;
		wake_up_all(&w->wq);
	}
	spin_unlock(&sbi->wanted_lock);
}

static void mxlfs_clear_managed(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	dentry->d_flags &= ~DCACHE_MANAGE_TRANSIT;
	spin_unlock(&dentry->d_lock);
}

static int mxlfs_wait_for(struct dentry *dentry)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(dentry->d_sb);
	unsigned long timeout = msecs_to_jiffies(READ_ONCE(sbi->wait_ms));
	struct mxlfs_wanted *w, *nw;
	struct dentry *parent;
	char *buf, *path;
	int state, error, ret;
	bool last;
	long left;

	if (!timeout)
		return 0;

	nw = kzalloc(sizeof(*nw), GFP_KERNEL);
	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!nw || !buf) {
		ret = -ENOMEM;
		goto out_free;
	}
	path = dentry_path_raw(dentry, buf, PATH_MAX);
	if (IS_ERR(path)) {
		ret = PTR_ERR(path);
		goto out_free;
	}
	nw->path = kstrdup(path, GFP_KERNEL);
	nw->name = kmemdup(dentry->d_name.name, dentry->d_name.len, GFP_KERNEL);
	if (!nw->path || !nw->name) {
		ret = -ENOMEM;
		goto out_free;
	}
	nw->len = dentry->d_name.len;
	init_waitqueue_head(&nw->wq);

	parent = dget_parent(dentry);
	spin_lock(&sbi->wanted_lock);
	w = mxlfs_wanted_find(sbi, d_inode(parent), &dentry->d_name);
	if (!w) {
		w = nw;
		nw = NULL;
		w->dir = igrab(d_inode(parent));
		w->id = ++sbi->wanted_seq;
		w->pid = get_pid(task_tgid(current));
		list_add_tail(&w->node, &sbi->wanted);
		wake_up_interruptible(&sbi->wanted_wq);
	}
	w->waiters++;
	spin_unlock(&sbi->wanted_lock);
	dput(parent);

	/*
	 * A rename onto the name unhashes this negative dentry; a create in
	 * place makes it positive. Either may have happened before the entry
	 * was listed, so both are part of the condition.
	 */
	left = wait_event_killable_timeout(w->wq,
			READ_ONCE(w->state) != MXLFS_WANT_PENDING ||
			d_unhashed(dentry) || d_really_is_positive(dentry),
			timeout);

	spin_lock(&sbi->wanted_lock);
	state = w->state;
	error = w->error;
	last = --w->waiters == 0;
	if (last)
		list_del(&w->node);
	spin_unlock(&sbi->wanted_lock);
	if (last)
		mxlfs_wanted_free(w);

	if (d_really_is_positive(dentry))
		ret = 0;
	else if (d_unhashed(dentry) || state == MXLFS_WANT_APPEARED)
		ret = -ESTALE;	/* the walk is retried and finds the flow */
	else if (left < 0)
		ret = left;
	else if (state == MXLFS_WANT_FAILED)
		ret = error;
	else
		ret = -ENOENT;
out_free:
	if (nw) {
		kfree(nw->path);
		kfree(nw->name);
		kfree(nw);
	}
	kfree(buf);
	return ret;
}

static int mxlfs_d_manage(const struct path *path, bool rcu)
{
	struct dentry *dentry = path->dentry;

	if (d_really_is_positive(dentry))
		return 0;
	if (rcu)
		return -ECHILD;
	return mxlfs_wait_for(dentry);
}

static const struct dentry_operations mxlfs_dentry_operations = {
	.d_manage	= mxlfs_d_manage,
};

static struct dentry *mxlfs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(dir->i_sb);

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	/* A create or rename target must see the name as absent, at once. */
	if (READ_ONCE(sbi->wait_ms) && mxlfs_is_flow_name(&dentry->d_name) &&
	    !(flags & (LOOKUP_CREATE | LOOKUP_EXCL | LOOKUP_RENAME_TARGET))) {
		spin_lock(&dentry->d_lock);
		dentry->d_flags |= DCACHE_MANAGE_TRANSIT;
		spin_unlock(&dentry->d_lock);
	}
	d_add(dentry, NULL);
	return NULL;
}

/* Directories */

static int mxlfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode = mxlfs_get_inode(dir->i_sb, dir, mode);
	int error;

	if (!inode)
		return -ENOSPC;
	if (!S_ISREG(mode) && !S_ISDIR(mode) && !S_ISLNK(mode))
		init_special_inode(inode, mode, dev);

	error = security_inode_init_security(inode, dir, &dentry->d_name,
					     NULL, NULL);
	if (error) {
		iput(inode);
		return error;
	}
	mxlfs_clear_managed(dentry);
	d_instantiate(dentry, inode);
	dget(dentry); /* pin the dentry, as ramfs does */
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mxlfs_wanted_appeared(dir, &dentry->d_name);
	return 0;
}

static struct dentry *mxlfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	int error = mxlfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);

	if (!error)
		inc_nlink(dir);
	return ERR_PTR(error);
}

static int mxlfs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	return mxlfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static int mxlfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	int error;

	inode = mxlfs_get_inode(dir->i_sb, dir, S_IFLNK | 0777);
	if (!inode)
		return -ENOSPC;
	error = security_inode_init_security(inode, dir, &dentry->d_name,
					     NULL, NULL);
	if (!error)
		error = page_symlink(inode, symname, strlen(symname) + 1);
	if (error) {
		iput(inode);
		return error;
	}
	mxlfs_clear_managed(dentry);
	d_instantiate(dentry, inode);
	dget(dentry);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mxlfs_wanted_appeared(dir, &dentry->d_name);
	return 0;
}

static int mxlfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir,
			 struct file *file, umode_t mode)
{
	struct inode *inode;
	int error;

	inode = mxlfs_get_inode(dir->i_sb, dir, mode);
	if (!inode)
		return -ENOSPC;
	error = security_inode_init_security(inode, dir,
					     &file_dentry(file)->d_name,
					     NULL, NULL);
	if (error)
		iput(inode);
	else
		d_tmpfile(file, inode);
	return finish_open_simple(file, error);
}

static int mxlfs_link(struct dentry *old_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	int error;

	mxlfs_clear_managed(dentry);
	error = simple_link(old_dentry, dir, dentry);
	if (!error)
		mxlfs_wanted_appeared(dir, &dentry->d_name);
	return error;
}

static int mxlfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
			struct dentry *old_dentry, struct inode *new_dir,
			struct dentry *new_dentry, unsigned int flags)
{
	struct qstr name = QSTR_LEN(new_dentry->d_name.name, new_dentry->d_name.len);
	char buf[NAME_MAX + 1];
	int error;

	/* d_move gives new_dentry the old name; keep the one being created. */
	memcpy(buf, name.name, name.len);
	name.name = buf;
	error = simple_rename(idmap, old_dir, old_dentry, new_dir, new_dentry, flags);
	if (!error)
		mxlfs_wanted_appeared(new_dir, &name);
	return error;
}

static const struct inode_operations mxlfs_dir_inode_operations = {
	.create		= mxlfs_create,
	.lookup		= mxlfs_lookup,
	.link		= mxlfs_link,
	.unlink		= simple_unlink,
	.symlink	= mxlfs_symlink,
	.mkdir		= mxlfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= mxlfs_mknod,
	.rename		= mxlfs_rename,
	.tmpfile	= mxlfs_tmpfile,
};

/* Regular files */

/* The largest folio order at index that is aligned and ends by end. */
static unsigned int mxlfs_fit_order(pgoff_t index, pgoff_t end)
{
	unsigned int order = min_t(unsigned int, HPAGE_PMD_ORDER,
				   MAX_PAGECACHE_ORDER);

	while (order > 0 &&
	       (!IS_ALIGNED(index, 1UL << order) || index + (1UL << order) > end))
		order--;
	return order;
}

/*
 * Replaces the small folios in [index, index + 2^order) with one folio of
 * that order, copying their contents; holes stay zero. Mappings of the
 * range are zapped first and refault onto the new folio. Called with the
 * inode locked, which keeps write() out; the invalidate lock keeps faults
 * and reads from instantiating pages meanwhile.
 */
static int mxlfs_fold(struct inode *inode, pgoff_t index, unsigned int order, gfp_t gfp)
{
	struct address_space *mapping = inode->i_mapping;
	pgoff_t nr = 1UL << order, end = index + nr - 1, next = index;
	struct folio_batch fbatch;
	struct folio *large;
	unsigned int i;
	int err = 0;

	large = filemap_alloc_folio(gfp | __GFP_ZERO | __GFP_NORETRY | __GFP_NOWARN, order);
	if (!large)
		return -ENOMEM;

	filemap_invalidate_lock(mapping);
	unmap_mapping_pages(mapping, index, nr, false);
	folio_batch_init(&fbatch);
	while (!err && filemap_get_folios(mapping, &next, end, &fbatch)) {
		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *f = fbatch.folios[i];

			if (folio_test_large(f) || folio_maybe_dma_pinned(f) ||
			    folio_mapped(f)) {
				err = -EBUSY;
				break;
			}
			folio_lock(f);
			memcpy_from_folio(folio_address(large) +
					  ((f->index - index) << PAGE_SHIFT),
					  f, 0, PAGE_SIZE);
			folio_unlock(f);
		}
		folio_batch_release(&fbatch);
	}
	if (!err) {
		truncate_inode_pages_range(mapping, (loff_t)index << PAGE_SHIFT,
					   ((loff_t)(end + 1) << PAGE_SHIFT) - 1);
		/* Nothing can have refilled the range under both locks. */
		err = filemap_add_folio(mapping, large, index, gfp);
		if (WARN_ON_ONCE(err)) {
			/* The copied contents are lost; keep the range zero-filled. */
			folio_put(large);
			goto out;
		}
		folio_mark_uptodate(large);
		folio_mark_dirty(large);
		folio_unlock(large);
	}
	folio_put(large);
out:
	filemap_invalidate_unlock(mapping);
	mxlfs_settle(inode);
	return err;
}

static long mxlfs_allocate(struct file *file, int mode, loff_t offset,
			   loff_t len, bool lazy)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	struct mxlfs_inode *mi = MXLFS_I(inode);
	struct mxlfs_sb_info *sbi = MXLFS_SB(inode->i_sb);
	gfp_t gfp = mapping_gfp_mask(mapping);
	loff_t new_size = offset + len;
	pgoff_t index, end;
	long ret;

	if (mode & ~FALLOC_FL_KEEP_SIZE)
		return -EOPNOTSUPP;

	inode_lock(inode);
	ret = -EPERM;
	if (mi->view)
		goto out;
	ret = inode_newsize_ok(inode, new_size);
	if (ret)
		goto out;

	index = offset >> PAGE_SHIFT;
	end = DIV_ROUND_UP(new_size, PAGE_SIZE);
	if (lazy) {
		if (!mapping->nrpages)
			mi->lazy = true;
		index = end;
	} else {
		mi->lazy = false;
	}
	while (index < end) {
		struct folio *folio = filemap_get_folio(mapping, index);
		unsigned int order = mxlfs_fit_order(index, end);

		if (!IS_ERR(folio)) {
			pgoff_t next = folio_next_index(folio);

			/* Pages a lazily sized file got on touch become one folio. */
			if (folio_order(folio) < order && folio->index == index &&
			    !mxlfs_over_limit(sbi, PAGE_SIZE << order) &&
			    !mxlfs_fold(inode, index, order, gfp))
				next = index + (1UL << order);
			folio_put(folio);
			index = next;
			continue;
		}

		for (;;) {
			u64 bytes = PAGE_SIZE << order;
			int err;

			if (mxlfs_over_limit(sbi, bytes)) {
				ret = -ENOSPC;
				goto out;
			}
			folio = filemap_alloc_folio(gfp | __GFP_ZERO |
					(order ? __GFP_NORETRY | __GFP_NOWARN : 0),
					order);
			err = folio ? filemap_add_folio(mapping, folio, index, gfp)
				    : -ENOMEM;
			if (!err)
				break;
			if (folio)
				folio_put(folio);
			if (order == 0) {
				ret = err;
				goto out;
			}
			/* No large folio, or a smaller one already overlaps. */
			order = 0;
		}
		folio_mark_uptodate(folio);
		folio_mark_dirty(folio);
		folio_unlock(folio);
		mxlfs_settle(inode);
		index = folio_next_index(folio);
		folio_put(folio);
		cond_resched();
	}

	if (!(mode & FALLOC_FL_KEEP_SIZE) && new_size > i_size_read(inode))
		i_size_write(inode, new_size);
	inode_set_ctime_current(inode);
	ret = 0;
out:
	mxlfs_settle(inode);
	inode_unlock(inode);
	return ret;
}

static long mxlfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	return mxlfs_allocate(file, mode, offset, len,
			      mxlfs_lazy(MXLFS_SB(file_inode(file)->i_sb)));
}

static int mxlfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			 struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct mxlfs_inode *mi = MXLFS_I(inode);
	int error;

	if ((attr->ia_valid & ATTR_SIZE) && attr->ia_size != i_size_read(inode) &&
	    (mi->view || atomic_read(&mi->view_users)))
		return -EBUSY;

	error = simple_setattr(idmap, dentry, attr);
	if (error)
		return error;

	if (attr->ia_valid & ATTR_SIZE)
		mxlfs_settle(inode);
	return 0;
}

static const struct inode_operations mxlfs_file_inode_operations = {
	.setattr	= mxlfs_setattr,
	.getattr	= simple_getattr,
};

/*
 * Pages that appear outside fallocate: faults and reads on holes come
 * through read_folio, write() through write_begin. Both are checked
 * against size= first, so the limit can be overshot by at most one folio
 * per racing caller.
 */
static int mxlfs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	int err = 0;

	if (mxlfs_over_limit(MXLFS_SB(inode->i_sb), 0))
		err = -ENOSPC;
	if (err) {
		folio_unlock(folio);
		return err;
	}
	folio_zero_range(folio, 0, folio_size(folio));
	flush_dcache_folio(folio);
	folio_mark_uptodate(folio);
	folio_mark_dirty(folio);
	folio_unlock(folio);
	mxlfs_settle(inode);
	return 0;
}

static int mxlfs_write_begin(const struct kiocb *iocb, struct address_space *mapping,
			     loff_t pos, unsigned int len, struct folio **foliop,
			     void **fsdata)
{
	int err;

	if (mxlfs_over_limit(MXLFS_SB(mapping->host->i_sb), len))
		return -ENOSPC;
	err = simple_write_begin(iocb, mapping, pos, len, foliop, fsdata);
	if (!err)
		mxlfs_settle(mapping->host);
	return err;
}

/* simple_write_end is static in 6.18; ram_aops carries it. */
static int mxlfs_write_end(const struct kiocb *iocb, struct address_space *mapping,
			   loff_t pos, unsigned int len, unsigned int copied,
			   struct folio *folio, void *fsdata)
{
	return ram_aops.write_end(iocb, mapping, pos, len, copied, folio, fsdata);
}

static const struct address_space_operations mxlfs_aops = {
	.read_folio	= mxlfs_read_folio,
	.write_begin	= mxlfs_write_begin,
	.write_end	= mxlfs_write_end,
	.dirty_folio	= noop_dirty_folio,
};

/* No page_mkwrite: shared writable mappings need no dirty tracking. */
static const struct vm_operations_struct mxlfs_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
};

/* Views */

static struct inode *mxlfs_view_source(struct mxlfs_view *view, pgoff_t index)
{
	return view->sources[view->sel[index]];
}

/* Whether every page of the view in [index, index + nr) selects source. */
static bool mxlfs_view_uniform(struct mxlfs_view *view, pgoff_t index,
			       unsigned long nr, u8 source)
{
	if (index + nr > view->pages)
		return false;
	return !memchr_inv(view->sel + index, source, nr);
}

/*
 * When every page the source folio covers selects that source, returns the
 * folio locked, so the core maps it as it maps page cache: a PMD-sized folio
 * with one PMD, any other large folio with all of its PTEs in one fault.
 * Otherwise maps the one page. Either way the rmap entries land on the
 * source's folio.
 */
static vm_fault_t mxlfs_view_fault(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct mxlfs_view *view = MXLFS_I(inode)->view;
	struct folio *folio;
	vm_fault_t ret = VM_FAULT_LOCKED;
	u8 source;

	if (vmf->pgoff >= view->pages)
		return VM_FAULT_SIGBUS;

	filemap_invalidate_lock_shared(inode->i_mapping);
	source = view->sel[vmf->pgoff];
	folio = filemap_get_folio(view->sources[source]->i_mapping, vmf->pgoff);
	if (IS_ERR(folio)) {
		/* A hole in the source; sources are expected to be allocated. */
		ret = VM_FAULT_SIGBUS;
	} else if (mxlfs_view_uniform(view, folio->index, folio_nr_pages(folio), source)) {
		/* Held until the core has mapped it; see mxlfs_view_zap. */
		folio_lock(folio);
		vmf->page = folio_file_page(folio, vmf->pgoff);
	} else {
		/* The invalidate lock keeps a zap from passing this insert. */
		ret = vmf_insert_page_mkwrite(vmf, folio_file_page(folio, vmf->pgoff),
					      vmf->flags & FAULT_FLAG_WRITE);
		folio_put(folio);
	}
	if (!(ret & VM_FAULT_ERROR))
		atomic64_inc(&MXLFS_SB(inode->i_sb)->view_faults);
	filemap_invalidate_unlock_shared(inode->i_mapping);
	return ret;
}

static const struct vm_operations_struct mxlfs_view_vm_ops = {
	.fault		= mxlfs_view_fault,
};

static void mxlfs_view_free(struct mxlfs_view *view)
{
	unsigned int i;

	for (i = 0; i < view->count; i++) {
		atomic_dec(&MXLFS_I(view->sources[i])->view_users);
		iput(view->sources[i]);
	}
	kvfree(view->sel);
	kfree(view);
}

static long mxlfs_view_set_sources(struct file *file, void __user *arg)
{
	struct inode *inode = file_inode(file);
	struct mxlfs_inode *mi = MXLFS_I(inode);
	struct mxlfs_view_sources req;
	struct mxlfs_view *view;
	unsigned int i;
	long ret;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.flags || !req.count || req.count > MXLFS_VIEW_MAX_SOURCES)
		return -EINVAL;

	view = kzalloc(sizeof(*view), GFP_KERNEL);
	if (!view)
		return -ENOMEM;
	for (i = 0; i < req.count; i++) {
		CLASS(fd, src)(req.fds[i]);
		struct inode *si;

		ret = -EBADF;
		if (fd_empty(src))
			goto fail;
		si = file_inode(fd_file(src));
		ret = -EINVAL;
		if (si->i_sb != inode->i_sb || !S_ISREG(si->i_mode) ||
		    si == inode || MXLFS_I(si)->view ||
		    i_size_read(si) != i_size_read(inode))
			goto fail;
		atomic_inc(&MXLFS_I(si)->view_users);
		view->sources[view->count++] = igrab(si);
	}

	view->pages = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	ret = -EINVAL;
	if (!view->pages)
		goto fail;
	ret = -ENOMEM;
	view->sel = kvzalloc(view->pages, GFP_KERNEL);
	if (!view->sel)
		goto fail;

	inode_lock(inode);
	ret = -EBUSY;
	if (mi->view || inode->i_mapping->nrpages ||
	    mapping_mapped(inode->i_mapping) || atomic_read(&mi->view_users)) {
		inode_unlock(inode);
		goto fail;
	}
	mi->view = view;
	inode_unlock(inode);
	return 0;
fail:
	mxlfs_view_free(view);
	return ret;
}

/*
 * Removes the view's mappings of [start, end), widened to whole PMD spans:
 * a fault maps a source folio whole only while its span selects one source,
 * so after a change no entry of the old layout may remain in any span the
 * change touches. A fault that looked up a folio before the change holds
 * it locked until its entry is installed; waiting for each lock in the
 * span and zapping again removes entries installed after the first zap.
 * Called with the invalidate lock held exclusively.
 */
static void mxlfs_view_zap(struct inode *inode, struct mxlfs_view *view,
			   pgoff_t start, pgoff_t end)
{
	pgoff_t first = round_down(start, HPAGE_PMD_NR);
	pgoff_t last = min_t(pgoff_t, round_up(end, HPAGE_PMD_NR), view->pages);
	struct folio_batch fbatch;
	unsigned int s, i;

	unmap_mapping_pages(inode->i_mapping, first, last - first, false);
	folio_batch_init(&fbatch);
	for (s = 0; s < view->count; s++) {
		pgoff_t next = first;

		while (filemap_get_folios(view->sources[s]->i_mapping, &next, last - 1, &fbatch)) {
			for (i = 0; i < folio_batch_count(&fbatch); i++) {
				folio_lock(fbatch.folios[i]);
				folio_unlock(fbatch.folios[i]);
			}
			folio_batch_release(&fbatch);
			cond_resched();
		}
	}
	unmap_mapping_pages(inode->i_mapping, first, last - first, false);
}

static long mxlfs_view_select_range(struct file *file, u32 source, u64 offset, u64 len)
{
	struct inode *inode = file_inode(file);
	struct mxlfs_view *view = MXLFS_I(inode)->view;
	pgoff_t start, end;

	if (!view)
		return -EINVAL;
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (source >= view->count || !len || !PAGE_ALIGNED(offset) ||
	    offset >= (u64)view->pages << PAGE_SHIFT)
		return -EINVAL;
	start = offset >> PAGE_SHIFT;
	len = min_t(u64, len, ((u64)view->pages << PAGE_SHIFT) - offset);
	end = DIV_ROUND_UP(offset + len, PAGE_SIZE);

	filemap_invalidate_lock(inode->i_mapping);
	if (!mxlfs_view_uniform(view, start, end - start, source)) {
		memset(view->sel + start, source, end - start);
		mxlfs_view_zap(inode, view, start, end);
		atomic64_inc(&MXLFS_SB(inode->i_sb)->view_selects);
	}
	filemap_invalidate_unlock(inode->i_mapping);
	return 0;
}

/*
 * Replaces one source of a view. Faults are held off throughout, so the
 * pages that select the source are zapped before the swap exactly as a
 * select would zap them: that waits for a fault holding a folio of the old
 * file, and nothing can map one after it.
 */
static long mxlfs_view_replace_source(struct file *file, void __user *arg)
{
	struct inode *inode = file_inode(file);
	struct mxlfs_view *view = MXLFS_I(inode)->view;
	struct mxlfs_view_replace req;
	pgoff_t start = 0, end = 0, p, q;
	struct inode *si, *old;
	long ret;

	if (!view)
		return -EINVAL;
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.flags || req.source >= view->count)
		return -EINVAL;

	CLASS(fd, src)(req.fd);
	if (fd_empty(src))
		return -EBADF;
	si = file_inode(fd_file(src));
	if (si->i_sb != inode->i_sb || !S_ISREG(si->i_mode) || si == inode)
		return -EINVAL;
	/* Under the lock that setattr and a SET_SOURCES on si take. */
	ret = -EINVAL;
	inode_lock(si);
	if (!MXLFS_I(si)->view && i_size_read(si) == i_size_read(inode)) {
		atomic_inc(&MXLFS_I(si)->view_users);
		ihold(si);
		ret = 0;
	}
	inode_unlock(si);
	if (ret)
		return ret;

	filemap_invalidate_lock(inode->i_mapping);
	old = view->sources[req.source];
	if (old != si) {
		/* Runs of pages that select it; runs sharing a PMD span zap once. */
		for (p = 0; p < view->pages; p = q) {
			u8 *at = memchr(view->sel + p, req.source, view->pages - p);

			if (!at)
				break;
			p = at - view->sel;
			at = memchr_inv(view->sel + p, req.source, view->pages - p);
			q = at ? at - view->sel : view->pages;
			if (end && p < round_up(end, HPAGE_PMD_NR)) {
				end = q;
				continue;
			}
			if (end)
				mxlfs_view_zap(inode, view, start, end);
			start = p;
			end = q;
		}
		if (end)
			mxlfs_view_zap(inode, view, start, end);
		view->sources[req.source] = si;
		atomic64_inc(&MXLFS_SB(inode->i_sb)->view_replaces);
	}
	filemap_invalidate_unlock(inode->i_mapping);

	atomic_dec(&MXLFS_I(old)->view_users);
	iput(old);
	return 0;
}

/* MXLFS_IOC_STATS before view_replaces; it gets the fields it knew. */
#define MXLFS_IOC_STATS_V1 _IOR('x', 4, __u64[4])

static long mxlfs_stats(struct file *file, void __user *arg, size_t size)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(file_inode(file)->i_sb);
	struct mxlfs_stats st = {
		.bytes_allocated = atomic64_read(&sbi->allocated),
		.bytes_limit = sbi->limit,
		.view_selects = atomic64_read(&sbi->view_selects),
		.view_faults = atomic64_read(&sbi->view_faults),
		.view_replaces = atomic64_read(&sbi->view_replaces),
	};

	return copy_to_user(arg, &st, size) ? -EFAULT : 0;
}

/* Copies the oldest flow not yet reported into req and marks it reported. */
static bool mxlfs_wanted_take(struct mxlfs_sb_info *sbi, struct mxlfs_wanted_req *req)
{
	struct mxlfs_wanted *w;
	bool found = false;

	spin_lock(&sbi->wanted_lock);
	list_for_each_entry(w, &sbi->wanted, node) {
		if (w->reported || w->state != MXLFS_WANT_PENDING)
			continue;
		w->reported = true;
		req->id = w->id;
		/* As the caller sees it; 0 when outside its pid namespace. */
		req->pid = pid_vnr(w->pid);
		strscpy(req->path, w->path, sizeof(req->path));
		found = true;
		break;
	}
	spin_unlock(&sbi->wanted_lock);
	return found;
}

static long mxlfs_wanted_next(struct file *file, struct mxlfs_wanted_req __user *arg)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(file_inode(file)->i_sb);
	struct mxlfs_wanted_req *req;
	u32 timeout_ms;
	long ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (get_user(timeout_ms, &arg->timeout_ms))
		return -EFAULT;
	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	ret = wait_event_interruptible_timeout(sbi->wanted_wq,
			mxlfs_wanted_take(sbi, req), msecs_to_jiffies(timeout_ms));
	if (ret > 0)
		ret = copy_to_user(arg, req, sizeof(*req)) ? -EFAULT : 0;
	else if (!ret)
		ret = -ETIMEDOUT;
	kfree(req);
	return ret;
}

static long mxlfs_wanted_fail(struct file *file, struct mxlfs_wanted_fail __user *arg)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(file_inode(file)->i_sb);
	struct mxlfs_wanted_fail req;
	struct mxlfs_wanted *w;
	long ret = -ENOENT;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.error <= 0 || req.error >= MAX_ERRNO)
		return -EINVAL;
	spin_lock(&sbi->wanted_lock);
	list_for_each_entry(w, &sbi->wanted, node) {
		if (w->id != req.id)
			continue;
		if (w->state == MXLFS_WANT_PENDING) {
			w->state = MXLFS_WANT_FAILED;
			w->error = -req.error;
			wake_up_all(&w->wq);
		}
		ret = 0;
		break;
	}
	spin_unlock(&sbi->wanted_lock);
	return ret;
}

static long mxlfs_dir_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case MXLFS_IOC_WANTED_NEXT:
		return mxlfs_wanted_next(file, (void __user *)arg);
	case MXLFS_IOC_WANTED_FAIL:
		return mxlfs_wanted_fail(file, (void __user *)arg);
	case MXLFS_IOC_LAZY_ALLOC:
		return mxlfs_set_lazy(MXLFS_SB(file_inode(file)->i_sb), (u32 __user *)arg);
	case MXLFS_IOC_STATS:
	case MXLFS_IOC_STATS_V1:
		return mxlfs_stats(file, (void __user *)arg, _IOC_SIZE(cmd));
	}
	return -ENOTTY;
}

static long mxlfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mxlfs_view_range range;
	struct mxlfs_view *view;
	u32 selected;

	switch (cmd) {
	case MXLFS_IOC_VIEW_SET_SOURCES:
		return mxlfs_view_set_sources(file, (void __user *)arg);
	case MXLFS_IOC_VIEW_SELECT:
		if (get_user(selected, (u32 __user *)arg))
			return -EFAULT;
		return mxlfs_view_select_range(file, selected, 0, U64_MAX);
	case MXLFS_IOC_VIEW_SELECT_RANGE:
		if (copy_from_user(&range, (void __user *)arg, sizeof(range)))
			return -EFAULT;
		if (range.flags)
			return -EINVAL;
		return mxlfs_view_select_range(file, range.source, range.offset, range.length);
	case MXLFS_IOC_VIEW_GET_SELECTED:
		view = MXLFS_I(file_inode(file))->view;
		if (!view)
			return -EINVAL;
		selected = READ_ONCE(view->sel[0]);
		return put_user(selected, (u32 __user *)arg);
	case MXLFS_IOC_VIEW_REPLACE_SOURCE:
		return mxlfs_view_replace_source(file, (void __user *)arg);
	case MXLFS_IOC_STATS:
	case MXLFS_IOC_STATS_V1:
		return mxlfs_stats(file, (void __user *)arg, _IOC_SIZE(cmd));
	case MXLFS_IOC_LAZY_ALLOC:
		return mxlfs_set_lazy(MXLFS_SB(file_inode(file)->i_sb), (u32 __user *)arg);
	case MXLFS_IOC_ALLOCATE:
		if (!(file->f_mode & FMODE_WRITE))
			return -EBADF;
		return mxlfs_allocate(file, FALLOC_FL_KEEP_SIZE, 0,
				      i_size_read(file_inode(file)), false);
	}
	return -ENOTTY;
}

static int mxlfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mxlfs_inode *mi = MXLFS_I(file_inode(file));

	file_accessed(file);
	if (!mi->view) {
		/*
		 * Faults on holes then allocate PMD-sized folios; not in a
		 * lazily sized file, where a header write must cost one page.
		 */
		if (!READ_ONCE(mi->lazy))
			vm_flags_set(vma, VM_HUGEPAGE);
		vma->vm_ops = &mxlfs_file_vm_ops;
		return 0;
	}
	/* A view is read-only: writes would land in a source. */
	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE))
		return -EACCES;
	vm_flags_clear(vma, VM_MAYWRITE);
	vma->vm_ops = &mxlfs_view_vm_ops;
	return 0;
}

static ssize_t mxlfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct mxlfs_view *view = MXLFS_I(inode)->view;
	ssize_t ret;

	if (!view)
		return generic_file_read_iter(iocb, to);

	/*
	 * Copies from the selected source's page cache; holes read as zeroes.
	 * The lock is held only to look a folio up: copying to user memory
	 * can fault, and a fault on a view takes the same lock after mmap_lock.
	 */
	ret = 0;
	while (iov_iter_count(to) && iocb->ki_pos < i_size_read(inode)) {
		loff_t pos = iocb->ki_pos;
		size_t n = min_t(loff_t, iov_iter_count(to), i_size_read(inode) - pos);
		struct folio *folio;
		size_t copied;

		filemap_invalidate_lock_shared(inode->i_mapping);
		folio = filemap_get_folio(mxlfs_view_source(view, pos >> PAGE_SHIFT)->i_mapping,
					  pos >> PAGE_SHIFT);
		filemap_invalidate_unlock_shared(inode->i_mapping);
		if (IS_ERR(folio)) {
			n = min_t(size_t, n, PAGE_SIZE - offset_in_page(pos));
			copied = iov_iter_zero(n, to);
		} else {
			size_t off = offset_in_folio(folio, pos);

			/* The next page may select another source. */
			n = min_t(size_t, n, PAGE_SIZE - offset_in_page(pos));
			copied = copy_folio_to_iter(folio, off, n, to);
			folio_put(folio);
		}
		iocb->ki_pos += copied;
		ret += copied;
		if (copied < n) {
			if (!ret)
				ret = -EFAULT;
			break;
		}
	}
	return ret;
}

static ssize_t mxlfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	if (MXLFS_I(file_inode(iocb->ki_filp))->view)
		return -EPERM;
	return generic_file_write_iter(iocb, from);
}

static unsigned long mxlfs_get_unmapped_area(struct file *file,
		unsigned long addr, unsigned long len, unsigned long pgoff,
		unsigned long flags)
{
	return thp_get_unmapped_area(file, addr, len, pgoff, flags);
}

static const struct file_operations mxlfs_file_operations = {
	.read_iter	= mxlfs_read_iter,
	.write_iter	= mxlfs_write_iter,
	.mmap		= mxlfs_mmap,
	.fsync		= noop_fsync,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.llseek		= generic_file_llseek,
	.get_unmapped_area = mxlfs_get_unmapped_area,
	.fallocate	= mxlfs_fallocate,
	.unlocked_ioctl	= mxlfs_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static const struct file_operations mxlfs_dir_operations = {
	.open		= dcache_dir_open,
	.release	= dcache_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.iterate_shared	= dcache_readdir,
	.fsync		= noop_fsync,
	.unlocked_ioctl	= mxlfs_dir_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

/* Superblock */

static struct inode *mxlfs_alloc_inode(struct super_block *sb)
{
	struct mxlfs_inode *mi = alloc_inode_sb(sb, mxlfs_inode_cachep, GFP_KERNEL);

	if (!mi)
		return NULL;
	atomic_long_set(&mi->charged, 0);
	mi->lazy = false;
	atomic_set(&mi->view_users, 0);
	mi->view = NULL;
	return &mi->vfs_inode;
}

static void mxlfs_free_inode(struct inode *inode)
{
	kmem_cache_free(mxlfs_inode_cachep, MXLFS_I(inode));
}

static void mxlfs_evict_inode(struct inode *inode)
{
	struct mxlfs_inode *mi = MXLFS_I(inode);

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	mxlfs_settle(inode);
	if (mi->view)
		mxlfs_view_free(mi->view);
}

static int mxlfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(dentry->d_sb);
	u64 used = atomic64_read(&sbi->allocated) >> PAGE_SHIFT;
	int err = simple_statfs(dentry, buf);

	if (err)
		return err;
	/* Unlimited reports all RAM, so capacity is never read as 0. */
	buf->f_blocks = sbi->limit ? sbi->limit >> PAGE_SHIFT : totalram_pages();
	buf->f_bfree = buf->f_bavail =
		buf->f_blocks > used ? buf->f_blocks - used : 0;
	return 0;
}

static int mxlfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(root->d_sb);

	if (sbi->mode != MXLFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", sbi->mode);
	seq_printf(m, ",size=%llu", sbi->limit);
	if (sbi->wait_ms)
		seq_printf(m, ",wait=%u", sbi->wait_ms);
	return 0;
}

static const struct super_operations mxlfs_ops = {
	.alloc_inode	= mxlfs_alloc_inode,
	.free_inode	= mxlfs_free_inode,
	.evict_inode	= mxlfs_evict_inode,
	.statfs		= mxlfs_statfs,
	.drop_inode	= inode_just_drop,
	.show_options	= mxlfs_show_options,
};

enum { Opt_mode, Opt_size, Opt_wait };

static const struct fs_parameter_spec mxlfs_fs_parameters[] = {
	fsparam_u32oct("mode", Opt_mode),
	fsparam_string("size", Opt_size),
	fsparam_u32("wait", Opt_wait),
	{}
};

static int mxlfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct mxlfs_sb_info *sbi = fc->s_fs_info;
	struct fs_parse_result result;
	char *rest;
	int opt;

	opt = fs_parse(fc, mxlfs_fs_parameters, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_mode:
		sbi->mode = result.uint_32 & S_IALLUGO;
		break;
	case Opt_size:
		sbi->limit = memparse(param->string, &rest);
		if (*rest)
			return invalfc(fc, "Bad size: %s", param->string);
		break;
	case Opt_wait:
		sbi->wait_ms = result.uint_32;
		break;
	}
	return 0;
}

static int mxlfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = sb->s_fs_info;
	struct inode *inode;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= MXLFS_MAGIC;
	sb->s_op		= &mxlfs_ops;
	set_default_d_op(sb, &mxlfs_dentry_operations);
	sb->s_d_flags		= DCACHE_DONTCACHE;
	sb->s_time_gran		= 1;

	inode = mxlfs_get_inode(sb, NULL, S_IFDIR | sbi->mode);
	sb->s_root = d_make_root(inode);
	return sb->s_root ? 0 : -ENOMEM;
}

static int mxlfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, mxlfs_fill_super);
}

static void mxlfs_free_fc(struct fs_context *fc)
{
	kfree(fc->s_fs_info);
}

static const struct fs_context_operations mxlfs_context_ops = {
	.free		= mxlfs_free_fc,
	.parse_param	= mxlfs_parse_param,
	.get_tree	= mxlfs_get_tree,
};

static int mxlfs_init_fs_context(struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);

	if (!sbi)
		return -ENOMEM;
	sbi->mode = MXLFS_DEFAULT_MODE;
	/* As tmpfs: half of RAM unless size= says otherwise. */
	sbi->limit = (u64)(totalram_pages() / 2) << PAGE_SHIFT;
	spin_lock_init(&sbi->lazy_lock);
	INIT_LIST_HEAD(&sbi->lazy);
	spin_lock_init(&sbi->wanted_lock);
	INIT_LIST_HEAD(&sbi->wanted);
	init_waitqueue_head(&sbi->wanted_wq);
	fc->s_fs_info = sbi;
	fc->ops = &mxlfs_context_ops;
	return 0;
}

static void mxlfs_kill_sb(struct super_block *sb)
{
	struct mxlfs_sb_info *sbi = sb->s_fs_info;

	kill_litter_super(sb);
	while (!list_empty(&sbi->lazy)) {
		struct mxlfs_lazy *l = list_first_entry(&sbi->lazy, struct mxlfs_lazy, node);

		list_del(&l->node);
		put_pid(l->tgid);
		kfree(l);
	}
	kfree(sbi);
}

static struct file_system_type mxlfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "mxlfs",
	.init_fs_context = mxlfs_init_fs_context,
	.parameters	= mxlfs_fs_parameters,
	.kill_sb	= mxlfs_kill_sb,
};

static void mxlfs_inode_init_once(void *p)
{
	inode_init_once(&((struct mxlfs_inode *)p)->vfs_inode);
}

static int __init mxlfs_init(void)
{
	int err;

	mxlfs_inode_cachep = kmem_cache_create("mxlfs_inode_cache",
					       sizeof(struct mxlfs_inode), 0,
					       SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
					       mxlfs_inode_init_once);
	if (!mxlfs_inode_cachep)
		return -ENOMEM;
	err = register_filesystem(&mxlfs_fs_type);
	if (err)
		kmem_cache_destroy(mxlfs_inode_cachep);
	return err;
}

static void __exit mxlfs_exit(void)
{
	unregister_filesystem(&mxlfs_fs_type);
	rcu_barrier();
	kmem_cache_destroy(mxlfs_inode_cachep);
}

module_init(mxlfs_init);
module_exit(mxlfs_exit);
MODULE_DESCRIPTION("Memory filesystem for MXL domains with source-switching views");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS_FS("mxlfs");
MODULE_VERSION("0.1.0-rc.2"); /* x-release-please-version */

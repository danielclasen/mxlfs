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
 * Folios end by EOF unless the mount has huge=always; see mxlfs_alloc_order.
 * With numa_node=, every folio of the mount prefers that NUMA node, wherever
 * the task that makes it runs; a file's own preferred node (MXLFS_IOC_SET_NODE)
 * takes precedence; see mxlfs_alloc_folio. MXLFS_IOC_MOVE moves a file's
 * folios to another node under its mappings; see mxlfs_move.
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
 * once the flow appears the walk is retried and finds it. A walk with
 * RESOLVE_NO_XDEV, which the VFS does not let through ->d_manage, does not
 * wait.
 */

#include <linux/backing-dev.h>
#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fsnotify.h>
#include <linux/fs_parser.h>
#include <linux/file.h>
#include <linux/hashtable.h>
#include <linux/highmem.h>
#include <linux/huge_mm.h>
#include <linux/magic.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/rcupdate_wait.h>
#include <linux/rmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/stringhash.h>
#include <linux/swap.h>
#include <linux/uio.h>
#include <linux/falloc.h>
#include <linux/namei.h>
#include <linux/sched/signal.h>

#include "compat.h"
#include "mxlfs_uapi.h"

#ifndef CONFIG_TRANSPARENT_HUGEPAGE
#error "mxlfs needs CONFIG_TRANSPARENT_HUGEPAGE for large page cache folios"
#endif
#ifdef CONFIG_HIGHMEM
#error "mxlfs copies into large folios through the direct map"
#endif

#define MXLFS_DEFAULT_MODE 0755

/* huge=, with tmpfs's meanings; tmpfs's never and advise do not apply. */
enum { MXLFS_HUGE_WITHIN_SIZE, MXLFS_HUGE_ALWAYS };

#define MXLFS_WINDOW_BUCKETS 9

/*
 * Missing flows waited for at once (wait=), per mount and per thread group
 * that made their entries. Each costs an entry holding its name and path
 * until its last waiter leaves, so the caps bound what unprivileged walks
 * can make the kernel hold; a walk past either fails at once, as without
 * wait=. The thread group cap keeps one runaway process from taking the
 * mount's share; it is no security boundary, since any process can fork
 * more thread groups. What a container's walks hold is bounded by its
 * memory cgroup, to which the entries are charged.
 */
#define MXLFS_WANTED_MAX 4096
#define MXLFS_WANTED_MAX_TGID 256
#define MXLFS_WANTED_HASH_BITS 10	/* both tables hold up to MXLFS_WANTED_MAX */

static_assert(MXLFS_NODE_NONE == NUMA_NO_NODE);
/* A view's source table, one byte per page, within what kvmalloc takes. */
static_assert(MXLFS_VIEW_MAX_SIZE % PAGE_SIZE == 0 &&
	      MXLFS_VIEW_MAX_SIZE / PAGE_SIZE <= INT_MAX);
static_assert(sizeof_field(struct mxlfs_stats, move_refused) ==
	      MXLFS_REFUSE_MAX * sizeof(__u64));
static_assert(sizeof_field(struct mxlfs_stats, move_window_hist) ==
	      MXLFS_WINDOW_BUCKETS * sizeof(__u64));

/* The mount's pages and allocations on one NUMA node. */
struct mxlfs_node_counts {
	atomic64_t pages;		/* uptodate page cache pages on the node */
	atomic64_t fallback_folios;	/* folios that preferred it and got another node */
	atomic64_t fallback_pages;
	atomic64_t moved_in;		/* pages, MXLFS_IOC_MOVE */
	atomic64_t moved_out;
};

/* MXLFS_IOC_MOVE since the mount; see struct mxlfs_stats. */
struct mxlfs_move_counts {
	atomic64_t calls, calls_partial, calls_none, dry_runs;
	atomic64_t blocks, bytes, ns;
	atomic64_t refused[MXLFS_REFUSE_MAX];
	atomic64_t window_sum, window_max;
	atomic64_t window_hist[MXLFS_WINDOW_BUCKETS];
};

struct mxlfs_sb_info {
	umode_t mode;
	unsigned int huge;
	int numa_node;	/* NUMA_NO_NODE: where the allocating task's policy puts it */
	u64 limit; /* bytes; 0 means unlimited (size=0) */
	unsigned int wait_ms; /* 0 disables waiting for missing flows */
	unsigned int seen;	/* BIT(Opt_*) given to the mount or remount it was parsed for */
	atomic64_t allocated;
	atomic64_t view_selects;
	atomic64_t view_faults;
	atomic64_t view_replaces;
	struct mxlfs_node_counts *node;	/* [nr_node_ids] */
	struct mxlfs_move_counts move;
	atomic64_t holds;		/* open files holding a file */

	spinlock_t lazy_lock;	/* protects lazy */
	struct list_head lazy;	/* struct mxlfs_lazy: thread groups with lazy fallocate */

	spinlock_t wanted_lock;	/* protects the wanted_* fields and each entry */
	struct list_head wanted;	/* struct mxlfs_wanted, oldest first */
	struct mxlfs_wanted_tables *wanted_tables;	/* with wait= only */
	unsigned int wanted_count;	/* entries; read without the lock by stats */
	wait_queue_head_t wanted_wq;	/* MXLFS_IOC_WANTED_NEXT callers */
	u64 wanted_seq;
	atomic64_t wanted_refused;	/* walks past MXLFS_WANTED_MAX* */
};

struct mxlfs_lazy {
	struct list_head node;
	struct pid *tgid;
};

enum { MXLFS_WANT_PENDING, MXLFS_WANT_APPEARED, MXLFS_WANT_FAILED };

/*
 * The lookup tables of the wanted list, allocated for a mount with wait=:
 * struct mxlfs_wanted by directory and name, struct mxlfs_wanted_owner by
 * thread group.
 */
struct mxlfs_wanted_tables {
	DECLARE_HASHTABLE(names, MXLFS_WANTED_HASH_BITS);
	DECLARE_HASHTABLE(owners, MXLFS_WANTED_HASH_BITS);
};

/* A thread group that made entries in the wanted list; under wanted_lock. */
struct mxlfs_wanted_owner {
	struct hlist_node node;
	struct pid *tgid;
	unsigned int count;	/* its entries */
};

/* A missing flow that path walks are waiting for; under wanted_lock. */
struct mxlfs_wanted {
	struct list_head node;
	struct hlist_node hash;
	struct inode *dir;
	char *name;
	unsigned int len;
	char *path;		/* from the filesystem root */
	u64 id;
	struct mxlfs_wanted_owner *owner;	/* first waiter's thread group */
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
	/* [fold_start, fold_end): the range a fold works on, read racily by faults */
	pgoff_t fold_start, fold_end;
	struct mxlfs_view *view;
	/*
	 * Uptodate page cache pages per node [nr_node_ids]; for regular files
	 * also the node allocations prefer (NUMA_NO_NODE: the mount's), set
	 * under the inode lock, and the open files that hold the file.
	 */
	atomic_long_t *node_pages;
	int pref;
	atomic_t holds;
};

/* file->private_data of an open file that holds its file (MXLFS_IOC_HOLD). */
#define MXLFS_HELD ((void *)1UL)

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

/*
 * Counts nr pages (uncounts, when negative) of inode on node nid, in the
 * inode's pages per node and the mount's. A folio counts while it is in the
 * page cache and uptodate: where it becomes so, where it moves and where it
 * leaves.
 */
static void mxlfs_count(struct inode *inode, int nid, long nr)
{
	atomic_long_add(nr, &MXLFS_I(inode)->node_pages[nid]);
	atomic64_add(nr, &MXLFS_SB(inode->i_sb)->node[nid].pages);
}

static void mxlfs_count_folio(struct inode *inode, struct folio *folio, long sign)
{
	mxlfs_count(inode, folio_nid(folio), sign * folio_nr_pages(folio));
}

/*
 * Adds the pages of inode's uptodate folios per node to pages[nr_node_ids].
 * Under the inode lock and the invalidate lock, which keep every path that
 * adds a folio or makes one uptodate out.
 */
static void mxlfs_walk_nodes(struct inode *inode, u64 *pages)
{
	struct folio_batch fbatch;
	pgoff_t next = 0;
	unsigned int i;

	folio_batch_init(&fbatch);
	while (filemap_get_folios(inode->i_mapping, &next, ULONG_MAX, &fbatch)) {
		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *f = fbatch.folios[i];

			if (folio_test_uptodate(f))
				pages[folio_nid(f)] += folio_nr_pages(f);
		}
		folio_batch_release(&fbatch);
		cond_resched();
	}
}

/*
 * Sets inode's pages per node to what its page cache holds. For a
 * truncation, which may split a large folio and drops pages without a way
 * back to the inode: ->free_folio gets no mapping. Under the inode lock.
 */
static void mxlfs_recount(struct inode *inode)
{
	u64 *pages = kcalloc(nr_node_ids, sizeof(*pages), GFP_KERNEL | __GFP_NOFAIL);
	unsigned int nid;

	filemap_invalidate_lock(inode->i_mapping);
	mxlfs_walk_nodes(inode, pages);
	for (nid = 0; nid < nr_node_ids; nid++)
		mxlfs_count(inode, nid, pages[nid] -
			    atomic_long_read(&MXLFS_I(inode)->node_pages[nid]));
	filemap_invalidate_unlock(inode->i_mapping);
	kfree(pages);
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
	if (on > 1)
		return -EINVAL;
	add = on ? kmalloc(sizeof(*add), GFP_KERNEL_ACCOUNT) : NULL;
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

	/* Symlinks keep their target in page cache too. */
	MXLFS_I(inode)->node_pages = kcalloc(nr_node_ids, sizeof(atomic_long_t),
					     GFP_KERNEL_ACCOUNT);
	if (!MXLFS_I(inode)->node_pages) {
		iput(inode);
		return NULL;
	}

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

static u32 mxlfs_wanted_key(const struct inode *dir, const struct qstr *name)
{
	return full_name_hash(dir, name->name, name->len);
}

/*
 * The pending entry for name in dir. An entry whose flow appeared or was
 * failed only waits for its waiters to leave; a walk joining it would
 * take that outcome for its own, so such an entry is not found and a
 * walk that still sees the name missing makes a new one.
 */
static struct mxlfs_wanted *mxlfs_wanted_find(struct mxlfs_sb_info *sbi,
					      struct inode *dir,
					      const struct qstr *name)
{
	struct mxlfs_wanted *w;

	lockdep_assert_held(&sbi->wanted_lock);
	hash_for_each_possible(sbi->wanted_tables->names, w, hash,
			       mxlfs_wanted_key(dir, name))
		if (w->state == MXLFS_WANT_PENDING && w->dir == dir &&
		    w->len == name->len && !memcmp(w->name, name->name, name->len))
			return w;
	return NULL;
}

static struct mxlfs_wanted_owner *mxlfs_wanted_owner(struct mxlfs_sb_info *sbi,
						     struct pid *tgid)
{
	struct mxlfs_wanted_owner *o;

	lockdep_assert_held(&sbi->wanted_lock);
	hash_for_each_possible(sbi->wanted_tables->owners, o, node, (unsigned long)tgid)
		if (o->tgid == tgid)
			return o;
	return NULL;
}

/* Whether another entry by o's thread group would pass a cap. */
static bool mxlfs_wanted_full(struct mxlfs_sb_info *sbi, struct mxlfs_wanted_owner *o)
{
	lockdep_assert_held(&sbi->wanted_lock);
	return sbi->wanted_count >= MXLFS_WANTED_MAX ||
	       (o && o->count >= MXLFS_WANTED_MAX_TGID);
}

/* Also frees an entry never listed, whose dir is NULL. */
static void mxlfs_wanted_free(struct mxlfs_wanted *w)
{
	iput(w->dir);
	kfree(w->name);
	kfree(w->path);
	kfree(w);
}

/*
 * A new entry for dentry, with its name and path, not yet listed. The
 * PATH_MAX scratch buffer is freed before the walk waits. A path
 * MXLFS_IOC_WANTED_NEXT cannot hold fails the walk with ENAMETOOLONG
 * rather than make it wait for a flow no daemon learns of.
 */
static struct mxlfs_wanted *mxlfs_wanted_new(struct dentry *dentry)
{
	struct mxlfs_wanted *w;
	char *buf, *path;
	int err = -ENOMEM;

	w = kzalloc(sizeof(*w), GFP_KERNEL_ACCOUNT);
	if (!w)
		return ERR_PTR(-ENOMEM);
	buf = __getname();
	if (buf) {
		path = dentry_path_raw(dentry, buf, PATH_MAX);
		if (IS_ERR(path))
			err = PTR_ERR(path);
		else if (strlen(path) >= sizeof_field(struct mxlfs_wanted_req, path))
			err = -ENAMETOOLONG;
		else
			w->path = kstrdup(path, GFP_KERNEL_ACCOUNT);
		__putname(buf);
	}
	w->name = kmemdup(dentry->d_name.name, dentry->d_name.len, GFP_KERNEL_ACCOUNT);
	if (!w->path || !w->name) {
		mxlfs_wanted_free(w);
		return ERR_PTR(err);
	}
	w->len = dentry->d_name.len;
	init_waitqueue_head(&w->wq);
	return w;
}

/* Wakes the walks waiting for name in dir, which now exists. */
static void mxlfs_wanted_appeared(struct inode *dir, const struct qstr *name)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(dir->i_sb);
	struct mxlfs_wanted *w;

	if (!sbi->wait_ms || !mxlfs_is_flow_name(name))
		return;
	spin_lock(&sbi->wanted_lock);
	w = mxlfs_wanted_find(sbi, dir, name);
	if (w) {
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
	unsigned long timeout = msecs_to_jiffies(sbi->wait_ms);
	struct mxlfs_wanted_owner *o, *no = NULL, *gone = NULL;
	struct pid *tgid = task_tgid(current);
	struct mxlfs_wanted *w, *nw = NULL;
	struct dentry *parent;
	struct inode *dir;
	int state, error, ret = 0;
	bool last, moved = false;
	long left;

	if (!timeout)
		return 0;

	parent = dget_parent(dentry);
	dir = d_inode(parent);
	/*
	 * A walk that joins an entry, or is turned away, allocates nothing.
	 * One that makes an entry, or the first of its thread group, drops
	 * the lock to allocate what it lacks and looks again: another walk
	 * may have made the entry, filled a cap or emptied the thread
	 * group's record meanwhile. Each pass allocates what is missing, so
	 * the third pass at the latest decides. A dentry the flow has moved
	 * onto or made positive since the walk found it gets no entry: the
	 * rename or create took wanted_lock after changing it, so either
	 * this sees the change or that finds the entry.
	 */
	spin_lock(&sbi->wanted_lock);
	for (;;) {
		w = mxlfs_wanted_find(sbi, dir, &dentry->d_name);
		if (w)
			break;
		moved = d_unhashed(dentry) || d_really_is_positive(dentry);
		if (moved)
			break;
		o = mxlfs_wanted_owner(sbi, tgid);
		if (mxlfs_wanted_full(sbi, o))
			break;
		if (nw && (o || no))
			break;
		spin_unlock(&sbi->wanted_lock);
		if (!nw) {
			nw = mxlfs_wanted_new(dentry);
			if (IS_ERR(nw)) {
				ret = PTR_ERR(nw);
				nw = NULL;
				goto out;
			}
		}
		if (!o && !no) {
			no = kmalloc(sizeof(*no), GFP_KERNEL_ACCOUNT);
			if (!no) {
				ret = -ENOMEM;
				goto out;
			}
		}
		spin_lock(&sbi->wanted_lock);
	}
	/* Not full, the loop left with what a new entry needs. */
	if (!w && !moved && !mxlfs_wanted_full(sbi, o)) {
		if (!o) {
			o = no;
			no = NULL;
			o->tgid = get_pid(tgid);
			o->count = 0;
			hash_add(sbi->wanted_tables->owners, &o->node, (unsigned long)tgid);
		}
		o->count++;
		w = nw;
		nw = NULL;
		w->owner = o;
		w->dir = igrab(dir);
		w->id = ++sbi->wanted_seq;
		list_add_tail(&w->node, &sbi->wanted);
		hash_add(sbi->wanted_tables->names, &w->hash,
			 mxlfs_wanted_key(dir, &dentry->d_name));
		WRITE_ONCE(sbi->wanted_count, sbi->wanted_count + 1);
		wake_up_interruptible(&sbi->wanted_wq);
	}
	if (w)
		w->waiters++;
	spin_unlock(&sbi->wanted_lock);
	if (moved) {
		ret = d_really_is_positive(dentry) ? 0 : -ESTALE;
	} else if (!w) {
		atomic64_inc(&sbi->wanted_refused);
		ret = -ENOENT;
	}
out:
	dput(parent);
	kfree(no);
	if (nw)
		mxlfs_wanted_free(nw);
	if (!w)
		return ret;

	/*
	 * A rename onto the name unhashes this negative dentry and a create
	 * in place makes it positive, each before it wakes the entry; which
	 * of them happened decides the result below.
	 */
	left = wait_event_killable_timeout(w->wq,
					   READ_ONCE(w->state) != MXLFS_WANT_PENDING ||
					   d_unhashed(dentry) || d_really_is_positive(dentry),
					   timeout);

	spin_lock(&sbi->wanted_lock);
	state = w->state;
	error = w->error;
	last = --w->waiters == 0;
	if (last) {
		list_del(&w->node);
		hash_del(&w->hash);
		WRITE_ONCE(sbi->wanted_count, sbi->wanted_count - 1);
		if (!--w->owner->count) {
			gone = w->owner;
			hash_del(&gone->node);
		}
	}
	spin_unlock(&sbi->wanted_lock);
	if (last)
		mxlfs_wanted_free(w);
	if (gone) {
		put_pid(gone->tgid);
		kfree(gone);
	}

	if (d_really_is_positive(dentry))
		return 0;
	if (d_unhashed(dentry) || state == MXLFS_WANT_APPEARED)
		return -ESTALE;	/* the walk is retried and finds the flow */
	if (left < 0)
		return left;
	if (state == MXLFS_WANT_FAILED)
		return error;
	return -ENOENT;
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
	if (sbi->wait_ms && mxlfs_is_flow_name(&dentry->d_name) &&
	    !(flags & (LOOKUP_CREATE | LOOKUP_EXCL | LOOKUP_RENAME_TARGET))) {
		/*
		 * The VFS fails a RESOLVE_NO_XDEV walk on a managed dentry with
		 * EXDEV, as for a mount it may not cross, without calling
		 * ->d_manage. Such a walk does not wait: the dentry is left
		 * unhashed, as simple_lookup leaves one in a casefolded
		 * directory, so the walk fails with ENOENT, and no other walk
		 * finds the name hashed without the flag and fails at once.
		 * Managed dentries are not kept once no walk holds them
		 * (s_d_flags), so such a walk meets one, and EXDEV, only
		 * while another walk holds the name.
		 */
		if (flags & LOOKUP_NO_XDEV)
			return NULL;
		spin_lock(&dentry->d_lock);
		dentry->d_flags |= DCACHE_MANAGE_TRANSIT;
		spin_unlock(&dentry->d_lock);
	}
	d_add(dentry, NULL);
	return NULL;
}

/* Directories */

static int mxlfs_mknod(MXLFS_IDMAP *idmap, struct inode *dir,
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
	mxlfs_d_pin(dentry, inode); /* pin the dentry, as ramfs does */
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mxlfs_wanted_appeared(dir, &dentry->d_name);
	return 0;
}

static struct dentry *mxlfs_mkdir(MXLFS_IDMAP *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	int error = mxlfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);

	if (!error)
		inc_nlink(dir);
	return ERR_PTR(error);
}

#if MXLFS_CREATE_HAS_EXCL
static int mxlfs_create(MXLFS_IDMAP *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
#else
static int mxlfs_create(MXLFS_IDMAP *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode)
#endif
{
	return mxlfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static int mxlfs_symlink(MXLFS_IDMAP *idmap, struct inode *dir,
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
	mxlfs_d_pin(dentry, inode);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mxlfs_wanted_appeared(dir, &dentry->d_name);
	return 0;
}

static int mxlfs_tmpfile(MXLFS_IDMAP *idmap, struct inode *dir,
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

static int mxlfs_rename(MXLFS_IDMAP *idmap, struct inode *old_dir,
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
	if (error)
		return error;
	/*
	 * A woken walk retries once, and the VFS moves the dentries only
	 * after this returns. Left hashed, the negative target would meet
	 * that retry, which would wait again or, joining waiters not yet
	 * gone, fail with ESTALE. Unhashed, the retry looks the name up
	 * under new_dir's lock, held until the move is done.
	 */
	if (d_really_is_negative(new_dentry))
		d_drop(new_dentry);
	mxlfs_wanted_appeared(new_dir, &name);
	return 0;
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
 * The folio order fallocate takes at index for a range ending at end, in a
 * file of size bytes. A folio ends by EOF (huge=within_size), except with
 * huge=always in the file's last partial PMD span, which takes one PMD
 * folio past EOF when size= has room for it. A NIC maps a memory region
 * with one page size, the largest every physical discontinuity allows
 * (ib_umem_find_best_pgsz), so the small folios of a tail would set it for
 * the whole file. A file below one PMD keeps folios within it, so a file
 * costs at most HPAGE_PMD_SIZE - PAGE_SIZE more than its pages, and only
 * when it is at least that large. The bytes past EOF read as zeroes when
 * the file grows into them; a mapping faults SIGBUS past EOF as before,
 * since the core maps no page beyond i_size of a file that is not shmem.
 */
static unsigned int mxlfs_alloc_order(struct mxlfs_sb_info *sbi, pgoff_t index,
				      pgoff_t end, loff_t size)
{
	if (sbi->huge == MXLFS_HUGE_ALWAYS && HPAGE_PMD_ORDER <= MAX_PAGECACHE_ORDER &&
	    size >= HPAGE_PMD_SIZE && end == DIV_ROUND_UP(size, PAGE_SIZE) &&
	    IS_ALIGNED(index, HPAGE_PMD_NR) && index + HPAGE_PMD_NR > end &&
	    !mxlfs_over_limit(sbi, HPAGE_PMD_SIZE))
		return HPAGE_PMD_ORDER;
	return mxlfs_fit_order(index, end);
}

/* Whether a node number from user space names a node with memory. */
static bool mxlfs_node_valid(long nid)
{
	return nid >= 0 && nid < nr_node_ids && node_state(nid, N_MEMORY);
}

/*
 * The node a folio of inode prefers: the file's own (MXLFS_IOC_SET_NODE),
 * else the mount's numa_node=, each only while it has memory; NUMA_NO_NODE
 * when neither applies.
 */
static int mxlfs_effective_node(struct inode *inode)
{
	int nid = READ_ONCE(MXLFS_I(inode)->pref);

	if (nid == NUMA_NO_NODE || !node_state(nid, N_MEMORY))
		nid = MXLFS_SB(inode->i_sb)->numa_node;
	if (nid == NUMA_NO_NODE || !node_state(nid, N_MEMORY))
		return NUMA_NO_NODE;
	return nid;
}

/* Whether inode's holes are filled on a preferred node; see mxlfs_place. */
static bool mxlfs_placed(struct inode *inode)
{
	return READ_ONCE(MXLFS_I(inode)->pref) != NUMA_NO_NODE ||
	       MXLFS_SB(inode->i_sb)->numa_node != NUMA_NO_NODE;
}

/*
 * Allocates a folio of inode's data. With a preferred node, the folio
 * prefers it and comes from the others, in the order of its zonelist, when
 * the node has none free, which counts as a fallback of that node; the
 * allocating task's memory policy does not apply, its cpuset's memory nodes
 * still do. Otherwise the folio comes from where the task's memory policy
 * puts it, as page cache does.
 */
static struct folio *mxlfs_alloc_folio(struct inode *inode, gfp_t gfp,
				       unsigned int order)
{
	struct mxlfs_node_counts *counts;
	int nid = mxlfs_effective_node(inode);
	struct folio *folio;

	if (nid == NUMA_NO_NODE)
		return mxlfs_filemap_alloc_folio(gfp, order);
	folio = __folio_alloc_node(gfp, order, nid);
	if (folio && folio_nid(folio) != nid) {
		counts = &MXLFS_SB(inode->i_sb)->node[nid];
		atomic64_inc(&counts->fallback_folios);
		atomic64_add(folio_nr_pages(folio), &counts->fallback_pages);
	}
	return folio;
}

/*
 * With a preferred node (numa_node= or the file's own), fills the holes in
 * [index, end) with zeroed folios from mxlfs_alloc_folio before a fault,
 * read or write reaches the core, which
 * would allocate them where the task's memory policy puts them. Folios
 * are the largest aligned ones of at most order most that end by end:
 * a VM_HUGEPAGE fault passes the order of the window it places (see
 * mxlfs_fault); any other fault, read or write passes 0, single pages,
 * as the core takes for them. A folio that
 * cannot be added, as when a racing fault added a page in its range,
 * gives way to smaller ones at the same index, looked up again first.
 * size= is checked and charged per folio as read_folio does: a page is
 * placed while the limit is not exceeded, a larger folio only while it
 * fits. Stops there, at a fatal signal, or when not even a page can be
 * added; the core then allocates the rest as without the option.
 */
static void mxlfs_place(struct inode *inode, pgoff_t index, pgoff_t end, unsigned int most)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(inode->i_sb);
	struct address_space *mapping = inode->i_mapping;
	unsigned int cap = most;
	gfp_t gfp = mapping_gfp_mask(mapping);

	if (!mxlfs_placed(inode))
		return;
	/* As the core instantiates page cache: not across a truncation. */
	filemap_invalidate_lock_shared(mapping);
	/* Nor into a file that became a view since the caller looked. */
	if (MXLFS_I(inode)->view)
		end = index;
	while (index < end && !fatal_signal_pending(current)) {
		struct folio *folio = filemap_get_folio(mapping, index);
		unsigned int order;
		int err;

		if (!IS_ERR(folio)) {
			index = folio_next_index(folio);
			folio_put(folio);
			cap = most;
			continue;
		}
		order = min(mxlfs_fit_order(index, end), cap);
		if (mxlfs_over_limit(sbi, order ? PAGE_SIZE << order : 0)) {
			if (!order)
				break;
			cap = order - 1;
			continue;
		}
		folio = mxlfs_alloc_folio(inode, gfp | __GFP_ZERO |
					  (order ? __GFP_NORETRY | __GFP_NOWARN : 0), order);
		err = folio ? filemap_add_folio(mapping, folio, index, gfp) : -ENOMEM;
		if (err) {
			if (folio)
				folio_put(folio);
			if (!order)
				break;
			cap = order - 1;
			continue;
		}
		folio_mark_uptodate(folio);
		folio_mark_dirty(folio);
		mxlfs_count_folio(inode, folio, 1);
		folio_unlock(folio);
		index = folio_next_index(folio);
		folio_put(folio);
		mxlfs_settle(inode);
		cap = most;
		cond_resched();
	}
	filemap_invalidate_unlock_shared(mapping);
}

/* mxlfs_place for a read of len bytes at pos, up to EOF. */
static void mxlfs_place_read(struct inode *inode, loff_t pos, size_t len)
{
	loff_t size = i_size_read(inode);

	if (pos < size && len)
		mxlfs_place(inode, pos >> PAGE_SHIFT,
			    DIV_ROUND_UP(min_t(loff_t, size, pos + len), PAGE_SIZE), 0);
}

/*
 * Whether a locked, unmapped small folio of a fold has a reference beyond
 * the page cache's and the fold's. One more is allowed while the folio is
 * not on an LRU list: a folio added to the page cache waits in a per-CPU
 * batch that holds it until the batch puts it on the list, and one taken
 * off the list is held by whoever isolated it; neither writes to it. A
 * module cannot drain the batches, and a folio just written to through a
 * mapping is usually in one.
 *
 * This relies on an mm invariant: an lru_add batch takes its reference
 * before PG_lru is set and drops it after, and an isolation holds a
 * reference taken before it clears PG_lru, its own where reclaim and
 * compaction isolate, the caller's where folio_isolate_lru does, which
 * requires one. Reading the count before and after PG_lru, and taking the
 * larger, therefore counts that reference whenever PG_lru reads clear.
 * The LRU batches have changed since 6.11; re-check this on a new kernel.
 *
 * A folio of an mlocked mapping is not folded: the fold's zap munlocks
 * it through a per-CPU mlock batch, which holds a reference with PG_lru
 * still set. An allowance for PG_mlocked would not do, since a folio
 * still mlocked by another mapping has no batch reference, so the extra
 * reference would let a get_user_pages one through.
 */
static bool mxlfs_fold_held(struct folio *folio)
{
	int before, after, lru;

	before = folio_ref_count(folio);
	/* Pairs with the batch's drop after it sets PG_lru. */
	smp_rmb();
	lru = folio_test_lru(folio);
	/* Pairs with an isolation's reference taken before it clears PG_lru. */
	smp_rmb();
	after = folio_ref_count(folio);
	return max(before, after) > folio_expected_ref_count(folio) + 1 + !lru;
}

/*
 * Whether one of the folio's pages [first, end) has a memory error, by
 * each page's own flag: memory_failure() sets PageHWPoison on the page
 * before it sets has_hwpoisoned on a large folio, so
 * folio_contain_hwpoisoned_page() can miss a tail page between the two.
 * Called with a reference on the folio.
 */
static bool mxlfs_pages_poisoned(struct folio *folio, long first, long end)
{
	long p;

	for (p = first; p < end; p++)
		if (PageHWPoison(folio_page(folio, p)))
			return true;
	return false;
}

static bool mxlfs_folio_poisoned(struct folio *folio)
{
	return mxlfs_pages_poisoned(folio, 0, folio_nr_pages(folio));
}

/*
 * Replaces the small folios in [index, index + 2^order) with one folio of
 * that order, copying their contents; holes stay zero. Mappings of the
 * range are zapped and refault onto the new folio. Called with the inode
 * locked, which keeps write() out; the invalidate lock keeps faults and
 * reads from instantiating pages meanwhile, and faults on the views that
 * use the file from mapping one of the small folios: a view's mappings are
 * not the file's, so the zap does not reach them, and a small folio a view
 * maps cannot be folded.
 *
 * A fault on a folio that is in the page cache takes no invalidate lock,
 * only the folio's lock, so every small folio stays locked from before its
 * copy until it has left the page cache: a store through a mapping made
 * after the copy would otherwise be dropped with the small folio. The
 * range is zapped again once all of them are locked, which removes what a
 * fault mapped before its folio was locked here. No other path holds two
 * folio locks of one file, so taking them all cannot deadlock.
 *
 * A store needs no mapping of the folio when it comes through a reference
 * taken with get_user_pages, as /proc/pid/mem and ptrace take one: it
 * lands without the folio lock, and after the copy it would be lost with
 * the small folio. A folio held beyond the page cache and the fold is
 * therefore not folded; see mxlfs_fold_held. A fault waiting for a folio's
 * lock holds a reference across the wait, and a view fault that dropped
 * the invalidate locks to wait (mxlfs_view_lock_folio) may still hold one
 * when a fold takes them; the fold then retries and may give up. A
 * plain-file fault waits for a fold on the invalidate lock instead.
 *
 * A page with a memory error in the range fails the fold with EIO and
 * leaves the range as it was. The pages are tested before the copies and
 * again after them, which narrows a race rather than closes it, as
 * memory_failure() sets the flag without the folio lock: an error that
 * arrives after the second test leaves the page's contents from before it
 * in the new folio. A copy that reads bad memory fails instead of
 * machine-checking the kernel only where copy_mc_highpage() is
 * machine-check-safe, which depends on the architecture and the CPU: it
 * is a plain copy without ARCH_HAS_COPY_MC (selected only on x86_64 and
 * ppc64), and x86_64's copy_mc_to_kernel() falls back to memcpy() on a
 * CPU with neither copy_mc_fragile nor ERMS.
 */
static int mxlfs_fold(struct inode *inode, pgoff_t index, unsigned int order, gfp_t gfp)
{
	struct address_space *mapping = inode->i_mapping;
	pgoff_t nr = 1UL << order, end = index + nr - 1, next = index;
	struct folio_batch fbatch;
	struct folio *large, **small;
	unsigned int i, n = 0, tries;
	int err = 0;

	large = mxlfs_alloc_folio(inode, gfp | __GFP_ZERO | __GFP_NORETRY | __GFP_NOWARN,
				  order);
	if (!large)
		return -ENOMEM;
	small = kvmalloc_array(nr, sizeof(*small), GFP_KERNEL);
	if (!small) {
		folio_put(large);
		return -ENOMEM;
	}

	filemap_invalidate_lock(mapping);
	WRITE_ONCE(MXLFS_I(inode)->fold_start, index);
	WRITE_ONCE(MXLFS_I(inode)->fold_end, index + nr);
	unmap_mapping_pages(mapping, index, nr, false);
	folio_batch_init(&fbatch);
	while (!err && filemap_get_folios(mapping, &next, end, &fbatch)) {
		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *f = fbatch.folios[i];

			/*
			 * After the lock: a view fault that looked the folio
			 * up before this fold took the invalidate lock holds
			 * it locked until it is mapped.
			 */
			if (!err && !folio_test_large(f)) {
				folio_lock(f);
				small[n++] = f;
				continue;
			}
			err = -EBUSY;
			folio_put(f);
		}
		/* The batch's references now belong to small[]. */
		folio_batch_reinit(&fbatch);
	}
	if (!err) {
		unmap_mapping_pages(mapping, index, nr, false);
		/*
		 * A page with a memory error stays in the file and fails
		 * reads (mxlfs_unpoisoned); copied into the large folio, its
		 * old contents would read as good data.
		 */
		for (i = 0; i < n; i++) {
			if (folio_maybe_dma_pinned(small[i]) || folio_mapped(small[i]))
				err = -EBUSY;
			else if (mxlfs_folio_poisoned(small[i]))
				err = -EIO;
		}
	}
	/*
	 * A reference beyond the page cache's is often about to go: a TLB
	 * batch that has zapped a mapping and not yet freed it, or a store
	 * through get_user_pages finishing. Nothing can map the locked folios
	 * meanwhile, so the fold waits a little for it, as a move does.
	 */
	for (tries = 0; !err; tries++) {
		for (i = 0; i < n && !mxlfs_fold_held(small[i]); i++)
			;
		if (i == n)
			break;
		if (tries == 3)
			err = -EBUSY;
		else
			usleep_range(100 << tries, 200 << tries);
	}
	/*
	 * Every copy before any small folio leaves the page cache, so a copy
	 * that finds bad memory leaves the range as it was. One read_folio
	 * refused holds nothing of the file: a hole; read_folio zeroed it,
	 * this is defence behind that.
	 */
	for (i = 0; !err && i < n; i++)
		if (folio_test_uptodate(small[i]) &&
		    copy_mc_highpage(folio_page(large, small[i]->index - index),
				     folio_page(small[i], 0)))
			err = -EIO;
	/* An error that arrived during the copies, which read without one. */
	for (i = 0; !err && i < n; i++)
		if (mxlfs_folio_poisoned(small[i]))
			err = -EIO;
	if (!err) {
		for (i = 0; i < n; i++) {
			if (folio_test_uptodate(small[i]))
				mxlfs_count_folio(inode, small[i], -1);
			generic_error_remove_folio(mapping, small[i]);
		}
		/* Nothing can have refilled the range under both locks. */
		err = filemap_add_folio(mapping, large, index, gfp);
		if (WARN_ON_ONCE(err)) {
			/* The copied contents are lost; keep the range zero-filled. */
			folio_put(large);
			large = NULL;
		} else {
			folio_mark_uptodate(large);
			folio_mark_dirty(large);
			mxlfs_count_folio(inode, large, 1);
			folio_unlock(large);
		}
	}
	for (i = 0; i < n; i++) {
		folio_unlock(small[i]);
		folio_put(small[i]);
	}
	kvfree(small);
	if (large)
		folio_put(large);
	WRITE_ONCE(MXLFS_I(inode)->fold_end, 0);
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
	loff_t new_size = offset + len, size;
	pgoff_t index, end;
	long ret;

	if (mode & ~FALLOC_FL_KEEP_SIZE)
		return -EOPNOTSUPP;

	inode_lock(inode);
	ret = -EPERM;
	if (mi->view)
		goto out;
	/* A view needs its sources at its own size, as setattr does. */
	ret = -EBUSY;
	if (!(mode & FALLOC_FL_KEEP_SIZE) && new_size > i_size_read(inode) &&
	    atomic_read(&mi->view_users))
		goto out;
	ret = inode_newsize_ok(inode, new_size);
	if (ret)
		goto out;

	index = offset >> PAGE_SHIFT;
	end = DIV_ROUND_UP(new_size, PAGE_SIZE);
	size = i_size_read(inode);
	if (!(mode & FALLOC_FL_KEEP_SIZE))
		size = max(size, new_size);
	if (lazy) {
		if (!mapping->nrpages)
			mi->lazy = true;
		index = end;
	} else {
		mi->lazy = false;
	}
	while (index < end) {
		struct folio *folio = filemap_get_folio(mapping, index);
		unsigned int within = mxlfs_fit_order(index, end);
		unsigned int order = mxlfs_alloc_order(sbi, index, end, size);

		if (!IS_ERR(folio)) {
			pgoff_t next = folio_next_index(folio);
			bool fold = folio_order(folio) < order && folio->index == index;

			/* Not held across the fold, which counts references. */
			folio_put(folio);
			/* Pages a lazily sized file got on touch become one folio. */
			if (fold && !mxlfs_over_limit(sbi, PAGE_SIZE << order) &&
			    !mxlfs_fold(inode, index, order, gfp)) {
				index += 1UL << order;
				continue;
			}
			/*
			 * One read_folio refused at size=: filled here, zeroed.
			 * Being in the page cache it is charged to size= already,
			 * so the limit is checked as read_folio checks it.
			 */
			folio = filemap_get_folio(mapping, index);
			if (!IS_ERR(folio) && folio_test_uptodate(folio)) {
				folio_put(folio);
			} else if (!IS_ERR(folio)) {
				folio_lock(folio);
				if (folio->mapping == mapping && !folio_test_uptodate(folio)) {
					if (mxlfs_over_limit(sbi, 0)) {
						folio_unlock(folio);
						folio_put(folio);
						ret = -ENOSPC;
						goto out;
					}
					folio_zero_range(folio, 0, folio_size(folio));
					folio_mark_uptodate(folio);
					folio_mark_dirty(folio);
					mxlfs_count_folio(inode, folio, 1);
				}
				folio_unlock(folio);
				folio_put(folio);
				mxlfs_settle(inode);
			}
			index = next;
			continue;
		}

		for (;;) {
			u64 bytes = PAGE_SIZE << order;
			int err;

			if (mxlfs_over_limit(sbi, bytes)) {
				/*
				 * mxlfs_alloc_order read allocated before; a
				 * racing allocation may have taken the room
				 * since. A folio past EOF falls back to the
				 * largest within it, which may still fit.
				 */
				if (order > within) {
					order = within;
					continue;
				}
				ret = -ENOSPC;
				goto out;
			}
			folio = mxlfs_alloc_folio(inode, gfp | __GFP_ZERO |
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
			/*
			 * No large folio, or a smaller one already overlaps. A
			 * folio past EOF falls back to the largest within it.
			 */
			order = order > within ? within : 0;
		}
		folio_mark_uptodate(folio);
		folio_mark_dirty(folio);
		mxlfs_count_folio(inode, folio, 1);
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

/*
 * MXLFS_IOC_ALLOCATE: fallocate(FALLOC_FL_KEEP_SIZE) over the whole file
 * whether or not the caller allocates lazily, which vfs_fallocate has no
 * way to be told. So this makes vfs_fallocate's checks for that mode
 * itself: a writable file that is not immutable (an append-only one takes
 * preallocation) and not a swap file, which mxlfs cannot be but the
 * check costs nothing, the LSM and fanotify permission hooks, which
 * rw_verify_area runs as vfs_fallocate does, freeze protection, and the
 * modify event on success. An empty file has nothing to allocate: where
 * fallocate refuses a zero length, this does nothing and succeeds, unless
 * the file is a view, which it refuses as at any size.
 */
static long mxlfs_ioc_allocate(struct file *file)
{
	struct inode *inode = file_inode(file);
	loff_t pos, size = i_size_read(inode);
	long ret;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!size)
		return READ_ONCE(MXLFS_I(inode)->view) ? -EPERM : 0;
	if (IS_IMMUTABLE(inode))
		return -EPERM;
	if (IS_SWAPFILE(inode))
		return -ETXTBSY;
	/*
	 * rw_verify_area takes a size_t count and refuses one past SSIZE_MAX,
	 * which a 32-bit size_t reaches below s_maxbytes. The file goes to it
	 * in pieces it accepts, one on 64 bits, so the hooks see the whole
	 * range, as vfs_fallocate's loff_t length shows it to them; cutting it
	 * at MAX_RW_COUNT, as read and write do, would hide its tail.
	 */
	for (pos = 0; pos < size; pos += SSIZE_MAX) {
		loff_t at = pos;

		ret = rw_verify_area(WRITE, file, &at, min_t(loff_t, size - pos, SSIZE_MAX));
		if (ret)
			return ret;
	}
	file_start_write(file);
	ret = mxlfs_allocate(file, FALLOC_FL_KEEP_SIZE, 0, size, false);
	if (!ret)
		fsnotify_modify(file);
	file_end_write(file);
	return ret;
}

static int mxlfs_setattr(MXLFS_IDMAP *idmap, struct dentry *dentry,
			 struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct mxlfs_inode *mi = MXLFS_I(inode);
	loff_t was = i_size_read(inode);
	int error;

	if ((attr->ia_valid & ATTR_SIZE) && attr->ia_size != was &&
	    (mi->view || atomic_read(&mi->view_users)))
		return -EBUSY;

	error = simple_setattr(idmap, dentry, attr);
	if (error)
		return error;

	if (attr->ia_valid & ATTR_SIZE) {
		mxlfs_settle(inode);
		if (attr->ia_size < was)
			mxlfs_recount(inode);
	}
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

	/*
	 * Zeroed either way: a folio refused here stays in the page cache, not
	 * uptodate, and the core reads it again when it is next used. What
	 * mxlfs uses directly, views and folds, takes it for a hole.
	 */
	folio_zero_range(folio, 0, folio_size(folio));
	if (mxlfs_over_limit(MXLFS_SB(inode->i_sb), 0))
		err = -ENOSPC;
	if (err) {
		folio_unlock(folio);
		return err;
	}
	folio_mark_uptodate(folio);
	folio_mark_dirty(folio);
	mxlfs_count_folio(inode, folio, 1);
	folio_unlock(folio);
	mxlfs_settle(inode);
	return 0;
}

static int mxlfs_write_begin(const struct kiocb *iocb, struct address_space *mapping,
			     loff_t pos, unsigned int len, struct folio **foliop,
			     void **fsdata)
{
	struct folio *folio;
	pgoff_t end;
	int err;

	if (mxlfs_over_limit(MXLFS_SB(mapping->host->i_sb), len))
		return -ENOSPC;
	/* simple_write_begin takes a single page where there is none. */
	mxlfs_place(mapping->host, pos >> PAGE_SHIFT, (pos >> PAGE_SHIFT) + 1, 0);
	err = simple_write_begin(iocb, mapping, pos, len, foliop, fsdata);
	if (err)
		return err;
	/*
	 * As shmem_write_begin: no write into a folio holding a page with a
	 * memory error, which stays in the file (see mxlfs_unpoisoned). Until
	 * memory_failure() sets has_hwpoisoned, only the poisoned page's own
	 * flag shows it, so the pages written are tested as well; not the
	 * whole folio, as a write of 4 KiB into a 2 MiB one would read 512
	 * flags. len may run past the folio, which the caller clamps it to.
	 */
	folio = *foliop;
	end = min_t(pgoff_t, DIV_ROUND_UP(pos + len, PAGE_SIZE), folio_next_index(folio));
	if (folio_contain_hwpoisoned_page(folio) ||
	    mxlfs_pages_poisoned(folio, (pos >> PAGE_SHIFT) - folio->index, end - folio->index)) {
		folio_unlock(folio);
		folio_put(folio);
		return -EIO;
	}
	mxlfs_settle(mapping->host);
	return 0;
}

/*
 * simple_write_end is static in 6.18; ram_aops carries it. It marks a folio
 * that write_begin created uptodate, and releases it.
 */
static int mxlfs_write_end(const struct kiocb *iocb, struct address_space *mapping,
			   loff_t pos, unsigned int len, unsigned int copied,
			   struct folio *folio, void *fsdata)
{
	bool fresh = !folio_test_uptodate(folio);
	int nid = folio_nid(folio);
	long nr = folio_nr_pages(folio);
	int ret = ram_aops.write_end(iocb, mapping, pos, len, copied, folio, fsdata);

	if (fresh)
		mxlfs_count(mapping->host, nid, nr);
	return ret;
}

/*
 * No .migrate_folio: move_to_new_folio() then calls
 * fallback_migrate_folio(), which refuses a dirty folio, and every
 * uptodate mxlfs folio is dirty, as nothing writes it back. Every caller
 * of migrate_pages() therefore fails on one: compaction, NUMA balancing,
 * soft-offline, memory offlining, CMA allocation, mbind() and
 * move_pages(), so none of them undoes MXLFS_IOC_SET_NODE placement.
 * mxlfs_move_block clears the dirty flag only while it holds a reference
 * of its own, which fails a migration's folio_ref_freeze().
 *
 * Dirtying through noop_dirty_folio() accounts nothing, but a split that
 * drops part of a dirty folio past EOF (__folio_split in
 * mm/huge_memory.c and the helpers it calls) still calls
 * folio_account_cleaned(), without the mapping_can_writeback() check
 * that __folio_cancel_dirty() makes. Each such split takes the dropped
 * pages off NR_FILE_DIRTY, NR_ZONE_WRITE_PENDING and WB_RECLAIMABLE,
 * which never counted them, and adds them to the task's
 * cancelled_write_bytes: those counters drift. The i_wb assignment in
 * mxlfs_get_inode only keeps the split from dereferencing NULL.
 */
static const struct address_space_operations mxlfs_aops = {
	.read_folio	= mxlfs_read_folio,
	.write_begin	= mxlfs_write_begin,
	.write_end	= mxlfs_write_end,
	.dirty_folio	= noop_dirty_folio,
};

/*
 * With a preferred node, places the folios the core would allocate on a
 * fault in a hole: for a VM_HUGEPAGE mapping, the window the core's forced
 * readahead takes, two spans of folios of at most 2 MiB (one with
 * VM_RAND_READ) from the span of the fault, up to EOF; otherwise the
 * faulting page. A fault on a page that is there, or past
 * EOF, is left to the core, which allocates nothing for it.
 */
static vm_fault_t mxlfs_fault(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	pgoff_t end = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	struct folio *folio;

	/* A mapping made as the file became a view; see mxlfs_view_set_sources. */
	if (READ_ONCE(MXLFS_I(inode)->view))
		return VM_FAULT_SIGBUS;
	/*
	 * A fold holds the invalidate lock across its zap, copy and page
	 * cache replacement. A fault that waits for it here holds no folio,
	 * where one waiting for a folio's lock would hold a reference the
	 * fold must count as one it cannot tell from a get_user_pages store.
	 * Not held across filemap_fault, which takes it to add a folio. Only
	 * a fault in the range a fold works on waits, so a move or a recount
	 * holding the lock does not hold up faults; the range is read without
	 * the lock, and a fault that misses it only waits on the folio lock,
	 * which a fold then counts and retries on. It waits as filemap_fault
	 * waits on the lock to add a folio, also with FAULT_FLAG_RETRY_NOWAIT.
	 */
	if (vmf->pgoff >= READ_ONCE(MXLFS_I(inode)->fold_start) &&
	    vmf->pgoff < READ_ONCE(MXLFS_I(inode)->fold_end)) {
		filemap_invalidate_lock_shared(inode->i_mapping);
		filemap_invalidate_unlock_shared(inode->i_mapping);
	}
	if (!mxlfs_placed(inode) || vmf->pgoff >= end)
		return filemap_fault(vmf);
	folio = filemap_get_folio(inode->i_mapping, vmf->pgoff);
	if (!IS_ERR(folio)) {
		folio_put(folio);
		return filemap_fault(vmf);
	}
	if ((vmf->vma->vm_flags & VM_HUGEPAGE) && mapping_large_folio_support(inode->i_mapping)) {
		unsigned int order = min_t(unsigned int, mapping_max_folio_order(inode->i_mapping),
					   get_order(SZ_2M));
		pgoff_t span = 1UL << order;
		pgoff_t index = round_down(vmf->pgoff, span);
		pgoff_t nr = vmf->vma->vm_flags & VM_RAND_READ ? span : 2 * span;

		mxlfs_place(inode, index, min(index + nr, end), order);
	} else {
		mxlfs_place(inode, vmf->pgoff, vmf->pgoff + 1, 0);
	}
	return filemap_fault(vmf);
}

/* Maps nothing of a view: its own page cache is not what it shows. */
static vm_fault_t mxlfs_map_pages(struct vm_fault *vmf, pgoff_t start, pgoff_t end)
{
	if (READ_ONCE(MXLFS_I(file_inode(vmf->vma->vm_file))->view))
		return 0;
	return filemap_map_pages(vmf, start, end);
}

/* No page_mkwrite: shared writable mappings need no dirty tracking. */
static const struct vm_operations_struct mxlfs_file_vm_ops = {
	.fault		= mxlfs_fault,
	.map_pages	= mxlfs_map_pages,
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
 * Locks a view fault's source folio as __folio_lock_or_retry would, which a
 * module cannot call, so that a fault that may be retried waits for a
 * folio lock without the mmap lock, unless the caller asked to keep it, as
 * filemap_fault does. The folio locks met here are short ones: another
 * view fault's until its mapping is installed, mxlfs_view_zap's, a move's
 * while it cleans the folio before its window, and read_folio's or
 * write_begin's. A fold or a move window holds the source's invalidate
 * lock, so a view fault waits for those on that lock, before this, as
 * filemap_fault waits on it to add a page. Returns 0 with the folio
 * locked, or VM_FAULT_RETRY with the invalidate locks of vmap and src
 * dropped and the folio's reference put.
 */
static vm_fault_t mxlfs_view_lock_folio(struct vm_fault *vmf, struct folio *folio,
					struct address_space *vmap, struct address_space *src)
{
	unsigned int flags = vmf->flags;

	if (folio_trylock(folio))
		return 0;
	if (!fault_flag_allow_retry_first(flags)) {
		if (!(flags & FAULT_FLAG_KILLABLE)) {
			folio_lock(folio);
			return 0;
		}
		if (!folio_lock_killable(folio))
			return 0;
		/* A fatal signal: the core then expects the fault lock dropped. */
		up_read(&src->invalidate_lock);
		filemap_invalidate_unlock_shared(vmap);
		release_fault_lock(vmf);
	} else {
		up_read(&src->invalidate_lock);
		filemap_invalidate_unlock_shared(vmap);
		if (!(flags & FAULT_FLAG_RETRY_NOWAIT)) {
			release_fault_lock(vmf);
			if (flags & FAULT_FLAG_KILLABLE)
				folio_wait_locked_killable(folio);
			else
				folio_wait_locked(folio);
		}
	}
	folio_put(folio);
	return VM_FAULT_RETRY;
}

/*
 * When every page the source folio covers selects that source, returns the
 * folio locked, so the core maps it as it maps page cache: a PMD-sized folio
 * with one PMD, any other large folio with all of its PTEs in one fault.
 * Otherwise maps the one page. Either way the rmap entries land on the
 * source's folio.
 *
 * They sit in a VMA whose file is the view, while every walk over a
 * folio's mappings goes through folio->mapping, the source's:
 * rmap_walk_file (folio_mkclean, try_to_unmap, try_to_migrate) and
 * collect_procs_file in memory-failure never visit them, and
 * unmap_mapping_pages on the source does not zap them. A change to a
 * source that needs its mappings gone therefore refuses while a view maps
 * it: mxlfs_fold and mxlfs_move_block (MXLFS_REFUSE_VIEW) find the folio
 * still mapped after zapping the source's own mappings, and mxlfs_setattr,
 * mxlfs_fallocate and mxlfs_write_iter refuse a size change while a view
 * uses the source.
 *
 * Memory failure on a source folio leaves the poisoned page in the file,
 * with or without a view (see mxlfs_unpoisoned). A view's entries also
 * keep it from unmapping the page: it cannot split a large folio they
 * map, nor unmap a small one, and gives up before truncate_error_folio(),
 * its only call into the address space, so no address space operation
 * could zap them. They keep reading the page until the mapping goes, or
 * until a select that changes the selection, or a replace of the source,
 * of any page in the same PMD span zaps them (mxlfs_view_zap). Reading
 * the bad memory itself through them raises SIGBUS: memory_failure()
 * finds the page already poisoned and kill_accessing_process() signals
 * the reader. The rest of the page reads its old contents. A fault
 * refuses the page with VM_FAULT_HWPOISON. __do_fault() checks
 * vmf->page, but finish_fault() maps every other page of a large folio
 * unchecked, so a folio holding a poisoned page is mapped a page at a
 * time and each page is checked here.
 */
static vm_fault_t mxlfs_view_fault(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct mxlfs_view *view = READ_ONCE(MXLFS_I(inode)->view);
	struct address_space *src;
	struct folio *folio;
	vm_fault_t ret = VM_FAULT_LOCKED;
	bool whole = false;
	long p;
	u8 source;

	if (vmf->pgoff >= view->pages)
		return VM_FAULT_SIGBUS;

	filemap_invalidate_lock_shared(inode->i_mapping);
	source = view->sel[vmf->pgoff];
	src = view->sources[source]->i_mapping;
	/*
	 * The source's lock too, so a fold of the source (mxlfs_fold) cannot
	 * remove the folio between the lookup and the insert or lock below.
	 * Always view, then source; nothing takes a view's lock under a
	 * source's.
	 */
	down_read_nested(&src->invalidate_lock, SINGLE_DEPTH_NESTING);
	/*
	 * A hole in the source, or a folio that is not uptodate, which the
	 * core never maps either: sources are expected to be allocated.
	 * mxlfs never clears uptodate, and under the folio lock read_folio
	 * is not setting it. Without the lock, where one page is inserted,
	 * a folio read_folio is still filling faults SIGBUS, as the hole it
	 * was when the fault looked it up.
	 */
	folio = filemap_get_folio(src, vmf->pgoff);
	if (!IS_ERR(folio) &&
	    mxlfs_view_uniform(view, folio->index, folio_nr_pages(folio), source)) {
		/*
		 * Held until the core has mapped it; see mxlfs_view_zap. Each
		 * page's own flag, as memory_failure() sets PageHWPoison
		 * before has_hwpoisoned (see mxlfs_unpoisoned); a folio with
		 * a poisoned page goes the page-at-a-time way below.
		 */
		ret = mxlfs_view_lock_folio(vmf, folio, inode->i_mapping, src);
		if (ret)
			return ret;
		ret = VM_FAULT_LOCKED;
		whole = folio_test_uptodate(folio);
		for (p = 0; whole && p < folio_nr_pages(folio); p++)
			whole = !PageHWPoison(folio_page(folio, p));
		if (!whole)
			folio_unlock(folio);
	}
	if (whole) {
		vmf->page = folio_file_page(folio, vmf->pgoff);
	} else if (IS_ERR(folio)) {
		ret = VM_FAULT_SIGBUS;
	} else if (!folio_test_uptodate(folio)) {
		folio_put(folio);
		ret = VM_FAULT_SIGBUS;
	} else if (PageHWPoison(folio_file_page(folio, vmf->pgoff))) {
		folio_put(folio);
		ret = VM_FAULT_HWPOISON;
	} else {
		/* The invalidate lock keeps a zap from passing this insert. */
		ret = vmf_insert_page_mkwrite(vmf, folio_file_page(folio, vmf->pgoff),
					      vmf->flags & FAULT_FLAG_WRITE);
		folio_put(folio);
	}
	up_read(&src->invalidate_lock);
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
	loff_t size;
	long ret;

	/* Flags first, on any regular file: callers probe with ~0. */
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.flags)
		return -EINVAL;
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!req.count || req.count > MXLFS_VIEW_MAX_SOURCES ||
	    memchr_inv(req.fds + req.count, 0,
		       (MXLFS_VIEW_MAX_SOURCES - req.count) * sizeof(req.fds[0])))
		return -EINVAL;
	/*
	 * The view's size until its lock is taken below, where it is checked
	 * again: a truncate in between makes the call fail as it would had
	 * the truncate come first. A source's size is fixed from the moment
	 * it counts the view as a user: truncate, fallocate and write all
	 * refuse to change it.
	 */
	size = i_size_read(inode);

	view = kzalloc(sizeof(*view), GFP_KERNEL_ACCOUNT);
	if (!view)
		return -ENOMEM;
	for (i = 0; i < req.count; i++) {
		CLASS(fd, src)(req.fds[i]);
		struct inode *si;

		ret = -EBADF;
		if (fd_empty(src) || !(fd_file(src)->f_mode & FMODE_READ))
			goto fail;
		si = file_inode(fd_file(src));
		ret = -EINVAL;
		if (si->i_sb != inode->i_sb || !S_ISREG(si->i_mode) || si == inode)
			goto fail;
		/* Under the lock that setattr, fallocate and SET_SOURCES on si take. */
		inode_lock(si);
		if (!MXLFS_I(si)->view && i_size_read(si) == size) {
			atomic_inc(&MXLFS_I(si)->view_users);
			view->sources[view->count++] = igrab(si);
			ret = 0;
		}
		inode_unlock(si);
		if (ret)
			goto fail;
	}

	ret = -EINVAL;
	if (!size || size > MXLFS_VIEW_MAX_SIZE)
		goto fail;
	view->pages = DIV_ROUND_UP(size, PAGE_SIZE);
	ret = -ENOMEM;
	view->sel = kvzalloc(view->pages, GFP_KERNEL_ACCOUNT | __GFP_NOWARN);
	if (!view->sel)
		goto fail;

	/*
	 * Whether a file is a view is decided under the locks that make it
	 * one: the inode lock for write(), truncate and fallocate, the
	 * invalidate lock for mxlfs_place, and i_mmap_rwsem for mmap, which
	 * links a new vma into the mapping after ->mmap_prepare chose its operations;
	 * one linked after this has the file's, and their faults see the view.
	 * Once set, a view stays until the inode goes.
	 */
	inode_lock(inode);
	filemap_invalidate_lock(inode->i_mapping);
	i_mmap_lock_read(inode->i_mapping);
	ret = -EINVAL;
	if (i_size_read(inode) != size)
		goto unlock;
	ret = -EBUSY;
	if (mi->view || inode->i_mapping->nrpages ||
	    mapping_mapped(inode->i_mapping) || atomic_read(&mi->view_users))
		goto unlock;
	/* Read without a lock elsewhere, so published whole. */
	smp_store_release(&mi->view, view);
	ret = 0;
unlock:
	i_mmap_unlock_read(inode->i_mapping);
	filemap_invalidate_unlock(inode->i_mapping);
	inode_unlock(inode);
	if (!ret)
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
	struct mxlfs_view *view = READ_ONCE(MXLFS_I(inode)->view);
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
	struct mxlfs_view *view = READ_ONCE(MXLFS_I(inode)->view);
	struct mxlfs_view_replace req;
	pgoff_t start = 0, end = 0, p, q;
	struct inode *si, *old;
	long ret;

	/* Flags first, on any regular file: callers probe with ~0. */
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.flags)
		return -EINVAL;
	if (!view)
		return -EINVAL;
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (req.source >= view->count)
		return -EINVAL;

	CLASS(fd, src)(req.fd);
	if (fd_empty(src) || !(fd_file(src)->f_mode & FMODE_READ))
		return -EBADF;
	si = file_inode(fd_file(src));
	if (si->i_sb != inode->i_sb || !S_ISREG(si->i_mode) || si == inode)
		return -EINVAL;
	/* Under the lock that setattr, fallocate and SET_SOURCES on si take. */
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

/* NUMA placement: preference, holds and moves */

static bool mxlfs_held_by_others(struct file *file)
{
	return atomic_read(&MXLFS_I(file_inode(file))->holds) > (file->private_data == MXLFS_HELD);
}

static long mxlfs_set_node(struct file *file, struct mxlfs_node_pref __user *arg)
{
	struct inode *inode = file_inode(file);
	struct mxlfs_node_pref req;
	long ret = -EBUSY;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.flags || (req.node != NUMA_NO_NODE && !mxlfs_node_valid(req.node)))
		return -EINVAL;
	if (!capable(CAP_SYS_NICE))
		return -EPERM;
	inode_lock(inode);
	if (MXLFS_I(inode)->view) {
		ret = -EPERM;
	} else if (!mxlfs_held_by_others(file)) {
		WRITE_ONCE(MXLFS_I(inode)->pref, req.node);
		ret = 0;
	}
	inode_unlock(inode);
	return ret;
}

/* Under the inode lock, so a move sees every hold taken before it began. */
static long mxlfs_hold(struct file *file, u32 __user *arg)
{
	struct inode *inode = file_inode(file);
	struct mxlfs_sb_info *sbi = MXLFS_SB(inode->i_sb);
	long ret = 0;
	u32 on;

	if (get_user(on, arg))
		return -EFAULT;
	if (on > 1)
		return -EINVAL;
	if (!capable(CAP_SYS_NICE))
		return -EPERM;
	inode_lock(inode);
	if (MXLFS_I(inode)->view) {
		ret = -EPERM;
	} else if (on && file->private_data != MXLFS_HELD) {
		file->private_data = MXLFS_HELD;
		atomic_inc(&MXLFS_I(inode)->holds);
		atomic64_inc(&sbi->holds);
	} else if (!on && file->private_data == MXLFS_HELD) {
		file->private_data = NULL;
		atomic_dec(&MXLFS_I(inode)->holds);
		atomic64_dec(&sbi->holds);
	}
	inode_unlock(inode);
	return ret;
}

/* The last close of an open file ends its hold. */
static int mxlfs_release(struct inode *inode, struct file *file)
{
	if (file->private_data == MXLFS_HELD) {
		atomic_dec(&MXLFS_I(inode)->holds);
		atomic64_dec(&MXLFS_SB(inode->i_sb)->holds);
	}
	return 0;
}

static long mxlfs_node_pages(struct file *file, struct mxlfs_node_pages __user *arg)
{
	struct inode *inode = file_inode(file);
	struct mxlfs_inode *mi = MXLFS_I(inode);
	u64 __user *out;
	struct mxlfs_node_pages req;
	u64 *walked = NULL;
	unsigned int nid, n;
	long ret = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if ((req.flags & ~MXLFS_NODE_PAGES_RECOUNT) || req.pad)
		return -EINVAL;
	if (req.flags & MXLFS_NODE_PAGES_RECOUNT) {
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		walked = kcalloc(nr_node_ids, sizeof(*walked), GFP_KERNEL);
		if (!walked)
			return -ENOMEM;
		inode_lock(inode);
		filemap_invalidate_lock(inode->i_mapping);
		mxlfs_walk_nodes(inode, walked);
		filemap_invalidate_unlock(inode->i_mapping);
		inode_unlock(inode);
	}
	out = u64_to_user_ptr(req.pages_ptr);
	n = min(req.nr_nodes, nr_node_ids);
	for (nid = 0; nid < n && !ret; nid++) {
		u64 pages = walked ? walked[nid] : atomic_long_read(&mi->node_pages[nid]);

		if (put_user(pages, out + nid))
			ret = -EFAULT;
	}
	kfree(walked);
	if (ret)
		return ret;
	req.nr_nodes = nr_node_ids;
	req.pref = READ_ONCE(mi->pref);
	req.effective = mxlfs_effective_node(inode);
	req.holds = atomic_read(&mi->holds);
	req.pad = 0;
	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

static long mxlfs_mount_nodes(struct file *file, struct mxlfs_mount_nodes __user *arg)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(file_inode(file)->i_sb);
	struct mxlfs_node_stat __user *out;
	struct mxlfs_mount_nodes req;
	unsigned int nid;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if (req.flags)
		return -EINVAL;
	out = u64_to_user_ptr(req.stats_ptr);
	for (nid = 0; nid < min(req.nr, nr_node_ids); nid++) {
		struct mxlfs_node_counts *c = &sbi->node[nid];
		struct mxlfs_node_stat st = {
			.pages = atomic64_read(&c->pages),
			.fallback_folios = atomic64_read(&c->fallback_folios),
			.fallback_pages = atomic64_read(&c->fallback_pages),
			.moved_in_pages = atomic64_read(&c->moved_in),
			.moved_out_pages = atomic64_read(&c->moved_out),
		};

		if (copy_to_user(out + nid, &st, sizeof(st)))
			return -EFAULT;
	}
	req.nr = nr_node_ids;
	return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

/* A folio of a file being moved, by index, with its reserved destination. */
struct mxlfs_block {
	pgoff_t index;
	unsigned int order;
	unsigned int reason;	/* MXLFS_REFUSE_* once refused */
	bool moved;
	struct folio *dst;
};

/*
 * Counts the file's folios into req and returns how many are off node;
 * with blocks, records up to cap of them, refusing the poisoned and the
 * pinned ones.
 */
static size_t mxlfs_move_scan(struct address_space *mapping, int node, struct mxlfs_move *req,
			      struct mxlfs_block *blocks, size_t cap)
{
	struct folio_batch fbatch;
	pgoff_t next = 0;
	size_t off = 0;
	unsigned int i;

	req->blocks = 0;
	req->blocks_on_node = 0;
	folio_batch_init(&fbatch);
	while (filemap_get_folios(mapping, &next, ULONG_MAX, &fbatch)) {
		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			struct folio *f = fbatch.folios[i];

			/*
			 * One read_folio refused holds none of the file's data;
			 * read_folio or ALLOCATE fills it before it is used. It
			 * is charged to size= as any page in the cache, but
			 * counted per node only once uptodate, and a move leaves
			 * it out as the per-node counts (MXLFS_IOC_NODE_PAGES) do.
			 */
			if (!folio_test_uptodate(f))
				continue;
			req->blocks++;
			if (folio_nid(f) == node) {
				req->blocks_on_node++;
				continue;
			}
			if (blocks && off < cap)
				blocks[off] = (struct mxlfs_block) {
					.index = f->index,
					.order = folio_order(f),
					.reason = mxlfs_folio_poisoned(f) ?
						  MXLFS_REFUSE_POISONED :
						  folio_maybe_dma_pinned(f) ?
						  MXLFS_REFUSE_PINNED : 0,
				};
			off++;
		}
		folio_batch_release(&fbatch);
		cond_resched();
	}
	return off;
}

static void mxlfs_max(atomic64_t *v, s64 x)
{
	s64 old = atomic64_read(v);

	while (x > old && !atomic64_try_cmpxchg(v, &old, x))
		;
}

static unsigned int mxlfs_window_bucket(u64 ns)
{
	static const u64 bounds[MXLFS_WINDOW_BUCKETS - 1] = {
		64 * NSEC_PER_USEC, 128 * NSEC_PER_USEC, 256 * NSEC_PER_USEC, 512 * NSEC_PER_USEC,
		NSEC_PER_MSEC, 2 * NSEC_PER_MSEC, 4 * NSEC_PER_MSEC, 8 * NSEC_PER_MSEC,
	};
	unsigned int b = 0;

	while (b < ARRAY_SIZE(bounds) && ns >= bounds[b])
		b++;
	return b;
}

/* Passes over the folios left after one was written to while copied ahead. */
#define MXLFS_MOVE_PASSES 3

/* How mxlfs_move_block treats a folio. */
enum mxlfs_block_mode {
	MXLFS_BLOCK_PROBE,	/* zap and check only */
	MXLFS_BLOCK_DEFER,	/* copy ahead; leave a folio written meanwhile for later */
	MXLFS_BLOCK_FINAL,	/* copy ahead; copy one written meanwhile in its window */
};

static bool mxlfs_block_is(struct folio *folio, struct address_space *mapping,
			   const struct mxlfs_block *b)
{
	return folio->mapping == mapping && folio->index == b->index &&
	       folio_order(folio) == b->order;
}

/*
 * Moves the folio at b->index into b->dst, or returns why not. The copy is
 * made ahead of the window: src's mappings are made clean and read-only and
 * its dirty flag cleared under the folio lock, then src is copied with the
 * lock dropped. A write to src meanwhile maps it writable and dirty again,
 * or marks it dirty when a pin is released. Under the folio lock, src's
 * mappings are cleaned again: a mapping found dirty or writable, or a dirty
 * flag, means the copy may be stale. MXLFS_BLOCK_DEFER then leaves the folio
 * mapped for a later pass and sets *deferred, with no window;
 * MXLFS_BLOCK_FINAL zaps it and copies it again in its window. A mapping can
 * still turn writable between that check and the zap without the folio lock
 * (a write fault on a PTE, mprotect, a NUMA hinting fault), so the zap must
 * leave its dirty bit in the folio: zapping a PTE does, zapping a whole PMD
 * does not, splitting one does. A PMD is therefore split by zapping one page
 * of it first, and a folio the zap leaves dirty is copied again in its
 * window in either mode. A pinned folio is refused before the copy, as a pin
 * may write without a fault. Faults wait for the folio lock and then find
 * the copy; holes in the block's range cannot be filled meanwhile, as the
 * invalidate lock is held. The page cache holds one reference per page,
 * replace_page_cache_folio moves one, so the rest are moved here. A probe
 * copies nothing and stops after the zap: a view's mapping of the folio,
 * which only then tells itself apart from the file's own, or a pin refuses
 * it.
 *
 * A folio holding a page with a memory error is refused before each copy
 * and again before the replace, and so is one folio_mc_copy() finds bad:
 * the copy would carry the page's old contents into a clean frame and the
 * error would be gone from the file (see mxlfs_unpoisoned). The checks
 * narrow a race rather than close it, as memory_failure() sets the flag
 * without the folio lock. folio_mc_copy() keeps a copy that reads bad
 * memory from machine-checking the kernel only where it is
 * machine-check-safe, which depends on the architecture and the CPU, as
 * at mxlfs_fold.
 */
static unsigned int mxlfs_move_block(struct inode *inode, struct mxlfs_block *b, u64 *window,
				     enum mxlfs_block_mode mode, bool *deferred)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(inode->i_sb);
	struct address_space *mapping = inode->i_mapping;
	bool probe = mode == MXLFS_BLOCK_PROBE, was_dirty = false, written = false;
	bool moved = false;
	struct folio *src, *dst = b->dst;
	unsigned int reason = 0, tries;
	long nr;
	u64 t0;

	*window = 0;
	*deferred = false;
	src = filemap_get_folio(mapping, b->index);
	if (IS_ERR(src))
		return MXLFS_REFUSE_BUSY;
	nr = folio_nr_pages(src);
	if (!probe) {
		folio_lock(src);
		if (!mxlfs_block_is(src, mapping, b)) {
			folio_unlock(src);
			folio_put(src);
			return MXLFS_REFUSE_BUSY;
		}
		if (folio_maybe_dma_pinned(src))
			reason = MXLFS_REFUSE_PINNED;
		else if (mxlfs_folio_poisoned(src))
			reason = MXLFS_REFUSE_POISONED;
		if (reason) {
			folio_unlock(src);
			folio_put(src);
			return reason;
		}
		was_dirty = folio_test_clear_dirty(src);
		folio_mkclean(src);
		folio_unlock(src);
		if (folio_mc_copy(dst, src)) {
			folio_lock(src);
			if (was_dirty)
				folio_mark_dirty(src);
			folio_unlock(src);
			folio_put(src);
			return MXLFS_REFUSE_POISONED;
		}
	}
	filemap_invalidate_lock(mapping);
	folio_lock(src);
	t0 = ktime_get_ns();
	if (!mxlfs_block_is(src, mapping, b)) {
		reason = MXLFS_REFUSE_BUSY;
		goto unlock;
	}
	/*
	 * Copying needs the references to the locked, unmapped src to be the
	 * page cache's and the caller's, as migration requires: another may
	 * belong to a GUP user that writes. A fault that looked src up and
	 * waits for its lock holds one too; the lock is let go for it to map
	 * src and return, and src is cleaned and zapped again. A small folio
	 * may wait in a per-CPU LRU batch. A reference taken after the check
	 * is a fault that will find the copy, or a read() or splice that
	 * reads only: write() is excluded by the inode lock.
	 */
	for (tries = 0;; tries++) {
		t0 = ktime_get_ns();
		if (!probe && (folio_mkclean(src) || folio_test_dirty(src))) {
			written = true;
			if (mode == MXLFS_BLOCK_DEFER)
				break;
		}
		if (!probe && folio_test_pmd_mappable(src))
			unmap_mapping_pages(mapping, b->index, 1, false);
		unmap_mapping_pages(mapping, b->index, nr, false);
		if (folio_mapped(src))
			reason = MXLFS_REFUSE_VIEW;
		else if (folio_maybe_dma_pinned(src))
			reason = MXLFS_REFUSE_PINNED;
		else if (mxlfs_folio_poisoned(src))
			reason = MXLFS_REFUSE_POISONED;
		else if (!probe && folio_ref_count(src) != folio_expected_ref_count(src) + 1)
			reason = tries == 3 ? MXLFS_REFUSE_BUSY : 0;
		else
			break;
		if (reason)
			break;
		folio_unlock(src);
		usleep_range(100 << tries, 200 << tries);
		folio_lock(src);
	}
	if (reason || probe)
		goto unlock;
	/* Left before the zap: no window, and the folio stays mapped. */
	if (written && mode == MXLFS_BLOCK_DEFER) {
		*deferred = true;
		goto unlock;
	}
	/* A mapping made writable after the check and zapped dirty. */
	if (folio_test_dirty(src))
		written = true;

	if ((written && folio_mc_copy(dst, src)) || mxlfs_folio_poisoned(src)) {
		reason = MXLFS_REFUSE_POISONED;
		goto unlock;
	}
	__folio_set_locked(dst);
	if (folio_test_uptodate(src))
		folio_mark_uptodate(dst);
	/*
	 * replace_page_cache_folio() freezes no reference count, unlike
	 * __folio_migrate_mapping(), so the check above is what keeps a
	 * writer off src. The count may have grown since: a fault or read()
	 * that looked src up, or a speculative reference such as
	 * isolate_migratepages_block() takes, neither of which writes. It
	 * cannot have dropped below the check's, nor src have been mapped or
	 * pinned, while src stays locked under the invalidate lock.
	 */
	VM_WARN_ON_ONCE_FOLIO(folio_ref_count(src) < folio_expected_ref_count(src) + 1 ||
			      folio_mapped(src) || folio_maybe_dma_pinned(src), src);
	folio_ref_add(dst, nr - 1);
	replace_page_cache_folio(src, dst);
	folio_put_refs(src, nr - 1);
	if (was_dirty || folio_test_dirty(src))
		folio_mark_dirty(dst);
	/* replace_page_cache_folio moves NR_FILE_PAGES, not NR_FILE_THPS. */
	if (folio_test_pmd_mappable(src)) {
		lruvec_stat_mod_folio(src, NR_FILE_THPS, -nr);
		lruvec_stat_mod_folio(dst, NR_FILE_THPS, nr);
	}
	if (folio_test_uptodate(src)) {
		mxlfs_count(inode, folio_nid(src), -nr);
		mxlfs_count(inode, folio_nid(dst), nr);
	}
	atomic64_add(nr, &sbi->node[folio_nid(src)].moved_out);
	atomic64_add(nr, &sbi->node[folio_nid(dst)].moved_in);
	folio_add_lru(dst);
	folio_unlock(dst);
	b->dst = NULL;
	moved = true;
	was_dirty = false;
unlock:
	/* src stays in the page cache unless it was replaced. */
	if (was_dirty)
		folio_mark_dirty(src);
	folio_unlock(src);
	*window = ktime_get_ns() - t0;
	filemap_invalidate_unlock(mapping);
	folio_put(src);
	if (moved)
		folio_put(dst);
	return reason;
}

/*
 * The whole call holds the inode lock, which keeps write(), fallocate,
 * truncation, folds, holds and other moves out; each block holds the
 * invalidate lock for its own window only. See mxlfs_uapi.h for the
 * contract.
 */
static long mxlfs_move(struct file *file, struct mxlfs_move __user *arg)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = inode->i_mapping;
	struct mxlfs_sb_info *sbi = MXLFS_SB(inode->i_sb);
	struct mxlfs_move_counts *mc = &sbi->move;
	struct mxlfs_block *blocks = NULL;
	u64 start = ktime_get_ns(), window;
	size_t off, i, at = 0, moved = 0, left;
	unsigned int pass;
	bool deferred;
	unsigned int stop = 0;	/* why the call stopped, at blocks[at] */
	struct mxlfs_move req;
	bool dry, strict;
	long ret = 0;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	if ((req.flags & ~(MXLFS_MOVE_DRY_RUN | MXLFS_MOVE_STRICT)) ||
	    !mxlfs_node_valid(req.node))
		return -EINVAL;
	dry = req.flags & MXLFS_MOVE_DRY_RUN;
	strict = req.flags & MXLFS_MOVE_STRICT;
	if (!dry && !capable(CAP_SYS_NICE))
		return -EPERM;
	memset_after(&req, 0, flags);
	if (READ_ONCE(MXLFS_I(inode)->view))
		return -EPERM;

	if (dry) {
		/*
		 * Without locks, so it delays no writer: what a move would find
		 * if it started now. The file may change between the scans; the
		 * second one counts and records what is there.
		 */
		off = mxlfs_move_scan(mapping, req.node, &req, NULL, 0);
		atomic64_inc(&mc->dry_runs);
		blocks = kvcalloc(off, sizeof(*blocks), GFP_KERNEL_ACCOUNT);
		if (off && !blocks)
			return -ENOMEM;
		off = min(off, mxlfs_move_scan(mapping, req.node, &req, blocks, off));
		if (off && mxlfs_held_by_others(file))
			stop = MXLFS_REFUSE_HELD;
		goto report;
	}

	inode_lock(inode);
	if (MXLFS_I(inode)->view) {
		inode_unlock(inode);
		return -EPERM;
	}
	/* The invalidate lock holds the set of folios still between the scans. */
	filemap_invalidate_lock(mapping);
	off = mxlfs_move_scan(mapping, req.node, &req, NULL, 0);
	blocks = kvcalloc(off, sizeof(*blocks), GFP_KERNEL_ACCOUNT);
	if (off && !blocks) {
		filemap_invalidate_unlock(mapping);
		inode_unlock(inode);
		return -ENOMEM;
	}
	mxlfs_move_scan(mapping, req.node, &req, blocks, off);
	filemap_invalidate_unlock(mapping);
	atomic64_inc(&mc->calls);

	if (off && mxlfs_held_by_others(file)) {
		stop = MXLFS_REFUSE_HELD;
		ret = -EBUSY;
		goto done;
	}
	/*
	 * Strict: a pinned, poisoned or view-mapped folio fails the call
	 * before any moves. Telling a view's mapping from the file's takes a
	 * zap, so every folio to move is zapped once here and once when it
	 * moves.
	 */
	for (i = 0; strict && i < off; i++) {
		if (!blocks[i].reason)
			blocks[i].reason = mxlfs_move_block(inode, &blocks[i], &window,
							    MXLFS_BLOCK_PROBE, &deferred);
		if (blocks[i].reason) {
			stop = blocks[i].reason;
			at = i;
			ret = -EBUSY;
			goto done;
		}
	}
	/* Every destination first: a move that cannot finish does not start. */
	for (i = 0; i < off; i++) {
		if (blocks[i].reason)
			continue;
		blocks[i].dst = __folio_alloc_node(mapping_gfp_mask(mapping) | __GFP_THISNODE |
						   __GFP_NORETRY | __GFP_NOWARN,
						   blocks[i].order, req.node);
		if (!blocks[i].dst) {
			stop = MXLFS_REFUSE_NOMEM;
			at = i;
			ret = -ENOMEM;
			goto done;
		}
	}
	/*
	 * A folio written to while it was copied ahead is left for a later
	 * pass, when its writer has moved on; the last pass copies such a
	 * folio again in its window.
	 */
	for (pass = 0, left = off; left && pass < MXLFS_MOVE_PASSES; pass++) {
		enum mxlfs_block_mode mode = pass + 1 < MXLFS_MOVE_PASSES ?
					     MXLFS_BLOCK_DEFER : MXLFS_BLOCK_FINAL;

		for (i = 0, left = 0; i < off; i++) {
			struct mxlfs_block *b = &blocks[i];

			if (b->reason || b->moved)
				continue;
			if (fatal_signal_pending(current)) {
				stop = MXLFS_REFUSE_SIGNAL;
				at = i;
				ret = -EINTR;
				goto moved;
			}
			b->reason = mxlfs_move_block(inode, b, &window, mode, &deferred);
			if (deferred) {
				left++;
				continue;
			}
			req.max_window_ns = max(req.max_window_ns, window);
			if (!b->reason) {
				atomic64_add(window, &mc->window_sum);
				atomic64_inc(&mc->window_hist[mxlfs_window_bucket(window)]);
				mxlfs_max(&mc->window_max, window);
				b->moved = true;
				moved++;
				req.bytes_moved += PAGE_SIZE << b->order;
			} else if (strict) {
				stop = b->reason;
				at = i;
				ret = -EAGAIN;
				goto moved;
			}
			cond_resched();
		}
	}
moved:
	if (moved == off && !mxlfs_held_by_others(file))
		WRITE_ONCE(MXLFS_I(inode)->pref, req.node);
done:
	inode_unlock(inode);
report:
	/*
	 * A block not moved is left for its own reason or for the one that
	 * stopped the call, which is the reason reported. A dry run moves
	 * nothing and reports what the move would leave.
	 */
	if (stop) {
		req.reason = stop;
		req.refused_offset = (u64)blocks[at].index << PAGE_SHIFT;
	}
	for (i = 0; i < off; i++) {
		struct mxlfs_block *b = &blocks[i];
		unsigned int reason = b->reason ? : stop;

		if (b->dst)
			folio_put(b->dst);
		if (b->moved || !reason)
			continue;
		if (!req.blocks_refused++ && !stop) {
			req.reason = reason;
			req.refused_offset = (u64)b->index << PAGE_SHIFT;
		}
		if (!dry)
			atomic64_inc(&mc->refused[reason]);
	}
	req.blocks_moved = moved;
	req.result = !req.blocks_refused ? MXLFS_MOVED_ALL :
		     req.blocks_refused < off ? MXLFS_MOVED_PARTIAL : MXLFS_MOVED_NONE;
	req.duration_ns = ktime_get_ns() - start;
	if (!dry) {
		atomic64_add(moved, &mc->blocks);
		atomic64_add(req.bytes_moved, &mc->bytes);
		atomic64_add(req.duration_ns, &mc->ns);
		if (req.result == MXLFS_MOVED_PARTIAL)
			atomic64_inc(&mc->calls_partial);
		else if (req.result == MXLFS_MOVED_NONE)
			atomic64_inc(&mc->calls_none);
	}
	kvfree(blocks);
	if (copy_to_user(arg, &req, sizeof(req)))
		return -EFAULT;
	return ret;
}

/*
 * MXLFS_IOC_STATS with any size: the size is that of the caller's struct.
 * A size below the first struct's is matched too, so mxlfs_stats() fails
 * it with EINVAL rather than ENOTTY.
 */
static bool mxlfs_is_stats(unsigned int cmd)
{
	return extensible_ioctl_valid(cmd, MXLFS_IOC_STATS, 0);
}

static long mxlfs_stats(struct file *file, void __user *arg, unsigned int cmd)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(file_inode(file)->i_sb);
	struct mxlfs_move_counts *mc = &sbi->move;
	struct mxlfs_stats st = {
		.bytes_allocated = atomic64_read(&sbi->allocated),
		.bytes_limit = sbi->limit,
		.view_selects = atomic64_read(&sbi->view_selects),
		.view_faults = atomic64_read(&sbi->view_faults),
		.view_replaces = atomic64_read(&sbi->view_replaces),
		.move_calls = atomic64_read(&mc->calls),
		.move_calls_partial = atomic64_read(&mc->calls_partial),
		.move_calls_none = atomic64_read(&mc->calls_none),
		.move_blocks = atomic64_read(&mc->blocks),
		.move_bytes = atomic64_read(&mc->bytes),
		.move_ns = atomic64_read(&mc->ns),
		.move_window_ns_sum = atomic64_read(&mc->window_sum),
		.move_window_ns_max = atomic64_read(&mc->window_max),
		.holds = atomic64_read(&sbi->holds),
		.dry_runs = atomic64_read(&mc->dry_runs),
		.wanted = READ_ONCE(sbi->wanted_count),
		.wanted_refused = atomic64_read(&sbi->wanted_refused),
	};
	size_t usize = _IOC_SIZE(cmd);
	unsigned int i;

	if (usize < MXLFS_STATS_SIZE_VER0)
		return -EINVAL;
	for (i = 0; i < nr_node_ids; i++) {
		st.numa_fallback_folios += atomic64_read(&sbi->node[i].fallback_folios);
		st.numa_fallback_bytes += atomic64_read(&sbi->node[i].fallback_pages) << PAGE_SHIFT;
	}
	for (i = 0; i < MXLFS_REFUSE_MAX; i++)
		st.move_refused[i] = atomic64_read(&mc->refused[i]);
	for (i = 0; i < MXLFS_WINDOW_BUCKETS; i++)
		st.move_window_hist[i] = atomic64_read(&mc->window_hist[i]);
	return copy_struct_to_user(arg, usize, &st, sizeof(st), NULL);
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
		req->pid = pid_vnr(w->owner->tgid);
		strscpy(req->path, w->path, sizeof(req->path));
		found = true;
		break;
	}
	spin_unlock(&sbi->wanted_lock);
	return found;
}

/*
 * Returns flow id to those not yet reported, after its copy to the caller
 * failed, and wakes the callers waiting for one. Another caller has not
 * taken it meanwhile, since it was marked; it may have appeared, been
 * failed or left the list, and then there is nothing to report.
 */
static void mxlfs_wanted_unreport(struct mxlfs_sb_info *sbi, u64 id)
{
	struct mxlfs_wanted *w;

	spin_lock(&sbi->wanted_lock);
	list_for_each_entry(w, &sbi->wanted, node) {
		if (w->id != id)
			continue;
		w->reported = false;
		wake_up_interruptible(&sbi->wanted_wq);
		break;
	}
	spin_unlock(&sbi->wanted_lock);
}

static long mxlfs_wanted_next(struct file *file, struct mxlfs_wanted_req __user *arg)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(file_inode(file)->i_sb);
	struct mxlfs_wanted_req *req;
	u32 flags, timeout_ms;
	long ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (get_user(flags, &arg->flags))
		return -EFAULT;
	if (flags)
		return -EINVAL;
	if (get_user(timeout_ms, &arg->timeout_ms))
		return -EFAULT;
	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	ret = wait_event_interruptible_timeout(sbi->wanted_wq,
					       mxlfs_wanted_take(sbi, req),
					       msecs_to_jiffies(timeout_ms));
	if (!ret) {
		ret = -ETIMEDOUT;
	} else if (ret > 0 && copy_to_user(arg, req, sizeof(*req))) {
		mxlfs_wanted_unreport(sbi, req->id);
		ret = -EFAULT;
	} else if (ret > 0) {
		ret = 0;
	}
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
	/* As fuse_dev_do_write: ERESTARTSYS (512) and up are kernel-internal. */
	if (req.error <= 0 || req.error >= ERESTARTSYS || req.pad)
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
	if (mxlfs_is_stats(cmd))
		return mxlfs_stats(file, (void __user *)arg, cmd);
	switch (cmd) {
	case MXLFS_IOC_WANTED_NEXT:
		return mxlfs_wanted_next(file, (void __user *)arg);
	case MXLFS_IOC_WANTED_FAIL:
		return mxlfs_wanted_fail(file, (void __user *)arg);
	case MXLFS_IOC_LAZY_ALLOC:
		return mxlfs_set_lazy(MXLFS_SB(file_inode(file)->i_sb), (u32 __user *)arg);
	case MXLFS_IOC_MOUNT_NODES:
		return mxlfs_mount_nodes(file, (void __user *)arg);
	}
	return -ENOTTY;
}

static long mxlfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mxlfs_view_range range;
	struct mxlfs_view *view;
	u32 selected;

	if (mxlfs_is_stats(cmd))
		return mxlfs_stats(file, (void __user *)arg, cmd);
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
		view = READ_ONCE(MXLFS_I(file_inode(file))->view);
		if (!view)
			return -EINVAL;
		selected = READ_ONCE(view->sel[0]);
		return put_user(selected, (u32 __user *)arg);
	case MXLFS_IOC_VIEW_REPLACE_SOURCE:
		return mxlfs_view_replace_source(file, (void __user *)arg);
	case MXLFS_IOC_LAZY_ALLOC:
		return mxlfs_set_lazy(MXLFS_SB(file_inode(file)->i_sb), (u32 __user *)arg);
	case MXLFS_IOC_ALLOCATE:
		return mxlfs_ioc_allocate(file);
	case MXLFS_IOC_SET_NODE:
		return mxlfs_set_node(file, (void __user *)arg);
	case MXLFS_IOC_NODE_PAGES:
		return mxlfs_node_pages(file, (void __user *)arg);
	case MXLFS_IOC_MOVE:
		return mxlfs_move(file, (void __user *)arg);
	case MXLFS_IOC_HOLD:
		return mxlfs_hold(file, (u32 __user *)arg);
	case MXLFS_IOC_MOUNT_NODES:
		return mxlfs_mount_nodes(file, (void __user *)arg);
	}
	return -ENOTTY;
}

static int mxlfs_mmap_prepare(struct vm_area_desc *desc)
{
	struct file *file = desc->file;
	struct mxlfs_inode *mi = MXLFS_I(file_inode(file));

	file_accessed(file);
	if (!READ_ONCE(mi->view)) {
		/*
		 * Faults on holes then allocate PMD-sized folios; not in a
		 * lazily sized file, where a header write must cost one page.
		 */
		if (!READ_ONCE(mi->lazy))
			mxlfs_desc_set(desc, VM_HUGEPAGE);
		desc->vm_ops = &mxlfs_file_vm_ops;
		return 0;
	}
	/* A view is read-only: writes would land in a source. */
	if (mxlfs_desc_shared_write(desc))
		return -EACCES;
	mxlfs_desc_clear(desc, VM_MAYWRITE);
	desc->vm_ops = &mxlfs_view_vm_ops;
	return 0;
}

/*
 * How many of the len bytes at pos read() and splice may return: all of
 * them, or those before the first page with a memory error, or -EIO when
 * pos is in that page. memory_failure() cannot drop a page from an mxlfs
 * file (MF_FAILED), so the page stays there with its old contents until
 * the file goes; shmem_file_read_iter and shmem_file_splice_read refuse
 * it by the same rule, testing PageHWPoison on each page they copy.
 *
 * filemap_read() and filemap_splice_read() test nothing, so this scans the
 * range before they run, and the caller clamps the read to what it
 * returns, instead of copying their loops. The scan walks the page cache
 * under RCU without taking references, as filemap_range_has_writeback()
 * does, so it adds no atomics on folios other readers share. A folio can
 * be freed and its memory reused meanwhile, and freeing zeroes its page
 * count before it stops being large, so the pages it tests come from the
 * xarray entry's order, as filemap_cachestat() takes it, bounded by the
 * range. The folio and the order are read from different slots, and a
 * fold can replace small folios with a larger one between the two reads,
 * so the entry is read again after the order: once the slot still holds
 * the folio at that order, the folio spans that many struct pages, which
 * stay memory map even if the folio is freed later, and no struct page
 * outside it is read. A second reload after the flags, ordered after
 * them, is what makes the result count, the check
 * filemap_get_read_batch() makes; after either reload fails the entry is
 * looked up again. Every XA_CHECK_SCHED folios it restarts the walk
 * after the entry and lets the CPU go, as memfd_tag_pins() does, so a
 * read of many GiB
 * does not hold the RCU read lock throughout. It tests every page's own
 * flag: memory_failure() sets PageHWPoison on the page before it sets
 * has_hwpoisoned on a large folio, so the folio's flag cannot rule a page
 * out. shmem's test races with a memory error that arrives after it, too;
 * here the window is the whole read rather than one page's copy, and a
 * page poisoned in it is read.
 */
static ssize_t mxlfs_unpoisoned(struct inode *inode, loff_t pos, size_t len)
{
	loff_t isize = i_size_read(inode);
	pgoff_t first = pos >> PAGE_SHIFT, end, index, p, last;
	XA_STATE(xas, &inode->i_mapping->i_pages, first);
	unsigned int folios = 0;
	struct folio *folio;
	ssize_t ret = len;
	int order;

	if (!IS_ENABLED(CONFIG_MEMORY_FAILURE) || !len || pos >= isize)
		return len;
	end = (min_t(loff_t, pos + len, isize) - 1) >> PAGE_SHIFT;
	rcu_read_lock();
	xas_for_each(&xas, folio, end) {
		if (xas_retry(&xas, folio) || xa_is_value(folio))
			continue;
		order = xas_get_order(&xas);
		/* The order belongs to this folio only if the slot still holds it. */
		smp_rmb();
		if (unlikely(folio != xas_reload(&xas))) {
			xas_reset(&xas);
			continue;
		}
		index = round_down(xas.xa_index, 1UL << order);
		last = min(index + (1UL << order) - 1, end);
		for (p = max(index, first); p <= last; p++)
			if (PageHWPoison(folio_page(folio, p - index)))
				break;
		/* The flags were read before the entry is checked again. */
		smp_rmb();
		if (unlikely(folio != xas_reload(&xas))) {
			xas_reset(&xas);
			continue;
		}
		if (p <= last) {
			ret = p == first ? -EIO : ((loff_t)p << PAGE_SHIFT) - pos;
			break;
		}
		if (++folios % XA_CHECK_SCHED == 0) {
			/* The entry ends at last, which the reload confirmed. */
			xas_set(&xas, last + 1);
			cond_resched_rcu();
		}
	}
	rcu_read_unlock();
	return ret;
}

static ssize_t mxlfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct mxlfs_view *view = READ_ONCE(MXLFS_I(inode)->view);
	size_t count;
	loff_t end;
	ssize_t ret;

	if (!view) {
		/*
		 * A read that finds no view reads the file as it is, also when
		 * it becomes a view meanwhile: it had no pages then, so holes
		 * read as zeroes, and the pages read_folio adds for them stay in
		 * the view's own page cache, charged against size=, until the
		 * inode goes. Nothing of the view maps or reads them.
		 *
		 * Without FMODE_NOWAIT no read gets here with IOCB_NOWAIT:
		 * preadv2 refuses RWF_NOWAIT, io_uring reads from a worker that
		 * may block. The core would allocate nothing for one: the noop
		 * bdi of a nodev mount has no readahead (ra_pages and io_pages
		 * 0), and filemap_create_folio refuses it. fadvise and madvise
		 * WILLNEED do nothing on that bdi.
		 */
		count = iov_iter_count(to);
		ret = mxlfs_unpoisoned(inode, iocb->ki_pos, count);
		if (ret < 0)
			return ret;
		iov_iter_truncate(to, ret);
		count -= ret;
		if (!(iocb->ki_flags & (IOCB_NOWAIT | IOCB_WAITQ)))
			mxlfs_place_read(inode, iocb->ki_pos, iov_iter_count(to));
		ret = generic_file_read_iter(iocb, to);
		iov_iter_reexpand(to, iov_iter_count(to) + count);
		return ret;
	}

	/*
	 * Copies from the selected source's page cache; holes read as zeroes.
	 * The lock is held only to look a folio up: copying to user memory
	 * can fault, and a fault on a view takes the same lock after mmap_lock.
	 */
	ret = 0;
	end = min_t(loff_t, i_size_read(inode), (loff_t)view->pages << PAGE_SHIFT);
	while (iov_iter_count(to) && iocb->ki_pos < end) {
		loff_t pos = iocb->ki_pos;
		size_t n = min_t(loff_t, iov_iter_count(to), end - pos);
		struct folio *folio;
		size_t copied;

		filemap_invalidate_lock_shared(inode->i_mapping);
		folio = filemap_get_folio(mxlfs_view_source(view, pos >> PAGE_SHIFT)->i_mapping,
					  pos >> PAGE_SHIFT);
		filemap_invalidate_unlock_shared(inode->i_mapping);
		/*
		 * One that is not uptodate reads as the hole it is; read_folio
		 * zeroes a folio it refuses, so this is defence behind it.
		 */
		if (!IS_ERR(folio) && !folio_test_uptodate(folio)) {
			folio_put(folio);
			folio = ERR_PTR(-ENODATA);
		}
		/* A page with a memory error ends the read, as in mxlfs_unpoisoned. */
		if (!IS_ERR(folio) && PageHWPoison(folio_file_page(folio, pos >> PAGE_SHIFT))) {
			folio_put(folio);
			if (!ret)
				ret = -EIO;
			break;
		}
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

/*
 * generic_file_write_iter, refusing a write that would grow a file a view
 * uses: a view needs its sources at its own size, as in setattr and
 * fallocate.
 */
static ssize_t mxlfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct mxlfs_inode *mi = MXLFS_I(inode);
	ssize_t ret;

	/* Under the lock that makes the file a view, which a write may not grow. */
	inode_lock(inode);
	ret = mi->view ? -EPERM : generic_write_checks(iocb, from);
	if (ret > 0 && iocb->ki_pos + ret > i_size_read(inode) &&
	    atomic_read(&mi->view_users))
		ret = -EBUSY;
	if (ret > 0)
		ret = __generic_file_write_iter(iocb, from);
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

static ssize_t mxlfs_splice_read(struct file *in, loff_t *ppos,
				 struct pipe_inode_info *pipe, size_t len,
				 unsigned int flags)
{
	ssize_t ok;

	/* A view's data is its sources' pages, which only read_iter reads. */
	if (READ_ONCE(MXLFS_I(file_inode(in))->view))
		return copy_splice_read(in, ppos, pipe, len, flags);
	ok = mxlfs_unpoisoned(file_inode(in), *ppos, len);
	if (ok < 0)
		return ok;
	mxlfs_place_read(file_inode(in), *ppos, ok);
	return filemap_splice_read(in, ppos, pipe, ok, flags);
}

static const struct file_operations mxlfs_file_operations = {
	.read_iter	= mxlfs_read_iter,
	.write_iter	= mxlfs_write_iter,
	.mmap_prepare	= mxlfs_mmap_prepare,
	.fsync		= noop_fsync,
	.splice_read	= mxlfs_splice_read,
	.splice_write	= iter_file_splice_write,
	.llseek		= generic_file_llseek,
	.get_unmapped_area = thp_get_unmapped_area,
	.fallocate	= mxlfs_fallocate,
	.release	= mxlfs_release,
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
	mi->fold_start = 0;
	mi->fold_end = 0;
	mi->view = NULL;
	mi->node_pages = NULL;
	mi->pref = NUMA_NO_NODE;
	atomic_set(&mi->holds, 0);
	return &mi->vfs_inode;
}

static void mxlfs_free_inode(struct inode *inode)
{
	kfree(MXLFS_I(inode)->node_pages);
	kmem_cache_free(mxlfs_inode_cachep, MXLFS_I(inode));
}

static void mxlfs_evict_inode(struct inode *inode)
{
	struct mxlfs_inode *mi = MXLFS_I(inode);
	unsigned int nid;

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	mxlfs_settle(inode);
	for (nid = 0; mi->node_pages && nid < nr_node_ids; nid++)
		mxlfs_count(inode, nid, -atomic_long_read(&mi->node_pages[nid]));
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
	buf->f_bfree = buf->f_blocks > used ? buf->f_blocks - used : 0;
	buf->f_bavail = buf->f_bfree;
	return 0;
}

static int mxlfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(root->d_sb);

	if (sbi->mode != MXLFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", sbi->mode);
	if (sbi->huge == MXLFS_HUGE_ALWAYS)
		seq_puts(m, ",huge=always");
	if (sbi->numa_node != NUMA_NO_NODE)
		seq_printf(m, ",numa_node=%d", sbi->numa_node);
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

enum { Opt_mode, Opt_huge, Opt_numa_node, Opt_size, Opt_wait };

static const struct constant_table mxlfs_param_huge[] = {
	{ "within_size", MXLFS_HUGE_WITHIN_SIZE },
	{ "always", MXLFS_HUGE_ALWAYS },
	{}
};

static const struct fs_parameter_spec mxlfs_fs_parameters[] = {
	fsparam_u32oct("mode", Opt_mode),
	fsparam_enum("huge", Opt_huge, mxlfs_param_huge),
	fsparam_u32("numa_node", Opt_numa_node),
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
	sbi->seen |= BIT(opt);

	switch (opt) {
	case Opt_mode:
		sbi->mode = result.uint_32 & S_IALLUGO;
		break;
	case Opt_huge:
		sbi->huge = result.uint_32;
		break;
	case Opt_numa_node:
		/* Whether it has memory is a mount's question; see mxlfs_get_tree. */
		if (result.uint_32 >= nr_node_ids)
			return invalfc(fc, "numa_node=%u is no node", result.uint_32);
		sbi->numa_node = result.uint_32;
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
	sb->s_d_flags		|= DCACHE_DONTCACHE;
	sb->s_time_gran		= 1;

	if (sbi->wait_ms) {
		/* The mount's, like its superblock info: not the mounter's cgroup. */
		sbi->wanted_tables = kvmalloc(sizeof(*sbi->wanted_tables), GFP_KERNEL);
		if (!sbi->wanted_tables)
			return -ENOMEM;
		hash_init(sbi->wanted_tables->names);
		hash_init(sbi->wanted_tables->owners);
	}
	inode = mxlfs_get_inode(sb, NULL, S_IFDIR | sbi->mode);
	sb->s_root = d_make_root(inode);
	return sb->s_root ? 0 : -ENOMEM;
}

/*
 * A mount's numa_node= must have memory. A remount repeating it is not
 * asked again: the node may have lost its memory since, which the mount
 * already copes with by allocating elsewhere.
 */
static int mxlfs_get_tree(struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = fc->s_fs_info;

	if (sbi->numa_node != NUMA_NO_NODE && !mxlfs_node_valid(sbi->numa_node))
		return invalfc(fc, "numa_node=%d is no node with memory", sbi->numa_node);
	return get_tree_nodev(fc, mxlfs_fill_super);
}

static void mxlfs_free_sbi(struct mxlfs_sb_info *sbi)
{
	if (sbi) {
		kfree(sbi->node);
		kvfree(sbi->wanted_tables);
	}
	kfree(sbi);
}

static void mxlfs_free_fc(struct fs_context *fc)
{
	mxlfs_free_sbi(fc->s_fs_info);
}

/*
 * A remount may repeat the mount's options but change none of them: the
 * mount reads each without a lock, and the root's mode and the structures
 * for wait= are made when it is filled. Flags such as ro are the VFS's.
 */
static int mxlfs_reconfigure(struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = MXLFS_SB(fc->root->d_sb);
	struct mxlfs_sb_info *ctx = fc->s_fs_info;
	static const char *const names[] = {
		[Opt_mode] = "mode", [Opt_huge] = "huge", [Opt_numa_node] = "numa_node",
		[Opt_size] = "size", [Opt_wait] = "wait",
	};
	unsigned int changed = 0;

	if (ctx->mode != sbi->mode)
		changed |= BIT(Opt_mode);
	if (ctx->huge != sbi->huge)
		changed |= BIT(Opt_huge);
	if (ctx->numa_node != sbi->numa_node)
		changed |= BIT(Opt_numa_node);
	if (ctx->limit != sbi->limit)
		changed |= BIT(Opt_size);
	if (ctx->wait_ms != sbi->wait_ms)
		changed |= BIT(Opt_wait);
	changed &= ctx->seen;
	if (changed)
		return invalfc(fc, "%s= cannot change on remount", names[__ffs(changed)]);
	return 0;
}

static const struct fs_context_operations mxlfs_context_ops = {
	.free		= mxlfs_free_fc,
	.parse_param	= mxlfs_parse_param,
	.get_tree	= mxlfs_get_tree,
	.reconfigure	= mxlfs_reconfigure,
};

static int mxlfs_init_fs_context(struct fs_context *fc)
{
	struct mxlfs_sb_info *sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);

	if (!sbi)
		return -ENOMEM;
	sbi->node = kcalloc(nr_node_ids, sizeof(*sbi->node), GFP_KERNEL);
	if (!sbi->node) {
		kfree(sbi);
		return -ENOMEM;
	}
	sbi->mode = MXLFS_DEFAULT_MODE;
	sbi->numa_node = NUMA_NO_NODE;
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

	mxlfs_kill_pinned_super(sb);
	while (!list_empty(&sbi->lazy)) {
		struct mxlfs_lazy *l = list_first_entry(&sbi->lazy, struct mxlfs_lazy, node);

		list_del(&l->node);
		put_pid(l->tgid);
		kfree(l);
	}
	mxlfs_free_sbi(sbi);
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
MODULE_LICENSE("GPL");
MODULE_ALIAS_FS("mxlfs");
MODULE_VERSION("0.1.0-rc.8"); /* x-release-please-version */

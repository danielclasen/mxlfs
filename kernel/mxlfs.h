/* SPDX-License-Identifier: GPL-2.0-only */
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
#ifndef MXLFS_H
#define MXLFS_H

#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#include "compat.h"
#include "mxlfs_uapi.h"

#ifndef CONFIG_TRANSPARENT_HUGEPAGE
#error "mxlfs needs CONFIG_TRANSPARENT_HUGEPAGE for large page cache folios"
#endif
#ifdef CONFIG_HIGHMEM
#error "mxlfs copies into large folios through the direct map"
#endif

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

/*
 * The lookup tables of the wanted list, allocated for a mount with wait=:
 * struct mxlfs_wanted by directory and name, struct mxlfs_wanted_owner by
 * thread group.
 */
struct mxlfs_wanted_tables {
	DECLARE_HASHTABLE(names, MXLFS_WANTED_HASH_BITS);
	DECLARE_HASHTABLE(owners, MXLFS_WANTED_HASH_BITS);
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

/* super.c */
struct inode *mxlfs_get_inode(struct super_block *sb,
			      const struct inode *dir, umode_t mode);

/* namei.c */
extern const struct inode_operations mxlfs_dir_inode_operations;
extern const struct file_operations mxlfs_dir_operations;

/* wait.c */
extern const struct dentry_operations mxlfs_dentry_operations;
void mxlfs_wanted_appeared(struct inode *dir, const struct qstr *name);
long mxlfs_wanted_next(struct file *file, struct mxlfs_wanted_req __user *arg);
long mxlfs_wanted_fail(struct file *file, struct mxlfs_wanted_fail __user *arg);

/* alloc.c */
extern const struct inode_operations mxlfs_file_inode_operations;
void mxlfs_walk_nodes(struct inode *inode, u64 *pages);
long mxlfs_set_lazy(struct mxlfs_sb_info *sbi, u32 __user *arg);
bool mxlfs_node_valid(long nid);
int mxlfs_effective_node(struct inode *inode);
void mxlfs_place(struct inode *inode, pgoff_t index, pgoff_t end, unsigned int most);
bool mxlfs_folio_poisoned(struct folio *folio);
long mxlfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len);
long mxlfs_ioc_allocate(struct file *file);

/* file.c */
extern const struct address_space_operations mxlfs_aops;
extern const struct file_operations mxlfs_file_operations;

/* view.c */
extern const struct vm_operations_struct mxlfs_view_vm_ops;
void mxlfs_view_free(struct mxlfs_view *view);
long mxlfs_view_set_sources(struct file *file, void __user *arg);
long mxlfs_view_select_range(struct file *file, u32 source, u64 offset, u64 len);
ssize_t mxlfs_view_read(struct kiocb *iocb, struct iov_iter *to, struct mxlfs_view *view);
long mxlfs_view_get_selected(struct file *file, u32 __user *arg);
long mxlfs_view_replace_source(struct file *file, void __user *arg);

/* numa.c */
long mxlfs_set_node(struct file *file, struct mxlfs_node_pref __user *arg);
long mxlfs_hold(struct file *file, u32 __user *arg);
int mxlfs_release(struct inode *inode, struct file *file);
long mxlfs_node_pages(struct file *file, struct mxlfs_node_pages __user *arg);
long mxlfs_mount_nodes(struct file *file, struct mxlfs_mount_nodes __user *arg);

/* move.c */
long mxlfs_move(struct file *file, struct mxlfs_move __user *arg);

/* ioctl.c */
long mxlfs_dir_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
long mxlfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static inline struct mxlfs_inode *MXLFS_I(struct inode *inode)
{
	return container_of(inode, struct mxlfs_inode, vfs_inode);
}

static inline struct mxlfs_sb_info *MXLFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/*
 * Brings the superblock's allocated bytes in line with the inode's page
 * cache. Anything that adds or removes pages calls it afterwards; each call
 * settles whatever changed since the last, so racing callers converge.
 */
static inline void mxlfs_settle(struct inode *inode)
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
static inline void mxlfs_count(struct inode *inode, int nid, long nr)
{
	atomic_long_add(nr, &MXLFS_I(inode)->node_pages[nid]);
	atomic64_add(nr, &MXLFS_SB(inode->i_sb)->node[nid].pages);
}

static inline void mxlfs_count_folio(struct inode *inode, struct folio *folio, long sign)
{
	mxlfs_count(inode, folio_nid(folio), sign * folio_nr_pages(folio));
}

/*
 * Makes a locked folio mxlfs added to inode's page cache uptodate, dirty,
 * as nothing writes it back (see mxlfs_aops), and counted. Once per folio:
 * a second call counts it twice.
 */
static inline void mxlfs_folio_ready(struct inode *inode, struct folio *folio)
{
	VM_WARN_ON_ONCE_FOLIO(!folio_test_locked(folio), folio);
	folio_mark_uptodate(folio);
	folio_mark_dirty(folio);
	mxlfs_count_folio(inode, folio, 1);
}

/* Whether bytes more would exceed size=. Checked before allocating. */
static inline bool mxlfs_over_limit(struct mxlfs_sb_info *sbi, u64 bytes)
{
	return sbi->limit && atomic64_read(&sbi->allocated) + bytes > sbi->limit;
}

static inline bool mxlfs_is_flow_name(const struct qstr *name)
{
	static const char suffix[] = ".mxl-flow";
	const unsigned int n = sizeof(suffix) - 1;

	return name->len > n && name->name[0] != '.' &&
	       !memcmp(name->name + name->len - n, suffix, n);
}

/* Whether inode's holes are filled on a preferred node; see mxlfs_place. */
static inline bool mxlfs_placed(struct inode *inode)
{
	return READ_ONCE(MXLFS_I(inode)->pref) != NUMA_NO_NODE ||
	       MXLFS_SB(inode->i_sb)->numa_node != NUMA_NO_NODE;
}

/*
 * Whether one of the folio's pages [first, end) has a memory error, by
 * each page's own flag: memory_failure() sets PageHWPoison on the page
 * before it sets has_hwpoisoned on a large folio, so
 * folio_contain_hwpoisoned_page() can miss a tail page between the two.
 * Called with a reference on the folio.
 */
static inline bool mxlfs_pages_poisoned(struct folio *folio, long first, long end)
{
	long p;

	for (p = first; p < end; p++)
		if (PageHWPoison(folio_page(folio, p)))
			return true;
	return false;
}

static inline bool mxlfs_held_by_others(struct file *file)
{
	return atomic_read(&MXLFS_I(file_inode(file))->holds) > (file->private_data == MXLFS_HELD);
}

#endif /* MXLFS_H */

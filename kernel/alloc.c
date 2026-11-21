// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/falloc.h>
#include <linux/fsnotify.h>
#include <linux/highmem.h>
#include <linux/huge_mm.h>
#include <linux/pagemap.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#include "mxlfs.h"

/*
 * Adds the pages of inode's uptodate folios per node to pages[nr_node_ids].
 * Under the inode lock and the invalidate lock, which keep every path that
 * adds a folio or makes one uptodate out.
 */
void mxlfs_walk_nodes(struct inode *inode, u64 *pages)
{
	struct folio_batch fbatch;
	pgoff_t next = 0;
	unsigned int i;

	lockdep_assert_held_write(&inode->i_rwsem);
	lockdep_assert_held_write(&inode->i_mapping->invalidate_lock);
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

	lockdep_assert_held_write(&inode->i_rwsem);
	filemap_invalidate_lock(inode->i_mapping);
	mxlfs_walk_nodes(inode, pages);
	for (nid = 0; nid < nr_node_ids; nid++)
		mxlfs_count(inode, nid, pages[nid] -
			    atomic_long_read(&MXLFS_I(inode)->node_pages[nid]));
	filemap_invalidate_unlock(inode->i_mapping);
	kfree(pages);
}

/* Drops entries of thread groups that have exited; under lazy_lock. */
static void mxlfs_lazy_prune(struct mxlfs_sb_info *sbi)
{
	struct mxlfs_lazy *l, *n;

	lockdep_assert_held(&sbi->lazy_lock);
	list_for_each_entry_safe(l, n, &sbi->lazy, node) {
		if (pid_has_task(l->tgid, PIDTYPE_TGID))
			continue;
		list_del(&l->node);
		put_pid(l->tgid);
		kfree(l);
	}
}

/* Whether the calling thread group allocates lazily (MXLFS_IOC_LAZY_ALLOC). */
static bool mxlfs_caller_lazy(struct mxlfs_sb_info *sbi)
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
long mxlfs_set_lazy(struct mxlfs_sb_info *sbi, u32 __user *arg)
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
bool mxlfs_node_valid(long nid)
{
	return nid >= 0 && nid < nr_node_ids && node_state(nid, N_MEMORY);
}

/*
 * The node a folio of inode prefers: the file's own (MXLFS_IOC_SET_NODE),
 * else the mount's numa_node=, each only while it has memory; NUMA_NO_NODE
 * when neither applies.
 */
int mxlfs_effective_node(struct inode *inode)
{
	int nid = READ_ONCE(MXLFS_I(inode)->pref);

	if (nid == NUMA_NO_NODE || !node_state(nid, N_MEMORY))
		nid = MXLFS_SB(inode->i_sb)->numa_node;
	if (nid == NUMA_NO_NODE || !node_state(nid, N_MEMORY))
		return NUMA_NO_NODE;
	return nid;
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
void mxlfs_place(struct inode *inode, pgoff_t index, pgoff_t end, unsigned int most)
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
		mxlfs_folio_ready(inode, folio);
		folio_unlock(folio);
		index = folio_next_index(folio);
		folio_put(folio);
		mxlfs_settle(inode);
		cap = most;
		cond_resched();
	}
	filemap_invalidate_unlock_shared(mapping);
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
 * reference taken before it clears PG_lru, its own where compaction
 * isolates, the caller's where folio_isolate_lru() does, which requires
 * one; reclaim does not scan the unevictable list these folios are on.
 * Reading the count before and after PG_lru, and taking the larger,
 * therefore counts that reference whenever PG_lru reads clear. The lru_add
 * batch takes its reference in __folio_batch_add_and_move() and sets
 * PG_lru before dropping it in folio_batch_move_lru(); the isolations that
 * reach these folios are isolate_migratepages_block() and
 * folio_isolate_lru().
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

	lockdep_assert_held_write(&inode->i_rwsem);
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
			mxlfs_folio_ready(inode, large);
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
			WRITE_ONCE(mi->lazy, true);
		index = end;
	} else {
		WRITE_ONCE(mi->lazy, false);
	}
	while (index < end) {
		struct folio *folio;
		unsigned int within, order;

		/*
		 * Only a fatal signal ends the call, as in shmem_fallocate: an
		 * OOM kill takes effect without waiting for the whole range,
		 * and other signals cannot make a long call restart forever.
		 * What is allocated stays, as after ENOSPC.
		 */
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto out;
		}
		folio = filemap_get_folio(mapping, index);
		within = mxlfs_fit_order(index, end);
		order = mxlfs_alloc_order(sbi, index, end, size);
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
					mxlfs_folio_ready(inode, folio);
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
		mxlfs_folio_ready(inode, folio);
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

long mxlfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	return mxlfs_allocate(file, mode, offset, len,
			      mxlfs_caller_lazy(MXLFS_SB(file_inode(file)->i_sb)));
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
long mxlfs_ioc_allocate(struct file *file)
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

const struct inode_operations mxlfs_file_inode_operations = {
	.setattr	= mxlfs_setattr,
	.getattr	= simple_getattr,
};

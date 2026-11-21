// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mm.h>
#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/huge_mm.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/rmap.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/swap.h>

#include "mxlfs.h"

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
long mxlfs_move(struct file *file, struct mxlfs_move __user *arg)
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

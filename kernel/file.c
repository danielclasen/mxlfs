// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/huge_mm.h>
#include <linux/pagemap.h>
#include <linux/rcupdate_wait.h>
#include <linux/uio.h>

#include "mxlfs.h"

/* mxlfs_place for a read of len bytes at pos, up to EOF. */
static void mxlfs_place_read(struct inode *inode, loff_t pos, size_t len)
{
	loff_t size = i_size_read(inode);

	if (pos < size && len)
		mxlfs_place(inode, pos >> PAGE_SHIFT,
			    DIV_ROUND_UP(min_t(loff_t, size, pos + len), PAGE_SIZE), 0);
}

/*
 * Pages that appear outside fallocate: faults and reads on holes come
 * through read_folio, write() through write_begin. Both are checked
 * against size= first, so the limit can be overshot by at most one folio
 * per racing caller.
 */
static int mxlfs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;

	/*
	 * Zeroed either way: a folio refused here stays in the page cache, not
	 * uptodate, and the core reads it again when it is next used. What
	 * mxlfs uses directly, views and folds, takes it for a hole.
	 */
	folio_zero_range(folio, 0, folio_size(folio));
	if (mxlfs_over_limit(MXLFS_SB(inode->i_sb), 0)) {
		folio_unlock(folio);
		return -ENOSPC;
	}
	mxlfs_folio_ready(inode, folio);
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
const struct address_space_operations mxlfs_aops = {
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

static struct inode *mxlfs_view_source(struct mxlfs_view *view, pgoff_t index)
{
	return view->sources[view->sel[index]];
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
 * does, so it adds no atomics on folios other readers share. Without a
 * reference a folio can be split, freed or reused while the scan reads
 * it, and its entry rewritten: xas_store() writes the head slot before
 * the siblings that give the order, and a split stores the old head in
 * every new slot before the new folios replace it. The scan therefore
 * tests a folio's pages only when the folio's own order and index, read
 * through the pointer the entry held, match the entry's order and first
 * index, and its first page is aligned to that order, at most
 * MAX_PAGE_ORDER. compound_order() reads the order without a reference
 * and may return a wild value; the pages tested then still lie in one
 * buddy-aligned block, which one memory section holds whole. The result
 * counts only if the entry still holds the folio after the flags, the
 * check filemap_get_read_batch() makes, with the flag reads ordered
 * before it. Otherwise the entry is looked up again. A mismatch comes
 * from a store, split or free that has run or is running, and lasts only
 * while a writer holds the xarray lock, which waits for no reader;
 * filemap_get_read_batch() spins as long on a split's frozen reference
 * count.
 *
 * A result that counts rests on a folio the entry held at the lookup and
 * again after the flags, whose own order and index matched the entry in
 * between: a folio truncated but not yet freed passes the match and
 * fails the reload. The checks cannot exclude a folio freed and its
 * struct pages reused at the same order and index, back in the entry by
 * the reload. A page goes untested only if its folio was added or
 * removed after the walk passed the folio's first index, while the read
 * runs. Every
 * XA_CHECK_SCHED folios the scan restarts the walk after the entry and
 * lets the CPU go, as memfd_tag_pins() does, so a read of many GiB does
 * not hold the RCU read lock throughout. It tests every page's own flag:
 * memory_failure() sets PageHWPoison on the page before it sets
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
		index = round_down(xas.xa_index, 1UL << order);
		/* Unreferenced reads, as compaction makes of compound_order(). */
		if (unlikely(data_race(compound_order(&folio->page) != order ||
				       folio->index != index) ||
			     order > MAX_PAGE_ORDER ||
			     !IS_ALIGNED(folio_pfn(folio), 1UL << order))) {
			xas_reset(&xas);
			continue;
		}
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
			/* The entry ends at last, as the folio's order showed. */
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

const struct file_operations mxlfs_file_operations = {
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

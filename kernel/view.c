// SPDX-License-Identifier: GPL-2.0-only
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/huge_mm.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/slab.h>

#include "mxlfs.h"

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

const struct vm_operations_struct mxlfs_view_vm_ops = {
	.fault		= mxlfs_view_fault,
};

void mxlfs_view_free(struct mxlfs_view *view)
{
	unsigned int i;

	for (i = 0; i < view->count; i++) {
		atomic_dec(&MXLFS_I(view->sources[i])->view_users);
		iput(view->sources[i]);
	}
	kvfree(view->sel);
	kfree(view);
}

long mxlfs_view_set_sources(struct file *file, void __user *arg)
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

long mxlfs_view_select_range(struct file *file, u32 source, u64 offset, u64 len)
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
long mxlfs_view_replace_source(struct file *file, void __user *arg)
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

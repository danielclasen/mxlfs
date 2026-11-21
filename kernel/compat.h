/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kernel interfaces that changed between the releases mxlfs builds on,
 * 6.18 and newer. Each wrapper names the release that changed it.
 */
#ifndef MXLFS_COMPAT_H
#define MXLFS_COMPAT_H

#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)
#error "mxlfs needs Linux 6.18 or newer"
#endif

/* 7.1 renames linux/pagevec.h (struct folio_batch) to linux/folio_batch.h. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0)
#include <linux/folio_batch.h>
#else
#include <linux/pagevec.h>
#endif

/*
 * 6.19 marks the dentries a memory filesystem pins as persistent:
 * d_make_persistent() instantiates and pins, and simple_unlink(),
 * simple_rmdir() and kill_anon_super() drop that pin. Before, the
 * filesystem took the extra reference itself and kill_litter_super()
 * dropped what was left at unmount.
 */
static inline void mxlfs_d_pin(struct dentry *dentry, struct inode *inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	d_make_persistent(dentry, inode);
#else
	d_instantiate(dentry, inode);
	dget(dentry);
#endif
}

static inline void mxlfs_kill_pinned_super(struct super_block *sb)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	kill_anon_super(sb);
#else
	kill_litter_super(sb);
#endif
}

/*
 * 6.19 adds a memory policy argument; without one it allocates as the
 * two-argument form did. A macro, as filemap_alloc_folio() is one, so
 * allocation profiling attributes the folio to the caller.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
#define mxlfs_filemap_alloc_folio(gfp, order) filemap_alloc_folio(gfp, order, NULL)
#else
#define mxlfs_filemap_alloc_folio(gfp, order) filemap_alloc_folio(gfp, order)
#endif

/* 7.3 drops create()'s exclusive flag, which a memory filesystem ignores. */
#define MXLFS_CREATE_HAS_EXCL (LINUX_VERSION_CODE < KERNEL_VERSION(7, 3, 0))

/* 7.4 passes the mount's idmap to inode operations as const. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 4, 0)
#define MXLFS_IDMAP const struct mnt_idmap
#else
#define MXLFS_IDMAP struct mnt_idmap
#endif

/*
 * The flags of a mapping being made (struct vm_area_desc): vm_flags, a
 * vm_flags_t, in 6.18; in 6.19 a union of vm_flags with vma_flags, so
 * vm_flags still works; since 7.0 only vma_flags, a vma_flags_t bitmap
 * whose first word holds the VM_* flags. vma_flags_set_word and
 * vma_flags_clear_word are in 7.0.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
static inline void mxlfs_desc_set(struct vm_area_desc *desc, vm_flags_t flags)
{
	vma_flags_set_word(&desc->vma_flags, flags);
}

static inline void mxlfs_desc_clear(struct vm_area_desc *desc, vm_flags_t flags)
{
	vma_flags_clear_word(&desc->vma_flags, flags);
}

/* vma_flags_test_all is in 7.0; vma_desc_test only from 7.1. */
static inline bool mxlfs_desc_shared_write(const struct vm_area_desc *desc)
{
	return vma_flags_test_all(&desc->vma_flags, VMA_SHARED_BIT, VMA_WRITE_BIT);
}
#else
static inline void mxlfs_desc_set(struct vm_area_desc *desc, vm_flags_t flags)
{
	desc->vm_flags |= flags;
}

static inline void mxlfs_desc_clear(struct vm_area_desc *desc, vm_flags_t flags)
{
	desc->vm_flags &= ~flags;
}

static inline bool mxlfs_desc_shared_write(const struct vm_area_desc *desc)
{
	return (desc->vm_flags & (VM_SHARED | VM_WRITE)) == (VM_SHARED | VM_WRITE);
}
#endif

#endif /* MXLFS_COMPAT_H */

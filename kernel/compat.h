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
 * two-argument form did.
 */
static inline struct folio *mxlfs_filemap_alloc_folio(gfp_t gfp,
						      unsigned int order)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
	return filemap_alloc_folio(gfp, order, NULL);
#else
	return filemap_alloc_folio(gfp, order);
#endif
}

#endif /* MXLFS_COMPAT_H */

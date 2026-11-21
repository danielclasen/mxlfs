// SPDX-License-Identifier: GPL-2.0-only
#include <linux/capability.h>
#include <linux/slab.h>

#include "mxlfs.h"

long mxlfs_set_node(struct file *file, struct mxlfs_node_pref __user *arg)
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
long mxlfs_hold(struct file *file, u32 __user *arg)
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
int mxlfs_release(struct inode *inode, struct file *file)
{
	if (file->private_data == MXLFS_HELD) {
		atomic_dec(&MXLFS_I(inode)->holds);
		atomic64_dec(&MXLFS_SB(inode->i_sb)->holds);
	}
	return 0;
}

long mxlfs_node_pages(struct file *file, struct mxlfs_node_pages __user *arg)
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

long mxlfs_mount_nodes(struct file *file, struct mxlfs_mount_nodes __user *arg)
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

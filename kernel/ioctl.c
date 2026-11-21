// SPDX-License-Identifier: GPL-2.0-only
#include "mxlfs.h"

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

long mxlfs_dir_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
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

long mxlfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
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

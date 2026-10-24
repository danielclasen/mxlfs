/* mxlfs userspace interface. */
#ifndef MXLFS_UAPI_H
#define MXLFS_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MXLFS_MAGIC 0x6d786c66 /* "mxlf" */

#define MXLFS_VIEW_MAX_SOURCES 8

/*
 * Turns an empty regular file into a view: its pages are those of one of
 * `count` source files on the same mount, all of the view's size. The view
 * must have no pages and no mappings of its own. Selection starts at 0.
 */
struct mxlfs_view_sources {
	__u32 count;
	__u32 flags;
	__s32 fds[MXLFS_VIEW_MAX_SOURCES];
};

struct mxlfs_stats {
	__u64 bytes_allocated;
	__u64 bytes_limit;
	__u64 view_selects;	/* SELECT calls that changed the source */
	__u64 view_faults;	/* faults on views; each maps one folio */
};

#define MXLFS_IOC_VIEW_SET_SOURCES _IOW('x', 1, struct mxlfs_view_sources)
#define MXLFS_IOC_VIEW_SELECT _IOW('x', 2, __u32)
#define MXLFS_IOC_VIEW_GET_SELECTED _IOR('x', 3, __u32)
#define MXLFS_IOC_STATS _IOR('x', 4, struct mxlfs_stats)

#endif

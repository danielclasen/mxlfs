/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
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

/*
 * A missing flow some path walk is waiting for (wait= mount option). On a
 * directory fd, with CAP_SYS_ADMIN: MXLFS_IOC_WANTED_NEXT returns the next
 * flow not yet reported, waiting up to timeout_ms; ETIMEDOUT if none.
 * Creating the flow wakes its waiters; MXLFS_IOC_WANTED_FAIL fails them
 * with the given positive errno instead.
 */
struct mxlfs_wanted_req {
	__u64 id;
	__u32 pid;		/* first waiter's tgid in the initial pid namespace */
	__u32 timeout_ms;	/* in */
	char path[4080];	/* from the filesystem root, e.g. /domain/<id>.mxl-flow */
};

struct mxlfs_wanted_fail {
	__u64 id;
	__s32 error;
	__u32 pad;
};

#define MXLFS_IOC_VIEW_SET_SOURCES _IOW('x', 1, struct mxlfs_view_sources)
#define MXLFS_IOC_VIEW_SELECT _IOW('x', 2, __u32)
#define MXLFS_IOC_VIEW_GET_SELECTED _IOR('x', 3, __u32)
#define MXLFS_IOC_STATS _IOR('x', 4, struct mxlfs_stats)
#define MXLFS_IOC_WANTED_NEXT _IOWR('x', 5, struct mxlfs_wanted_req)
#define MXLFS_IOC_WANTED_FAIL _IOW('x', 6, struct mxlfs_wanted_fail)

/*
 * With a non-zero argument, the calling thread group's fallocate calls on
 * this mount only set the file size; pages are allocated on first touch.
 * Zero turns it off. Needs CAP_SYS_ADMIN; up to 16 thread groups.
 */
#define MXLFS_IOC_LAZY_ALLOC _IOW('x', 7, __u32)

/*
 * On a file open for writing: allocates all of it in large folios, folding
 * pages already there in, whether or not the caller allocates lazily.
 */
#define MXLFS_IOC_ALLOCATE _IO('x', 8)

#endif

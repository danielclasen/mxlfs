/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* mxlfs userspace interface. */
#ifndef MXLFS_UAPI_H
#define MXLFS_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define MXLFS_MAGIC 0x6d786c66 /* "mxlf" */

#define MXLFS_VIEW_MAX_SOURCES 8

/*
 * Turns an empty regular file into a view: each of its pages is the page at
 * the same offset of one of `count` source files on the same mount, all of
 * the view's size, each fd open for reading (EBADF otherwise). The view must
 * have no pages and no mappings of its own. Every page starts out selecting
 * source 0. While a view uses a file, truncate and fallocate cannot change
 * its size (EBUSY).
 */
struct mxlfs_view_sources {
	__u32 count;
	__u32 flags;
	__s32 fds[MXLFS_VIEW_MAX_SOURCES];
};

/*
 * MXLFS_IOC_STATS takes the size of the caller's struct from the command
 * and copies that much of this one, or all of it if the caller's is larger,
 * leaving the rest as it was. Before view_replaces the struct was 32 bytes,
 * _IOR('x', 4, __u64[4]); callers built against that header still work.
 */
struct mxlfs_stats {
	__u64 bytes_allocated;
	__u64 bytes_limit;
	__u64 view_selects;	/* SELECT calls that changed the source */
	__u64 view_faults;	/* faults on views; each maps one folio */
	__u64 view_replaces;	/* REPLACE_SOURCE calls that changed a source */
	/* Folios that preferred a node (numa_node= or the file's) and got another. */
	__u64 numa_fallback_folios;
	__u64 numa_fallback_bytes;
	/* MXLFS_IOC_MOVE, since the mount; dry runs count only in dry_runs. */
	__u64 move_calls;
	__u64 move_calls_partial;	/* result MXLFS_MOVED_PARTIAL */
	__u64 move_calls_none;		/* blocks were off the node, none moved */
	__u64 move_blocks;		/* folios moved */
	__u64 move_bytes;
	__u64 move_ns;			/* sum of call durations */
	__u64 move_refused[8];		/* blocks left off the node, by MXLFS_REFUSE_* */
	/*
	 * Block windows, one per folio moved: from the last check of the
	 * folio for writes and its unmap until its replacement is unlocked,
	 * which is the longest a fault on it waits. Histogram
	 * buckets: <64 us, <128 us, <256 us, <512 us, <1 ms, <2 ms, <4 ms,
	 * <8 ms, >=8 ms.
	 */
	__u64 move_window_ns_sum;
	__u64 move_window_ns_max;
	__u64 move_window_hist[9];
	__u64 holds;			/* open files holding a file now (MXLFS_IOC_HOLD) */
	__u64 dry_runs;
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
	__u32 pid;		/* first waiter's tgid in the caller's pid namespace, or 0 */
	__u32 timeout_ms;	/* in */
	char path[4080];	/* from the filesystem root, e.g. /domain/<id>.mxl-flow */
};

struct mxlfs_wanted_fail {
	__u64 id;
	__s32 error;
	__u32 pad;
};

/*
 * Selects source for the pages of [offset, offset + length): offset must be
 * page aligned, the end is rounded up to a page and clipped to the view.
 */
struct mxlfs_view_range {
	__u32 source;
	__u32 flags;
	__u64 offset;
	__u64 length;
};

#define MXLFS_IOC_VIEW_SET_SOURCES _IOW('x', 1, struct mxlfs_view_sources)
#define MXLFS_IOC_VIEW_SELECT _IOW('x', 2, __u32)	/* the whole view */
#define MXLFS_IOC_VIEW_GET_SELECTED _IOR('x', 3, __u32)	/* of the first page */
#define MXLFS_IOC_STATS _IOR('x', 4, struct mxlfs_stats)
#define MXLFS_IOC_WANTED_NEXT _IOWR('x', 5, struct mxlfs_wanted_req)
#define MXLFS_IOC_WANTED_FAIL _IOW('x', 6, struct mxlfs_wanted_fail)

/*
 * With a non-zero argument, the calling thread group's fallocate calls on
 * this mount only set the file size; pages are allocated on first touch.
 * Zero turns it off. Needs no privilege.
 */
#define MXLFS_IOC_LAZY_ALLOC _IOW('x', 7, __u32)

/*
 * On a file open for writing: allocates all of it in large folios, folding
 * pages already there in, whether or not the caller allocates lazily.
 */
#define MXLFS_IOC_ALLOCATE _IO('x', 8)
#define MXLFS_IOC_VIEW_SELECT_RANGE _IOW('x', 9, struct mxlfs_view_range)

/*
 * On a view open for writing: makes the file at fd the view's source number
 * `source` in place of the one there, under the same conditions
 * MXLFS_IOC_VIEW_SET_SOURCES puts on a source. Mappings of the pages that
 * select it are zapped and refault onto the new file's pages; readers keep
 * their mappings. The selection stays as it is. The file already there
 * leaves the view unchanged.
 *
 * flags is checked before anything else, on any file: unknown bits fail
 * with EINVAL. With flags = ~0, EINVAL means the module has this ioctl and
 * ENOTTY that it does not. Then EINVAL if the file is not a view, EBADF if
 * it is not open for writing, EINVAL for a source number out of range,
 * EBADF if fd is not open for reading, and EINVAL if the file at fd cannot
 * be a source of the view.
 */
struct mxlfs_view_replace {
	__u32 source;
	__s32 fd;
	__u32 flags;
};

#define MXLFS_IOC_VIEW_REPLACE_SOURCE _IOW('x', 10, struct mxlfs_view_replace)

/*
 * NUMA placement of a file's pages. A folio of a regular file comes from
 * the file's preferred node if it has one with memory, else from the
 * mount's numa_node=, else from where the allocating task's memory policy
 * puts it. A preferred node falls back to the others when it has no free
 * memory, and the fallback is counted. Without these calls a file follows
 * numa_node= alone.
 */
#define MXLFS_NODE_NONE (-1)

/*
 * On a regular file that is not a view, with CAP_SYS_NICE: later
 * allocations of the file prefer node; MXLFS_NODE_NONE clears the
 * preference. Pages already there stay where they are. EINVAL for a node
 * without memory or flags other than 0, EBUSY while another open file
 * holds the file (MXLFS_IOC_HOLD).
 */
struct mxlfs_node_pref {
	__s32 node;
	__u32 flags;
};

#define MXLFS_IOC_SET_NODE _IOW('x', 11, struct mxlfs_node_pref)

/*
 * Where a file's pages are, on any open regular file, without privilege
 * and without taking a lock: pages_ptr receives the file's pages per node
 * for the first nr_nodes nodes, and nr_nodes is set to the number of node
 * ids. A page counts while it is in the page cache and uptodate. With
 * MXLFS_NODE_PAGES_RECOUNT (CAP_SYS_ADMIN) the pages are counted by
 * walking the file's page cache under its locks instead; that blocks the
 * file's writers and is meant for tests and diagnosis.
 */
struct mxlfs_node_pages {
	__u64 pages_ptr;	/* in: __u64[nr_nodes] */
	__u32 nr_nodes;		/* in: entries at pages_ptr; out: number of node ids */
	__u32 flags;		/* in: MXLFS_NODE_PAGES_* */
	__s32 pref;		/* out: the file's preferred node or MXLFS_NODE_NONE */
	__s32 effective;	/* out: the node allocations prefer, or MXLFS_NODE_NONE */
	__u32 holds;		/* out: open files holding the file */
	__u32 pad;
};

#define MXLFS_NODE_PAGES_RECOUNT (1U << 0)
#define MXLFS_IOC_NODE_PAGES _IOWR('x', 12, struct mxlfs_node_pages)

/*
 * On a regular file that is not a view, with CAP_SYS_NICE (none for a dry
 * run): moves every folio of the file that is not on node to node, one at
 * a time, keeping offsets and contents; mappings of a folio refault onto
 * its copy. A destination folio of the same size is reserved on node for
 * every folio first; if any reservation fails nothing moves (ENOMEM). A
 * folio is left where it is if it is pinned (long-term GUP, such as an
 * RDMA registration), mapped by a view, or holds references beyond its
 * mappings and the page cache; the first such folio sets reason and
 * refused_offset. Without MXLFS_MOVE_STRICT the call goes on with the
 * next folio and returns 0, result MXLFS_MOVED_PARTIAL or
 * MXLFS_MOVED_NONE. With it, every folio is checked for a pin or a view's
 * mapping before any moves, which zaps its mappings once more, and such a
 * folio fails the call with EBUSY; one refused later fails it with
 * EAGAIN, the folios moved so far staying moved. EBUSY with reason
 * MXLFS_REFUSE_HELD when another open file holds the file and a folio is
 * off node; EINTR on a fatal signal, between folios. Each folio is copied
 * before its window; it holds up a fault on it for that window only, from
 * the zap until the copy is in place, and a folio written to while it was
 * copied is tried again after the others, the last try copying it within the
 * window; the whole call holds the file's inode lock, so write(), fallocate
 * and truncation wait for it. The file's preferred node becomes node when
 * the result is MXLFS_MOVED_ALL and no other open file holds it.
 * MXLFS_MOVE_DRY_RUN changes nothing and takes no lock: it counts and
 * reports what a move would find before it starts, pins and holds, not a
 * view's mappings.
 */
struct mxlfs_move {
	__s32 node;		/* in */
	__u32 flags;		/* in: MXLFS_MOVE_* */
	__u32 result;		/* out: MXLFS_MOVED_* */
	__u32 reason;		/* out: MXLFS_REFUSE_* that stopped the call, else of the
				 * first folio left, or 0 */
	__u64 blocks;		/* out: folios of the file */
	__u64 blocks_on_node;	/* out: on node before the call */
	__u64 blocks_moved;	/* out */
	__u64 blocks_refused;	/* out: left off node */
	__u64 bytes_moved;	/* out */
	__u64 refused_offset;	/* out: byte offset of the folio reason belongs to */
	__u64 duration_ns;	/* out: the whole call */
	__u64 max_window_ns;	/* out: the longest block window, as in mxlfs_stats */
};

#define MXLFS_MOVE_DRY_RUN (1U << 0)
#define MXLFS_MOVE_STRICT (1U << 1)

enum {
	MXLFS_MOVED_ALL = 1,	/* every folio is on node now */
	MXLFS_MOVED_PARTIAL,
	MXLFS_MOVED_NONE,	/* folios were off node, none moved */
};

enum {
	MXLFS_REFUSE_PINNED = 1,	/* long-term pinned */
	MXLFS_REFUSE_VIEW,		/* mapped by a view */
	MXLFS_REFUSE_BUSY,		/* references beyond mappings and page cache */
	MXLFS_REFUSE_NOMEM,		/* no destination folio could be reserved */
	MXLFS_REFUSE_HELD,		/* another open file holds the file */
	MXLFS_REFUSE_SIGNAL,		/* a fatal signal stopped the call */
	MXLFS_REFUSE_MAX = 8
};

#define MXLFS_IOC_MOVE _IOWR('x', 13, struct mxlfs_move)

/*
 * 1 takes, 0 drops this open file's hold on its file, with CAP_SYS_NICE.
 * While another open file holds a file, MXLFS_IOC_MOVE and
 * MXLFS_IOC_SET_NODE through this one fail with EBUSY. A hold belongs to
 * the open file, so dup() and fork() share it, and it ends with the
 * file's last close, as when its process dies. EINVAL on a view.
 */
#define MXLFS_IOC_HOLD _IOW('x', 14, __u32)

/*
 * The mount's counters per NUMA node, on any fd of the mount, without
 * privilege: fills min(nr, number of node ids) records at stats_ptr and
 * sets nr to the number of node ids.
 */
struct mxlfs_node_stat {
	__u64 pages;		/* the mount's pages on this node now */
	__u64 fallback_folios;	/* folios that preferred this node and got another */
	__u64 fallback_pages;
	__u64 moved_in_pages;	/* by MXLFS_IOC_MOVE, since the mount */
	__u64 moved_out_pages;
};

struct mxlfs_mount_nodes {
	__u64 stats_ptr;	/* in: struct mxlfs_node_stat[nr] */
	__u32 nr;		/* in/out */
	__u32 flags;		/* 0 */
};

#define MXLFS_IOC_MOUNT_NODES _IOWR('x', 15, struct mxlfs_mount_nodes)

#endif

#ifndef HARNESS_H
#define HARNESS_H

#define _GNU_SOURCE
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/futex.h>
#include <linux/io_uring.h>
#include <linux/openat2.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/sendfile.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>
#include <mxl/time.h>

#include "../kernel/mxlfs_uapi.h"

/* libmxl's shared flow header: mxlFlowInfo followed by FlowState. */
struct flow_header {
	mxlFlowInfo info;
	uint64_t inode;
	uint32_t sync;
};
_Static_assert(sizeof(mxlFlowInfo) == 2048, "mxlFlowInfo layout changed");
#define MAX_MEMBERS (MXLFS_VIEW_MAX_SOURCES - 1)
/* libmxl puts a grain's header in the first 8 KiB of its file, before the payload. */
#define GRAIN_HEADER_SIZE 8192

enum { FMT_VIDEO, FMT_DATA, FMT_AUDIO };

#define MAX_NODES 64

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: %s: ", __func__, __LINE__, #cond); \
			fprintf(stderr, __VA_ARGS__);                          \
			fputc('\n', stderr);                                   \
			failed++;                                              \
		}                                                              \
	} while (0)

extern volatile sig_atomic_t stop;

/* util.c */
__attribute__((noreturn)) void die(const char *what);
void *zalloc(size_t n, size_t size);
unsigned long env(const char *name, unsigned long def);
uint64_t splitmix64(uint64_t x);
void sleep_ns(uint64_t ns);
void *map_file(const char *path, int flags, int prot, size_t *size, int *fdp);
char *read_all(const char *path);
void write_all(const char *path, const char *data);
int cmp_u64(const void *a, const void *b);
uint64_t now_ns(void);

/* flow.c */
mxlRational rate(void);
bool dropped(unsigned member, uint64_t index);
int format(void);
void flow_def(char *buf, size_t len, const char *id);
int parse_uuid(const char *s, uint8_t out[16]);
int cmd_placeholder(const char *domain, const char *id);
int cmd_materialize(const char *domain, const char *id);
int cmd_stats(const char *path);

/* redundancy.c */
int cmd_writer(const char *domain, const char *id, unsigned member);
int cmd_merge(const char *domain, const char *out_id, unsigned skew, int count, char **member_ids);
int cmd_reader(const char *domain, const char *id);
int cmd_peek(const char *domain, const char *id);

/* audio.c */
int cmd_awriter(const char *domain, const char *id, unsigned member);
int cmd_amerge(const char *domain, const char *out_id, unsigned skew, int count, char **member_ids,
	       struct flow_header **mh, size_t data_size);
int cmd_areader(mxlInstance inst, mxlFlowReader rd, const mxlFlowConfigInfo *cfg, const char *id);

/* wait.c */
int cmd_wanted(const char *dir, unsigned timeout_ms, int fail);
int cmd_noxdev(const char *dir, const char *path);
int cmd_wantcheck(const char *root, unsigned wait_ms);
int cmd_wantrace(const char *dir, unsigned rounds);

/* views.c */
int cmd_bench(const char *dir, unsigned readers, unsigned rounds);
int cmd_ranges(const char *dir, unsigned readers, unsigned rounds);
int marked_file(const char *path, size_t size, uint64_t gen, unsigned source);
int new_view(const char *dir, const char *name, size_t size, const int *fds, unsigned count);
int cmd_replace(const char *dir, unsigned readers, unsigned rounds);
int cmd_viewrace(const char *dir, unsigned rounds);
int cmd_writerace(const char *dir, unsigned rounds);
int cmd_stale(const char *dir);

/* lazy.c */
int cmd_foldhold(const char *dir);
int cmd_foldmem(const char *dir, unsigned rounds);
int cmd_fold(const char *dir, unsigned readers, unsigned rounds);
int cmd_foldrace(const char *dir, unsigned rounds);
int cmd_allocate(const char *path);
int cmd_lazyfile(const char *path, off_t size);
int cmd_touch(const char *path);

/* folios.c */
int cmd_shrink(const char *dir);
bool pmd_folio(int fd, off_t off);
bool folio_orders(const uint8_t *map, size_t size, char *out, size_t len);
size_t span_of(size_t size);
uint8_t *map_span(int fd, size_t size);
int cmd_tail(const char *dir);
int cmd_poison(const char *path, off_t off, const char *how, const char *via);

/* numa.c */
size_t off_node(int fd, size_t size, int node, size_t *resident);
void node_pages(int fd, unsigned flags, uint64_t *pages, struct mxlfs_node_pages *np);
bool counts_agree(int fd, uint64_t *pages);
bool all_on(int fd, int node, size_t size);
int set_node(int fd, int node);
void mount_nodes(int fd, struct mxlfs_node_stat *st);
void stats_of(int fd, struct mxlfs_stats *s);
uint64_t node_kb(int node, const char *key);
uint64_t vmstat(const char *key);
int cmd_nodepages(const char *path, const char *how);
int cmd_setnode(const char *path, int node);
int cmd_mountnodes(const char *path);
int cmd_numacount(const char *dir, unsigned steps);
int cmd_numa(const char *dir, int node);

/* move.c */
int move_file(int fd, int node, unsigned flags, struct mxlfs_move *m);
int cmd_move(const char *path, int node, const char *how);
int cmd_hold(const char *path);
int cmd_movebench(const char *dir, unsigned seconds);
int cmd_movepin(const char *dir);
int cmd_moveview(const char *dir);
int cmd_moveflags(const char *dir);
int cmd_movehold(const char *dir);
int cmd_moveorders(const char *dir);
int cmd_movenomem(const char *dir, const char *fill);

#endif

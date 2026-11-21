#include "harness.h"

/*
 * Counts the pages of fd (size bytes) in the page cache, and those of them
 * that move_pages(2) does not report on node. Maps only what is resident,
 * so the check allocates nothing.
 */
size_t off_node(int fd, size_t size, int node, size_t *resident)
{
	size_t pages = (size + 4095) / 4096, off = 0;
	uint8_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	unsigned char *in = zalloc(pages, 1);
	void **addr = zalloc(pages, sizeof(*addr));
	int *status = zalloc(pages, sizeof(*status));
	size_t n = 0;

	if (p == MAP_FAILED)
		die("off_node");
	if (mincore(p, size, in))
		die("mincore");
	for (size_t pg = 0; pg < pages; pg++)
		if (in[pg] & 1) {
			(void)*(volatile const uint8_t *)(p + pg * 4096);
			addr[n++] = p + pg * 4096;
		}
	if (n && syscall(SYS_move_pages, 0, n, addr, NULL, status, 0))
		die("move_pages");
	for (size_t i = 0; i < n; i++)
		if (status[i] != node)
			off++;
	*resident = n;
	munmap(p, size);
	free(in);
	free(addr);
	free(status);
	return off;
}

/* The file's pages per node into pages[MAX_NODES], from its counters or, with RECOUNT, a walk. */
void node_pages(int fd, unsigned flags, uint64_t *pages, struct mxlfs_node_pages *np)
{
	memset(pages, 0, MAX_NODES * sizeof(*pages));
	memset(np, 0, sizeof(*np));
	np->pages_ptr = (uintptr_t)pages;
	np->nr_nodes = MAX_NODES;
	np->flags = flags;
	if (ioctl(fd, MXLFS_IOC_NODE_PAGES, np))
		die("MXLFS_IOC_NODE_PAGES");
}

/* Whether fd's counters agree with a walk of its page cache; the counters go to pages. */
bool counts_agree(int fd, uint64_t *pages)
{
	uint64_t walked[MAX_NODES];
	struct mxlfs_node_pages np;

	node_pages(fd, 0, pages, &np);
	node_pages(fd, MXLFS_NODE_PAGES_RECOUNT, walked, &np);
	return !memcmp(pages, walked, sizeof(walked));
}

/* Whether all size bytes of fd count on node, by the counters and a walk. */
bool all_on(int fd, int node, size_t size)
{
	uint64_t pages[MAX_NODES], sum = 0;

	if (!counts_agree(fd, pages))
		return false;
	for (int n = 0; n < MAX_NODES; n++)
		sum += pages[n];
	return pages[node] == sum && sum * 4096 >= size;
}

int set_node(int fd, int node)
{
	struct mxlfs_node_pref p = { .node = node };

	return ioctl(fd, MXLFS_IOC_SET_NODE, &p) ? -errno : 0;
}

void mount_nodes(int fd, struct mxlfs_node_stat *st)
{
	struct mxlfs_mount_nodes req = { .stats_ptr = (uintptr_t)st, .nr = MAX_NODES };

	memset(st, 0, MAX_NODES * sizeof(*st));
	if (ioctl(fd, MXLFS_IOC_MOUNT_NODES, &req))
		die("MXLFS_IOC_MOUNT_NODES");
}

void stats_of(int fd, struct mxlfs_stats *s)
{
	if (ioctl(fd, MXLFS_IOC_STATS, s))
		die("MXLFS_IOC_STATS");
}

/*
 * A value of /sys/devices/system/node/nodeN/meminfo, in kB, with the
 * per-CPU differences folded in first (root), so it is exact.
 */
uint64_t node_kb(int node, const char *key)
{
	char path[64], line[256], want[64];
	bool found = false;
	uint64_t kb;
	FILE *f;

	if ((f = fopen("/proc/sys/vm/stat_refresh", "w"))) {
		fputs("1", f);
		fclose(f);
	}
	snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/meminfo", node);
	snprintf(want, sizeof(want), "Node %d %s: %%" SCNu64, node, key);
	if (!(f = fopen(path, "r")))
		die(path);
	while (!found && fgets(line, sizeof(line), f))
		found = sscanf(line, want, &kb) == 1;
	fclose(f);
	if (!found)
		errx(1, "%s has no %s", path, key);
	return kb;
}

uint64_t vmstat(const char *key)
{
	char line[256], name[128];
	uint64_t v, value;
	bool found = false;
	FILE *f = fopen("/proc/vmstat", "r");

	if (!f)
		die("/proc/vmstat");
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%127s %" SCNu64, name, &v) == 2 && !strcmp(name, key)) {
			value = v;
			found = true;
		}
	fclose(f);
	if (!found)
		errx(1, "/proc/vmstat has no %s", key);
	return value;
}

/* harness nodepages FILE [recount|pad]; pad tries a non-zero pad. */
int cmd_nodepages(const char *path, const char *how)
{
	uint64_t pages[MAX_NODES];
	struct mxlfs_node_pages np;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	bool recount = how && !strcmp(how, "recount");

	if (fd < 0)
		die(path);
	if (how && !strcmp(how, "pad")) {
		np = (struct mxlfs_node_pages){ .pages_ptr = (uintptr_t)pages, .nr_nodes = MAX_NODES, .pad = 1 };
		printf("nodepages pad rc=%s\n", ioctl(fd, MXLFS_IOC_NODE_PAGES, &np) ? strerror(errno) : "0");
		return 0;
	}
	node_pages(fd, recount ? MXLFS_NODE_PAGES_RECOUNT : 0, pages, &np);
	printf("nodepages pref=%d effective=%d holds=%u", np.pref, np.effective, np.holds);
	for (unsigned n = 0; n < np.nr_nodes && n < MAX_NODES; n++)
		printf(" node%u=%" PRIu64, n, pages[n]);
	printf("\n");
	return 0;
}

/* harness setnode FILE NODE */
int cmd_setnode(const char *path, int node)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), rc;

	if (fd < 0)
		die(path);
	rc = set_node(fd, node);
	printf("setnode rc=%s\n", rc ? strerror(-rc) : "0");
	return rc ? 1 : 0;
}

/* harness mountnodes PATH */
int cmd_mountnodes(const char *path)
{
	struct mxlfs_node_stat st[MAX_NODES];
	struct mxlfs_mount_nodes req = { .stats_ptr = (uintptr_t)st, .nr = MAX_NODES };
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0 || ioctl(fd, MXLFS_IOC_MOUNT_NODES, &req))
		die(path);
	for (unsigned n = 0; n < req.nr && n < MAX_NODES; n++)
		printf("mountnodes node=%u pages=%llu fallback_folios=%llu fallback_pages=%llu moved_in=%llu"
		       " moved_out=%llu\n", n, (unsigned long long)st[n].pages,
		       (unsigned long long)st[n].fallback_folios, (unsigned long long)st[n].fallback_pages,
		       (unsigned long long)st[n].moved_in_pages, (unsigned long long)st[n].moved_out_pages);
	return 0;
}

/*
 * The counters stay exact through a random sequence of everything that
 * adds, moves or drops pages, on a mount of its own (DIR, size=0) that
 * nothing else uses: create, fallocate, a lazily sized file touched
 * through a mapping, its allocation (a fold), write(), read() over holes,
 * shrinking, MOVE and unlink. After each step each file's counters equal
 * a walk of its page cache, the mount's pages per node equal the files'
 * sum and bytes_allocated, and each node's FilePages moved by what the
 * mount's pages on it did.
 */
int cmd_numacount(const char *dir, unsigned steps)
{
	enum { FILES = 4 };
	static const char *const op_name[] = {
		"create", "fallocate", "lazy-touch", "fold", "write", "read-holes", "shrink", "move", "unlink",
	};
	const unsigned nops = sizeof(op_name) / sizeof(op_name[0]);
	int fds[FILES], ops[sizeof(op_name) / sizeof(op_name[0])] = { 0 };
	struct mxlfs_node_stat st[MAX_NODES];
	uint64_t seed = env("SEED", 1), fp0[2];
	int64_t mount0[2];
	unsigned failed = 0, slack = env("FILEPAGES_SLACK_KB", 0);
	char path[PATH_MAX], buf[65536];
	int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (dirfd < 0)
		die(dir);
	/*
	 * Brings every page of this program into the page cache now, so no
	 * code path run for the first time later adds to a node's FilePages.
	 */
	if (mlockall(MCL_CURRENT) || munlockall())
		die("mlockall");
	memset(buf, 0x6e, sizeof(buf));
	for (int f = 0; f < FILES; f++)
		fds[f] = -1;
	mount_nodes(dirfd, st);
	for (int n = 0; n < 2; n++) {
		fp0[n] = node_kb(n, "FilePages");
		mount0[n] = st[n].pages;
	}
	for (unsigned step = 0; step < steps; step++) {
		uint64_t r = seed = splitmix64(seed);
		int f = r % FILES, op = (r >> 8) % nops, node = (r >> 16) & 1;
		size_t size = ((r >> 20) % 2048 + 1) * 4096, off = ((r >> 32) % 2048) * 4096;
		struct mxlfs_move m;
		struct stat sb;

		snprintf(path, sizeof(path), "%s/count%d", dir, f);
		if (fds[f] < 0 && op != 0)
			op = 0;
		switch (op) {
		case 0:	/* create, or keep the one there */
			if (fds[f] < 0 && (fds[f] = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644)) < 0)
				die(path);
			break;
		case 1:
			if (posix_fallocate(fds[f], 0, size))
				die("fallocate");
			break;
		case 2: {	/* a lazily sized file touched in a few pages */
			if (fstat(fds[f], &sb))
				die("fstat");
			if (!sb.st_size) {
				lazy_size(fds[f], size);
				sb.st_size = size;
			}
			uint8_t *p = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[f], 0);

			if (p == MAP_FAILED)
				die("mmap");
			for (int t = 0; t < 8; t++)
				p[(off + t * 12345 * 4096ULL) % sb.st_size / 4096 * 4096] = 1;
			munmap(p, sb.st_size);
			break;
		}
		case 3:
			if (ioctl(fds[f], MXLFS_IOC_ALLOCATE))
				die("MXLFS_IOC_ALLOCATE");
			break;
		case 4:
			if (pwrite(fds[f], buf, sizeof(buf), off) != sizeof(buf))
				die("pwrite");
			break;
		case 5:	/* past what is there, then read over the holes */
			if (fstat(fds[f], &sb) || ftruncate(fds[f], sb.st_size + size) ||
			    pread(fds[f], buf, sizeof(buf), sb.st_size) < 0)
				die("read holes");
			break;
		case 6:
			if (fstat(fds[f], &sb) || ftruncate(fds[f], sb.st_size / 3 / 4096 * 4096 + 1234))
				die("shrink");
			break;
		case 7:
			if (move_file(fds[f], node, 0, &m))
				die("MXLFS_IOC_MOVE");
			break;
		case 8:
			close(fds[f]);
			fds[f] = -1;
			unlink(path);
			break;
		}
		ops[op]++;

		uint64_t pages[MAX_NODES], files[2] = { 0 }, total = 0;
		struct mxlfs_stats s;

		for (int g = 0; g < FILES; g++) {
			if (fds[g] < 0)
				continue;
			if (!counts_agree(fds[g], pages)) {
				CHECK(false, "step %u (%s): file %d's counters disagree with a walk", step, op_name[op], g);
				break;
			}
			files[0] += pages[0];
			files[1] += pages[1];
		}
		mount_nodes(dirfd, st);
		stats_of(dirfd, &s);
		for (int n = 0; n < MAX_NODES; n++)
			total += st[n].pages;
		CHECK(st[0].pages == files[0] && st[1].pages == files[1] && total * 4096 == s.bytes_allocated,
		      "step %u (%s): mount %llu+%llu, files %" PRIu64 "+%" PRIu64 ", allocated %llu", step, op_name[op],
		      (unsigned long long)st[0].pages, (unsigned long long)st[1].pages, files[0], files[1],
		      (unsigned long long)s.bytes_allocated);
		for (int n = 0; n < 2; n++) {
			int64_t d_sys = (int64_t)node_kb(n, "FilePages") - (int64_t)fp0[n];
			int64_t d_mount = ((int64_t)st[n].pages - mount0[n]) * 4;

			CHECK(llabs(d_sys - d_mount) <= slack, "step %u (%s): node %d FilePages moved %lld kB, the mount %lld",
			      step, op_name[op], n, (long long)d_sys, (long long)d_mount);
		}
		if (failed > 10)
			break;
	}
	for (int f = 0; f < FILES; f++)
		if (fds[f] >= 0) {
			close(fds[f]);
			snprintf(path, sizeof(path), "%s/count%d", dir, f);
			unlink(path);
		}
	mount_nodes(dirfd, st);
	CHECK(!st[0].pages && !st[1].pages, "pages left after every file is gone");
	printf("numacount steps=%u", steps);
	for (unsigned o = 0; o < nops; o++)
		printf(" %s=%d", op_name[o], ops[o]);
	printf(" failed=%u\n", failed);
	return failed ? 3 : 0;
}

int cmd_numa(const char *dir, int node)
{
	enum {
		FALLOCATE, HEADER, ALLOCATE, FAULT_HUGE, FAULT_GAP, FAULT_LAZY, WRITE, READ,
		SENDFILE, READ_NOWAIT, WILLNEED, NCASES
	};
	static const char *const name[NCASES] = {
		"fallocate", "lazy-header", "allocate", "fault-huge", "fault-gap", "fault-lazy",
		"write", "read", "sendfile", "read-nowait", "willneed",
	};
	const size_t grain = 5537792, small = 1 << 20, gap = 4 << 20;
	const bool placed = getenv("PLACED") && !strcmp(getenv("PLACED"), "1");
	char path[PATH_MAX], buf[65536], orders[4096];
	unsigned bad = 0;
	int fds[NCASES];

	memset(buf, 0x5a, sizeof(buf));
	for (int c = 0; c < NCASES; c++) {
		snprintf(path, sizeof(path), "%s/numa-%s", dir, name[c]);
		unlink(path);
		fds[c] = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (fds[c] < 0)
			die(path);
		if (getenv("PREF") && set_node(fds[c], atoi(getenv("PREF"))))
			die("MXLFS_IOC_SET_NODE");
		if (getenv("PREF_CLEAR") && set_node(fds[c], MXLFS_NODE_NONE))
			die("MXLFS_IOC_SET_NODE");
	}
	if (posix_fallocate(fds[FALLOCATE], 0, grain))
		die("posix_fallocate");

	/* As libmxl creates a placeholder and the agent materializes it. */
	lazy_size(fds[HEADER], grain);
	lazy_size(fds[ALLOCATE], grain);
	if (pwrite(fds[HEADER], buf, 8192, 0) != 8192 ||
	    pwrite(fds[ALLOCATE], buf, 8192, 0) != 8192)
		die("header");
	if (ioctl(fds[ALLOCATE], MXLFS_IOC_ALLOCATE))
		die("MXLFS_IOC_ALLOCATE");

	if (ftruncate(fds[FAULT_HUGE], grain))
		die("ftruncate");
	{
		uint8_t *p = mmap(NULL, grain, PROT_READ | PROT_WRITE, MAP_SHARED, fds[FAULT_HUGE], 0);

		if (p == MAP_FAILED)
			die("mmap");
		for (size_t off = 0; off < grain; off += 4096)
			p[off] = 1;
		/* The folios readahead takes, with or without numa_node=. */
		if (!folio_orders(p, grain, orders, sizeof(orders)) || strcmp(orders, "9,9,8,6,3")) {
			fprintf(stderr, "numa fault-huge: folios %s, want 9,9,8,6,3\n", orders);
			bad++;
		}
		munmap(p, grain);
	}

	/*
	 * A page already in the span of a VM_HUGEPAGE fault, as a racing
	 * fault leaves one: with numa_node= the folios around it are as
	 * large as fit beside it, and the next PMD span is one folio.
	 */
	if (ftruncate(fds[FAULT_GAP], gap) || pwrite(fds[FAULT_GAP], buf, 4096, 5 * 4096) != 4096)
		die("fault-gap");
	{
		uint8_t *p = mmap(NULL, gap, PROT_READ | PROT_WRITE, MAP_SHARED, fds[FAULT_GAP], 0);
		const char *want = "2,0,0,1,3,4,5,6,7,8,9";

		if (p == MAP_FAILED)
			die("mmap");
		for (size_t off = 0; off < gap; off += 4096)
			p[off] = 1;
		if (placed && (!folio_orders(p, gap, orders, sizeof(orders)) || strcmp(orders, want))) {
			fprintf(stderr, "numa fault-gap: folios %s, want %s\n", orders, want);
			bad++;
		}
		munmap(p, gap);
	}

	/* Lazily sized, so its mapping faults single pages. */
	lazy_size(fds[FAULT_LAZY], grain);
	{
		uint8_t *p = mmap(NULL, grain, PROT_READ | PROT_WRITE, MAP_SHARED, fds[FAULT_LAZY], 0);

		if (p == MAP_FAILED)
			die("mmap");
		p[0] = p[4096 * 100] = p[grain - 1] = 1;
		munmap(p, grain);
	}

	if (ftruncate(fds[WRITE], small) || pwrite(fds[WRITE], buf, sizeof(buf), 131072) != sizeof(buf))
		die("write");
	if (ftruncate(fds[READ], small) || pread(fds[READ], buf, sizeof(buf), 0) != sizeof(buf))
		die("read");
	{
		int null = open("/dev/null", O_WRONLY | O_CLOEXEC);
		off_t off = 262144;

		if (null < 0 || ftruncate(fds[SENDFILE], small) ||
		    sendfile(null, fds[SENDFILE], &off, sizeof(buf)) != sizeof(buf))
			die("sendfile");
		close(null);
	}

	/*
	 * Neither a read that must not block nor WILLNEED advice allocates
	 * a page of a hole, so nothing reaches the core unplaced.
	 */
	{
		struct iovec iov = { buf, sizeof(buf) };
		uint8_t *p;

		if (ftruncate(fds[READ_NOWAIT], small) || ftruncate(fds[WILLNEED], small))
			die("ftruncate");
		/* mxlfs files have no FMODE_NOWAIT. */
		if (preadv2(fds[READ_NOWAIT], &iov, 1, 0, RWF_NOWAIT) != -1 || errno != EOPNOTSUPP) {
			fprintf(stderr, "numa read-nowait: RWF_NOWAIT was not refused with EOPNOTSUPP\n");
			bad++;
		}
		if (posix_fadvise(fds[WILLNEED], 0, small, POSIX_FADV_WILLNEED))
			die("posix_fadvise");
		p = mmap(NULL, small, PROT_READ, MAP_SHARED, fds[WILLNEED], 0);
		if (p == MAP_FAILED || madvise(p, small, MADV_WILLNEED))
			die("madvise");
		munmap(p, small);
	}

	for (int c = 0; c < NCASES; c++) {
		struct stat st;
		size_t resident, off;

		if (fstat(fds[c], &st))
			die("fstat");
		off = off_node(fds[c], st.st_size, node, &resident);
		/* Each case must have made pages, the header exactly its two. */
		if (off || (c == HEADER ? resident != 2 :
			    c == READ_NOWAIT || c == WILLNEED ? resident != 0 : !resident))
			bad++;
		printf("numa %s: pages=%zu off_node=%zu\n", name[c], resident, off);
		close(fds[c]);
		snprintf(path, sizeof(path), "%s/numa-%s", dir, name[c]);
		unlink(path);
	}
	printf("numa node=%d cases=%d failed=%u\n", node, NCASES, bad);
	return bad ? 3 : 0;
}

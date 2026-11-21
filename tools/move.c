#include "harness.h"

/* MXLFS_REFUSE_*, then "?" for a value this harness does not know. */
static const char *const refuse_name[MXLFS_REFUSE_MAX + 1] = {
	"none", "pinned", "view", "busy", "nomem", "held", "signal", "poisoned", "?",
};

int move_file(int fd, int node, unsigned flags, struct mxlfs_move *m)
{
	memset(m, 0, sizeof(*m));
	m->node = node;
	m->flags = flags;
	return ioctl(fd, MXLFS_IOC_MOVE, m) ? -errno : 0;
}

static int hold(int fd, uint32_t on)
{
	return ioctl(fd, MXLFS_IOC_HOLD, &on) ? -errno : 0;
}

/* The kB of FilePmdMapped in the calling process's mappings. */
static uint64_t pmd_mapped_kb(void)
{
	char line[256];
	uint64_t kb, sum = 0;
	FILE *f = fopen("/proc/self/smaps", "r");

	if (!f)
		die("smaps");
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "FilePmdMapped: %" SCNu64 " kB", &kb) == 1)
			sum += kb;
	fclose(f);
	return sum;
}

static void pin_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu % sysconf(_SC_NPROCESSORS_ONLN), &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		die("sched_setaffinity");
}

static void unpin_cpu(void)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	for (long c = 0; c < sysconf(_SC_NPROCESSORS_ONLN); c++)
		CPU_SET(c, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		die("sched_setaffinity");
}

/* A new file of size bytes in dir, allocated, every page carrying tag ^ its offset. */
static int tagged_file(const char *dir, const char *name, size_t size, uint64_t tag)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0 || posix_fallocate(fd, 0, size))
		die(path);
	for (size_t off = 0; off < size; off += 4096) {
		uint64_t v = tag ^ off;

		if (pwrite(fd, &v, sizeof(v), off) != sizeof(v))
			die("pwrite");
	}
	return fd;
}

/* Pages of fd whose tag read() does not return. */
static size_t tag_errors(int fd, size_t size, uint64_t tag)
{
	size_t bad = 0;

	for (size_t off = 0; off < size; off += 4096) {
		uint64_t v;

		if (pread(fd, &v, sizeof(v), off) != sizeof(v) || v != (tag ^ off))
			bad++;
	}
	return bad;
}

static void print_move(const char *what, int rc, const struct mxlfs_move *m)
{
	printf("%s rc=%s result=%u reason=%s blocks=%llu on_node=%llu moved=%llu refused=%llu bytes=%llu"
	       " refused_offset=%llu duration_us=%llu max_window_us=%llu\n", what, rc ? strerror(-rc) : "0",
	       m->result, refuse_name[m->reason < MXLFS_REFUSE_MAX ? m->reason : MXLFS_REFUSE_MAX],
	       (unsigned long long)m->blocks, (unsigned long long)m->blocks_on_node,
	       (unsigned long long)m->blocks_moved, (unsigned long long)m->blocks_refused,
	       (unsigned long long)m->bytes_moved, (unsigned long long)m->refused_offset,
	       (unsigned long long)m->duration_ns / 1000, (unsigned long long)m->max_window_ns / 1000);
}

/* harness move FILE NODE [dry|strict] */
int cmd_move(const char *path, int node, const char *how)
{
	unsigned flags = !how ? 0 : !strcmp(how, "dry") ? MXLFS_MOVE_DRY_RUN :
			 !strcmp(how, "strict") ? MXLFS_MOVE_STRICT : ~0u;
	struct mxlfs_move m;
	int fd = open(path, O_RDONLY | O_CLOEXEC), rc;

	if (fd < 0)
		die(path);
	rc = move_file(fd, node, flags, &m);
	print_move("move", rc, &m);
	return rc ? 1 : 0;
}

/* harness hold FILE: holds FILE until SIGTERM. */
int cmd_hold(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0 || hold(fd, 1))
		die(path);
	printf("held %s\n", path);
	while (!stop)
		pause();
	return 0;
}

/* Waits for a byte on fd. */
static void await(int fd)
{
	char c;

	if (read(fd, &c, 1) != 1)
		die("pipe");
}

/*
 * Ring files moved between nodes 0 and 1 under one writer and three
 * readers, as a mirror's ring would be under its flow's writer and local
 * readers. FILES files of 6 MiB (three 2 MiB folios with huge=always) are
 * a ring of grains; the writer on CPU 0 fills one grain every 20 ms,
 * stamping every page with (grain, page, sequence) and then publishing the
 * grain, and readers on CPUs 1-3 check every page of every grain. Each
 * accessor times its pass over every 2 MiB block, which a move makes wait
 * for the block's window. The parent moves every file to the other node
 * in turn until SECONDS - 2, and after each pass checks that every page
 * counts on the target, by the counters and by a walk. With MOVES=0 it
 * does not move, for the accessors' times without moves; with SCRAPE=n a
 * child makes n MXLFS_IOC_NODE_PAGES calls meanwhile. Fails when a grain
 * is stale or torn, the readers miss more than twice the share of grains
 * a run without moves missed (MISSING_BASE_PERMILLE) plus 0.5%, a move
 * leaves a block after three calls more for a block refused BUSY, more
 * than 1% of the calls are such retries, the writer's mappings lose a
 * PMD or a THP is split.
 *
 * Times depend on the machine, so the one timing bound that always applies
 * is relative to this run: half of the kernel's block windows must end
 * within what copying 2 MiB from a folio on node 0 to one on node 1 takes
 * here in user space (see copy_2m_ns), or within the smallest bucket, 64 us.
 * The kernel copies a folio before its window, so the window of a folio not
 * written meanwhile is a zap and a page cache swap; a window that holds the
 * copy takes longer. The bounds of a strict local run apply only when set:
 * WINDOW_UNDER_1MS_PERMILLE, the share of windows under 1 ms, and
 * STALL_P99_US, the 99th percentile of either side's block times.
 */
#define MB_SIZE (6 << 20)
#define MB_PAGES (MB_SIZE / 4096)
#define MB_BLOCK_PAGES 512
#define MB_ROLES 4

struct mb_shared {
	uint64_t head;		/* last grain published, from 1 */
	uint32_t stop;
	uint32_t pad;
	uint64_t checked, stale, torn, missing;
	uint64_t pmd_kb_start, pmd_kb_end, scrapes, scrape_ns;
	uint32_t nsamples[MB_ROLES];
};

static void mb_sample(struct mb_shared *sh, uint32_t *samples, size_t max, int role, uint64_t ns)
{
	uint32_t i = sh->nsamples[role];

	if (i < max) {
		samples[role * max + i] = ns > UINT32_MAX ? UINT32_MAX : ns;
		sh->nsamples[role] = i + 1;
	}
}

static void mb_writer(struct mb_shared *sh, uint32_t *samples, size_t max, uint8_t **map, unsigned files)
{
	uint64_t next = now_ns(), stopping = 0;

	pin_cpu(0);
	for (uint64_t g = 1;; g++) {
		uint8_t *f = map[g % files];

		for (unsigned b = 0; b < MB_PAGES / MB_BLOCK_PAGES; b++) {
			uint64_t t = now_ns();

			for (unsigned pg = b * MB_BLOCK_PAGES; pg < (b + 1) * MB_BLOCK_PAGES; pg++) {
				volatile uint64_t *w = (volatile uint64_t *)(f + (size_t)pg * 4096);

				w[1] = pg;
				w[2] = g * MB_PAGES + pg;
				w[0] = g;
			}
			mb_sample(sh, samples, max, 0, now_ns() - t);
		}
		__atomic_store_n(&sh->head, g, __ATOMIC_RELEASE);
		/*
		 * The mappings once every slot was written, and again after a
		 * whole ring more once the moves have ended, which refaults
		 * every block the last moves zapped.
		 */
		if (g == files)
			sh->pmd_kb_start = pmd_mapped_kb();
		if (!stopping && __atomic_load_n(&sh->stop, __ATOMIC_ACQUIRE) == 1)
			stopping = g;
		if (stopping && g >= stopping + files) {
			sh->pmd_kb_end = pmd_mapped_kb();
			__atomic_store_n(&sh->stop, 2, __ATOMIC_RELEASE);
			return;
		}
		next += 20000000;
		struct timespec ts = { .tv_sec = next / 1000000000, .tv_nsec = next % 1000000000 };

		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
	}
}

static void mb_reader(struct mb_shared *sh, uint32_t *samples, size_t max, uint8_t **map, unsigned files,
		      int role)
{
	uint64_t last;

	pin_cpu(role);
	while (!(last = __atomic_load_n(&sh->head, __ATOMIC_ACQUIRE)))
		usleep(1000);
	while (__atomic_load_n(&sh->stop, __ATOMIC_ACQUIRE) != 2) {
		uint64_t h = __atomic_load_n(&sh->head, __ATOMIC_ACQUIRE);

		if (h == last) {
			usleep(200);
			continue;
		}
		for (uint64_t g = last + 1; g <= h; g++) {
			const uint8_t *f = map[g % files];
			uint64_t stale = 0, torn = 0, newer = 0;

			/* The writer may already be refilling the slot. */
			if (h - g >= files - 2) {
				__atomic_fetch_add(&sh->missing, 1, __ATOMIC_RELAXED);
				continue;
			}
			for (unsigned b = 0; b < MB_PAGES / MB_BLOCK_PAGES; b++) {
				uint64_t t = now_ns();

				for (unsigned pg = b * MB_BLOCK_PAGES; pg < (b + 1) * MB_BLOCK_PAGES; pg++) {
					const volatile uint64_t *w = (const volatile uint64_t *)(f + (size_t)pg * 4096);
					uint64_t w0 = w[0], w1 = w[1], w2 = w[2];

					if (w0 < g)
						stale++;
					else if (w0 > g)
						newer++;
					else if (w1 != pg || w2 != g * MB_PAGES + pg)
						torn++;
				}
				mb_sample(sh, samples, max, role, now_ns() - t);
			}
			/* Overwritten while read: a slow reader, not lost data. */
			if (newer || __atomic_load_n(&sh->head, __ATOMIC_ACQUIRE) - g >= files - 1) {
				__atomic_fetch_add(&sh->missing, 1, __ATOMIC_RELAXED);
				continue;
			}
			__atomic_fetch_add(&sh->checked, 1, __ATOMIC_RELAXED);
			if (stale)
				__atomic_fetch_add(&sh->stale, 1, __ATOMIC_RELAXED);
			if (torn)
				__atomic_fetch_add(&sh->torn, 1, __ATOMIC_RELAXED);
		}
		last = h;
	}
}

/*
 * calls NODE_PAGES calls spread evenly over the moves, as an exporter's
 * scans would be, so they compete with the accessors for locks, if any,
 * rather than for CPU time.
 */
static void mb_scraper(struct mb_shared *sh, const int *fds, unsigned files, unsigned long calls,
		       uint64_t ns)
{
	uint64_t pages[MAX_NODES], t = now_ns(), busy = 0;
	struct mxlfs_node_pages np = { .pages_ptr = (uintptr_t)pages, .nr_nodes = MAX_NODES };
	unsigned long i;

	for (i = 0; i < calls && !__atomic_load_n(&sh->stop, __ATOMIC_ACQUIRE); i++) {
		if (i % 1000 == 0) {
			uint64_t due = t + ns / calls * i, now = now_ns();

			if (due > now)
				sleep_ns(due - now);
		}
		uint64_t c = now_ns();

		if (ioctl(fds[i % files], MXLFS_IOC_NODE_PAGES, &np))
			die("MXLFS_IOC_NODE_PAGES");
		busy += now_ns() - c;
	}
	sh->scrapes = i;
	sh->scrape_ns = busy;
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

static void mb_pct(const char *name, uint32_t *v, size_t n)
{
	qsort(v, n, sizeof(*v), cmp_u32);
	if (!n) {
		printf(" %s_block_us=none", name);
		return;
	}
	printf(" %s_block_us_p50=%.1f p99=%.1f max=%.1f", name, v[n / 2] / 1e3, v[n * 99 / 100] / 1e3,
	       v[n - 1] / 1e3);
}

/*
 * The median time of 64 user-space copies of 2 MiB from a 2 MiB folio on
 * node 0 to one on node 1, of two 64 MiB files of DIR with those preferred
 * nodes, each block copied twice and the files larger than a cache, so a
 * copy reads memory: what a move's copy of one block costs on this
 * machine, without its zap and page cache swap.
 */
static uint64_t copy_2m_ns(const char *dir)
{
	const size_t block = 2 << 20, size = 64 << 20;
	uint8_t *map[2];
	uint64_t ns[64];
	char path[PATH_MAX];

	for (int n = 0; n < 2; n++) {
		int fd;

		snprintf(path, sizeof(path), "%s/copy.%d", dir, n);
		unlink(path);
		fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (fd < 0 || set_node(fd, n) || posix_fallocate(fd, 0, size))
			die(path);
		map[n] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (map[n] == MAP_FAILED)
			die("mmap");
		memset(map[n], n, size);
		close(fd);
		unlink(path);
	}
	for (int i = 0; i < 64; i++) {
		size_t off = (size_t)(i % 32) * block;
		uint64_t t = now_ns();

		memcpy(map[1] + off, map[0] + off, block);
		ns[i] = now_ns() - t;
	}
	munmap(map[0], size);
	munmap(map[1], size);
	qsort(ns, 64, sizeof(ns[0]), cmp_u64);
	return ns[32];
}

int cmd_movebench(const char *dir, unsigned seconds)
{
	const unsigned files = env("FILES", 32), moves = env("MOVES", 1);
	const unsigned long scrape = env("SCRAPE", 0), p99_max_us = env("STALL_P99_US", 0),
			    under_1ms_permille = env("WINDOW_UNDER_1MS_PERMILLE", 0),
			    missing_base = env("MISSING_BASE_PERMILLE", 0);
	/* The upper ends of the kernel's window buckets, in us; the last is open. */
	static const uint64_t bucket_us[9] = { 64, 128, 256, 512, 1000, 2000, 4000, 8000, UINT64_MAX };
	uint64_t copy_ns = 0;
	const size_t max = (size_t)(seconds + 5) * 50 * (MB_PAGES / MB_BLOCK_PAGES) * 3;
	struct mxlfs_stats s0, s1;
	struct mb_shared *sh;
	uint32_t *samples;
	uint8_t **map = calloc(files, sizeof(*map));
	int *fds = calloc(files, sizeof(*fds));
	uint64_t passes = 0, calls = 0, busy_retries = 0, not_all = 0, mismatch = 0, split0, start;
	pid_t pids[MB_ROLES + 1];
	unsigned npids = 0, failed = 0, target = 0;
	char name[64];
	int status;

	sh = mmap(NULL, sizeof(*sh) + MB_ROLES * max * sizeof(*samples), PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED || !map || !fds)
		die("movebench");
	samples = (uint32_t *)(sh + 1);
	/* Allocated by the caller's task, which the test puts on node 0. */
	for (unsigned i = 0; i < files; i++) {
		snprintf(name, sizeof(name), "ring.%u", i);
		fds[i] = tagged_file(dir, name, MB_SIZE, 0);
		map[i] = mmap(NULL, MB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
		if (map[i] == MAP_FAILED)
			die("mmap");
		CHECK(all_on(fds[i], 0, MB_SIZE), "ring.%u is not on node 0 at the start", i);
	}
	/*
	 * A guest's memory is backed by its host only once touched, so the
	 * first copies to node 1 would time the host's page faults. Two
	 * passes without accessors touch as much of node 1 as the files take.
	 */
	for (int pass = 1; moves && pass >= 0; pass--)
		for (unsigned i = 0; i < files; i++) {
			struct mxlfs_move m;

			if (move_file(fds[i], pass, 0, &m))
				die("MXLFS_IOC_MOVE");
		}
	if (moves)
		copy_ns = copy_2m_ns(dir);
	split0 = vmstat("thp_split_page");
	stats_of(fds[0], &s0);

	for (int role = 0; role < MB_ROLES; role++) {
		if ((pids[npids++] = fork()))
			continue;
		if (role == 0)
			mb_writer(sh, samples, max, map, files);
		else
			mb_reader(sh, samples, max, map, files, role);
		_exit(0);
	}
	if (scrape && !(pids[npids++] = fork())) {
		unpin_cpu();
		mb_scraper(sh, fds, files, scrape, (seconds - 3) * 1000000000ULL);
		_exit(0);
	}
	/*
	 * The moves stay on the writer's CPU and take half its time, so the
	 * readers keep theirs: a reader that falls behind is one a move held
	 * up, not one the moving starved.
	 */
	start = now_ns();
	sleep(1);
	while (now_ns() - start < (seconds - 2) * 1000000000ULL && !stop) {
		uint64_t pass = now_ns();

		if (!moves) {
			usleep(100000);
			continue;
		}
		target ^= 1;
		for (unsigned i = 0; i < files; i++) {
			struct mxlfs_move m;
			int rc = move_file(fds[i], target, 0, &m);

			calls++;
			/*
			 * A fault that is still on its way to a block holds a
			 * reference the move waits out only so long; on a
			 * starved guest it can outlast that, and the caller
			 * moves the block on its next call, as a caller would.
			 */
			for (int r = 0; r < 3 && !rc && m.result == MXLFS_MOVED_PARTIAL &&
					m.reason == MXLFS_REFUSE_BUSY; r++) {
				busy_retries++;
				calls++;
				rc = move_file(fds[i], target, 0, &m);
			}
			if (rc || m.result != MXLFS_MOVED_ALL) {
				if (not_all++ < 5)
					print_move("movebench: not all moved:", rc, &m);
			}
		}
		passes++;
		for (unsigned i = 0; i < files; i++)
			if (!all_on(fds[i], target, MB_SIZE))
				mismatch++;
		sleep_ns(now_ns() - pass);
	}
	__atomic_store_n(&sh->stop, 1, __ATOMIC_RELEASE);
	for (unsigned i = 0; i < npids; i++)
		if (waitpid(pids[i], &status, 0) != pids[i] || !WIFEXITED(status) || WEXITSTATUS(status))
			failed++;
	stats_of(fds[0], &s1);

	/* Buckets 0-4 of the kernel's histogram are the windows under 1 ms. */
	uint64_t hist = 0, under_1ms = 0, seen = 0, median_us = 0, blocks = s1.move_blocks - s0.move_blocks;

	for (int b = 0; b < 9; b++) {
		hist += s1.move_window_hist[b] - s0.move_window_hist[b];
		if (b < 5)
			under_1ms += s1.move_window_hist[b] - s0.move_window_hist[b];
	}
	/* The upper end of the bucket that holds the median window. */
	for (int b = 0; b < 9 && hist && !median_us; b++) {
		seen += s1.move_window_hist[b] - s0.move_window_hist[b];
		if (seen * 2 >= hist)
			median_us = bucket_us[b];
	}
	printf("movebench seconds=%u files=%u moves=%u passes=%" PRIu64 " calls=%" PRIu64 " busy_retries=%" PRIu64
	       " not_all=%" PRIu64
	       " node_mismatch=%" PRIu64 " grains=%" PRIu64 " checked=%" PRIu64 " stale=%" PRIu64
	       " torn=%" PRIu64 " missing=%" PRIu64 " pmd_kb_start=%" PRIu64 " pmd_kb_end=%" PRIu64
	       " thp_split=%" PRIu64, seconds, files, moves, passes, calls, busy_retries, not_all, mismatch, sh->head,
	       sh->checked, sh->stale, sh->torn, sh->missing, sh->pmd_kb_start, sh->pmd_kb_end,
	       vmstat("thp_split_page") - split0);
	mb_pct("writer", samples, sh->nsamples[0]);
	/* The readers' samples, packed after each other. */
	size_t nr = 0;

	for (int role = 1; role < MB_ROLES; role++) {
		memmove(samples + max + nr, samples + role * max, sh->nsamples[role] * sizeof(*samples));
		nr += sh->nsamples[role];
	}
	uint32_t w99 = sh->nsamples[0] ? samples[sh->nsamples[0] * 99 / 100] : 0;

	mb_pct("reader", samples + max, nr);
	uint32_t r99 = nr ? samples[max + nr * 99 / 100] : 0;

	printf(" copy_2m_us_p50=%.1f window_us_p50_under=%s%" PRIu64 " kernel_blocks=%" PRIu64
	       " windows_under_1ms_permille=%" PRIu64 " window_us_max=%.1f avg=%.1f hist=", copy_ns / 1e3,
	       median_us == UINT64_MAX ? "inf" : "", median_us == UINT64_MAX ? 0 : median_us, blocks,
	       hist ? under_1ms * 1000 / hist : 0,
	       s1.move_window_ns_max / 1e3, blocks ? (s1.move_window_ns_sum - s0.move_window_ns_sum) / 1e3 / blocks : 0);
	for (int b = 0; b < 9; b++)
		printf("%s%llu", b ? "," : "", (unsigned long long)(s1.move_window_hist[b] - s0.move_window_hist[b]));
	if (scrape)
		printf(" scrapes=%" PRIu64 " scrape_ns_per_call=%.0f", sh->scrapes,
		       sh->scrapes ? (double)sh->scrape_ns / sh->scrapes : 0);
	CHECK(sh->checked > 0, "no grain was checked");
	/*
	 * A stale page is a lost write and a torn grain a mixed one: never.
	 * A missing grain is one a reader could not check before the writer
	 * reused its slot, 600 ms after it was written: a reader starved of
	 * CPU, which a guest on a slow runner is with or without moves, or
	 * held up by a move. So the bound is relative to the run without
	 * moves (MISSING_BASE_PERMILLE): twice its share plus 0.5%.
	 */
	CHECK(!sh->stale && !sh->torn, "stale or torn grains");
	CHECK(sh->missing * 1000 <= sh->checked * (2 * missing_base + 5),
	      "%" PRIu64 " of %" PRIu64 " grains missed, over twice the %lu permille without moves plus 0.5%%",
	      sh->missing, sh->checked, missing_base);
	/* BUSY is the exception: a fault that outlasted the move's wait. */
	CHECK(busy_retries * 100 <= calls, "%" PRIu64 " of %" PRIu64 " move calls retried a BUSY block, over 1%%",
	      busy_retries, calls);
	CHECK(!not_all && !mismatch, "a move left blocks or the counters disagree");
	CHECK(sh->pmd_kb_start == (uint64_t)files * MB_SIZE / 1024 && sh->pmd_kb_end == sh->pmd_kb_start,
	      "FilePmdMapped %" PRIu64 " kB at the start, %" PRIu64 " at the end", sh->pmd_kb_start,
	      sh->pmd_kb_end);
	CHECK(vmstat("thp_split_page") == split0, "a THP was split");
	CHECK(!moves || (passes && hist == blocks && blocks == passes * files * (MB_SIZE >> 21)),
	      "kernel windows %" PRIu64 " for %" PRIu64 " blocks moved", hist, blocks);
	CHECK(!moves || median_us <= bucket_us[0] || median_us * 1000 <= copy_ns,
	      "half the kernel block windows take up to %" PRIu64 " us, over a 2 MiB copy of %.1f us",
	      median_us, copy_ns / 1e3);
	CHECK(!moves || !under_1ms_permille || under_1ms * 1000 >= hist * under_1ms_permille,
	      "%" PRIu64 " of %" PRIu64 " kernel block windows under 1 ms, fewer than %lu permille", under_1ms, hist,
	      under_1ms_permille);
	CHECK(!moves || !p99_max_us || (w99 <= p99_max_us * 1000 && r99 <= p99_max_us * 1000),
	      "the 99th percentile of the accessors' block times is %u us, over %lu",
	      (w99 > r99 ? w99 : r99) / 1000, p99_max_us);
	printf(" failed=%u\n", failed);
	for (unsigned i = 0; i < files; i++) {
		snprintf(name, sizeof(name), "%s/ring.%u", dir, i);
		unlink(name);
	}
	return failed ? 3 : 0;
}

/*
 * A pin refuses a block: one 2 MiB block of a three-block file pinned
 * long term through an io_uring fixed buffer, which pins as an RDMA memory
 * registration does (FOLL_LONGTERM). A strict move fails with EBUSY and
 * moves nothing; a move moves the other blocks and reports the pinned one;
 * once unpinned it moves too.
 */
int cmd_movepin(const char *dir)
{
	const size_t size = 6 << 20, block = 2 << 20;
	struct io_uring_params p = { 0 };
	struct mxlfs_move m;
	uint64_t pages[MAX_NODES];
	unsigned failed = 0;
	int fd = tagged_file(dir, "pin", size, 0x50494e), ring, rc;
	uint8_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct iovec iov = { map + block, block };

	if (map == MAP_FAILED)
		die("mmap");
	ring = syscall(__NR_io_uring_setup, 1, &p);
	if (ring < 0 || syscall(__NR_io_uring_register, ring, IORING_REGISTER_BUFFERS, &iov, 1))
		die("io_uring fixed buffer");
	CHECK(all_on(fd, 0, size), "not on node 0 at the start");

	rc = move_file(fd, 1, MXLFS_MOVE_STRICT, &m);
	print_move("movepin strict", rc, &m);
	CHECK(rc == -EBUSY && m.reason == MXLFS_REFUSE_PINNED && m.refused_offset == block && !m.blocks_moved,
	      "strict move of a pinned file");
	CHECK(all_on(fd, 0, size), "a strict move that failed moved pages");

	rc = move_file(fd, 1, 0, &m);
	print_move("movepin", rc, &m);
	CHECK(!rc && m.result == MXLFS_MOVED_PARTIAL && m.reason == MXLFS_REFUSE_PINNED &&
	      m.refused_offset == block && m.blocks_moved == 2 && m.blocks_refused == 1,
	      "move of a file with a pinned block");
	CHECK(counts_agree(fd, pages) && pages[0] == block / 4096 && pages[1] == 2 * block / 4096,
	      "pages per node after a partial move: %" PRIu64 " %" PRIu64, pages[0], pages[1]);
	CHECK(!tag_errors(fd, size, 0x50494e), "contents changed");

	/* Unregistering unpins at once; closing the ring would unpin later, from a worker. */
	if (syscall(__NR_io_uring_register, ring, IORING_UNREGISTER_BUFFERS, NULL, 0))
		die("IORING_UNREGISTER_BUFFERS");
	close(ring);
	rc = move_file(fd, 1, 0, &m);
	print_move("movepin unpinned", rc, &m);
	CHECK(!rc && m.result == MXLFS_MOVED_ALL && m.blocks_moved == 1 && m.blocks_on_node == 2,
	      "move after the pin ended");
	CHECK(all_on(fd, 1, size) && !tag_errors(fd, size, 0x50494e), "after the pin ended");
	munmap(map, size);
	close(fd);
	printf("movepin failed=%u\n", failed);
	return failed ? 3 : 0;
}

/*
 * A view maps a block: a reader of a view over the file faults the first
 * 2 MiB. A move leaves exactly that block, reason VIEW, and the reader
 * still reads the file's data through the view, on every page; once the
 * reader is gone the block moves.
 */
int cmd_moveview(const char *dir)
{
	const size_t size = 6 << 20;
	struct mxlfs_move m;
	unsigned failed = 0;
	int src[2], view, rc, up[2], down[2], status;
	pid_t pid;

	src[0] = tagged_file(dir, "vsrc0", size, 0x5600);
	src[1] = tagged_file(dir, "vsrc1", size, 0x5601);
	view = new_view(dir, "vview", size, src, 2);
	if (pipe(up) || pipe(down))
		die("pipe");
	if (!(pid = fork())) {
		const volatile uint64_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, view, 0);
		unsigned bad = 0;

		if (p == MAP_FAILED)
			die("mmap view");
		(void)p[0];
		if (write(up[1], "r", 1) != 1)
			die("pipe");
		await(down[0]);
		for (size_t off = 0; off < size; off += 4096)
			if (p[off / 8] != (0x5600 ^ off))
				bad++;
		_exit(bad ? 1 : 0);
	}
	await(up[0]);
	rc = move_file(src[0], 1, 0, &m);
	print_move("moveview", rc, &m);
	CHECK(!rc && m.result == MXLFS_MOVED_PARTIAL && m.reason == MXLFS_REFUSE_VIEW && m.refused_offset == 0 &&
	      m.blocks_refused == 1 && m.blocks_moved == 2, "move under a view's mapping of block 0");
	if (write(down[1], "g", 1) != 1)
		die("pipe");
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status),
	      "the view reader read wrong data after the move");
	CHECK(!tag_errors(view, size, 0x5600), "read() through the view after the move");
	rc = move_file(src[0], 1, 0, &m);
	print_move("moveview unmapped", rc, &m);
	CHECK(!rc && m.result == MXLFS_MOVED_ALL && m.blocks_moved == 1, "move once the view is unmapped");
	CHECK(all_on(src[0], 1, size) && !tag_errors(view, size, 0x5600), "after the last move");
	close(view);
	close(src[0]);
	close(src[1]);
	printf("moveview failed=%u\n", failed);
	return failed ? 3 : 0;
}

/*
 * DRY_RUN and STRICT. A dry run reports what a move would do and changes
 * nothing: no page, no preferred node, no counter but dry_runs. A strict
 * move of a file whose last block a view maps fails with EBUSY before it
 * moves anything. Bad nodes and flags are refused.
 */
int cmd_moveflags(const char *dir)
{
	const size_t size = 6 << 20, block = 2 << 20;
	struct mxlfs_stats s0, s1;
	struct mxlfs_node_pages np;
	struct mxlfs_move m;
	uint64_t pages[MAX_NODES];
	unsigned failed = 0;
	int src[2], view, rc;

	src[0] = tagged_file(dir, "fsrc0", size, 0x4600);
	src[1] = tagged_file(dir, "fsrc1", size, 0x4601);
	stats_of(src[0], &s0);
	rc = move_file(src[0], 1, MXLFS_MOVE_DRY_RUN, &m);
	print_move("moveflags dry", rc, &m);
	stats_of(src[0], &s1);
	node_pages(src[0], 0, pages, &np);
	CHECK(!rc && m.result == MXLFS_MOVED_ALL && m.blocks == 3 && !m.blocks_on_node && !m.blocks_moved &&
	      !m.blocks_refused && !m.bytes_moved, "dry run counts");
	CHECK(all_on(src[0], 0, size) && np.pref == MXLFS_NODE_NONE, "a dry run changed pages or the preference");
	CHECK(s1.dry_runs == s0.dry_runs + 1 && s1.move_calls == s0.move_calls &&
	      s1.move_blocks == s0.move_blocks && s1.move_window_ns_sum == s0.move_window_ns_sum,
	      "a dry run counted as a move");

	view = new_view(dir, "fview", size, src, 2);
	{
		const volatile uint8_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, view, 0);

		if (p == MAP_FAILED)
			die("mmap view");
		(void)p[2 * block];
		rc = move_file(src[0], 1, MXLFS_MOVE_STRICT, &m);
		print_move("moveflags strict", rc, &m);
		CHECK(rc == -EBUSY && m.reason == MXLFS_REFUSE_VIEW && m.refused_offset == 2 * block &&
		      !m.blocks_moved && m.blocks_refused == 3, "strict move under a view's mapping of block 2");
		CHECK(all_on(src[0], 0, size) && !tag_errors(src[0], size, 0x4600), "a strict move that failed moved");
		rc = move_file(src[0], 1, MXLFS_MOVE_DRY_RUN | MXLFS_MOVE_STRICT, &m);
		CHECK(!rc && m.result == MXLFS_MOVED_ALL, "a strict dry run cannot see the view's mapping");
		munmap((void *)p, size);
	}
	rc = move_file(src[0], 1, MXLFS_MOVE_STRICT, &m);
	print_move("moveflags strict unmapped", rc, &m);
	CHECK(!rc && m.result == MXLFS_MOVED_ALL && m.blocks_moved == 3, "strict move once unmapped");
	node_pages(src[0], 0, pages, &np);
	CHECK(np.pref == 1 && np.effective == 1, "the preference after a whole move: %d", np.pref);
	CHECK(move_file(src[0], MAX_NODES, 0, &m) == -EINVAL && move_file(src[0], -1, 0, &m) == -EINVAL &&
	      move_file(src[0], 0, 4, &m) == -EINVAL && move_file(view, 0, 0, &m) == -EPERM &&
	      move_file(view, 0, MXLFS_MOVE_DRY_RUN, &m) == -EPERM, "bad node or flags (EINVAL), a view (EPERM)");
	close(view);
	close(src[0]);
	close(src[1]);
	printf("moveflags failed=%u\n", failed);
	return failed ? 3 : 0;
}

static uint32_t holds_of(int fd)
{
	uint64_t pages[MAX_NODES];
	struct mxlfs_node_pages np;

	node_pages(fd, 0, pages, &np);
	return np.holds;
}

/*
 * HOLD: held through one open file, MOVE and SET_NODE through another
 * fail with EBUSY (reason HELD) while through the holder's own they work;
 * the hold ends when its process dies, and dup() and fork() share it until
 * the last close. A view cannot be held.
 */
int cmd_movehold(const char *dir)
{
	const size_t size = 4 << 20;
	char path[PATH_MAX];
	struct mxlfs_stats st;
	struct mxlfs_move m;
	unsigned failed = 0;
	int a, b, rc, pfd[2], status, src[2], view;
	pid_t pid;

	b = tagged_file(dir, "hfile", size, 0x4800);
	snprintf(path, sizeof(path), "%s/hfile", dir);
	a = open(path, O_RDONLY | O_CLOEXEC);
	if (a < 0)
		die(path);
	CHECK(!hold(a, 1) && holds_of(b) == 1, "taking a hold");
	CHECK(!hold(a, 1) && holds_of(b) == 1, "a second hold through the same file");
	stats_of(a, &st);
	CHECK(st.holds == 1, "the mount counts %llu holds", (unsigned long long)st.holds);
	rc = move_file(b, 1, 0, &m);
	print_move("movehold other", rc, &m);
	CHECK(rc == -EBUSY && m.reason == MXLFS_REFUSE_HELD && !m.blocks_moved, "move through another file");
	CHECK(set_node(b, 1) == -EBUSY, "SET_NODE through another file");
	CHECK(all_on(b, 0, size), "a refused move moved");
	rc = move_file(a, 1, 0, &m);
	print_move("movehold holder", rc, &m);
	CHECK(!rc && m.result == MXLFS_MOVED_ALL, "move through the holder");
	CHECK(!set_node(a, 0), "SET_NODE through the holder");
	CHECK(!hold(a, 0) && holds_of(b) == 0, "dropping the hold");
	CHECK(!set_node(b, MXLFS_NODE_NONE), "SET_NODE once nobody holds");

	/* A holder that dies. */
	if (pipe(pfd))
		die("pipe");
	if (!(pid = fork())) {
		int fd = open(path, O_RDONLY | O_CLOEXEC);

		if (fd < 0 || hold(fd, 1) || write(pfd[1], "h", 1) != 1)
			_exit(1);
		pause();
		_exit(0);
	}
	await(pfd[0]);
	CHECK(holds_of(b) == 1 && move_file(b, 0, 0, &m) == -EBUSY, "held by another process");
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	CHECK(holds_of(b) == 0 && !move_file(b, 0, 0, &m), "the hold ended with its process");

	/* dup() and fork() share a hold until the last close. */
	close(a);
	a = open(path, O_RDONLY | O_CLOEXEC);
	if (a < 0 || hold(a, 1))
		die("hold");
	int a2 = fcntl(a, F_DUPFD, 0);

	if (!(pid = fork())) {
		char c;

		close(a);
		/* a2 is not close-on-exec and survives here; wait to be told to go. */
		if (read(pfd[0], &c, 1) != 1)
			_exit(1);
		_exit(0);
	}
	close(a);
	CHECK(holds_of(b) == 1, "a dup keeps the hold");
	close(a2);
	CHECK(holds_of(b) == 1, "a forked child's copy keeps the hold");
	if (write(pfd[1], "x", 1) != 1)
		die("pipe");
	waitpid(pid, &status, 0);
	CHECK(holds_of(b) == 0, "the last close ends the hold");

	src[0] = tagged_file(dir, "hsrc0", size, 1);
	src[1] = tagged_file(dir, "hsrc1", size, 2);
	view = new_view(dir, "hview", size, src, 2);
	CHECK(hold(view, 1) == -EPERM && set_node(view, 0) == -EPERM, "a view cannot be held or placed (EPERM)");
	/* Coverage of an errno ALLOCATE always had, beside the three that changed. */
	CHECK(ioctl(view, MXLFS_IOC_ALLOCATE) == -1 && errno == EPERM, "a view cannot be allocated (EPERM)");
	CHECK(hold(b, 2) == -EINVAL, "HOLD takes 0 or 1");
	close(view);
	close(src[0]);
	close(src[1]);
	close(b);
	printf("movehold failed=%u\n", failed);
	return failed ? 3 : 0;
}

/*
 * The folios of the files a domain holds keep their orders across a move
 * and back, under the mount's huge= (HUGE=always or the default): a grain
 * file, a lazily sized one allocated after its header was written, and a
 * small file. Every page ends up on the node, as move_pages(2) reports it,
 * the counters agree with a walk, the contents are unchanged, and the
 * nodes' FilePages and FileHugePages move by the file's pages.
 */
int cmd_moveorders(const char *dir)
{
	const bool always = getenv("HUGE") && !strcmp(getenv("HUGE"), "always");
	static const size_t sizes[] = { 5537792, 5537792, 1 << 20 };
	unsigned failed = 0;

	/*
	 * Brings every page of this program into the page cache now, as
	 * cmd_numacount does: a code page first run between the FilePages
	 * readings around a move would count on the node it runs on.
	 */
	if (mlockall(MCL_CURRENT) || munlockall())
		die("mlockall");
	for (int c = 0; c < 3; c++) {
		size_t size = sizes[c], resident;
		char name[32], before[256], after[256];
		uint64_t tag = 0x4f00 + c, fp[2], fh[2];
		int fd;

		snprintf(name, sizeof(name), "orders%d", c);
		if (c == 1) {
			__u32 on = 1;
			char path[PATH_MAX];

			snprintf(path, sizeof(path), "%s/%s", dir, name);
			unlink(path);
			fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
			if (fd < 0 || ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on) || posix_fallocate(fd, 0, size))
				die(path);
			on = 0;
			if (pwrite(fd, &tag, sizeof(tag), 0) != sizeof(tag) ||
			    ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on) || ioctl(fd, MXLFS_IOC_ALLOCATE))
				die("lazy allocate");
			for (size_t off = 0; off < size; off += 4096) {
				uint64_t v = tag ^ off;

				if (pwrite(fd, &v, sizeof(v), off) != sizeof(v))
					die("pwrite");
			}
		} else {
			fd = tagged_file(dir, name, size, tag);
		}
		uint8_t *p = map_span(fd, size);

		if (!folio_orders(p, size, before, sizeof(before)))
			die("folio orders");
		munmap(p, span_of(size));
		for (int to = 1; to >= 0; to--) {
			struct mxlfs_move m;
			int rc;

			for (int n = 0; n < 2; n++) {
				fp[n] = node_kb(n, "FilePages");
				fh[n] = node_kb(n, "FileHugePages");
			}
			rc = move_file(fd, to, 0, &m);
			/* Before anything is printed: the output may land in page cache. */
			int64_t d_to = node_kb(to, "FilePages") - fp[to], d_from = fp[1 - to] - node_kb(1 - to, "FilePages");
			int64_t h_to = node_kb(to, "FileHugePages") - fh[to],
				h_from = fh[1 - to] - node_kb(1 - to, "FileHugePages");

			print_move(name, rc, &m);
			CHECK(!rc && m.result == MXLFS_MOVED_ALL && m.blocks_moved == m.blocks,
			      "%s to node %d", name, to);
			p = map_span(fd, size);
			CHECK(folio_orders(p, size, after, sizeof(after)) && !strcmp(before, after),
			      "%s: folios %s before the move, %s after", name, before, after);
			munmap(p, span_of(size));
			CHECK(!off_node(fd, size, to, &resident) && all_on(fd, to, size), "%s: pages off node %d",
			      name, to);
			CHECK(!tag_errors(fd, size, tag), "%s: contents changed", name);
			int64_t moved_kb = m.bytes_moved / 1024, huge_kb = 0;

			for (const char *o = after; *o; o++)
				if (*o == '9' && (o == after || o[-1] == ','))
					huge_kb += 2048;
			CHECK(d_to == moved_kb && d_from == moved_kb,
			      "%s: FilePages moved %lld and %lld kB for %lld", name, (long long)d_to,
			      (long long)d_from, (long long)moved_kb);
			CHECK(h_to == huge_kb && h_from == huge_kb, "%s: FileHugePages moved %lld and %lld kB for %lld",
			      name, (long long)h_to, (long long)h_from, (long long)huge_kb);
		}
		printf("moveorders huge=%s %s size=%zu folios=%s\n", always ? "always" : "within_size", name, size,
		       before);
		close(fd);
	}
	printf("moveorders huge=%s failed=%u\n", always ? "always" : "within_size", failed);
	return failed ? 3 : 0;
}

/*
 * No room on the destination: node 1 is filled through FILL, a mount with
 * numa_node=1, until a 2 MiB folio that preferred it came from another
 * node, which leaves less free than the 128 MiB file. A move finds no
 * destination for every block, fails with ENOMEM and moves and splits
 * nothing. Once the filler is gone the move succeeds.
 */
int cmd_movenomem(const char *dir, const char *fill)
{
	const size_t size = 128 << 20;
	struct mxlfs_node_stat st[MAX_NODES];
	struct mxlfs_stats s0, s1;
	struct mxlfs_move m;
	char path[PATH_MAX], before[512], after[512];
	unsigned failed = 0;
	uint64_t split0 = vmstat("thp_split_page"), fallbacks, filled = 0, free_kb;
	int fd = tagged_file(dir, "nomem", size, 0x4e4f), ffd, rc;
	uint8_t *p = map_span(fd, size);

	if (!folio_orders(p, size, before, sizeof(before)))
		die("folio orders");
	munmap(p, span_of(size));
	snprintf(path, sizeof(path), "%s/filler", fill);
	unlink(path);
	ffd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (ffd < 0)
		die(path);
	mount_nodes(ffd, st);
	for (fallbacks = st[1].fallback_folios; st[1].fallback_folios == fallbacks; mount_nodes(ffd, st)) {
		filled += 2 << 20;
		if (posix_fallocate(ffd, 0, filled))
			die("fill");
	}
	free_kb = node_kb(1, "MemFree");
	printf("movenomem filled_mb=%" PRIu64 " node1_free_kb=%" PRIu64 "\n", filled >> 20, free_kb);
	CHECK(free_kb * 1024 < size, "node 1 kept %" PRIu64 " kB free", free_kb);
	stats_of(fd, &s0);
	rc = move_file(fd, 1, 0, &m);
	print_move("movenomem", rc, &m);
	stats_of(fd, &s1);
	CHECK(rc == -ENOMEM && m.reason == MXLFS_REFUSE_NOMEM && m.result == MXLFS_MOVED_NONE && !m.blocks_moved &&
	      m.blocks_refused == m.blocks && m.blocks == size >> 21, "move to a full node");
	CHECK(s1.move_refused[MXLFS_REFUSE_NOMEM] - s0.move_refused[MXLFS_REFUSE_NOMEM] == size >> 21 &&
	      s1.move_calls_none == s0.move_calls_none + 1, "refusals counted");
	p = map_span(fd, size);
	CHECK(folio_orders(p, size, after, sizeof(after)) && !strcmp(before, after), "folios %s, were %s", after,
	      before);
	munmap(p, span_of(size));
	CHECK(all_on(fd, 0, size) && !tag_errors(fd, size, 0x4e4f), "a move without room moved or changed pages");
	CHECK(vmstat("thp_split_page") == split0, "a THP was split");
	close(ffd);
	unlink(path);
	rc = move_file(fd, 1, 0, &m);
	print_move("movenomem freed", rc, &m);
	CHECK(!rc && m.result == MXLFS_MOVED_ALL && all_on(fd, 1, size), "move once node 1 has room");
	close(fd);
	printf("movenomem failed=%u\n", failed);
	return failed ? 3 : 0;
}

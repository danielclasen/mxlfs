#include "harness.h"

static void pct(const char *name, uint64_t *v, uint64_t n)
{
	qsort(v, n, sizeof(*v), cmp_u64);
	printf(" %s_us_p50=%.1f p99=%.1f max=%.1f", name, v[n / 2] / 1e3, v[n * 99 / 100] / 1e3, v[n - 1] / 1e3);
}

struct bench_shared {
	uint32_t round;	/* futex: bumped by the parent per round */
	uint32_t done;	/* readers finished with the round */
	uint32_t source;	/* source selected for the round */
	uint32_t errors;
	uint64_t touch[];	/* [round * readers + reader] */
};

int cmd_bench(const char *dir, unsigned readers, unsigned rounds)
{
	const size_t size = GRAIN_HEADER_SIZE + 5529600;
	struct bench_shared *sh;
	uint64_t *sel_ns = calloc(rounds, sizeof(*sel_ns)), *sw, *warm;
	uint64_t nsw = 0, nwarm = 0;
	char path[PATH_MAX];
	int src[2], view;

	sh = mmap(NULL, sizeof(*sh) + (size_t)rounds * readers * 8, PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		die("mmap shared");
	/* A view from an earlier run keeps its sources from being resized. */
	snprintf(path, sizeof(path), "%s/view", dir);
	unlink(path);
	for (int i = 0; i < 2; i++) {
		uint8_t *p;

		snprintf(path, sizeof(path), "%s/src%d", dir, i);
		src[i] = open(path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0644);
		if (src[i] < 0 || posix_fallocate(src[i], 0, size))
			die(path);
		p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, src[i], 0);
		if (p == MAP_FAILED)
			die("mmap source");
		for (size_t off = 0; off < size; off += 4096)
			*(uint64_t *)(p + off) = i;
		munmap(p, size);
	}
	snprintf(path, sizeof(path), "%s/view", dir);
	view = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (view < 0 || ftruncate(view, size))
		die(path);
	struct mxlfs_view_sources req = { .count = 2, .fds = { src[0], src[1] } };
	if (ioctl(view, MXLFS_IOC_VIEW_SET_SOURCES, &req))
		die("MXLFS_IOC_VIEW_SET_SOURCES");

	for (unsigned r = 0; r < readers; r++) {
		if (fork())
			continue;
		const volatile uint8_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, view, 0);
		uint32_t seen = 0;

		if (p == MAP_FAILED)
			die("mmap view");
		for (unsigned round = 1; round <= rounds; round++) {
			uint64_t t0, sum = 0;

			while (__atomic_load_n(&sh->round, __ATOMIC_ACQUIRE) == seen)
				syscall(SYS_futex, &sh->round, FUTEX_WAIT, seen, NULL, NULL, 0);
			seen = round;
			t0 = now_ns();
			for (size_t off = 0; off < size; off += 4096)
				sum += *(const volatile uint64_t *)(p + off);
			sh->touch[(round - 1) * readers + r] = now_ns() - t0;
			if (sum != (uint64_t)sh->source * (size / 4096))
				__atomic_fetch_add(&sh->errors, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&sh->done, 1, __ATOMIC_RELEASE);
		}
		_exit(0);
	}

	for (unsigned round = 1; round <= rounds; round++) {
		uint32_t target = round % 4 == 0 ? !sh->source : sh->source;
		uint64_t t0 = now_ns();

		if (ioctl(view, MXLFS_IOC_VIEW_SELECT, &target))
			die("MXLFS_IOC_VIEW_SELECT");
		sel_ns[round - 1] = now_ns() - t0;
		sh->source = target;
		__atomic_store_n(&sh->done, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&sh->round, round, __ATOMIC_RELEASE);
		syscall(SYS_futex, &sh->round, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
		while (__atomic_load_n(&sh->done, __ATOMIC_ACQUIRE) < readers)
			usleep(50);
		usleep(2000);
	}
	while (wait(NULL) > 0)
		;

	sw = calloc((size_t)rounds * readers, sizeof(*sw));
	warm = calloc((size_t)rounds * readers, sizeof(*warm));
	uint64_t nsel = 0;
	for (unsigned round = 1; round <= rounds; round++) {
		bool switched = round % 4 == 0;

		if (switched)
			sel_ns[nsel++] = sel_ns[round - 1];
		for (unsigned r = 0; r < readers; r++) {
			uint64_t t = sh->touch[(round - 1) * readers + r];

			if (round == 1)
				continue; /* first mapping of the view */
			if (switched)
				sw[nsw++] = t;
			else
				warm[nwarm++] = t;
		}
	}
	printf("bench readers=%u switches=%" PRIu64 " errors=%u", readers, nsel, sh->errors);
	pct("select", sel_ns, nsel);
	pct("read_after_switch", sw, nsw);
	pct("read_warm", warm, nwarm);
	printf("\n");
	return sh->errors ? 3 : 0;
}

int cmd_ranges(const char *dir, unsigned readers, unsigned rounds)
{
	const size_t size = 3 << 20, pages = size / 4096;
	const unsigned nsrc = 3;
	struct shared {
		uint32_t round, done, errors, pad;
		uint8_t expect[];
	} *sh;
	char path[PATH_MAX];
	int src[3], view;
	uint64_t seed = env("SEED", 1);

	sh = mmap(NULL, sizeof(*sh) + pages, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		die("mmap shared");
	snprintf(path, sizeof(path), "%s/rview", dir);
	unlink(path);
	for (unsigned i = 0; i < nsrc; i++) {
		uint8_t *p;

		snprintf(path, sizeof(path), "%s/rsrc%u", dir, i);
		unlink(path);
		src[i] = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
		if (src[i] < 0 || posix_fallocate(src[i], 0, size))
			die(path);
		p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, src[i], 0);
		if (p == MAP_FAILED)
			die("mmap source");
		for (size_t pg = 0; pg < pages; pg++)
			*(uint64_t *)(p + pg * 4096) = ((uint64_t)i << 32) | pg;
		munmap(p, size);
	}
	snprintf(path, sizeof(path), "%s/rview", dir);
	view = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (view < 0 || ftruncate(view, size))
		die(path);
	struct mxlfs_view_sources req = { .count = nsrc, .fds = { src[0], src[1], src[2] } };
	if (ioctl(view, MXLFS_IOC_VIEW_SET_SOURCES, &req))
		die("MXLFS_IOC_VIEW_SET_SOURCES");

	for (unsigned r = 0; r < readers; r++) {
		if (fork())
			continue;
		const volatile uint8_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, view, 0);
		uint32_t seen = 0;

		if (p == MAP_FAILED)
			die("mmap view");
		for (unsigned round = 1; round <= rounds; round++) {
			while (__atomic_load_n(&sh->round, __ATOMIC_ACQUIRE) == seen)
				syscall(SYS_futex, &sh->round, FUTEX_WAIT, seen, NULL, NULL, 0);
			seen = round;
			for (size_t pg = 0; pg < pages; pg++) {
				uint64_t v = *(const volatile uint64_t *)(p + pg * 4096);

				if (v != (((uint64_t)sh->expect[pg] << 32) | pg)) {
					__atomic_fetch_add(&sh->errors, 1, __ATOMIC_RELAXED);
					break;
				}
			}
			__atomic_fetch_add(&sh->done, 1, __ATOMIC_RELEASE);
		}
		_exit(0);
	}

	uint64_t pread_errors = 0;
	for (unsigned round = 1; round <= rounds; round++) {
		/* A few ranges per round, some inside one PMD span, some across. */
		for (int k = 0; k < 3; k++) {
			uint64_t x = splitmix64(seed ^ (round * 31 + k));
			struct mxlfs_view_range rr = {
				.source = x % nsrc,
				.offset = ((x >> 8) % pages) * 4096,
				.length = (1 + (x >> 24) % (k == 2 ? pages : 8)) * 4096,
			};
			size_t first = rr.offset / 4096, last = first + rr.length / 4096;

			if (last > pages)
				last = pages;
			if (ioctl(view, MXLFS_IOC_VIEW_SELECT_RANGE, &rr))
				die("MXLFS_IOC_VIEW_SELECT_RANGE");
			for (size_t pg = first; pg < last; pg++)
				sh->expect[pg] = rr.source;
		}
		for (size_t pg = 0; pg < pages; pg += 97) {
			uint64_t v;

			if (pread(view, &v, sizeof(v), pg * 4096) != sizeof(v) ||
			    v != (((uint64_t)sh->expect[pg] << 32) | pg))
				pread_errors++;
		}
		__atomic_store_n(&sh->done, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&sh->round, round, __ATOMIC_RELEASE);
		syscall(SYS_futex, &sh->round, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
		while (__atomic_load_n(&sh->done, __ATOMIC_ACQUIRE) < readers)
			usleep(50);
	}
	while (wait(NULL) > 0)
		;
	printf("ranges readers=%u rounds=%u errors=%u pread_errors=%" PRIu64 "\n", readers, rounds, sh->errors,
	       pread_errors);
	return (sh->errors || pread_errors) ? 3 : 0;
}

/* A new file of size whose pages carry gen << 40 | source << 32 | page. */
int marked_file(const char *path, size_t size, uint64_t gen, unsigned source)
{
	int fd;
	uint8_t *p;

	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0 || posix_fallocate(fd, 0, size))
		die(path);
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		die("mmap source");
	for (size_t pg = 0; pg < size / 4096; pg++)
		*(uint64_t *)(p + pg * 4096) = gen << 40 | (uint64_t)source << 32 | pg;
	munmap(p, size);
	return fd;
}

int new_view(const char *dir, const char *name, size_t size, const int *fds, unsigned count)
{
	struct mxlfs_view_sources req = { .count = count };
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0 || ftruncate(fd, size))
		die(path);
	memcpy(req.fds, fds, count * sizeof(*fds));
	if (ioctl(fd, MXLFS_IOC_VIEW_SET_SOURCES, &req))
		die("MXLFS_IOC_VIEW_SET_SOURCES");
	return fd;
}

/* 1 when ret is not what err (0 for success) says it should be. */
static unsigned unexpected(const char *what, int ret, int err)
{
	if (err ? ret == -1 && errno == err : ret == 0)
		return 0;
	fprintf(stderr, "replace: %s: got %s, want %s\n", what, ret ? strerror(errno) : "success",
		err ? strerror(err) : "success");
	return 1;
}

/*
 * Replacing a source with the file already there changes nothing: no
 * replace is counted, and the view uses the file once, so it can be
 * resized again once the view is gone. Returns the failures.
 */
static unsigned replace_same_file(const char *dir)
{
	const size_t size = 65536;
	struct mxlfs_stats s0, s1;
	char path[PATH_MAX];
	unsigned bad = 0;
	int f[2], v;

	for (int i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/ssrc%d", dir, i);
		f[i] = marked_file(path, size, 0, i);
	}
	v = new_view(dir, "sview", size, f, 2);
	if (ioctl(v, MXLFS_IOC_STATS, &s0))
		die("MXLFS_IOC_STATS");
	for (int i = 0; i < 3; i++) {
		struct mxlfs_view_replace rq = { .source = 0, .fd = f[0] };

		bad += unexpected("a source in its own place", ioctl(v, MXLFS_IOC_VIEW_REPLACE_SOURCE, &rq), 0);
	}
	if (ioctl(v, MXLFS_IOC_STATS, &s1))
		die("MXLFS_IOC_STATS");
	if (s1.view_replaces != s0.view_replaces) {
		errno = EIO;
		bad += unexpected("a source in its own place counts no replace", -1, 0);
	}
	bad += unexpected("resizing it while the view uses it", ftruncate(f[0], 4096), EBUSY);
	snprintf(path, sizeof(path), "%s/sview", dir);
	unlink(path);
	close(v);
	bad += unexpected("resizing it once the view is gone", ftruncate(f[0], 4096), 0);
	for (int i = 0; i < 2; i++)
		close(f[i]);
	return bad;
}

/* A file of size on another filesystem: a memfd, which is shmem's. */
static int foreign_file(size_t size)
{
	int fd = memfd_create("mxlfs-foreign", MFD_CLOEXEC);

	if (fd < 0 || ftruncate(fd, size))
		die("memfd");
	return fd;
}

/* The refusals and the accounting, on 64 KiB views; returns the failures. */
static unsigned replace_refusals(const char *dir)
{
	const size_t size = 65536;
	char path[PATH_MAX];
	int f[4], nview, nview2, ro, wo, foreign, other = -1;
	unsigned bad = 0;
	/* The first struct and one word past it; larger than the module's by 6 words. */
	__u64 v1[5], v8[sizeof(struct mxlfs_stats) / sizeof(__u64) + 6];
	const size_t n8 = sizeof(v8) / sizeof(v8[0]);

	for (int i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "%s/nsrc%d", dir, i);
		f[i] = marked_file(path, i == 3 ? 2 * size : size, 0, i);
	}
	nview = new_view(dir, "nview", size, f, 2);
	nview2 = new_view(dir, "nview2", size, f, 1);
	snprintf(path, sizeof(path), "%s/nview", dir);
	ro = open(path, O_RDONLY | O_CLOEXEC);
	if (ro < 0)
		die(path);
	snprintf(path, sizeof(path), "%s/nsrc2", dir);
	wo = open(path, O_WRONLY | O_CLOEXEC);
	if (wo < 0)
		die(path);
	foreign = foreign_file(size);
	/* Another mxlfs, if the caller mounted one there. */
	if (getenv("OTHER_DIR")) {
		snprintf(path, sizeof(path), "%s/nsrc-other", getenv("OTHER_DIR"));
		other = marked_file(path, size, 0, 2);
	} else {
		fprintf(stderr, "replace: OTHER_DIR unset, no second mxlfs to take a source from\n");
	}

	struct {
		const char *what;
		int view, fd;
		__u32 source, flags;
		int err;
	} cases[] = {
		{ "a source of another size", nview, f[3], 0, 0, EINVAL },
		{ "a view fd open read-only", ro, f[2], 0, 0, EBADF },
		{ "a source number past the view's sources", nview, f[2], 2, 0, EINVAL },
		{ "a view as the source", nview, nview2, 0, 0, EINVAL },
		{ "the view as its own source", nview, nview, 0, 0, EINVAL },
		{ "unknown flags", nview, f[2], 0, 1, EINVAL },
		/* The probe a caller makes for the ioctl: flags come first. */
		{ "flags ~0 on a view fd open read-only", ro, f[2], 0, ~0u, EINVAL },
		{ "flags ~0 on a file that is no view", f[0], -1, 99, ~0u, EINVAL },
		{ "no file", nview, -1, 0, 0, EBADF },
		{ "a source open write-only", nview, wo, 0, 0, EBADF },
		{ "a file on another filesystem", nview, foreign, 0, 0, EINVAL },
		{ "a file on another mxlfs", nview, other < 0 ? foreign : other, 0, 0, EINVAL },
		{ "a file that is no view", f[0], f[2], 0, 0, EINVAL },
		{ "c in place of b", nview, f[2], 1, 0, 0 },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct mxlfs_view_replace rq = { .source = cases[i].source, .fd = cases[i].fd, .flags = cases[i].flags };

		bad += unexpected(cases[i].what, ioctl(cases[i].view, MXLFS_IOC_VIEW_REPLACE_SOURCE, &rq),
				  cases[i].err);
	}
	{
		struct mxlfs_view_sources req = { .count = 2, .fds = { f[0], wo } };
		int v;

		snprintf(path, sizeof(path), "%s/nview3", dir);
		unlink(path);
		v = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (v < 0 || ftruncate(v, size))
			die(path);
		bad += unexpected("SET_SOURCES with a source open write-only", ioctl(v, MXLFS_IOC_VIEW_SET_SOURCES, &req),
				  EBADF);
		/* Entries past count are unused and must be 0. */
		req = (struct mxlfs_view_sources){ .count = 1, .fds = { f[0], f[1] } };
		bad += unexpected("SET_SOURCES with an fd past count", ioctl(v, MXLFS_IOC_VIEW_SET_SOURCES, &req),
				  EINVAL);
		/* The probe a caller makes for the ioctl: flags come first. */
		req = (struct mxlfs_view_sources){ .count = 1, .flags = ~0u, .fds = { f[0] } };
		bad += unexpected("SET_SOURCES flags ~0 on a view fd open read-only",
				  ioctl(ro, MXLFS_IOC_VIEW_SET_SOURCES, &req), EINVAL);
		close(v);
		unlink(path);
	}
	{
		__u32 two = 2, off = 0;

		bad += unexpected("LAZY_ALLOC with 2", ioctl(f[0], MXLFS_IOC_LAZY_ALLOC, &two), EINVAL);
		ioctl(f[0], MXLFS_IOC_LAZY_ALLOC, &off);
	}
	/*
	 * A view keeps a byte per page. Sizes set by ftruncate allocate
	 * nothing, so any process can ask for a table of any size; the last
	 * case is past what kvmalloc takes, where it would warn.
	 */
	static const struct {
		const char *what;
		off_t size;
		int err;
	} big[] = {
		{ "a view of MXLFS_VIEW_MAX_SIZE", MXLFS_VIEW_MAX_SIZE, 0 },
		{ "a view a page past MXLFS_VIEW_MAX_SIZE", MXLFS_VIEW_MAX_SIZE + 4096, EINVAL },
		{ "a view of 1 PiB", 1LL << 50, EINVAL },
	};
	for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); i++) {
		struct mxlfs_view_sources req = { .count = 1 };
		char vpath[PATH_MAX];
		int v;

		snprintf(path, sizeof(path), "%s/nbig-src", dir);
		snprintf(vpath, sizeof(vpath), "%s/nbig-view", dir);
		req.fds[0] = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		v = open(vpath, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (req.fds[0] < 0 || v < 0 || ftruncate(req.fds[0], big[i].size) || ftruncate(v, big[i].size))
			die(path);
		bad += unexpected(big[i].what, ioctl(v, MXLFS_IOC_VIEW_SET_SOURCES, &req), big[i].err);
		close(v);
		close(req.fds[0]);
		unlink(vpath);
		unlink(path);
	}
	__u32 one = 1;
	uint64_t v = 0;
	if (ioctl(nview, MXLFS_IOC_VIEW_SELECT, &one) || pread(nview, &v, sizeof(v), 4096) != sizeof(v) ||
	    v != (2ULL << 32 | 1)) {
		errno = EIO;
		bad += unexpected("read() through the view shows c", -1, 0);
	}
	{
		loff_t off = 4096;
		uint64_t w = 0;
		int p[2];

		if (pipe(p))
			die("pipe");
		if (splice(nview, &off, p[1], NULL, sizeof(w), 0) != sizeof(w) ||
		    read(p[0], &w, sizeof(w)) != sizeof(w) || w != (2ULL << 32 | 1)) {
			errno = EIO;
			bad += unexpected("splice() from the view shows c", -1, 0);
		}
		close(p[0]);
		close(p[1]);
	}
	{
		/* A view is read-only; both checks go through mmap_prepare's flags. */
		void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, nview, 0);

		bad += unexpected("a shared writable mapping of a view", m == MAP_FAILED ? -1 : 0, EACCES);
		if (m != MAP_FAILED)
			munmap(m, size);
		m = mmap(NULL, size, PROT_READ, MAP_SHARED, nview, 0);
		if (m == MAP_FAILED)
			die("mmap view");
		bad += unexpected("mprotect of a view's mapping to PROT_WRITE",
				  mprotect(m, size, PROT_READ | PROT_WRITE), EACCES);
		munmap(m, size);
	}
	bad += unexpected("resizing b, which no view uses now", ftruncate(f[1], 4096), 0);
	bad += unexpected("resizing c, which a view uses now", ftruncate(f[2], 4096), EBUSY);
	errno = posix_fallocate(f[2], 0, 2 * size);
	bad += unexpected("growing c with fallocate", errno ? -1 : 0, EBUSY);
	bad += unexpected("growing c with a write past its end",
			  pwrite(f[2], &v, sizeof(v), size - 4) < 0 ? -1 : 0, EBUSY);
	{
		int app;

		snprintf(path, sizeof(path), "%s/nsrc2", dir);
		app = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
		if (app < 0)
			die(path);
		bad += unexpected("growing c with an appending write",
				  write(app, &v, sizeof(v)) < 0 ? -1 : 0, EBUSY);
		close(app);
	}
	bad += unexpected("writing c within its size", pwrite(f[2], &v, sizeof(v), size - 8) < 0 ? -1 : 0, 0);
	{
		struct stat st;

		if (fstat(f[2], &st) || st.st_size != (off_t)size) {
			errno = EIO;
			bad += unexpected("c keeps its size", -1, 0);
		}
	}
	bad += unexpected("allocating c in place (KEEP_SIZE)", fallocate(f[2], FALLOC_FL_KEEP_SIZE, 0, size), 0);
	/* The first struct is filled, as the larger one below shows, and nothing past it. */
	memset(v1, 0xff, sizeof(v1));
	bad += unexpected("MXLFS_IOC_STATS as it was before view_replaces", ioctl(nview, _IOR('x', 4, __u64[4]), v1), 0);
	if (v1[4] != ~0ULL) {
		errno = EIO;
		bad += unexpected("MXLFS_IOC_STATS of 32 bytes writes no more", -1, 0);
	}
	/* A caller's larger struct gets what the module has, and zeroes past it. */
	memset(v8, 0xff, sizeof(v8));
	if (ioctl(nview, _IOR('x', 4, __u64[sizeof(struct mxlfs_stats) / sizeof(__u64) + 6]), v8) ||
	    v8[0] != v1[0] || v8[1] != v1[1]) {
		errno = EIO;
		bad += unexpected("MXLFS_IOC_STATS with a larger struct", -1, 0);
	}
	for (size_t i = n8 - 6; i < n8; i++) {
		if (v8[i]) {
			errno = EIO;
			bad += unexpected("MXLFS_IOC_STATS zeroes a larger struct's tail", -1, 0);
			break;
		}
	}
	/*
	 * Between the first struct and the module's, and not a whole number
	 * of __u64: the module fills exactly that many bytes.
	 */
	for (size_t sz = 40; sz <= 44; sz += 4) {
		uint8_t *b = (uint8_t *)v8;

		memset(v8, 0xff, sizeof(v8));
		if (ioctl(nview, _IOC(_IOC_READ, 'x', 4, sz), v8) || v8[0] != v1[0] ||
		    b[sz] != 0xff || b[sz + 3] != 0xff) {
			errno = EIO;
			bad += unexpected(sz == 40 ? "MXLFS_IOC_STATS of 40 bytes fills exactly 40" :
						     "MXLFS_IOC_STATS of 44 bytes fills exactly 44", -1, 0);
		}
	}
	/* Smaller than any struct the module had: nothing to fill. */
	bad += unexpected("MXLFS_IOC_STATS of 0 bytes", ioctl(nview, _IOC(_IOC_READ, 'x', 4, 0), v8), EINVAL);
	bad += unexpected("MXLFS_IOC_STATS of 16 bytes", ioctl(nview, _IOR('x', 4, __u64[2]), v8), EINVAL);
	bad += unexpected("MXLFS_IOC_STATS one byte short of the first struct",
			  ioctl(nview, _IOC(_IOC_READ, 'x', 4, MXLFS_STATS_SIZE_VER0 - 1), v8), EINVAL);
	close(wo);
	close(foreign);
	if (other >= 0)
		close(other);
	close(ro);
	close(nview);
	close(nview2);
	for (int i = 0; i < 4; i++)
		close(f[i]);
	return bad + replace_same_file(dir);
}

int cmd_replace(const char *dir, unsigned readers, unsigned rounds)
{
	const size_t size = 3 << 20, pages = size / 4096;
	const unsigned nsrc = 3;
	struct shared {
		uint32_t go, busy, done, errors, loose_errors, crashed;
		uint32_t gen[3];
		uint8_t expect[];
	} *sh;
	struct mxlfs_stats s0, s1;
	char path[PATH_MAX], tmp[PATH_MAX];
	int src[3], view, status;
	uint64_t seed = env("SEED", 1), pread_errors = 0;
	unsigned refusals = replace_refusals(dir), selected_at_replace = 0;

	sh = mmap(NULL, sizeof(*sh) + pages, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		die("mmap shared");
	for (unsigned i = 0; i < nsrc; i++) {
		snprintf(path, sizeof(path), "%s/xsrc%u", dir, i);
		src[i] = marked_file(path, size, 0, i);
	}
	view = new_view(dir, "xview", size, src, nsrc);
	for (unsigned i = 0; i < nsrc; i++)
		close(src[i]);
	if (ioctl(view, MXLFS_IOC_STATS, &s0))
		die("MXLFS_IOC_STATS");

	for (unsigned r = 0; r < readers; r++) {
		if (fork())
			continue;
		const volatile uint8_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, view, 0);

		if (p == MAP_FAILED)
			die("mmap view");
		for (unsigned round = 1; round <= rounds; round++) {
			uint32_t go;

			while ((go = __atomic_load_n(&sh->go, __ATOMIC_ACQUIRE)) != round)
				syscall(SYS_futex, &sh->go, FUTEX_WAIT, go, NULL, NULL, 0);
			/* While the parent selects and replaces: some source's page, at the right offset. */
			do {
				for (size_t pg = 0; pg < pages; pg++) {
					uint64_t v = *(const volatile uint64_t *)(p + pg * 4096);

					if ((uint32_t)v != pg || (v >> 32 & 0xff) >= nsrc)
						__atomic_fetch_add(&sh->loose_errors, 1, __ATOMIC_RELAXED);
				}
			} while (__atomic_load_n(&sh->busy, __ATOMIC_ACQUIRE));
			/* Then: no entry of a replaced file is left anywhere. */
			for (size_t pg = 0; pg < pages; pg++) {
				uint64_t v = *(const volatile uint64_t *)(p + pg * 4096);
				uint8_t s = sh->expect[pg];

				if (v != ((uint64_t)sh->gen[s] << 40 | (uint64_t)s << 32 | pg)) {
					__atomic_fetch_add(&sh->errors, 1, __ATOMIC_RELAXED);
					break;
				}
			}
			__atomic_fetch_add(&sh->done, 1, __ATOMIC_RELEASE);
		}
		_exit(0);
	}

	for (unsigned round = 1; round <= rounds; round++) {
		uint64_t x = splitmix64(seed ^ round);
		unsigned k = x % nsrc;
		struct mxlfs_view_replace rq = { .source = k };

		__atomic_store_n(&sh->done, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&sh->busy, 1, __ATOMIC_RELAXED);
		__atomic_store_n(&sh->go, round, __ATOMIC_RELEASE);
		syscall(SYS_futex, &sh->go, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
		/* The last range selects k, so some pages select it when it is replaced. */
		for (int j = 0; j < 4; j++) {
			uint64_t y = splitmix64(seed ^ (round * 31 + j));
			struct mxlfs_view_range rr = {
				.source = j == 3 ? k : y % nsrc,
				.offset = ((y >> 8) % pages) * 4096,
				.length = (1 + (y >> 24) % (j == 2 ? pages : 8)) * 4096,
			};
			size_t first = rr.offset / 4096, last = first + rr.length / 4096;

			if (last > pages)
				last = pages;
			if (ioctl(view, MXLFS_IOC_VIEW_SELECT_RANGE, &rr))
				die("MXLFS_IOC_VIEW_SELECT_RANGE");
			for (size_t pg = first; pg < last; pg++)
				sh->expect[pg] = rr.source;
		}
		for (size_t pg = 0; pg < pages; pg++)
			if (sh->expect[pg] == k) {
				selected_at_replace++;
				break;
			}
		/* The new file takes the old one's name; the view held its last reference. */
		snprintf(tmp, sizeof(tmp), "%s/xnew", dir);
		snprintf(path, sizeof(path), "%s/xsrc%u", dir, k);
		rq.fd = marked_file(tmp, size, sh->gen[k] + 1, k);
		if (ioctl(view, MXLFS_IOC_VIEW_REPLACE_SOURCE, &rq))
			die("MXLFS_IOC_VIEW_REPLACE_SOURCE");
		close(rq.fd);
		if (rename(tmp, path))
			die("rename");
		sh->gen[k]++;
		for (size_t pg = 0; pg < pages; pg += 97) {
			uint64_t v;
			uint8_t s = sh->expect[pg];

			if (pread(view, &v, sizeof(v), pg * 4096) != sizeof(v) ||
			    v != ((uint64_t)sh->gen[s] << 40 | (uint64_t)s << 32 | pg))
				pread_errors++;
		}
		__atomic_store_n(&sh->busy, 0, __ATOMIC_RELEASE);
		while (__atomic_load_n(&sh->done, __ATOMIC_ACQUIRE) < readers)
			usleep(50);
	}
	while (wait(&status) > 0)
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			sh->crashed++;
	if (ioctl(view, MXLFS_IOC_STATS, &s1))
		die("MXLFS_IOC_STATS");
	printf("replace readers=%u rounds=%u errors=%u loose_errors=%u crashed=%u pread_errors=%" PRIu64
	       " refusals_failed=%u view_replaces=%llu selected_at_replace=%u\n", readers, rounds, sh->errors,
	       sh->loose_errors, sh->crashed, pread_errors, refusals,
	       (unsigned long long)(s1.view_replaces - s0.view_replaces), selected_at_replace);
	return (sh->errors || sh->loose_errors || sh->crashed || pread_errors || refusals ||
		s1.view_replaces - s0.view_replaces != rounds || selected_at_replace != rounds) ? 3 : 0;
}

/*
 * MXLFS_IOC_VIEW_SET_SOURCES against a truncate of the view. Sizes set
 * by truncate allocate nothing, so the view is as large as a view may be,
 * which gives it the largest table and the call its longest run. A view
 * the call made with another size than its source's would map pages its
 * source does not have; one made past MXLFS_VIEW_MAX_SIZE would exceed
 * the bound on its table.
 */
int cmd_viewrace(const char *dir, unsigned rounds)
{
	const off_t size = MXLFS_VIEW_MAX_SIZE;
	struct mxlfs_view_sources req = { .count = 1 };
	unsigned set = 0, refused = 0, wrong = 0;
	char path[PATH_MAX], src[PATH_MAX];
	volatile uint32_t *stop;
	int status;
	pid_t pid;

	stop = mmap(NULL, sizeof(*stop), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (stop == MAP_FAILED)
		die("mmap shared");
	snprintf(src, sizeof(src), "%s/vrace-src", dir);
	snprintf(path, sizeof(path), "%s/vrace", dir);
	unlink(src);
	req.fds[0] = open(src, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (req.fds[0] < 0 || ftruncate(req.fds[0], size))
		die(src);
	pid = fork();
	if (pid < 0)
		die("fork");
	if (!pid) {
		/* By name: each round's view is a new file. */
		while (!__atomic_load_n(stop, __ATOMIC_RELAXED)) {
			(void)!truncate(path, size + 4096);
			(void)!truncate(path, size);
		}
		_exit(0);
	}
	for (unsigned r = 0; r < rounds; r++) {
		struct stat st;
		int v;

		unlink(path);
		v = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (v < 0 || ftruncate(v, size))
			die(path);
		if (!ioctl(v, MXLFS_IOC_VIEW_SET_SOURCES, &req)) {
			set++;
			if (fstat(v, &st))
				die(path);
			if (st.st_size != size) {
				if (!wrong)
					fprintf(stderr, "viewrace: a view of %lld bytes over a source of %lld\n",
						(long long)st.st_size, (long long)size);
				wrong++;
			}
		} else if (errno == EINVAL) {
			refused++;
		} else {
			die("MXLFS_IOC_VIEW_SET_SOURCES");
		}
		close(v);
	}
	*stop = 1;
	if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
		die("truncating child");
	unlink(path);
	close(req.fds[0]);
	unlink(src);
	printf("viewrace rounds=%u set=%u refused=%u wrong_size=%u\n", rounds, set, refused, wrong);
	return wrong ? 3 : 0;
}

/* The writer of cmd_writerace: spins for its round's delay, then writes. */
struct writerace {
	uint32_t go, delay;
	int fd, err;
	off_t at;
};

static void *writerace_run(void *arg)
{
	struct writerace *w = arg;
	char page[4096] = { 1 };

	while (!__atomic_load_n(&w->go, __ATOMIC_ACQUIRE))
		;
	for (volatile uint32_t i = 0; i < w->delay; i++)
		;
	w->err = pwrite(w->fd, page, sizeof(page), w->at) == sizeof(page) ? 0 : errno;
	return NULL;
}

/*
 * MXLFS_IOC_VIEW_SET_SOURCES against a write that extends the file. One
 * of them must lose: a write that went first leaves pages and another
 * size, so the call fails; a view made first refuses the write. Were both
 * to succeed, the view would be larger than its table of sources. The
 * writer's delay sweeps over the rounds, so the two meet at every point
 * of the call.
 */
int cmd_writerace(const char *dir, unsigned rounds)
{
	const off_t size = 65536;
	struct mxlfs_view_sources req = { .count = 1 };
	unsigned set = 0, written = 0, both = 0;
	char path[PATH_MAX], src[PATH_MAX];

	snprintf(src, sizeof(src), "%s/wrace-src", dir);
	snprintf(path, sizeof(path), "%s/wrace", dir);
	unlink(src);
	req.fds[0] = open(src, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (req.fds[0] < 0 || ftruncate(req.fds[0], size))
		die(src);
	for (unsigned r = 0; r < rounds; r++) {
		struct writerace w = { .delay = r % 4096 * 4, .at = size };
		pthread_t thread;
		struct stat st;
		int ok;

		unlink(path);
		w.fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (w.fd < 0 || ftruncate(w.fd, size))
			die(path);
		if (pthread_create(&thread, NULL, writerace_run, &w))
			die("pthread_create");
		__atomic_store_n(&w.go, 1, __ATOMIC_RELEASE);
		ok = !ioctl(w.fd, MXLFS_IOC_VIEW_SET_SOURCES, &req);
		if (!ok && errno != EINVAL && errno != EBUSY)
			die("MXLFS_IOC_VIEW_SET_SOURCES");
		pthread_join(thread, NULL);
		if (w.err && w.err != EPERM) {
			errno = w.err;
			die("pwrite");
		}
		set += ok;
		written += !w.err;
		if (ok && !w.err) {
			if (fstat(w.fd, &st))
				die(path);
			if (!both++)
				fprintf(stderr, "writerace: a view of %lld bytes over a source of %lld\n",
					(long long)st.st_size, (long long)size);
		}
		close(w.fd);
	}
	unlink(path);
	close(req.fds[0]);
	unlink(src);
	printf("writerace rounds=%u set=%u written=%u both=%u\n", rounds, set, written, both);
	return both ? 3 : 0;
}

/* Bytes of the page at off of fd that are not zero, or -1 if the read fails. */
static int nonzero_bytes(int fd, off_t off)
{
	uint8_t page[4096];
	int n = 0;

	if (pread(fd, page, sizeof(page), off) != sizeof(page))
		return -1;
	for (size_t i = 0; i < sizeof(page); i++)
		n += page[i] != 0;
	return n;
}

/*
 * A page read_folio refuses at size= stays in the page cache, not
 * uptodate, with whatever the allocator left in it; the core never shows
 * it. Memory filled with a pattern and freed just before makes stale
 * contents likely to be that pattern rather than zeroes. Needs the mount
 * to itself and a size= well below free memory.
 */
int cmd_stale(const char *dir)
{
	const size_t size = 4 << 20, dirty = 64 << 20;
	char path[PATH_MAX], src_path[PATH_MAX], fill[PATH_MAX], over[PATH_MAX], one_path[PATH_MAX];
	int src, view, fd, read_nonzero, fold_nonzero, status, refused, folded, one, one_view;
	int one_nonzero, one_pages = 0, one_enospc, one_boundary;
	struct mxlfs_stats st = { 0 };
	struct stat fst;
	char sig[16];
	__u64 pages[64] = { 0 };
	struct mxlfs_node_pages np = { .pages_ptr = (uintptr_t)pages, .nr_nodes = 64 };
	const char *fault, *one_fault;
	struct statfs sf;
	uint8_t *p;
	pid_t pid;

	p = mmap(NULL, dirty, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		die("mmap");
	memset(p, 0xa5, dirty);
	munmap(p, dirty);

	snprintf(src_path, sizeof(src_path), "%s/stale-src", dir);
	src = open(src_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (src < 0 || ftruncate(src, size))
		die(src_path);
	view = new_view(dir, "stale-view", size, &src, 1);
	/* One page: ALLOCATE has nothing to fold there. */
	snprintf(one_path, sizeof(one_path), "%s/stale-one", dir);
	one = open(one_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (one < 0 || ftruncate(one, 4096))
		die(one_path);
	one_view = new_view(dir, "stale-one-view", 4096, &one, 1);

	/* Up to size=, then one page past it, which read_folio still takes. */
	snprintf(fill, sizeof(fill), "%s/stale-fill", dir);
	snprintf(over, sizeof(over), "%s/stale-over", dir);
	fd = open(fill, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0 || fstatfs(fd, &sf) || posix_fallocate(fd, 0, (off_t)sf.f_bavail * sf.f_bsize))
		die(fill);
	close(fd);
	fd = open(over, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0 || ftruncate(fd, 4096) || nonzero_bytes(fd, 0) < 0)
		die(over);
	close(fd);

	refused = nonzero_bytes(src, 0) < 0 && errno == ENOSPC;
	refused &= nonzero_bytes(one, 0) < 0 && errno == ENOSPC;
	read_nonzero = nonzero_bytes(view, 0);

	p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, view, 0);
	if (p == MAP_FAILED)
		die("mmap view");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (!pid) {
		int n = 0;

		for (size_t i = 0; i < 4096; i++)
			n += ((volatile uint8_t *)p)[i] != 0;
		_exit(n ? 2 : 0);
	}
	if (waitpid(pid, &status, 0) != pid)
		die("waitpid");
	fault = WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS ? "sigbus" :
		WIFEXITED(status) && WEXITSTATUS(status) == 2 ? "stale" : "zero";
	munmap(p, 4096);

	/*
	 * A refused page is charged to size= while in the page cache. Past
	 * the limit ALLOCATE refuses to fill it; at the limit exactly, where
	 * read_folio would still take it, ALLOCATE fills it.
	 */
	one_enospc = ioctl(one, MXLFS_IOC_ALLOCATE) && errno == ENOSPC;
	if (ioctl(one, MXLFS_IOC_STATS, &st))
		die("MXLFS_IOC_STATS");
	fd = open(fill, O_RDWR | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &fst) ||
	    ftruncate(fd, fst.st_size - (off_t)(st.bytes_allocated - st.bytes_limit)))
		die(fill);
	close(fd);
	if (ioctl(one, MXLFS_IOC_STATS, &st))
		die("MXLFS_IOC_STATS");
	one_boundary = st.bytes_allocated == st.bytes_limit && !ioctl(one, MXLFS_IOC_ALLOCATE);
	unlink(fill);
	unlink(over);
	if (ioctl(src, MXLFS_IOC_ALLOCATE))
		die("MXLFS_IOC_ALLOCATE");
	/* The fold must have run, or read_folio filled the page instead. */
	folded = pmd_folio(src, 0);
	fold_nonzero = nonzero_bytes(src, 0);
	one_nonzero = nonzero_bytes(one_view, 0);
	if (ioctl(one, MXLFS_IOC_NODE_PAGES, &np))
		die("MXLFS_IOC_NODE_PAGES");
	for (__u32 i = 0; i < np.nr_nodes && i < 64; i++)
		one_pages += pages[i];
	p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, one_view, 0);
	if (p == MAP_FAILED)
		die("mmap view");
	pid = fork();
	if (pid < 0)
		die("fork");
	if (!pid) {
		int n = 0;

		for (size_t i = 0; i < 4096; i++)
			n += ((volatile uint8_t *)p)[i] != 0;
		_exit(n ? 2 : 0);
	}
	if (waitpid(pid, &status, 0) != pid)
		die("waitpid");
	snprintf(sig, sizeof(sig), "signal%d", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
	one_fault = WIFSIGNALED(status) ? (WTERMSIG(status) == SIGBUS ? "sigbus" : sig) :
		    WEXITSTATUS(status) == 2 ? "stale" : WEXITSTATUS(status) ? "error" : "zero";
	munmap(p, 4096);

	printf("stale refused=%d view_read_nonzero=%d view_fault=%s folded=%d fold_nonzero=%d "
	       "one_enospc=%d one_boundary=%d one_read_nonzero=%d one_fault=%s one_pages=%d\n", refused,
	       read_nonzero, fault, folded, fold_nonzero, one_enospc, one_boundary, one_nonzero, one_fault,
	       one_pages);
	close(view);
	close(src);
	close(one_view);
	close(one);
	snprintf(path, sizeof(path), "%s/stale-view", dir);
	unlink(path);
	snprintf(path, sizeof(path), "%s/stale-one-view", dir);
	unlink(path);
	unlink(src_path);
	unlink(one_path);
	return refused && !read_nonzero && !strcmp(fault, "sigbus") && folded && !fold_nonzero &&
	       one_enospc && one_boundary && !one_nonzero && !strcmp(one_fault, "zero") &&
	       one_pages == 1 ? 0 : 3;
}

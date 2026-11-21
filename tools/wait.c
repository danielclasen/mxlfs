#include "harness.h"

int cmd_wanted(const char *dir, unsigned timeout_ms, int fail)
{
	struct mxlfs_wanted_req req = { .timeout_ms = timeout_ms };
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		die(dir);
	if (ioctl(fd, MXLFS_IOC_WANTED_NEXT, &req)) {
		if (errno == ETIMEDOUT)
			return printf("wanted none\n"), 1;
		die("MXLFS_IOC_WANTED_NEXT");
	}
	printf("wanted id=%llu pid=%u path=%s\n", (unsigned long long)req.id, req.pid, req.path);
	if (fail) {
		/*
		 * Input WANTED_FAIL refuses, for an id nobody waits for: what
		 * it takes reaches the lookup and fails with ENOENT instead.
		 * 134 is EFTYPE, which 7.3 defines past EHWPOISON.
		 */
		static const struct {
			__s32 error;
			__u32 pad;
			int err;
		} cases[] = {
			{ EACCES, 0, ENOENT }, { 134, 0, ENOENT }, { 511, 0, ENOENT },
			{ 0, 0, EINVAL }, { -EACCES, 0, EINVAL }, { 512, 0, EINVAL },
			{ EACCES, 1, EINVAL },
		};
		struct mxlfs_wanted_fail f = { .id = req.id, .error = fail };
		unsigned wrong = 0;

		for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			struct mxlfs_wanted_fail b = { .id = ~0ULL, .error = cases[i].error, .pad = cases[i].pad };

			if (!ioctl(fd, MXLFS_IOC_WANTED_FAIL, &b) || errno != cases[i].err) {
				fprintf(stderr, "wanted: WANTED_FAIL error=%d pad=%u: got %s, want %s\n",
					cases[i].error, cases[i].pad, strerror(errno), strerror(cases[i].err));
				wrong++;
			}
		}
		printf("wanted bad_input_wrong=%u\n", wrong);

		if (ioctl(fd, MXLFS_IOC_WANTED_FAIL, &f))
			die("MXLFS_IOC_WANTED_FAIL");
	}
	return 0;
}

int cmd_wantfail(const char *dir, uint64_t id, int error)
{
	struct mxlfs_wanted_fail f = { .id = id, .error = error };
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		die(dir);
	printf("wantfail rc=%s\n", ioctl(fd, MXLFS_IOC_WANTED_FAIL, &f) ? strerror(errno) : "0");
	close(fd);
	return 0;
}

/*
 * openat2 of PATH below DIR with RESOLVE_NO_XDEV, which a mount crossing
 * fails with EXDEV; DIR is opened without it, so the walk starts inside
 * DIR's mount.
 */
int cmd_noxdev(const char *dir, const char *path)
{
	struct open_how how = { .flags = O_PATH | O_CLOEXEC, .resolve = RESOLVE_NO_XDEV };
	int dfd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
	uint64_t t = now_ns();
	long fd;
	int err;

	if (dfd < 0)
		die(dir);
	fd = syscall(SYS_openat2, dfd, path, &how, sizeof(how));
	err = fd < 0 ? errno : 0;

	printf("noxdev %s %.3f\n", err ? strerror(err) : "ok", (now_ns() - t) / 1e9);
	if (fd >= 0)
		close(fd);
	close(dfd);
	return 0;
}

/* A path walk of cmd_wantcheck: stats name/data in dir. */
struct wantstat {
	pthread_t thread;
	int dir;
	char name[NAME_MAX + 1];
	int err;
};

static void *wantstat_run(void *arg)
{
	struct wantstat *w = arg;
	char path[NAME_MAX + 6];
	struct stat st;

	snprintf(path, sizeof(path), "%s/data", w->name);
	w->err = fstatat(w->dir, path, &st, 0) ? errno : 0;
	return NULL;
}

/*
 * Takes the next wanted flow, which must be the one at path, fails its
 * waiters with ENOENT and joins the walk w. Counts a mismatch in *wrong.
 */
static void wantcheck_take(int fd, struct wantstat *w, const char *path, unsigned *wrong)
{
	struct mxlfs_wanted_req req = { .timeout_ms = 1000 };
	struct mxlfs_wanted_fail f = { .error = ENOENT };
	int r;

	r = ioctl(fd, MXLFS_IOC_WANTED_NEXT, &req);
	if (r || strcmp(req.path, path)) {
		fprintf(stderr, "wantcheck: WANTED_NEXT for %.40s...: %s, path of %zu bytes\n", path,
			r ? strerror(errno) : "success", strnlen(req.path, sizeof(req.path)));
		++*wrong;
	} else {
		f.id = req.id;
		if (ioctl(fd, MXLFS_IOC_WANTED_FAIL, &f))
			die("MXLFS_IOC_WANTED_FAIL");
	}
	pthread_join(w->thread, NULL);
}

/*
 * Makes nested directories of tag under the root fd so that a flow name
 * in the last one has a path from the root of len bytes, and returns that
 * directory. Components of 200 bytes leave the name 10 to 210 bytes long.
 */
static int wantdeep(int root, char tag, size_t len, char name[NAME_MAX + 1], char *path)
{
	char comp[201];
	size_t at = 0, n;
	int fd = dup(root);

	memset(comp, tag, 200);
	comp[200] = 0;
	while (len - at - 1 > 210) {
		int sub;

		if (mkdirat(fd, comp, 0755))
			die("mkdirat");
		sub = openat(fd, comp, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (sub < 0)
			die("openat");
		close(fd);
		fd = sub;
		path[at++] = '/';
		memcpy(path + at, comp, 200);
		at += 200;
	}
	n = len - at - 1;
	memset(name, tag, n - 9);
	memcpy(name + n - 9, ".mxl-flow", 10);
	path[at++] = '/';
	memcpy(path + at, name, n + 1);
	return fd;
}

/*
 * On the root of a mount with wait= and nothing waited for: WANTED_NEXT
 * refuses a flags bit before it waits; a flow whose copy to the caller
 * fails, into a struct the module can read but not write, is reported to
 * the next call; a path of the longest length path holds is waited for
 * and reported in full, and a walk through a longer one, of a byte more
 * or of PATH_MAX - 1, fails with ENAMETOOLONG well within wait= (WAIT_MS)
 * and leaves no flow wanted.
 */
int cmd_wantcheck(const char *root, unsigned wait_ms)
{
	int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	struct wantstat w = { .dir = fd, .name = "wantcheck-efault.mxl-flow" };
	static const __u32 bad_flags[] = { 1, 1u << 31, ~0u };
	const size_t longest = sizeof(((struct mxlfs_wanted_req *)0)->path) - 1;
	const size_t toolong[] = { longest + 1, PATH_MAX - 1 };
	unsigned flags_wrong = 0, efault_lost = 0, toolong_wrong = 0;
	struct mxlfs_stats stats = { 0 };
	struct mxlfs_wanted_req *ro;
	char path[PATH_MAX];

	if (fd < 0)
		die(root);
	for (size_t i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); i++) {
		struct mxlfs_wanted_req req = { .timeout_ms = 100, .flags = bad_flags[i] };

		int r = ioctl(fd, MXLFS_IOC_WANTED_NEXT, &req);

		if (!r || errno != EINVAL) {
			fprintf(stderr, "wantcheck: WANTED_NEXT flags=%#x: %s, want EINVAL\n",
				bad_flags[i], r ? strerror(errno) : "success");
			flags_wrong++;
		}
	}
	ro = mmap(NULL, sizeof(*ro), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ro == MAP_FAILED)
		die("mmap");
	ro->timeout_ms = 1000;
	if (mprotect(ro, sizeof(*ro), PROT_READ))
		die("mprotect");
	if (pthread_create(&w.thread, NULL, wantstat_run, &w))
		die("pthread_create");
	if (!ioctl(fd, MXLFS_IOC_WANTED_NEXT, ro) || errno != EFAULT) {
		fprintf(stderr, "wantcheck: WANTED_NEXT into a read-only struct: %s, want EFAULT\n",
			strerror(errno));
		efault_lost++;
	}
	wantcheck_take(fd, &w, "/wantcheck-efault.mxl-flow", &efault_lost);
	if (w.err != ENOENT)
		efault_lost++;
	munmap(ro, sizeof(*ro));

	w.dir = wantdeep(fd, 'a', longest, w.name, path);
	if (pthread_create(&w.thread, NULL, wantstat_run, &w))
		die("pthread_create");
	wantcheck_take(fd, &w, path, &toolong_wrong);
	if (w.err != ENOENT)
		toolong_wrong++;
	close(w.dir);

	for (size_t i = 0; i < sizeof(toolong) / sizeof(toolong[0]); i++) {
		struct stat st;
		uint64_t t, ms;
		int dir, err;

		dir = wantdeep(fd, 'b' + i, toolong[i], w.name, path);
		strcat(w.name, "/data");
		t = now_ns();
		err = fstatat(dir, w.name, &st, 0) ? errno : 0;
		ms = (now_ns() - t) / 1000000;
		printf("wantcheck walk of %zu bytes: %s after %llu ms\n", toolong[i], strerror(err),
		       (unsigned long long)ms);
		if (err != ENAMETOOLONG || ms >= wait_ms / 4) {
			fprintf(stderr, "wantcheck: a walk of %zu bytes: %s after %llu ms, want "
				"ENAMETOOLONG within %u ms\n", toolong[i], strerror(err),
				(unsigned long long)ms, wait_ms / 4);
			toolong_wrong++;
		}
		close(dir);
	}
	stats_of(fd, &stats);
	if (stats.wanted) {
		fprintf(stderr, "wantcheck: %llu flows wanted after the walks\n",
			(unsigned long long)stats.wanted);
		toolong_wrong++;
	}
	close(fd);
	printf("wantcheck flags_wrong=%u efault_lost=%u toolong_wrong=%u\n", flags_wrong,
	       efault_lost, toolong_wrong);
	return flags_wrong || efault_lost || toolong_wrong ? 3 : 0;
}

/* A path walk of cmd_wantrace: waits for go, then stats its flow. */
struct wantwalk {
	pthread_t thread;
	const uint32_t *go;
	char path[PATH_MAX];
	int err;
	uint32_t done;
	uint64_t ns;
};

static void *wantwalk_run(void *arg)
{
	struct wantwalk *w = arg;
	struct stat st;
	uint64_t t;

	/* Spinning, so walks released together enter the kernel together. */
	while (!__atomic_load_n(w->go, __ATOMIC_ACQUIRE))
		;
	t = now_ns();
	w->err = stat(w->path, &st) ? errno : 0;
	w->ns = now_ns() - t;
	__atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
	return NULL;
}

static void wantwalk_start(struct wantwalk *w, const char *dir, const char *name,
			   const uint32_t *go)
{
	pthread_attr_t attr;

	w->go = go;
	w->done = 0;
	snprintf(w->path, sizeof(w->path), "%s/%s.mxl-flow/data", dir, name);
	if (pthread_attr_init(&attr) || pthread_attr_setstacksize(&attr, 1 << 16) ||
	    pthread_create(&w->thread, &attr, wantwalk_run, w))
		die("pthread_create");
	pthread_attr_destroy(&attr);
}

/* Joins a walk its flow must have woken; one still waiting fails the test. */
static void wantwalk_join(struct wantwalk *w)
{
	uint64_t end = now_ns() + 5000000000ULL;

	while (!__atomic_load_n(&w->done, __ATOMIC_ACQUIRE)) {
		if (now_ns() > end) {
			fprintf(stderr, "wantrace: %s exists and a walk through it still waits\n", w->path);
			exit(3);
		}
		usleep(100);
	}
	pthread_join(w->thread, NULL);
}

/*
 * Makes the flow name in dir complete, as libmxl does, under a temporary
 * name renamed into place: a walk woken by the name must find its file.
 * Without make, removes it.
 */
static void wantflow(const char *dir, const char *name, bool make)
{
	char path[PATH_MAX], tmp[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "%s/%s.mxl-flow", dir, name);
	if (!make) {
		strcat(path, "/data");
		if (unlink(path))
			die(path);
		*strrchr(path, '/') = 0;
		if (rmdir(path))
			die(path);
		return;
	}
	snprintf(tmp, sizeof(tmp), "%s/.mxl-tmp-%s", dir, name);
	if (mkdir(tmp, 0755))
		die(tmp);
	strcat(tmp, "/data");
	fd = open(tmp, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
	if (fd < 0)
		die(tmp);
	close(fd);
	*strrchr(tmp, '/') = 0;
	if (rename(tmp, path))
		die(path);
}

static struct mxlfs_stats wantstats(int fd)
{
	struct mxlfs_stats s = { 0 };

	stats_of(fd, &s);
	return s;
}

/* Waits up to 60 s for the mount to wait for at least n and at most max flows. */
static uint64_t wanted_within(int fd, uint64_t n, uint64_t max)
{
	uint64_t end = now_ns() + 60000000000ULL, have;

	while ((have = wantstats(fd).wanted) < n || have > max) {
		if (now_ns() > end) {
			fprintf(stderr, "wantrace: timeout wanted=%llu, want %llu to %llu\n",
				(unsigned long long)have, (unsigned long long)n,
				(unsigned long long)max);
			exit(3);
		}
		usleep(1000);
	}
	return have;
}

static void wanted_reaches(int fd, uint64_t n)
{
	wanted_within(fd, n, n);
}

/*
 * A walk that misses a flow drops wanted_lock to allocate its entry and
 * looks again: walks through the same flow must end up in one entry, and
 * a cap another walk fills meanwhile must still hold. Released together,
 * the walks of a round take that path when they miss within the time an
 * allocation takes; which interleaving a round gets is not observable,
 * only that every outcome is right.
 */
int cmd_wantrace(const char *dir, unsigned rounds)
{
	/* BASE: one short of the 256 flows a thread group may wait for. */
	enum { SAME = 3, BASE = 255 };
	struct wantwalk *base = zalloc(BASE, sizeof(*base)), w[SAME];
	unsigned dup = 0, same_failed = 0, cap_wrong = 0;
	uint64_t refused0, refused;
	char name[64];
	uint32_t go;
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		die(dir);
	wanted_reaches(fd, 0);
	refused0 = wantstats(fd).wanted_refused;
	for (unsigned r = 0; r < rounds; r++) {
		snprintf(name, sizeof(name), "s%u", r);
		__atomic_store_n(&go, 0, __ATOMIC_RELAXED);
		for (int i = 0; i < SAME; i++)
			wantwalk_start(&w[i], dir, name, &go);
		__atomic_store_n(&go, 1, __ATOMIC_RELEASE);
		wanted_within(fd, 1, UINT64_MAX);
		usleep(20000);
		if (wantstats(fd).wanted != 1)
			dup++;
		wantflow(dir, name, true);
		for (int i = 0; i < SAME; i++) {
			wantwalk_join(&w[i]);
			if (w[i].err && !same_failed++)
				fprintf(stderr, "wantrace: %s: %s\n", w[i].path, strerror(w[i].err));
		}
		wantflow(dir, name, false);
		wanted_reaches(fd, 0);
	}

	__atomic_store_n(&go, 1, __ATOMIC_RELAXED);
	for (int i = 0; i < BASE; i++) {
		snprintf(name, sizeof(name), "b%d", i);
		wantwalk_start(&base[i], dir, name, &go);
	}
	wanted_reaches(fd, BASE);
	/* A wrong round ends the rounds: both walks then wait for wait=. */
	for (unsigned r = 0; r < rounds && !cap_wrong; r++) {
		uint64_t end = now_ns() + 5000000000ULL;
		unsigned done = 0, in = 0, out = 0;

		__atomic_store_n(&go, 0, __ATOMIC_RELAXED);
		for (int i = 0; i < 2; i++) {
			snprintf(name, sizeof(name), "c%u-%d", r, i);
			wantwalk_start(&w[i], dir, name, &go);
		}
		__atomic_store_n(&go, 1, __ATOMIC_RELEASE);
		while (!__atomic_load_n(&w[0].done, __ATOMIC_ACQUIRE) &&
		       !__atomic_load_n(&w[1].done, __ATOMIC_ACQUIRE) && now_ns() < end)
			usleep(100);
		usleep(20000);
		for (int i = 0; i < 2; i++)
			done += __atomic_load_n(&w[i].done, __ATOMIC_ACQUIRE);
		if (done != 1 || wantstats(fd).wanted != BASE + 1)
			cap_wrong++;
		for (int i = 0; i < 2; i++) {
			snprintf(name, sizeof(name), "c%u-%d", r, i);
			wantflow(dir, name, true);
		}
		for (int i = 0; i < 2; i++) {
			wantwalk_join(&w[i]);
			in += w[i].err == 0;
			out += w[i].err == ENOENT && w[i].ns < 200000000;
		}
		if (in != 1 || out != 1)
			cap_wrong++;
		for (int i = 0; i < 2; i++) {
			snprintf(name, sizeof(name), "c%u-%d", r, i);
			wantflow(dir, name, false);
		}
		wanted_reaches(fd, BASE);
	}
	refused = wantstats(fd).wanted_refused - refused0;
	for (int i = 0; i < BASE; i++) {
		snprintf(name, sizeof(name), "b%d", i);
		wantflow(dir, name, true);
	}
	for (int i = 0; i < BASE; i++) {
		wantwalk_join(&base[i]);
		cap_wrong += base[i].err != 0;
		snprintf(name, sizeof(name), "b%d", i);
		wantflow(dir, name, false);
	}
	wanted_reaches(fd, 0);
	printf("wantrace rounds=%u same_dup=%u same_failed=%u cap_wrong=%u refused=%llu\n", rounds, dup,
	       same_failed, cap_wrong, (unsigned long long)refused);
	free(base);
	close(fd);
	return dup || same_failed || cap_wrong || refused != rounds ? 3 : 0;
}

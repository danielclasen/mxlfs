#include "harness.h"

/*
 * Sizes the open file fd to size bytes with fallocate while the thread
 * group allocates lazily, so its pages come 4 KiB at a time as they are
 * touched; a file's laziness is set by that fallocate, not by the flag
 * later.
 */
void lazy_size(int fd, off_t size)
{
	__u32 on = 1;

	if (ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on))
		die("MXLFS_IOC_LAZY_ALLOC");
	errno = posix_fallocate(fd, 0, size);
	if (errno)
		die("posix_fallocate");
	on = 0;
	if (ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on))
		die("MXLFS_IOC_LAZY_ALLOC");
}

/* A lazily sized file of size bytes at dir/name, open for writing. */
static int lazy_file(const char *dir, const char *name, size_t size)
{
	char path[PATH_MAX];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0)
		die(path);
	lazy_size(fd, size);
	return fd;
}

/*
 * A fold copies small folios into a large one; a store through a
 * get_user_pages reference, which needs no mapping and no folio lock,
 * would be lost if it landed after the copy. vmsplice holds such a
 * reference in the pipe until the pipe is read.
 */
int cmd_foldhold(const char *dir)
{
	const size_t size = 4 << 20;
	int folded[2];

	for (int k = 0; k < 2; k++) {
		char name[32], page[4096];
		struct iovec iov;
		uint64_t v = 1;
		uint8_t *m;
		int fd, p[2];

		snprintf(name, sizeof(name), "fhold%d", k);
		fd = lazy_file(dir, name, size);
		m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (m == MAP_FAILED || pipe(p))
			die("mmap");
		memcpy(m, &v, sizeof(v));
		iov = (struct iovec){ .iov_base = m, .iov_len = 4096 };
		if (vmsplice(p[1], &iov, 1, 0) != 4096)
			die("vmsplice");
		munmap(m, 4096);
		if (k && read(p[0], page, sizeof(page)) != sizeof(page))
			die("read pipe");
		if (ioctl(fd, MXLFS_IOC_ALLOCATE))
			die("MXLFS_IOC_ALLOCATE");
		folded[k] = pmd_folio(fd, 0);
		close(p[0]);
		close(p[1]);
		close(fd);
		snprintf(page, sizeof(page), "%s/%s", dir, name);
		unlink(page);
	}
	printf("foldhold held_folded=%d released_folded=%d\n", folded[0], folded[1]);
	return folded[0] || !folded[1] ? 3 : 0;
}

/* The writer of cmd_foldmem: stores through /proc/self/mem until stopped. */
struct foldmem {
	int mem;
	uint8_t *map;
	uint32_t stop;
	uint64_t last[4];
};

static const size_t foldmem_pages[4] = { 0, 1, 7, 300 };

static void *foldmem_run(void *arg)
{
	struct foldmem *f = arg;

	for (uint64_t v = 1; !__atomic_load_n(&f->stop, __ATOMIC_ACQUIRE); v++) {
		for (int i = 0; i < 4; i++) {
			uint64_t val = v << 16 | (uint64_t)i;
			off_t at = (off_t)(uintptr_t)(f->map + foldmem_pages[i] * 4096);

			if (pwrite(f->mem, &val, sizeof(val), at) == sizeof(val))
				f->last[i] = val;
		}
	}
	return NULL;
}

/*
 * Folds a lazily sized file while a thread writes four of its touched
 * pages through /proc/self/mem; each page must then hold the last value
 * the thread wrote to it. Whether a store lands after a fold's copy is
 * timing; the rounds and the folds among them are reported.
 */
int cmd_foldmem(const char *dir, unsigned rounds)
{
	const size_t size = 4 << 20;
	unsigned lost = 0, folded = 0;
	int mem = open("/proc/self/mem", O_RDWR | O_CLOEXEC);

	if (mem < 0)
		die("/proc/self/mem");
	for (unsigned r = 0; r < rounds; r++) {
		struct foldmem f = { .mem = mem };
		pthread_t thread;
		int fd = lazy_file(dir, "fmem", size);

		f.map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (f.map == MAP_FAILED)
			die("mmap");
		for (int i = 0; i < 4; i++)
			f.map[foldmem_pages[i] * 4096] = 1;
		if (pthread_create(&thread, NULL, foldmem_run, &f))
			die("pthread_create");
		usleep(500 + r % 8 * 250);
		if (ioctl(fd, MXLFS_IOC_ALLOCATE))
			die("MXLFS_IOC_ALLOCATE");
		usleep(500);
		__atomic_store_n(&f.stop, 1, __ATOMIC_RELEASE);
		pthread_join(thread, NULL);
		for (int i = 0; i < 4; i++) {
			uint64_t got;

			if (pread(fd, &got, sizeof(got), foldmem_pages[i] * 4096) != sizeof(got))
				die("pread");
			if (got != f.last[i] && !lost++)
				fprintf(stderr, "foldmem: round %u page %zu holds %llx, last written %llx\n", r,
					foldmem_pages[i], (unsigned long long)got,
					(unsigned long long)f.last[i]);
		}
		munmap(f.map, size);
		folded += pmd_folio(fd, 0);
		close(fd);
	}
	printf("foldmem rounds=%u folded=%u lost=%u\n", rounds, folded, lost);
	return lost ? 3 : 0;
}

/*
 * A view source whose file was sized lazily and touched in a few pages,
 * so those pages are small folios. Each round a new such file replaces
 * the source, READERS processes drop and refault their mapping of the
 * view in a loop, and the parent allocates the file, which folds the
 * small folios into large ones (mxlfs_fold) unless a view maps one; in
 * the first round before the readers fault, so that one folds. A
 * fold must not take a folio from under a view's mapping: the kernel
 * reports a page cache folio still mapped (dmesg), and a reader keeps
 * seeing it. Then the parent writes new markers into the file and every
 * reader must see them through the view. Prints how many rounds folded.
 */
int cmd_fold(const char *dir, unsigned readers, unsigned rounds)
{
	const size_t size = 4 << 20;
	static const size_t touched[] = { 0, 1, 2, 7, 64, 300, 511, 512, 513, 700 };
	const size_t ntouched = sizeof(touched) / sizeof(touched[0]);
	struct shared {
		uint32_t round, phase, done, errors;
	} *sh;
	char path[PATH_MAX], tmp[PATH_MAX];
	unsigned folded = 0, crashed = 0;
	int src[2], view, status;
	pid_t *pids = zalloc(readers, sizeof(*pids));

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		die("mmap");
	snprintf(path, sizeof(path), "%s/fsrc1", dir);
	src[1] = marked_file(path, size, 0, 1);
	/* The lazy source of round r carries 2r << 40 in its touched pages. */
	snprintf(tmp, sizeof(tmp), "%s/fnew", dir);
	snprintf(path, sizeof(path), "%s/fsrc0", dir);
	for (unsigned round = 0; round <= rounds; round++) {
		uint64_t gen = 2 * round;
		int fd;

		unlink(tmp);
		fd = open(tmp, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (fd < 0)
			die(tmp);
		lazy_size(fd, size);
		for (size_t t = 0; t < ntouched; t++) {
			uint64_t v = gen << 40 | touched[t];

			if (pwrite(fd, &v, sizeof(v), touched[t] * 4096) != sizeof(v))
				die("pwrite");
		}

		if (round == 0) {
			src[0] = fd;
			if (rename(tmp, path))
				die("rename");
			view = new_view(dir, "fview", size, src, 2);
			for (unsigned r = 0; r < readers; r++) {
				if ((pids[r] = fork()))
					continue;
				const volatile uint8_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, view, 0);

				if (p == MAP_FAILED)
					die("mmap view");
				for (unsigned rr = 1; rr <= rounds; rr++) {
					uint32_t v;

					while ((v = __atomic_load_n(&sh->round, __ATOMIC_ACQUIRE)) != rr)
						syscall(SYS_futex, &sh->round, FUTEX_WAIT, v, NULL, NULL, 0);
					while (__atomic_load_n(&sh->phase, __ATOMIC_ACQUIRE) == 0) {
						madvise((void *)p, size, MADV_DONTNEED);
						for (size_t t = 0; t < ntouched; t++)
							(void)*(const volatile uint64_t *)(p + touched[t] * 4096);
					}
					/* The entries the last pass installed stay for the check. */
					while (__atomic_load_n(&sh->phase, __ATOMIC_ACQUIRE) != 2)
						usleep(10);
					for (size_t t = 0; t < ntouched; t++) {
						uint64_t want = (2ULL * rr + 1) << 40 | touched[t];

						if (*(const volatile uint64_t *)(p + touched[t] * 4096) != want) {
							__atomic_fetch_add(&sh->errors, 1, __ATOMIC_RELAXED);
							break;
						}
					}
					__atomic_fetch_add(&sh->done, 1, __ATOMIC_RELEASE);
				}
				_exit(0);
			}
			continue;
		}

		struct mxlfs_view_replace rq = { .source = 0, .fd = fd };

		if (ioctl(view, MXLFS_IOC_VIEW_REPLACE_SOURCE, &rq))
			die("MXLFS_IOC_VIEW_REPLACE_SOURCE");
		if (rename(tmp, path))
			die("rename");
		__atomic_store_n(&sh->done, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&sh->phase, 0, __ATOMIC_RELAXED);
		/*
		 * The first round allocates before the readers fault the new
		 * source, so nothing maps a small folio and the fold cannot be
		 * refused; the later rounds race the readers' faults.
		 */
		if (round == 1 && ioctl(fd, MXLFS_IOC_ALLOCATE))
			die("MXLFS_IOC_ALLOCATE");
		__atomic_store_n(&sh->round, round, __ATOMIC_RELEASE);
		syscall(SYS_futex, &sh->round, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
		usleep(200);
		if (round > 1 && ioctl(fd, MXLFS_IOC_ALLOCATE))
			die("MXLFS_IOC_ALLOCATE");
		__atomic_store_n(&sh->phase, 1, __ATOMIC_RELEASE);
		folded += pmd_folio(fd, 0);
		for (size_t t = 0; t < ntouched; t++) {
			uint64_t v = (gen + 1) << 40 | touched[t];

			if (pwrite(fd, &v, sizeof(v), touched[t] * 4096) != sizeof(v))
				die("pwrite");
		}
		__atomic_store_n(&sh->phase, 2, __ATOMIC_RELEASE);
		while (__atomic_load_n(&sh->done, __ATOMIC_ACQUIRE) < readers) {
			/*
			 * Readers end after the last round; one that ended before
			 * failed. A reader that reported the last round exits 0
			 * while the others may still be checking it.
			 */
			if (waitpid(-1, &status, WNOHANG) > 0) {
				if (round == rounds && WIFEXITED(status) && !WEXITSTATUS(status))
					continue;
				fprintf(stderr, "fold: a reader ended in round %u, %s\n", round,
					WIFSIGNALED(status) ? strsignal(WTERMSIG(status)) : "exited");
				crashed++;
				goto out;
			}
			usleep(50);
		}
		close(fd);
	}
out:
	for (unsigned r = 0; crashed && r < readers; r++)
		kill(pids[r], SIGKILL);
	while (wait(&status) > 0)
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			crashed++;
	printf("fold readers=%u rounds=%u folded=%u errors=%u crashed=%u\n", readers, rounds, folded, sh->errors,
	       crashed);
	return (sh->errors || crashed || !folded) ? 3 : 0;
}

/*
 * A fold copies a lazily sized file's small folios into a large one and
 * then drops them. A store through a mapping must land in the large folio
 * whether it happens before, during or after the fold. The child stores
 * into every page in turn, so while the fold walks the range the child
 * refaults pages behind and ahead of it; the parent lets the child finish
 * two whole passes after the fold before it stops it, so every page is
 * read back from the large folio.
 */
int cmd_foldrace(const char *dir, unsigned rounds)
{
	const size_t size = 4 << 20, pages = size / 4096;
	struct shared {
		uint32_t passes, stop;
		uint64_t lost;
	} *sh;
	char path[PATH_MAX];
	unsigned folded = 0, crashed = 0;
	int status;

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		die("mmap");
	snprintf(path, sizeof(path), "%s/foldrace", dir);
	for (unsigned round = 0; round < rounds && !stop; round++) {
		uint32_t at;
		pid_t pid;
		int fd;

		unlink(path);
		fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (fd < 0)
			die(path);
		lazy_size(fd, size);
		__atomic_store_n(&sh->passes, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&sh->stop, 0, __ATOMIC_RELEASE);
		pid = fork();
		if (pid < 0)
			die("fork");
		if (!pid) {
			volatile uint64_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			uint64_t *last = zalloc(pages, sizeof(*last)), seq = 0;

			if (p == MAP_FAILED)
				die("foldrace child");
			while (!__atomic_load_n(&sh->stop, __ATOMIC_ACQUIRE)) {
				for (size_t pg = 0; pg < pages; pg++) {
					volatile uint64_t *w = p + pg * (4096 / sizeof(*p));

					if (*w != last[pg])
						__atomic_fetch_add(&sh->lost, 1, __ATOMIC_RELAXED);
					*w = last[pg] = ++seq;
					if (*w != seq)
						__atomic_fetch_add(&sh->lost, 1, __ATOMIC_RELAXED);
				}
				__atomic_fetch_add(&sh->passes, 1, __ATOMIC_RELEASE);
			}
			_exit(0);
		}
		/* A whole pass first, so every page is a small folio. */
		while (__atomic_load_n(&sh->passes, __ATOMIC_ACQUIRE) < 1)
			usleep(10);
		if (ioctl(fd, MXLFS_IOC_ALLOCATE))
			die("MXLFS_IOC_ALLOCATE");
		at = __atomic_load_n(&sh->passes, __ATOMIC_ACQUIRE);
		while (__atomic_load_n(&sh->passes, __ATOMIC_ACQUIRE) < at + 2) {
			if (waitpid(pid, &status, WNOHANG) == pid) {
				fprintf(stderr, "foldrace: the child ended in round %u\n", round);
				crashed++;
				goto out;
			}
			usleep(50);
		}
		__atomic_store_n(&sh->stop, 1, __ATOMIC_RELEASE);
		if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
			crashed++;
		for (size_t off = 0; off < size; off += 2 << 20)
			folded += pmd_folio(fd, off);
		close(fd);
	}
out:
	unlink(path);
	printf("foldrace rounds=%u folded_spans=%u lost=%" PRIu64 " crashed=%u\n", rounds, folded,
	       sh->lost, crashed);
	return (sh->lost || crashed || !folded) ? 3 : 0;
}

/* MXLFS_IOC_ALLOCATE on PATH; prints the error, or ok, and how many IN_MODIFY events it raised. */
int cmd_allocate(const char *path)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	int fd = open(path, O_RDWR | O_CLOEXEC);
	int err, events = 0;
	ssize_t n;

	if (in < 0 || inotify_add_watch(in, path, IN_MODIFY) < 0)
		die("inotify");
	if (fd < 0)
		die(path);
	err = ioctl(fd, MXLFS_IOC_ALLOCATE) ? errno : 0;
	while ((n = read(in, buf, sizeof(buf))) > 0)
		for (char *p = buf; p < buf + n; p += sizeof(struct inotify_event) +
						     ((struct inotify_event *)p)->len)
			events++;
	printf("allocate %s modify=%d\n", err ? strerror(err) : "ok", events);
	close(fd);
	close(in);
	return 0;
}

int cmd_lazyfile(const char *path, off_t size)
{
	int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);

	if (fd < 0)
		die(path);
	lazy_size(fd, size);
	close(fd);
	return 0;
}

/* Writes a byte to every page of PATH through a shared mapping. */
int cmd_touch(const char *path)
{
	size_t size = 0;
	uint8_t *p = map_file(path, O_RDWR, PROT_READ | PROT_WRITE, &size, NULL);

	for (size_t off = 0; off < size; off += 4096)
		p[off] = 1;
	return 0;
}

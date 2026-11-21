#include "harness.h"

/*
 * Shrinks files across a large folio, which splits it: every folio of
 * mxlfs is dirty, and the split accounts the part it drops as cleaned.
 * Checks the size, the data left, the zeroes past the cut once the file
 * grows back, and the bytes the filesystem still charges for the file,
 * which are the pages kept, whether or not a tail folio went past EOF
 * (huge=always). A file that was
 * only created and fallocated has never been marked dirty as an inode;
 * writing through a mapping marks it when the clock has moved on since
 * its ctime, so only the first cases are certain to be shrunk unmarked.
 */
/* Bytes the filesystem holding fd has allocated. */
static uint64_t allocated(int fd)
{
	struct mxlfs_stats s;

	stats_of(fd, &s);
	return s.bytes_allocated;
}

int cmd_shrink(const char *dir)
{
	enum { FALLOCATED, WRITTEN, MAPPED };
	static const struct {
		size_t size, to;
		int how;
	} cases[] = {
		{ 65536, 4096, FALLOCATED },			/* one 64 KiB folio */
		{ 4 << 20, (3 << 20) + 4096 + 100, FALLOCATED },	/* into the second PMD folio */
		{ 65536, 4096, WRITTEN },
		{ 4 << 20, (3 << 20) + 4096 + 100, WRITTEN },
		{ 4 << 20, (1 << 20) + 12345, MAPPED },		/* into the first, still mapped */
		{ 5537792, (3 << 20) + 4096 + 100, FALLOCATED },	/* a grain, past its tail */
		{ 5537792, (5 << 20) + 100, MAPPED },		/* into a grain's tail folio */
	};
	const size_t ncases = sizeof(cases) / sizeof(cases[0]);
	char path[PATH_MAX];
	unsigned bad = 0;

	for (size_t i = 0; i < ncases; i++) {
		size_t size = cases[i].size, to = cases[i].to, kept = (to + 4095) / 4096 * 4096;
		uint64_t before;
		uint8_t *p = NULL, *buf = zalloc(1, size);
		struct stat st;
		unsigned was = bad;
		int fd;

		snprintf(path, sizeof(path), "%s/shrink%zu", dir, i);
		unlink(path);
		fd = open(dir, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			die(dir);
		before = allocated(fd);
		close(fd);
		if (cases[i].how == FALLOCATED) {
			fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
			if (fd < 0)
				die(path);
			falloc(fd, size, path);
		} else {
			fd = marked_file(path, size, 1, 0);
		}
		if (cases[i].how == MAPPED) {
			p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (p == MAP_FAILED)
				die("mmap");
			for (size_t pg = 0; pg < size / 4096; pg++)
				*(volatile uint64_t *)(p + pg * 4096) = 1ULL << 40 | pg;
		}
		if (ftruncate(fd, to)) {
			perror("ftruncate");
			bad++;
		}
		if (fstat(fd, &st) || (size_t)st.st_size != to)
			bad++;
		if (allocated(fd) - before != kept) {
			fprintf(stderr, "shrink %zu: %llu bytes charged, want %zu\n", i,
				(unsigned long long)(allocated(fd) - before), kept);
			bad++;
		}
		for (size_t pg = 0; pg * 4096 + 8 <= to; pg++) {
			uint64_t v = p ? *(volatile uint64_t *)(p + pg * 4096) : 0;

			if (!p && pread(fd, &v, sizeof(v), pg * 4096) != sizeof(v))
				v = ~0ULL;
			if (v != (cases[i].how == FALLOCATED ? 0 : 1ULL << 40 | pg)) {
				fprintf(stderr, "shrink %zu: page %zu holds %#llx\n", i, pg, (unsigned long long)v);
				bad++;
				break;
			}
		}
		if (p)
			munmap(p, size);
		if (ftruncate(fd, size) || pread(fd, buf, size, 0) != (ssize_t)size)
			die("grow back");
		for (size_t b = to; b < size; b++)
			if (buf[b]) {
				fprintf(stderr, "shrink %zu: byte %zu past the cut is %#x\n", i, b, buf[b]);
				bad++;
				break;
			}
		printf("shrink %zu: %zu -> %zu%s %s\n", i, size, to, p ? " mapped" : "", bad == was ? "ok" : "FAIL");
		free(buf);
		close(fd);
		unlink(path);
	}
	printf("shrink cases=%zu failed=%u\n", ncases, bad);
	return bad ? 3 : 0;
}

/* Whether the PMD span of fd at off maps with one PMD entry, as a 2 MiB folio does. */
bool pmd_folio(int fd, off_t off)
{
	const size_t span = 2 << 20;
	char line[256], want[32];
	bool found = false, pmd = false;
	uint8_t *p = mmap(NULL, span, PROT_READ, MAP_SHARED, fd, off);
	FILE *f;

	if (p == MAP_FAILED)
		die("mmap");
	(void)*(volatile uint8_t *)p;
	snprintf(want, sizeof(want), "%lx-", (unsigned long)p);
	f = fopen("/proc/self/smaps", "r");
	if (!f)
		die("smaps");
	while (fgets(line, sizeof(line), f)) {
		unsigned long kb;

		if (!strncmp(line, want, strlen(want)))
			found = true;
		else if (found && sscanf(line, "FilePmdMapped: %lu kB", &kb) == 1) {
			pmd = kb > 0;
			break;
		}
	}
	fclose(f);
	munmap(p, span);
	return pmd;
}

#define KPF_COMPOUND_HEAD_BIT 15
#define KPF_COMPOUND_TAIL_BIT 16

static uint64_t kpageflags(int kpf, uint64_t pfn)
{
	uint64_t flags;

	if (pread(kpf, &flags, sizeof(flags), pfn * sizeof(flags)) != sizeof(flags))
		die("/proc/kpageflags");
	return flags;
}

/*
 * Writes the orders of the folios backing the pages of map (a mapping of
 * size bytes, every page of which is present) to out as "9,9,8", from the
 * page frames /proc/self/pagemap gives and the compound flags of
 * /proc/kpageflags. Both need CAP_SYS_ADMIN. Returns false when a folio's
 * pages are not the consecutive frames of one folio.
 */
bool folio_orders(const uint8_t *map, size_t size, char *out, size_t len)
{
	size_t pages = (size + 4095) / 4096, pg = 0, used = 0;
	int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	int kpf = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	bool ok = true;

	if (pm < 0 || kpf < 0)
		die("pagemap");
	out[0] = 0;
	while (pg < pages) {
		uint64_t e, pfn, flags, n = 1;
		unsigned order = 0;

		if (pread(pm, &e, sizeof(e), ((uintptr_t)map / 4096 + pg) * sizeof(e)) != sizeof(e))
			die("/proc/self/pagemap");
		pfn = e & ((UINT64_C(1) << 55) - 1);
		if (!(e >> 63) || !pfn) {
			ok = false;
			break;
		}
		flags = kpageflags(kpf, pfn);
		if (flags & (UINT64_C(1) << KPF_COMPOUND_TAIL_BIT)) {
			ok = false;	/* the page starts no folio */
			break;
		}
		if (flags & (UINT64_C(1) << KPF_COMPOUND_HEAD_BIT))
			while (n < 4096 && (kpageflags(kpf, pfn + n) &
					    (UINT64_C(1) << KPF_COMPOUND_TAIL_BIT)))
				n++;
		while ((UINT64_C(1) << order) < n)
			order++;
		if ((UINT64_C(1) << order) != n || pfn % n || pg % n)
			ok = false;
		/* The file's pages in the folio are its consecutive frames. */
		for (uint64_t i = 1; i < n && pg + i < pages; i++) {
			if (pread(pm, &e, sizeof(e), ((uintptr_t)map / 4096 + pg + i) * sizeof(e)) != sizeof(e))
				die("/proc/self/pagemap");
			if ((e & ((UINT64_C(1) << 55) - 1)) != pfn + i)
				ok = false;
		}
		used += snprintf(out + used, len - used, "%s%u", used ? "," : "", order);
		if (used >= len)
			errx(1, "folio orders: does not fit in %zu bytes", len);
		pg += n;
	}
	close(pm);
	close(kpf);
	return ok;
}

/* Whether reading the first page past the size bytes of map faults SIGBUS. */
static bool sigbus_past(const uint8_t *map, size_t size)
{
	int status;
	pid_t pid = fork();

	if (pid < 0)
		die("fork");
	if (!pid) {
		(void)*(volatile const uint8_t *)(map + (size + 4095) / 4096 * 4096);
		_exit(0);
	}
	if (waitpid(pid, &status, 0) != pid)
		die("waitpid");
	return WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS;
}

/* The PMD spans that cover size bytes and the page past them. */
size_t span_of(size_t size)
{
	return (size + 4096 + (2 << 20) - 1) / (2 << 20) * (2 << 20);
}

/* Maps span_of(size) of fd and touches every page up to size. */
uint8_t *map_span(int fd, size_t size)
{
	uint8_t *p = mmap(NULL, span_of(size), PROT_READ, MAP_SHARED, fd, 0);

	if (p == MAP_FAILED)
		die("mmap");
	for (size_t off = 0; off < size; off += 4096)
		(void)*(volatile uint8_t *)(p + off);
	return p;
}

/*
 * Checks the folios fallocate gives files of the sizes a domain holds, on
 * a mount whose huge= is HUGE (within_size, the default, or always): a
 * 1080p v210 grain file, a lazily sized one allocated after its header was
 * written, and small files, which must stay small under either. For each:
 * the folio orders (folio_orders), the bytes charged, SIGBUS on the first
 * page past EOF, and zeroes past EOF once the file grows over them. Then a
 * view over two grain files must show the selected source on every page
 * and fault SIGBUS past EOF, where a source's tail folio continues.
 */
int cmd_tail(const char *dir)
{
	const bool always = getenv("HUGE") && !strcmp(getenv("HUGE"), "always");
	enum { FALLOCATED, LAZY };
	static const struct {
		size_t size;
		int how;
		const char *within, *always;
	} cases[] = {
		{ 5537792, FALLOCATED, "9,9,8,6,3", "9,9,9" },	/* 8 KiB header + 1080p v210 */
		{ 5537792, LAZY, "9,9,8,6,3", "9,9,9" },
		{ 8192, FALLOCATED, "1", "1" },
		{ 65536, FALLOCATED, "4", "4" },
		{ (2 << 20) - 4096, FALLOCATED, "8,7,6,5,4,3,2,1,0", "8,7,6,5,4,3,2,1,0" },
		{ 2 << 20, FALLOCATED, "9", "9" },
		{ (2 << 20) + 4096, FALLOCATED, "9,0", "9,9" },
	};
	const size_t ncases = sizeof(cases) / sizeof(cases[0]);
	char path[PATH_MAX], orders[256];
	unsigned bad = 0;
	int fds[2];

	for (size_t i = 0; i < ncases; i++) {
		size_t size = cases[i].size;
		const char *want = always ? cases[i].always : cases[i].within;
		uint64_t charge = 0, before, grown;
		unsigned was = bad;
		uint8_t *p, buf[4096];
		bool layout, sigbus;
		int fd;

		for (const char *o = want; *o;) {
			char *e;

			charge += UINT64_C(4096) << strtoul(o, &e, 10);
			o = *e ? e + 1 : e;
		}
		snprintf(path, sizeof(path), "%s/tail%zu", dir, i);
		unlink(path);
		fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (fd < 0)
			die(path);
		before = allocated(fd);
		if (cases[i].how == LAZY) {
			/* As libmxl creates a placeholder: sized lazily, header written. */
			lazy_size(fd, size);
			memset(buf, 0x5a, sizeof(buf));
			if (pwrite(fd, buf, sizeof(buf), 0) != sizeof(buf) ||
			    pwrite(fd, buf, sizeof(buf), 4096) != sizeof(buf))
				die("header");
			if (ioctl(fd, MXLFS_IOC_ALLOCATE))
				die("MXLFS_IOC_ALLOCATE");
		} else {
			falloc(fd, size, path);
		}
		p = map_span(fd, size);
		layout = folio_orders(p, size, orders, sizeof(orders));
		sigbus = sigbus_past(p, size);
		if (!layout || strcmp(orders, want)) {
			fprintf(stderr, "tail %zu: folios %s%s, want %s\n", i, orders,
				layout ? "" : " (not one folio each)", want);
			bad++;
		}
		if (allocated(fd) - before != charge) {
			fprintf(stderr, "tail %zu: %llu bytes charged, want %llu\n", i,
				(unsigned long long)(allocated(fd) - before), (unsigned long long)charge);
			bad++;
		}
		if (!sigbus) {
			fprintf(stderr, "tail %zu: no SIGBUS past EOF\n", i);
			bad++;
		}
		if (cases[i].how == LAZY && p[0] != 0x5a) {
			fprintf(stderr, "tail %zu: the header did not survive the fold\n", i);
			bad++;
		}
		munmap(p, span_of(size));
		/* Growing over what a tail folio covers past EOF shows zeroes. */
		grown = (size + (2 << 20) - 1) / (2 << 20) * (2 << 20);
		if (ftruncate(fd, grown))
			die("ftruncate");
		for (size_t off = size; off < grown; off += sizeof(buf)) {
			if (pread(fd, buf, sizeof(buf), off) != sizeof(buf))
				die("pread");
			for (size_t b = 0; b < sizeof(buf); b++)
				if (buf[b]) {
					fprintf(stderr, "tail %zu: byte %zu past the old EOF is %#x\n", i,
						off + b, buf[b]);
					bad++;
					off = grown;
					break;
				}
		}
		printf("tail %zu: %zu bytes folios=%s %s\n", i, size, orders, bad == was ? "ok" : "FAIL");
		close(fd);
		unlink(path);
	}

	/* A view over two grain files: nothing of a tail folio past EOF shows. */
	for (unsigned s = 0; s < 2; s++) {
		snprintf(path, sizeof(path), "%s/tailsrc%u", dir, s);
		fds[s] = marked_file(path, 5537792, 1, s);
	}
	{
		int v = new_view(dir, "tailview", 5537792, fds, 2);
		__u32 one = 1;
		unsigned was = bad;
		uint8_t *p;

		if (ioctl(v, MXLFS_IOC_VIEW_SELECT, &one))
			die("MXLFS_IOC_VIEW_SELECT");
		p = map_span(v, 5537792);
		for (size_t pg = 0; pg < 5537792 / 4096; pg++)
			if (*(uint64_t *)(p + pg * 4096) != (UINT64_C(1) << 40 | UINT64_C(1) << 32 | pg)) {
				fprintf(stderr, "tail view: page %zu holds %#llx\n", pg,
					(unsigned long long)*(uint64_t *)(p + pg * 4096));
				bad++;
				break;
			}
		if (!sigbus_past(p, 5537792)) {
			fprintf(stderr, "tail view: no SIGBUS past EOF\n");
			bad++;
		}
		printf("tail view: %s\n", bad == was ? "ok" : "FAIL");
		munmap(p, span_of(5537792));
		close(v);
		snprintf(path, sizeof(path), "%s/tailview", dir);
		unlink(path);
	}
	for (unsigned s = 0; s < 2; s++) {
		close(fds[s]);
		snprintf(path, sizeof(path), "%s/tailsrc%u", dir, s);
		unlink(path);
	}
	printf("tail huge=%s cases=%zu failed=%u\n", always ? "always" : "within_size", ncases + 1, bad);
	return bad ? 3 : 0;
}

#define KPF_HWPOISON_BIT 19
/* uapi asm-generic/mman-common.h; glibc's sys/mman.h leaves it out. */
#ifndef MADV_SOFT_OFFLINE
#define MADV_SOFT_OFFLINE 101
#endif
#define POISON_MARK (UINT64_C(0xce) << 56)

static sigjmp_buf poison_jmp;
static volatile sig_atomic_t poison_armed;
static siginfo_t poison_si;

/*
 * Records the signal. A read that faulted jumps back past the read; a
 * signal memory_failure() sent while madvise() ran returns, so the
 * result of madvise() is kept.
 */
static void poison_sigbus(int sig, siginfo_t *si, void *ctx)
{
	(void)sig;
	(void)ctx;
	poison_si = *si;
	if (poison_armed)
		siglongjmp(poison_jmp, 1);
}

static uint64_t meminfo_kb(const char *key)
{
	char line[256], name[128];
	uint64_t v, found = 0;
	FILE *f = fopen("/proc/meminfo", "r");

	if (!f)
		die("/proc/meminfo");
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%127[^:]: %" SCNu64, name, &v) == 2 && !strcmp(name, key))
			found = v;
	fclose(f);
	return found;
}

/* Reads the word at p into *v, or sets poison_si when that faults SIGBUS. */
static void read_sigbus(const uint8_t *p, uint64_t *v)
{
	memset(&poison_si, 0, sizeof(poison_si));
	poison_armed = 1;
	if (!sigsetjmp(poison_jmp, 1))
		*v = *(volatile const uint64_t *)p;
	poison_armed = 0;
}

/* Prints " NAME=<si_code of the SIGBUS, or none> NAME_addr=<si_addr is at>". */
static void print_sigbus(const char *name, const void *at)
{
	const int code = poison_si.si_signo == SIGBUS ? poison_si.si_code : 0;

	printf(" %s=%s %s_addr=%d", name,
	       !code ? "none" : code == BUS_MCEERR_AR ? "BUS_MCEERR_AR" :
	       code == BUS_MCEERR_AO ? "BUS_MCEERR_AO" : "other",
	       name, code && poison_si.si_addr == at);
}

/* pread(), splice() or pwrite() of len bytes at off: the bytes, or -errno. */
static ssize_t poison_io(int fd, off_t off, size_t len, char how)
{
	static char buf[8192];
	loff_t o = off;
	ssize_t n;
	int p[2], err;

	if (how == 'r')
		n = pread(fd, buf, len, off);
	else if (how == 'w')
		n = pwrite(fd, buf, len, off);
	else if (pipe(p))
		die("pipe");
	else
		n = splice(fd, &o, p[1], NULL, len, 0);
	err = errno;
	if (how == 's') {
		close(p[0]);
		close(p[1]);
	}
	return n < 0 ? -err : n;
}

/* Prints " NAME=<bytes, or the errno name>". */
static void print_io(const char *name, ssize_t n)
{
	if (n < 0)
		printf(" %s=%s", name, strerrorname_np(-n));
	else
		printf(" %s=%zd", name, n);
}

int cmd_poison(const char *path, off_t off, const char *how, const char *via)
{
	const bool hard = !strcmp(how, "hard");
	struct sigaction sa = { .sa_sigaction = poison_sigbus, .sa_flags = SA_SIGINFO };
	struct mxlfs_view_sources req = { .count = 1 };
	char vpath[PATH_MAX], opath[PATH_MAX];
	size_t size = 0;
	uint8_t *m = map_file(path, O_RDWR, PROT_READ | PROT_WRITE, &size, NULL);
	int err, fd, rfd, wfd, pm, kpf, large, sigbus_pages = 0, wrong = 0;
	uint64_t e, v, pfn, alloc, corrupted;

	if (!hard && strcmp(how, "soft"))
		errx(1, "%s: not hard or soft", how);
	off &= ~(off_t)4095;
	for (size_t pg = 0; pg < size / 4096; pg++)
		*(uint64_t *)(m + pg * 4096) = POISON_MARK | pg;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		die(path);
	rfd = fd;
	if (via) {
		snprintf(vpath, sizeof(vpath), "%s.view", path);
		rfd = open(vpath, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		req.fds[0] = fd;
		if (rfd < 0 || ftruncate(rfd, size))
			die(vpath);
		if (!strcmp(via, "mixed")) {
			struct mxlfs_view_range rr = { .source = 1, .offset = off + 4096, .length = 4096 };

			snprintf(opath, sizeof(opath), "%s.other", path);
			req.fds[1] = open(opath, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
			if (req.fds[1] < 0 || ftruncate(req.fds[1], size))
				die(opath);
			for (size_t pg = 0; pg < size / 4096; pg++) {
				v = POISON_MARK | pg;
				if (pwrite(req.fds[1], &v, sizeof(v), pg * 4096) != sizeof(v))
					die(opath);
			}
			req.count = 2;
			if (ioctl(rfd, MXLFS_IOC_VIEW_SET_SOURCES, &req) ||
			    ioctl(rfd, MXLFS_IOC_VIEW_SELECT_RANGE, &rr))
				die(vpath);
			close(req.fds[1]);
		} else if (ioctl(rfd, MXLFS_IOC_VIEW_SET_SOURCES, &req)) {
			die(vpath);
		}
		/* The view's is then the only mapping of the page. */
		munmap(m, size);
		m = map_file(vpath, O_RDONLY, PROT_READ, &size, NULL);
		(void)*(volatile uint64_t *)(m + off);
	}

	pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	kpf = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	if (pm < 0 || kpf < 0 ||
	    pread(pm, &e, sizeof(e), ((uintptr_t)m + off) / 4096 * sizeof(e)) != sizeof(e))
		die("pagemap");
	pfn = e >> 63 ? e & ((UINT64_C(1) << 55) - 1) : 0;
	large = pfn && (kpageflags(kpf, pfn) &
			((UINT64_C(1) << KPF_COMPOUND_HEAD_BIT) | (UINT64_C(1) << KPF_COMPOUND_TAIL_BIT)));
	alloc = allocated(fd);
	corrupted = meminfo_kb("HardwareCorrupted");
	if (sigaction(SIGBUS, &sa, NULL))
		die("sigaction");

	memset(&poison_si, 0, sizeof(poison_si));
	err = madvise(m + off, 4096, hard ? MADV_HWPOISON : MADV_SOFT_OFFLINE) ? errno : 0;
	printf("poison %s madvise=%s", how, err ? strerrorname_np(err) : "ok");
	print_sigbus("signal", m + off);
	read_sigbus(m + off, &v);
	print_sigbus("read", m + off);
	for (size_t pg = 0; pg < size / 4096; pg++) {
		read_sigbus(m + pg * 4096, &v);
		if (poison_si.si_signo)
			sigbus_pages++;
		else
			wrong += v != (POISON_MARK | pg);
	}
	munmap(m, size);
	m = map_file(via ? vpath : path, O_RDONLY, PROT_READ, &size, NULL);
	read_sigbus(m + off, &v);
	print_sigbus("refault", m + off);
	printf(" pfn=%#" PRIx64 " large=%d hwpoison=%d corrupted_kb=%" PRId64 " sigbus_pages=%d wrong=%d allocated_delta=%" PRId64,
	       pfn, large, pfn ? !!(kpageflags(kpf, pfn) & (UINT64_C(1) << KPF_HWPOISON_BIT)) : 0,
	       (int64_t)(meminfo_kb("HardwareCorrupted") - corrupted), sigbus_pages, wrong,
	       (int64_t)(allocated(fd) - alloc));
	print_io("pread", poison_io(rfd, off, 4096, 'r'));
	if (off)
		print_io("pread_before", poison_io(rfd, off - 4096, 8192, 'r'));
	else
		printf(" pread_before=-");
	print_io("splice", poison_io(rfd, off, 4096, 's'));
	if (off)
		print_io("splice_before", poison_io(rfd, off - 4096, 8192, 's'));
	else
		printf(" splice_before=-");
	wfd = open(path, O_WRONLY | O_CLOEXEC);
	if (wfd < 0)
		die(path);
	print_io("pwrite", poison_io(wfd, off, 8, 'w'));
	if (off)
		print_io("pwrite_before", poison_io(wfd, off - 4096, 8, 'w'));
	else
		printf(" pwrite_before=-");
	if (off)
		print_io("pread_unaligned", poison_io(rfd, off - 4000, 4000, 'r'));
	else
		printf(" pread_unaligned=-");
	munmap(m, size);
	m = map_file(via ? vpath : path, O_RDONLY, PROT_READ, &size, NULL);
	if (off) {
		read_sigbus(m + off - 4096, &v);
		read_sigbus(m + off, &v);
		print_sigbus("after", m + off);
	} else {
		printf(" after=-");
	}
	printf("\n");
	close(wfd);
	if (via)
		close(rfd);
	munmap(m, size);
	close(pm);
	close(kpf);
	close(fd);
	return 0;
}

/*
 * fallocate of half the available memory in DIR, on a fresh mount, from a
 * child that is sent SIGKILL as soon as the first folio is counted. Prints
 * the bytes asked for and those the mount had allocated when the kill was
 * sent and once the child was reaped, and whether SIGKILL ended it.
 */
int cmd_allockill(const char *dir)
{
	uint64_t len = meminfo_kb("MemAvailable") / 2 * 1024 & ~((2ULL << 20) - 1);
	uint64_t base, at_kill, at_exit;
	char path[PATH_MAX];
	int fd, status;
	bool reaped = false;
	pid_t pid;

	snprintf(path, sizeof(path), "%s/allockill", dir);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0)
		die(path);
	base = allocated(fd);
	pid = fork();
	if (pid < 0)
		die("fork");
	if (!pid)
		_exit(fallocate(fd, 0, 0, len) ? 1 : 0);
	while (!reaped && (at_kill = allocated(fd)) == base)
		reaped = waitpid(pid, &status, WNOHANG) == pid;
	/* A child that ended by itself is reaped already; its pid may be reused. */
	if (!reaped) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}
	at_exit = allocated(fd);
	printf("allockill len=%" PRIu64 " at_kill=%" PRIu64 " at_exit=%" PRIu64 " killed=%d\n",
	       len, at_kill - base, at_exit - base,
	       WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
	close(fd);
	if (unlink(path))
		die(path);
	return 0;
}

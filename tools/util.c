#include "harness.h"

volatile sig_atomic_t stop;

void die(const char *what)
{
	perror(what);
	exit(1);
}

/* calloc that dies on failure, for the arrays a command cannot run without. */
void *zalloc(size_t n, size_t size)
{
	void *p = calloc(n, size);

	if (!p)
		die("calloc");
	return p;
}

unsigned long env(const char *name, unsigned long def)
{
	const char *v = getenv(name);

	return v ? strtoul(v, NULL, 0) : def;
}

uint64_t splitmix64(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

void sleep_ns(uint64_t ns)
{
	struct timespec ts = { .tv_sec = ns / 1000000000, .tv_nsec = ns % 1000000000 };

	while (nanosleep(&ts, &ts) && errno == EINTR && !stop)
		;
}

void *map_file(const char *path, int flags, int prot, size_t *size, int *fdp)
{
	struct stat st;
	void *p;
	int fd = open(path, flags | O_CLOEXEC);

	if (fd < 0)
		die(path);
	if (fstat(fd, &st))
		die("fstat");
	if (*size == 0)
		*size = st.st_size;
	p = mmap(NULL, *size, prot, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		die("mmap");
	if (fdp)
		*fdp = fd;
	else
		close(fd);
	return p;
}

char *read_all(const char *path)
{
	FILE *f = fopen(path, "r");
	char *buf;
	long n;

	if (!f)
		die(path);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	buf = zalloc(1, n + 1);
	if (fread(buf, 1, n, f) != (size_t)n)
		die("fread");
	fclose(f);
	return buf;
}

void write_all(const char *path, const char *data)
{
	int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0664);

	if (fd < 0 || write(fd, data, strlen(data)) != (ssize_t)strlen(data))
		die(path);
	close(fd);
}

int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

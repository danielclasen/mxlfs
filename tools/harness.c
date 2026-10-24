/*
 * Redundancy harness for mxlfs views, built on unmodified libmxl.
 *
 *   harness writer DOMAIN FLOW_ID MEMBER
 *       Writes a 1080p v210 flow. Every 4 KiB of each payload carries
 *       (member << 56 | grain index). Grains are dropped by a deterministic
 *       schedule, so readers can tell which indices no member delivered.
 *   harness merge DOMAIN OUT_ID SKEW MEMBER_FLOW_ID...
 *       Builds OUT_ID as a view flow over the members and publishes each
 *       grain from the primary member, or from another member that holds
 *       it complete when the primary skipped it or is TOLERANCE_US (2000)
 *       late; a late primary is replaced. An index no member delivers
 *       within SKEW grains of the leading member maps to an INVALID
 *       placeholder grain.
 *   harness reader DOMAIN FLOW_ID
 *       Reads every grain, checks every 4 KiB page for index and member,
 *       and compares INVALID grains with the drop schedule.
 *   harness stats PATH
 *       Prints the mxlfs counters of the filesystem holding PATH.
 *   harness flowdef FLOW_ID
 *       Prints the flow definition the writer uses.
 *   harness wanted DIR TIMEOUT_MS [fail ERRNO]
 *       Prints the next flow a path walk waits for (mount option wait=),
 *       or exits 1 after TIMEOUT_MS; with fail, fails its waiters.
 *   harness peek DOMAIN FLOW_ID
 *       Samples the head grain 50 times regardless of validSlices and
 *       counts samples whose every page carries the head's index.
 *   harness bench DIR READERS ROUNDS
 *       Two 1080p v210 grain-sized sources and a view over them in DIR.
 *       READERS processes map the view; each round the parent either
 *       switches the source or not, and every reader then reads one word
 *       of every page. Prints SELECT latency and the per-reader time to
 *       read the grain after a switch and after no switch.
 *
 * Environment: SECONDS (30), SEED (1), DROP_PERMILLE (0), OUTAGE_PERIOD and
 * OUTAGE_LEN in grains (0), MEMBERS (number of members, 2), RATE (50).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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

#define MARKER_INDEX_MASK ((UINT64_C(1) << 56) - 1)
#define MAX_MEMBERS (MXLFS_VIEW_MAX_SOURCES - 1)

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void die(const char *what)
{
	perror(what);
	exit(1);
}

static unsigned long env(const char *name, unsigned long def)
{
	const char *v = getenv(name);

	return v ? strtoul(v, NULL, 0) : def;
}

static mxlRational rate(void)
{
	return (mxlRational){ .numerator = (int64_t)env("RATE", 50), .denominator = 1 };
}

static uint64_t splitmix64(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

/* The drop schedule shared by writers and readers. */
static bool dropped(unsigned member, uint64_t index)
{
	unsigned long period = env("OUTAGE_PERIOD", 0), len = env("OUTAGE_LEN", 0);
	unsigned long members = env("MEMBERS", 2);

	if (period && (index + member * period / members) % period < len)
		return true;
	return splitmix64(env("SEED", 1) ^ (index * 0x100000001b3ULL) ^ member) % 1000 <
	       env("DROP_PERMILLE", 0);
}

static bool all_dropped(uint64_t index)
{
	for (unsigned m = 0; m < env("MEMBERS", 2); m++)
		if (!dropped(m, index))
			return false;
	return true;
}

static void flow_def(char *buf, size_t len, const char *id)
{
	mxlRational r = rate();

	snprintf(buf, len,
		 "{\"description\":\"mxlfs harness\",\"id\":\"%s\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"mxlfs-harness:Video\"]},"
		 "\"format\":\"urn:x-nmos:format:video\",\"label\":\"mxlfs harness\",\"parents\":[],\"media_type\":\"video/v210\","
		 "\"grain_rate\":{\"numerator\":%" PRId64 ",\"denominator\":%" PRId64 "},\"frame_width\":1920,\"frame_height\":1080,"
		 "\"interlace_mode\":\"progressive\",\"colorspace\":\"BT709\",\"components\":["
		 "{\"name\":\"Y\",\"width\":1920,\"height\":1080,\"bit_depth\":10},"
		 "{\"name\":\"Cb\",\"width\":960,\"height\":1080,\"bit_depth\":10},"
		 "{\"name\":\"Cr\",\"width\":960,\"height\":1080,\"bit_depth\":10}]}",
		 id, r.numerator, r.denominator);
}

static void sleep_ns(uint64_t ns)
{
	struct timespec ts = { .tv_sec = ns / 1000000000, .tv_nsec = ns % 1000000000 };

	while (nanosleep(&ts, &ts) && errno == EINTR && !stop)
		;
}

static int cmd_writer(const char *domain, const char *id, unsigned member)
{
	mxlRational r = rate();
	mxlInstance inst = mxlCreateInstance(domain, "");
	mxlFlowWriter w;
	mxlFlowConfigInfo cfg;
	bool created;
	char def[2048];
	uint64_t index, end, written = 0, skipped = 0;
	mxlStatus st;

	if (!inst)
		die("mxlCreateInstance");
	flow_def(def, sizeof(def), id);
	st = mxlCreateFlowWriter(inst, def, NULL, &w, &cfg, &created);
	if (st != MXL_STATUS_OK) {
		fprintf(stderr, "mxlCreateFlowWriter: %d\n", st);
		return 1;
	}

	index = mxlGetCurrentIndex(&r);
	end = index + env("SECONDS", 30) * (uint64_t)r.numerator / (uint64_t)r.denominator;
	for (; index < end && !stop; index++) {
		mxlGrainInfo gi;
		uint8_t *payload;

		sleep_ns(mxlGetNsUntilIndex(index, &r));
		if (dropped(member, index)) {
			skipped++;
			continue;
		}
		st = mxlFlowWriterOpenGrain(w, index, &gi, &payload);
		if (st != MXL_STATUS_OK) {
			fprintf(stderr, "open grain %" PRIu64 ": %d\n", index, st);
			return 1;
		}
		for (uint32_t off = 0; off + 8 <= gi.grainSize; off += 4096)
			*(uint64_t *)(payload + off) = ((uint64_t)member << 56) | index;
		gi.validSlices = gi.totalSlices;
		st = mxlFlowWriterCommitGrain(w, &gi);
		if (st != MXL_STATUS_OK) {
			fprintf(stderr, "commit grain %" PRIu64 ": %d\n", index, st);
			return 1;
		}
		written++;
	}
	printf("writer member=%u written=%" PRIu64 " dropped=%" PRIu64 "\n", member, written, skipped);
	mxlReleaseFlowWriter(inst, w);
	mxlDestroyInstance(inst);
	return 0;
}

static uint64_t now_ns(void);

static void *map_file(const char *path, int flags, int prot, size_t *size, int *fdp)
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

static int parse_uuid(const char *s, uint8_t out[16])
{
	for (int i = 0; i < 16; i++) {
		while (*s == '-')
			s++;
		if (sscanf(s, "%2hhx", &out[i]) != 1)
			return -1;
		s += 2;
	}
	return 0;
}

static char *read_all(const char *path)
{
	FILE *f = fopen(path, "r");
	char *buf;
	long n;

	if (!f)
		die(path);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	buf = calloc(1, n + 1);
	if (fread(buf, 1, n, f) != (size_t)n)
		die("fread");
	fclose(f);
	return buf;
}

static void write_all(const char *path, const char *data)
{
	int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0664);

	if (fd < 0 || write(fd, data, strlen(data)) != (ssize_t)strlen(data))
		die(path);
	close(fd);
}

static void wait_for(const char *path)
{
	for (int i = 0; access(path, F_OK) && i < 1000; i++)
		usleep(10000);
}

static int cmd_merge(const char *domain, const char *out_id, unsigned skew, int count, char **member_ids)
{
	struct flow_header *mh[MAX_MEMBERS], *vh;
	mxlGrainInfo **gh[MAX_MEMBERS];
	int *view_fd, *cur, src_fd[MAX_MEMBERS + 1], data_fd;
	unsigned grains, place = count;
	size_t data_size = 0, grain_size;
	char path[PATH_MAX], tmp[PATH_MAX], *def, *at;
	uint64_t vhead = 0, picks[MAX_MEMBERS + 1] = { 0 }, switches = 0, failovers = 0;
	uint8_t uuid[16];
	struct stat st;

	if (count < 1 || count > MAX_MEMBERS || parse_uuid(out_id, uuid))
		return fprintf(stderr, "bad arguments\n"), 2;

	for (int m = 0; m < count; m++) {
		size_t sz = 0;

		snprintf(path, sizeof(path), "%s/%s.mxl-flow/data", domain, member_ids[m]);
		wait_for(path);
		mh[m] = map_file(path, O_RDONLY, PROT_READ, &sz, NULL);
		if (m == 0)
			data_size = sz;
	}
	grains = mh[0]->info.config.discrete.grainCount;
	for (int m = 1; m < count; m++)
		if (mh[m]->info.config.discrete.grainCount != grains)
			return fprintf(stderr, "members differ in grain count\n"), 1;
	snprintf(path, sizeof(path), "%s/%s.mxl-flow/grains/data.0", domain, member_ids[0]);
	if (stat(path, &st))
		die(path);
	grain_size = st.st_size;

	/* Build the view flow in a scaffold and rename it into the domain. */
	snprintf(tmp, sizeof(tmp), "%s/.mxl-tmp-mergeXXXXXX", domain);
	if (!mkdtemp(tmp))
		die("mkdtemp");

	snprintf(path, sizeof(path), "%s/%s.mxl-flow/flow_def.json", domain, member_ids[0]);
	def = read_all(path);
	at = strstr(def, member_ids[0]);
	if (!at)
		return fprintf(stderr, "member id not in its flow_def.json\n"), 1;
	memcpy(at, out_id, strlen(out_id));
	snprintf(path, sizeof(path), "%s/flow_def.json", tmp);
	write_all(path, def);

	snprintf(path, sizeof(path), "%s/access", tmp);
	write_all(path, "");

	snprintf(path, sizeof(path), "%s/data", tmp);
	data_fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0664);
	if (data_fd < 0 || ftruncate(data_fd, data_size) || fstat(data_fd, &st))
		die(path);
	vh = mmap(NULL, data_size, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);
	if (vh == MAP_FAILED)
		die("mmap data");
	memcpy(vh, mh[0], data_size);
	memcpy(vh->info.config.common.id, uuid, 16);
	vh->info.runtime.headIndex = 0;
	vh->info.runtime.lastReadTime = 0;
	vh->info.runtime.lastWriteTime = mxlGetTime();
	vh->inode = st.st_ino;
	vh->sync = 0;
	if (flock(data_fd, LOCK_SH))
		die("flock");

	/* The INVALID placeholder grain, the last source of every slot. */
	snprintf(path, sizeof(path), "%s/invalid", tmp);
	src_fd[place] = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0664);
	if (src_fd[place] < 0 || posix_fallocate(src_fd[place], 0, grain_size))
		die(path);
	{
		size_t sz = sizeof(mxlGrainInfo);
		mxlGrainInfo *p;

		snprintf(path, sizeof(path), "%s/%s.mxl-flow/grains/data.0", domain, member_ids[0]);
		p = map_file(path, O_RDONLY, PROT_READ, &sz, NULL);
		mxlGrainInfo *inv = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, src_fd[place], 0);
		if (inv == MAP_FAILED)
			die("mmap placeholder");
		*inv = *p;
		inv->index = 0;
		inv->flags = MXL_GRAIN_FLAG_INVALID;
		inv->validSlices = inv->totalSlices;
		munmap(inv, sz);
		munmap(p, sz);
	}

	snprintf(path, sizeof(path), "%s/grains", tmp);
	if (mkdir(path, 0755))
		die(path);
	view_fd = calloc(grains, sizeof(int));
	cur = calloc(grains, sizeof(int));
	for (int m = 0; m < count; m++)
		gh[m] = calloc(grains, sizeof(mxlGrainInfo *));
	for (unsigned k = 0; k < grains; k++) {
		struct mxlfs_view_sources req = { .count = count + 1 };

		for (int m = 0; m < count; m++) {
			size_t sz = sizeof(mxlGrainInfo);

			snprintf(path, sizeof(path), "%s/%s.mxl-flow/grains/data.%u", domain, member_ids[m], k);
			gh[m][k] = map_file(path, O_RDONLY, PROT_READ, &sz, &src_fd[m]);
			req.fds[m] = src_fd[m];
		}
		req.fds[place] = src_fd[place];
		snprintf(path, sizeof(path), "%s/grains/data.%u", tmp, k);
		view_fd[k] = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0664);
		if (view_fd[k] < 0 || ftruncate(view_fd[k], grain_size))
			die(path);
		if (ioctl(view_fd[k], MXLFS_IOC_VIEW_SET_SOURCES, &req))
			die("MXLFS_IOC_VIEW_SET_SOURCES");
		if (ioctl(view_fd[k], MXLFS_IOC_VIEW_SELECT, &place))
			die("MXLFS_IOC_VIEW_SELECT");
		cur[k] = place;
		for (int m = 0; m < count; m++)
			close(src_fd[m]);
	}
	close(src_fd[place]);

	chmod(tmp, 0755);
	snprintf(path, sizeof(path), "%s/%s.mxl-flow", domain, out_id);
	if (renameat2(AT_FDCWD, tmp, AT_FDCWD, path, RENAME_NOREPLACE))
		die("rename into domain");
	fprintf(stderr, "merge: %s over %d members, %u grains of %zu bytes\n", out_id, count, grains, grain_size);

	for (int m = 0; m < count; m++)
		if (__atomic_load_n(&mh[m]->info.runtime.headIndex, __ATOMIC_ACQUIRE) > vhead)
			vhead = mh[m]->info.runtime.headIndex;

	mxlRational r = rate();
	uint64_t period = 1000000000ULL * r.denominator / r.numerator;
	uint64_t tolerance = env("TOLERANCE_US", 2000) * 1000, waiting_index = 0, waiting_since = 0;
	int primary = -1;
	while (!stop) {
		struct futex_waitv w[MAX_MEMBERS] = { 0 };
		uint64_t head[MAX_MEMBERS], lead = 0, timeout = period;
		bool moved = false;
		struct timespec ts;

		for (int m = 0; m < count; m++) {
			w[m].val = __atomic_load_n(&mh[m]->sync, __ATOMIC_ACQUIRE);
			w[m].uaddr = (uintptr_t)&mh[m]->sync;
			w[m].flags = FUTEX2_SIZE_U32;
			head[m] = __atomic_load_n(&mh[m]->info.runtime.headIndex, __ATOMIC_ACQUIRE);
			if (head[m] > lead)
				lead = head[m];
		}

		while (vhead < lead) {
			uint64_t index = vhead + 1;
			unsigned k = index % grains;
			int pick = -1, alt = -1;

			for (int m = 0; m < count; m++) {
				const mxlGrainInfo *g = gh[m][k];

				if (head[m] < index || head[m] - index > grains - 2)
					continue;
				if (__atomic_load_n(&g->index, __ATOMIC_ACQUIRE) != index ||
				    (g->flags & MXL_GRAIN_FLAG_INVALID) || g->validSlices < g->totalSlices)
					continue;
				if (m == primary)
					pick = m;
				else if (alt < 0)
					alt = m;
			}
			/*
			 * The primary serves every grain it has. Another member
			 * serves a grain the primary skipped at once; one the
			 * primary is merely late with only after the tolerance,
			 * and then becomes the primary.
			 */
			if (pick < 0 && alt >= 0) {
				uint64_t t = now_ns();

				if (waiting_index != index) {
					waiting_index = index;
					waiting_since = t;
				}
				if (primary < 0 || head[primary] >= index) {
					pick = alt;
				} else if (t - waiting_since >= tolerance) {
					pick = alt;
					primary = alt;
					failovers++;
				} else {
					timeout = tolerance - (t - waiting_since);
					break;
				}
			}
			if (pick < 0) {
				if (lead < index + skew)
					break;
				pick = place;
			}
			if (primary < 0 && pick != (int)place)
				primary = pick;
			if (cur[k] != pick) {
				if (ioctl(view_fd[k], MXLFS_IOC_VIEW_SELECT, &pick))
					die("MXLFS_IOC_VIEW_SELECT");
				cur[k] = pick;
				switches++;
			}
			picks[pick]++;
			vhead = index;
			moved = true;
		}

		if (moved) {
			vh->info.runtime.lastWriteTime = mxlGetTime();
			__atomic_store_n(&vh->info.runtime.headIndex, vhead, __ATOMIC_RELEASE);
			__atomic_fetch_add(&vh->sync, 1, __ATOMIC_RELEASE);
			syscall(SYS_futex, &vh->sync, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
		}

		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_nsec += timeout;
		ts.tv_sec += ts.tv_nsec / 1000000000;
		ts.tv_nsec %= 1000000000;
		syscall(SYS_futex_waitv, w, count, 0, &ts, CLOCK_MONOTONIC);
	}

	printf("merge switches=%" PRIu64 " failovers=%" PRIu64, switches, failovers);
	for (int m = 0; m <= count; m++)
		printf(" %s=%" PRIu64, m == (int)place ? "invalid" : member_ids[m], picks[m]);
	printf("\n");
	return 0;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static int cmd_reader(const char *domain, const char *id)
{
	mxlInstance inst = mxlCreateInstance(domain, "");
	mxlFlowReader rd;
	mxlFlowConfigInfo cfg;
	mxlFlowRuntimeInfo rt;
	mxlRational r;
	uint64_t index, end, ok = 0, invalid = 0, unexpected_invalid = 0, missed_invalid = 0;
	uint64_t wrong = 0, torn = 0, late = 0, timeouts = 0, per_member[256] = { 0 };
	uint64_t *lat, nlat = 0, cap;
	mxlStatus st;

	if (!inst)
		die("mxlCreateInstance");
	for (int i = 0; (st = mxlCreateFlowReader(inst, id, "", &rd)) != MXL_STATUS_OK; i++) {
		if (i > 1000)
			return fprintf(stderr, "mxlCreateFlowReader: %d\n", st), 1;
		usleep(10000);
	}
	mxlFlowReaderGetConfigInfo(rd, &cfg);
	r = cfg.common.grainRate;
	mxlFlowReaderGetRuntimeInfo(rd, &rt);
	/* A flow nobody has written to yet has head index 0. */
	index = rt.headIndex ? rt.headIndex + 1 : mxlGetCurrentIndex(&r);
	end = index + env("SECONDS", 30) * (uint64_t)r.numerator / (uint64_t)r.denominator;
	cap = end - index + 16;
	lat = calloc(cap, sizeof(*lat));

	uint64_t deadline = mxlGetTime() + (env("SECONDS", 30) + 5) * 1000000000ULL;
	while (index < end && !stop && mxlGetTime() < deadline) {
		mxlGrainInfo gi;
		uint8_t *p;

		st = mxlFlowReaderGetGrain(rd, index, 500000000ULL, &gi, &p);
		if (st != MXL_STATUS_OK && env("DEBUG", 0) && late + timeouts < 5) {
			mxlFlowReaderGetRuntimeInfo(rd, &rt);
			mxlFlowReaderGetGrainNonBlocking(rd, rt.headIndex, &gi, &p);
			fprintf(stderr, "get %" PRIu64 ": status %d, head %" PRIu64 " (index %" PRIu64
				" flags %u valid %u/%u)\n", index, st, rt.headIndex, gi.index, gi.flags,
				gi.validSlices, gi.totalSlices);
		}
		if (st == MXL_STATUS_OK) {
			uint64_t now = mxlGetTime();

			if (nlat < cap)
				lat[nlat++] = now - mxlIndexToTimestamp(&r, index);
			if (gi.flags & MXL_GRAIN_FLAG_INVALID) {
				invalid++;
				if (!all_dropped(index))
					unexpected_invalid++;
			} else {
				uint64_t first = *(uint64_t *)p >> 56;
				bool bad = false;

				for (uint32_t off = 0; off + 8 <= gi.grainSize; off += 4096) {
					uint64_t v = *(uint64_t *)(p + off);

					if ((v & MARKER_INDEX_MASK) != index) {
						wrong++;
						bad = true;
						break;
					}
					if (v >> 56 != first) {
						torn++;
						bad = true;
						break;
					}
				}
				if (!bad) {
					ok++;
					per_member[first]++;
					if (all_dropped(index))
						missed_invalid++;
				}
			}
			index++;
		} else if (st == MXL_ERR_OUT_OF_RANGE_TOO_LATE) {
			late++;
			mxlFlowReaderGetRuntimeInfo(rd, &rt);
			index = rt.headIndex;
		} else if (st == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || st == MXL_ERR_TIMEOUT) {
			timeouts++;
		} else {
			fprintf(stderr, "get grain %" PRIu64 ": %d\n", index, st);
			return 1;
		}
	}

	qsort(lat, nlat, sizeof(*lat), cmp_u64);
	printf("reader flow=%s ok=%" PRIu64 " invalid=%" PRIu64 " unexpected_invalid=%" PRIu64
	       " missed_invalid=%" PRIu64 " wrong_index=%" PRIu64 " torn=%" PRIu64 " late=%" PRIu64
	       " timeouts=%" PRIu64,
	       id, ok, invalid, unexpected_invalid, missed_invalid, wrong, torn, late, timeouts);
	for (int m = 0; m < 8; m++)
		if (per_member[m])
			printf(" member%d=%" PRIu64, m, per_member[m]);
	if (nlat)
		printf(" latency_us_p50=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64, lat[nlat / 2] / 1000,
		       lat[nlat * 99 / 100] / 1000, lat[nlat - 1] / 1000);
	printf("\n");
	mxlReleaseFlowReader(inst, rd);
	mxlDestroyInstance(inst);
	return (wrong || torn || unexpected_invalid || missed_invalid) ? 3 : 0;
}

static int cmd_peek(const char *domain, const char *id)
{
	mxlInstance inst = mxlCreateInstance(domain, "");
	mxlFlowReader rd;
	unsigned match = 0, samples = 0;

	if (!inst || mxlCreateFlowReader(inst, id, "", &rd) != MXL_STATUS_OK)
		return fprintf(stderr, "cannot open %s\n", id), 1;
	for (int i = 0; i < 50; i++, usleep(40000)) {
		mxlFlowRuntimeInfo rt;
		mxlGrainInfo gi;
		uint8_t *p;
		bool good = true;

		mxlFlowReaderGetRuntimeInfo(rd, &rt);
		if (mxlFlowReaderGetGrainSliceNonBlocking(rd, rt.headIndex, 0, &gi, &p) != MXL_STATUS_OK)
			continue;
		samples++;
		for (uint32_t off = 0; off + 8 <= gi.grainSize; off += 4096)
			if ((*(uint64_t *)(p + off) & MARKER_INDEX_MASK) != rt.headIndex)
				good = false;
		match += good;
	}
	printf("peek flow=%s samples=%u full_marker_match=%u\n", id, samples, match);
	return match ? 0 : 3;
}

static int cmd_wanted(const char *dir, unsigned timeout_ms, int fail)
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
		struct mxlfs_wanted_fail f = { .id = req.id, .error = fail };

		if (ioctl(fd, MXLFS_IOC_WANTED_FAIL, &f))
			die("MXLFS_IOC_WANTED_FAIL");
	}
	return 0;
}

static int cmd_stats(const char *path)
{
	struct mxlfs_stats s;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0 || ioctl(fd, MXLFS_IOC_STATS, &s))
		die(path);
	printf("mxlfs allocated=%llu limit=%llu view_selects=%llu view_faults=%llu\n",
	       (unsigned long long)s.bytes_allocated, (unsigned long long)s.bytes_limit,
	       (unsigned long long)s.view_selects, (unsigned long long)s.view_faults);
	return 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

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

static int cmd_bench(const char *dir, unsigned readers, unsigned rounds)
{
	const size_t size = 8192 + 5529600;
	struct bench_shared *sh;
	uint64_t *sel_ns = calloc(rounds, sizeof(*sel_ns)), *sw, *warm;
	uint64_t nsw = 0, nwarm = 0;
	char path[PATH_MAX];
	int src[2], view;

	sh = mmap(NULL, sizeof(*sh) + (size_t)rounds * readers * 8, PROT_READ | PROT_WRITE,
		  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED)
		die("mmap shared");
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
	unlink(path);
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

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc == 5 && !strcmp(argv[1], "writer"))
		return cmd_writer(argv[2], argv[3], atoi(argv[4]));
	if (argc >= 6 && !strcmp(argv[1], "merge"))
		return cmd_merge(argv[2], argv[3], atoi(argv[4]), argc - 5, argv + 5);
	if (argc == 4 && !strcmp(argv[1], "reader"))
		return cmd_reader(argv[2], argv[3]);
	if ((argc == 4 || argc == 6) && !strcmp(argv[1], "wanted"))
		return cmd_wanted(argv[2], atoi(argv[3]), argc == 6 ? atoi(argv[5]) : 0);
	if (argc == 4 && !strcmp(argv[1], "peek"))
		return cmd_peek(argv[2], argv[3]);
	if (argc == 3 && !strcmp(argv[1], "stats"))
		return cmd_stats(argv[2]);
	if (argc == 5 && !strcmp(argv[1], "bench"))
		return cmd_bench(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (argc == 3 && !strcmp(argv[1], "flowdef")) {
		char def[2048];

		flow_def(def, sizeof(def), argv[2]);
		puts(def);
		return 0;
	}
	fprintf(stderr, "usage: see the comment at the top of tools/harness.c\n");
	return 2;
}

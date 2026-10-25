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
 *       and compares INVALID grains with the drop schedule. With TAIL=1 it
 *       instead samples the oldest grain the flow offers, grainCount - 2
 *       behind the head, every millisecond.
 *   harness stats PATH
 *       Prints the mxlfs counters of the filesystem holding PATH.
 *   harness flowdef FLOW_ID
 *       Prints the flow definition the writer uses.
 *   harness placeholder DOMAIN FLOW_ID
 *       Creates the writer's flow with lazy allocation, so its ring takes
 *       no memory until written or mapped, and holds it open as a writer
 *       until SIGTERM, which keeps libmxl's garbage collection away.
 *   harness materialize DOMAIN FLOW_ID
 *       Allocates a placeholder's grains (fallocate over each grain file),
 *       as the agent does before a mirror starts writing.
 *   harness wanted DIR TIMEOUT_MS [fail ERRNO]
 *       Prints the next flow a path walk waits for (mount option wait=),
 *       or exits 1 after TIMEOUT_MS; with fail, fails its waiters.
 *   harness peek DOMAIN FLOW_ID
 *       Samples the head grain 50 times regardless of validSlices and
 *       counts samples whose every page carries the head's index.
 *   harness ranges DIR READERS ROUNDS
 *       Three sources of 3 MiB (one PMD folio and a tail) and a view over
 *       them. Each round selects random page ranges, then READERS
 *       processes check every page of their mapping, and read() checks
 *       the same, against the expected source per page.
 *   harness bench DIR READERS ROUNDS
 *       Two 1080p v210 grain-sized sources and a view over them in DIR.
 *       READERS processes map the view; each round the parent either
 *       switches the source or not, and every reader then reads one word
 *       of every page. Prints SELECT latency and the per-reader time to
 *       read the grain after a switch and after no switch.
 *
 * FORMAT selects the flow: video (v210, the default), data (ANC,
 * video/smpte291, committed with DATA_LEN (64) valid bytes the way libmxl
 * reads data grains) or audio (float32 at 48 kHz, CHANNELS (4) channels,
 * written in batches of BATCH (480) samples, at SAMPLE_RATE). An audio writer reports each
 * committed batch to the merger through the FIFO $DOMAIN/.ev-MEMBER, as a
 * gateway learns of each arrived batch from its completion queue; the
 * merger cannot tell a skipped batch from two quick ones by the ring alone.
 *
 * Environment: SECONDS (30), SEED (1), DROP_PERMILLE (0), OUTAGE_PERIOD and
 * OUTAGE_LEN in grains or batches (0), MEMBERS (number of members, 2),
 * RATE and RATE_DEN (50/1), WIDTH and HEIGHT (1920x1080), SAMPLE_RATE
 * (48000).
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
#include <poll.h>
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
/* libmxl puts a grain's header in the first 8 KiB of its file, before the payload. */
#define GRAIN_HEADER_SIZE 8192

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
	return (mxlRational){ .numerator = (int64_t)env("RATE", 50), .denominator = (int64_t)env("RATE_DEN", 1) };
}

static mxlRational sample_rate(void)
{
	return (mxlRational){ .numerator = (int64_t)env("SAMPLE_RATE", 48000), .denominator = 1 };
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

enum { FMT_VIDEO, FMT_DATA, FMT_AUDIO };

static int format(void)
{
	const char *f = getenv("FORMAT");

	if (!f || !strcmp(f, "video"))
		return FMT_VIDEO;
	if (!strcmp(f, "data"))
		return FMT_DATA;
	if (!strcmp(f, "audio"))
		return FMT_AUDIO;
	fprintf(stderr, "FORMAT must be video, data or audio\n");
	exit(2);
}

static void flow_def(char *buf, size_t len, const char *id)
{
	mxlRational r = rate();
	unsigned long w = env("WIDTH", 1920), h = env("HEIGHT", 1080);

	if (format() == FMT_DATA) {
		snprintf(buf, len,
			 "{\"description\":\"mxlfs harness\",\"id\":\"%s\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"mxlfs-harness:Data\"]},"
			 "\"format\":\"urn:x-nmos:format:data\",\"label\":\"mxlfs harness\",\"parents\":[],\"media_type\":\"video/smpte291\","
			 "\"grain_rate\":{\"numerator\":%" PRId64 ",\"denominator\":%" PRId64 "}}",
			 id, r.numerator, r.denominator);
		return;
	}
	if (format() == FMT_AUDIO) {
		snprintf(buf, len,
			 "{\"description\":\"mxlfs harness\",\"id\":\"%s\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"mxlfs-harness:Audio\"]},"
			 "\"format\":\"urn:x-nmos:format:audio\",\"label\":\"mxlfs harness\",\"parents\":[],\"media_type\":\"audio/float32\","
			 "\"sample_rate\":{\"numerator\":%lu},\"channel_count\":%lu,\"bit_depth\":32}",
			 id, env("SAMPLE_RATE", 48000), env("CHANNELS", 4));
		return;
	}
	snprintf(buf, len,
		 "{\"description\":\"mxlfs harness\",\"id\":\"%s\",\"tags\":{\"urn:x-nmos:tag:grouphint/v1.0\":[\"mxlfs-harness:Video\"]},"
		 "\"format\":\"urn:x-nmos:format:video\",\"label\":\"mxlfs harness\",\"parents\":[],\"media_type\":\"video/v210\","
		 "\"grain_rate\":{\"numerator\":%" PRId64 ",\"denominator\":%" PRId64 "},\"frame_width\":%lu,\"frame_height\":%lu,"
		 "\"interlace_mode\":\"progressive\",\"colorspace\":\"BT709\",\"components\":["
		 "{\"name\":\"Y\",\"width\":%lu,\"height\":%lu,\"bit_depth\":10},"
		 "{\"name\":\"Cb\",\"width\":%lu,\"height\":%lu,\"bit_depth\":10},"
		 "{\"name\":\"Cr\",\"width\":%lu,\"height\":%lu,\"bit_depth\":10}]}",
		 id, r.numerator, r.denominator, w, h, w, h, w / 2, h, w / 2, h);
}

static void sleep_ns(uint64_t ns)
{
	struct timespec ts = { .tv_sec = ns / 1000000000, .tv_nsec = ns % 1000000000 };

	while (nanosleep(&ts, &ts) && errno == EINTR && !stop)
		;
}

static int cmd_awriter(const char *domain, const char *id, unsigned member);

static int cmd_writer(const char *domain, const char *id, unsigned member)
{
	if (format() == FMT_AUDIO)
		return cmd_awriter(domain, id, member);

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
		/* A data grain's valid slices are its length in bytes. */
		gi.validSlices = format() == FMT_DATA ? env("DATA_LEN", 64) : gi.totalSlices;
		st = mxlFlowWriterCommitGrain(w, &gi);
		if (st != MXL_STATUS_OK) {
			fprintf(stderr, "commit grain %" PRIu64 ": %d\n", index, st);
			return 1;
		}
		written++;
	}
	printf("writer created=%d member=%u written=%" PRIu64 " dropped=%" PRIu64 "\n", created, member, written, skipped);
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

static int cmd_amerge(const char *domain, const char *out_id, unsigned skew, int count, char **member_ids,
		      struct flow_header **mh, size_t data_size);

/* Whether a member may be writing index next: the grain after its head, or the one due now. */
static bool overwrites(const struct flow_header *f, uint64_t due, uint64_t next)
{
	uint64_t h = __atomic_load_n(&f->info.runtime.headIndex, __ATOMIC_ACQUIRE);

	return (h > due ? h : due) + 1 >= next;
}

static int cmd_merge(const char *domain, const char *out_id, unsigned skew, int count, char **member_ids)
{
	struct flow_header *mh[MAX_MEMBERS], *vh;
	mxlGrainInfo **gh[MAX_MEMBERS];
	int *view_fd, *cur, src_fd[MAX_MEMBERS + 1], data_fd;
	unsigned grains, place = count;
	size_t data_size = 0, grain_size;
	char path[PATH_MAX], tmp[PATH_MAX], *def, *at;
	uint64_t vhead = 0, picks[MAX_MEMBERS + 1] = { 0 }, switches = 0, failovers = 0, retired = 0;
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
	if (mh[0]->info.config.common.format == MXL_DATA_FORMAT_AUDIO)
		return cmd_amerge(domain, out_id, skew, count, member_ids, mh, data_size);
	bool is_data = mh[0]->info.config.common.format == MXL_DATA_FORMAT_DATA;
	for (int m = 1; m < count; m++)
		if (mh[m]->info.config.common.format != mh[0]->info.config.common.format)
			return fprintf(stderr, "members differ in format\n"), 1;
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

		/* Members that had not written yet when the merge began. */
		if (!vhead && lead)
			vhead = lead - 1;
		while (vhead < lead) {
			uint64_t index = vhead + 1;
			unsigned k = index % grains;
			int pick = -1, alt = -1;

			for (int m = 0; m < count; m++) {
				const mxlGrainInfo *g = gh[m][k];

				if (head[m] < index || head[m] - index > grains - 2)
					continue;
				/*
				 * head >= index says the writer committed it. A
				 * video grain is complete with all its slices; a
				 * data grain's slices are its length, so its commit
				 * is all there is.
				 */
				if (__atomic_load_n(&g->index, __ATOMIC_ACQUIRE) != index ||
				    (g->flags & MXL_GRAIN_FLAG_INVALID) ||
				    (!is_data && g->validSlices < g->totalSlices))
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

		/*
		 * A member writes the grain after its head next, or after a
		 * loss the one due now, into the slot of that index - grains.
		 * Readers of the view trust every index from vhead - grains + 2
		 * on, so while the view lags, the oldest indices it published
		 * are about to be overwritten under them. Each moves to a member
		 * that still holds it, or to the INVALID grain, rather than
		 * serving the next ring period's picture as its own.
		 */
		uint64_t due = mxlGetCurrentIndex(&r);

		for (uint64_t i = vhead + 2 > grains ? vhead + 2 - grains : 1;
		     i <= vhead && i + grains <= (lead > due ? lead : due) + 1; i++) {
			unsigned k = i % grains;
			int to = place;

			if (cur[k] == (int)place || !overwrites(mh[cur[k]], due, i + grains))
				continue;
			for (int m = 0; m < count; m++) {
				const mxlGrainInfo *g = gh[m][k];
				uint64_t h = __atomic_load_n(&mh[m]->info.runtime.headIndex, __ATOMIC_ACQUIRE);

				if (m != cur[k] && h >= i && !overwrites(mh[m], due, i + grains) &&
				    __atomic_load_n(&g->index, __ATOMIC_ACQUIRE) == i &&
				    !(g->flags & MXL_GRAIN_FLAG_INVALID) && (is_data || g->validSlices == g->totalSlices)) {
					to = m;
					break;
				}
			}
			if (ioctl(view_fd[k], MXLFS_IOC_VIEW_SELECT, &to))
				die("MXLFS_IOC_VIEW_SELECT");
			cur[k] = to;
			retired++;
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

	printf("merge switches=%" PRIu64 " failovers=%" PRIu64 " retired=%" PRIu64, switches, failovers, retired);
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

static int cmd_areader(mxlInstance inst, mxlFlowReader rd, const mxlFlowConfigInfo *cfg, const char *id);

static int cmd_reader(const char *domain, const char *id)
{
	mxlInstance inst = mxlCreateInstance(domain, "");
	mxlFlowReader rd;
	mxlFlowConfigInfo cfg;
	mxlFlowRuntimeInfo rt;
	mxlRational r;
	uint64_t index, end, ok = 0, invalid = 0, unexpected_invalid = 0, missed_invalid = 0;
	uint64_t wrong = 0, torn = 0, late = 0, timeouts = 0, per_member[256] = { 0 }, retired_mid_read = 0, overtaken = 0;
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
	if (cfg.common.format == MXL_DATA_FORMAT_AUDIO)
		return cmd_areader(inst, rd, &cfg, id);
	r = cfg.common.grainRate;
	mxlFlowReaderGetRuntimeInfo(rd, &rt);
	/* A flow nobody has written to yet has head index 0. */
	index = rt.headIndex ? rt.headIndex + 1 : mxlGetCurrentIndex(&r);
	end = index + env("SECONDS", 30) * (uint64_t)r.numerator / (uint64_t)r.denominator;
	cap = end - index + 16;
	lat = calloc(cap, sizeof(*lat));

	uint64_t deadline = mxlGetTime() + (env("SECONDS", 30) + 5) * 1000000000ULL;
	uint64_t lag = env("TAIL", 0) ? cfg.discrete.grainCount - 2 : 0, stop_ns = mxlIndexToTimestamp(&r, end);
	while ((lag ? mxlGetTime() < stop_ns : index < end) && !stop && mxlGetTime() < deadline) {
		mxlGrainInfo gi;
		uint8_t *p;

		/* A tail reader samples the oldest grain the flow still offers. */
		if (lag) {
			usleep(1000);
			mxlFlowReaderGetRuntimeInfo(rd, &rt);
			if (rt.headIndex <= lag)
				continue;
			index = rt.headIndex - lag;
		}

		/* A data grain is never complete; any committed length is all of it. */
		if (cfg.common.format == MXL_DATA_FORMAT_DATA)
			st = mxlFlowReaderGetGrainSlice(rd, index, MXL_GRAIN_VALID_SLICES_ANY, 500000000ULL, &gi, &p);
		else
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
						/*
						 * The merger retired the slot while this
						 * read was in progress if its header, 8 KiB
						 * before the payload, now reads INVALID.
						 */
						const mxlGrainInfo *now = (const void *)(p - GRAIN_HEADER_SIZE);
						mxlFlowRuntimeInfo now_rt;

						mxlFlowReaderGetRuntimeInfo(rd, &now_rt);
						if (__atomic_load_n(&now->flags, __ATOMIC_ACQUIRE) & MXL_GRAIN_FLAG_INVALID)
							retired_mid_read++;
						/*
						 * The index left the window the reader
						 * checked it against while it read: the
						 * reader was too slow, which libmxl leaves
						 * to the reader.
						 */
						else if (now_rt.headIndex + 2 > index + cfg.discrete.grainCount)
							overtaken++;
						else if (wrong++ < 5 && env("DEBUG", 0))
							fprintf(stderr, "wrong: want %" PRIu64 " got %" PRIu64 " member %" PRIu64
								" at %u, head %" PRIu64 " then %" PRIu64 "\n", index,
								v & MARKER_INDEX_MASK, v >> 56, off, rt.headIndex, now_rt.headIndex);
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
	       " timeouts=%" PRIu64 " retired_mid_read=%" PRIu64 " overtaken=%" PRIu64,
	       id, ok, invalid, unexpected_invalid, missed_invalid, wrong, torn, late, timeouts, retired_mid_read,
	       overtaken);
	for (int m = 0; m < 8; m++)
		if (per_member[m])
			printf(" member%d=%" PRIu64, m, per_member[m]);
	if (nlat)
		printf(" latency_us_p50=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64, lat[nlat / 2] / 1000,
		       lat[nlat * 99 / 100] / 1000, lat[nlat - 1] / 1000);
	printf("\n");
	mxlReleaseFlowReader(inst, rd);
	mxlDestroyInstance(inst);
	/* A reader that got nothing proves nothing about the rest. */
	return (!ok || wrong || torn || unexpected_invalid || missed_invalid) ? 3 : 0;
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

static int cmd_placeholder(const char *domain, const char *id)
{
	mxlInstance inst = mxlCreateInstance(domain, "");
	mxlFlowConfigInfo cfg;
	mxlFlowWriter w;
	char def[2048];
	bool created;
	__u32 on = 1;
	int fd = open(domain, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	mxlStatus st;

	if (!inst || fd < 0)
		die(domain);
	if (ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on))
		die("MXLFS_IOC_LAZY_ALLOC");
	flow_def(def, sizeof(def), id);
	st = mxlCreateFlowWriter(inst, def, NULL, &w, &cfg, &created);
	if (st != MXL_STATUS_OK)
		return fprintf(stderr, "mxlCreateFlowWriter: %d\n", st), 1;
	on = 0;
	ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on);
	printf("placeholder flow=%s created=%d grains=%u\n", id, created, cfg.discrete.grainCount);
	while (!stop)
		pause();
	mxlReleaseFlowWriter(inst, w);
	mxlDestroyInstance(inst);
	return 0;
}

static int cmd_materialize(const char *domain, const char *id)
{
	char path[PATH_MAX];
	unsigned n = 0;

	for (;; n++) {
		struct stat st;
		int fd;

		snprintf(path, sizeof(path), "%s/%s.mxl-flow/grains/data.%u", domain, id, n);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			break;
		/* MXLFS_IOC_ALLOCATE works in a process that allocates lazily, too. */
		if (fstat(fd, &st) || (getenv("USE_IOCTL") ? ioctl(fd, MXLFS_IOC_ALLOCATE) :
						posix_fallocate(fd, 0, st.st_size)))
			die(path);
		close(fd);
	}
	printf("materialized flow=%s grains=%u\n", id, n);
	return n ? 0 : 1;
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

static int cmd_ranges(const char *dir, unsigned readers, unsigned rounds)
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

/* Audio: samples carry 0x80000000 | member << 28 | (index & 0x0fffffff). */
#define SAMPLE_MARK 0x80000000u
#define SAMPLE_INDEX_MASK 0x0fffffffu
#define PAGE_SAMPLES (4096 / 4)

struct batch_event {
	uint64_t start, end;
};

static bool batch_dropped(unsigned member, uint64_t batch_end)
{
	return dropped(member, batch_end / env("BATCH", 480));
}

/* Whether member committed every batch that overlaps page n. */
static bool page_whole(unsigned member, uint64_t n)
{
	uint64_t batch = env("BATCH", 480);

	for (uint64_t e = (n * PAGE_SAMPLES / batch + 1) * batch; e - batch < (n + 1) * PAGE_SAMPLES; e += batch)
		if (batch_dropped(member, e))
			return false;
	return true;
}

/* A sample may be missing only where no member holds its page whole. */
static bool page_lost(uint64_t n)
{
	for (unsigned m = 0; m < env("MEMBERS", 2); m++)
		if (page_whole(m, n))
			return false;
	return true;
}

static int cmd_awriter(const char *domain, const char *id, unsigned member)
{
	mxlRational r = sample_rate();
	mxlInstance inst = mxlCreateInstance(domain, "");
	uint64_t batch = env("BATCH", 480), written = 0, skipped = 0, end_index, stop_at;
	mxlFlowConfigInfo cfg;
	mxlFlowWriter w;
	char def[2048], path[PATH_MAX];
	bool created;
	mxlStatus st;
	int ev;

	if (!inst)
		die("mxlCreateInstance");
	flow_def(def, sizeof(def), id);
	st = mxlCreateFlowWriter(inst, def, NULL, &w, &cfg, &created);
	if (st != MXL_STATUS_OK)
		return fprintf(stderr, "mxlCreateFlowWriter: %d\n", st), 1;
	snprintf(path, sizeof(path), "%s/.ev-%u", domain, member);
	ev = open(path, O_WRONLY | O_CLOEXEC);
	if (ev < 0)
		die(path);

	end_index = (mxlGetCurrentIndex(&r) / batch + 1) * batch;
	stop_at = end_index + env("SECONDS", 30) * (uint64_t)r.numerator;
	for (; end_index < stop_at && !stop; end_index += batch) {
		mxlMutableWrappedMultiBufferSlice sl;
		struct batch_event e = { end_index - batch, end_index };

		sleep_ns(mxlGetNsUntilIndex(end_index, &r));
		if (batch_dropped(member, end_index)) {
			skipped++;
			continue;
		}
		st = mxlFlowWriterOpenSamples(w, end_index, batch, &sl);
		if (st != MXL_STATUS_OK)
			return fprintf(stderr, "open samples %" PRIu64 ": %d\n", end_index, st), 1;
		for (size_t c = 0; c < sl.count; c++) {
			uint64_t sidx = end_index - batch;

			for (int f = 0; f < 2; f++) {
				uint32_t *q = (uint32_t *)((uint8_t *)sl.base.fragments[f].pointer + c * sl.stride);

				for (size_t j = 0; j < sl.base.fragments[f].size / 4; j++, sidx++)
					q[j] = SAMPLE_MARK | member << 28 | (uint32_t)(sidx & SAMPLE_INDEX_MASK);
			}
		}
		st = mxlFlowWriterCommitSamples(w);
		if (st != MXL_STATUS_OK)
			return fprintf(stderr, "commit samples %" PRIu64 ": %d\n", end_index, st), 1;
		if (write(ev, &e, sizeof(e)) != sizeof(e))
			die("write event");
		written++;
	}
	printf("writer created=%d member=%u written=%" PRIu64 " dropped=%" PRIu64 "\n", created, member, written, skipped);
	close(ev);
	mxlReleaseFlowWriter(inst, w);
	mxlDestroyInstance(inst);
	return 0;
}

/* What one member holds of the page in one ring slot. */
struct page_cover {
	uint64_t n;	/* absolute page number, n * PAGE_SAMPLES is its first sample */
	uint64_t to;	/* samples [n * PAGE_SAMPLES, to) arrived without a gap */
	bool broken;	/* a gap: the member does not hold this page whole */
};

/*
 * The audio merger: the view's channels file selects a member per page of
 * every channel's ring. Each page slot takes the primary while it holds the
 * page without a gap, another member that does when the primary has a gap,
 * and another member after TOLERANCE_US when the primary is only late.
 */
static int cmd_amerge(const char *domain, const char *out_id, unsigned skew, int count, char **member_ids,
		      struct flow_header **mh, size_t data_size)
{
	uint32_t channels = mh[0]->info.config.continuous.channelCount;
	uint32_t buflen = mh[0]->info.config.continuous.bufferLength;
	uint64_t pages = buflen / PAGE_SAMPLES, vh = 0, lead = 0, switches = 0, failovers = 0;
	uint64_t ev_head[MAX_MEMBERS] = { 0 }, picks[MAX_MEMBERS + 1] = { 0 }, unresolved = 0;
	uint64_t tolerance = env("TOLERANCE_US", 2000) * 1000, waiting_page = UINT64_MAX, waiting_since = 0;
	struct page_cover *cov[MAX_MEMBERS];
	struct pollfd pfd[MAX_MEMBERS];
	struct flow_header *vhdr;
	char path[PATH_MAX], tmp[PATH_MAX], *def, *at;
	int src[MAX_MEMBERS + 1], view, data_fd, primary = -1, *sel;
	bool started = false;
	uint8_t uuid[16];
	struct stat st;

	if (buflen % PAGE_SAMPLES || mh[0]->info.config.common.format != MXL_DATA_FORMAT_AUDIO)
		return fprintf(stderr, "buffer length %u is not whole pages\n", buflen), 1;
	for (int m = 1; m < count; m++)
		if (mh[m]->info.config.common.format != MXL_DATA_FORMAT_AUDIO ||
		    mh[m]->info.config.continuous.channelCount != channels ||
		    mh[m]->info.config.continuous.bufferLength != buflen ||
		    mh[m]->info.config.common.grainRate.numerator != mh[0]->info.config.common.grainRate.numerator ||
		    mh[m]->info.config.common.grainRate.denominator != mh[0]->info.config.common.grainRate.denominator)
			return fprintf(stderr, "members are not sample-aligned: geometry or rate differ\n"), 1;
	parse_uuid(out_id, uuid);

	for (int m = 0; m < count; m++) {
		snprintf(path, sizeof(path), "%s/.ev-%d", domain, m);
		pfd[m].fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		pfd[m].events = POLLIN;
		if (pfd[m].fd < 0)
			die(path);
		cov[m] = calloc(pages, sizeof(**cov));
		snprintf(path, sizeof(path), "%s/%s.mxl-flow/channels", domain, member_ids[m]);
		src[m] = open(path, O_RDONLY | O_CLOEXEC);
		if (src[m] < 0)
			die(path);
	}
	sel = calloc(pages, sizeof(*sel));

	snprintf(tmp, sizeof(tmp), "%s/.mxl-tmp-amergeXXXXXX", domain);
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

	snprintf(path, sizeof(path), "%s/data", tmp);
	data_fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0664);
	if (data_fd < 0 || ftruncate(data_fd, data_size) || fstat(data_fd, &st))
		die(path);
	vhdr = mmap(NULL, data_size, PROT_READ | PROT_WRITE, MAP_SHARED, data_fd, 0);
	if (vhdr == MAP_FAILED)
		die("mmap data");
	memcpy(vhdr, mh[0], data_size);
	memcpy(vhdr->info.config.common.id, uuid, 16);
	vhdr->info.runtime.headIndex = 0;
	vhdr->info.runtime.lastWriteTime = mxlGetTime();
	vhdr->inode = st.st_ino;
	vhdr->sync = 0;
	if (flock(data_fd, LOCK_SH))
		die("flock");

	snprintf(path, sizeof(path), "%s/channels", tmp);
	view = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0664);
	if (view < 0 || ftruncate(view, (off_t)channels * buflen * 4))
		die(path);
	/*
	 * Silence, the last source: a page no member holds whole would
	 * otherwise show the selected member's samples from a ring ago.
	 */
	int silence = count;
	snprintf(path, sizeof(path), "%s/silence", tmp);
	src[silence] = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0664);
	if (src[silence] < 0 || posix_fallocate(src[silence], 0, (off_t)channels * buflen * 4))
		die(path);
	struct mxlfs_view_sources req = { .count = count + 1 };
	for (int m = 0; m <= count; m++)
		req.fds[m] = src[m];
	if (ioctl(view, MXLFS_IOC_VIEW_SET_SOURCES, &req))
		die("MXLFS_IOC_VIEW_SET_SOURCES");
	chmod(tmp, 0755);
	snprintf(path, sizeof(path), "%s/%s.mxl-flow", domain, out_id);
	if (renameat2(AT_FDCWD, tmp, AT_FDCWD, path, RENAME_NOREPLACE))
		die("rename into domain");
	fprintf(stderr, "amerge: %s over %d members, %u channels of %u samples\n", out_id, count, channels, buflen);

	while (!stop) {
		uint64_t timeout_ms = 2;
		bool moved = false;

		poll(pfd, count, timeout_ms);
		for (int m = 0; m < count; m++) {
			struct batch_event e;

			while (read(pfd[m].fd, &e, sizeof(e)) == sizeof(e)) {
				if (!started) {
					vh = e.start;
					started = true;
				}
				for (uint64_t n = e.start / PAGE_SAMPLES; n * PAGE_SAMPLES < e.end; n++) {
					struct page_cover *c = &cov[m][n % pages];
					uint64_t a = n * PAGE_SAMPLES > e.start ? n * PAGE_SAMPLES : e.start;
					uint64_t b = (n + 1) * PAGE_SAMPLES < e.end ? (n + 1) * PAGE_SAMPLES : e.end;

					if (c->n != n) {
						c->n = n;
						c->to = n * PAGE_SAMPLES;
						c->broken = false;
					}
					if (a != c->to)
						c->broken = true;
					else
						c->to = b;
				}
				if (e.end > ev_head[m])
					ev_head[m] = e.end;
				if (e.end > lead)
					lead = e.end;
			}
		}

		while (started && vh < lead) {
			uint64_t n = vh / PAGE_SAMPLES, p = n % pages, upto[MAX_MEMBERS];
			int pick = -1, alt = -1;

			for (int m = 0; m < count; m++) {
				struct page_cover *c = &cov[m][p];

				upto[m] = (c->n == n && !c->broken) ? c->to : 0;
				if (upto[m] <= vh)
					continue;
				if (m == primary)
					pick = m;
				else if (alt < 0 || upto[m] > upto[alt])
					alt = m;
			}
			if (pick < 0 && alt >= 0) {
				uint64_t t = now_ns();
				bool gap = primary < 0 || (cov[primary][p].n == n && cov[primary][p].broken) ||
					   ev_head[primary] > vh;

				if (waiting_page != n) {
					waiting_page = n;
					waiting_since = t;
				}
				if (gap) {
					pick = alt;
				} else if (t - waiting_since >= tolerance) {
					pick = alt;
					primary = alt;
					failovers++;
				} else {
					break;
				}
			}
			if (pick < 0) {
				/* Nobody holds this page whole: it becomes silence. */
				if (lead < vh + skew)
					break;
				pick = silence;
				upto[pick] = (n + 1) * PAGE_SAMPLES < lead ? (n + 1) * PAGE_SAMPLES : lead;
				unresolved++;
			}
			if (primary < 0 && pick != silence)
				primary = pick;
			if (sel[p] != pick) {
				for (uint32_t c = 0; c < channels; c++) {
					struct mxlfs_view_range rr = {
						.source = pick,
						.offset = ((uint64_t)c * buflen + p * PAGE_SAMPLES) * 4,
						.length = 4096,
					};

					if (ioctl(view, MXLFS_IOC_VIEW_SELECT_RANGE, &rr))
						die("MXLFS_IOC_VIEW_SELECT_RANGE");
				}
				sel[p] = pick;
				switches++;
			}
			picks[pick] += upto[pick] - vh;
			vh = upto[pick];
			moved = true;
		}

		if (moved) {
			vhdr->info.runtime.lastWriteTime = mxlGetTime();
			__atomic_store_n(&vhdr->info.runtime.headIndex, vh, __ATOMIC_RELEASE);
			__atomic_fetch_add(&vhdr->sync, 1, __ATOMIC_RELEASE);
			syscall(SYS_futex, &vhdr->sync, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
		}
	}

	printf("amerge switches=%" PRIu64 " failovers=%" PRIu64 " unresolved_pages=%" PRIu64, switches, failovers,
	       unresolved);
	for (int m = 0; m < count; m++)
		printf(" %s=%" PRIu64, member_ids[m], picks[m]);
	printf("\n");
	return 0;
}

static int cmd_areader(mxlInstance inst, mxlFlowReader rd, const mxlFlowConfigInfo *cfg, const char *id)
{
	mxlRational r = cfg->common.grainRate;
	uint64_t batch = env("BATCH", 480), ok = 0, lost = 0, unexpected_lost = 0, wrong = 0, late = 0, timeouts = 0;
	uint64_t per_member[8] = { 0 }, *lat, nlat = 0, cap, index, stop_at;
	uint64_t deadline = mxlGetTime() + (env("SECONDS", 30) + 5) * 1000000000ULL;
	mxlFlowRuntimeInfo rt;
	mxlStatus st;

	mxlFlowReaderGetRuntimeInfo(rd, &rt);
	index = ((rt.headIndex ? rt.headIndex : mxlGetCurrentIndex(&r)) / batch + 1) * batch;
	stop_at = index + env("SECONDS", 30) * (uint64_t)r.numerator;
	cap = (stop_at - index) / batch + 16;
	lat = calloc(cap, sizeof(*lat));

	while (index < stop_at && !stop && mxlGetTime() < deadline) {
		mxlWrappedMultiBufferSlice sl;

		st = mxlFlowReaderGetSamples(rd, index, batch, 500000000ULL, &sl);
		if (st == MXL_STATUS_OK) {
			if (nlat < cap)
				lat[nlat++] = mxlGetTime() - mxlIndexToTimestamp(&r, index);
			for (size_t c = 0; c < sl.count; c++) {
				uint64_t sidx = index - batch;

				for (int f = 0; f < 2; f++) {
					const uint32_t *q = (const uint32_t *)((const uint8_t *)sl.base.fragments[f].pointer +
									      c * sl.stride);

					for (size_t j = 0; j < sl.base.fragments[f].size / 4; j++, sidx++) {
						uint32_t v = q[j];

						if (!v) {
							lost++;
							if (!page_lost(sidx / PAGE_SAMPLES))
								unexpected_lost++;
						} else if (!(v & SAMPLE_MARK) || (v & SAMPLE_INDEX_MASK) != (sidx & SAMPLE_INDEX_MASK)) {
							wrong++;
						} else {
							ok++;
							per_member[(v >> 28) & 7]++;
						}
					}
				}
			}
			index += batch;
		} else if (st == MXL_ERR_OUT_OF_RANGE_TOO_LATE) {
			late++;
			mxlFlowReaderGetRuntimeInfo(rd, &rt);
			index = (rt.headIndex / batch) * batch;
		} else if (st == MXL_ERR_OUT_OF_RANGE_TOO_EARLY || st == MXL_ERR_TIMEOUT) {
			timeouts++;
		} else {
			fprintf(stderr, "get samples %" PRIu64 ": %d\n", index, st);
			return 1;
		}
	}

	qsort(lat, nlat, sizeof(*lat), cmp_u64);
	printf("areader flow=%s samples_ok=%" PRIu64 " lost=%" PRIu64 " unexpected_lost=%" PRIu64
	       " wrong_index=%" PRIu64 " late=%" PRIu64 " timeouts=%" PRIu64,
	       id, ok, lost, unexpected_lost, wrong, late, timeouts);
	for (int m = 0; m < 8; m++)
		if (per_member[m])
			printf(" member%d=%" PRIu64, m, per_member[m]);
	if (nlat)
		printf(" latency_us_p50=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64, lat[nlat / 2] / 1000,
		       lat[nlat * 99 / 100] / 1000, lat[nlat - 1] / 1000);
	printf("\n");
	mxlReleaseFlowReader(inst, rd);
	mxlDestroyInstance(inst);
	return (!ok || wrong || unexpected_lost || late) ? 3 : 0;
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
	if (argc == 4 && !strcmp(argv[1], "placeholder"))
		return cmd_placeholder(argv[2], argv[3]);
	if (argc == 4 && !strcmp(argv[1], "materialize"))
		return cmd_materialize(argv[2], argv[3]);
	if ((argc == 4 || argc == 6) && !strcmp(argv[1], "wanted"))
		return cmd_wanted(argv[2], atoi(argv[3]), argc == 6 ? atoi(argv[5]) : 0);
	if (argc == 4 && !strcmp(argv[1], "peek"))
		return cmd_peek(argv[2], argv[3]);
	if (argc == 3 && !strcmp(argv[1], "stats"))
		return cmd_stats(argv[2]);
	if (argc == 5 && !strcmp(argv[1], "ranges"))
		return cmd_ranges(argv[2], atoi(argv[3]), atoi(argv[4]));
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

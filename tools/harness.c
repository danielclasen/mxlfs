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
 *       placeholder grain. With REPLACE=1, a member whose flow is
 *       recreated (a restarted writer) replaces its old ring in every
 *       grain view (MXLFS_IOC_VIEW_REPLACE_SOURCE), without recreating
 *       the views.
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
 *   harness replace DIR READERS ROUNDS
 *       As ranges, and each round also replaces one source with a new
 *       file while READERS processes fault through their mapping; then
 *       they and read() check every page against the expected source and
 *       generation. Checks the ioctl's refusals first.
 *   harness shrink DIR
 *       Shrinks files whose large folios were written through a mapping,
 *       one of them while mapped, and checks size, contents and charge.
 *   harness fold DIR READERS ROUNDS
 *       Replaces a view's source with a lazily sized file touched in a
 *       few pages and allocates it while READERS processes refault
 *       through the view; they must then see what was written to it.
 *   harness foldrace DIR ROUNDS
 *       Each round a lazily sized 4 MiB file, a child that stores an
 *       increasing value into every page of its mapping of it in a loop
 *       and reads each back, and the parent allocating the file
 *       (MXLFS_IOC_ALLOCATE), which folds the child's small folios into
 *       2 MiB ones. A page whose value goes back lost a write. Prints
 *       how many of the 2 MiB spans folded.
 *   harness tail DIR
 *       Allocates files of the sizes a domain holds and checks the order
 *       of every folio backing them, read from /proc/kpageflags (root),
 *       against what the mount's huge= gives (HUGE=always or the
 *       default), the bytes charged, SIGBUS past EOF and zeroes past EOF
 *       once a file grows; then a view over two of them.
 *   harness numa DIR NODE
 *       Makes pages of files in DIR in every way a file gets them:
 *       fallocate, a lazily sized file's header write() and its
 *       allocation (MXLFS_IOC_ALLOCATE, which folds the header's pages),
 *       faults through a VM_HUGEPAGE mapping and through a lazily sized
 *       file's mapping, write(), read() and sendfile() over holes. Checks
 *       that every page of each is on NODE, as move_pages(2) reports it,
 *       and the folios of the VM_HUGEPAGE faults (root, as tail): with
 *       PLACED=1, for a mount with numa_node=, also those of a fault into
 *       a span that holds a page. A read with RWF_NOWAIT, which mxlfs
 *       refuses, and WILLNEED advice over holes must allocate nothing.
 *       PREF=n sets each file's preferred node to n first; PREF_CLEAR=1
 *       clears it again right after.
 *   harness touch PATH
 *       Writes a byte to every page of PATH through a shared mapping.
 *   harness move FILE NODE [dry|strict]
 *       Moves FILE's pages to NODE (MXLFS_IOC_MOVE) and prints the result.
 *   harness nodepages FILE [recount]
 *       Prints FILE's preferred node, holds and pages per node, from its
 *       counters or a walk of its page cache (root).
 *   harness setnode FILE NODE
 *       Sets FILE's preferred node; -1 clears it.
 *   harness mountnodes PATH
 *       Prints the per-node counters of the mount holding PATH.
 *   harness hold FILE
 *       Holds FILE (MXLFS_IOC_HOLD) until SIGTERM.
 *   harness movebench DIR SECONDS
 *       Moves ring files between nodes 0 and 1 under a writer and three
 *       readers that check every page of every grain and time each 2 MiB
 *       block they touch; see cmd_movebench.
 *   harness movepin|moveview|moveflags|movehold|moveorders DIR
 *       A pinned block, a view's mapping, DRY_RUN and STRICT, holds, and
 *       the folios of a domain's files across moves; see each command.
 *   harness movenomem DIR FILL
 *       A move to node 1 once FILL, a mount with numa_node=1, filled it.
 *   harness numacount DIR STEPS
 *       Checks the per-node counters after each of STEPS random steps.
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
#include <linux/io_uring.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/sendfile.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
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

/*
 * Whether a member may write index next before the merger looks again:
 * the grain after its head, or after a loss the one due now. One merge
 * pass waits at most a grain period, so the clock can move on by one.
 */
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
	uint64_t vhead = 0, picks[MAX_MEMBERS + 1] = { 0 }, switches = 0, failovers = 0, retired = 0, late_retired = 0;
	uint64_t replaced = 0, renewed_picks = 0;
	bool replace = env("REPLACE", 0), renewed[MAX_MEMBERS] = { 0 };
	ino_t mino[MAX_MEMBERS];
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
		if (stat(path, &st))
			die(path);
		mino[m] = st.st_ino;
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

		/*
		 * A member whose flow was recreated: libmxl renames a flow into
		 * place allocated, so a new inode at its path is a whole ring.
		 * Slots that select the old ring move first to a member that
		 * holds their grain, or to INVALID, as the new ring holds none
		 * of them; then the new ring replaces the old in every view.
		 */
		for (int m = 0; replace && m < count; m++) {
			snprintf(path, sizeof(path), "%s/%s.mxl-flow/data", domain, member_ids[m]);
			if (stat(path, &st) || st.st_ino == mino[m] || (size_t)st.st_size != data_size)
				continue;
			uint64_t t0 = now_ns();
			for (unsigned k = 0; k < grains; k++) {
				uint64_t back = (vhead % grains + grains - k) % grains, i = vhead - back;
				int to = place;

				if (cur[k] != m || vhead < back)
					continue;
				for (int o = 0; o < count; o++) {
					const mxlGrainInfo *g = gh[o][k];

					if (o != m && __atomic_load_n(&g->index, __ATOMIC_ACQUIRE) == i &&
					    !(g->flags & MXL_GRAIN_FLAG_INVALID) &&
					    (is_data || g->validSlices == g->totalSlices)) {
						to = o;
						break;
					}
				}
				if (ioctl(view_fd[k], MXLFS_IOC_VIEW_SELECT, &to))
					die("MXLFS_IOC_VIEW_SELECT");
				cur[k] = to;
				retired++;
			}
			for (unsigned k = 0; k < grains; k++) {
				struct mxlfs_view_replace rq = { .source = m };
				size_t sz = sizeof(mxlGrainInfo);

				munmap((void *)gh[m][k], sz);
				snprintf(path, sizeof(path), "%s/%s.mxl-flow/grains/data.%u", domain, member_ids[m], k);
				gh[m][k] = map_file(path, O_RDONLY, PROT_READ, &sz, &rq.fd);
				if (ioctl(view_fd[k], MXLFS_IOC_VIEW_REPLACE_SOURCE, &rq))
					die("MXLFS_IOC_VIEW_REPLACE_SOURCE");
				close(rq.fd);
			}
			munmap(mh[m], data_size);
			snprintf(path, sizeof(path), "%s/%s.mxl-flow/data", domain, member_ids[m]);
			mh[m] = map_file(path, O_RDONLY, PROT_READ, &data_size, NULL);
			mino[m] = st.st_ino;
			renewed[m] = true;
			replaced++;
			fprintf(stderr, "merge: replaced %s in %.1f ms\n", member_ids[m], (now_ns() - t0) / 1e6);
		}

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
				/*
				 * Readers trust the index the slot held until the
				 * head reaches index - 1, so a pass that moves the
				 * head on by more than one publishes that first.
				 */
				if (__atomic_load_n(&vh->info.runtime.headIndex, __ATOMIC_RELAXED) + 1 < index)
					__atomic_store_n(&vh->info.runtime.headIndex, index - 1, __ATOMIC_RELEASE);
				if (ioctl(view_fd[k], MXLFS_IOC_VIEW_SELECT, &pick))
					die("MXLFS_IOC_VIEW_SELECT");
				cur[k] = pick;
				switches++;
			}
			picks[pick]++;
			if (pick != (int)place && renewed[pick])
				renewed_picks++;
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
			/* The member already wrote over it: the merger looked too late. */
			if (__atomic_load_n(&mh[cur[k]]->info.runtime.headIndex, __ATOMIC_ACQUIRE) >= i + grains)
				late_retired++;
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

	printf("merge switches=%" PRIu64 " failovers=%" PRIu64 " retired=%" PRIu64 " late_retired=%" PRIu64
	       " replaced=%" PRIu64 " renewed_picks=%" PRIu64, switches, failovers, retired, late_retired, replaced,
	       renewed_picks);
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

			/* A tail reader's latency is its lag. */
			if (!lag && nlat < cap)
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
	printf("mxlfs allocated=%llu limit=%llu view_selects=%llu view_faults=%llu view_replaces=%llu"
	       " numa_fallback_folios=%llu numa_fallback_bytes=%llu move_calls=%llu move_calls_partial=%llu"
	       " move_calls_none=%llu move_blocks=%llu move_bytes=%llu move_ns=%llu move_refused=",
	       (unsigned long long)s.bytes_allocated, (unsigned long long)s.bytes_limit,
	       (unsigned long long)s.view_selects, (unsigned long long)s.view_faults,
	       (unsigned long long)s.view_replaces, (unsigned long long)s.numa_fallback_folios,
	       (unsigned long long)s.numa_fallback_bytes, (unsigned long long)s.move_calls,
	       (unsigned long long)s.move_calls_partial, (unsigned long long)s.move_calls_none,
	       (unsigned long long)s.move_blocks, (unsigned long long)s.move_bytes,
	       (unsigned long long)s.move_ns);
	for (int i = 0; i < MXLFS_REFUSE_MAX; i++)
		printf("%s%llu", i ? "," : "", (unsigned long long)s.move_refused[i]);
	printf(" move_window_ns_sum=%llu move_window_ns_max=%llu move_window_hist=",
	       (unsigned long long)s.move_window_ns_sum, (unsigned long long)s.move_window_ns_max);
	for (int i = 0; i < 9; i++)
		printf("%s%llu", i ? "," : "", (unsigned long long)s.move_window_hist[i]);
	printf(" holds=%llu dry_runs=%llu\n", (unsigned long long)s.holds, (unsigned long long)s.dry_runs);
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

/* A new file of size whose pages carry gen << 40 | source << 32 | page. */
static int marked_file(const char *path, size_t size, uint64_t gen, unsigned source)
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

static int new_view(const char *dir, const char *name, size_t size, const int *fds, unsigned count)
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
	/* Larger than the module's struct by 6 words. */
	__u64 v1[4], v8[sizeof(struct mxlfs_stats) / sizeof(__u64) + 6];
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
		close(v);
		unlink(path);
	}
	__u32 one = 1;
	uint64_t v = 0;
	if (ioctl(nview, MXLFS_IOC_VIEW_SELECT, &one) || pread(nview, &v, sizeof(v), 4096) != sizeof(v) ||
	    v != (2ULL << 32 | 1)) {
		errno = EIO;
		bad += unexpected("read() through the view shows c", -1, 0);
	}
	bad += unexpected("resizing b, which no view uses now", ftruncate(f[1], 4096), 0);
	bad += unexpected("resizing c, which a view uses now", ftruncate(f[2], 4096), EBUSY);
	errno = posix_fallocate(f[2], 0, 2 * size);
	bad += unexpected("growing c with fallocate", errno ? -1 : 0, EBUSY);
	bad += unexpected("allocating c in place (KEEP_SIZE)", fallocate(f[2], FALLOC_FL_KEEP_SIZE, 0, size), 0);
	bad += unexpected("MXLFS_IOC_STATS as it was before view_replaces", ioctl(nview, _IOR('x', 4, __u64[4]), v1), 0);
	/* A caller's larger struct gets what the module has; the rest stays. */
	memset(v8, 0xa5, sizeof(v8));
	if (ioctl(nview, _IOR('x', 4, __u64[sizeof(struct mxlfs_stats) / sizeof(__u64) + 6]), v8) ||
	    v8[0] != v1[0] || v8[1] != v1[1] || v8[n8 - 6] != 0xa5a5a5a5a5a5a5a5ULL ||
	    v8[n8 - 1] != 0xa5a5a5a5a5a5a5a5ULL) {
		errno = EIO;
		bad += unexpected("MXLFS_IOC_STATS with a larger struct", -1, 0);
	}
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

static int cmd_replace(const char *dir, unsigned readers, unsigned rounds)
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
static int cmd_shrink(const char *dir)
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
		/* The stats as every version of the module answers them; [0] is bytes allocated. */
		__u64 s0[4], s1[4];
		uint8_t *p = NULL, *buf = malloc(size);
		struct stat st;
		unsigned was = bad;
		int fd;

		snprintf(path, sizeof(path), "%s/shrink%zu", dir, i);
		unlink(path);
		fd = open(dir, O_RDONLY | O_CLOEXEC);
		if (fd < 0 || ioctl(fd, _IOR('x', 4, __u64[4]), s0))
			die("MXLFS_IOC_STATS");
		close(fd);
		if (cases[i].how == FALLOCATED) {
			fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
			if (fd < 0 || posix_fallocate(fd, 0, size))
				die(path);
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
		if (ioctl(fd, _IOR('x', 4, __u64[4]), s1))
			die("MXLFS_IOC_STATS");
		if (fstat(fd, &st) || (size_t)st.st_size != to)
			bad++;
		if (s1[0] - s0[0] != kept) {
			fprintf(stderr, "shrink %zu: %llu bytes charged, want %zu\n", i,
				(unsigned long long)(s1[0] - s0[0]), kept);
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
static bool pmd_folio(int fd, off_t off)
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

/*
 * A view source whose file was sized lazily and touched in a few pages,
 * so those pages are small folios. Each round a new such file replaces
 * the source, READERS processes drop and refault their mapping of the
 * view in a loop, and the parent allocates the file, which folds the
 * small folios into large ones (mxlfs_fold) unless a view maps one. A
 * fold must not take a folio from under a view's mapping: the kernel
 * reports a page cache folio still mapped (dmesg), and a reader keeps
 * seeing it. Then the parent writes new markers into the file and every
 * reader must see them through the view. Prints how many rounds folded.
 */
static int cmd_fold(const char *dir, unsigned readers, unsigned rounds)
{
	const size_t size = 4 << 20;
	static const size_t touched[] = { 0, 1, 2, 7, 64, 300, 511, 512, 513, 700 };
	const size_t ntouched = sizeof(touched) / sizeof(touched[0]);
	struct shared {
		uint32_t round, phase, done, errors;
	} *sh;
	char path[PATH_MAX], tmp[PATH_MAX];
	unsigned folded = 0, crashed = 0;
	int dirfd, src[2], view, status;
	pid_t *pids = calloc(readers, sizeof(*pids));
	__u32 on;

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (sh == MAP_FAILED || dirfd < 0)
		die(dir);
	snprintf(path, sizeof(path), "%s/fsrc1", dir);
	src[1] = marked_file(path, size, 0, 1);
	/* The lazy source of round r carries 2r << 40 in its touched pages. */
	snprintf(tmp, sizeof(tmp), "%s/fnew", dir);
	snprintf(path, sizeof(path), "%s/fsrc0", dir);
	for (unsigned round = 0; round <= rounds; round++) {
		uint64_t gen = 2 * round;
		int fd;

		on = 1;
		if (ioctl(dirfd, MXLFS_IOC_LAZY_ALLOC, &on))
			die("MXLFS_IOC_LAZY_ALLOC");
		unlink(tmp);
		fd = open(tmp, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (fd < 0 || posix_fallocate(fd, 0, size))
			die(tmp);
		for (size_t t = 0; t < ntouched; t++) {
			uint64_t v = gen << 40 | touched[t];

			if (pwrite(fd, &v, sizeof(v), touched[t] * 4096) != sizeof(v))
				die("pwrite");
		}
		on = 0;
		if (ioctl(dirfd, MXLFS_IOC_LAZY_ALLOC, &on))
			die("MXLFS_IOC_LAZY_ALLOC");

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
		__atomic_store_n(&sh->round, round, __ATOMIC_RELEASE);
		syscall(SYS_futex, &sh->round, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
		usleep(200);
		if (ioctl(fd, MXLFS_IOC_ALLOCATE))
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
			/* Readers end after the last round; one that ended before failed. */
			if (waitpid(-1, &status, WNOHANG) > 0) {
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
static int cmd_foldrace(const char *dir, unsigned rounds)
{
	const size_t size = 4 << 20, pages = size / 4096;
	struct shared {
		uint32_t passes, stop;
		uint64_t lost;
	} *sh;
	char path[PATH_MAX];
	unsigned folded = 0, crashed = 0;
	int dirfd, status;
	__u32 on;

	sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (sh == MAP_FAILED || dirfd < 0)
		die(dir);
	snprintf(path, sizeof(path), "%s/foldrace", dir);
	for (unsigned round = 0; round < rounds && !stop; round++) {
		uint32_t at;
		pid_t pid;
		int fd;

		unlink(path);
		on = 1;
		if (ioctl(dirfd, MXLFS_IOC_LAZY_ALLOC, &on))
			die("MXLFS_IOC_LAZY_ALLOC");
		fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
		if (fd < 0 || posix_fallocate(fd, 0, size))
			die(path);
		on = 0;
		if (ioctl(dirfd, MXLFS_IOC_LAZY_ALLOC, &on))
			die("MXLFS_IOC_LAZY_ALLOC");
		__atomic_store_n(&sh->passes, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&sh->stop, 0, __ATOMIC_RELEASE);
		pid = fork();
		if (pid < 0)
			die("fork");
		if (!pid) {
			volatile uint64_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			uint64_t *last = calloc(pages, sizeof(*last)), seq = 0;

			if (p == MAP_FAILED || !last)
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

/* Bytes the filesystem holding fd has allocated. */
static uint64_t allocated(int fd)
{
	struct mxlfs_stats s;

	if (ioctl(fd, MXLFS_IOC_STATS, &s))
		die("MXLFS_IOC_STATS");
	return s.bytes_allocated;
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
static bool folio_orders(const uint8_t *map, size_t size, char *out, size_t len)
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
			die("folio orders");
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
static size_t span_of(size_t size)
{
	return (size + 4096 + (2 << 20) - 1) / (2 << 20) * (2 << 20);
}

/* Maps span_of(size) of fd and touches every page up to size. */
static uint8_t *map_span(int fd, size_t size)
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
static int cmd_tail(const char *dir)
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
		unsigned was = bad, on = 1;
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
			if (ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on))
				die("MXLFS_IOC_LAZY_ALLOC");
			if (posix_fallocate(fd, 0, size))
				die("posix_fallocate");
			on = 0;
			if (ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on))
				die("MXLFS_IOC_LAZY_ALLOC");
			memset(buf, 0x5a, sizeof(buf));
			if (pwrite(fd, buf, sizeof(buf), 0) != sizeof(buf) ||
			    pwrite(fd, buf, sizeof(buf), 4096) != sizeof(buf))
				die("header");
			if (ioctl(fd, MXLFS_IOC_ALLOCATE))
				die("MXLFS_IOC_ALLOCATE");
		} else if (posix_fallocate(fd, 0, size)) {
			die("posix_fallocate");
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

/*
 * Counts the pages of fd (size bytes) in the page cache, and those of them
 * that move_pages(2) does not report on node. Maps only what is resident,
 * so the check allocates nothing.
 */
static size_t off_node(int fd, size_t size, int node, size_t *resident)
{
	size_t pages = (size + 4095) / 4096, off = 0;
	uint8_t *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	unsigned char *in = calloc(pages, 1);
	void **addr = calloc(pages, sizeof(*addr));
	int *status = calloc(pages, sizeof(*status));
	size_t n = 0;

	if (p == MAP_FAILED || !in || !addr || !status)
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

/* Writes a byte to every page of PATH through a shared mapping. */
static int cmd_touch(const char *path)
{
	size_t size = 0;
	uint8_t *p = map_file(path, O_RDWR, PROT_READ | PROT_WRITE, &size, NULL);

	for (size_t off = 0; off < size; off += 4096)
		p[off] = 1;
	return 0;
}

/* NUMA placement: MXLFS_IOC_SET_NODE, _NODE_PAGES, _MOVE, _HOLD, _MOUNT_NODES */

#define MAX_NODES 64

static const char *const refuse_name[MXLFS_REFUSE_MAX] = {
	"none", "pinned", "view", "busy", "nomem", "held", "signal", "?",
};

static int move_file(int fd, int node, unsigned flags, struct mxlfs_move *m)
{
	memset(m, 0, sizeof(*m));
	m->node = node;
	m->flags = flags;
	return ioctl(fd, MXLFS_IOC_MOVE, m) ? -errno : 0;
}

/* The file's pages per node into pages[MAX_NODES], from its counters or, with RECOUNT, a walk. */
static void node_pages(int fd, unsigned flags, uint64_t *pages, struct mxlfs_node_pages *np)
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
static bool counts_agree(int fd, uint64_t *pages)
{
	uint64_t walked[MAX_NODES];
	struct mxlfs_node_pages np;

	node_pages(fd, 0, pages, &np);
	node_pages(fd, MXLFS_NODE_PAGES_RECOUNT, walked, &np);
	return !memcmp(pages, walked, sizeof(walked));
}

/* Whether all size bytes of fd count on node, by the counters and a walk. */
static bool all_on(int fd, int node, size_t size)
{
	uint64_t pages[MAX_NODES], sum = 0;

	if (!counts_agree(fd, pages))
		return false;
	for (int n = 0; n < MAX_NODES; n++)
		sum += pages[n];
	return pages[node] == sum && sum * 4096 >= size;
}

static int set_node(int fd, int node)
{
	struct mxlfs_node_pref p = { .node = node };

	return ioctl(fd, MXLFS_IOC_SET_NODE, &p) ? -errno : 0;
}

static int hold(int fd, uint32_t on)
{
	return ioctl(fd, MXLFS_IOC_HOLD, &on) ? -errno : 0;
}

static void mount_nodes(int fd, struct mxlfs_node_stat *st)
{
	struct mxlfs_mount_nodes req = { .stats_ptr = (uintptr_t)st, .nr = MAX_NODES };

	memset(st, 0, MAX_NODES * sizeof(*st));
	if (ioctl(fd, MXLFS_IOC_MOUNT_NODES, &req))
		die("MXLFS_IOC_MOUNT_NODES");
}

static void stats_of(int fd, struct mxlfs_stats *s)
{
	if (ioctl(fd, MXLFS_IOC_STATS, s))
		die("MXLFS_IOC_STATS");
}

/*
 * A value of /sys/devices/system/node/nodeN/meminfo, in kB, with the
 * per-CPU differences folded in first (root), so it is exact.
 */
static uint64_t node_kb(int node, const char *key)
{
	char path[64], line[256], want[64];
	uint64_t kb = 0;
	FILE *f;

	if ((f = fopen("/proc/sys/vm/stat_refresh", "w"))) {
		fputs("1", f);
		fclose(f);
	}
	snprintf(path, sizeof(path), "/sys/devices/system/node/node%d/meminfo", node);
	snprintf(want, sizeof(want), "Node %d %s: %%" SCNu64, node, key);
	if (!(f = fopen(path, "r")))
		die(path);
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, want, &kb) == 1)
			break;
	fclose(f);
	return kb;
}

static uint64_t vmstat(const char *key)
{
	char line[256], name[128];
	uint64_t v, found = 0;
	FILE *f = fopen("/proc/vmstat", "r");

	if (!f)
		die("/proc/vmstat");
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%127s %" SCNu64, name, &v) == 2 && !strcmp(name, key))
			found = v;
	fclose(f);
	return found;
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
	       m->result, refuse_name[m->reason < MXLFS_REFUSE_MAX ? m->reason : MXLFS_REFUSE_MAX - 1],
	       (unsigned long long)m->blocks, (unsigned long long)m->blocks_on_node,
	       (unsigned long long)m->blocks_moved, (unsigned long long)m->blocks_refused,
	       (unsigned long long)m->bytes_moved, (unsigned long long)m->refused_offset,
	       (unsigned long long)m->duration_ns / 1000, (unsigned long long)m->max_window_ns / 1000);
}

/* harness move FILE NODE [dry|strict] */
static int cmd_move(const char *path, int node, const char *how)
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

/* harness nodepages FILE [recount] */
static int cmd_nodepages(const char *path, bool recount)
{
	uint64_t pages[MAX_NODES];
	struct mxlfs_node_pages np;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		die(path);
	node_pages(fd, recount ? MXLFS_NODE_PAGES_RECOUNT : 0, pages, &np);
	printf("nodepages pref=%d effective=%d holds=%u", np.pref, np.effective, np.holds);
	for (unsigned n = 0; n < np.nr_nodes && n < MAX_NODES; n++)
		printf(" node%u=%" PRIu64, n, pages[n]);
	printf("\n");
	return 0;
}

/* harness setnode FILE NODE */
static int cmd_setnode(const char *path, int node)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), rc;

	if (fd < 0)
		die(path);
	rc = set_node(fd, node);
	printf("setnode rc=%s\n", rc ? strerror(-rc) : "0");
	return rc ? 1 : 0;
}

/* harness mountnodes PATH */
static int cmd_mountnodes(const char *path)
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

/* harness hold FILE: holds FILE until SIGTERM. */
static int cmd_hold(const char *path)
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

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "FAIL %s:%d: %s: ", __func__, __LINE__, #cond); \
			fprintf(stderr, __VA_ARGS__);                          \
			fputc('\n', stderr);                                   \
			failed++;                                              \
		}                                                              \
	} while (0)

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
 * Times depend on the machine, so the one timing bound that always
 * applies is relative to this run: half of the kernel's block windows
 * must end within what copying 2 MiB from a folio on node 0 to one on
 * node 1 takes here in user space (see copy_2m_ns), or within the
 * smallest bucket, 64 us. The kernel copies a folio before its window,
 * so the window of a folio not written meanwhile is a zap and a page
 * cache swap; a window that holds the copy takes longer. The bounds of a strict local run apply only when set:
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

static int cmd_movebench(const char *dir, unsigned seconds)
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
static int cmd_movepin(const char *dir)
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
static int cmd_moveview(const char *dir)
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
static int cmd_moveflags(const char *dir)
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
	      move_file(src[0], 0, 4, &m) == -EINVAL && move_file(view, 0, 0, &m) == -EINVAL,
	      "bad node, flags or a view");
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
static int cmd_movehold(const char *dir)
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
	CHECK(hold(view, 1) == -EINVAL && set_node(view, 0) == -EINVAL, "a view cannot be held or placed");
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
static int cmd_moveorders(const char *dir)
{
	const bool always = getenv("HUGE") && !strcmp(getenv("HUGE"), "always");
	static const size_t sizes[] = { 5537792, 5537792, 1 << 20 };
	unsigned failed = 0;

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
static int cmd_movenomem(const char *dir, const char *fill)
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
static int cmd_numacount(const char *dir, unsigned steps)
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
			__u32 on = 1;

			if (fstat(fds[f], &sb))
				die("fstat");
			if (!sb.st_size) {
				if (ioctl(fds[f], MXLFS_IOC_LAZY_ALLOC, &on) || posix_fallocate(fds[f], 0, size))
					die("lazy");
				on = 0;
				if (ioctl(fds[f], MXLFS_IOC_LAZY_ALLOC, &on))
					die("lazy");
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

static int cmd_numa(const char *dir, int node)
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
	unsigned bad = 0, on;
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
	on = 1;
	if (ioctl(fds[HEADER], MXLFS_IOC_LAZY_ALLOC, &on) ||
	    posix_fallocate(fds[HEADER], 0, grain) ||
	    posix_fallocate(fds[ALLOCATE], 0, grain))
		die("lazy posix_fallocate");
	if (pwrite(fds[HEADER], buf, 8192, 0) != 8192 ||
	    pwrite(fds[ALLOCATE], buf, 8192, 0) != 8192)
		die("header");
	on = 0;
	if (ioctl(fds[ALLOCATE], MXLFS_IOC_LAZY_ALLOC, &on) ||
	    ioctl(fds[ALLOCATE], MXLFS_IOC_ALLOCATE))
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
	on = 1;
	if (ioctl(fds[FAULT_LAZY], MXLFS_IOC_LAZY_ALLOC, &on) ||
	    posix_fallocate(fds[FAULT_LAZY], 0, grain))
		die("lazy posix_fallocate");
	on = 0;
	if (ioctl(fds[FAULT_LAZY], MXLFS_IOC_LAZY_ALLOC, &on))
		die("MXLFS_IOC_LAZY_ALLOC");
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

/* Points page slot p of every channel of the audio view at source. */
static void select_page(int view, uint32_t channels, uint32_t buflen, uint64_t p, int source)
{
	for (uint32_t c = 0; c < channels; c++) {
		struct mxlfs_view_range rr = {
			.source = source,
			.offset = ((uint64_t)c * buflen + p * PAGE_SAMPLES) * 4,
			.length = 4096,
		};

		if (ioctl(view, MXLFS_IOC_VIEW_SELECT_RANGE, &rr))
			die("MXLFS_IOC_VIEW_SELECT_RANGE");
	}
}

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
	uint64_t retired = 0, replaced = 0, renewed_picks = 0;
	bool started = false, replace = env("REPLACE", 0), renewed[MAX_MEMBERS] = { 0 };
	ino_t mino[MAX_MEMBERS];
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
		if (src[m] < 0 || fstat(src[m], &st))
			die(path);
		mino[m] = st.st_ino;
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

		/*
		 * A member whose flow was recreated: the page slots it serves
		 * move to a member that holds as much of the same page, or to
		 * silence, as its new ring holds none of them; then the new
		 * ring replaces the old one in the view, while every page that
		 * selects it is still mapped by the readers.
		 */
		for (int m = 0; replace && m < count; m++) {
			struct mxlfs_view_replace rq = { .source = m };

			snprintf(path, sizeof(path), "%s/%s.mxl-flow/channels", domain, member_ids[m]);
			if (stat(path, &st) || st.st_ino == mino[m] || (uint64_t)st.st_size != (uint64_t)channels * buflen * 4)
				continue;
			uint64_t t0 = now_ns();
			for (uint64_t p = 0; p < pages; p++) {
				const struct page_cover *c = &cov[m][p];
				int to = silence;

				if (sel[p] != m)
					continue;
				for (int o = 0; o < count; o++)
					if (o != m && cov[o][p].n == c->n && !cov[o][p].broken && cov[o][p].to >= c->to) {
						to = o;
						break;
					}
				select_page(view, channels, buflen, p, to);
				sel[p] = to;
				retired++;
			}
			rq.fd = open(path, O_RDONLY | O_CLOEXEC);
			if (rq.fd < 0)
				die(path);
			if (ioctl(view, MXLFS_IOC_VIEW_REPLACE_SOURCE, &rq))
				die("MXLFS_IOC_VIEW_REPLACE_SOURCE");
			close(rq.fd);
			memset(cov[m], 0, pages * sizeof(**cov));
			mino[m] = st.st_ino;
			renewed[m] = true;
			replaced++;
			fprintf(stderr, "amerge: replaced %s in %.1f ms\n", member_ids[m], (now_ns() - t0) / 1e6);
		}

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
				select_page(view, channels, buflen, p, pick);
				sel[p] = pick;
				switches++;
			}
			picks[pick] += upto[pick] - vh;
			if (pick < count && renewed[pick])
				renewed_picks += upto[pick] - vh;
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

	printf("amerge switches=%" PRIu64 " failovers=%" PRIu64 " unresolved_pages=%" PRIu64 " retired=%" PRIu64
	       " replaced=%" PRIu64 " renewed_picks=%" PRIu64, switches, failovers, unresolved, retired, replaced,
	       renewed_picks);
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
	if (argc == 5 && !strcmp(argv[1], "replace"))
		return cmd_replace(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (argc == 3 && !strcmp(argv[1], "shrink"))
		return cmd_shrink(argv[2]);
	if (argc == 5 && !strcmp(argv[1], "fold"))
		return cmd_fold(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (argc == 4 && !strcmp(argv[1], "foldrace"))
		return cmd_foldrace(argv[2], atoi(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "tail"))
		return cmd_tail(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "touch"))
		return cmd_touch(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "numa"))
		return cmd_numa(argv[2], atoi(argv[3]));
	if (argc == 5 && !strcmp(argv[1], "bench"))
		return cmd_bench(argv[2], atoi(argv[3]), atoi(argv[4]));
	if ((argc == 4 || argc == 5) && !strcmp(argv[1], "move"))
		return cmd_move(argv[2], atoi(argv[3]), argc == 5 ? argv[4] : NULL);
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "nodepages"))
		return cmd_nodepages(argv[2], argc == 4 && !strcmp(argv[3], "recount"));
	if (argc == 4 && !strcmp(argv[1], "setnode"))
		return cmd_setnode(argv[2], atoi(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "mountnodes"))
		return cmd_mountnodes(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "hold"))
		return cmd_hold(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "movebench"))
		return cmd_movebench(argv[2], atoi(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "movepin"))
		return cmd_movepin(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "moveview"))
		return cmd_moveview(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "moveflags"))
		return cmd_moveflags(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "movehold"))
		return cmd_movehold(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "moveorders"))
		return cmd_moveorders(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "movenomem"))
		return cmd_movenomem(argv[2], argv[3]);
	if (argc == 4 && !strcmp(argv[1], "numacount"))
		return cmd_numacount(argv[2], atoi(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "flowdef")) {
		char def[2048];

		flow_def(def, sizeof(def), argv[2]);
		puts(def);
		return 0;
	}
	fprintf(stderr, "usage: see the comment at the top of tools/harness.c\n");
	return 2;
}

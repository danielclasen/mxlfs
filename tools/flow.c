#include "harness.h"

mxlRational rate(void)
{
	return (mxlRational){ .numerator = (int64_t)env("RATE", 50), .denominator = (int64_t)env("RATE_DEN", 1) };
}

/*
 * The environment settings writers, mergers and readers must agree on,
 * with their defaults in one place.
 */
mxlRational sample_rate(void)
{
	return (mxlRational){ .numerator = (int64_t)env("SAMPLE_RATE", 48000), .denominator = 1 };
}

uint64_t run_seconds(void)
{
	return env("SECONDS", 30);
}

unsigned member_count(void)
{
	return env("MEMBERS", 2);
}

uint64_t batch_len(void)
{
	return env("BATCH", 480);
}

uint64_t run_seed(void)
{
	return env("SEED", 1);
}

uint64_t tolerance_ns(void)
{
	return env("TOLERANCE_US", 2000) * 1000;
}

/* The drop schedule shared by writers and readers. */
bool dropped(unsigned member, uint64_t index)
{
	unsigned long period = env("OUTAGE_PERIOD", 0), len = env("OUTAGE_LEN", 0);
	unsigned long members = member_count();

	if (period && (index + member * period / members) % period < len)
		return true;
	return splitmix64(run_seed() ^ (index * 0x100000001b3ULL) ^ member) % 1000 <
	       env("DROP_PERMILLE", 0);
}

int format(void)
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

void flow_def(char *buf, size_t len, const char *id)
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
			 id, (unsigned long)sample_rate().numerator, env("CHANNELS", 4));
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

int parse_uuid(const char *s, uint8_t out[16])
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

/*
 * Starts the flow out_id as a merger publishes it: in a new directory under
 * domain, named in tmp (PATH_MAX bytes), flow_def.json is member_id's with
 * out_id in place of its id, access is empty, and data is a copy of the
 * member's header hdr, of size bytes, with out_id's id, its own inode and
 * nothing written or read yet. data stays locked shared, as a writer holds
 * it. Returns the mapped header.
 */
struct flow_header *view_flow_start(const char *domain, const char *member_id,
				    const char *out_id, const struct flow_header *hdr,
				    size_t size, char *tmp)
{
	char path[PATH_MAX], *def, *at;
	struct flow_header *vh;
	struct stat st;
	uint8_t uuid[16];
	int fd;

	if (parse_uuid(out_id, uuid))
		die("view flow id");
	snprintf(tmp, PATH_MAX, "%s/.mxl-tmp-mergeXXXXXX", domain);
	if (!mkdtemp(tmp))
		die("mkdtemp");
	snprintf(path, sizeof(path), "%s/%s.mxl-flow/flow_def.json", domain, member_id);
	def = read_all(path);
	at = strstr(def, member_id);
	if (!at) {
		fprintf(stderr, "member id not in its flow_def.json\n");
		exit(1);
	}
	memcpy(at, out_id, strlen(out_id));
	snprintf(path, sizeof(path), "%s/flow_def.json", tmp);
	write_all(path, def);
	free(def);

	snprintf(path, sizeof(path), "%s/access", tmp);
	write_all(path, "");

	snprintf(path, sizeof(path), "%s/data", tmp);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0664);
	if (fd < 0 || ftruncate(fd, size) || fstat(fd, &st))
		die(path);
	vh = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (vh == MAP_FAILED)
		die("mmap data");
	memcpy(vh, hdr, size);
	memcpy(vh->info.config.common.id, uuid, 16);
	vh->info.runtime.headIndex = 0;
	vh->info.runtime.lastReadTime = 0;
	vh->info.runtime.lastWriteTime = mxlGetTime();
	vh->inode = st.st_ino;
	vh->sync = 0;
	if (flock(fd, LOCK_SH))
		die("flock");
	return vh;
}

/* A writer of the harness's flow id in inst; exits on failure. */
mxlFlowWriter new_writer(mxlInstance inst, const char *id, mxlFlowConfigInfo *cfg, bool *created)
{
	char def[2048];
	mxlFlowWriter w;
	mxlStatus st;

	if (!inst)
		errx(1, "mxlCreateInstance failed");
	flow_def(def, sizeof(def), id);
	st = mxlCreateFlowWriter(inst, def, NULL, &w, cfg, created);
	if (st != MXL_STATUS_OK)
		errx(1, "mxlCreateFlowWriter: %d", st);
	return w;
}

/* A reader's head, or exit: every caller acts on rt->headIndex. */
void runtime_info(mxlFlowReader rd, mxlFlowRuntimeInfo *rt)
{
	mxlStatus st = mxlFlowReaderGetRuntimeInfo(rd, rt);

	if (st != MXL_STATUS_OK)
		errx(1, "mxlFlowReaderGetRuntimeInfo: %d", st);
}

/*
 * Ends a reader's summary line: grains or samples per member, then the
 * latency percentiles of the nlat latencies in lat, in ns, which it sorts.
 */
void print_summary_tail(const uint64_t per_member[8], uint64_t *lat, uint64_t nlat)
{
	qsort(lat, nlat, sizeof(*lat), cmp_u64);
	for (int m = 0; m < 8; m++)
		if (per_member[m])
			printf(" member%d=%" PRIu64, m, per_member[m]);
	if (nlat)
		printf(" latency_us_p50=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64, lat[nlat / 2] / 1000,
		       lat[nlat * 99 / 100] / 1000, lat[nlat - 1] / 1000);
	printf("\n");
}

int cmd_placeholder(const char *domain, const char *id)
{
	mxlInstance inst = mxlCreateInstance(domain, "");
	mxlFlowConfigInfo cfg;
	mxlFlowWriter w;
	bool created;
	__u32 on = 1;
	int fd = open(domain, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (!inst)
		errx(1, "mxlCreateInstance failed");
	if (fd < 0)
		die(domain);
	if (ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on))
		die("MXLFS_IOC_LAZY_ALLOC");
	w = new_writer(inst, id, &cfg, &created);
	on = 0;
	ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on);
	printf("placeholder flow=%s created=%d grains=%u\n", id, created, cfg.discrete.grainCount);
	while (!stop)
		pause();
	mxlReleaseFlowWriter(inst, w);
	mxlDestroyInstance(inst);
	return 0;
}

int cmd_materialize(const char *domain, const char *id)
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
		if (fstat(fd, &st))
			die(path);
		if (!getenv("USE_IOCTL"))
			falloc(fd, st.st_size, path);
		else if (ioctl(fd, MXLFS_IOC_ALLOCATE))
			die(path);
		close(fd);
	}
	printf("materialized flow=%s grains=%u\n", id, n);
	return n ? 0 : 1;
}

int cmd_stats(const char *path)
{
	/* A module with a shorter struct leaves the rest as it is. */
	struct mxlfs_stats s = { 0 };
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
	for (size_t i = 0; i < sizeof(s.move_window_hist) / sizeof(s.move_window_hist[0]); i++)
		printf("%s%llu", i ? "," : "", (unsigned long long)s.move_window_hist[i]);
	printf(" holds=%llu dry_runs=%llu wanted=%llu wanted_refused=%llu\n",
	       (unsigned long long)s.holds, (unsigned long long)s.dry_runs,
	       (unsigned long long)s.wanted, (unsigned long long)s.wanted_refused);
	return 0;
}

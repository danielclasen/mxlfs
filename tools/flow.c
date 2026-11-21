#include "harness.h"

mxlRational rate(void)
{
	return (mxlRational){ .numerator = (int64_t)env("RATE", 50), .denominator = (int64_t)env("RATE_DEN", 1) };
}

/* The drop schedule shared by writers and readers. */
bool dropped(unsigned member, uint64_t index)
{
	unsigned long period = env("OUTAGE_PERIOD", 0), len = env("OUTAGE_LEN", 0);
	unsigned long members = env("MEMBERS", 2);

	if (period && (index + member * period / members) % period < len)
		return true;
	return splitmix64(env("SEED", 1) ^ (index * 0x100000001b3ULL) ^ member) % 1000 <
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

int cmd_placeholder(const char *domain, const char *id)
{
	mxlInstance inst = mxlCreateInstance(domain, "");
	mxlFlowConfigInfo cfg;
	mxlFlowWriter w;
	char def[2048];
	bool created;
	__u32 on = 1;
	int fd = open(domain, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	mxlStatus st;

	if (!inst)
		errx(1, "mxlCreateInstance failed");
	if (fd < 0)
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

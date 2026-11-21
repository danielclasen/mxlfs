#include "harness.h"

/* Audio: samples carry 0x80000000 | member << 28 | (index & 0x0fffffff). */
#define SAMPLE_MARK 0x80000000u
#define SAMPLE_INDEX_MASK 0x0fffffffu
#define PAGE_SAMPLES (4096 / 4)

struct batch_event {
	uint64_t start, end;
};

static bool batch_dropped(unsigned member, uint64_t batch_end)
{
	return dropped(member, batch_end / batch_len());
}

/* Whether member committed every batch that overlaps page n. */
static bool page_whole(unsigned member, uint64_t n)
{
	uint64_t batch = batch_len();

	for (uint64_t e = (n * PAGE_SAMPLES / batch + 1) * batch; e - batch < (n + 1) * PAGE_SAMPLES; e += batch)
		if (batch_dropped(member, e))
			return false;
	return true;
}

/* A sample may be missing only where no member holds its page whole. */
static bool page_lost(uint64_t n)
{
	for (unsigned m = 0; m < member_count(); m++)
		if (page_whole(m, n))
			return false;
	return true;
}

int cmd_awriter(const char *domain, const char *id, unsigned member)
{
	mxlRational r = sample_rate();
	mxlInstance inst = mxlCreateInstance(domain, "");
	uint64_t batch = batch_len(), written = 0, skipped = 0, end_index, stop_at;
	mxlFlowConfigInfo cfg;
	mxlFlowWriter w;
	char path[PATH_MAX];
	bool created;
	mxlStatus st;
	int ev;

	w = new_writer(inst, id, &cfg, &created);
	snprintf(path, sizeof(path), "%s/.ev-%u", domain, member);
	ev = open(path, O_WRONLY | O_CLOEXEC);
	if (ev < 0)
		die(path);

	end_index = (mxlGetCurrentIndex(&r) / batch + 1) * batch;
	stop_at = end_index + run_seconds() * (uint64_t)r.numerator;
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
int cmd_amerge(const char *domain, const char *out_id, unsigned skew, int count, char **member_ids,
		      struct flow_header **mh, size_t data_size)
{
	uint32_t channels = mh[0]->info.config.continuous.channelCount;
	uint32_t buflen = mh[0]->info.config.continuous.bufferLength;
	uint64_t pages = buflen / PAGE_SAMPLES, vh = 0, lead = 0, switches = 0, failovers = 0;
	uint64_t ev_head[MAX_MEMBERS] = { 0 }, picks[MAX_MEMBERS + 1] = { 0 }, unresolved = 0;
	uint64_t tolerance = tolerance_ns(), waiting_page = UINT64_MAX, waiting_since = 0;
	struct page_cover *cov[MAX_MEMBERS];
	struct pollfd pfd[MAX_MEMBERS];
	struct flow_header *vhdr;
	char path[PATH_MAX], tmp[PATH_MAX];
	int src[MAX_MEMBERS + 1], view, primary = -1, *sel;
	uint64_t retired = 0, replaced = 0, renewed_picks = 0;
	bool started = false, replace = env("REPLACE", 0), renewed[MAX_MEMBERS] = { 0 };
	ino_t mino[MAX_MEMBERS];
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

	for (int m = 0; m < count; m++) {
		snprintf(path, sizeof(path), "%s/.ev-%d", domain, m);
		pfd[m].fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		pfd[m].events = POLLIN;
		if (pfd[m].fd < 0)
			die(path);
		cov[m] = zalloc(pages, sizeof(**cov));
		snprintf(path, sizeof(path), "%s/%s.mxl-flow/channels", domain, member_ids[m]);
		src[m] = open(path, O_RDONLY | O_CLOEXEC);
		if (src[m] < 0 || fstat(src[m], &st))
			die(path);
		mino[m] = st.st_ino;
	}
	sel = zalloc(pages, sizeof(*sel));

	vhdr = view_flow_start(domain, member_ids[0], out_id, mh[0], data_size, tmp);

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
	if (src[silence] < 0)
		die(path);
	falloc(src[silence], (off_t)channels * buflen * 4, path);
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

int cmd_areader(mxlInstance inst, mxlFlowReader rd, const mxlFlowConfigInfo *cfg, const char *id)
{
	mxlRational r = cfg->common.grainRate;
	uint64_t batch = batch_len(), ok = 0, lost = 0, unexpected_lost = 0, wrong = 0, late = 0, timeouts = 0;
	uint64_t per_member[8] = { 0 }, *lat, nlat = 0, cap, index, stop_at;
	uint64_t deadline = mxlGetTime() + (run_seconds() + 5) * 1000000000ULL;
	mxlFlowRuntimeInfo rt;
	mxlStatus st;

	if ((st = mxlFlowReaderGetRuntimeInfo(rd, &rt)) != MXL_STATUS_OK)
		errx(1, "mxlFlowReaderGetRuntimeInfo: %d", st);
	index = ((rt.headIndex ? rt.headIndex : mxlGetCurrentIndex(&r)) / batch + 1) * batch;
	stop_at = index + run_seconds() * (uint64_t)r.numerator;
	cap = (stop_at - index) / batch + 16;
	lat = zalloc(cap, sizeof(*lat));

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

	printf("areader flow=%s samples_ok=%" PRIu64 " lost=%" PRIu64 " unexpected_lost=%" PRIu64
	       " wrong_index=%" PRIu64 " late=%" PRIu64 " timeouts=%" PRIu64,
	       id, ok, lost, unexpected_lost, wrong, late, timeouts);
	print_summary_tail(per_member, lat, nlat);
	mxlReleaseFlowReader(inst, rd);
	mxlDestroyInstance(inst);
	return (!ok || wrong || unexpected_lost || late) ? 3 : 0;
}

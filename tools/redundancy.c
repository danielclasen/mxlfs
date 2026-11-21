#include "harness.h"

#define MARKER_INDEX_MASK ((UINT64_C(1) << 56) - 1)

static bool all_dropped(uint64_t index)
{
	for (unsigned m = 0; m < member_count(); m++)
		if (!dropped(m, index))
			return false;
	return true;
}

int cmd_writer(const char *domain, const char *id, unsigned member)
{
	if (format() == FMT_AUDIO)
		return cmd_awriter(domain, id, member);

	mxlRational r = rate();
	mxlInstance inst = mxlCreateInstance(domain, "");
	mxlFlowWriter w;
	mxlFlowConfigInfo cfg;
	bool created;
	uint64_t index, end, written = 0, skipped = 0;
	mxlStatus st;

	w = new_writer(inst, id, &cfg, &created);

	index = mxlGetCurrentIndex(&r);
	end = index + run_seconds() * (uint64_t)r.numerator / (uint64_t)r.denominator;
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

static void wait_for(const char *path)
{
	for (int i = 0; access(path, F_OK) && i < 1000; i++)
		usleep(10000);
}

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

/*
 * Whether slot k, whose grain i readers trusted, selects a member grain g
 * that no longer holds i: the merger selects a member's grain only while
 * its header holds the index, and libmxl's writer sets the header to the
 * next index it opens there, or to a skipped one it invalidates, before
 * it writes a byte of the payload. Then the member wrote over the grain
 * or is writing it, and the merger retired the slot too late. Each grain
 * of a slot counts once, in late_grain[k], however many passes find it.
 */
static bool retired_late(uint64_t *late_grain, unsigned k, uint64_t i, const mxlGrainInfo *g)
{
	if (late_grain[k] == i || __atomic_load_n(&g->index, __ATOMIC_ACQUIRE) == i)
		return false;
	late_grain[k] = i;
	return true;
}

int cmd_merge(const char *domain, const char *out_id, unsigned skew, int count, char **member_ids)
{
	struct flow_header *mh[MAX_MEMBERS], *vh;
	mxlGrainInfo **gh[MAX_MEMBERS];
	int *view_fd, *cur, src_fd[MAX_MEMBERS + 1];
	unsigned grains, place = count;
	size_t data_size = 0, grain_size;
	char path[PATH_MAX], tmp[PATH_MAX];
	uint64_t vhead = 0, picks[MAX_MEMBERS + 1] = { 0 }, switches = 0, failovers = 0, retired = 0, late_retired = 0;
	uint64_t *late_grain;
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
	vh = view_flow_start(domain, member_ids[0], out_id, mh[0], data_size, tmp);

	/* The INVALID placeholder grain, the last source of every slot. */
	snprintf(path, sizeof(path), "%s/invalid", tmp);
	src_fd[place] = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0664);
	if (src_fd[place] < 0)
		die(path);
	falloc(src_fd[place], grain_size, path);
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
	view_fd = zalloc(grains, sizeof(int));
	cur = zalloc(grains, sizeof(int));
	late_grain = zalloc(grains, sizeof(*late_grain));
	memset(late_grain, 0xff, grains * sizeof(*late_grain));
	for (int m = 0; m < count; m++)
		gh[m] = zalloc(grains, sizeof(mxlGrainInfo *));
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
	uint64_t tolerance = tolerance_ns(), waiting_index = 0, waiting_since = 0;
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
		uint64_t published = vhead;

		while (vhead < lead) {
			uint64_t index = vhead + 1;
			unsigned k = index % grains;
			int pick = -1, alt = -1;

			/*
			 * Readers trusted the index this slot held, index -
			 * grains, while the published head was below index - 1.
			 * A merger that did not run for a grain publishes several
			 * indices in one pass, and the retire loop below starts
			 * past the slots it skips; this counts them, including
			 * the one at which the pass stops.
			 */
			if (index >= published + 2 && cur[k] != (int)place &&
			    retired_late(late_grain, k, index - grains, gh[cur[k]][k]))
				late_retired++;

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
		/* A pass that reached the lead stops before the index after it. */
		if (vhead == lead && lead + 1 >= published + 2) {
			unsigned k = (lead + 1) % grains;

			if (cur[k] != (int)place &&
			    retired_late(late_grain, k, lead + 1 - grains, gh[cur[k]][k]))
				late_retired++;
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
			/* Its member wrote over it or is writing it: retired too late. */
			if (retired_late(late_grain, k, i, gh[cur[k]][k]))
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

int cmd_reader(const char *domain, const char *id)
{
	mxlInstance inst = mxlCreateInstance(domain, "");
	mxlFlowReader rd;
	mxlFlowConfigInfo cfg;
	mxlFlowRuntimeInfo rt;
	mxlRational r;
	uint64_t index, end, ok = 0, invalid = 0, unexpected_invalid = 0, missed_invalid = 0;
	uint64_t wrong = 0, torn = 0, late = 0, timeouts = 0, per_member[256] = { 0 }, retired_mid_read = 0, overtaken = 0;
	uint64_t last_wrong = UINT64_MAX;
	uint64_t *lat, nlat = 0, cap;
	mxlStatus st;

	if (!inst)
		errx(1, "mxlCreateInstance failed");
	for (int i = 0; (st = mxlCreateFlowReader(inst, id, "", &rd)) != MXL_STATUS_OK; i++) {
		if (i > 1000)
			return fprintf(stderr, "mxlCreateFlowReader: %d\n", st), 1;
		usleep(10000);
	}
	if ((st = mxlFlowReaderGetConfigInfo(rd, &cfg)) != MXL_STATUS_OK)
		errx(1, "mxlFlowReaderGetConfigInfo: %d", st);
	if (cfg.common.format == MXL_DATA_FORMAT_AUDIO)
		return cmd_areader(inst, rd, &cfg, id);
	r = cfg.common.grainRate;
	if ((st = mxlFlowReaderGetRuntimeInfo(rd, &rt)) != MXL_STATUS_OK)
		errx(1, "mxlFlowReaderGetRuntimeInfo: %d", st);
	/* A flow nobody has written to yet has head index 0. */
	index = rt.headIndex ? rt.headIndex + 1 : mxlGetCurrentIndex(&r);
	end = index + run_seconds() * (uint64_t)r.numerator / (uint64_t)r.denominator;
	cap = end - index + 16;
	lat = zalloc(cap, sizeof(*lat));

	uint64_t deadline = mxlGetTime() + (run_seconds() + 5) * 1000000000ULL;
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
						/*
						 * One wrong grain per index: a tail reader
						 * reads the oldest index again every
						 * millisecond while the head stays.
						 */
						else if (index != last_wrong) {
							last_wrong = index;
							if (wrong++ < 5)
								fprintf(stderr, "wrong: want %" PRIu64 " got %" PRIu64
									" member %" PRIu64 " at %u, head %" PRIu64
									" then %" PRIu64 "\n", index, v & MARKER_INDEX_MASK,
									v >> 56, off, rt.headIndex, now_rt.headIndex);
						}
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

	printf("reader flow=%s ok=%" PRIu64 " invalid=%" PRIu64 " unexpected_invalid=%" PRIu64
	       " missed_invalid=%" PRIu64 " wrong_index=%" PRIu64 " torn=%" PRIu64 " late=%" PRIu64
	       " timeouts=%" PRIu64 " retired_mid_read=%" PRIu64 " overtaken=%" PRIu64,
	       id, ok, invalid, unexpected_invalid, missed_invalid, wrong, torn, late, timeouts, retired_mid_read,
	       overtaken);
	print_summary_tail(per_member, lat, nlat);
	mxlReleaseFlowReader(inst, rd);
	mxlDestroyInstance(inst);
	/* A reader that got nothing proves nothing about the rest. */
	return (!ok || wrong || torn || unexpected_invalid || missed_invalid) ? 3 : 0;
}

int cmd_peek(const char *domain, const char *id)
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

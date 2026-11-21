/*
 * Test harness for mxlfs, built on unmodified libmxl: the commands the VM
 * suites in tests/ run, redundancy over views among them.
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
 *       or exits 1 after TIMEOUT_MS; with fail, fails its waiters after
 *       checking that WANTED_FAIL takes errnos 1 to 511 and refuses
 *       others and a non-zero pad (bad_input_wrong counts the calls that
 *       answered otherwise).
 *   harness noxdev DIR PATH
 *       Opens PATH below DIR with openat2 and RESOLVE_NO_XDEV and prints
 *       the error, or ok, and the seconds the call took.
 *   harness wantcheck ROOT WAIT_MS
 *       On the root of a mount with wait= and nothing waited for:
 *       WANTED_NEXT refuses flags other than 0 (flags_wrong counts the
 *       calls that answered otherwise), and a flow whose copy to the
 *       caller fails with EFAULT is reported again (efault_lost counts
 *       the checks that failed). The longest path the struct holds is
 *       reported in full, and a walk through a path one byte longer, or
 *       of PATH_MAX - 1, fails with ENAMETOOLONG within a quarter of
 *       WAIT_MS, the mount's wait= (toolong_wrong).
 *   harness wantrace DIR ROUNDS
 *       On a mount with a long wait= and nothing waited for: ROUNDS times
 *       several walks through one missing flow at once, which must make
 *       one entry, then ROUNDS times two walks through two missing flows
 *       at once while the thread group is one entry short of its cap, of
 *       which exactly one must be refused.
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
 *       generation. Checks the refusals of the view ioctls and
 *       LAZY_ALLOC first.
 *   harness shrink DIR
 *       Shrinks files whose large folios were written through a mapping,
 *       one of them while mapped, and checks size, contents and charge.
 *   harness viewrace DIR ROUNDS
 *       Makes a new file a view of MXLFS_VIEW_MAX_SIZE ROUNDS times while
 *       a child truncates it to a page more and back; every view made
 *       must have its source's size.
 *   harness writerace DIR ROUNDS
 *       Makes a new file a view ROUNDS times while a thread writes a page
 *       past its end; the write and the view must not both succeed.
 *   harness foldhold DIR
 *       Folds a lazily sized file whose touched page a pipe holds through
 *       vmsplice, which must be refused, and one whose pipe was drained,
 *       which must fold.
 *   harness foldmem DIR ROUNDS
 *       Folds a lazily sized file while a thread writes its touched pages
 *       through /proc/self/mem; every page must keep the last value
 *       written to it.
 *   harness stale DIR
 *       On a mount with a small size=, has read_folio refuse a page of a
 *       view's source and checks that the page reads as a hole through
 *       the view, faults SIGBUS there, and is zero after a fold; and that
 *       ALLOCATE makes a refused page of a one-page source a zeroed,
 *       counted page the view maps.
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
 *   harness allocate PATH
 *       MXLFS_IOC_ALLOCATE on PATH under an inotify watch; prints the
 *       error, or ok, and how many IN_MODIFY events it raised.
 *   harness lazyfile PATH SIZE
 *       Creates PATH lazily sized to SIZE bytes (MXLFS_IOC_LAZY_ALLOC and
 *       fallocate), so its pages come 4 KiB at a time as they are touched.
 *   harness touch PATH
 *       Writes a byte to every page of PATH through a shared mapping.
 *   harness poison FILE OFF hard|soft [view|mixed]
 *       Marks every page of FILE through a shared mapping, then injects a
 *       memory error into the page at OFF with MADV_HWPOISON (hard) or
 *       MADV_SOFT_OFFLINE (soft), through FILE's mapping or, with view,
 *       through the mapping of a new view FILE.view over it (root). With
 *       mixed, the page after OFF selects a second source, FILE.other,
 *       so the view maps FILE's folio a page at a time; the reads and
 *       mappings below are then the view's.
 *       Prints the madvise() error; the si_code of the SIGBUS, and whether
 *       its si_addr is the page, that madvise() raised (signal), that the
 *       next read of the page raised (read) and that a read through a
 *       fresh mapping raised (refault); the page's frame, whether its
 *       folio was large before the injection, and its KPF_HWPOISON, the change of HardwareCorrupted, the pages that
 *       fault and that lost their mark, and the change of bytes_allocated.
 *       Then the bytes, or the error, of pread() and splice() of the page
 *       at OFF and of two pages from the one before it (- when OFF is in
 *       the first page), and of a pwrite() into the page and into the
 *       one before it, then of a pread() of the 4000 bytes that end where
 *       the page at OFF starts. Last, the SIGBUS of a read of the page
 *       through a fresh mapping that first read the page before it
 *       (after; - when OFF is in the first page).
 *   harness move FILE NODE [dry|strict]
 *       Moves FILE's pages to NODE (MXLFS_IOC_MOVE) and prints the result.
 *   harness nodepages FILE [recount|pad]
 *       Prints FILE's preferred node, holds and pages per node, from its
 *       counters or a walk of its page cache (root); with pad, the result
 *       of a call with a non-zero pad.
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
#include "harness.h"

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
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
	if (argc == 4 && !strcmp(argv[1], "noxdev"))
		return cmd_noxdev(argv[2], argv[3]);
	if (argc == 4 && !strcmp(argv[1], "wantrace"))
		return cmd_wantrace(argv[2], atoi(argv[3]));
	if (argc == 4 && !strcmp(argv[1], "wantcheck"))
		return cmd_wantcheck(argv[2], atoi(argv[3]));
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
	if (argc == 4 && !strcmp(argv[1], "viewrace"))
		return cmd_viewrace(argv[2], atoi(argv[3]));
	if (argc == 4 && !strcmp(argv[1], "writerace"))
		return cmd_writerace(argv[2], atoi(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "stale"))
		return cmd_stale(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "foldhold"))
		return cmd_foldhold(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "foldmem"))
		return cmd_foldmem(argv[2], atoi(argv[3]));
	if (argc == 5 && !strcmp(argv[1], "fold"))
		return cmd_fold(argv[2], atoi(argv[3]), atoi(argv[4]));
	if (argc == 4 && !strcmp(argv[1], "foldrace"))
		return cmd_foldrace(argv[2], atoi(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "tail"))
		return cmd_tail(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "allocate"))
		return cmd_allocate(argv[2]);
	if (argc == 4 && !strcmp(argv[1], "lazyfile"))
		return cmd_lazyfile(argv[2], atoll(argv[3]));
	if (argc == 3 && !strcmp(argv[1], "touch"))
		return cmd_touch(argv[2]);
	if ((argc == 5 || (argc == 6 && (!strcmp(argv[5], "view") || !strcmp(argv[5], "mixed")))) &&
	    !strcmp(argv[1], "poison"))
		return cmd_poison(argv[2], atoll(argv[3]), argv[4], argc == 6 ? argv[5] : NULL);
	if (argc == 4 && !strcmp(argv[1], "numa"))
		return cmd_numa(argv[2], atoi(argv[3]));
	if (argc == 5 && !strcmp(argv[1], "bench"))
		return cmd_bench(argv[2], atoi(argv[3]), atoi(argv[4]));
	if ((argc == 4 || argc == 5) && !strcmp(argv[1], "move"))
		return cmd_move(argv[2], atoi(argv[3]), argc == 5 ? argv[4] : NULL);
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "nodepages"))
		return cmd_nodepages(argv[2], argc == 4 ? argv[3] : NULL);
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
	fprintf(stderr, "usage: see the comment at the top of tools/main.c\n");
	return 2;
}

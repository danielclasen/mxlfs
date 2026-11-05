# mxlfs

A Linux memory filesystem for [MXL](https://github.com/dmf-mxl/mxl) domains.
An MXL domain is a directory of shared-memory ring buffers that writers and
readers `mmap`; mxlfs is a drop-in for the tmpfs such a domain usually lives
on, plus views: files whose pages are switched between several equally sized
source files without copying.

With views, one flow can be served from the rings of several member flows,
choosing per grain the member that holds it complete. Readers are unmodified
libmxl readers; they see an ordinary flow.

## Requirements

- Linux 6.18 with `CONFIG_TRANSPARENT_HUGEPAGE`, without which the
  module does not build. It is built and tested against 6.18.42, the
  kernel `dev/kernel.sh` builds.
- One mount per domain directory, `mount -t mxlfs -o <options> mxlfs
  <dir>`, with the options `mode`, `size`, `wait` and `huge` described
  below.
- libmxl for writers and readers. On a domain mounted with `wait=`,
  releasing the last writer of a flow returns at once only with the
  domain watcher of the qvest-digital/mxl fork, which `dev/libmxl.sh`
  builds; upstream's looks the deleted flow up by name and waits out
  the full `wait`.

## What it does

- `kernel/`: the module.
  - Regular files keep their data in unevictable page cache. `fallocate`
    allocates the largest folios that fit, so a 1080p v210 grain
    (5537792 bytes) maps with two 2 MiB PMDs plus 1 MiB, 256 KiB and 32 KiB
    folios for its tail.
  - `huge=` has tmpfs's meanings. `within_size`, the default, is the
    above: every folio ends by EOF. With `huge=always`, a file of at least
    2 MiB takes a 2 MiB folio for its last partial 2 MiB, extending past
    EOF, so the grain above is three 2 MiB folios. A NIC maps a registered
    region with one page size, the largest every physical discontinuity in
    it allows, so the small tail folios hold the whole grain to pages of
    32 KiB to 256 KiB: 22 to 169 translation entries instead of 3. The
    cost is under 2 MiB per file of at least 2 MiB (736 KiB for the
    grain), charged against `size=`; a tail folio that `size=` has no room
    for falls back to folios within EOF. Smaller files are allocated as
    with `within_size`. A mapping faults SIGBUS past EOF as before, and the
    pages past EOF read as zeroes once the file grows over them.
  - There is no writeback and no `page_mkwrite`, so shared writable mappings
    can be long-term pinned for RDMA (`ibv_reg_mr`).
  - Views (`MXLFS_IOC_VIEW_SET_SOURCES`, `MXLFS_IOC_VIEW_SELECT`,
    `MXLFS_IOC_VIEW_SELECT_RANGE`): each page of a view selects a source.
    Selecting another source for a range zaps the view's mappings around
    it; readers refault onto the new source's folios. A source folio whose
    pages all select it maps whole, a PMD folio by one PMD; otherwise the
    view maps it page by page. `MXLFS_IOC_VIEW_REPLACE_SOURCE` puts another
    file in place of one source, as when a member's flow is recreated: the
    pages that select it are zapped the same way, and readers keep their
    mappings.
  - `size=` caps the bytes `fallocate` may allocate. As with tmpfs, the
    default is half of RAM and `size=0` means unlimited; statfs reports the
    cap (all of RAM when unlimited) and what is free under it.
  - Lazy allocation (`MXLFS_IOC_LAZY_ALLOC`, per thread group): that
    process's `fallocate` only sets the size, so a flow it creates through
    libmxl is listed and openable but its ring takes no memory; the 8 KiB
    grain headers libmxl writes at creation cost two pages per grain. A
    later `fallocate` from any other process, or `MXLFS_IOC_ALLOCATE` from
    any process, allocates the ring and folds
    the pages already there into 2 MiB folios, zapping mappings so readers
    refault onto them. This is how a node can list flows whose rings live
    elsewhere.
  - With `wait=<ms>`, a path walk through a missing `*.mxl-flow` name waits
    up to that long for the flow to appear instead of failing with ENOENT.
    It waits outside the directory lock, so the writer creating the flow is
    not held up; creating or renaming the flow into place wakes the walk,
    which is then retried. A process with CAP_SYS_ADMIN learns which flows
    are wanted (`MXLFS_IOC_WANTED_NEXT` on a directory) and can fail their
    waiters (`MXLFS_IOC_WANTED_FAIL`). Directory listings never show a
    flow that is only waited for.
  - The filesystem reports its own magic (`MXLFS_MAGIC`), so libmxl's
    `mxlIsTmpFs()` returns false for an mxlfs domain. libmxl itself never
    calls it; it is a helper for applications. Reporting tmpfs's magic
    would instead switch Smack, EVM and IMA paths that key on it, while
    the checks that matter here (long-term pinning, memfd seals) key on
    shmem's address_space operations and are unaffected either way.
- `tools/harness.c`: writers with a deterministic drop schedule, a merger
  that builds a view flow over member flows and replaces a member whose
  flow is recreated, readers that check every page
  of every grain or every sample, an RDMA peek and a switch microbenchmark.
  It covers video, ANC data (whose valid slices are a length, so a commit
  completes the grain) and audio (merged per page of each channel's ring,
  with a silence source for pages no member holds whole).
- `tools/setcheck/`: a writer that stamps each grain and sample with its
  member and index, and a reader that checks every one against the index
  it asked for and reports gaps, INVALID grains, latency and CPU time. It
  is built as an image for tests on a cluster, where the merge runs in
  the mxl-k8s gateway rather than in the harness.
- `tests/cluster/failover.sh`: video, ANC and audio sets with members on
  two nodes and readers on a third, or with MEMBER1 set to the READER
  node, through a member kill, its return and a drain of the other
  member's node, using setcheck. It fails when a set's reader had to
  reopen the output. `HOSTPATH` mounts the writers' domain from a node
  directory instead of through the CSI driver.
- `tests/`: the redundancy test, a member restart under readers of a view,
  views on their own, the folios each `huge=` gives, placeholder flows,
  waiting for missing flows, RDMA into mxlfs
  pages over soft-RoCE, and libmxl's own test suite on an mxlfs.

## Releases

Merging to `main` opens a release pull request; merging that tags the next
`vX.Y.Z-rc.N` and attaches the source tarball with its checksums, which the
talos-pkgs fork builds the module from. setcheck is published as
`ghcr.io/danielclasen/mxlfs-setcheck:<version>`.

## Build and test

Everything runs in a container built from `dev/Dockerfile` (libmxl comes from
the go-mxl builder image), with tests in a virtme-ng VM on the kernel version
Talos ships. The host needs Docker and `/dev/kvm`.

```sh
./dev/run.sh ./dev/kernel.sh                       # debug kernel: KASAN, lockdep, DEBUG_VM
./dev/run.sh make -C kernel KDIR=/src/build/linux-6.18.42
./dev/run.sh make -C tools
./dev/run.sh ./dev/vm.sh ./tests/redundancy.sh          # FORMAT=video|data|audio
./dev/run.sh ./dev/vm.sh ./tests/replace.sh             # FORMAT=video|data|audio
./dev/run.sh ./dev/vm.sh ./tests/wait.sh
./dev/run.sh ./dev/vm.sh ./tests/placeholder.sh
./dev/run.sh ./dev/vm.sh ./tests/views.sh
./dev/run.sh ./dev/vm.sh ./tests/huge.sh
./dev/libmxl.sh <checkout of qvest-digital/mxl>     # libmxl, its tests and tools
./dev/run.sh ./dev/vm.sh ./tests/libmxl.sh
b=build/mxl-src/build/Linux-Clang-Release
./dev/run.sh ./dev/vm.sh "DEMO=$b/tools/mxl-fabrics-demo/mxl-fabrics-demo \
  LD_LIBRARY_PATH=$b/lib:$b/lib/fabrics/ofi:/opt/libfabric/lib ./tests/rdma.sh"
```

`tests/rdma.sh` needs a libmxl with upstream ed1dcd1 on the target side,
which the libmxl of the dev image predates, so it runs against the
`dev/libmxl.sh` build. Under KASAN soft-RoCE moves well under 1080p50;
there it also needs a lower rate and a ring that covers the delivery lag:
`RATE=5 HISTORY_NS=10000000000`.

`MXLFS_OPTS` adds mount options to the mxlfs either script mounts, as in
`./dev/run.sh env MXLFS_OPTS=huge=always ./dev/vm.sh ./tests/redundancy.sh`.

On a real host, `sudo dev/host.sh ./tests/redundancy.sh` does the same with
the module built against the running kernel and an mxlfs at
`/run/mxlfs-test` (`MXLFS_MNT`).

`tests/redundancy.sh` is timing sensitive: it gives a member two grains to
deliver before an index is published INVALID, so a host that stalls the VM
for longer than that fails it with `unexpected_invalid` while `wrong_index`,
`torn` and `missed_invalid` stay 0. `tests/replace.sh` fails the same way
when a reader falls most of its 1 s ring behind: the merger retires a slot
to INVALID just before a member writes over it.

`FLAVOR=perf ./dev/run.sh env FLAVOR=perf ./dev/kernel.sh` builds a kernel
without the debug options in `build/linux-6.18.42-perf`; point `KDIR` at it
for both the module build and `dev/vm.sh`.

## Using a view

```c
int src[2] = { open(".../a.mxl-flow/grains/data.0", O_RDONLY),
               open(".../b.mxl-flow/grains/data.0", O_RDONLY) };
int view = open(".../v.mxl-flow/grains/data.0", O_CREAT | O_RDWR, 0664);
ftruncate(view, grain_size);                  /* same size as the sources */
struct mxlfs_view_sources req = { .count = 2, .fds = { src[0], src[1] } };
ioctl(view, MXLFS_IOC_VIEW_SET_SOURCES, &req);
__u32 sel = 1;
ioctl(view, MXLFS_IOC_VIEW_SELECT, &sel);     /* readers now map b's pages */
/* b's flow was recreated: its new grain file takes b's place. */
struct mxlfs_view_replace rep = { .source = 1, .fd = new_b };
ioctl(view, MXLFS_IOC_VIEW_REPLACE_SOURCE, &rep);
```

A view is read-only. Its sources cannot be resized while it exists. A view
must never be registered with a NIC: a pinned MR keeps the pages it pinned
and does not follow a switch.

## License

- `kernel/`: GPL-2.0-only (`LICENSES/GPL-2.0-only.txt`). The module uses
  kernel interfaces exported to GPL modules only.
- `kernel/mxlfs_uapi.h`: GPL-2.0-only WITH Linux-syscall-note
  (`LICENSES/Linux-syscall-note.txt`), as the kernel's own uapi headers, so
  programs under any license can include it to use the ioctls.
- Everything else: Apache-2.0 (`LICENSE`).

Each file under `kernel/` carries an SPDX identifier, as kernel code does.

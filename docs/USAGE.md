# Using mxlfs

How each feature behaves, with its mount option or ioctl, its errors and
its limits. The ioctls, structs and constants are in
[`kernel/mxlfs_uapi.h`](../kernel/mxlfs_uapi.h), whose comments are the
reference this page follows. Every ioctl's type is `'x'`.

## Mounting

```sh
mount -t mxlfs -o size=8G,huge=always,numa_node=1,wait=2000 mxlfs /dev/shm/domain
```

One mount per MXL domain directory. A mount of type `mxlfs` loads the
module on demand.

| Option | Value | Default | Effect |
| --- | --- | --- | --- |
| `mode=` | octal | `0755` | mode of the root directory |
| `size=` | bytes, with an optional `K`, `M`, `G`, `T`, `P` or `E` suffix | half of RAM | cap on what the mount allocates; `0` means unlimited ([Size and statfs](#size-and-statfs)) |
| `huge=` | `within_size`, `always` | `within_size` | folio sizes at the end of a file ([Large folios](#large-folios)) |
| `numa_node=` | node number | none | node every page of the mount prefers ([NUMA placement](#numa-placement)) |
| `wait=` | milliseconds | `0`, off | how long a walk waits for a missing flow ([Waiting for missing flows](#waiting-for-missing-flows)) |

A `numa_node=` that names no node with memory, an unknown `huge=` value
and a `size=` that is not a number fail the mount with EINVAL.
`/proc/mounts` shows `size=` always and the other options when they
differ from the default.

A remount may repeat the options with the values the mount has, give
none, or change only VFS flags such as `ro`. A remount that gives
`mode=`, `size=`, `huge=`, `numa_node=` or `wait=` with another value fails
with EINVAL; unmount and mount again to change them.

## Files

Directories, regular files, symlinks, hard links, device nodes, fifos and
sockets work as on tmpfs, as do `O_TMPFILE` and `rename()` with
`RENAME_NOREPLACE` and `RENAME_EXCHANGE`. Regular files differ:

- Their data is page cache that is never evicted, written back or
  swapped. There is no backing store.
- There is no writeback and no `page_mkwrite`, so a shared writable
  mapping can be long-term pinned, as `ibv_reg_mr` does to register a
  ring with a NIC.
- `fallocate` allocates at once ([Large folios](#large-folios)), except
  for a process that allocates lazily
  ([Lazy allocation](#lazy-allocation)). It takes mode 0 or
  `FALLOC_FL_KEEP_SIZE`; any other mode fails with EOPNOTSUPP.
- A mapping faults SIGBUS past EOF.

### Large folios

`fallocate` allocates the largest folios that are aligned and fit, up to
2 MiB, so a 2 MiB aligned range maps with one PMD. A 1080p v210 grain
(5537792 bytes) is two 2 MiB folios plus 1 MiB, 256 KiB and 32 KiB ones.

`huge=` has tmpfs's meanings:

- `within_size`, the default: every folio ends by EOF.
- `always`: a file of at least 2 MiB takes a 2 MiB folio for its last
  partial 2 MiB, extending past EOF, so the grain above is three 2 MiB
  folios. A NIC maps a registered region with one page size, the largest
  every physical discontinuity in it allows, so the small tail folios of
  `within_size` hold the whole grain to pages of 32 KiB to 256 KiB. The
  cost is under 2 MiB per file of at least 2 MiB, charged against
  `size=`; a tail folio that `size=` has no room for falls back to folios
  within EOF. Smaller files are allocated as with `within_size`. The
  pages past EOF read as zeroes once the file grows over them.

A mapping of a regular file is made `VM_HUGEPAGE`, so a fault on a hole
allocates 2 MiB folios, except in a lazily sized file
([Lazy allocation](#lazy-allocation)).

### Size and statfs

`size=` caps the bytes the mount allocates. `fallocate` and `write()`
past it fail with ENOSPC, and a fault that would allocate past it raises
SIGBUS. Pages that appear outside `fallocate`, from faults, reads of holes
and `write()`, are checked before they are allocated, so concurrent callers
can overshoot the cap by one folio each. `statfs` reports the cap as the
filesystem's size, all of RAM when unlimited, and what is free under it.
`bytes_allocated` and `bytes_limit` in
[`MXLFS_IOC_STATS`](#mxlfs_ioc_stats) are the bytes allocated and the
cap, 0 when unlimited.

## Lazy allocation

A process can create flows whose rings take no memory until something
needs them, so a node can list flows whose rings live elsewhere.

```c
__u32 on = 1;
ioctl(fd, MXLFS_IOC_LAZY_ALLOC, &on);   /* any fd of the mount */
```

- `MXLFS_IOC_LAZY_ALLOC` (`_IOW('x', 7, __u32)`): 1 makes the calling
  thread group's `fallocate` calls on this mount set only the file size;
  0 turns it off; other values fail with EINVAL. It applies on a
  directory fd or a file fd of the mount, needs no privilege, and ends
  with the thread group.
- A lazily sized file gets its pages on first touch, 4 KiB at a time,
  from `write()` and from faults on its mappings, which are not made
  `VM_HUGEPAGE`, so writing a grain header costs only its own pages: two
  per grain for the 8 KiB headers libmxl writes at creation.
- A later `fallocate` from a process that is not lazy, or
  `MXLFS_IOC_ALLOCATE` (`_IO('x', 8)`) on a file open for writing from
  any process, allocates the rest in large folios and folds the pages
  already there into them. Mappings of the file are zapped and readers
  refault onto the large folios. `MXLFS_IOC_ALLOCATE` on a file not open
  for writing fails with EBADF; otherwise it fails as `fallocate` would,
  and raises the modify event `fallocate` raises. On an empty file other
  than a view it does nothing and returns 0, where `fallocate` refuses a
  zero length with EINVAL.

A fold is skipped for a range a [view](#views) maps or that holds a
pinned page; those pages stay as they are.

## Views

A view is a file whose pages are those of several equally sized source
files, each page taken from the source selected for it. Selecting another
source changes what readers of the view map without copying data: the
view's mappings of the range are zapped and readers refault onto the
selected source's folios. A source folio whose pages all select it maps
whole, a 2 MiB folio with one PMD; otherwise the view maps it page by
page.

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

### Making a view

`MXLFS_IOC_VIEW_SET_SOURCES` (`_IOW('x', 1, struct mxlfs_view_sources)`)
on a regular file open for writing turns it into a view of `count`
sources, `fds[0..count-1]`; the entries past `count` are unused and
must be 0. Every page starts out selecting source 0. `flags` is checked
first, on any regular file, so `flags = ~0` probes for the ioctl as for
[`MXLFS_IOC_VIEW_REPLACE_SOURCE`](#replacing-a-source).

| Error | When |
| --- | --- |
| EBADF | the file is not open for writing, or a source fd is not open for reading |
| EINVAL | `flags` is not 0; `count` is 0 or more than `MXLFS_VIEW_MAX_SOURCES` (8); an entry of `fds` past `count` is not 0; a source is on another mount, is not a regular file, is the file itself or is a view; a source's size differs from the file's; the file's size is 0 or more than `MXLFS_VIEW_MAX_SIZE` (4 GiB); the file was resized during the call |
| EBUSY | the file is a view already, has pages or mappings of its own, or is a source of a view |
| ENOMEM | the selection table could not be allocated |

A view keeps one byte per page for its selection, 1 MiB at 4 GiB, charged
to the memory cgroup of the process that makes it.

Map a file only once it is a view: a mapping made while the call turns
it into one can succeed, but then gets SIGBUS on every access, a shared
writable one included, which a view otherwise refuses with EACCES; a
mapping that exists before the call makes it fail with EBUSY. A `read()`
that races the call can leave pages in the view's own page cache, read
from holes. They count in [`MXLFS_IOC_NODE_PAGES`](#mxlfs_ioc_node_pages)
and against `size=`, at most the view's size, and are freed with the
inode; nothing of the view maps or reads them.

### Selecting

Each fails with EINVAL on a file that is not a view. The two that select
need the view open for writing (EBADF otherwise).

- `MXLFS_IOC_VIEW_SELECT` (`_IOW('x', 2, __u32)`): every page selects the
  given source.
- `MXLFS_IOC_VIEW_SELECT_RANGE`
  (`_IOW('x', 9, struct mxlfs_view_range)`): the pages of
  `[offset, offset + length)` select `source`. `offset` must be page
  aligned and inside the view; the end is rounded up to a page and
  clipped to the view. EINVAL for `flags` other than 0, a source out of
  range, `length` 0 or a bad `offset`.
- `MXLFS_IOC_VIEW_GET_SELECTED` (`_IOR('x', 3, __u32)`): the source the
  first page selects. It needs only a view, not write access.

Selecting the source a range already selects changes nothing and zaps
nothing. `view_selects` in [`MXLFS_IOC_STATS`](#mxlfs_ioc_stats) counts
the calls that changed a selection, `view_faults` the faults on views.

### Replacing a source

`MXLFS_IOC_VIEW_REPLACE_SOURCE`
(`_IOW('x', 10, struct mxlfs_view_replace)`) makes the file at `fd` source
number `source` in place of the file there, as when a member's flow is
recreated. The pages that select it are zapped and refault onto the new
file; readers keep their mappings and the selection stays as it is.
Replacing a source with the file already there changes nothing.

`flags` is checked first, on any regular file: unknown bits fail with
EINVAL, so `flags = ~0` tells whether the module has the ioctl (EINVAL)
or not (ENOTTY); a directory answers ENOTTY. Then EINVAL if the file is
not a view, EBADF if it is not open for writing, EINVAL for a source
number out of range, EBADF if `fd` is not open for reading, and EINVAL
if the file at `fd` cannot be a source of the view, by the rules of
[Making a view](#making-a-view). `view_replaces` in
[`MXLFS_IOC_STATS`](#mxlfs_ioc_stats) counts the calls that changed a
source.

### What a view refuses

- A view is read-only: a shared writable mapping of it fails with
  EACCES. A view has no pages of its own, so `write()`, `fallocate` and
  the ioctls that allocate or place pages, `MXLFS_IOC_ALLOCATE`,
  `MXLFS_IOC_SET_NODE` and `MXLFS_IOC_MOVE`, as well as `MXLFS_IOC_HOLD`,
  fail on it with EPERM. The view ioctls fail with EINVAL on a file that
  is not a view; the split is deliberate.
- While a view uses a file, `truncate`, `fallocate` and `write()` cannot
  change that file's size: EBUSY.
- A pinned registration does not follow a selection change: a NIC keeps
  the pages it pinned. Register the sources, never a view.
- Truncating a view to another size fails with EBUSY.
- A fault on a page that is a hole in its selected source raises SIGBUS,
  while `read()` returns zeroes there: allocate the sources, including
  lazily sized ones, before readers map the view.
- [`MXLFS_IOC_MOVE`](#moving-pages-between-nodes) leaves a folio a view
  maps where it is.

## Waiting for missing flows

With `wait=<ms>`, a path walk through a missing name ending in
`.mxl-flow` (and not starting with `.`) waits up to that long for the
flow to appear instead of failing with ENOENT at once, so a reader can
open a flow whose writer starts later. Only a plain lookup waits: an
`open()` with `O_CREAT` or a rename onto the name sees it missing at once.

- The walk waits outside the directory lock, so the writer creating the
  flow is not held up. Creating the flow, or renaming it into place, wakes
  the walk, which is then retried and finds it.
- The walk fails with ENOENT when the time is up. Before that, only the
  flow appearing, `MXLFS_IOC_WANTED_FAIL` or a fatal signal ends it;
  other signals do not interrupt it.
- A directory listing never shows a flow that is only waited for.
- A walk with openat2's `RESOLVE_NO_XDEV` does not wait: it fails a
  missing flow with ENOENT at once. While any other walk holds the name,
  waiting or not (looking it up, refused by a cap, or just woken), it
  gets EXDEV instead: the VFS refuses such a walk a name another walk may
  wait on, as it does an autofs mount point.
- A walk through a missing flow whose path from the filesystem root is
  longer than 4075 bytes fails with ENAMETOOLONG at once instead of
  waiting, unless a cap below is full: `MXLFS_IOC_WANTED_NEXT` could not
  report the path.
- A mount waits for at most 4096 missing flows at once, and a thread
  group's walks start waits for at most 256 of them; a walk that joins a
  wait already there counts against neither. A walk past either cap
  fails with ENOENT at once, as without `wait=`, and counts in
  `wanted_refused` of
  [`MXLFS_IOC_STATS`](#mxlfs_ioc_stats); `wanted` there counts the flows
  waited for now. The thread group cap keeps one process from taking the
  whole mount's share; it is no security boundary, since a process can
  fork more thread groups. What a waiting walk holds is charged to its
  memory cgroup.

A process with CAP_SYS_ADMIN learns which flows are wanted and can fail
their waiters, on a directory fd of the mount:

- `MXLFS_IOC_WANTED_NEXT` (`_IOWR('x', 5, struct mxlfs_wanted_req)`):
  waits up to `timeout_ms` for a missing flow not yet reported and returns
  its `id`, its `path` from the filesystem root, and `pid`, the first
  waiter's thread group id in the caller's pid namespace or 0 outside it.
  ETIMEDOUT if none appears. Any signal interrupts the wait. After the
  CAP_SYS_ADMIN check, `flags` is checked: it must be 0, EINVAL
  otherwise, and is written back as 0. A flow counts as reported only
  once the struct is written back: EFAULT there leaves it to the next
  call, and the struct's contents are then undefined. `path` holds at
  most 4075 bytes and its NUL.
- `MXLFS_IOC_WANTED_FAIL` (`_IOW('x', 6, struct mxlfs_wanted_fail)`):
  fails the waiters of flow `id` with the errno `error`, which must be
  from 1 to 511, as FUSE takes from its daemon; 512 and up are the
  kernel's own. EINVAL for another `error` or a `pad` other than 0,
  ENOENT for an unknown `id`.

Both work on a directory fd of the mount and fail with ENOTTY on another
file; there, the CAP_SYS_ADMIN check comes first (EPERM), then `flags`
or the other input.

## NUMA placement

A page of a regular file comes from the file's preferred node if it has
one with memory, else from the mount's `numa_node=`, else from the node
the allocating process's memory policy picks, by default the node it runs
on, as on tmpfs. That covers `fallocate`, the allocation of a lazily sized
file, faults on holes, `write()` and reads of holes.

A preferred node is preferred, not required: when it has no free memory a
page comes from another node, and no allocation fails that would succeed
without it. The fallback is counted in `numa_fallback_folios` and
`numa_fallback_bytes` of [`MXLFS_IOC_STATS`](#mxlfs_ioc_stats) and per node
in [`MXLFS_IOC_MOUNT_NODES`](#mxlfs_ioc_mount_nodes). The process's memory
policy does not apply to a preferred node; its cpuset's memory nodes do.

`MXLFS_IOC_SET_NODE` (`_IOW('x', 11, struct mxlfs_node_pref)`), on a
regular file that is not a view, with CAP_SYS_NICE: later allocations of
the file prefer `node`, ahead of `numa_node=`; `MXLFS_NODE_NONE` (-1)
clears the preference. Pages already there stay where they are. EINVAL
for a node without memory or `flags` other than 0, EPERM on a view or
without CAP_SYS_NICE, EBUSY while another open file holds the file
([`MXLFS_IOC_HOLD`](#holding-a-file)).

## Moving pages between nodes

`MXLFS_IOC_MOVE` (`_IOWR('x', 13, struct mxlfs_move)`) moves an existing
file's pages, such as a running flow's ring, to NUMA-near memory in
place: offsets and contents stay, readers and writers keep their
mappings, and a process touching a folio while it moves waits for that
one folio and then maps the copy, a 2 MiB folio again with one PMD.

On a regular file that is not a view, with CAP_SYS_NICE (none for a dry
run), every folio not on `node` moves there, one at a time:

- A folio of the same size is reserved on `node` for every folio first.
  If any reservation fails, nothing moves: ENOMEM, reason
  `MXLFS_REFUSE_NOMEM`.
- Each folio is copied while its mappings stay readable, then its
  mappings are zapped and the copy takes its place. A folio written to
  while it was copied is tried again after the others, and copied once
  more while zapped on the last try. The call holds the file's inode lock,
  so `write()`, `fallocate` and truncation wait for it.
- A folio is left where it is, and reported, if it is long-term pinned
  (`MXLFS_REFUSE_PINNED`, as an RDMA registration), mapped by a view
  (`MXLFS_REFUSE_VIEW`), or holds references beyond its mappings and the
  page cache (`MXLFS_REFUSE_BUSY`). The first such folio sets `reason` and
  `refused_offset`; the call goes on with the next folio and returns 0
  with `result` `MXLFS_MOVED_PARTIAL` or `MXLFS_MOVED_NONE`. A fault that
  is waiting to map a folio holds such a reference while it waits; a later
  call can move the folio.
- With `MXLFS_MOVE_STRICT`, every folio is first checked for a pin or a
  view's mapping, and such a folio fails the call with EBUSY before
  anything moves; one refused later fails it with EAGAIN, the folios
  moved so far staying moved.
- `MXLFS_MOVE_DRY_RUN` changes nothing and takes no lock: it reports what
  a move would find before it starts, pins and holds, not a view's
  mappings. The record it keeps of each folio off the node while it runs
  is charged to the caller's memory cgroup, as a move's is.
- EBUSY with reason `MXLFS_REFUSE_HELD` when another open file holds the
  file and a folio is off the node; EINTR with `MXLFS_REFUSE_SIGNAL` on a
  fatal signal, between folios; EINVAL for unknown `flags` or a node
  without memory; EPERM on a view, a dry run included, and without
  CAP_SYS_NICE, unless it is a dry run.

The call reports `result` (`MXLFS_MOVED_ALL`, `MXLFS_MOVED_PARTIAL`,
`MXLFS_MOVED_NONE`), `blocks`, the folios of the file, `blocks_on_node`,
those on the node before the call, `blocks_moved`, `blocks_refused`,
`bytes_moved`, `duration_ns` for the whole call, and `max_window_ns`,
its longest block window. A block window is per folio moved: from the
last check of the folio for writes and its unmap until its copy is
unlocked, the longest a fault on that folio waits. When the result is
`MXLFS_MOVED_ALL` and no other open file holds the file, the file's
preferred node becomes `node`.

The mount counts calls, results, folios, bytes, time, refusals by reason
and block windows in [`MXLFS_IOC_STATS`](#mxlfs_ioc_stats), and pages moved
in and out per node in [`MXLFS_IOC_MOUNT_NODES`](#mxlfs_ioc_mount_nodes).

### Holding a file

`MXLFS_IOC_HOLD` (`_IOW('x', 14, __u32)`), with CAP_SYS_NICE: 1 takes, 0
drops this open file's hold on its file. While another open file holds a
file, `MXLFS_IOC_SET_NODE` through this one fails with EBUSY, and
`MXLFS_IOC_MOVE` refuses as described
[above](#moving-pages-between-nodes); through the holding file both work. A hold belongs to the
open file, so `dup()` and `fork()` share it, and it ends with the file's
last close, as when its process dies. EINVAL for a value other than 0
or 1, EPERM on a view. `holds` in
[`MXLFS_IOC_STATS`](#mxlfs_ioc_stats) counts the open files holding a file
now.

## Counters

All counters are read without a lock and without privilege, except with
`MXLFS_NODE_PAGES_RECOUNT`.

### MXLFS_IOC_STATS

`_IOR('x', 4, struct mxlfs_stats)` on any fd of the mount. The module
takes the size of the caller's struct from the command and copies that
much, or all of its own and zeroes the rest if the caller's is larger,
so a program built against an older header keeps working and one built
against a newer header reads fields the module does not have as 0. A
size below `MXLFS_STATS_SIZE_VER0` (32 bytes, the struct before
`view_replaces`) fails with EINVAL, not the ENOTTY of a command the
module does not match. The `move_*` counters count since the mount; a
dry run counts only in `dry_runs`.

| Field | Counts |
| --- | --- |
| `bytes_allocated` | bytes the mount has allocated |
| `bytes_limit` | the `size=` cap, 0 when unlimited |
| `view_selects` | select calls that changed a selection |
| `view_faults` | faults on views; each maps one folio |
| `view_replaces` | replace calls that changed a source |
| `numa_fallback_folios`, `numa_fallback_bytes` | folios, and their bytes, that preferred a node and got another |
| `move_calls` | `MXLFS_IOC_MOVE` calls |
| `move_calls_partial` | calls with result `MXLFS_MOVED_PARTIAL` |
| `move_calls_none` | calls with result `MXLFS_MOVED_NONE` |
| `move_blocks`, `move_bytes` | folios moved, and their bytes |
| `move_ns` | the sum of call durations |
| `move_refused[8]` | folios left off the node, indexed by `MXLFS_REFUSE_*` (`MXLFS_REFUSE_MAX` entries) |
| `move_window_ns_sum`, `move_window_ns_max` | the sum and the longest of the block windows |
| `move_window_hist[9]` | block windows under 64 us, 128 us, 256 us, 512 us, 1 ms, 2 ms, 4 ms, 8 ms, and 8 ms or more |
| `holds` | open files holding a file now |
| `dry_runs` | `MXLFS_IOC_MOVE` dry runs |
| `wanted` | missing flows walks wait for now |
| `wanted_refused` | walks that found a flow missing and did not wait, the caps being full |

### MXLFS_IOC_NODE_PAGES

`_IOWR('x', 12, struct mxlfs_node_pages)` on any open regular file:
`pages_ptr` receives the file's pages per node for the first `nr_nodes`
nodes, and `nr_nodes` is set to the number of node ids; `pref` is the
file's preferred node, `effective` the node its allocations prefer, and
`holds` the open files holding it. A page counts while it is in the page
cache and uptodate. With `MXLFS_NODE_PAGES_RECOUNT` (CAP_SYS_ADMIN) the
pages are counted by walking the file's page cache under its locks
instead, which blocks the file's writers. Other `flags` bits or a `pad`
other than 0 fail with EINVAL.

### MXLFS_IOC_MOUNT_NODES

`_IOWR('x', 15, struct mxlfs_mount_nodes)` on any fd of the mount: fills
`min(nr, number of node ids)` `struct mxlfs_node_stat` records at
`stats_ptr` and sets `nr` to the number of node ids. `flags` must be 0
(EINVAL otherwise).

| Field | Counts, for one node |
| --- | --- |
| `pages` | the mount's pages on the node now |
| `fallback_folios`, `fallback_pages` | folios, and their pages, that preferred the node and got another |
| `moved_in_pages`, `moved_out_pages` | pages `MXLFS_IOC_MOVE` moved to and from the node since the mount |

## Filesystem magic

`statfs` reports `MXLFS_MAGIC` (`0x6d786c66`), not tmpfs's, so libmxl's
`mxlIsTmpFs()` returns false for an mxlfs domain.

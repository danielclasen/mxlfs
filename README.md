# mxlfs

A Linux memory filesystem for [MXL](https://github.com/dmf-mxl/mxl) domains.
An MXL domain is a directory of shared-memory ring buffers that writers and
readers `mmap`; mxlfs is a drop-in for the tmpfs such a domain usually lives
on, plus views: files whose pages are switched between several equally sized
source files without copying.

With views, one flow can be served from the rings of several member flows,
choosing per grain the member that holds it complete. Readers are unmodified
libmxl readers; they see an ordinary flow.

## Status

An experiment. What exists:

- `kernel/`: the module.
  - Regular files keep their data in unevictable page cache. `fallocate`
    allocates the largest folios that fit, so a 1080p v210 grain
    (5537792 bytes) maps with two 2 MiB PMDs plus 1 MiB, 256 KiB and 32 KiB
    folios for its tail.
  - There is no writeback and no `page_mkwrite`, so shared writable mappings
    can be long-term pinned for RDMA (`ibv_reg_mr`).
  - Views (`MXLFS_IOC_VIEW_SET_SOURCES`, `MXLFS_IOC_VIEW_SELECT`):
    selecting another source zaps the view's mappings; readers refault onto
    the new source's folios, with PMD folios mapped by one PMD.
  - `size=` caps the bytes `fallocate` may allocate. As with tmpfs, the
    default is half of RAM and `size=0` means unlimited; statfs reports the
    cap (all of RAM when unlimited) and what is free under it.
  - Lazy allocation (`MXLFS_IOC_LAZY_ALLOC`, per thread group): that
    process's `fallocate` only sets the size, so a flow it creates through
    libmxl is listed and openable but its ring takes no memory; the 8 KiB
    grain headers libmxl writes at creation cost two pages per grain. A
    later `fallocate` from any other process allocates the ring and folds
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
  that builds a view flow over member flows, readers that check every page
  of every grain, an RDMA peek and a switch microbenchmark.
- `tests/`: the redundancy test, placeholder flows, waiting for missing flows, RDMA into mxlfs
  pages over soft-RoCE, and libmxl's own test suite on an mxlfs.

Not there yet: per-pod policy, the merge engine in a gateway, and
measurements on hardware NICs.

## Build and test

Everything runs in a container built from `dev/Dockerfile` (libmxl comes from
the go-mxl builder image), with tests in a virtme-ng VM on the kernel version
Talos ships. The host needs Docker and `/dev/kvm`.

```sh
./dev/run.sh ./dev/kernel.sh                       # debug kernel: KASAN, lockdep, DEBUG_VM
./dev/run.sh make -C kernel KDIR=/src/build/linux-6.18.42
./dev/run.sh make -C tools
./dev/run.sh ./dev/vm.sh ./tests/redundancy.sh
./dev/run.sh ./dev/vm.sh ./tests/wait.sh
./dev/run.sh ./dev/vm.sh ./tests/placeholder.sh
./dev/run.sh ./dev/vm.sh ./tests/rdma.sh
./dev/libmxl.sh <checkout of qvest-digital/mxl>     # then:
./dev/run.sh ./dev/vm.sh ./tests/libmxl.sh
```

On a real host, `sudo dev/host.sh ./tests/redundancy.sh` does the same with
the module built against the running kernel and an mxlfs at
`/run/mxlfs-test` (`MXLFS_MNT`).

`tests/redundancy.sh` is timing sensitive: it gives a member two grains to
deliver before an index is published INVALID, so a host that stalls the VM
for longer than that fails it with `unexpected_invalid` while `wrong_index`,
`torn` and `missed_invalid` stay 0.

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

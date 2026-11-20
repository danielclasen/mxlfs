# mxlfs

A Linux memory filesystem for [MXL](https://github.com/dmf-mxl/mxl) domains.
An MXL domain is a directory of shared-memory ring buffers that writers and
readers `mmap`. mxlfs takes the place of the tmpfs such a domain usually
lives on, and libmxl writers and readers run on it unmodified.

## Features

- Large folios: `fallocate` allocates 2 MiB folios where alignment
  allows, so a ring maps with PMDs and a NIC registration needs few
  translation entries; `huge=always` gives a file's tail a 2 MiB folio as
  well. [Large folios](docs/USAGE.md#large-folios)
- Pinnable memory: file data never leaves memory, and there is no
  writeback, so a writable mapping can be pinned for an RDMA registration.
  [Files](docs/USAGE.md#files)
- Views: a file whose pages come from several equally sized source
  files, chosen per page and switched without copying, so one flow can be
  served from whichever member flow holds each grain complete; readers
  see an ordinary flow. A source can be replaced in place when its flow is
  recreated. [Views](docs/USAGE.md#views)
- Lazy allocation: a process can create flows whose rings take no
  memory until another process allocates them, so a node can list flows
  whose rings live elsewhere. [Lazy allocation](docs/USAGE.md#lazy-allocation)
- Waiting for missing flows: with `wait=`, opening a flow that does not
  exist yet waits for its writer instead of failing, and a privileged
  process can learn which flows are wanted and fail their waiters.
  [Waiting for missing flows](docs/USAGE.md#waiting-for-missing-flows)
- NUMA placement: a mount or a single file can prefer a NUMA node, such
  as the one a NIC is attached to, whichever node the writer runs on.
  [NUMA placement](docs/USAGE.md#numa-placement)
- Moving pages between nodes: a running flow's ring can be moved to
  another node in place, while its writers and readers keep their
  mappings. [Moving pages between nodes](docs/USAGE.md#moving-pages-between-nodes)
- A size cap and counters: `size=` caps what a mount allocates, and
  ioctls report its allocation, view, move and wait counters and its pages
  per node. [Counters](docs/USAGE.md#counters)

The mount options are in [Mounting](docs/USAGE.md#mounting).

## Requirements

- Linux 6.18 or newer with `CONFIG_TRANSPARENT_HUGEPAGE`.
- libmxl for writers and readers. On a domain mounted with `wait=`,
  releasing the last writer of a flow returns at once only with the domain
  watcher of the qvest-digital/mxl fork; upstream's looks the deleted flow
  up by name and waits out the full `wait`.

## Install

The module builds against the kernel's headers on the machine or node it
runs on:

- Debian and Ubuntu: the `mxlfs-dkms` package each release carries.
  [Debian and Ubuntu](docs/BUILDING.md#debian-and-ubuntu)
- Other distributions: DKMS from the release's source tarball.
  [Other distributions with DKMS](docs/BUILDING.md#other-distributions-with-dkms)
- From source: `make -C kernel`, `modules_install` and `depmod`.
  [From source](docs/BUILDING.md#from-source)
- Kubernetes on Ubuntu and Debian nodes: the KMM operator.
  [Kubernetes with KMM](docs/BUILDING.md#kubernetes-with-kmm)
- Talos: a system extension. [Talos](docs/BUILDING.md#talos)

Building, testing, CI and releases are in
[docs/BUILDING.md](docs/BUILDING.md).

## License

- `kernel/`: GPL-2.0-only (`LICENSES/GPL-2.0-only.txt`). The module uses
  kernel interfaces exported to GPL modules only.
- `kernel/mxlfs_uapi.h`: GPL-2.0-only WITH Linux-syscall-note
  (`LICENSES/Linux-syscall-note.txt`), as the kernel's own uapi headers, so
  programs under any license can include it to use the ioctls.
- Everything else: Apache-2.0 (`LICENSE`).

Each file under `kernel/` carries an SPDX identifier, as kernel code does.

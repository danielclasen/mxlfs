# Building, testing and packaging mxlfs

How mxlfs is installed, built, tested and released.
[USAGE.md](USAGE.md) describes how it behaves once mounted.

## Install

The module needs the kernel's headers to build, wherever it is built:
`linux-headers-generic` on Ubuntu, `linux-headers-amd64` from
trixie-backports on Debian. A
mount of type `mxlfs` loads the module on demand. Loading it at boot is
optional:

```sh
echo mxlfs | sudo tee /etc/modules-load.d/mxlfs.conf
```

### Debian and Ubuntu

Each release carries `mxlfs-dkms_<version>_all.deb`, `<version>` being
the release's, as `0.1.0-rc.7`; the package inside is versioned
`0.1.0~rc.7`, so that it sorts before `0.1.0`. DKMS builds the
module for every installed kernel of 6.18 or newer, now and whenever a
kernel is installed later, and skips older ones, such as Debian 13's own
6.12.

On Debian 13, a kernel DKMS builds for comes from trixie-backports,
which keeps the headers of its most recent kernels only:

```sh
sudo apt install -t trixie-backports linux-image-amd64 linux-headers-amd64
```

Then, on either distribution:

```sh
sudo apt install ./mxlfs-dkms_<version>_all.deb
dkms status mxlfs                # mxlfs/<version>, <kernel>, x86_64: installed
sudo modprobe mxlfs
```

With Secure Boot on, DKMS signs the module, and the kernel loads it once
the signing key is enrolled as a Machine Owner Key:

- Debian: DKMS generates a key on its first build if there is none, with
  the certificate at `/var/lib/dkms/mok.pub`. Enrol it with
  `sudo mokutil --import /var/lib/dkms/mok.pub` and reboot.
- Ubuntu: DKMS signs with the MOK of `shim-signed`,
  `/var/lib/shim-signed/mok/MOK.der`, which it creates with
  `update-secureboot-policy` if there is none. If that key is not enrolled
  yet, enrol it with `sudo update-secureboot-policy --enroll-key` and
  reboot.

Either command asks for a password, which the MOK manager asks for at the
next boot to confirm the enrolment.

### Other distributions with DKMS

From the release's source tarball, or a checkout. `dkms.conf` limits the
build to kernels of 6.18 or newer with `CONFIG_TRANSPARENT_HUGEPAGE`
(`BUILD_EXCLUSIVE_KERNEL_MIN` and `BUILD_EXCLUSIVE_CONFIG`); DKMS skips
other kernels:

```sh
sudo dkms add ./mxlfs-<version>   # or the checkout directory
sudo dkms install mxlfs/<version>
```

### From source

```sh
make -C kernel                       # KDIR=<kernel build tree> for a kernel other than the running one
sudo make -C kernel modules_install  # into /lib/modules/<kernel>/updates/
sudo depmod "$(uname -r)"            # or the kernel KDIR is for
sudo modprobe mxlfs
```

`depmod` is a step of its own because `modules_install` skips it when the
kernel build tree has no `System.map`, as with the distributions' kernel
headers packages, and `modprobe` then does not find the module.

A module built this way is for that one kernel; rebuild it after each
kernel update, or use DKMS.

### Kubernetes with KMM

On Ubuntu 26.04 and Debian 13 (backports kernel) nodes the
[Kernel Module Management](https://kmm.sigs.k8s.io) operator (KMM) can
build and load the module. `kmm/render.sh <version> <registry>` prints a
privileged `mxlfs` namespace, a `Module` that KMM builds an image for per
node kernel from `kmm/Dockerfile` (the kernel's headers from the
distribution archive and the release's source tarball, checked against
its published SHA-256) and pushes to `<registry>`, and the ConfigMap
holding that Dockerfile. KMM needs cert-manager; see its documentation
for installing both.

```sh
./kmm/render.sh <version> registry.example.com/team | kubectl apply -f -
kubectl -n mxlfs create secret docker-registry mxlfs-kmm-registry \
  --docker-server=registry.example.com --docker-username=... --docker-password=...
```

KMM labels a node `kmm.node.kubernetes.io/mxlfs.mxlfs.ready` once the
module is loaded there.

The Ubuntu mapping builds against the Ubuntu 26.04 archive and matches
`-generic` kernels of 6.18 or newer. Ubuntu 24.04's HWE kernel is named
like a 26.04 kernel (`7.0.0-<n>-generic`) and matches as well, so in a
cluster that also has nodes on another Ubuntu release or kernel flavour
(`-aws`, `-azure`), label the 26.04 nodes, set this `Module`'s
`selector` to that label, and give the other nodes a `Module` of their
own with their release as `BASE_IMAGE`. The Debian mapping matches the
trixie-backports kernels; Debian 13's own 6.12 is too old for mxlfs and
gets no module.

The build installs the headers of the node's running kernel from the
distribution archive, so it works only while the archive still has
them. Debian's trixie-backports keeps the images of older kernels but
the headers of its most recent ones only, so a Debian node has to run
the current backports kernel. On either distribution, a node that has
installed a newer kernel but not rebooted into it can fail to build
until it reboots. DKMS needs the headers the same way, for every kernel
it builds for.

Known issue on Ubuntu 26.04 nodes: from kernel 7.0.0-41 on, container
AppArmor labels stack (`apparmor_restrict_unprivileged_unconfined=1`),
and containerd's default profile then denies signals inside containers
([containerd#14485](https://github.com/containerd/containerd/issues/14485),
fixed by
[containerd#14487](https://github.com/containerd/containerd/pull/14487),
merged 2026-11-16 and in no containerd release as of 2026-11-20). kaniko, the
builder KMM runs in the cluster, kills each `RUN` step's process group,
so a `RUN` that leaves a process behind fails with "permission denied".
`kmm/Dockerfile` leaves none; another Dockerfile built through KMM on
such nodes needs the same, or a containerd with the fix. KMM v2.7.0
cannot set an AppArmor profile on its build pod, so running that pod
unconfined needs an admission webhook.

### Talos

KMM cannot load modules on Talos: Talos drops `CAP_SYS_MODULE` from the
capability bounding set of its container runtime, so no pod can load a
module. The module comes from a Talos system extension built outside this
repository from a release's source tarball, pinned by its checksum
([Releases](#releases)), and loaded at boot.

## Build and test

The kernel and module builds and the VM tests run in a container built
from `dev/Dockerfile` (libmxl comes from the go-mxl builder image), with
tests in a virtme-ng VM. `dev/checkpatch.sh` runs on the host;
`dev/libmxl.sh` is started on the host and builds in the development
image itself. `dev/kernel.sh`
builds the kernel Talos ships, named in `dev/talos-kernel`, or the release
or mainline release candidate named by `KERNEL_VERSION` (as `7.3.5` or
`7.4-rc3`), which `dev/vm.sh` then boots. `dev/module.sh` builds the
module with `W=1` and sparse and fails on any warning; `dev/checkpatch.sh`
runs `checkpatch.pl --strict` with the exceptions it and `.checkpatch.conf`
state. The host needs Docker and `/dev/kvm`. `dev/run.sh` uses the
development image CI pushed for the same `dev/Dockerfile` and
`dev/sparse.sh` when there is one, and builds it otherwise;
`dev/kernel-cache.sh pull` puts the kernel tree CI built for a
`KERNEL_VERSION` and `FLAVOR` into `build/` in place of a `dev/kernel.sh`
build.

```sh
./dev/run.sh ./dev/kernel.sh                       # debug kernel: KASAN, lockdep, DEBUG_VM
./dev/run.sh ./dev/module.sh /src/build/linux-$(cat dev/talos-kernel)
./dev/checkpatch.sh
./dev/run.sh make -C tools
./dev/run.sh ./dev/vm.sh ./tests/redundancy.sh          # FORMAT=video|data|audio
./dev/run.sh ./dev/vm.sh ./tests/replace.sh             # FORMAT=video|data|audio
./dev/run.sh ./dev/vm.sh ./tests/wait.sh
./dev/run.sh ./dev/vm.sh ./tests/placeholder.sh
./dev/run.sh ./dev/vm.sh ./tests/views.sh
./dev/run.sh ./dev/vm.sh ./tests/huge.sh
./dev/run.sh ./dev/vm.sh ./tests/dentries.sh
./dev/run.sh ./dev/vm.sh ./tests/hwpoison.sh           # VM only: poisoned frames stay until reboot
./dev/run.sh env VNG_OPTS='--numa 1536M,cpus=0-1 --numa 1536M,cpus=2-3' \
  ./dev/vm.sh ./tests/numa.sh   # two NUMA nodes; the halves of VM_MEMORY=3G, VM_CPUS=4
./dev/run.sh env VNG_OPTS='--numa 1536M,cpus=0-1 --numa 1536M,cpus=2-3' \
  ./dev/vm.sh ./tests/move.sh   # the same two nodes; STRICT=1 adds absolute stall bounds
./dev/libmxl.sh <checkout of qvest-digital/mxl>     # libmxl, its tests and tools
./dev/run.sh ./dev/vm.sh ./tests/libmxl.sh
b=build/mxl-src/build/Linux-Clang-Release
./dev/run.sh ./dev/vm.sh "DEMO=$b/tools/mxl-fabrics-demo/mxl-fabrics-demo \
  LD_LIBRARY_PATH=$b/lib:$b/lib/fabrics/ofi:/opt/libfabric/lib ./tests/rdma.sh"
```

`tests/rdma.sh` needs a libmxl with upstream ed1dcd1 on the target side,
which the libmxl of the dev image predates, so it runs against the
`dev/libmxl.sh` build. Under KASAN, soft-RoCE does not deliver 1080p50 in
time; there the test needs a rate of 5 grains/s and a ring that covers the
delivery lag: `RATE=5 HISTORY_NS=10000000000`.

On a real host, `sudo dev/host.sh ./tests/redundancy.sh` loads
`kernel/mxlfs.ko`, built with `make -C kernel` against the running kernel,
mounts an mxlfs at `/run/mxlfs-test` (`MXLFS_MNT`), runs the suite, and
unmounts and unloads mxlfs, also when it was loaded before. It sets
transparent huge pages to `madvise` and leaves them so. The suites need
libmxl under `/opt/libmxl` and `tools/harness`, built with `make -C tools`.

`MXLFS_OPTS` adds mount options to the mxlfs `dev/vm.sh` or `dev/host.sh`
mounts, as in
`./dev/run.sh env MXLFS_OPTS=huge=always ./dev/vm.sh ./tests/redundancy.sh`.

`tests/redundancy.sh` is timing sensitive: it gives a member two grains to
deliver before an index is published INVALID, so a host that stalls the VM
for longer than that fails it with `unexpected_invalid` while `wrong_index`,
`torn` and `missed_invalid` stay 0. `tests/replace.sh` fails the same way
when a reader falls most of its 1 s ring behind: the merger retires a slot
to INVALID just before a member writes over it.

`./dev/run.sh env FLAVOR=perf ./dev/kernel.sh` builds a kernel
without the debug options in `build/linux-<version>-perf`; pass that tree
to `dev/module.sh` and set `KDIR` to it for `dev/vm.sh`.

## Test tools

- `tools/harness`: `main.c` holds the command reference and dispatch,
  `harness.h` what the files share, `util.c` the common helpers, and each
  other file one command group with its helpers. Writers with a
  deterministic drop schedule, a merger that builds a view flow over
  member flows and replaces a member whose flow is recreated, readers that
  check every page of every grain or every sample, an RDMA peek and a
  switch microbenchmark.
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
  views on their own, the folios each `huge=` gives, the node of each page
  with and without `numa_node=`, lazily allocated flows,
  waiting for missing flows, memory errors injected into file pages,
  RDMA into mxlfs
  pages over soft-RoCE, moves of ring files between nodes under writers and
  readers and what refuses them, and libmxl's own test suite on an mxlfs.

## Quality gates and CI

`.github/workflows/ci.yml` runs on every pull request, on pushes to
`main`, and daily, which also builds the trees of a new stable release or
release candidate before a change needs them.

- `checkpatch`: `dev/checkpatch.sh`, `checkpatch.pl --strict` from a
  pinned kernel commit, with the exceptions the script and
  `.checkpatch.conf` give.
- `build`: `dev/module.sh` against the kernel `dev/talos-kernel` names and
  the newest stable release from kernel.org, both built by `dev/kernel.sh`
  with KASAN, lockdep and `DEBUG_VM`. It builds with `W=1` and sparse 0.6.5-rc1
  (`dev/sparse.sh`) and fails on any compiler, sparse or modpost warning,
  and when sparse did not run. Also the harness with `-Werror`, and
  setcheck with gofmt, `go vet` and `go test`.
- `vm-tests`: on both kernels, in virtme-ng, one VM per line of a group:
  `redundancy.sh` in seven format, geometry and rate variants, `wait.sh`,
  `replace.sh` for each format, `placeholder.sh`, `views.sh`, `huge.sh`,
  `dentries.sh`, `hwpoison.sh`, and `numa.sh` and `move.sh` in a guest
  of two NUMA nodes. `rdma.sh`, `libmxl.sh` and `tests/cluster/` do not
  run in CI; `installed.sh` runs in the `dkms` job.
- `distro`: the module against the Ubuntu 26.04 and Debian 13 backports
  kernel headers.
- `between`: compiled against 6.19 and 7.1, kernels no other job pins.
- `next-rc`: compiled against the open mainline release candidate; a
  failure there is a warning, not a failure of the change.
- `deb` and `dkms`: the Debian package, checked by lintian at pedantic
  level, then installed by `dev/distro-test.sh` on Ubuntu 26.04 and on
  Debian 13 with a backports kernel: DKMS builds the module for the
  distribution's kernel, skips Debian's own 6.12 for `BUILD_EXCLUSIVE`,
  and the kernel is booted to run `tests/installed.sh`.

`.github/workflows/pr-title.yml` checks that a pull request title is a
Conventional Commit whose type release-please knows. Kernel trees and the
development image come from the registry (`dev/kernel-cache.sh`,
`dev/ci-images.sh`); a run that had to build one pushes it, unless it
comes from a fork.

## Releases

release-please (`.github/release-please-config.json`) keeps a release
pull request open for the changes merged to `main`. Merging it tags the
next `vX.Y.Z-rc.N` and `.github/workflows/release-please.yml` attaches:

- the source tarball `mxlfs-<version>.tar.gz`, a `git archive` of the
  tag, with `.sha256` and `.sha512` checksum files. KMM and the Talos
  extension build from it, so an attached tarball is never replaced;
- the Debian package `mxlfs-dkms_<version>_all.deb` and its `SHA256SUMS`;
- the setcheck image, `ghcr.io/danielclasen/mxlfs-setcheck:<version>`.

The version is in `dkms.conf` and in `MODULE_VERSION` in `kernel/super.c`,
which release-please updates in the release pull request.

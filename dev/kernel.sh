#!/bin/sh
# Fetch and build the kernel the module is developed against. The version
# follows the Talos kernel in talos-pkgs (Pkgfile linux_version).
# FLAVOR=debug (default) adds KASAN, lockdep and DEBUG_VM; FLAVOR=perf
# leaves them out, for measurements. Each flavor builds in its own tree.
# Both have cgroup writeback, as the Talos kernel does: with it, the core
# reads an inode's i_wb on paths a kernel without it never takes.
set -eu
version=${KERNEL_VERSION:-6.18.42}
flavor=${FLAVOR:-debug}
src=build/linux-$version
out=$src
[ "$flavor" = debug ] || out=build/linux-$version-$flavor
mkdir -p build
if [ ! -d "$src" ]; then
  curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$version.tar.xz" | tar -xJ -C build
fi
if [ "$out" != "$src" ] && [ ! -d "$out" ]; then
  # A separate tree rather than O=: virtme-ng boots from a source tree.
  cp -a "$src" "$out"
  make -C "$out" -s mrproper
fi
cd "$out"
if [ ! -f .config ]; then
  vng --kconfig
  ./scripts/config \
    -e MODULES -e MODULE_UNLOAD \
    -e TRANSPARENT_HUGEPAGE -d TRANSPARENT_HUGEPAGE_ALWAYS -e TRANSPARENT_HUGEPAGE_MADVISE \
    -e FUTEX -e FANOTIFY -e INOTIFY_USER -e TMPFS -e SHMEM \
    -e MEMCG -e BLK_CGROUP -e CGROUP_WRITEBACK \
    -e INFINIBAND -e INFINIBAND_USER_ACCESS -e INFINIBAND_USER_MEM -m RDMA_RXE -m VETH -m DUMMY \
    -d DEBUG_INFO_BTF
  if [ "$flavor" = debug ]; then
    ./scripts/config -e DEBUG_KERNEL -e DEBUG_VM -e PROVE_LOCKING -e KASAN -e KASAN_INLINE
  fi
  make olddefconfig
fi
make -j"$(nproc)" bzImage modules

#!/bin/sh
# Fetch and build the kernel the module is developed against. The version
# follows the Talos kernel in talos-pkgs (Pkgfile linux_version).
set -eu
version=${KERNEL_VERSION:-6.18.42}
dir=build/linux-$version
mkdir -p build
if [ ! -d "$dir" ]; then
  curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$version.tar.xz" | tar -xJ -C build
fi
cd "$dir"
if [ ! -f .config ]; then
  vng --kconfig
  ./scripts/config \
    -e MODULES -e MODULE_UNLOAD \
    -e TRANSPARENT_HUGEPAGE -d TRANSPARENT_HUGEPAGE_ALWAYS -e TRANSPARENT_HUGEPAGE_MADVISE \
    -e FUTEX -e FANOTIFY -e INOTIFY_USER -e TMPFS -e SHMEM \
    -e INFINIBAND -e INFINIBAND_USER_ACCESS -e INFINIBAND_USER_MEM -m RDMA_RXE \
    -e DEBUG_KERNEL -e DEBUG_VM -e PROVE_LOCKING -e KASAN -e KASAN_INLINE \
    -d DEBUG_INFO_BTF
  make olddefconfig
fi
make -j"$(nproc)" bzImage modules

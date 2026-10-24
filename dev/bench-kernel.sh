#!/bin/sh
# Build and install the Talos kernel version on a Debian bench host, from
# the running kernel's config, with THP in madvise mode as on Talos and
# soft-RoCE/veth for tests/rdma.sh. LOCALMOD=1 trims the config to the
# modules loaded right now, which is quicker to build but leaves out
# whatever is not loaded yet, such as what Docker and kind need. The
# distribution kernel stays installed as a fallback. Run as a user with
# sudo; reboot afterwards.
set -eu
version=${KERNEL_VERSION:-6.18.42}
dir=${BUILD_DIR:-$HOME/kernel}
mkdir -p "$dir" && cd "$dir"
[ -d linux-$version ] || curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$version.tar.xz" | tar -xJ
cd linux-$version
cp "${BASE_CONFIG:-/boot/config-$(uname -r)}" .config
if [ "${LOCALMOD:-0}" = 1 ]; then
  lsmod > /tmp/lsmod.mxlfs
  make LSMOD=/tmp/lsmod.mxlfs localmodconfig < /dev/null > /dev/null
fi
./scripts/config \
  --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS "" \
  --set-str LOCALVERSION "-mxlfs" \
  -d TRANSPARENT_HUGEPAGE_ALWAYS -e TRANSPARENT_HUGEPAGE_MADVISE \
  -e INFINIBAND -e INFINIBAND_USER_ACCESS -e INFINIBAND_USER_MEM -m RDMA_RXE -m VETH \
  -d DEBUG_INFO_BTF
make olddefconfig > /dev/null
make -j"$(nproc)" bzImage modules
sudo make modules_install install

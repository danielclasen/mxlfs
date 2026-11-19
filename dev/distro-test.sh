#!/bin/sh
# Install the mxlfs-dkms package from build/deb on this Debian or Ubuntu
# system (a container is enough) together with kernels of the
# distribution, let DKMS build the module for them, and boot the newest
# under virtme-ng to run tests/installed.sh. Needs root and /dev/kvm.
# KERNEL_PACKAGES names the kernel image and headers packages, and
# APT_OPTS options for installing them (as -t trixie-backports).
# OLD_KERNEL_PACKAGES, if set, names an older kernel's packages, installed
# first without APT_OPTS. A kernel older than 6.18 must get no module, and
# DKMS must say it skipped it for BUILD_EXCLUSIVE rather than failed to
# build it.
set -eu
cd "$(dirname "$0")/.."
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends virtme-ng qemu-system-x86 kmod udev busybox-static zstd xz-utils
# shellcheck disable=SC2086 # all are lists of words
{
  [ -z "${OLD_KERNEL_PACKAGES:-}" ] ||
    apt-get install -y -qq --no-install-recommends $OLD_KERNEL_PACKAGES
  apt-get install -y -qq --no-install-recommends ${APT_OPTS:-} $KERNEL_PACKAGES
}
log=$(mktemp)
trap 'rm -f "$log"' EXIT
apt-get install -y -qq --no-install-recommends ./build/deb/mxlfs-dkms_*.deb >"$log" 2>&1 ||
  { cat "$log"; exit 1; }
cat "$log"
dkms status mxlfs
skipped=0
kernels=$(find /lib/modules -mindepth 1 -maxdepth 1 -printf '%f\n' | sort -V)
for k in $kernels; do
  [ "$(printf '6.18\n%s\n' "$k" | sort -V | head -n1)" = 6.18 ] && continue
  if dkms status mxlfs | grep -qF ", $k,"; then
    echo "DKMS built mxlfs for $k, older than 6.18" >&2; exit 1
  fi
  if ls "/lib/modules/$k/updates/dkms/mxlfs.ko"* >/dev/null 2>&1; then
    echo "DKMS installed mxlfs for $k, older than 6.18" >&2; exit 1
  fi
  if ! grep -A1 -F "/$k/" "$log" | grep -q 'includes a BUILD_EXCLUSIVE directive'; then
    echo "DKMS did not report $k as excluded by BUILD_EXCLUSIVE" >&2; exit 1
  fi
  echo "skipped $k: older than 6.18, excluded by BUILD_EXCLUSIVE"
  skipped=$((skipped + 1))
done
if [ -n "${OLD_KERNEL_PACKAGES:-}" ] && [ "$skipped" -eq 0 ]; then
  echo "OLD_KERNEL_PACKAGES is set but no kernel older than 6.18 was checked" >&2; exit 1
fi
kver=$(echo "$kernels" | tail -n1)
ls "/lib/modules/$kver/updates/dkms/mxlfs.ko"* >/dev/null 2>&1 ||
  { echo "DKMS installed no mxlfs for $kver" >&2; exit 1; }
vng --run "/boot/vmlinuz-$kver" --memory 2G --cpus 2 --user root --exec ./tests/installed.sh

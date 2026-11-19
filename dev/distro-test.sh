#!/bin/sh
# Install the mxlfs-dkms package from build/deb on this Debian or Ubuntu
# system (a container is enough) together with a kernel of the
# distribution, let DKMS build the module for that kernel, and boot it
# under virtme-ng to run tests/installed.sh. Needs root and /dev/kvm.
# KERNEL_PACKAGES names the kernel image and headers packages, and
# APT_OPTS options for installing them (as -t trixie-backports).
set -eu
cd "$(dirname "$0")/.."
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
# shellcheck disable=SC2086 # word splitting is intended
apt-get install -y -qq --no-install-recommends virtme-ng qemu-system-x86 kmod udev busybox-static zstd xz-utils
apt-get install -y -qq --no-install-recommends ${APT_OPTS:-} $KERNEL_PACKAGES
apt-get install -y -qq --no-install-recommends ./build/deb/mxlfs-dkms_*.deb
kver=$(ls /lib/modules | sort -V | tail -1)
dkms status mxlfs
ls "/lib/modules/$kver/updates/dkms/mxlfs.ko"* >/dev/null 2>&1 ||
  { echo "DKMS installed no mxlfs for $kver" >&2; exit 1; }
vng --run "/boot/vmlinuz-$kver" --memory 2G --cpus 2 --user root --exec ./tests/installed.sh

#!/bin/sh
# Boot the built kernel under virtme-ng (inside the dev image) and run a
# command as root in it, with the module loaded and an mxlfs at /dev/shm,
# mounted with MXLFS_OPTS added to its options.
set -eu
kdir=${KDIR:-build/linux-${KERNEL_VERSION:-6.18.42}}
exec vng --run "$kdir" --memory "${VM_MEMORY:-3G}" --cpus "${VM_CPUS:-4}" --user root \
  --exec "set -e; echo always > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true; \
          echo madvise > /sys/kernel/mm/transparent_hugepage/enabled; \
          insmod /src/${MXLFS_KO:-kernel/mxlfs.ko}; mount -t mxlfs -o size=${MXLFS_SIZE:-2G},mode=1777${MXLFS_OPTS:+,$MXLFS_OPTS} mxlfs /dev/shm; \
          cd /src; $*"

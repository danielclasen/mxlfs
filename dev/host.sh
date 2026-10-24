#!/bin/sh
# Run a command as root on a real host (not under virtme-ng) with the
# module loaded and an mxlfs mounted at $MXLFS_MNT. Unlike dev/vm.sh it
# does not take over /dev/shm. Unmounts and unloads afterwards.
set -eu
cd "$(dirname "$0")/.."
export MXLFS_MNT=${MXLFS_MNT:-/run/mxlfs-test}
export LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-/opt/libmxl/lib:/opt/libfabric/lib}
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
grep -q '^mxlfs ' /proc/modules || insmod ${MXLFS_KO:-kernel/mxlfs.ko}
mkdir -p "$MXLFS_MNT"
mount -t mxlfs -o size=${MXLFS_SIZE:-4G},mode=1777 mxlfs "$MXLFS_MNT"
rc=0
sh -c "$*" || rc=$?
umount "$MXLFS_MNT"
rmmod mxlfs
exit $rc

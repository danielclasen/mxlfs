#!/bin/sh
# Boot the built kernel under virtme-ng (inside the dev image) and run a
# command as root in it, with the module loaded and an mxlfs at /dev/shm,
# mounted with MXLFS_OPTS added to its options. The command sees
# MXLFS_OPTS too, for the mounts a suite makes of its own.
# The kernel log is cleared before the module loads: a suite checks it for
# reports, and one the guest's boot printed is no finding of the suite.
# VM_NODES splits the guest's memory and CPUs evenly into that many NUMA
# nodes.
set -eu
kdir=${KDIR:-build/linux-${KERNEL_VERSION:-6.18.42}}
memory=${VM_MEMORY:-3G}
cpus=${VM_CPUS:-4}
nodes=${VM_NODES:-1}
numa=
if [ "$nodes" -gt 1 ]; then
  mib=$(($(numfmt --from=iec "$memory") / 1048576 / nodes))
  i=0
  while [ $i -lt "$nodes" ]; do
    numa="$numa --numa ${mib}M,cpus=$((i * cpus / nodes))-$(((i + 1) * cpus / nodes - 1))"
    i=$((i + 1))
  done
fi
exec vng --run "$kdir" --memory "$memory" --cpus "$cpus" $numa --user root \
  --exec "set -e; echo always > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true; \
          echo madvise > /sys/kernel/mm/transparent_hugepage/enabled; \
          dmesg -C; insmod /src/${MXLFS_KO:-kernel/mxlfs.ko}; mount -t mxlfs -o size=${MXLFS_SIZE:-2G},mode=1777${MXLFS_OPTS:+,$MXLFS_OPTS} mxlfs /dev/shm; \
          export MXLFS_OPTS='${MXLFS_OPTS:-}'; cd /src; $*"

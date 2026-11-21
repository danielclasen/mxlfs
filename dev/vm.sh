#!/bin/sh
# Boot the built kernel under virtme-ng (inside the dev image, VNG_OPTS
# added to vng's, as --numa) and run a command as root in it, with the
# module loaded and an mxlfs at /dev/shm, mounted with MXLFS_OPTS added to
# its options. The command sees MXLFS_OPTS too, for its own mounts.
# The kernel log is cleared before the module loads: a suite checks it for
# reports, and one the guest's boot printed is no finding of the suite.
#
# A failure prints its exit status, the stage it failed in and the tail of
# the guest console; a guest still running prints the tail of its kernel
# log. vng sends its console, ttyS0, to /dev/null when it runs a command,
# and the 8250 driver drives one console port only, so a second serial
# port, written to a file, carries an early console kept for the guest's
# whole life, and the command marks each stage on it where it can: a mark
# never changes the command's result. At the quiet log level vng sets,
# that console carries oops and panic reports. vng returns 255 when the
# guest returned no status: it died, or QEMU stopped.
set -eu
kdir=${KDIR:-build/linux-${KERNEL_VERSION:-$(cat "$(dirname "$0")/talos-kernel")}}
diag=$(mktemp -d)
trap 'rm -rf "$diag"' EXIT
console=$diag/console
rc=0
vng --run "$kdir" --memory "${VM_MEMORY:-3G}" --cpus "${VM_CPUS:-4}" ${VNG_OPTS:-} --user root \
  --qemu-opts="-serial file:$console" --append "earlycon=uart8250,io,0x2f8,115200 keep_bootcon" \
  --exec "stage() { { echo \"vm.sh: stage \$1\" > /dev/ttyS1; } 2>/dev/null || :; }; \
          trap 'rc=\$?; stage \"exit \$rc\"; [ \$rc = 0 ] || { echo \"vm.sh: kernel log, last 100 lines:\"; dmesg | tail -n 100; } >&2' EXIT; \
          set -e; stage setup; echo madvise > /sys/kernel/mm/transparent_hugepage/enabled; \
          dmesg -C; stage insmod; insmod /src/${MXLFS_KO:-kernel/mxlfs.ko}; \
          stage mount; mount -t mxlfs -o size=${MXLFS_SIZE:-2G},mode=1777${MXLFS_OPTS:+,$MXLFS_OPTS} mxlfs /dev/shm; \
          export MXLFS_OPTS='${MXLFS_OPTS:-}'; cd /src; stage test; $*" || rc=$?
[ "$rc" = 0 ] && exit 0
if [ ! -e "$console" ]; then
  stage="vng start"
else
  stage=$(tr -d '\r' < "$console" | sed -n '/^vm\.sh: stage exit /d; s/^vm\.sh: stage //p' | tail -n 1)
  stage=${stage:-boot}
fi
echo "vm.sh: exit $rc in stage $stage" >&2
if [ -e "$console" ]; then
  echo "vm.sh: guest console, last 100 lines:" >&2
  # What follows the command's exit is the guest powering off.
  tr -d '\r' < "$console" | sed '/^vm\.sh: stage exit /q' | tail -n 100 >&2
fi
exit "$rc"

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
# guest returned no status: it died, or QEMU stopped. So QEMU runs through
# a wrapper that keeps its exit status and run time, and logs its run
# states: a guest that never reached "running" never started, and one
# that left it for "shutdown" was asked to stop, by a guest power-off, a
# guest reset under -no-reboot or a signal, which QEMU reports on stderr.
# A failure prints both. QEMU's own messages go to stderr as before.
set -eu
case ${FLAVOR:-debug} in debug | perf) ;; *) echo "FLAVOR must be debug or perf, not $FLAVOR" >&2; exit 2 ;; esac
tree=build/linux-${KERNEL_VERSION:-$(cat "$(dirname "$0")/talos-kernel")}
[ "${FLAVOR:-debug}" = debug ] || tree=$tree-$FLAVOR
kdir=${KDIR:-$tree}
diag=$(mktemp -d)
trap 'rm -rf "$diag"' EXIT
console=$diag/console
qemu=$(command -v qemu-system-x86_64) || {
  echo "vm.sh: no qemu-system-x86_64 in PATH" >&2
  exit 1
}
# QEMU runs in the wrapper's foreground: a background job's stdin is
# /dev/null, and virtme-run gives the guest QEMU's stdin. The wrapper
# forwards no signals: docker stop's SIGTERM ends vm.sh through tini, and
# the container's teardown then kills the wrapper and QEMU.
cat > "$diag/qemu" <<EOF
#!/bin/sh
[ "\$*" = --version ] && exec '$qemu' --version
s=\$(date +%s.%N)
rc=0
'$qemu' "\$@" || rc=\$?
echo "\$rc \$s \$(date +%s.%N)" > '$diag/qemu-status'
exit \$rc
EOF
chmod +x "$diag/qemu"
rc=0
vng --run "$kdir" --memory "${VM_MEMORY:-3G}" --cpus "${VM_CPUS:-4}" ${VNG_OPTS:-} --user root --qemu "$diag/qemu" \
  --qemu-opts="-serial file:$console -D $diag/qemu-log -trace runstate_set" \
  --append "earlycon=uart8250,io,0x2f8,115200 keep_bootcon" \
  --exec "stage() { { echo \"vm.sh: stage \$1\" > /dev/ttyS1; } 2>/dev/null || :; }; \
          trap 'rc=\$?; stage \"exit \$rc\"; [ \$rc = 0 ] || { echo \"vm.sh: kernel log, last 100 lines:\"; dmesg | tail -n 100; } >&2' EXIT; \
          set -e; stage setup; echo madvise > /sys/kernel/mm/transparent_hugepage/enabled; \
          dmesg -C; stage insmod; insmod /src/${MXLFS_KO:-kernel/mxlfs.ko}; \
          stage mount; mount -t mxlfs -o size=${MXLFS_SIZE:-2G},mode=1777${MXLFS_OPTS:+,$MXLFS_OPTS} mxlfs /dev/shm; \
          export MXLFS_OPTS='${MXLFS_OPTS:-}'; cd /src; stage test; $*" || rc=$?
[ "$rc" = 0 ] && exit 0
q='' s='' e=''
if [ -e "$diag/qemu-status" ]; then
  read -r q s e < "$diag/qemu-status" || :
fi
if [ ! -e "$diag/qemu-status" ] && [ ! -e "$console" ]; then
  stage="vng start"
elif [ -n "$q" ] && ! grep -qs '(running)' "$diag/qemu-log"; then
  stage="qemu start"
elif [ -e "$console" ]; then
  stage=$(tr -d '\r' < "$console" | sed -n '/^vm\.sh: stage exit /d; s/^vm\.sh: stage //p' | tail -n 1)
  stage=${stage:-boot}
else
  stage=boot
fi
echo "vm.sh: exit $rc in stage $stage" >&2
if [ ! -e "$diag/qemu-status" ] && [ ! -e "$console" ]; then
  echo "vm.sh: QEMU did not run" >&2
elif [ -z "$q" ]; then
  echo "vm.sh: QEMU status unknown" >&2
else
  t=
  if [ -n "$s" ] && [ -n "$e" ]; then
    t=" after $(echo "$s $e" | awk '{printf "%.2f", $2 - $1}') s"
  fi
  if [ "$q" -gt 128 ] 2>/dev/null; then
    echo "vm.sh: QEMU killed by signal $((q - 128))$t" >&2
  else
    echo "vm.sh: QEMU exited $q$t" >&2
  fi
fi
if [ -s "$diag/qemu-log" ]; then
  echo "vm.sh: QEMU run states:" >&2
  cat "$diag/qemu-log" >&2
elif [ -e "$diag/qemu-status" ] || [ -e "$console" ]; then
  echo "vm.sh: no run states" >&2
fi
if [ -e "$console" ]; then
  echo "vm.sh: guest console, last 100 lines:" >&2
  # What follows the command's exit is the guest powering off.
  tr -d '\r' < "$console" | sed '/^vm\.sh: stage exit /q' | tail -n 100 >&2
fi
exit "$rc"

# shellcheck shell=sh
# Sourced by the suites. check NAME CONDITION prints ok or FAIL for a shell
# condition and counts each failure in fails. kernel_reports prints the
# bugs, warnings, KASAN, UBSAN, lockdep, refcount and hung-task reports and
# the inodes or dentries left busy at unmount that the kernel log holds
# after its first KMSG_START lines, and fails when there is none. A log
# that cannot be read counts as a report. dev/vm.sh clears the log before
# it loads the module and leaves KMSG_START unset, so the whole log counts,
# module load and mount included; installed.sh clears it itself. dev/host.sh
# sets KMSG_START to the log's length before it loads the module, or before
# it mounts when the module is loaded already, so what the host logged
# earlier does not count; a log buffer that wraps during the run can push
# new lines below that count.
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
kmsg_start=${KMSG_START:-0}
kernel_reports() {
  kmsg_log=$(dmesg) || { echo "dmesg failed" >&2; return 0; }
  printf '%s\n' "$kmsg_log" | tail -n +$((kmsg_start + 1)) |
    grep -E 'BUG: |kernel BUG at|WARNING:|KASAN|UBSAN:|circular locking|Call Trace|Oops|general protection|refcount_t|blocked for more than|Busy inodes|still in use'
}

#!/bin/sh
# Views on their own mount: read() through a view returns the selected
# source, random per-range selections show on every page of every reader's
# mapping, a view does not keep the filesystem from being unmounted, and
# the switch benchmark runs twice over the same files. Runs as root under
# dev/vm.sh or dev/host.sh.
set -eu
h=tools/harness
mnt=/tmp/views
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }

mkdir -p $mnt
mount -t mxlfs mxlfs $mnt
$h bench $mnt 2 40 > /tmp/b1.out
$h bench $mnt 2 40 > /tmp/b2.out
cat /tmp/b1.out /tmp/b2.out
check "the benchmark runs twice over the same files" 'grep -q "errors=0" /tmp/b2.out'
check "read() through a view returns the selected source" 'cmp -s $mnt/view $mnt/src0 || cmp -s $mnt/view $mnt/src1'
$h ranges $mnt 3 300 > /tmp/ranges.out; cat /tmp/ranges.out
check "per-range selection maps the selected source on every page" 'grep -q "errors=0 pread_errors=0" /tmp/ranges.out'
check "a mount with views unmounts" 'umount $mnt'
grep -q " $mnt " /proc/mounts && umount -l $mnt
dmesg | grep -E 'BUG:|WARNING:|KASAN|circular locking|Call Trace|Busy inodes' && fails=$((fails + 1))
echo "fails=$fails"
[ $fails = 0 ]

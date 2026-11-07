#!/bin/sh
# The numa_node= mount option. Without it a file's pages come from the node
# of the task that makes them; with it, from the given node, wherever that
# task runs. Each run makes pages in every way a file gets them (harness
# numa) from a task on the CPUs of one node and checks the node of each
# page. Also checks size= under the option, the option's parsing and
# display, and that a node without free memory left makes the allocation
# fall back to another node instead of failing. A file's own preferred node
# takes precedence over the option. Needs two NUMA nodes with memory and CPUs, as
# dev/vm.sh boots with VNG_OPTS='--numa 1536M,cpus=0-1 --numa 1536M,cpus=2-3'.
# Runs as root under dev/vm.sh or dev/host.sh.
set -eu
h=tools/harness
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }

nodes=/sys/devices/system/node
if [ ! -d $nodes/node1 ] || [ -z "$(cat $nodes/node1/cpulist)" ] ||
   ! grep -q '^0-' $nodes/has_memory; then
  echo "FAIL needs NUMA nodes 0 and 1 with memory and CPUs (see the top of $0)"
  exit 1
fi
cpu0=$(cut -d, -f1 $nodes/node0/cpulist | cut -d- -f1)
cpu1=$(cut -d, -f1 $nodes/node1/cpulist | cut -d- -f1)

mnt=/tmp/numa
mkdir -p $mnt

# The node of a task's pages: the baseline the option overrides.
mount -t mxlfs ${MXLFS_OPTS:+-o $MXLFS_OPTS} mxlfs $mnt
taskset -c $cpu1 $h numa $mnt 1 > /tmp/numa.out 2>&1 || true; cat /tmp/numa.out
check "without numa_node=, pages come from the node of the task on node 1" 'grep -q "^numa node=1 .* failed=0$" /tmp/numa.out'
check "the default shows no numa_node=" '! grep " $mnt mxlfs " /proc/mounts | grep -q numa_node='
umount $mnt

for n in 0 1; do
  other=$((1 - n))
  eval cpu=\$cpu$other
  mount -t mxlfs -o numa_node=$n${MXLFS_OPTS:+,$MXLFS_OPTS} mxlfs $mnt
  PLACED=1 taskset -c $cpu $h numa $mnt $n > /tmp/numa.out 2>&1 || true; cat /tmp/numa.out
  check "numa_node=$n: pages of a task on node $other come from node $n" 'grep -q "^numa node=$n .* failed=0$" /tmp/numa.out'
  check "numa_node=$n shows in the mount options" 'grep -q " $mnt mxlfs .*numa_node=$n" /proc/mounts'
  umount $mnt
done

# More than node 0 has free: what it has no room for comes from node 1,
# as it would without the option, and the allocation does not fail.
file_kb() { awk '/FilePages/ { print $4 }' $nodes/node$1/meminfo; }
mount -t mxlfs -o numa_node=0,size=0 mxlfs $mnt
free0=$(awk '/MemFree/ { print $4 * 1024 }' $nodes/node0/meminfo)
was1=$(file_kb 1)
check "an allocation larger than numa_node='s free memory falls back to another node" \
  'taskset -c $cpu0 fallocate -l $((free0 + (64 << 20))) $mnt/big'
check "node 1 took at least what node 0 had no room for" '[ $(($(file_kb 1) - was1)) -ge 65536 ]'
rm -f $mnt/big
umount $mnt

# size= holds as without the option: the pages placed for a read or a
# fault are charged one by one, and the core refuses those past the limit.
# A read over holes ends at the limit, and the faults there add at most
# the one window of two PMDs readahead takes. Measured as page cache,
# since a page read_folio refuses stays in it uncharged.
cached_kb() { awk '/^Cached:/ { print $2 }' /proc/meminfo; }
mount -t mxlfs -o numa_node=0,size=16M mxlfs $mnt
truncate -s 256M $mnt/holes
was=$(cached_kb)
taskset -c $cpu1 dd if=$mnt/holes of=/dev/null bs=256M count=1 2>/dev/null || true
check "a read over holes stops at size=" '[ $(($(cached_kb) - was)) -le $(((16 + 1) * 1024)) ]'
rm -f $mnt/holes
truncate -s 256M $mnt/holes
was=$(cached_kb)
taskset -c $cpu1 $h touch $mnt/holes 2>/dev/null || true
check "faults over holes stop at size=" '[ $(($(cached_kb) - was)) -le $(((16 + 4 + 1) * 1024)) ]'
rm -f $mnt/holes
umount $mnt

# A file's own preferred node (MXLFS_IOC_SET_NODE) takes precedence over
# numa_node=, and over the node of the task without it; every way of making
# pages honours it, the fold of a lazily sized file too. Cleared, the
# file's pages follow numa_node= again.
mount -t mxlfs -o numa_node=0${MXLFS_OPTS:+,$MXLFS_OPTS} mxlfs $mnt
PREF=1 PLACED=1 taskset -c $cpu0 $h numa $mnt 1 > /tmp/numa.out 2>&1 || true; cat /tmp/numa.out
check "numa_node=0 and a file preference of 1: pages of a task on node 0 come from node 1" 'grep -q "^numa node=1 .* failed=0$" /tmp/numa.out'
PREF=1 PREF_CLEAR=1 PLACED=1 taskset -c $cpu1 $h numa $mnt 0 > /tmp/numa.out 2>&1 || true; cat /tmp/numa.out
check "a cleared preference leaves numa_node=0 in charge" 'grep -q "^numa node=0 .* failed=0$" /tmp/numa.out'
umount $mnt
mount -t mxlfs ${MXLFS_OPTS:+-o $MXLFS_OPTS} mxlfs $mnt
PREF=1 PLACED=1 taskset -c $cpu0 $h numa $mnt 1 > /tmp/numa.out 2>&1 || true; cat /tmp/numa.out
check "without numa_node=, a file preference of 1 wins over a task on node 0" 'grep -q "^numa node=1 .* failed=0$" /tmp/numa.out'
umount $mnt

# A preferred node without free memory falls back like numa_node= does,
# and the mount counts each folio that did.
fallbacks() { $h stats $1 | sed -E 's/.* numa_fallback_folios=([0-9]+) .*/\1/'; }
mkdir -p /tmp/numa-fill
mount -t mxlfs -o numa_node=1,size=0 mxlfs /tmp/numa-fill
mount -t mxlfs -o size=0 mxlfs $mnt
free1=$(awk '/MemFree/ { print $4 * 1024 }' $nodes/node1/meminfo)
taskset -c $cpu1 fallocate -l $((free1 + (64 << 20))) /tmp/numa-fill/big
touch $mnt/pref
$h setnode $mnt/pref 1 > /dev/null
was=$(fallbacks $mnt/pref)
check "a file preferring a full node is allocated all the same" 'taskset -c $cpu0 fallocate -l $((32 << 20)) $mnt/pref'
check "the mount counts the folios that fell back ($(fallbacks $mnt/pref) after $was)" '[ $(fallbacks $mnt/pref) -gt $was ]'
check "MOUNT_NODES counts them against node 1" '$h mountnodes $mnt/pref | grep "^mountnodes node=1 " | grep -qv " fallback_folios=0 "'
rm -f /tmp/numa-fill/big $mnt/pref
umount /tmp/numa-fill $mnt

check "a node beyond the last is refused" '! mount -t mxlfs -o numa_node=$(ls -d $nodes/node* | wc -l) mxlfs $mnt 2>/dev/null'
check "a negative node is refused" '! mount -t mxlfs -o numa_node=-1 mxlfs $mnt 2>/dev/null'
check "a node that is no number is refused" '! mount -t mxlfs -o numa_node=near mxlfs $mnt 2>/dev/null'
grep -q " $mnt mxlfs " /proc/mounts && umount $mnt
dmesg | grep -E 'BUG: |WARNING: CPU|KASAN|circular locking|Call Trace|Busy inodes' && fails=$((fails + 1))
echo "fails=$fails"
[ $fails = 0 ]

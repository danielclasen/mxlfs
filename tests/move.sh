#!/bin/sh
# Moving a file's pages to another NUMA node (MXLFS_IOC_MOVE) and the calls
# around it: holds, per-node page counters and the mount's move counters.
# Ring files move between the nodes under a writer and readers, then under
# libmxl's own writer and reader; a pin, a view's mapping, a hold and a
# full destination each refuse what they must; the folios keep their
# orders under both huge= modes; the counters stay exact through a random
# sequence of everything that adds, moves or drops pages; and the calls
# that change placement need CAP_SYS_NICE. Needs two NUMA nodes with memory
# and CPUs, as dev/vm.sh boots with
# VNG_OPTS='--numa 1536M,cpus=0-1 --numa 1536M,cpus=2-3'. Runs as root under
# dev/vm.sh or dev/host.sh.
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
on0() { taskset -c $cpu0 "$@"; }
mnt=/tmp/move
fill=/tmp/move-fill
mkdir -p $mnt $fill
opts=${MXLFS_OPTS:+,$MXLFS_OPTS}
seconds=${BENCH_SECONDS:-30}

# Under load: the accessors' time per 2 MiB block without moves, then
# with every ring file moving between the nodes, then with a scraper
# calling MXLFS_IOC_NODE_PAGES 10^6 times meanwhile. What fails a run is
# what a move must never do: a stale or torn grain, a block left behind, a
# lost PMD mapping, a split THP; and readers that missed more than 5% of
# the grains. Times depend on the machine: a
# slow or shared runner moves more slowly and stalls a guest's vCPUs for
# milliseconds, moves or not. So the kernel's median block window is
# bounded relative to a 2 MiB copy timed on the same guest, and with
# STRICT=1, for a quiet local machine, also absolutely: 99% of windows
# under 1 ms and the accessors' 99th percentile under 1 ms or twice what
# the run without moves measured. Otherwise those are printed only.
mount -t mxlfs -o huge=always$opts mxlfs $mnt
MOVES=0 on0 $h movebench $mnt $seconds > /tmp/mb0.out 2>&1 || true; cat /tmp/mb0.out
check "ring files without moves: every grain whole" 'grep -q " failed=0$" /tmp/mb0.out'
bound=$(sed -nE 's/.* writer_block_us_p50=[0-9.]+ p99=([0-9.]+) .* reader_block_us_p50=[0-9.]+ p99=([0-9.]+) .*/\1 \2/p' /tmp/mb0.out |
  awk '{ b = 2 * ($1 > $2 ? $1 : $2); print int(b > 1000 ? b : 1000) }')
strict=
[ "${STRICT:-0}" = 1 ] && strict="STALL_P99_US=${bound:-1000} WINDOW_UNDER_1MS_PERMILLE=990"
env $strict taskset -c $cpu0 $h movebench $mnt $seconds > /tmp/mb1.out 2>&1 || true; cat /tmp/mb1.out
check "ring files moving under a writer and readers: nothing stale or torn, PMDs kept, windows in line with a 2 MiB copy${strict:+, $strict}" \
  'grep -q " failed=0$" /tmp/mb1.out && ! grep -q " passes=0 " /tmp/mb1.out'
env $strict SCRAPE=1000000 taskset -c $cpu0 $h movebench $mnt $seconds > /tmp/mb2.out 2>&1 || true; cat /tmp/mb2.out
check "10^6 NODE_PAGES calls during the moves change none of that" \
  'grep -q " scrapes=1000000 " /tmp/mb2.out && grep -q " failed=0$" /tmp/mb2.out'
umount $mnt

# libmxl's writer and reader, the grain files moving between the nodes.
mount -t mxlfs -o huge=always$opts mxlfs $mnt
domain=$mnt/domain
id=99999999-9999-4999-8999-999999999999
mkdir -p $domain
echo '{"urn:x-mxl:option:history_duration/v1.0": 1000000000}' > $domain/options.json
SECONDS=24 taskset -c $cpu0 $h writer $domain $id 0 > /tmp/mw.out & w=$!
for i in $(seq 50); do [ -e $domain/$id.mxl-flow/grains/data.0 ] && break; sleep 0.1; done
SECONDS=16 taskset -c $cpu1 $h reader $domain $id > /tmp/mr.out & r=$!
moves=0 partial=0 node=1 end=$(($(date +%s) + 14))
while [ "$(date +%s)" -lt $end ]; do
  for f in $domain/$id.mxl-flow/grains/data.*; do
    $h move $f $node > /tmp/mv.out || true
    grep -q " result=1 " /tmp/mv.out || { partial=$((partial + 1)); cat /tmp/mv.out; }
    moves=$((moves + 1))
  done
  node=$((1 - node))
done
rrc=0; wait $r || rrc=$?
wait $w || true
cat /tmp/mw.out /tmp/mr.out
check "a libmxl reader sees every grain whole while its ring moves" \
  '[ $rrc = 0 ] && grep -q " wrong_index=0 torn=0 late=0 " /tmp/mr.out'
check "every one of $moves moves of a grain file moved all of it" '[ $moves -gt 0 ] && [ $partial = 0 ]'
umount $mnt

# What refuses a block, the flags, and holds; the files start on node 0.
mount -t mxlfs ${MXLFS_OPTS:+-o $MXLFS_OPTS} mxlfs $mnt
for t in movepin moveview moveflags movehold; do
  on0 $h $t $mnt > /tmp/$t.out 2>&1 || true; cat /tmp/$t.out
done
check "a pinned block stays, the others move, a strict move fails with EBUSY" 'grep -q "^movepin failed=0$" /tmp/movepin.out'
check "a block a view maps stays, and the view reads the same data" 'grep -q "^moveview failed=0$" /tmp/moveview.out'
check "a dry run changes nothing, a strict move under a view moves nothing" 'grep -q "^moveflags failed=0$" /tmp/moveflags.out'
check "a hold refuses moves through other files until its last close" 'grep -q "^movehold failed=0$" /tmp/movehold.out'
umount $mnt

for huge in within_size always; do
  o=
  [ $huge = always ] && o=-ohuge=always
  mount -t mxlfs $o mxlfs $mnt
  HUGE=$huge on0 $h moveorders $mnt > /tmp/orders.out 2>&1 || true; cat /tmp/orders.out
  check "huge=$huge: folios keep their orders and contents across moves" 'grep -q "^moveorders huge=$huge failed=0$" /tmp/orders.out'
  umount $mnt
done

# No room on node 1: nothing moves, nothing splits.
mount -t mxlfs -o size=0$opts mxlfs $mnt
mount -t mxlfs -o numa_node=1,size=0 mxlfs $fill
on0 $h movenomem $mnt $fill > /tmp/nomem.out 2>&1 || true; cat /tmp/nomem.out
check "a move to a node without room fails with ENOMEM and moves nothing" 'grep -q "^movenomem failed=0$" /tmp/nomem.out'
umount $fill $mnt

# A mount of its own: its pages are all the counters see.
mount -t mxlfs -o size=0 mxlfs $mnt
on0 $h numacount $mnt ${COUNT_STEPS:-400} > /tmp/count.out 2>&1 || true; cat /tmp/count.out
check "the counters equal a walk of the page cache after every step" 'grep -q " failed=0$" /tmp/count.out'
umount $mnt

# Placement needs CAP_SYS_NICE; reading it and a dry run need nothing.
mount -t mxlfs mxlfs $mnt
on0 fallocate -l 4M $mnt/priv
nobody() { setpriv --reuid=65534 --regid=65534 --clear-groups "$@"; }
check "MOVE without CAP_SYS_NICE is refused" 'nobody $h move $mnt/priv 1 | grep -q "rc=Operation not permitted"'
check "SET_NODE without CAP_SYS_NICE is refused" 'nobody $h setnode $mnt/priv 1 | grep -q "rc=Operation not permitted"'
check "a dry run needs no privilege" 'nobody $h move $mnt/priv 1 dry | grep -q "rc=0 result=1 "'
check "NODE_PAGES and MOUNT_NODES need no privilege" \
  'nobody $h nodepages $mnt/priv | grep -q " node0=1024 node1=0" && nobody $h mountnodes $mnt/priv | grep -q "node=0 pages=1024 "'
umount $mnt

dmesg | grep -E 'BUG: |WARNING: CPU|KASAN|circular locking|Call Trace|Busy inodes' && fails=$((fails + 1))
echo "fails=$fails"
[ $fails = 0 ]

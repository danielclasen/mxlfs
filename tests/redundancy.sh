#!/bin/sh
# Two members with random drops and staggered outages, a view flow merged
# from them, readers on the view and on each member, and a reader of the
# oldest grain the view offers. FORMAT is video
# (default), data (ANC) or audio; drops count grains, or batches of BATCH
# samples for audio, and SKEW is in grains or samples. Runs as root in the VM
# (dev/vm.sh or dev/host.sh), with an mxlfs mounted at $MXLFS_MNT
# (default /dev/shm).
set -eu
MNT=${MXLFS_MNT:-/dev/shm}
h=tools/harness
domain=$MNT/domain
a=11111111-1111-4111-8111-111111111111
b=22222222-2222-4222-8222-222222222222
v=33333333-3333-4333-8333-333333333333
readers=${READERS:-2}
export SECONDS_RUN=${SECONDS:-30}
export FORMAT=${FORMAT:-video}
[ "$FORMAT" = audio ] && skew=${SKEW:-2048} || skew=${SKEW:-2}
export MEMBERS=2 SEED=${SEED:-7} DROP_PERMILLE=${DROP_PERMILLE:-50} \
       OUTAGE_PERIOD=${OUTAGE_PERIOD:-500} OUTAGE_LEN=${OUTAGE_LEN:-100}

rm -rf $domain
mkdir -p $domain
# Audio writers report committed batches to the merger here.
[ "$FORMAT" = audio ] && mkfifo $domain/.ev-0 $domain/.ev-1
echo '{"urn:x-mxl:option:history_duration/v1.0": 400000000}' > $domain/options.json

SECONDS=$((SECONDS_RUN + 4)) $h writer $domain $a 0 & wa=$!
SECONDS=$((SECONDS_RUN + 4)) $h writer $domain $b 1 & wb=$!
# A large ring takes a while to allocate, and the merger needs both members.
for i in $(seq 100); do
  [ -e $domain/$a.mxl-flow/data ] && [ -e $domain/$b.mxl-flow/data ] && break
  sleep 0.1
done
$h merge $domain $v $skew $a $b > /tmp/merge.out & m=$!
sleep 1
pids=
for i in $(seq "$readers"); do
  SECONDS=$SECONDS_RUN $h reader $domain $v > /tmp/reader-$i.out & pids="$pids $!"
done
MEMBERS=1 SECONDS=$SECONDS_RUN $h reader $domain $a > /tmp/reader-a.out & pa=$!
# The oldest grains of the view, which the members overwrite first.
[ "$FORMAT" = audio ] || { TAIL=1 SECONDS=$SECONDS_RUN $h reader $domain $v > /tmp/tail.out & pt=$!; }

rc=0
for p in $pids; do wait $p || rc=1; done
wait $pa || true
if [ "$FORMAT" != audio ]; then
  wait $pt || true
  sed 's/^reader/tail reader/' /tmp/tail.out
  # It may see INVALID where a slot was retired, never another grain.
  grep -q ' wrong_index=0 torn=0 ' /tmp/tail.out || rc=1
fi
kill $m; wait $m > /dev/null || true
wait $wa $wb || true
cat /tmp/reader-*.out
grep -h merge /tmp/merge.out 2>/dev/null || true
$h stats $domain/options.json

# Tearing the domain down must return every charged byte.
rm -rf $domain
touch $MNT/.probe
$h stats $MNT/.probe | tee /tmp/stats.out
grep -q ' allocated=0 ' /tmp/stats.out || rc=1
rm $MNT/.probe
exit $rc

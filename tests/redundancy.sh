#!/bin/sh
# Two members with random drops and staggered outages, a view flow merged
# from them, readers on the view and on each member. Runs as root in the VM
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
export MEMBERS=2 SEED=${SEED:-7} DROP_PERMILLE=${DROP_PERMILLE:-50} \
       OUTAGE_PERIOD=${OUTAGE_PERIOD:-500} OUTAGE_LEN=${OUTAGE_LEN:-100}

mkdir -p $domain
echo '{"urn:x-mxl:option:history_duration/v1.0": 400000000}' > $domain/options.json

SECONDS=$((SECONDS_RUN + 4)) $h writer $domain $a 0 & wa=$!
SECONDS=$((SECONDS_RUN + 4)) $h writer $domain $b 1 & wb=$!
$h merge $domain $v ${SKEW:-2} $a $b & m=$!
sleep 1
pids=
for i in $(seq "$readers"); do
  SECONDS=$SECONDS_RUN $h reader $domain $v > /tmp/reader-$i.out & pids="$pids $!"
done
MEMBERS=1 SECONDS=$SECONDS_RUN $h reader $domain $a > /tmp/reader-a.out & pa=$!

rc=0
for p in $pids; do wait $p || rc=1; done
wait $pa || true
kill $m; wait $m || true
wait $wa $wb || true
cat /tmp/reader-*.out
$h stats $domain/options.json

# Tearing the domain down must return every charged byte.
rm -rf $domain
touch $MNT/.probe
$h stats $MNT/.probe | tee /tmp/stats.out
grep -q ' allocated=0 ' /tmp/stats.out || rc=1
rm $MNT/.probe
exit $rc

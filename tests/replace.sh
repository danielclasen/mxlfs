#!/bin/sh
# Two members and a view flow merged from them with REPLACE=1. The primary
# member's writer is killed and its flow deleted and recreated by a new
# writer, as a restarted gateway would; the merger replaces that member's
# ring in every grain view of the existing view flow. Then the other member
# stops for good, so the view serves the new ring alone. Readers open the
# view once and read throughout: every grain or sample must carry its own
# index, and no grain may be INVALID nor any page silence, as some member
# always holds it. FORMAT is video (default), data or audio; for audio the
# ring is replaced in the view's channels file, whose pages select members
# one by one. Runs as root in the VM (dev/vm.sh or dev/host.sh), with an
# mxlfs mounted at $MXLFS_MNT (default /dev/shm).
set -eu
MNT=${MXLFS_MNT:-/dev/shm}
h=tools/harness
domain=$MNT/domain
a=11111111-1111-4111-8111-111111111111
b=22222222-2222-4222-8222-222222222222
v=33333333-3333-4333-8333-333333333333
run=${SECONDS:-24}
export FORMAT=${FORMAT:-video}
export MEMBERS=2 DROP_PERMILLE=0 OUTAGE_PERIOD=0
[ "$FORMAT" = audio ] && skew=2048 || skew=2

rm -rf $domain
mkdir -p $domain
# Audio writers report committed batches to the merger through .ev-MEMBER,
# and the merger reads .ev-N for its Nth member: b is member 0.
[ "$FORMAT" = audio ] && mkfifo $domain/.ev-0 $domain/.ev-1
echo '{"urn:x-mxl:option:history_duration/v1.0": 1000000000}' > $domain/options.json

SECONDS=$((run + 4)) $h writer $domain $b 0 & wb=$!
SECONDS=$((run + 4)) $h writer $domain $a 1 & wa=$!
for i in $(seq 100); do
  [ -e $domain/$a.mxl-flow/data ] && [ -e $domain/$b.mxl-flow/data ] && break
  sleep 0.1
done
# b comes first, so it is the primary until it is killed.
REPLACE=1 $h merge $domain $v $skew $b $a > /tmp/merge.out & m=$!
sleep 1
pids=
for i in 1 2; do
  SECONDS=$run $h reader $domain $v > /tmp/reader-$i.out & pids="$pids $!"
done

sleep $((run / 4))
kill -KILL $wb; wait $wb || true
rm -rf $domain/$b.mxl-flow
sleep 1
SECONDS=$run $h writer $domain $b 0 & wb=$!
sleep $((run / 4))
kill -KILL $wa; wait $wa || true

rc=0
for p in $pids; do wait $p || rc=1; done
kill $m; wait $m > /dev/null || true
wait $wb || true
cat /tmp/reader-*.out
cat /tmp/merge.out
$h stats $domain/options.json
replaced=$(sed -n 's/^a\{0,1\}merge .* replaced=\([0-9]*\) .*/\1/p' /tmp/merge.out)
renewed=$(sed -n 's/^a\{0,1\}merge .* renewed_picks=\([0-9]*\).*/\1/p' /tmp/merge.out)
# The recreated ring was swapped in once and then served the view.
[ "$replaced" = 1 ] && [ "${renewed:-0}" -gt 0 ] || { echo "replaced=$replaced renewed_picks=$renewed" >&2; rc=1; }

# Tearing the domain down must return every charged byte, the old ring's too.
rm -rf $domain
touch $MNT/.probe
$h stats $MNT/.probe | tee /tmp/stats.out
grep -q ' allocated=0 ' /tmp/stats.out || rc=1
rm $MNT/.probe
dmesg | grep -E 'BUG: |WARNING: CPU|KASAN|circular locking|Call Trace' && rc=1
exit $rc

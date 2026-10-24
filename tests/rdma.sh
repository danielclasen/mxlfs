#!/bin/sh
# libmxl-fabrics over soft-RoCE (one rdma_rxe device on a veth, which rxe
# loops back internally; two local addresses would route over lo). The target's
# flow lives on mxlfs, so every grain is an RDMA write into mxlfs pages
# registered as long-term pinned MRs. A peek at the target's head grain
# checks the payload markers on every page. Runs as root in the VM (dev/vm.sh).
set -eu
MNT=${MXLFS_MNT:-/dev/shm}
h=tools/harness
demo=/opt/libmxl/bin/mxl-fabrics-demo
id=44444444-4444-4444-8444-444444444444
export SECONDS_RUN=${SECONDS:-20} MEMBERS=1 DROP_PERMILLE=0 OUTAGE_PERIOD=0

dst=${DST:-$MNT/dst}
if ! [ -e /sys/class/infiniband/rxe0 ]; then
  modprobe rdma_rxe; modprobe veth
  ip link add veth0 type veth peer name veth1
  ip addr add 10.99.0.1/24 dev veth0
  ip link set veth0 up; ip link set veth1 up
  rdma link add rxe0 type rxe netdev veth0
fi
rm -rf $MNT/src $dst /tmp/target-info

for d in $MNT/src $dst; do
  mkdir -p $d
  echo '{"urn:x-mxl:option:history_duration/v1.0": 400000000}' > $d/options.json
done
$h flowdef $id > /tmp/flow.json

SECONDS=$((SECONDS_RUN + 6)) $h writer $MNT/src $id 0 & w=$!
sleep 1
$demo -d $dst -f /tmp/flow.json -p verbs -n 10.99.0.1 -s 7777 -t /tmp/target-info > /tmp/target.log 2>&1 & t=$!
for i in $(seq 100); do [ -s /tmp/target-info ] && break; sleep 0.1; done
$demo -d $MNT/src -f $id -i -p verbs -n 10.99.0.1 -t @/tmp/target-info > /tmp/initiator.log 2>&1 & in=$!
sleep 2
rc=0
SECONDS=$SECONDS_RUN timeout $((SECONDS_RUN + 20)) $h reader $dst $id > /tmp/reader.out || rc=$?
echo "reader exit=$rc"; cat /tmp/reader.out
# Payload written by RDMA must be what a reader of the target maps.
$h peek $dst $id || rc=1
kill $in $t 2>/dev/null || true
wait $w || true
for f in /tmp/target.log /tmp/initiator.log; do echo "== $f"; tail -n 15 $f; done
ls -la $dst
exit $rc

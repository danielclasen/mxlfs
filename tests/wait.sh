#!/bin/sh
# Waiting for missing flows (mount option wait=). Runs as root in the VM
# (dev/vm.sh), which mounts an mxlfs at /dev/shm; this test mounts its own
# at /tmp/w with wait=2000.
set -eu
h=tools/harness
mnt=/tmp/w
mkdir -p $mnt
mount -t mxlfs -o wait=2000 mxlfs $mnt
mkdir $mnt/domain
echo '{"urn:x-mxl:option:history_duration/v1.0": 400000000}' > $mnt/domain/options.json
f=55555555-5555-4555-8555-555555555555
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
ms() { date +%s%3N; }

# 1. A walk through a missing flow waits; the flow's creation wakes it.
t0=$(ms)
( rc=0; stat -c %s $mnt/domain/$f.mxl-flow/data > /tmp/stat.out 2>&1 || rc=$?; echo $rc > /tmp/stat.rc ) &
sleep 0.5
mkdir $mnt/domain/.mxl-tmp-a && : > $mnt/domain/.mxl-tmp-a/data
mv -T --no-clobber $mnt/domain/.mxl-tmp-a $mnt/domain/$f.mxl-flow
wait
check "waiter sees a flow renamed into place ($(($(ms) - t0)) ms)" '[ "$(cat /tmp/stat.rc)" = 0 ]'
rm -r $mnt/domain/$f.mxl-flow

# 2. Without the flow, the walk fails with ENOENT after wait= ms.
t0=$(ms)
rc=0; stat $mnt/domain/$f.mxl-flow/data > /tmp/stat.out 2>&1 || rc=$?
el=$(($(ms) - t0))
check "missing flow fails after the timeout ($el ms)" '[ $rc != 0 ] && [ $el -ge 1900 ] && grep -q "No such file" /tmp/stat.out'

# 3. Names that are not flows do not wait.
t0=$(ms)
stat $mnt/domain/nothing > /dev/null 2>&1 || true
check "a non-flow name fails at once" '[ $(($(ms) - t0)) -lt 500 ]'

# 4. Concurrent waiters raise one request; a privileged process fails them.
for i in 1 2 3 4 5 6; do
  ( rc=0; stat $mnt/domain/$f.mxl-flow/data > /tmp/w$i.out 2>&1 || rc=$?; echo $rc > /tmp/w$i.rc ) &
done
sleep 0.3
$h wanted $mnt 1000 fail 13 > /tmp/wanted.out
cat /tmp/wanted.out
wait
check "one request names the flow" 'grep -q "path=/domain/$f.mxl-flow" /tmp/wanted.out'
check "no second request for the same flow" '! $h wanted $mnt 200 > /dev/null'
check "failed waiters get the errno" '[ "$(cat /tmp/w*.rc | sort -u)" = 1 ] && grep -q "Permission denied" /tmp/w1.out'

# 5. A killed waiter leaves no request behind that blocks the next one.
stat $mnt/domain/$f.mxl-flow/data > /dev/null 2>&1 & p=$!
sleep 0.3; kill -9 $p; wait $p 2>/dev/null || true
$h wanted $mnt 200 > /dev/null || true
check "a killed waiter returns" 'true'

# 5b. The request names the waiter as the reporting process sees it: an
# agent in a container's pid namespace must find the waiting pod by it.
g=88888888-0000-4000-8000-000000000005
unshare --pid --fork sh -c "exec stat $mnt/domain/$g.mxl-flow/data > /dev/null 2>&1" & u=$!
sleep 0.3
inner=$(pgrep -x stat | head -1)
$h wanted $mnt 1000 fail 2 > /tmp/wanted-host.out || true
check "a waiter in a child pid namespace is named by its host pid ($inner)" 'grep -q "pid=$inner " /tmp/wanted-host.out'
wait $u 2>/dev/null || true
h2=99999999-0000-4000-8000-000000000005
stat $mnt/domain/$h2.mxl-flow/data > /dev/null 2>&1 &
sleep 0.3
unshare --pid --fork --mount-proc $h wanted $mnt 1000 fail 2 > /tmp/wanted-child.out || true
check "a waiter outside the reporter's pid namespace is named 0" 'grep -q "pid=0 " /tmp/wanted-child.out'
wait

# 6. libmxl: a reader created before the writer gets the flow in one call.
t0=$(ms)
SECONDS=3 $h reader $mnt/domain $f > /tmp/reader.out 2>&1 & r=$!
sleep 0.5
MEMBERS=1 SECONDS=5 $h writer $mnt/domain $f 0 > /dev/null
wait $r || true
cat /tmp/reader.out
check "libmxl reader reads a flow created after it started" 'grep -q " ok=[1-9]" /tmp/reader.out'

# 7. The writer that creates a flow is not blocked by waiters on it.
t0=$(ms)
MEMBERS=1 SECONDS=1 $h writer $mnt/domain 77777777-7777-4777-8777-777777777777 0 > /dev/null
base=$(($(ms) - t0))
( stat $mnt/domain/66666666-6666-4666-8666-666666666666.mxl-flow/data > /dev/null 2>&1 ) &
sleep 0.2
t0=$(ms)
MEMBERS=1 SECONDS=1 $h writer $mnt/domain 66666666-6666-4666-8666-666666666666 0 > /dev/null
el=$(($(ms) - t0))
wait
check "a writer under waiters is not delayed ($el ms, $base ms without waiters)" '[ $el -lt $((base + 500)) ]'

umount $mnt
dmesg | grep -E 'BUG: |WARNING: CPU|KASAN|circular locking|Call Trace' && fails=$((fails + 1))
echo "fails=$fails"
[ $fails = 0 ]

#!/bin/sh
# Waiting for missing flows (mount option wait=). Runs as root in the VM
# (dev/vm.sh), which mounts an mxlfs at /dev/shm; this test mounts its own
# at /tmp/w with wait=2000 and the options in MXLFS_OPTS.
set -eu
h=tools/harness
mnt=/tmp/w
mkdir -p $mnt
mount -t mxlfs -o wait=2000${MXLFS_OPTS:+,$MXLFS_OPTS} mxlfs $mnt
mkdir $mnt/domain
echo '{"urn:x-mxl:option:history_duration/v1.0": 400000000}' > $mnt/domain/options.json
f=55555555-5555-4555-8555-555555555555
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
ms() { date +%s%3N; }

# 1. A walk through a missing flow waits; the flow's creation wakes it.
# The waiter's request being posted is what proves it is blocked.
rm -f /tmp/stat.rc
t0=$(ms)
( rc=0; stat -c %s $mnt/domain/$f.mxl-flow/data > /tmp/stat.out 2>&1 || rc=$?; echo $rc > /tmp/stat.rc ) &
$h wanted $mnt 1000 > /dev/null
mkdir $mnt/domain/.mxl-tmp-a && : > $mnt/domain/.mxl-tmp-a/data
mv -T --no-clobber $mnt/domain/.mxl-tmp-a $mnt/domain/$f.mxl-flow
wait
el=$(($(ms) - t0))
check "waiter sees a flow renamed into place ($el ms)" '[ "$(cat /tmp/stat.rc)" = 0 ] && [ $el -lt 1500 ]'
rm -r $mnt/domain/$f.mxl-flow

# 1b. The rename wakes the waiter rather than the timeout: a woken walk
# and one retried after wait= both succeed, so only the time tells them
# apart. Covers a plain rename(2), which mv never issues (it asks for
# RENAME_NOREPLACE), and a flow that existed before, was removed while a
# reader held a file in it, and is created again.
for mode in plain recreate; do
  if [ $mode = recreate ]; then
    mkdir $mnt/domain/$f.mxl-flow && : > $mnt/domain/$f.mxl-flow/data
    exec 3< $mnt/domain/$f.mxl-flow/data
    rm -r $mnt/domain/$f.mxl-flow
  fi
  rm -f /tmp/stat.rc
  t0=$(ms)
  ( rc=0; stat -c %s $mnt/domain/$f.mxl-flow/data > /tmp/stat.out 2>&1 || rc=$?; echo $rc > /tmp/stat.rc ) &
  $h wanted $mnt 1000 > /dev/null
  mkdir $mnt/domain/.mxl-tmp-b && : > $mnt/domain/.mxl-tmp-b/data
  if [ $mode = plain ]; then
    perl -e 'rename($ARGV[0], $ARGV[1]) or die "$!\n"' $mnt/domain/.mxl-tmp-b $mnt/domain/$f.mxl-flow
  else
    mv -T $mnt/domain/.mxl-tmp-b $mnt/domain/$f.mxl-flow
  fi
  wait
  el=$(($(ms) - t0))
  check "a $mode rename wakes the waiter ($el ms)" '[ "$(cat /tmp/stat.rc)" = 0 ] && [ $el -lt 1500 ]'
  [ $mode = recreate ] && exec 3<&-
  rm -r $mnt/domain/$f.mxl-flow
done

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

# 8. The caps: a thread group waits for at most 256 missing flows at once
# and a mount for at most 4096; a walk past either fails with ENOENT at
# once, as without wait=, and counts in wanted_refused. Waiting ends by
# SIGKILL of the waiters, so wait= only has to outlast the setup; once they
# are reaped the mount waits for nothing.
cap=/tmp/w-cap
mkdir -p $cap
mount -t mxlfs -o wait=60000${MXLFS_OPTS:+,$MXLFS_OPTS} mxlfs $cap
trap 'umount $cap 2>/dev/null || true' EXIT
mkdir $cap/domain
rc=0
python3 - $cap/domain $h > /tmp/cap.out <<'PY' || rc=$?
import os, re, select, signal, subprocess, sys, threading, time, traceback

d, harness = sys.argv[1], sys.argv[2]
DEADLINE = 60

def wanted():
    out = subprocess.run([harness, "stats", d], capture_output=True, text=True, check=True).stdout
    m = re.search(r" wanted=(\d+)", out)
    if not m:
        sys.exit("harness stats has no wanted= field: %r" % out)
    return int(m.group(1))

def wanted_until(ok, what):
    """Waits until ok(wanted) holds, for at most DEADLINE seconds."""
    end = time.monotonic() + DEADLINE
    while True:
        n = wanted()
        if ok(n):
            return n
        if time.monotonic() > end:
            sys.exit("timeout wanted=%d, waiting for %s" % (n, what))
        time.sleep(0.1)

def walk(name):
    t = time.monotonic()
    try:
        os.stat("%s/%s.mxl-flow/data" % (d, name))
        r = "found"
    except OSError as e:
        r = os.strerror(e.errno)
    return r, time.monotonic() - t

def group(prefix, report=None):
    """A thread group of 256 threads, each waiting for its own missing
    flow; with report, it then walks a 257th and writes the result."""
    for i in range(256):
        threading.Thread(target=walk, args=("%s-%d" % (prefix, i),), daemon=True).start()
    if report is not None:
        wanted_until(lambda n: n >= 256, "256 flows wanted by one thread group")
        r, el = walk(prefix + "-over")
        os.write(report, ("%s %.3f\n" % (r, el)).encode())
    time.sleep(600)

def fork(fn, *args):
    """Runs fn in a child that never returns into this code."""
    pid = os.fork()
    if pid == 0:
        code = 1
        try:
            fn(*args)
            code = 0
        except BaseException:
            traceback.print_exc()
        finally:
            os._exit(code)
    return pid

kids = []
try:
    rd, wr = os.pipe()
    kids.append(fork(group, "g0", wr))
    os.close(wr)
    if not select.select([rd], [], [], DEADLINE + 30)[0]:
        sys.exit("timeout: no result from the thread group at its cap")
    # The mount holds 256 entries, so only the thread group's cap applies.
    print("tgid_over " + os.read(rd, 100).decode().strip(), flush=True)
    for g in range(1, 16):
        kids.append(fork(group, "g%d" % g))
    wanted_until(lambda n: n >= 4096, "4096 flows wanted by 16 thread groups")
    # 16 x 256 = 4096 entries: this thread group has none, the mount is full.
    r, el = walk("sb-over")
    print("sb_over %s %.3f" % (r, el), flush=True)
finally:
    for k in kids:
        os.kill(k, signal.SIGKILL)
    for k in kids:
        os.waitpid(k, 0)
print("drained wanted=%d" % wanted_until(lambda n: n == 0, "the waiters to leave"), flush=True)
PY
cat /tmp/cap.out
check "the cap test ran to its end (rc=$rc)" '[ $rc = 0 ]'
check "a thread group past its 256 waits is refused at once" 'grep -Eq "^tgid_over No such file or directory 0\.[0-2]" /tmp/cap.out'
check "a mount past its 4096 waits is refused at once" 'grep -Eq "^sb_over No such file or directory 0\.[0-2]" /tmp/cap.out'
check "the mount waits for nothing once the waiters are gone" 'grep -q "^drained wanted=0$" /tmp/cap.out'
$h stats $cap/domain > /tmp/cap-stats.out || true
cat /tmp/cap-stats.out
check "both refusals count in wanted_refused" 'grep -q " wanted_refused=2$" /tmp/cap-stats.out'
umount $cap
trap - EXIT

dmesg | grep -E 'BUG: |WARNING: CPU|KASAN|circular locking|Call Trace' && fails=$((fails + 1))
echo "fails=$fails"
[ $fails = 0 ]

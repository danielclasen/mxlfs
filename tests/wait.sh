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

# 3b. The VFS refuses a RESOLVE_NO_XDEV walk a managed dentry with EXDEV,
# as a mount crossing. Such a walk does not wait for a missing flow: it
# fails with ENOENT at once, and leaves nothing behind that would keep a
# later walk from waiting. Only while another walk holds the name, here
# one that waits, does it meet the managed dentry, and EXDEV.
p=$mnt/domain/$f.mxl-flow/data
$h noxdev $mnt domain/$f.mxl-flow/data > /tmp/noxdev.out
check "a RESOLVE_NO_XDEV walk fails a missing flow with ENOENT at once" \
  'grep -Eq "^noxdev No such file or directory 0\.[0-4]" /tmp/noxdev.out'
t0=$(ms)
stat $p > /dev/null 2>&1 || true
el=$(($(ms) - t0))
check "a walk after it still waits ($el ms)" '[ $el -ge 1900 ]'
$h noxdev $mnt domain/$f.mxl-flow/data > /tmp/noxdev.out
check "after a timed-out wait, a RESOLVE_NO_XDEV walk gets ENOENT" \
  'grep -Eq "^noxdev No such file or directory 0\.[0-4]" /tmp/noxdev.out'
( stat $p > /dev/null 2>&1 || true ) &
$h wanted $mnt 1000 > /dev/null
$h noxdev $mnt domain/$f.mxl-flow/data > /tmp/noxdev.out
wait
check "while a walk waits, a RESOLVE_NO_XDEV walk gets EXDEV" \
  'grep -q "^noxdev Invalid cross-device link" /tmp/noxdev.out'
$h noxdev $mnt domain > /tmp/noxdev.out
check "a RESOLVE_NO_XDEV walk to a directory goes through" 'grep -q "^noxdev ok" /tmp/noxdev.out'

# 4. Concurrent waiters raise one request; a privileged process fails them,
# with input WANTED_FAIL checks.
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
check "WANTED_FAIL takes errnos 1 to 511 and refuses 512 and a non-zero pad" 'grep -q "bad_input_wrong=0" /tmp/wanted.out'

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

# 5c. WANTED_NEXT keeps a flow it could not hand to the caller for the
# next call.
rc=0
$h wantcheck $mnt > /tmp/wantcheck.out || rc=$?
cat /tmp/wantcheck.out
check "a flow WANTED_NEXT failed to copy out is reported again (rc=$rc)" 'grep -q "efault_lost=0" /tmp/wantcheck.out'

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

# 7b. A remount may repeat the mount's options but change none of them;
# the VFS's flags such as ro change as on any filesystem. Whether
# numa_node= has memory is checked at mount, not on a remount repeating it.
check "a remount repeating wait= goes through" 'mount -o remount,wait=2000 $mnt'
check "a remount changing wait= is refused" '! mount -o remount,wait=5000 $mnt 2>/dev/null'
check "a remount changing size= is refused" '! mount -o remount,size=1M $mnt 2>/dev/null'
check "a remount changing mode= is refused" '! mount -o remount,mode=700 $mnt 2>/dev/null'
huge=always
grep " $mnt mxlfs " /proc/mounts | grep -q ",huge=always" && huge=within_size
check "a remount changing huge= is refused" '! mount -o remount,huge=$huge $mnt 2>/dev/null'
node=$(grep " $mnt mxlfs " /proc/mounts | sed -n 's/.*,numa_node=\([0-9]*\).*/\1/p')
node=$((${node:--1} + 1))
check "a remount changing numa_node= is refused" '! mount -o remount,numa_node=$node $mnt 2>/dev/null'
mkdir -p /tmp/w-node
mount -t mxlfs -o numa_node=0 mxlfs /tmp/w-node
check "a remount repeating numa_node= goes through" 'mount -o remount,numa_node=0 /tmp/w-node'
umount /tmp/w-node
check "the mount keeps its options" 'grep " $mnt mxlfs " /proc/mounts | grep -q ",wait=2000" && ! grep " $mnt mxlfs " /proc/mounts | grep -q ",mode=700"'
check "a remount read-only and back goes through" 'mount -o remount,ro $mnt && mount -o remount,rw $mnt'

umount $mnt

# 8. The caps: a thread group waits for at most 256 missing flows at once
# and a mount for at most 4096; a walk past either fails with ENOENT at
# once, as without wait=, and counts in wanted_refused. Waiting ends by
# SIGKILL of the waiters, so wait= only has to outlast the test; once they
# are reaped the mount waits for nothing. The waiters die with the script,
# so a failed run does not leave the mount busy.
cap=/tmp/w-cap
mkdir -p $cap
mount -t mxlfs -o wait=600000${MXLFS_OPTS:+,$MXLFS_OPTS} mxlfs $cap
trap 'umount $cap 2>/dev/null || true' EXIT
mkdir $cap/domain
rc=0
python3 - $cap/domain $h > /tmp/cap.out <<'PY' || rc=$?
import ctypes, os, re, select, signal, subprocess, sys, threading, time, traceback

d, harness = sys.argv[1], sys.argv[2]
DEADLINE = 60
PR_SET_PDEATHSIG = 1
libc = ctypes.CDLL(None, use_errno=True)

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
    """Runs fn in a child that never returns into this code and is
    killed when this process dies."""
    parent = os.getpid()
    pid = os.fork()
    if pid == 0:
        code = 1
        try:
            if libc.prctl(PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0):
                raise OSError(ctypes.get_errno(), "prctl(PR_SET_PDEATHSIG)")
            if os.getppid() != parent:
                sys.exit("parent gone before PR_SET_PDEATHSIG")
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

# 9. A walk that misses a flow allocates its entry outside the lock and
# looks again. Walks through one missing flow released together make one
# entry and all find the flow; two walks through new flows released
# together, one entry short of the thread group's cap, leave exactly one
# refused.
rc=0
$h wantrace $cap/domain 200 > /tmp/wantrace.out || rc=$?
cat /tmp/wantrace.out
check "walks racing to wait make one entry per flow and keep the cap (rc=$rc)" \
  'grep -q "same_dup=0 same_failed=0 cap_wrong=0 refused=200$" /tmp/wantrace.out'
umount $cap
trap - EXIT

dmesg | grep -E 'BUG: |WARNING: CPU|KASAN|circular locking|Call Trace' && fails=$((fails + 1))
echo "fails=$fails"
[ $fails = 0 ]

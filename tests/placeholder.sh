#!/bin/sh
# A placeholder flow: a complete libmxl flow whose ring takes no memory,
# listed in the domain like any other. A reader can open it and wait; a
# fallocate from another process allocates the ring in 2 MiB folios, and a
# second writer adopts the flow (libmxl opens an existing flow on EEXIST). Runs as root under dev/vm.sh or
# dev/host.sh with an mxlfs at $MXLFS_MNT (default /dev/shm).
set -eu
MNT=${MXLFS_MNT:-/dev/shm}
h=tools/harness
domain=$MNT/placeholder
f=88888888-8888-4888-8888-888888888888
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
alloc() { $h stats $domain/options.json | sed -E 's/.* allocated=([0-9]+) .*/\1/'; }

rm -rf $domain; mkdir -p $domain
echo '{"urn:x-mxl:option:history_duration/v1.0": 400000000}' > $domain/options.json
base=$(alloc)

$h placeholder $domain $f > /tmp/ph.out & ph=$!
for i in $(seq 50); do grep -q placeholder /tmp/ph.out 2>/dev/null && break; sleep 0.1; done
cat /tmp/ph.out
used=$(($(alloc) - base))
ring=$(du -b --apparent-size $domain/$f.mxl-flow/grains | cut -f1)
check "placeholder is listed" 'ls $domain | grep -q "^$f.mxl-flow$"'
# libmxl writes each grain's 8 KiB header when it creates a flow.
grains=$(ls $domain/$f.mxl-flow/grains | wc -l)
headers=$((grains * 8192 + 65536))
check "its ring of $ring bytes takes $used bytes (grain headers and metadata)" '[ $used -le $headers ]'

# A reader opens it before any data and waits; opening maps every grain.
SECONDS=4 $h reader $domain $f > /tmp/rd.out 2>&1 & rd=$!
sleep 1
used=$(($(alloc) - base))
check "an open reader allocates nothing more ($used bytes)" '[ $used -le $headers ]'

# Mirroring would start now: the agent allocates the ring, folding the
# header pages libmxl wrote into 2 MiB folios under the waiting reader.
USE_IOCTL=1 $h materialize $domain $f
used=$(($(alloc) - base))
check "materializing allocates the ring ($used of $ring bytes)" '[ $used -ge $ring ] && [ $used -lt $((ring + 4194304)) ]'

# A writer adopts the placeholder and writes into the reader's pages.
MEMBERS=1 SECONDS=3 $h writer $domain $f 0 > /tmp/wr.out
cat /tmp/wr.out
wait $rd || true
cat /tmp/rd.out
used=$(($(alloc) - base))
check "the writer adopted the placeholder" 'grep -q "created=0" /tmp/wr.out'
check "the reader read the adopted flow" 'grep -q " ok=[1-9]" /tmp/rd.out && grep -q " wrong_index=0 torn=0" /tmp/rd.out'
check "writing allocated nothing more ($used bytes)" '[ $used -lt $((ring + 4194304)) ]'

# The ring is PMD-mapped where its folios allow it.
MEMBERS=1 SECONDS=2 $h writer $domain $f 0 > /dev/null & wr=$!
sleep 1
pmd=$(awk '/^[0-9a-f]+-[0-9a-f]+ /{f=$6} /^FilePmdMapped/{ if (f ~ /placeholder/) s+=$2 } END { print s+0 }' /proc/$wr/smaps)
wait $wr
check "materialized grains map with PMDs ($pmd kB)" '[ $pmd -ge 40960 ]'

kill $ph; wait $ph || true
check "releasing the last writer removes the flow" '! ls $domain | grep -q "^$f.mxl-flow$"'
check "and returns its memory" '[ $(($(alloc) - base)) -lt 65536 ]'
rm -rf $domain

# Lazy allocation needs no privilege: an unprivileged writer gets a
# placeholder too.
mkdir -m 1777 $domain
echo '{"urn:x-mxl:option:history_duration/v1.0": 400000000}' > $domain/options.json
base=$(alloc)
setpriv --reuid 65534 --regid 65534 --clear-groups $h placeholder $domain $f > /tmp/ph2.out 2>&1 & ph=$!
for i in $(seq 50); do grep -q placeholder /tmp/ph2.out 2>/dev/null && break; sleep 0.1; done
used=$(($(alloc) - base))
check "an unprivileged process creates a placeholder ($used bytes)" 'grep -q "created=1" /tmp/ph2.out && [ $used -le $headers ]'
kill $ph; wait $ph || true
rm -rf $domain
dmesg | grep -E 'BUG:|WARNING:|KASAN|circular locking|Call Trace' && fails=$((fails + 1))
echo "fails=$fails"
[ $fails = 0 ]

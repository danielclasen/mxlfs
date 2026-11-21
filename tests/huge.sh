#!/bin/sh
# The huge= mount option. With the default (within_size) every folio ends
# by EOF; with huge=always a file of at least 2 MiB takes a 2 MiB folio for
# its last partial 2 MiB, past EOF, and smaller files stay as they are.
# Checks the folios behind each file, what they are charged, SIGBUS and
# zeroes past EOF, shrinking, and views over such files, under both, and
# that folding a lazily sized file into 2 MiB folios loses no store made
# through a mapping meanwhile, and that SIGKILL ends a long fallocate.
# Runs as root under dev/vm.sh or dev/host.sh.
set -eu
h=tools/harness
# shellcheck source=tests/lib.sh
. "$(dirname "$0")/lib.sh"

for huge in within_size always; do
  mnt=/tmp/huge-$huge
  other=/tmp/huge-$huge-other
  opts=
  [ $huge = always ] && opts=-ohuge=always
  mkdir -p $mnt $other
  mount -t mxlfs $opts mxlfs $mnt
  mount -t mxlfs $opts mxlfs $other
  HUGE=$huge $h tail $mnt > /tmp/tail.out 2>&1 || true; cat /tmp/tail.out
  check "huge=$huge: folios, charge and EOF of each file and a view" 'grep -q "^tail huge=$huge .* failed=0$" /tmp/tail.out'
  $h shrink $mnt > /tmp/shrink.out 2>&1 || true; cat /tmp/shrink.out
  check "huge=$huge: shrinking keeps what is left and charges it" 'grep -q "failed=0" /tmp/shrink.out'
  $h ranges $mnt 3 100 > /tmp/ranges.out; cat /tmp/ranges.out
  check "huge=$huge: per-range selection over 3 MiB sources" 'grep -q "errors=0 pread_errors=0" /tmp/ranges.out'
  OTHER_DIR=$other $h replace $mnt 3 100 > /tmp/replace.out || true; cat /tmp/replace.out
  check "huge=$huge: replacing a source under readers" 'grep -q "errors=0 loose_errors=0 crashed=0 pread_errors=0 refusals_failed=0" /tmp/replace.out'
  $h fold $mnt 3 100 > /tmp/fold.out || true; cat /tmp/fold.out
  check "huge=$huge: allocating a lazily sized source under view readers" 'grep -q "errors=0 crashed=0" /tmp/fold.out && ! grep -q "folded=0 " /tmp/fold.out'
  if [ $huge = always ]; then
    check "huge=always shows in the mount options" 'grep -q " $mnt mxlfs .*huge=always" /proc/mounts'
  else
    check "the default shows no huge=" '! grep " $mnt mxlfs " /proc/mounts | grep -q huge='
  fi
  umount $mnt $other
done

mkdir -p /tmp/huge-opt
# A store through a mapping while the file is folded into 2 MiB folios
# must land in the large folio, not in a small one about to be dropped.
mount -t mxlfs ${MXLFS_OPTS:+-o $MXLFS_OPTS} mxlfs /tmp/huge-opt
# Each round folds the two PMD spans of a 4 MiB file.
rounds=${FOLDRACE_ROUNDS:-300}
$h foldrace /tmp/huge-opt $rounds > /tmp/foldrace.out 2>&1 || true; cat /tmp/foldrace.out
spans=$(sed -n 's/.*folded_spans=\([0-9]*\).*/\1/p' /tmp/foldrace.out)
check "allocating a lazily sized file under a writer folds it and loses none of its stores" \
  'grep -q "lost=0 crashed=0" /tmp/foldrace.out && [ "${spans:-0}" -ge $((rounds * 2 * 9 / 10)) ]'
umount /tmp/huge-opt
# size= leaves room for a grain's pages but not its tail folio: the tail
# takes folios within EOF instead, and the allocation does not fail.
mount -t mxlfs -o huge=always,size=5600000 mxlfs /tmp/huge-opt
check "a tail folio that size= has no room for falls back to folios within EOF" \
  'fallocate -l 5537792 /tmp/huge-opt/grain && $h stats /tmp/huge-opt/grain | grep -q " allocated=5537792 "'
umount /tmp/huge-opt
# A fatal signal ends fallocate at the next folio. Uninterrupted, the call
# would allocate all of len before the SIGKILL sent after its first folio
# took effect.
mount -t mxlfs mxlfs /tmp/huge-opt
$h allockill /tmp/huge-opt > /tmp/allockill.out 2>&1 || true; cat /tmp/allockill.out
len=$(sed -n 's/.* len=\([0-9]*\) .*/\1/p' /tmp/allockill.out)
left=$(sed -n 's/.* at_exit=\([0-9]*\) .*/\1/p' /tmp/allockill.out)
check "SIGKILL ends fallocate before half the range is allocated" \
  'grep -q " killed=1$" /tmp/allockill.out && [ "${left:-0}" -gt 0 ] && [ $((left * 2)) -lt "${len:-0}" ]'
umount /tmp/huge-opt
check "huge=within_size is accepted" 'mount -t mxlfs -o huge=within_size mxlfs /tmp/huge-opt && umount /tmp/huge-opt'
check "a huge= value mxlfs does not have is refused" '! mount -t mxlfs -o huge=never mxlfs /tmp/huge-opt 2>/dev/null'
check "no report in the kernel log" '! kernel_reports'
echo "fails=$fails"
[ $fails = 0 ]

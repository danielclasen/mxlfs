#!/bin/sh
# Memory errors injected into mxlfs pages with madvise(). A read of a
# hard-poisoned page of a file faults SIGBUS with BUS_MCEERR_AR at its
# address, through the mapping it was injected through and through a
# fresh one, and no other page does, whether the page is a file's only
# one or part of a large folio. MADV_HWPOISON fails with EBUSY, since the
# page stays in the file (MF_FAILED); the page is flagged KPF_HWPOISON
# and counted in HardwareCorrupted, and the file's allocated bytes stay
# as they were. MADV_SOFT_OFFLINE fails with EBUSY and leaves the page
# as it was, unpoisoned. Needs CONFIG_MEMORY_FAILURE. A poisoned page
# frame stays out of use until the machine restarts, so each injection
# takes a fresh file, and the suite runs as root under dev/vm.sh only.
set -eu
h=tools/harness
mnt=/tmp/hwpoison
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
# poison NAME SIZE OFF hard|soft: a fresh file of SIZE, poisoned at OFF.
poison() {
  truncate -s "$2" "$mnt/$1"
  $h poison "$mnt/$1" "$3" "$4" > "/tmp/hwpoison-$1.out"
  cat "/tmp/hwpoison-$1.out"
}
# each NAMES PATTERN: every one of the injections NAMES printed PATTERN.
each() {
  for n in $1; do grep -q -- "$2" "/tmp/hwpoison-$n.out" || return 1; done
}

mkdir -p $mnt
mount -t mxlfs ${MXLFS_OPTS:+-o $MXLFS_OPTS} mxlfs $mnt
poison page 4096 0 hard
if grep -q "madvise=EINVAL" /tmp/hwpoison-page.out; then
  echo "SKIP: MADV_HWPOISON fails with EINVAL, the kernel lacks CONFIG_MEMORY_FAILURE"
  umount $mnt
  echo "fails=1"
  exit 1
fi
poison small 65536 8192 hard
poison large 4M 1048576 hard
poison small-soft 65536 8192 soft
poison large-soft 4M 1048576 soft
check "a read of a hard-poisoned page faults SIGBUS with BUS_MCEERR_AR at its address" \
  'each "page small large" "read=BUS_MCEERR_AR read_addr=1 refault=BUS_MCEERR_AR refault_addr=1 "'
check "no other page of the file faults, and none lost its contents" 'each "page small large" " sigbus_pages=1 wrong=0 "'
check "MADV_HWPOISON fails with EBUSY, as the page stays in the file" 'each "page small large" "madvise=EBUSY "'
check "a hard-poisoned page is flagged KPF_HWPOISON and counted in HardwareCorrupted" \
  'each "page small large" " hwpoison=1 corrupted_kb=4 "'
check "MADV_SOFT_OFFLINE fails with EBUSY" 'each "small-soft large-soft" "madvise=EBUSY "'
check "a soft offline leaves the page as it was, unpoisoned" \
  'each "small-soft large-soft" "read=none read_addr=0 refault=none refault_addr=0 pfn=[^ ]* hwpoison=0 corrupted_kb=0 sigbus_pages=0 wrong=0 "'
check "no injection changes the file's allocated bytes" 'each "page small large small-soft large-soft" " allocated_delta=0$"'
umount $mnt
dmesg | grep -E 'BUG: |WARNING: CPU|KASAN|circular locking|Call Trace|Busy inodes' && fails=$((fails + 1))
echo "fails=$fails"
[ $fails = 0 ]

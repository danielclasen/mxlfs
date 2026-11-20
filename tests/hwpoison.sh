#!/bin/sh
# Memory errors injected into mxlfs pages with madvise(). A read of a
# hard-poisoned page of a file faults SIGBUS with BUS_MCEERR_AR at its
# address, through the mapping it was injected through and through a
# fresh one, and no other page does, whether the page is a file's only
# one or part of a large folio. The injection itself sends that SIGBUS
# for a file's only page and none for a page of a large folio.
# MADV_HWPOISON fails with EBUSY, since the page stays in the file
# (MF_FAILED). That is deliberate: the error persists, as in shmem, and
# memory_failure() discounts the page cache reference for shmem_mapping()
# only, which no module's mapping is. The page is flagged KPF_HWPOISON
# and counted in HardwareCorrupted, and the file's allocated bytes stay
# as they were. pread(), splice() and pwrite() fail with EIO at the
# page, and a read starting before it stops short at it, until the file
# is recreated or truncated below the page. MADV_SOFT_OFFLINE fails
# with EBUSY and leaves the page as it was, unpoisoned. Needs CONFIG_MEMORY_FAILURE. A poisoned page
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
  echo "FAIL: MADV_HWPOISON fails with EINVAL, the kernel lacks CONFIG_MEMORY_FAILURE"
  umount $mnt
  echo "fails=1"
  exit 1
fi
poison small 65536 8192 hard
poison large 4M 1048576 hard
poison small-soft 65536 8192 soft
poison large-soft 4M 1048576 soft
check "the 64K and 4M files' poisoned pages are in large folios, the 4K file's is not" \
  'each "small large" " large=1 " && each page " large=0 "'
check "MADV_HWPOISON sends SIGBUS BUS_MCEERR_AR at a file's only page, none in a large folio" \
  'each page "madvise=EBUSY signal=BUS_MCEERR_AR signal_addr=1 " && each "small large" "madvise=EBUSY signal=none signal_addr=0 "'
check "a read of a hard-poisoned page faults SIGBUS with BUS_MCEERR_AR at its address" \
  'each "page small large" "read=BUS_MCEERR_AR read_addr=1 refault=BUS_MCEERR_AR refault_addr=1 "'
check "no other page of the file faults, and none lost its contents" 'each "page small large" " sigbus_pages=1 wrong=0 "'
check "MADV_HWPOISON fails with EBUSY, as the page stays in the file" 'each "page small large" "madvise=EBUSY "'
check "a hard-poisoned page is flagged KPF_HWPOISON and counted in HardwareCorrupted" \
  'each "page small large" " hwpoison=1 corrupted_kb=4 "'
check "MADV_SOFT_OFFLINE fails with EBUSY" 'each "small-soft large-soft" "madvise=EBUSY "'
check "a soft offline leaves the page as it was, unpoisoned" \
  'each "small-soft large-soft" "signal=none signal_addr=0 read=none read_addr=0 refault=none refault_addr=0 pfn=0x[0-9a-f]* large=1 hwpoison=0 corrupted_kb=0 sigbus_pages=0 wrong=0 "'
check "no injection changes the file's allocated bytes" 'each "page small large small-soft large-soft" " allocated_delta=0 "'
check "pread() of a hard-poisoned page fails with EIO, one from the page before stops at it" \
  'each "page small large" " pread=EIO " && each "small large" " pread_before=4096 "'
check "splice() of a hard-poisoned page fails with EIO, one from the page before stops at it" \
  'each "page small large" " splice=EIO " && each "small large" " splice_before=4096 "'
check "pwrite() into a hard-poisoned page fails with EIO, one into the page before does not" \
  'each "page small large" " pwrite=EIO " && each "small large" " pwrite_before=8 "'
check "a soft-offlined page reads, splices and writes in full" \
  'each "small-soft large-soft" " pread=4096 pread_before=8192 splice=4096 splice_before=8192 pwrite=8 pwrite_before=8 "'
# Guards: these pass without the EIO refusals as well.
check "guard: an unaligned pread() ending where a hard-poisoned page starts returns in full" \
  'each "small large small-soft large-soft" " pread_unaligned=4000$"'
rm "$mnt/small"
truncate -s 65536 "$mnt/small"
got=$(dd if="$mnt/small" bs=65536 count=1 status=none | wc -c)
check "guard: a file recreated in place of a poisoned one reads in full" "[ $got -eq 65536 ]"
truncate -s 1044480 "$mnt/large"
truncate -s 4M "$mnt/large"
got=$(dd if="$mnt/large" bs=4M count=1 status=none | wc -c)
check "guard: a file truncated below its poisoned page and grown again reads in full" "[ $got -eq 4194304 ]"
umount $mnt
dmesg | grep -E 'BUG: |WARNING: CPU|KASAN|circular locking|Call Trace|Busy inodes' && fails=$((fails + 1))
echo "fails=$fails"
[ $fails = 0 ]

#!/bin/sh
# The module as a distribution installs it, in a guest booted on that
# distribution's kernel: modprobe finds it, a mount takes the options, a
# file allocates and reads back what was written, huge= and numa_node=
# are honoured, unmount and unload leave no report in the kernel log, and
# a mount alone loads the module again.
# Needs no libmxl. Runs as root.
set -eu
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }

kver=$(uname -r)
dmesg -C
check "modprobe loads mxlfs on $kver" 'modprobe mxlfs'
check "the module is the one DKMS installed" 'modinfo -n mxlfs | grep -q "/lib/modules/$kver/updates/dkms/mxlfs.ko"'
check "the kernel lists the filesystem" 'grep -qw mxlfs /proc/filesystems'

mnt=/tmp/installed
mkdir -p $mnt
check "mount with size, mode, huge and numa_node" \
  'mount -t mxlfs -o size=64M,mode=1777,huge=always,numa_node=0 mxlfs $mnt'
check "the options show in /proc/mounts" \
  'grep " $mnt mxlfs " /proc/mounts | grep -q "huge=always" && grep " $mnt mxlfs " /proc/mounts | grep -q "numa_node=0"'
check "fallocate within size= succeeds" 'fallocate -l 8M $mnt/a'
check "fallocate past size= fails" '! fallocate -l 128M $mnt/b 2>/dev/null'
rm -f $mnt/b
check "written data reads back" \
  'head -c 4M /dev/urandom > /tmp/pattern && dd if=/tmp/pattern of=$mnt/a conv=notrunc status=none && cmp -n 4194304 /tmp/pattern $mnt/a'
check "statfs reports the size= cap" '[ $(($(stat -f -c %b $mnt) * $(stat -f -c %S $mnt))) -eq 67108864 ]'
check "rename and unlink" 'mv $mnt/a $mnt/c && rm $mnt/c && [ -z "$(ls -A $mnt)" ]'
check "mkdir, symlink, link and rmdir" \
  'mkdir $mnt/d && touch $mnt/d/f && ln $mnt/d/f $mnt/d/g && ln -s f $mnt/d/s && rm $mnt/d/f $mnt/d/g $mnt/d/s && rmdir $mnt/d'
check "unmount" 'umount $mnt'
check "unload" 'rmmod mxlfs'
check "a mount loads the module without modprobe" \
  'mount -t mxlfs mxlfs $mnt && grep -qw mxlfs /proc/filesystems && umount $mnt && rmmod mxlfs'
check "no warning, bug or oops in the kernel log" '! dmesg | grep -E "WARNING:|BUG:|Oops|refcount_t|general protection"'

echo "installed: kernel=$kver failed=$fails"
[ $fails -eq 0 ]

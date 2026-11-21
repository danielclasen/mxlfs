#!/bin/sh
# Every way a name is made or removed: mkdir, mknod, create, symlink,
# hard link, O_TMPFILE linked in with linkat, rename over an existing
# file and over an empty directory, RENAME_EXCHANGE of two files and of
# a file with a directory across directories, RENAME_NOREPLACE onto a
# free name, unlink and rmdir. Each kind is unmounted while still
# present, then made and removed again on a fresh mount and unmounted
# empty. Memory filesystems pin their dentries, by the filesystem before
# 6.19 and by d_make_persistent() from 6.19 on; a pin taken twice or not
# at all shows as "Dentry ... still in use", "Busy inodes" or a warning
# in the kernel log. Runs as root under dev/vm.sh or dev/host.sh.
set -eu
mnt=/tmp/dentries
fails=0
trap 'umount $mnt 2>/dev/null || true' EXIT
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }

populate() {
  mkdir $mnt/d
  mkfifo $mnt/d/fifo
  mknod $mnt/d/chr c 1 3
  echo a > $mnt/d/a
  echo b > $mnt/d/b
  ln $mnt/d/a $mnt/d/a-link
  ln -s a $mnt/d/a-sym
  # An O_TMPFILE file has no name until linkat gives it one, through
  # /proc/self/fd with AT_SYMLINK_FOLLOW; dst_dir_fd makes os.link() use
  # linkat. The second file is closed without a name.
  python3 -c '
import os, sys
d = os.open(sys.argv[1], os.O_RDONLY | os.O_DIRECTORY)
fd = os.open(sys.argv[1], os.O_TMPFILE | os.O_RDWR, 0o600)
os.write(fd, b"tmp\n")
os.link("/proc/self/fd/%d" % fd, "tmp", dst_dir_fd=d, follow_symlinks=True)
os.close(fd)
os.close(os.open(sys.argv[1], os.O_TMPFILE | os.O_RDWR, 0o600))' $mnt/d
  echo over > $mnt/d/over
  mv -T $mnt/d/over $mnt/d/b
  echo x > $mnt/d/x
  echo y > $mnt/d/y
  mv --exchange $mnt/d/x $mnt/d/y
  mkdir $mnt/d/e1 $mnt/d/e2
  mv -T $mnt/d/e1 $mnt/d/e2
  mkdir $mnt/e $mnt/e/sub
  echo f > $mnt/d/f
  mv -T --exchange $mnt/d/f $mnt/e/sub
  # RENAME_NOREPLACE onto a taken name is refused by the VFS before the
  # filesystem sees it; onto a free one it reaches mxlfs with the flag.
  echo n > $mnt/d/n
  python3 -c '
import ctypes, errno, sys
c = ctypes.CDLL(None, use_errno=True)
AT_FDCWD, RENAME_NOREPLACE = -100, 1
r = c.renameat2(AT_FDCWD, sys.argv[1].encode(), AT_FDCWD, sys.argv[2].encode(), RENAME_NOREPLACE)
assert r == -1 and ctypes.get_errno() == errno.EEXIST
r = c.renameat2(AT_FDCWD, sys.argv[1].encode(), AT_FDCWD, sys.argv[3].encode(), RENAME_NOREPLACE)
assert r == 0, errno.errorcode[ctypes.get_errno()]' $mnt/d/n $mnt/d/y $mnt/d/m
}

mkdir -p $mnt
mount -t mxlfs ${MXLFS_OPTS:+-o $MXLFS_OPTS} mxlfs $mnt
populate
check "hard link shares the inode" '[ "$(stat -c %i $mnt/d/a)" = "$(stat -c %i $mnt/d/a-link)" ] && [ "$(stat -c %h $mnt/d/a)" = 2 ]'
check "symlink resolves" '[ "$(cat $mnt/d/a-sym)" = a ]'
check "O_TMPFILE linked in keeps its data" '[ "$(cat $mnt/d/tmp)" = tmp ]'
check "rename over a name replaces it" '[ "$(cat $mnt/d/b)" = over ] && [ ! -e $mnt/d/over ]'
check "RENAME_EXCHANGE swaps two names" '[ "$(cat $mnt/d/x)" = y ] && [ "$(cat $mnt/d/y)" = x ]'
check "the fifo is a fifo" '[ -p $mnt/d/fifo ]'
check "a device node keeps its device number" '[ -c $mnt/d/chr ] && [ "$(stat -c %t:%T $mnt/d/chr)" = 1:3 ]'
check "rename over an empty directory replaces it" \
  '[ -d $mnt/d/e2 ] && [ ! -e $mnt/d/e1 ] && [ "$(stat -c %h $mnt/d/e2)" = 2 ]'
check "exchanging a file and a directory moves both, with link counts" \
  '[ -d $mnt/d/f ] && [ "$(cat $mnt/e/sub)" = f ] && [ "$(stat -c %h $mnt/d)" = 4 ] && [ "$(stat -c %h $mnt/e)" = 2 ] &&
   [ "$(stat -c %h $mnt/d/f)" = 2 ]'
check "RENAME_NOREPLACE moves onto a free name only" '[ "$(cat $mnt/d/m)" = n ] && [ ! -e $mnt/d/n ] && [ "$(cat $mnt/d/y)" = x ]'
check "the root counts its subdirectories" '[ "$(stat -c %h $mnt)" = 4 ]'
check "unmount with every kind of name present" 'umount $mnt'

mount -t mxlfs ${MXLFS_OPTS:+-o $MXLFS_OPTS} mxlfs $mnt
populate
check "unlink and rmdir remove every name" \
  'rm $mnt/d/fifo $mnt/d/chr $mnt/d/a $mnt/d/a-link $mnt/d/a-sym $mnt/d/b $mnt/d/tmp $mnt/d/x $mnt/d/y $mnt/d/m $mnt/e/sub &&
   rmdir $mnt/d/e2 $mnt/d/f $mnt/d $mnt/e && [ -z "$(ls -A $mnt)" ] && [ "$(stat -c %h $mnt)" = 2 ]'
check "statfs counts nothing allocated once all is removed" '[ "$(stat -f -c %f $mnt)" = "$(stat -f -c %b $mnt)" ]'
check "unmount empty" 'umount $mnt'
check "no dentry, inode or refcount report in the kernel log" \
  'log=$(dmesg) && ! printf "%s\n" "$log" | grep -E "still in use|Busy inodes|WARNING:|BUG:|refcount_t"'

echo "dentries: failed=$fails"
[ $fails -eq 0 ]

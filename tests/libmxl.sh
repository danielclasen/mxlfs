#!/bin/sh
# libmxl's own test suite, whose domains live under /dev/shm. Runs as root
# in the VM (dev/vm.sh), where /dev/shm is an mxlfs. MXL_TESTS points at a
# build of the suite (dev/libmxl.sh).
#
# The suite runs as nobody: its permission tests expect an unreadable
# flow to fail to open, which root's DAC override defeats on any
# filesystem. With no arguments it leaves out the one test that requires
# /dev/shm to be a tmpfs, which mxlfs by design is not (README.md).
#
# The release-walk tests then run with /dev/shm mounted again with wait=,
# where a walk to a deleted flow's name blocks for the wait.
set -eu
tests=${MXL_TESTS:-build/mxl-src/build/Linux-Clang-Release/lib/tests/mxl-tests}
cd "$(dirname "$tests")"
[ "$#" -gt 0 ] || set -- '~mxlIsTmpFs detects /dev/shm as tmpfs on Linux'
nobody() { setpriv --reuid=65534 --regid=65534 --clear-groups "$@"; }
nobody ./mxl-tests "$@"
umount /dev/shm
mount -t mxlfs -o "size=${MXLFS_SIZE:-2G},mode=1777,wait=${WAIT_MS:-2000}${MXLFS_OPTS:+,$MXLFS_OPTS}" mxlfs /dev/shm
nobody ./mxl-release-walk-tests

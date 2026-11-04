#!/bin/sh
# libmxl's own test suite, whose domains live under /dev/shm. Runs in the
# VM (dev/vm.sh), where /dev/shm is an mxlfs. MXL_TESTS points at a build
# of the suite (dev/libmxl.sh).
#
# The suite runs as nobody: its permission tests expect an unreadable
# flow to fail to open, which root's DAC override defeats on any
# filesystem. With no arguments it leaves out the one test that requires
# /dev/shm to be a tmpfs, which mxlfs by design is not (README.md).
set -eu
tests=${MXL_TESTS:-build/mxl-src/build/Linux-Clang-Release/lib/tests/mxl-tests}
cd "$(dirname "$tests")"
[ "$#" -gt 0 ] || set -- '~mxlIsTmpFs detects /dev/shm as tmpfs on Linux'
exec setpriv --reuid=65534 --regid=65534 --clear-groups ./mxl-tests "$@"

#!/bin/sh
# libmxl's own test suite, whose domains live under /dev/shm. Runs as root
# in the VM (dev/vm.sh), where /dev/shm is an mxlfs. MXL_TESTS points at a
# build of the suite (dev/libmxl.sh).
set -eu
tests=${MXL_TESTS:-build/mxl-src/build/Linux-Clang-Release/lib/tests/mxl-tests}
cd "$(dirname "$tests")"
exec ./mxl-tests "$@"

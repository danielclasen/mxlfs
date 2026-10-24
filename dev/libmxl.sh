#!/bin/sh
# Build libmxl's test suite from the qvest-digital/mxl fork at the commit
# go-mxl pins, as root in the dev image (vcpkg lives under /root there).
# Usage: dev/libmxl.sh <path to a checkout of the fork>
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
src=$1
commit=${MXL_COMMIT:-c259243931333497e61faabeedac8048915dcb2b}
rm -rf "$here/build/mxl-src" && mkdir -p "$here/build/mxl-src"
git -C "$src" archive "$commit" | tar -x -C "$here/build/mxl-src"
docker build -q -t mxlfs-dev "$here/dev" >/dev/null
docker run --rm -v "$here:/src" -w /src/build/mxl-src mxlfs-dev sh -c "
  cmake --preset Linux-Clang-Release -DBUILD_DOCS=OFF -DBUILD_TOOLS=OFF -DMXL_ENABLE_FABRICS_OFI=ON &&
  cmake --build build/Linux-Clang-Release -j\$(nproc); rc=\$?
  chown -R $(id -u):$(id -g) /src/build/mxl-src; exit \$rc"

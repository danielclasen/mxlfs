#!/bin/sh
# Run a command in the development image with the repository at /src and
# KVM passed through. Kernel sources and build output live in build/.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
image=mxlfs-dev
docker build -q -t "$image" "$here/dev" >/dev/null
exec docker run --rm -i --init $( [ -t 0 ] && echo -t ) \
  --device /dev/kvm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$here:/src" -w /src \
  "$image" "$@"

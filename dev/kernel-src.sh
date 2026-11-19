#!/bin/sh
# Unpack the source of kernel <version> into <dir>/linux-<version>: a
# release (6.18.42, 7.3) from cdn.kernel.org, a mainline release candidate
# (7.4-rc3) from the git.kernel.org snapshot of its tag.
set -eu
version=$1
dir=$2
[ -d "$dir/linux-$version" ] && exit 0
mkdir -p "$dir"
case "$version" in
*-rc*)
  curl -fsSL "https://git.kernel.org/torvalds/t/linux-$version.tar.gz" | tar -xz -C "$dir"
  ;;
*)
  curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v${version%%.*}.x/linux-$version.tar.xz" |
    tar -xJ -C "$dir"
  ;;
esac

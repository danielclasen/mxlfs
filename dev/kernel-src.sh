#!/bin/sh
# Unpack the source of kernel <version> into <dir>/linux-<version>: a
# release (6.18.58, 7.3) from cdn.kernel.org, checked against the
# sha256sums.asc kernel.org publishes beside it, or a mainline release
# candidate (7.4-rc3) from the git.kernel.org snapshot of its tag, for
# which kernel.org publishes no checksum. The tree appears only once it
# is complete, so an interrupted run leaves nothing behind that a later
# one would take for a finished tree.
set -eu
version=$1
dir=$2
[ -d "$dir/linux-$version" ] && exit 0
mkdir -p "$dir"
work=$(mktemp -d "$dir/.linux-$version.XXXXXX")
trap 'rm -rf "$work"' EXIT
case "$version" in
*-rc*)
  curl -fsSL "https://git.kernel.org/torvalds/t/linux-$version.tar.gz" | tar -xz -C "$work"
  ;;
*)
  base="https://cdn.kernel.org/pub/linux/kernel/v${version%%.*}.x"
  curl -fsSL -o "$work/linux.tar.xz" "$base/linux-$version.tar.xz"
  sum=$(curl -fsSL "$base/sha256sums.asc" | awk -v f="linux-$version.tar.xz" '$2 == f { print $1 }')
  [ -n "$sum" ] || { echo "no checksum for linux-$version.tar.xz" >&2; exit 1; }
  echo "$sum  $work/linux.tar.xz" | sha256sum -c - >/dev/null
  tar -xJf "$work/linux.tar.xz" -C "$work"
  rm "$work/linux.tar.xz"
  ;;
esac
mv -T "$work/linux-$version" "$dir/linux-$version"

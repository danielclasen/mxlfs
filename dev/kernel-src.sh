#!/bin/sh
# Unpack the source of kernel <version> into <dir>/linux-<version>: a
# release (6.18.58, 7.3) from cdn.kernel.org, checked against the
# sha256sums.asc kernel.org publishes beside it, or a mainline release
# candidate (7.4-rc3) from the git.kernel.org snapshot of its tag, for
# which kernel.org publishes no checksum. git.kernel.org generates the
# snapshot as it streams it, so a cut connection leaves a truncated
# archive, and a stalled one would hang: each download goes to a file,
# is cut when it moves less than 1000 bytes/s for 2 minutes and is tried
# up to 5 times, and counts as downloaded only once its check passes,
# gzip reading the snapshot to its end or a release matching its checksum.
# The tree appears only once it is complete, so an interrupted run leaves
# nothing behind that a later one would take for a finished tree.
set -eu
version=$1
dir=$2
[ -d "$dir/linux-$version" ] && exit 0
mkdir -p "$dir"
work=$(mktemp -d "$dir/.linux-$version.XXXXXX")
trap 'rm -rf "$work"' EXIT
# INT and TERM exit through the EXIT trap, so no partial download is left.
trap 'exit 130' INT
trap 'exit 143' TERM

# fetch URL FILE [CHECK...]: download URL to FILE, and run CHECK on it if
# given, up to 5 times.
fetch() {
  url=$1 file=$2
  shift 2
  for try in 1 2 3 4 5; do
    if curl -fsSL --speed-limit 1000 --speed-time 120 --retry 3 --retry-delay 10 -o "$file" "$url" &&
       { [ $# = 0 ] || "$@" "$file"; }; then
      return 0
    fi
    rm -f "$file"
    echo "download of $url failed (try $try of 5)" >&2
    [ "$try" = 5 ] || sleep 30
  done
  return 1
}

case "$version" in
*-rc*)
  fetch "https://git.kernel.org/torvalds/t/linux-$version.tar.gz" "$work/linux.tar.gz" gzip -t
  tar -xzf "$work/linux.tar.gz" -C "$work"
  rm "$work/linux.tar.gz"
  ;;
*)
  base="https://cdn.kernel.org/pub/linux/kernel/v${version%%.*}.x"
  fetch "$base/sha256sums.asc" "$work/sha256sums.asc"
  sum=$(awk -v f="linux-$version.tar.xz" '$2 == f { print $1 }' "$work/sha256sums.asc")
  [ -n "$sum" ] || { echo "no checksum for linux-$version.tar.xz" >&2; exit 1; }
  matches() { echo "$sum  $1" | sha256sum -c - >/dev/null; }
  fetch "$base/linux-$version.tar.xz" "$work/linux.tar.xz" matches
  tar -xJf "$work/linux.tar.xz" -C "$work"
  rm "$work/linux.tar.xz" "$work/sha256sums.asc"
  ;;
esac
mv -T "$work/linux-$version" "$dir/linux-$version"

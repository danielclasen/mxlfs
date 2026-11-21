#!/bin/sh
# Build and install sparse into /usr/local. Kernels from 7.0 on use
# __typeof_unqual__, which sparse understands from 0.6.5-rc1; with an
# older one kbuild skips the check (C=1) and says so.
set -eu
version=0.6.5-rc1
sha256=3ac8855ec7f3ab92f325be0f64347ccc2b10f8b4d7c2906783ee8e2bdab3f0de
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
url=https://git.kernel.org/pub/scm/devel/sparse/sparse.git/snapshot/sparse-$version.tar.gz
# git.kernel.org generates a snapshot while it streams it, and curl's
# --retry does not repeat a cut connection, so a download whose checksum
# fails is fetched again, up to 5 times, as dev/kernel-src.sh does.
for try in 1 2 3 4 5; do
  if curl -fsSL --speed-limit 1000 --speed-time 120 --retry 3 --retry-delay 10 \
       -o "$work/sparse.tar.gz" "$url" &&
     echo "$sha256  $work/sparse.tar.gz" | sha256sum -c - >/dev/null; then
    break
  fi
  echo "download of $url failed (try $try of 5)" >&2
  [ "$try" = 5 ] && exit 1
  sleep 30
done
tar -xzf "$work/sparse.tar.gz" -C "$work"
make -C "$work/sparse-$version" -j"$(nproc)" PREFIX=/usr/local HAVE_LLVM=no HAVE_GTK=no HAVE_LIBXML=no install >/dev/null
sparse --version

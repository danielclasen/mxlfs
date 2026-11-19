#!/bin/sh
# Build and install sparse into /usr/local. Kernels from 7.0 on use
# __typeof_unqual__, which sparse understands from 0.6.5-rc1; with an
# older one kbuild skips the check (C=1) and says so.
set -eu
version=0.6.5-rc1
sha256=3ac8855ec7f3ab92f325be0f64347ccc2b10f8b4d7c2906783ee8e2bdab3f0de
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsSL -o "$work/sparse.tar.gz" \
  "https://git.kernel.org/pub/scm/devel/sparse/sparse.git/snapshot/sparse-$version.tar.gz"
echo "$sha256  $work/sparse.tar.gz" | sha256sum -c - >/dev/null
tar -xzf "$work/sparse.tar.gz" -C "$work"
make -C "$work/sparse-$version" -j"$(nproc)" PREFIX=/usr/local HAVE_LLVM=no HAVE_GTK=no HAVE_LIBXML=no install >/dev/null
sparse --version

#!/bin/sh
# Build and install sparse into /usr/local. Kernels from 7.0 on use
# __typeof_unqual__, which sparse understands from 0.6.5-rc1; with an
# older one kbuild skips the check (C=1) and says so. That release still
# reports itself as 0.6.4.
set -eu
version=0.6.5-rc1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsSL "https://git.kernel.org/pub/scm/devel/sparse/sparse.git/snapshot/sparse-$version.tar.gz" |
  tar -xz -C "$work"
make -C "$work/sparse-$version" -j"$(nproc)" PREFIX=/usr/local HAVE_LLVM=no HAVE_GTK=no HAVE_LIBXML=no install >/dev/null
sparse --version

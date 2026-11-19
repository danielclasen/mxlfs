#!/bin/sh
# checkpatch --strict (.checkpatch.conf) over the module, with the
# exceptions each file needs. The checkpatch is the one of the kernel
# commit below (v7.3), bumped deliberately so a new check does not fail a
# change that did not touch the code it flags.
set -eu
commit=c03c1a14507dfaf8b856c2608f9ac216021fc222
cd "$(dirname "$0")/.."
cp=${CHECKPATCH_DIR:-build/checkpatch-$commit}
tmp=
trap 'rm -rf "$tmp"' EXIT
if [ ! -f "$cp/checkpatch.pl" ]; then
  if [ -e "$cp" ]; then
    echo "$cp exists without checkpatch.pl; remove it to fetch again" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$cp")"
  tmp=$(mktemp -d "$cp.XXXXXX")
  for f in checkpatch.pl spelling.txt const_structs.checkpatch; do
    curl -fsSL "https://raw.githubusercontent.com/torvalds/linux/$commit/scripts/$f" -o "$tmp/$f"
  done
  mv -T "$tmp" "$cp"
  tmp=
fi
run() { perl "$cp/checkpatch.pl" --no-tree --terse "$@"; }
# Opt_* is the fs_parser naming every in-tree filesystem uses.
run --ignore CAMELCASE -f kernel/mxlfs.c
# BIT() is not available to uapi headers.
run --ignore BIT_MACRO -f kernel/mxlfs_uapi.h
# Selecting by kernel version is what compat.h is for.
run --ignore LINUX_VERSION_CODE,CONSTANT_COMPARISON -f kernel/compat.h

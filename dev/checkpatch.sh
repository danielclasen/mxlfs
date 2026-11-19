#!/bin/sh
# checkpatch --strict (.checkpatch.conf) over the module, with the
# exceptions each file needs. The checkpatch is the one of the kernel
# release below, bumped deliberately so a new check does not fail a
# change that did not touch the code it flags.
set -eu
tag=v7.3
cd "$(dirname "$0")/.."
cp=${CHECKPATCH_DIR:-build/checkpatch-$tag}
if [ ! -f "$cp/checkpatch.pl" ]; then
  mkdir -p "$cp"
  for f in checkpatch.pl spelling.txt const_structs.checkpatch; do
    curl -fsSL "https://raw.githubusercontent.com/torvalds/linux/$tag/scripts/$f" -o "$cp/$f"
  done
fi
run() { perl "$cp/checkpatch.pl" --no-tree --terse "$@"; }
# Opt_* is the fs_parser naming every in-tree filesystem uses.
run --ignore CAMELCASE -f kernel/mxlfs.c
# BIT() is not available to uapi headers.
run --ignore BIT_MACRO -f kernel/mxlfs_uapi.h
# Selecting by kernel version is what compat.h is for.
run --ignore LINUX_VERSION_CODE,CONSTANT_COMPARISON -f kernel/compat.h

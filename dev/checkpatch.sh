#!/bin/sh
# checkpatch --strict (.checkpatch.conf) over the module, with the
# exceptions each file needs. The checkpatch is the one of the kernel
# commit below (v7.3), bumped deliberately so a new check does not fail a
# change that did not touch the code it flags. Its files come through the
# GitHub API at that commit, authenticated with GH_TOKEN when it is set,
# as the workflow sets it, and retried on 429 and 5xx with curl's backoff.
set -eu
commit=c03c1a14507dfaf8b856c2608f9ac216021fc222
cd "$(dirname "$0")/.."
cp=${CHECKPATCH_DIR:-build/checkpatch-$commit}
# A trailing slash would put the download directory inside $cp.
while [ "$cp" != "${cp%/}" ] && [ -n "${cp%/}" ]; do cp=${cp%/}; done
tmp=
trap 'rm -rf "$tmp"' EXIT
# INT and TERM exit through the EXIT trap, so no partial download is left.
trap 'exit 130' INT
trap 'exit 143' TERM
if [ ! -f "$cp/checkpatch.pl" ]; then
  if [ -e "$cp" ]; then
    echo "$cp exists without checkpatch.pl; remove it to fetch again" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$cp")"
  tmp=$(mktemp -d "$cp.XXXXXX")
  for f in checkpatch.pl spelling.txt const_structs.checkpatch; do
    # The token goes in on stdin, never on the command line.
    printf '%s\n' "${GH_TOKEN:+Authorization: Bearer $GH_TOKEN}" |
      curl -fsSL --retry 5 --retry-max-time 300 -H @- \
        -H 'Accept: application/vnd.github.raw' -H 'X-GitHub-Api-Version: 2022-11-28' \
        -o "$tmp/$f" "https://api.github.com/repos/torvalds/linux/contents/scripts/$f?ref=$commit"
  done
  mv -T "$tmp" "$cp"
  tmp=
fi
run() { perl "$cp/checkpatch.pl" --no-tree --terse "$@"; }
# Opt_* is the fs_parser naming every in-tree filesystem uses.
for f in kernel/*.c kernel/mxlfs.h; do
  # kbuild writes mxlfs.mod.c into kernel/ when it builds there.
  case $f in *.mod.c) continue ;; esac
  run --ignore CAMELCASE -f "$f"
done
# BIT() is not available to uapi headers.
run --ignore BIT_MACRO -f kernel/mxlfs_uapi.h
# Selecting by kernel version is what compat.h is for.
run --ignore LINUX_VERSION_CODE,CONSTANT_COMPARISON -f kernel/compat.h

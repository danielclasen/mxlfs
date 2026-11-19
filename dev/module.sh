#!/bin/sh
# Build the module against the kernel tree or headers at KDIR with W=1 and,
# unless SPARSE=0, sparse; fail on any compiler, sparse or modpost warning
# or error, or when kbuild skipped sparse. The two messages ignored are the
# notes that the compiler or pahole differs from the kernel's, which
# distribution headers print. Distribution header packages leave out
# scripts/checker-valid.sh, without which kbuild never runs sparse, so
# they are built with SPARSE=0.
set -eu
kdir=$1
check=C=1
[ "${SPARSE:-1}" = 0 ] && check=C=0
cd "$(dirname "$0")/../kernel"
log=$(mktemp)
trap 'rm -f "$log"' EXIT
make KDIR="$kdir" clean >/dev/null
status=0
make KDIR="$kdir" W=1 "$check" >"$log" 2>&1 || status=$?
cat "$log"
[ "$status" -eq 0 ] || exit "$status"
if grep -Ei 'warning|error' "$log" | grep -Ev 'compiler differs from the one used|pahole version differs'; then
  exit 1
fi
if grep -E 'C=[12] specified, but' "$log"; then
  echo "sparse did not run" >&2
  exit 1
fi
modinfo -F vermagic mxlfs.ko

#!/bin/sh
# Make the development image (dev) or the kernel tree for KERNEL_VERSION
# and FLAVOR (kernel) available, from the registry when it has them, and
# push what had to be built when PUSH=true. A registry that cannot be
# asked fails the step rather than starting a build; a failed push only
# warns, since what was built is in place.
set -eu
cd "$(dirname "$0")/.."
case "$1" in
dev)
  ref=$(./dev/dev-image.sh)
  if err=$(docker manifest inspect "$ref" 2>&1 >/dev/null); then
    docker pull -q "$ref" >/dev/null
    docker tag "$ref" mxlfs-dev
    echo "pulled $ref"
    exit 0
  fi
  case "$err" in
  *"manifest unknown"* | *"no such manifest"*) echo "no $ref" ;;
  *)
    echo "cannot look up $ref: $err" >&2
    exit 2
    ;;
  esac
  ./dev/run.sh true
  if [ "${PUSH:-}" = true ]; then
    docker tag mxlfs-dev "$ref"
    if docker push -q "$ref" >/dev/null; then
      echo "pushed $ref"
    else
      echo "::warning::could not push $ref"
    fi
  fi
  ;;
kernel)
  status=0
  ./dev/kernel-cache.sh pull || status=$?
  case $status in
  0) exit 0 ;;
  1) ;;
  *) exit "$status" ;;
  esac
  ./dev/run.sh env KERNEL_VERSION="$KERNEL_VERSION" FLAVOR="${FLAVOR:-debug}" ./dev/kernel.sh
  if [ "${PUSH:-}" = true ]; then
    ./dev/kernel-cache.sh push || echo "::warning::could not push the $KERNEL_VERSION ${FLAVOR:-debug} kernel tree"
  fi
  ;;
*)
  echo "usage: $0 dev|kernel" >&2
  exit 2
  ;;
esac

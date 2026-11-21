#!/bin/sh
# Run a command in the development image with the repository at /src and
# KVM passed through, with /dev/kvm's group. Kernel sources and build output
# live in build/, which may be a symlink to a build/ shared between worktrees.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
image=mxlfs-dev
# The image is labelled with a hash of what it is built from; one with
# another hash is rebuilt, or pulled from DEV_CACHE when CI pushed it there.
cache=$("$here/dev/dev-image.sh")
hash=${cache##*:}
if [ "$(docker image inspect -f '{{index .Config.Labels "mxlfs.dev"}}' "$image" 2>/dev/null)" != "$hash" ]; then
  if docker pull -q "$cache" >/dev/null 2>&1; then
    docker tag "$cache" "$image"
  else
    docker build -q --label mxlfs.dev="$hash" \
      --label org.opencontainers.image.source=https://github.com/danielclasen/mxlfs \
      -t "$image" "$here/dev" >/dev/null
  fi
fi
build_mount=
if [ -L "$here/build" ]; then build_mount="$(readlink -f "$here/build"):/src/build"; fi
exec docker run --rm -i --init $( [ -t 0 ] && echo -t ) \
  --device /dev/kvm --group-add "$(stat -c %g /dev/kvm)" --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$here:/src" -w /src \
  ${build_mount:+-v "$build_mount"} \
  "$image" "$@"

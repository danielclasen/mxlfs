#!/bin/sh
# Share built kernel trees through a registry: `pull` puts the tree
# dev/kernel.sh would build for KERNEL_VERSION and FLAVOR into build/ if
# the registry has it, and exits 1 if the registry does not have it or 2 if
# it could not be fetched; `push` stores the tree built there. The tag
# carries what decides the tree: the version, the flavor, dev/kernel.sh and
# dev/kernel-src.sh, which fetch and build it, and the build image,
# dev/Dockerfile. A release candidate's source is the git.kernel.org
# snapshot of its tag, which has no checksum to check it against.
# KERNEL_CACHE names the repository, ghcr.io/danielclasen/mxlfs-ci-kernel
# by default.
set -eu
cd "$(dirname "$0")/.."
action=${1-}
version=${KERNEL_VERSION:-$(cat dev/talos-kernel)}
flavor=${FLAVOR:-debug}
tree=build/linux-$version
[ "$flavor" = debug ] || tree=$tree-$flavor
hash=$(cat dev/kernel.sh dev/kernel-src.sh dev/Dockerfile | sha256sum | cut -c1-12)
image=${KERNEL_CACHE:-ghcr.io/danielclasen/mxlfs-ci-kernel}:$version-$flavor-$hash
case "$action" in
pull)
  [ -d "$tree" ] && exit 0
  if ! err=$(docker manifest inspect "$image" 2>&1 >/dev/null); then
    case "$err" in
    *"manifest unknown"* | *"no such manifest"*)
      echo "no $image"
      exit 1
      ;;
    esac
    echo "cannot look up $image: $err" >&2
    exit 2
  fi
  # Past here, a failure is the registry's or the host's, not a miss.
  docker pull -q "$image" >/dev/null || exit 2
  mkdir -p build || exit 2
  rm -rf "$tree.part" || exit 2
  id=$(docker create "$image" /none) || exit 2
  trap 'docker rm "$id" >/dev/null' EXIT
  docker cp "$id:/tree/." "$tree.part" || exit 2
  mv "$tree.part" "$tree" || exit 2
  docker rm "$id" >/dev/null || exit 2
  trap - EXIT
  # The tree is in place; a stale image only takes space.
  docker rmi "$image" >/dev/null || echo "could not remove $image" >&2
  echo "pulled $image"
  ;;
push)
  printf 'FROM scratch\nCOPY . /tree\n' |
    docker build -q -f - --label org.opencontainers.image.source=https://github.com/danielclasen/mxlfs \
      -t "$image" "$tree" >/dev/null
  docker push -q "$image" >/dev/null
  echo "pushed $image"
  ;;
*)
  echo "usage: $0 pull|push" >&2
  exit 2
  ;;
esac

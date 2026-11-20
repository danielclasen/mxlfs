#!/bin/sh
# Print the KMM resources for mxlfs release <version> on Ubuntu and Debian
# nodes: the namespace, the Module whose images KMM builds in the cluster
# and pushes to <registry>, and the ConfigMap holding kmm/Dockerfile they
# are built from.
set -eu
[ $# -eq 2 ] || { echo "usage: $0 <version> <registry>" >&2; exit 2; }
version=$1
registry=$2
# A release version as release-please writes it, without the tag's "v";
# the case keeps a newline from splitting it into lines grep checks alone.
release_version() {
  case $1 in *[!0-9A-Za-z.-]*) return 1 ;; esac
  echo "$1" | grep -Eqx '[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?'
}
release_version "$version" ||
  { echo "$0: version $version is not a release version such as 0.1.0 or 0.1.0-rc.7" >&2; exit 2; }
# An image reference without a tag: lowercase, with no empty path part.
case $registry in '' | *[!a-z0-9._:/-]* | /* | */ | *//*)
  echo "$0: registry $registry must be lowercase letters, digits and . _ : - with single / between parts" >&2; exit 2 ;;
esac
here=$(cd "$(dirname "$0")" && pwd)
sed -e "s|@VERSION@|$version|g" -e "s|@REGISTRY@|$registry|g" "$here/module.yaml"
echo ---
kubectl create configmap mxlfs-kmm-dockerfile --namespace mxlfs \
  --from-file=dockerfile="$here/Dockerfile" --dry-run=client -o yaml

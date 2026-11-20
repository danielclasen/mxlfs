#!/bin/sh
# Print the registry reference of the development image built from this
# tree: DEV_CACHE (ghcr.io/danielclasen/mxlfs-ci-dev by default) tagged
# with a hash of what the image is built from.
set -eu
cd "$(dirname "$0")"
echo "${DEV_CACHE:-ghcr.io/danielclasen/mxlfs-ci-dev}:$(cat Dockerfile sparse.sh | sha256sum | cut -c1-12)"

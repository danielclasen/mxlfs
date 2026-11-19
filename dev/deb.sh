#!/bin/sh
# Build the mxlfs-dkms Debian package into build/deb. Runs on Debian or
# Ubuntu with dpkg-dev, debhelper and dh-dkms installed. The package version
# is the one in dkms.conf with "~" before a pre-release, so 0.1.0~rc.7 sorts
# before 0.1.0; debian/changelog is written from it.
set -eu
cd "$(dirname "$0")/.."
version=$(sed -n 's/^PACKAGE_VERSION="\([^"]*\)".*/\1/p' dkms.conf)
[ -n "$version" ] || { echo "no PACKAGE_VERSION in dkms.conf" >&2; exit 1; }
debversion=$(echo "$version" | sed 's/-/~/')
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/mxlfs"
cp -a dkms.conf kernel debian "$work/mxlfs/"
cat > "$work/mxlfs/debian/changelog" <<CHANGELOG
mxlfs ($debversion) unstable; urgency=medium

  * mxlfs $version, see
    https://github.com/danielclasen/mxlfs/releases/tag/v$version

 -- Daniel Clasen <danielclasen@users.noreply.github.com>  $(date -R)
CHANGELOG
(cd "$work/mxlfs" && dpkg-buildpackage -b -us -uc)
mkdir -p build/deb
find build/deb -name '*.deb' -delete
cp "$work"/*.deb build/deb/
ls -l build/deb

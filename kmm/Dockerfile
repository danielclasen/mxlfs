# The kmod image for a node on Ubuntu or Debian, built in the cluster by
# KMM, which sets KERNEL_FULL_VERSION to the node's kernel: the module is
# compiled against that kernel's headers from the distribution and the
# source of mxlfs release MXLFS_VERSION, checked against its published
# SHA-256. BASE_IMAGE is the distribution release whose archive has those
# headers; APT_SOURCE adds a line to its sources, as for Debian backports.
# MXLFS_URL overrides where the source tarball is fetched from.
ARG BASE_IMAGE=ubuntu:26.04
FROM ${BASE_IMAGE} AS build
ARG KERNEL_FULL_VERSION
ARG MXLFS_VERSION
ARG MXLFS_URL=https://github.com/danielclasen/mxlfs/releases/download/v${MXLFS_VERSION}/mxlfs-${MXLFS_VERSION}.tar.gz
ARG APT_SOURCE=
RUN if [ -n "$APT_SOURCE" ]; then echo "$APT_SOURCE" > /etc/apt/sources.list.d/mxlfs-kmm.list; fi \
 && apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      build-essential kmod curl ca-certificates "linux-headers-${KERNEL_FULL_VERSION}" >/dev/null
RUN mkdir -p /tmp/mxlfs \
 && curl -fsSL -o /tmp/mxlfs.tar.gz "$MXLFS_URL" \
 && echo "$(curl -fsSL "$MXLFS_URL.sha256" | cut -d' ' -f1)  /tmp/mxlfs.tar.gz" | sha256sum -c - \
 && tar -xzf /tmp/mxlfs.tar.gz --strip-components=1 -C /tmp/mxlfs \
 && make -C /tmp/mxlfs/kernel KDIR="/lib/modules/${KERNEL_FULL_VERSION}/build" \
 && mkdir -p "/opt/lib/modules/${KERNEL_FULL_VERSION}" \
 && cp /tmp/mxlfs/kernel/mxlfs.ko "/opt/lib/modules/${KERNEL_FULL_VERSION}/" \
 && depmod -b /opt "${KERNEL_FULL_VERSION}"

FROM busybox:1.37.0
COPY --from=build /opt /opt

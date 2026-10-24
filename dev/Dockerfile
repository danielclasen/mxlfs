# Development image: libmxl from the go-mxl builder, plus what is needed to
# build a kernel and boot it under virtme-ng with the module loaded.
ARG GO_MXL_TAG=1.1.0-rc.7
FROM ghcr.io/qvest-digital/go-mxl-builder:${GO_MXL_TAG}

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      bc bison flex libelf-dev libssl-dev cpio kmod dwarves rsync xz-utils \
      qemu-system-x86 virtme-ng busybox udev iproute2 procps strace \
      linux-perf rdma-core ibverbs-utils \
 && rm -rf /var/lib/apt/lists/*

ENV PKG_CONFIG_PATH=/opt/libmxl/lib/pkgconfig:/opt/libfabric/lib/pkgconfig

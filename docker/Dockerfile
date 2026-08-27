# syntax=docker/dockerfile:1
#
# Build container for the Raspberry Pi 5 / Circle U-Boot fork.
#
# The image carries the toolchain only.  The U-Boot tree is bind-mounted at
# /work at run time, so a source change never requires an image rebuild.
#
# The image is multi-arch: it builds and runs on both linux/amd64 and
# linux/arm64, and cross-compiles with CROSS_COMPILE=aarch64-linux-gnu- on
# both.  gcc-aarch64-linux-gnu is a real package on either architecture --
# on arm64 it resolves through gcc-13-aarch64-linux-gnu, whose dependencies
# are all native -- so no per-architecture special-casing is needed.
#
#   docker build -t uboot-build:24.04 docker/
#   docker/build.sh
#
# See docker/README.md.

FROM ubuntu:24.04

LABEL org.opencontainers.image.title="uboot-build" \
      org.opencontainers.image.description="AArch64 cross-build environment for the Raspberry Pi 5 / Circle U-Boot fork" \
      org.opencontainers.image.source="https://github.com/yeokiwi/uboot"

ARG DEBIAN_FRONTEND=noninteractive

# The first three lines are README.md's documented dependency list.  The rest
# are things that list takes for granted on a developer machine:
#
#   build-essential      host gcc, for tools/, scripts/dtc and Kconfig
#   libncurses-dev       scripts/kconfig/mconf-cfg.sh, i.e. "make menuconfig"
#   python3-setuptools   Python 3.12 has no bundled distutils, which
#                        scripts/dtc/pylibfdt needs if it is ever built
#   file, ca-certificates  assorted Makefile probes and git over https
#
# libgnutls28-dev is load-bearing, not optional: both Pi 5 defconfigs set
# CONFIG_EFI_CAPSULE_FIRMWARE_RAW=y, which builds tools/mkeficapsule.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu \
        make bison flex bc \
        libssl-dev libgnutls28-dev uuid-dev \
        device-tree-compiler swig python3-dev \
        git \
        build-essential \
        libncurses-dev \
        python3-setuptools \
        ca-certificates \
        file \
    && rm -rf /var/lib/apt/lists/*

# Fail here, at image build time, rather than several minutes into someone's
# first U-Boot build, if the cross toolchain is not where CROSS_COMPILE
# expects to find it.
RUN aarch64-linux-gnu-gcc --version && \
    aarch64-linux-gnu-ld --version | head -1

# scripts/setlocalversion (Makefile:2227) runs git against the bind-mounted
# /work.  In the usual case the container runs as the UID that owns the
# worktree and git is happy, but as soon as it does not -- a root-owned
# checkout, a CI runner, someone else's tree -- git refuses with "detected
# dubious ownership" and the build silently loses its version string.
# /etc/gitconfig applies to every UID; a ~/.gitconfig would not, since the
# UID has no home directory of its own.
RUN git config --system --add safe.directory '*'

# That same UID has no /etc/passwd entry, so HOME would otherwise be unset.
ENV HOME=/tmp

# Always cross-compile, on either host architecture, so the output does not
# depend on where the image happens to be running.
ENV CROSS_COMPILE=aarch64-linux-gnu-

COPY entrypoint.sh /usr/local/bin/entrypoint.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]

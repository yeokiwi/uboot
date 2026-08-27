#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
#
# Container entrypoint for the U-Boot build image.
#
# Sets the two defaults that make a bind-mounted build behave, then gets out
# of the way: whatever command was passed to "docker run" is exec'd as-is, so
# "make menuconfig", "bash" and "make O=elsewhere" all work unchanged.

set -e

# Always cross-compile.  Set by the Dockerfile; honoured here so that
# "docker run -e CROSS_COMPILE=..." can still override it.
export CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"

# Build out of tree by default.
#
# /work is the host's worktree, which may already hold objects from a native
# host build made with a different GCC.  Building in place on top of those
# produces confusing failures, so send container output somewhere of its own.
# kbuild reads KBUILD_OUTPUT from the environment (Makefile:121-134); an O= on
# the make command line still takes precedence over it.
#
# Note ${VAR-default}, not ${VAR:-default}: passing KBUILD_OUTPUT= explicitly
# empty has to mean "build in tree", which is what "make mrproper" needs in
# order to clear an in-tree build.  With :- it would silently be redirected
# out of tree and clean nothing.
export KBUILD_OUTPUT="${KBUILD_OUTPUT-build-docker}"

exec "$@"

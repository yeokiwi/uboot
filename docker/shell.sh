#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0+
#
# Interactive shell in the build container, at the U-Boot worktree.
#
# Usage:
#   docker/shell.sh                     # bash prompt at /work
#   docker/shell.sh make menuconfig
#   docker/shell.sh -c 'aarch64-linux-gnu-nm build-docker/u-boot | grep bootcircle'
#
# With no arguments you get bash; note that KBUILD_OUTPUT is already set to
# build-docker, so a bare "make" inside the shell builds out of tree too.

set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

ensure_image

if [ $# -eq 0 ]; then
	uboot_run bash
elif [ "$1" = "-c" ]; then
	uboot_run bash "$@"
else
	uboot_run "$@"
fi

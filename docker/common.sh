# SPDX-License-Identifier: GPL-2.0+
#
# Shared plumbing for build.sh and shell.sh.  Sourced, not executed.

IMAGE=${UBOOT_IMAGE:-uboot-build:24.04}
DOCKER=${DOCKER:-docker}

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)

# Build the image on first use.  The build context is docker/, not the repo
# root, so the ~12 MB of build output sitting in the worktree never enters it.
ensure_image() {
	if ! "$DOCKER" image inspect "$IMAGE" >/dev/null 2>&1; then
		echo ">> building $IMAGE (first run only)" >&2
		"$DOCKER" build -t "$IMAGE" "$here"
	fi
}

# Run a command in the container against the bind-mounted worktree.
uboot_run() {
	local tty=()
	# Only ask for a TTY when there is one, so this works under CI.
	if [ -t 0 ] && [ -t 1 ]; then
		tty=(-it)
	fi

	"$DOCKER" run --rm "${tty[@]}" \
		-v "$repo:/work" \
		-w /work \
		-u "$(id -u):$(id -g)" \
		${CROSS_COMPILE+-e CROSS_COMPILE} \
		${KBUILD_OUTPUT+-e KBUILD_OUTPUT} \
		${SOURCE_DATE_EPOCH+-e SOURCE_DATE_EPOCH} \
		"$IMAGE" "$@"
}

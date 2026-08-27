# Containerised build

Build the Raspberry Pi 5 U-Boot images without installing a toolchain on the
host. The image carries only the cross toolchain and build dependencies; your
worktree is bind-mounted at `/work`, so editing source never means rebuilding
the image.

## Quick start

```bash
docker/build.sh
```

That builds the image on first use, then `rpi5_circle_net_defconfig`. Output
lands in `build-docker/u-boot.bin` (about 920 KiB), owned by you rather than
root.

The other defconfig, and extra make arguments, work as you would expect:

```bash
docker/build.sh rpi5_circle_defconfig
docker/build.sh rpi5_circle_net_defconfig V=1
```

## If the worktree already has a native build

kbuild refuses to build out of tree while the source tree still holds an
in-tree build, and `build.sh` stops with an explanation rather than letting
make fail halfway. The two styles genuinely cannot coexist: the check is
unconditional (`Makefile:634`).

So a worktree that has been built with a plain `make` needs clearing once:

```bash
docker/build.sh --mrproper
```

That runs `make mrproper` in the container first. It deletes the in-tree build
products — `u-boot.bin`, `.config`, `System.map`, the object files — all of
which the next build regenerates. If you have unsaved `menuconfig` work, run
`make savedefconfig` first. After this the worktree is out-of-tree only; native
host builds should then use `make O=build-host ...` too.

## Interactive use

```bash
docker/shell.sh                       # bash prompt at /work
docker/shell.sh make menuconfig
docker/shell.sh make savedefconfig
docker/shell.sh -c 'aarch64-linux-gnu-nm build-docker/u-boot | grep bootcircle'
```

## Where the output goes

Container builds go to `build-docker/` rather than into the tree. The worktree
may already hold objects from a native host build made with a different GCC,
and building in place on top of those fails in confusing ways. `/build*` is
already covered by `.gitignore`, so nothing shows up as untracked.

Override with `KBUILD_OUTPUT`:

```bash
KBUILD_OUTPUT=build-net docker/build.sh rpi5_circle_net_defconfig
KBUILD_OUTPUT=build-sd  docker/build.sh rpi5_circle_defconfig
```

Setting it to the empty string means "build in tree", the way a bare `make`
does — useful if you want the container's output to land where the top-level
[README.md](../README.md#build-u-boot) says it will:

```bash
KBUILD_OUTPUT= docker/shell.sh -c 'make rpi5_circle_net_defconfig && make -j"$(nproc)"'
```

Be aware that this shares object files with any native host build in the same
tree, which is exactly what the `build-docker/` default exists to avoid.

To install the result on an SD card, carry on with `mk-circle-sdcard.sh` on
the host as described in the top-level [README.md](../README.md#install-to-an-sd-card) —
it needs a mounted FAT partition, which is a host concern, not a container one:

```bash
board/raspberrypi/rpi/circle/mk-circle-sdcard.sh \
        /media/$USER/boot build-docker/u-boot.bin ../circle/sample/26-cpustress
```

## x86_64 and aarch64 hosts

The image runs on both, and cross-compiles with
`CROSS_COMPILE=aarch64-linux-gnu-` on both, so the output does not depend on
which kind of machine you built it on. `gcc-aarch64-linux-gnu` is a real
package on either architecture — on arm64 it resolves through
`gcc-13-aarch64-linux-gnu`, whose dependencies are all native — so there is no
per-architecture special-casing in the Dockerfile.

`docker/build.sh` builds for the host's own architecture. To produce a
genuinely multi-arch image:

```bash
docker buildx build --platform linux/amd64,linux/arm64 -t uboot-build:24.04 docker/
```

`--load` accepts only one platform at a time, so a two-platform run is a
build-only check unless you also `--push` to a registry. To load a single
non-native platform locally you need QEMU binfmt registered:

```bash
docker run --privileged --rm tonistiigi/binfmt --install arm64
docker buildx build --platform linux/arm64 --load -t uboot-build:24.04-arm64 docker/
```

Both variants were checked to build the same `u-boot.bin` from the same tree.
The only difference between them is the timestamp U-Boot embeds in its version
string; pin it with `SOURCE_DATE_EPOCH` (`Makefile:2318`,
[doc/build/reproducible.rst](../doc/build/reproducible.rst)) and the two are
byte-identical:

```bash
SOURCE_DATE_EPOCH=1672531200 docker/build.sh
```

`build.sh` passes `SOURCE_DATE_EPOCH`, `CROSS_COMPILE` and `KBUILD_OUTPUT`
through to the container when they are set.

## What is in the image

`ubuntu:24.04` plus the dependency list from the top-level
[README.md](../README.md#dependencies), and four things that list takes for
granted on a developer machine: `build-essential` (host gcc, for `tools/` and
Kconfig), `libncurses-dev` (`make menuconfig`), `python3-setuptools` (Python
3.12 has no bundled `distutils`) and `file`/`ca-certificates`.

`libgnutls28-dev` is not optional. Both Pi 5 defconfigs set
`CONFIG_EFI_CAPSULE_FIRMWARE_RAW=y`, which builds `tools/mkeficapsule`; without
gnutls the build dies on `fatal error: gnutls/gnutls.h: No such file or
directory`.

Deliberately absent: the binman, buildman, pytest and Sphinx Python stacks.
Neither Pi 5 defconfig sets `CONFIG_BINMAN`, and the in-tree `dtc` path needs
no `pylibfdt`. Also absent is the `aarch64-none-elf` bare-metal toolchain used
to build Circle itself — that is a separate download and a separate job.

This is not the same thing as `tools/docker/Dockerfile`, which is upstream
U-Boot's CI runner image: crosstool toolchains for twelve architectures plus
QEMU, TF-A, OP-TEE, coreboot and Arm FVP. That file is left alone.

## Files

| File | |
|---|---|
| `Dockerfile` | the image |
| `entrypoint.sh` | sets `CROSS_COMPILE` and the default `KBUILD_OUTPUT`, then execs your command |
| `common.sh` | shared `docker run` plumbing, sourced by the two scripts below |
| `build.sh` | build a defconfig |
| `shell.sh` | interactive shell or one-off command |
| `compose.yaml` | optional Compose front end; export `UBOOT_UID`/`UBOOT_GID` first, see the note in the file |

## Notes

- The build context is `docker/`, not the repo root, so the build output
  sitting in a used worktree never enters it. The root `.dockerignore` is only
  a safety net for anyone running `docker build -f docker/Dockerfile .` by hand.
- The image sets `git config --system safe.directory '*'`. Normally the
  container runs as the UID that owns the worktree and git is content, but
  when it does not — a root-owned checkout, a CI runner — `setlocalversion`
  would fail git's "dubious ownership" check and the build would silently lose
  its version string. A successful build stamps in something like
  `U-Boot 2026.07-g24ed17207471`.
- Set `UBOOT_IMAGE` to use a different image tag, or `DOCKER=podman` to drive
  Podman instead.
- The container toolchain is GCC 13.3 / binutils 2.42, so its output will not
  be byte-identical to a host build made with a different GCC. On this tree
  `rpi5_circle_net_defconfig` comes out at 877592 bytes and
  `rpi5_circle_defconfig` at 942520 bytes.

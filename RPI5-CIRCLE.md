# U-Boot for the Raspberry Pi 5, booting Circle

This is a fork of [U-Boot](https://www.denx.de/wiki/U-Boot) v2026.07 that can
chain-boot [Circle](https://github.com/rsta2/circle) bare metal applications on
the Raspberry Pi 5 — **including Circle's multi-core support**, which is the
part that normally breaks when a bootloader sits between the firmware and a
bare metal payload.

```bash
make rpi5_circle_defconfig
make CROSS_COMPILE=aarch64-linux-gnu-
board/raspberrypi/rpi/circle/mk-circle-sdcard.sh \
        /media/sdcard u-boot.bin /path/to/circle/sample/26-cpustress
```

Full documentation: [`doc/board/broadcom/raspberrypi-circle.rst`](doc/board/broadcom/raspberrypi-circle.rst).

## What was added

| Change | File |
|---|---|
| Reserve and repair the resident EL3 firmware in low DRAM | `board/raspberrypi/rpi/firmware_mem.c` |
| `bootcircle` command, with a PSCI self test | `board/raspberrypi/rpi/cmd_bootcircle.c` |
| Secondary core trampoline for the self test | `board/raspberrypi/rpi/bootcircle_smp.S` |
| `circle_*` environment | `include/configs/rpi.h` |
| Board configuration | `configs/rpi5_circle_defconfig` |
| SD card contents and helper | `board/raspberrypi/rpi/circle/` |

## Why multi-core needed fixing

Circle starts its secondary cores with a PSCI `CPU_ON` SMC. On the Pi 5 that
call is served by the armstub (`armstub8-2712.bin`, a TF-A BL31), which does
**not** go away after launching its payload — it stays resident in the first
512 KiB of DRAM, and cores 1-3 sit in its hold loop the whole time waiting to
be woken.

U-Boot has no idea any of that is there. DRAM bank 0 starts at `0x0`, so to the
LMB allocator, to image relocation and to every load command, the running PSCI
implementation is ordinary free memory. Anything placed over it leaves core 0
perfectly happy — it is not executing that code — so U-Boot boots, and a
single-core Circle app boots, and the only symptom is that `CPU_ON` later fails
and Circle reports `CPU core 1 did not start`.

This fork reserves `0x0`–`0x80000` with `LMB_NOOVERWRITE` so a load into it
fails loudly, snapshots the region at startup and restores it immediately
before jumping to Circle, and reports the state of the secondary cores first so
that a remaining failure names itself. `bootcircle -t` goes further and
actually starts and stops each core before handing over.

## Upstream

Commit 1 is an unmodified snapshot of U-Boot v2026.07 (upstream commit
`ece349ade2973e220f524ce59e59711cc919263f`). Everything after it is this
project's own work, so `git log` past that commit shows only the delta.

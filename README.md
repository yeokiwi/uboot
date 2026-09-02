# U-Boot for the Raspberry Pi 5, booting Circle

A fork of [U-Boot](https://www.denx.de/wiki/U-Boot) v2026.07 that chain-boots
[Circle](https://github.com/rsta2/circle) bare metal applications on the
Raspberry Pi 5 — **including Circle's multi-core support**, which is the part
that normally breaks when a bootloader sits between the firmware and a bare
metal payload.

Instead of the VideoCore firmware loading `kernel_2712.img` directly, the
firmware loads U-Boot, and U-Boot loads Circle. That buys a prompt, scripting,
and loading over SD, USB or the network in front of a Circle application.

> **Status:** builds clean and is verified by construction and cross-reference
> against Circle, U-Boot and TF-A sources. It has **not** been run on hardware —
> QEMU has no `raspi5` machine. See [Verification status](#verification-status).

There are two builds:

| Config | Use it for |
|---|---|
| `rpi5_circle_defconfig` | The default. Touches as little hardware as possible so the hand-off matches what Circle sees from the firmware. SD card only. |
| `rpi5_circle_net_defconfig` | Adds Raspberry Pi 5 Ethernet so Circle images can be pulled over **TFTP**, a network console, and a **web UI for writing files to the SD card** from a browser. Requires U-Boot to bring up PCIe and the RP1 southbridge on every boot — see [Ethernet and TFTP](#ethernet-and-tftp). |

- Detailed reference: [`doc/board/broadcom/raspberrypi-circle.rst`](doc/board/broadcom/raspberrypi-circle.rst)
- Ethernet reference: [`doc/board/broadcom/raspberrypi-circle-net.rst`](doc/board/broadcom/raspberrypi-circle-net.rst)
- Upstream U-Boot docs: [`doc/`](doc/), and the original [`README`](README)

---

## Contents

- [Dependencies](#dependencies)
- [Build U-Boot](#build-u-boot)
- [Build a Circle application](#build-a-circle-application)
- [Install to an SD card](#install-to-an-sd-card)
- [Run](#run)
- [The U-Boot prompt](#the-u-boot-prompt)
- [Ethernet and TFTP](#ethernet-and-tftp)
- [USB Ethernet and the network console](#usb-ethernet-and-the-network-console)
- [Web storage loader](#web-storage-loader)
- [Troubleshooting](#troubleshooting)
- [Why multi-core needed fixing](#why-multi-core-needed-fixing)
- [What was changed](#what-was-changed)
- [Verification status](#verification-status)
- [Tracking upstream](#tracking-upstream)

---

## Dependencies

### Host toolchain and build tools

Tested on Ubuntu 24.04. On Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
        gcc-aarch64-linux-gnu \
        make bison flex bc \
        libssl-dev libgnutls28-dev uuid-dev \
        device-tree-compiler swig python3-dev \
        git
```

What each is for:

| Package | Needed for |
|---|---|
| `gcc-aarch64-linux-gnu` | the AArch64 cross compiler |
| `bison`, `flex` | Kconfig and dtc parsers |
| `bc`, `make`, `git` | the build itself |
| `libssl-dev` | `mkimage` signing support |
| `libgnutls28-dev` | `tools/mkeficapsule`, which this config builds because `CONFIG_EFI_CAPSULE_FIRMWARE_RAW=y`. **The build fails without it**, with `fatal error: gnutls/gnutls.h: No such file or directory` |
| `uuid-dev` | `tools/mkfwumdata` |
| `device-tree-compiler` | `dtc` |
| `swig`, `python3-dev` | `pylibfdt`, used by binman |

Add `gcc-arm-linux-gnueabihf` only if you also want to build the 32-bit
Raspberry Pi configurations.

On Fedora the equivalents are `gcc-aarch64-linux-gnu`, `bison`, `flex`,
`openssl-devel`, `gnutls-devel`, `libuuid-devel`, `dtc`, `swig` and
`python3-devel`.

### Circle toolchain

Circle wants the bare metal AArch64 toolchain, not the Linux one — its default
prefix for a Pi 5 build is `aarch64-none-elf-`. Get it from
[Arm GNU Toolchain downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
(*AArch64 bare-metal target (aarch64-none-elf)*) and put its `bin/` on your
`PATH`.

### Hardware

- Raspberry Pi 5
- SD card with a FAT32 first partition
- A 3.3 V USB-to-serial adapter. **Read [Run](#run) before wiring it up** —
  U-Boot and a stock Circle application come out of two different connectors.

---

## Build U-Boot

```bash
git clone https://github.com/yeokiwi/uboot.git
cd uboot
make rpi5_circle_defconfig
make -j"$(nproc)" CROSS_COMPILE=aarch64-linux-gnu-
```

The output is `u-boot.bin` (about 920 KiB).

To check the Circle support actually made it in:

```bash
aarch64-linux-gnu-nm u-boot | grep -E 'bootcircle|rpi_fw_mem'
strings u-boot.bin | grep '^circle_boot'
```

`make rpi_arm64_defconfig` still builds the stock upstream configuration, with
`bootcircle` and the `circle_*` environment absent.

### In a container

If you would rather not install a toolchain on the host, everything above is
also available in a build container, on both x86_64 and aarch64 machines:

```bash
docker/build.sh                         # rpi5_circle_net_defconfig
docker/build.sh rpi5_circle_defconfig
```

The output lands in `build-docker/u-boot.bin`, owned by you rather than root.
No host `apt-get` is needed. See [docker/README.md](docker/README.md) — in
particular the note on clearing a native in-tree build first, since kbuild will
not build out of tree alongside one.

---

## Build a Circle application

Any Circle application works. `sample/26-cpustress` is a good first target
because it uses all four cores, which is exactly what this fork exists to keep
working.

```bash
git clone https://github.com/rsta2/circle.git
cd circle

# -r 5 selects the Raspberry Pi 5 and defaults the toolchain prefix to
# aarch64-none-elf-.  --multicore defines ARM_ALLOW_MULTI_CORE.
./configure -r 5 --multicore

# build the Circle libraries
./makeall

# build the application itself (./makeall --sample builds every sample)
( cd sample/26-cpustress && make )

# fetch the firmware files the SD card needs
( cd boot && make )
```

This gives you `sample/26-cpustress/kernel_2712.img` and, in `boot/`,
`bcm2712-rpi-5-b.dtb` and `bcm2712d0.dtbo`.

Circle's default `KERNEL_MAX_SIZE` is 2 MiB from `0x80000`. For something
larger, pass `--kernel-max-size <megabytes>` to `configure`.

---

## Install to an SD card

The Pi 5 keeps its firmware in EEPROM, so the boot partition only needs five
files. Mount the FAT32 first partition and run:

```bash
board/raspberrypi/rpi/circle/mk-circle-sdcard.sh \
        /media/sdcard \
        u-boot.bin \
        /path/to/circle/sample/26-cpustress
```

It copies, it does not partition or format — point it at an already-mounted
partition. The result:

```
config.txt                  from board/raspberrypi/rpi/circle/
u-boot.bin                  this build
kernel_2712.img             your Circle application
bcm2712-rpi-5-b.dtb         from circle/boot/
overlays/bcm2712d0.dtbo     from circle/boot/
```

By default the script looks for the `.dtb` and `.dtbo` in
`<app-dir>/../../boot`; pass a fourth argument to point somewhere else.

`config.txt` sets `kernel=u-boot.bin`, `kernel_address=0x80000`,
`arm_64bit=1`, `initial_turbo=0` and `enable_uart=1`.

---

## Run

### Wiring the serial console

This is the one thing that catches people out. On the Raspberry Pi 5 the two
halves of the boot talk on **different physical connectors**:

| Output | UART | Where |
|---|---|---|
| U-Boot | `uart10` @ `0x107d001000`, the DT `stdout-path` | the dedicated 3-pin **debug UART** header next to the HDMI ports |
| Circle, as shipped | RP1 UART0 @ `0x1f00030000`, `CSerialDevice` device 0 | **GPIO 14/15**, pins 8 and 10 of the 40-pin header |

Both run at 115200 8N1. Pick one:

- **Two adapters** (or move the cable once U-Boot hands over) — no code changes.
- **One cable**: construct Circle's serial device as device 10 in your
  application, e.g. `CSerialDevice m_Serial {&m_Interrupt, FALSE, 10};`, and
  everything comes out of the debug header.

### Boot

Power on. The firmware loads `u-boot.bin`, U-Boot relocates itself to the top
of RAM — which frees `0x80000` again — and after a two second countdown runs
`bootcmd`, which is `run circle_boot`:

```
U-Boot 2026.07 (...)

DRAM:  8 GiB
RPI 5 Model B (0x...)
Core:  ... devices, ... uclasses
MMC:   mmc@... : 0
Loading Environment from FAT... OK
Hit any key to stop autoboot:  0
1234567 bytes read in 62 ms (19 MiB/s)
PSCI: v1.1
Starting Circle at 0x80000 (DTB 0x2eff2e00)

Starting kernel ...
```

and then Circle's own output on the other connector. Press any key during the
countdown to get a prompt instead.

---

## The U-Boot prompt

### `bootcircle`

```
bootcircle [-t] [entry [dtb]]
```

`entry` defaults to `0x80000`, `dtb` to the device tree the firmware handed
U-Boot. An `entry` below `0x80000` is refused — that is where the resident EL3
firmware lives.

`-t` starts and stops each secondary core before handing over, to prove PSCI
can still bring them up:

```
U-Boot> bootcircle -t
PSCI: v1.1
PSCI self test:
  core 1: started, MPIDR 0x100
  core 2: started, MPIDR 0x200
  core 3: started, MPIDR 0x300
Starting Circle at 0x80000 (DTB 0x2eff2e00)
```

Each core takes itself back off once it has reported in, so a passing self test
leaves them exactly as Circle expects. Use `-t` when diagnosing, not routinely:
a core that starts but does not go off again is reported as such, and Circle
would then see `ALREADY_ON` for it.

### Environment

| Variable | Default | Meaning |
|---|---|---|
| `circle_kernel` | `kernel_2712.img` | image to load |
| `circle_addr` | `0x80000` | load and entry address, fixed by Circle |
| `circle_dev` | `mmc` | device to load from |
| `circle_part` | `0:1` | partition |
| `circle_full_hw` | `0` | `0` leaves the RP1, PCIe and framebuffer as the firmware left them. `1` runs `pci enum; usb start` at preboot for a USB keyboard and HDMI console in U-Boot |
| `circle_load` | `fatload ...` | the load command |
| `circle_boot` | `if run circle_load; then bootcircle ...; fi` | what `bootcmd` runs |
| `circle_usbnet` | `0` | `1` runs `usb start` at preboot, so a USB Ethernet adapter is ready without a command (network build only) |
| `circle_netcon` | `0` | `1` brings the network up and attaches the network console at preboot (network build only) |

Turn on the full hardware bring-up:

```
U-Boot> setenv circle_full_hw 1
U-Boot> saveenv
```

Note that Circle then re-initialises hardware U-Boot has already configured,
which is untested — hence the default of `0`.

Load over the network instead of from SD:

```
U-Boot> setenv circle_load 'tftp ${circle_addr} kernel_2712.img'
U-Boot> saveenv
```

---

## Ethernet and TFTP

Copying a new `kernel_2712.img` to the SD card for every build gets old fast.
`rpi5_circle_net_defconfig` adds networking so you can pull the image over TFTP
— and drive U-Boot itself over the network instead of a serial cable:

```bash
make rpi5_circle_net_defconfig
make -j"$(nproc)" CROSS_COMPILE=aarch64-linux-gnu-
```

Then, with `kernel_2712.img` in your TFTP root:

```
U-Boot> setenv serverip 192.168.1.10
U-Boot> run circle_netboot
```

`run circle_netcheck` walks the whole chain (`pci`, `dm tree`, `clk dump`,
`usb tree`, `mdio list`, `net list`) in one command, which is what you want the
first time you bring it up. The layer-by-layer procedure, and a table of
failure signatures, are in
[`raspberrypi-circle-net.rst`](doc/board/broadcom/raspberrypi-circle-net.rst).

### What this actually required

There is no Ethernet MAC on the BCM2712. On the Pi 5 it is a Cadence GEM
*inside the RP1 southbridge*, which is a PCIe endpoint:

```
pcie2  brcm,bcm2712-pcie          <- root complex, in U-Boot already
 └ pci@0,0
    └ dev@0,0  pci1de4,1          <- RP1 endpoint, BAR1 = 4 MiB window
       └ pci-ep-bus@1  simple-bus
          ├ clocks@40018000   raspberrypi,rp1-clocks
          ├ pinctrl@400d0000  raspberrypi,rp1-gpio
          └ ethernet@40100000 raspberrypi,rp1-gem / cdns,macb
```

Only the root complex existed in U-Boot v2026.07 — and RP1 support is in no
U-Boot release, including current master. The endpoint, clock and GPIO drivers
plus the macb-over-PCI changes are carried here from Oleksii Moisieiev's
[unmerged RFC series](https://patchwork.ozlabs.org/project/uboot/list/?series=442967),
with original authorship preserved, and adapted on top.

### Why it is a separate build

Ethernet means U-Boot brings PCIe and RP1 up on every boot, and Circle then
re-initialises hardware U-Boot has already configured. The default build
deliberately leaves the hardware as the firmware left it, which is the state
Circle is known to work from. Keeping them separate makes that an explicit
choice and leaves a known-good fallback. Both builds get the multi-core fix and
the firmware restore.

The network build also uses the **legacy** U-Boot network stack rather than
lwIP, because `CONFIG_NETCONSOLE` exists only there. The only casualty is
`wget https`, whose TLS glue is written against lwIP's TCP; plain `wget` and
EFI HTTP boot still work.

---

## USB Ethernet and the network console

The Pi 5 can use either of two interfaces, and either one can do TFTP and the
network console:

| | Interface | Brought up by |
|---|---|---|
| `eth0` | onboard MAC (Cadence GEM in RP1) | automatically |
| `eth1` | USB adapter (RTL8152B/8153A/8153B) | `usb start` |

`netdev` picks one and everything follows it:

```
U-Boot> run net_onboard          # netdev=eth0
U-Boot> run net_usb              # usb start, then netdev=eth1
U-Boot> net list                 # which is which, and which is active
```

USB works because RP1's two USB host controllers are ordinary DesignWare USB3
cores sitting on the same PCIe-endpoint bus as the Ethernet MAC, so
`CONFIG_USB_XHCI_DWC3` binds them without new code. The Realtek driver was
already in the tree, unused by any Pi config.

### Network console

`CONFIG_NETCONSOLE` carries the U-Boot console over UDP, in both directions, on
whichever interface `netdev` selects:

```
U-Boot> run net_usb              # or: run net_onboard
U-Boot> setenv ncip 192.168.1.10 # your host; leave unset to broadcast
U-Boot> run nc
```

and on the host, `tools/netconsole <board-ip>`. `run nc_off` puts it back.

`nc_on` *adds* `nc` to the existing console rather than replacing it, so the
serial port keeps working and a wrong `ncip` is not a lockout. To start it on
every boot, set `circle_netcon=1` and `saveenv` — don't put `nc` in `stdout`
directly, because the console is assigned long before any network device
exists and the output would go nowhere.

Full details, including how to switch interfaces cleanly while netconsole is
attached, are in
[`raspberrypi-circle-net.rst`](doc/board/broadcom/raspberrypi-circle-net.rst).

### Which adapters work

| Chip | Chip version | Speed |
|---|---|---|
| RTL8152B | 1, 2, 7 | 100M |
| RTL8153A | 3, 4, 5, 6 | 1G |
| RTL8153B | 8, 9 | 1G |
| RTL8153C | 14 | 1G |
| RTL8156 / RTL8156A | 10, 11 | 2.5G |
| RTL8156B / RTL8156BG | 12, 13, 15 | 2.5G |

The 2.5GbE parts all share the USB product ID `0bda:8156` (some `0bda:8155`);
the driver reads the chip-version register to tell them apart. A chip newer
than the driver shows up in `usb tree` and is absent from `net list`, and says
which version it is:

```
r8152: unknown chip, TCR version 0x....
r8152: unsupported chip version 0
```

RTL8156 support is an **unmerged vendor patch series** (ChunHao Lin, Realtek,
u-boot list, Nov 2024) carried here with authorship preserved — it is in no
U-Boot release. RTL8157 (5G) is patch 5/5 of that series and is not carried.

---

## Web storage loader

The network build serves a web page that writes an uploaded file to the SD
card or eMMC. It exists for the Pi that is already installed in something:
replacing `kernel_2712.img` needs neither a card reader nor a serial cable nor
a TFTP server — a browser on the same network is enough.

**Every boot offers it before `bootcmd` runs**, and the decision is one ping:

```
preboot → circle_preboot → circle_netpreboot → web_preboot
                                                  │
                             run net_usb   USB adapter becomes the active interface
                             dhcp          (skipped if ipaddr is already set)
                             ping ${web_server}   ← or serverip, from DHCP
                               │                     │
                             reply                 silence
                               │                     │
                             httpd — the board waits │
                                                     └→ bootcmd, as normal
```

A machine that answers means someone is there to hand the board a file, so the
UI is served and the board waits for it. Nothing answering — no adapter, no
cable, no DHCP, no server — costs about ten seconds, which is `ping`'s own
timeout, and then the board boots as it always did. The page's **Continue
boot** button (and Ctrl-C on the console) gives the board back at any time.

Point it at the machine that will do the flashing, rather than at whatever
DHCP left in `serverip`:

```
U-Boot> setenv web_server 192.168.1.10
U-Boot> saveenv
```

To turn the whole thing off: `setenv web_enable 0; saveenv`. To run the UI by
hand instead: `run net_usb; dhcp; httpd`.

When the loader stands down it says `Web loader: no server, continuing boot` —
that one line covers no adapter, no address and no reply alike, and `ping`'s
own "host … is not alive" appears before it when the probe was actually made.
Running `run web_net`, `run web_ip` and `run web_ping` by hand shows which step
gives up.

The page lists the MMC partitions it can write to with the filesystem on each,
takes a dropped or chosen file, shows upload progress, and lists what is on the
partition before and after. It fetches nothing from the internet — it is served
by U-Boot itself. Scripted uploads work too:

```bash
curl -F iface=mmc -F part=0:1 -F name=kernel_2712.img \
     -F file=@kernel_2712.img http://<board-ip>/api/upload
```

The upload is buffered at `$loadaddr` (`httpd_addr`) and capped at 64 MiB
(`httpd_maxsize`), and the write goes through the filesystem, so the target
partition needs `CONFIG_FAT_WRITE` — which the network config enables.

Because U-Boot's legacy TCP has room for one connection at a time, this is a
server for a trusted network and one user at a time: while the page is up,
anything that can reach the board can write to its card. Command reference:
[`doc/usage/cmd/httpd.rst`](doc/usage/cmd/httpd.rst).

---

## Troubleshooting

**Nothing on the serial console at all.** Check you are on the right connector
(see [Run](#run)), then that `config.txt` reached the card and says
`kernel=u-boot.bin`.

**U-Boot runs, Circle never starts.** Check the load succeeded and the address
is `0x80000`. `bootcircle` prints the entry and DTB it is using.

**Circle starts but reports `mcore: CPU core 1 did not start`.** Interrupt the
countdown and run `bootcircle -t`. If the self test also fails, the problem is
on the firmware side rather than in Circle — see below.

**`ALREADY_ON (-4)`.** Something started the secondary cores before Circle did.
`bootcircle` reports any core it finds in the `ON` state before jumping.

**Confirm the low-memory guard is live.** This should be *refused*, not
silently accepted:

```
U-Boot> fatload mmc 0:1 0x1000 kernel_2712.img
```

---

## Why multi-core needed fixing

Circle starts its secondary cores with a PSCI `CPU_ON` SMC. On the Pi 5 that
call is served by the armstub (`armstub8-2712.bin`, a TF-A BL31), which does
**not** go away after launching its payload. Per TF-A's rpi5 `platform_def.h`
it stays resident in the first 512 KiB of DRAM:

| Address | Contents |
|---|---|
| `0x100` | trusted mailbox entry point |
| `0x108`–`0x128` | per-core hold state |
| `0x1000`–`0x80000` | BL31 text, data and stacks |
| `0xF8` | the firmware's device tree pointer, which is also what Circle reads |

Cores 1–3 never leave BL31's hold loop; they sit there waiting to be woken.

U-Boot has no idea any of that is there. DRAM bank 0 starts at `0x0`, so to the
LMB allocator, to image relocation and to every load command, the running PSCI
implementation looks like ordinary free memory. Overwriting it does not upset
core 0 — it is not executing that code — so U-Boot boots fine, and a
single-core Circle application boots fine. The only symptom is that `CPU_ON`
later fails and Circle reports `CPU core 1 did not start`.

This fork addresses it in three layers:

1. **Reserve** `0x0`–`0x80000` in the LMB with `LMB_NOOVERWRITE`, so a load into
   the region fails loudly instead of quietly costing three cores. The existing
   one-page EFI spin-table reservation is widened to match on BCM2712.
2. **Repair** — snapshot the region at startup, restore it immediately before
   jumping to Circle.
3. **Report** — print the PSCI version and the state of cores 1–3 before
   handing over, and with `-t` actually start and stop each one.

---

## What was changed

Commit 1 is an unmodified U-Boot v2026.07 snapshot. Everything after it is
either this project's work or the ported RP1 series.

Circle support:

| Change | File |
|---|---|
| Reserve, snapshot and restore the resident EL3 firmware | `board/raspberrypi/rpi/firmware_mem.c` |
| Region layout and API | `arch/arm/mach-bcm283x/include/mach/fw_mem.h` |
| `bootcircle` command and PSCI self test | `board/raspberrypi/rpi/cmd_bootcircle.c` |
| Secondary core trampoline | `board/raspberrypi/rpi/bootcircle_smp.S` |
| Hook, widened EFI reservation, `rpi_fw_dtb_pointer()` | `board/raspberrypi/rpi/rpi.c` |
| `CONFIG_CMD_BOOTCIRCLE` | `board/raspberrypi/rpi/Kconfig` |
| `circle_*` environment | `include/configs/rpi.h` |
| Board configuration | `configs/rpi5_circle_defconfig` |
| SD card contents and helper script | `board/raspberrypi/rpi/circle/` |
| Reference documentation | `doc/board/broadcom/raspberrypi-circle.rst` |

Ethernet, for TFTP boot — the RP1 drivers are carried from the
[unmerged RFC series](https://patchwork.ozlabs.org/project/uboot/list/?series=442967)
with original authorship preserved, then adapted:

| Change | File |
|---|---|
| RP1 PCIe endpoint driver, BAR ordering workaround | `drivers/mfd/rp1.c` |
| RP1 clock driver | `drivers/clk/clk-rp1.c` |
| RP1 GPIO driver | `drivers/gpio/rp1_gpio.c` |
| macb over PCI, `raspberrypi,rp1-gem`, mdio `reset-gpios` | `drivers/net/macb.c` |
| RP1 probe hook, `board_type` | `board/raspberrypi/rpi/rpi.c` |
| Network configuration | `configs/rpi5_circle_net_defconfig` |
| Ethernet documentation | `doc/board/broadcom/raspberrypi-circle-net.rst` |

USB Ethernet and the network console — no new drivers were needed, only
configuration and glue:

| Change | File |
|---|---|
| Legacy net stack, `NETCONSOLE`, `USB_ETHER_RTL8152`, `USB_XHCI_DWC3`, `CMD_CLK` | `configs/rpi5_circle_net_defconfig` |
| `netdev` interface selection, `nc_*` scripts, `circle_netpreboot` hook | `include/configs/rpi.h` |
| Detach the network console and halt the NIC before handing over | `board/raspberrypi/rpi/cmd_bootcircle.c` |
| Name the unsupported chip instead of "Unknown Device" | `drivers/usb/eth/r8152.c` |

Web storage loader — an HTTP server on the legacy TCP stack, and the boot-time
ping that decides whether to run it:

| Change | File |
|---|---|
| HTTP server: request parsing, multipart upload, write to storage | `net/httpd.c` |
| The page it serves | `net/httpd_page.c` |
| `httpd` command | `cmd/httpd.c` |
| `HTTPD` protocol in the net loop | `net/net.c`, `include/net-legacy.h` |
| `CONFIG_HTTPD`, `CONFIG_CMD_HTTPD` | `net/Kconfig`, `cmd/Kconfig` |
| `web_*` environment and the `web_preboot` hook | `include/configs/rpi.h` |
| `HTTPD`, `CMD_HTTPD`, `FAT_WRITE` | `configs/rpi5_circle_net_defconfig` |
| Command documentation | `doc/usage/cmd/httpd.rst` |

RTL8156/RTL8156B (2.5GbE) and RTL8153C — carried from ChunHao Lin's unmerged
[vendor series](https://lists.denx.de/pipermail/u-boot/2024-November/572954.html)
with original authorship preserved:

| Change | File |
|---|---|
| Bit-manipulation helpers (patch 1/5) | `drivers/usb/eth/r8152.c` |
| RTL8156/RTL8156B support, 2.5G speed path (patch 2/5) | `drivers/usb/eth/r8152.{c,h}`, `r8152_fw.c` |
| RTL8153C support (patch 3/5) | `drivers/usb/eth/r8152.{c,h}`, `r8152_fw.c` |
| Kconfig prompt and help text | `drivers/usb/eth/Kconfig` |

### The hand-off `bootcircle` performs

Neither `booti` nor `go` can start a Circle image: `booti` rejects it for
having no Linux ARM64 image header, and `go` jumps with the MMU and caches
still on. `bootcircle` reproduces what the armstub does instead —

| | |
|---|---|
| Entry | `0x80000` (`MEM_KERNEL_START`) |
| Caches and MMU | off, via `cleanup_before_linux()` |
| Exception level | EL2; Circle drops to EL1 itself |
| Device tree | 32-bit pointer written to `0xF8` (`ARM_DTB_PTR32`) — Circle does **not** read `x0` |

— in the order `bootm_final()` → restore firmware → write `0xF8` →
`cleanup_before_linux()` → `armv8_switch_to_el2()`.

---

## Verification status

Done in CI-equivalent conditions:

- `rpi5_circle_defconfig` and `rpi5_circle_net_defconfig` build clean, zero
  compiler diagnostics
- `rpi_arm64_defconfig` builds clean, with `bootcircle` and the `circle_*`
  environment absent — no regression to the stock configuration
- `rpi_2/3/4_defconfig` and `rpi_3_32b/4_32b_defconfig` build clean across both
  toolchains, exercising the 32-bit stub path in the shared `rpi.c`
- `scripts/checkpatch.pl` clean on all new files, apart from its standing
  "new command — add a test" notice; the command cannot run under sandbox
- The hand-off call order confirmed by disassembly
- Hardcoded constants cross-checked against Circle master (`MEM_KERNEL_START`,
  `ARM_DTB_PTR32`) and TF-A rpi5 (`BL31_BASE`, `BL31_LIMIT`,
  `PLAT_RPI3_TM_ENTRYPOINT`)
- RP1 clock IDs diffed against the upstream `raspberrypi,rp1-clocks.h` binding:
  every shared ID matches, including `RP1_CLK_SYS`, `RP1_CLK_ETH` and
  `RP1_CLK_ETH_TSU`
- All RP1 drivers confirmed linked and registered (`clk_rp1`, `rp1_driver`,
  `rp1_gpio`, `eth_macb`, `pcie_brcm_base`)
- The HTTP server's request handling was exercised on the host: the same
  `net/httpd.c` compiled against stubs for the fs, block and TCP layers, then
  driven through the TCP callbacks. Covered: the page and its `Content-Length`,
  the JSON endpoints (including URL decoding and JSON escaping), a 100 kB
  multipart upload delivered both in order and as reversed 512-byte segments
  (byte-compared after the write), `Expect: 100-continue`, and the rejection
  paths — path traversal, unknown endpoint, unsupported method, chunked body
  and an upload larger than the buffer. The harness is not part of the tree;
  the server has not been driven by a real browser on hardware
- USB Ethernet and netconsole: `r8152_eth` (with its USB id table),
  `xhci_dwc3` and `drv_nc_init` confirmed linked and registered, and the
  `net_*`/`nc_*` environment confirmed present in `u-boot.bin`
- `make savedefconfig` on the network build round-trips: every symbol added is
  load-bearing, and none was silently dropped by the lwIP → legacy switch
- The RTL8156 vendor series was verified by **git blob hash at every step**: the
  base `r8152.c` is `e3f20e08c3`, and applying 1/5 → 2/5 → 3/5 reproduces
  `294f7d776e` → `c767b78291` → `8ddfb402d8` exactly, matching the `index`
  lines the patches themselves declare, for all three files touched. Patch 1/5
  had to be reconstructed from a PDF that lost tab indentation; the hash match
  proves the reconstruction is byte-identical to the original
- `r8156_init`, `r8156b_init`, `rtl8156_up/down/enable/disable`, `r8153c_init`
  and `r8153c_firmware` confirmed linked, and `0bda:8156`/`0bda:8155` confirmed
  present in the USB id table in `u-boot.bin`
- `rock5b-rk3588` and `imx8mp_venice` — two of the 71 non-Pi boards that enable
  this driver — compile `r8152.o`/`r8152_fw.o` and link `u-boot` cleanly; their
  builds stop later at binman for want of vendor blobs (rockchip-tpl, ATF, DDR
  firmware, OP-TEE), which is unrelated to this change and equally true before it

Not done:

- **No hardware test.** QEMU has no `raspi5` machine, so nothing here has run
  on a real Pi 5. [Run](#run) and [Troubleshooting](#troubleshooting) are the
  procedure to validate it.
- The *mechanism* behind the multi-core failure is pinned down, but not the
  specific write that lands on the firmware region on any given setup. The fix
  is deliberately layered so it holds either way.
- The RP1 Ethernet stack is an unmerged RFC adapted to a device tree it was not
  written against. It is the least proven part of this tree — bring it up with
  `run circle_netcheck` and the layer-by-layer procedure in the Ethernet doc.
- **RP1 USB has never run under U-Boot.** The controllers need no clock, reset
  or PHY properties, so no code had to be written for them — but that also
  means U-Boot leaves them exactly as the firmware's own USB boot scan did, and
  the USB-3 ports' VBUS pinmux is not applied because the RP1 GPIO driver has
  no pinctrl support. If an adapter is not detected, try the USB-2 ports.
- The USB adapter is assumed to be `eth1`, which holds whenever the onboard MAC
  is also present, but the firmware device tree has no `ethernet0` alias, so
  the numbering is bind order rather than a guarantee. `net list` is the ground
  truth.
- **The 2.5GbE support has never seen an RTL8156.** It is a vendor patch series
  that was posted to the U-Boot list and never merged, applied here unmodified.
  Nothing in it has run against silicon in this tree, and the `r8156_hw_phy_cfg`
  register sequences cannot be verified by reading. Treat first bring-up as
  genuinely unproven, and use `run circle_netcheck` for it.

---

## Tracking upstream

Commit 1 imports the upstream tree wholesale:

```
tag         v2026.07
tag object  5b7003b7dd0ffb2d98f46edae3d818d9d1adc2bf
commit      ece349ade2973e220f524ce59e59711cc919263f
```

No upstream history is carried, so `git log` past that commit shows only this
project's changes. To move to a newer release, re-import the new tag over the
tree and replay the commits after it.

---

## Licence

U-Boot is GPL-2.0+; see [`Licenses/`](Licenses/) and the SPDX identifier at the
top of each file. The files added by this fork carry the same licence.

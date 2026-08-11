.. SPDX-License-Identifier: GPL-2.0+

Booting Circle on the Raspberry Pi 5
====================================

`Circle <https://github.com/rsta2/circle>`_ is a C++ bare metal environment for
the Raspberry Pi.  Normally the VideoCore firmware loads a Circle application
directly; putting U-Boot in front of it adds a prompt, scripting, and loading
over SD, USB or the network, at the cost of having to reproduce the hand-off
the firmware would have performed.

``CONFIG_CMD_BOOTCIRCLE`` adds a ``bootcircle`` command that does exactly that,
and ``rpi5_circle_defconfig`` is a ready-made configuration around it.

Why not booti or go
-------------------

``kernel_2712.img`` is a raw binary with no Linux ARM64 image header, so
``booti`` rejects it.  ``go`` jumps with the MMU and caches still enabled,
which Circle's ``sysinit`` does not tolerate.

The hand-off Circle expects
---------------------------

From ``circle/lib/startup64.S``, ``circle/include/circle/memorymap64.h`` and
``circle/include/circle/startup.h``:

======================  ======================================================
Entry address           ``0x80000`` (``MEM_KERNEL_START``)
Caches and MMU          off
Exception level         EL2; Circle drops to EL1 itself
Device tree             32-bit pointer in the word at ``0xF8``
                        (``ARM_DTB_PTR32``).  ``x0`` is *not* read
Secondary cores         started by Circle with PSCI ``CPU_ON`` via ``smc #0``
======================  ======================================================

``bootcircle`` reproduces all of it.

.. warning::

   Never load anything below ``0x80000`` on a Raspberry Pi 5.

   The armstub (``armstub8-2712.bin``, a TF-A BL31) stays resident there and
   serves PSCI from EL3 for the lifetime of the system.  Per TF-A's rpi5
   ``platform_def.h``:

   ==================  ====================================================
   ``0x100``           trusted mailbox entry point
   ``0x108``-``0x128`` per-core hold state
   ``0x1000``          BL31 text, data and stacks
   ``0x80000``         end of BL31, start of the payload
   ==================  ====================================================

   Cores 1-3 never leave BL31's hold loop; they wait there for a ``CPU_ON``.
   U-Boot's DRAM bank 0 starts at ``0x0``, so without the reservation this
   configuration installs, that region looks like ordinary free memory to the
   allocator and to every load command.  Overwriting it does not upset core 0,
   which is not executing any of that code, so U-Boot and a single core
   payload both come up normally - the failure only shows up when Circle tries
   to start the other cores and they never arrive.

   With ``CONFIG_CMD_BOOTCIRCLE`` the region is reserved in the LMB with
   ``LMB_NOOVERWRITE``, so a load into it fails rather than silently costing
   you three cores, and a snapshot taken at startup is restored just before
   the jump.

Building
--------

.. code-block:: bash

   make rpi5_circle_defconfig
   make CROSS_COMPILE=aarch64-linux-gnu-

Preparing the SD card
---------------------

The Raspberry Pi 5 keeps its firmware in EEPROM, so the boot partition only
needs the device tree, the D0 stepping overlay, ``config.txt``, ``u-boot.bin``
and the Circle image:

.. code-block:: bash

   board/raspberrypi/rpi/circle/mk-circle-sdcard.sh \
           /media/sdcard u-boot.bin /path/to/circle/sample/26-cpustress

giving::

   config.txt
   u-boot.bin
   kernel_2712.img
   bcm2712-rpi-5-b.dtb
   overlays/bcm2712d0.dtbo

The ``.dtb`` and ``.dtbo`` come from the ``boot/`` directory of a Circle
checkout once ``make`` there has downloaded the firmware.

Booting
-------

The firmware loads ``u-boot.bin`` in place of the kernel.  U-Boot relocates
itself to the top of RAM, which frees ``0x80000`` again, then after a two
second delay runs::

   bootcmd=run circle_boot

which loads ``kernel_2712.img`` to ``0x80000`` and calls ``bootcircle``.  Press
a key during the countdown for a prompt.

Environment
-----------

=====================  =====================================================
``circle_kernel``      image to load, default ``kernel_2712.img``
``circle_addr``        load and entry address, fixed at ``0x80000`` by Circle
``circle_dev``         device to load from, default ``mmc``
``circle_part``        partition, default ``0:1``
``circle_full_hw``     ``0`` (default) leaves the RP1, PCIe and the
                       framebuffer as the firmware left them, which is the
                       state Circle initialises from natively.  ``1`` runs
                       ``pci enum; usb start`` at preboot, for a USB keyboard
                       and HDMI console in U-Boot - but then Circle
                       re-initialises hardware U-Boot already configured,
                       which is untested
``circle_load``        the load command
``circle_boot``        load, then ``bootcircle``
=====================  =====================================================

To load over the network instead::

   setenv circle_load 'tftp ${circle_addr} kernel_2712.img'
   saveenv

The bootcircle command
----------------------

::

   bootcircle [-t] [entry [dtb]]

``entry`` defaults to ``0x80000`` and ``dtb`` to the device tree the firmware
gave U-Boot.  An ``entry`` below ``0x80000`` is refused, because that is the
resident firmware.

Before jumping, the command reports the PSCI version and the state of cores
1-3::

   U-Boot> bootcircle
   PSCI: v1.1
   Starting Circle at 0x80000 (DTB 0x2eff2e00)

A core reported as ``ON`` here means something has already started it and
Circle's ``CPU_ON`` will return ``ALREADY_ON`` (-4).

``-t`` additionally starts and stops each secondary core before handing over::

   U-Boot> bootcircle -t
   PSCI: v1.1
   PSCI self test:
     core 1: started, MPIDR 0x100
     core 2: started, MPIDR 0x200
     core 3: started, MPIDR 0x300
   Starting Circle at 0x80000 (DTB 0x2eff2e00)

This is worth more than the version check alone: ``PSCI_VERSION`` answering
only shows the SMC vector is intact, while ``CPU_ON`` exercises the parked
cores and the trusted mailbox, which is the part that breaks.  Each core is
taken back off afterwards, so Circle still finds them in the state it expects.

Testing multicore
-----------------

Build a Circle sample that uses ``CMultiCoreSupport``, with
``ARM_ALLOW_MULTI_CORE`` defined in ``circle/include/circle/sysconfig.h`` -
``sample/26-cpustress`` is a good one.  All four cores should come up.  If
Circle reports::

   mcore: CPU core 1 did not start

then run ``bootcircle -t`` from the prompt.  If the self test fails too, the
problem is on the firmware side, not in Circle.

As a negative control, check that the reservation is live::

   U-Boot> fatload mmc 0:1 0x1000 kernel_2712.img

This should be refused rather than quietly corrupting BL31.

Without bootcircle
------------------

A Circle image can also be wrapped in a legacy uImage and started with
``bootm``, which needs no new code::

   mkimage -A arm64 -O linux -T kernel -C none -a 0x80000 -e 0x80000 \
           -n Circle -d kernel_2712.img circle.uimg

That path does the same cache and MMU teardown, but it does not restore the
resident firmware and does not write the device tree pointer at ``0xF8``, so
treat it as a cross-check rather than a substitute.

Limitations
-----------

* Circle's default ``KERNEL_MAX_SIZE`` is 2 MiB from ``0x80000``.  A larger
  application needs that raised in ``circle/include/circle/sysconfig.h``.
* With ``circle_full_hw=1``, U-Boot has already configured the RP1, PCIe and
  the framebuffer.  ``bootcircle`` removes the drivers before jumping but does
  not reset the hardware.
* If a firmware release ever parks the secondary cores in the ``ON`` state,
  nothing U-Boot does can fix it - only a core itself can call ``CPU_OFF``.
  Supplying a known good ``armstub8-2712.bin`` from TF-A's rpi5 platform with
  ``armstub=`` in ``config.txt`` is the escape hatch.

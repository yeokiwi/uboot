.. SPDX-License-Identifier: GPL-2.0+

Ethernet and TFTP boot on the Raspberry Pi 5
============================================

``rpi5_circle_net_defconfig`` adds Ethernet to the Circle setup described in
:doc:`raspberrypi-circle`, so a Circle image can be pulled over TFTP instead of
being copied to the SD card for every build.

.. warning::

   This is a **separate build** from ``rpi5_circle_defconfig`` on purpose.  See
   `Why this is a separate config`_ before choosing it.

What had to be built
--------------------

There is no Ethernet MAC on the BCM2712 itself.  On the Raspberry Pi 5 it is a
Cadence GEM inside the RP1 southbridge, which is a PCIe endpoint::

   pcie2  brcm,bcm2712-pcie          <- root complex, in U-Boot already
    └ pci@0,0                        <- bridge
       └ dev@0,0  pci1de4,1          <- RP1 endpoint, BAR1 = 4 MiB window
          └ pci-ep-bus@1  simple-bus
             ├ clocks@40018000   raspberrypi,rp1-clocks
             ├ pinctrl@400d0000  raspberrypi,rp1-gpio
             └ ethernet@40100000 raspberrypi,rp1-gem / cdns,macb

Only the root complex was present in U-Boot v2026.07.  Everything below it -
the RP1 endpoint driver, its clock and GPIO drivers, and the macb changes to
work over PCI - comes from Oleksii Moisieiev's RFC series `Introduce support
for Raspberry PI 5
<https://patchwork.ozlabs.org/project/uboot/list/?series=442967>`_, which has
never been merged upstream because the device tree bindings are still being
finalised in Linux.  The patches are carried here with their original
authorship; the adaptations on top are described in the commit that follows
them.

Building
--------

.. code-block:: bash

   make rpi5_circle_net_defconfig
   make CROSS_COMPILE=aarch64-linux-gnu-

The SD card is prepared exactly as in :doc:`raspberrypi-circle`, except that
``kernel_2712.img`` does not need to be on it.

TFTP boot
---------

Put ``kernel_2712.img`` in your TFTP root, then::

   U-Boot> setenv serverip 192.168.1.10
   U-Boot> run circle_netboot

``circle_netboot`` runs ``dhcp``, fetches ``${circle_kernel}`` into
``${circle_addr}`` and hands it to ``bootcircle``.  To make it the default::

   U-Boot> setenv bootcmd 'run circle_netboot'
   U-Boot> saveenv

With a static address instead of DHCP::

   U-Boot> setenv ipaddr 192.168.1.50
   U-Boot> setenv serverip 192.168.1.10
   U-Boot> setenv circle_netboot 'if run circle_tftp; then bootcircle ${circle_addr}; fi'
   U-Boot> saveenv

Bringing it up the first time
-----------------------------

The chain has five layers, and a failure in any of them looks the same from
``tftp``.  ``circle_netcheck`` walks all of them::

   U-Boot> run circle_netcheck

Take it one layer at a time:

1. **PCIe link.**  ``pci enum; pci`` must list a device with vendor ``1de4``
   and device ``0001``.  Nothing there means the root complex did not train -
   check ``dm tree`` for ``pcie_brcm``.
2. **RP1 endpoint.**  ``dm tree`` must show ``rp1_driver`` bound and probed,
   with the RP1 children below it.  If the endpoint appears under ``pci`` but
   has no children, the BAR window did not translate; see the BAR ordering
   workaround in ``drivers/mfd/rp1.c``.
3. **Clocks.**  ``clk dump`` should show the RP1 clocks.  The GEM needs
   ``RP1_CLK_SYS``, ``RP1_CLK_ETH`` (125 MHz) and ``RP1_CLK_ETH_TSU``
   (50 MHz).
4. **MDIO and PHY.**  ``mdio list`` must show the bus, and ``mdio read eth0 1
   2`` should return the PHY identifier rather than ``0xffff``.  All ones
   means the PHY is held in reset or the MDIO clock is wrong.
5. **Link and traffic.**  ``net list``, then ``dhcp`` and ``ping
   ${serverip}``.

Why this is a separate config
-----------------------------

Enabling Ethernet means U-Boot brings PCIe and the RP1 up on every boot -
``board_late_init()`` has to probe the RP1 endpoint explicitly, because the
current device tree format does not let the PCI subsystem do it.  Circle then
initialises the same hardware again from a state the firmware did not leave it
in, which is untested.

``rpi5_circle_defconfig`` deliberately touches as little as possible so that
the hand-off matches what Circle sees when the firmware boots it directly.
Keeping the network build separate means choosing the network is an explicit
decision rather than a side effect, and it leaves a known-good configuration to
fall back to when something misbehaves.

``bootcircle`` still removes the active drivers via ``bootm_final()`` before
jumping, and still restores the resident EL3 firmware, so the multi-core fix
applies to both builds.

Known limitations
-----------------

* The RP1 patches are from an unmerged RFC.  The bindings may change before
  they land in Linux, and this will need reworking when they do.
* ``drivers/mfd/rp1.c`` forces the BAR assignment order to match Linux's.
  U-Boot assigns BARs without sorting by size, and the device tree ranges
  assume Linux's layout.  This is a workaround, not a fix.
* The macb binding is matched without a ``macb_config``.  Linux uses
  ``dma_burst_length = 16`` with jumbo and PTP capabilities; that is worth
  revisiting once basic operation is confirmed, but plain ``cdns,macb``
  behaviour is what the RFC exercised.
* USB is behind RP1 too and is not enabled here.

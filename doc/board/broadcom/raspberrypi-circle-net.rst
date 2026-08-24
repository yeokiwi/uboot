.. SPDX-License-Identifier: GPL-2.0+

Ethernet, TFTP and the network console on the Raspberry Pi 5
============================================================

``rpi5_circle_net_defconfig`` adds networking to the Circle setup described in
:doc:`raspberrypi-circle`, so a Circle image can be pulled over TFTP instead of
being copied to the SD card for every build, and so U-Boot can be driven over
the network instead of a serial cable.

Two interfaces are supported and either can do both jobs:

* the **onboard MAC**, a Cadence GEM inside the RP1 southbridge;
* a **USB Ethernet adapter** built on the Realtek RTL8152B, RTL8153A or
  RTL8153B.

``netdev`` selects between them; everything else follows it.

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
             ├ ethernet@40100000 raspberrypi,rp1-gem / cdns,macb
             ├ usb@40200000      snps,dwc3   <- USB 3 ports
             └ usb@40300000      snps,dwc3   <- USB 2 ports

USB hangs off the same endpoint, which is why it is enabled here and was not
before: the two host controllers are ordinary DesignWare USB3 cores on the
RP1's internal bus, so ``CONFIG_USB_XHCI_DWC3`` binds them with no new code -
its reset, clock and PHY setup are all optional, and the firmware device tree
gives the nodes none of those properties.  They are siblings of the GEM under
the same ``pci-ep-bus@1``, so they inherit the same DMA window.

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

Choosing the interface
----------------------

``netdev`` holds the interface number that ``tftp``, ``dhcp``, ``ping`` and the
network console all use.  Two shortcuts set it::

   U-Boot> run net_onboard      # netdev=eth0, the RP1 GEM
   U-Boot> run net_usb          # usb start, then netdev=eth1

Both end by running ``net_pick``, which sets ``ethact`` and pins it with
``ethrotate=no`` so a failure on the chosen interface is reported rather than
silently retried on the other one.

The numbers are a convention, not a guarantee.  The firmware device tree has no
``ethernet0`` alias, so the sequence follows bind order: the GEM binds during
``board_late_init()`` and a USB adapter only at ``usb start``, which makes the
GEM ``eth0`` and the adapter ``eth1`` **whenever both are present**.  If the RP1
never comes up, the adapter becomes ``eth0``.  ``net list`` is the ground truth
and prints ``active`` against the current device::

   U-Boot> usb start
   U-Boot> net list

To have the USB adapter brought up on every boot, without waiting for a
command::

   U-Boot> setenv circle_usbnet 1
   U-Boot> setenv netdev eth1
   U-Boot> saveenv

MAC addresses
~~~~~~~~~~~~~

U-Boot takes the onboard MAC from the VideoCore mailbox and puts it in
``ethaddr``, which applies to interface 0.  A USB adapter at interface 1 uses
``eth1addr`` if set, otherwise the address in its own EEPROM.  Note the
consequence of the numbering above: if the RP1 fails to probe and the adapter
lands at interface 0, U-Boot will program the *onboard* MAC into it.  Set
``eth1addr``, or check ``net list``, if that matters to you.

TFTP boot
---------

Put ``kernel_2712.img`` in your TFTP root, then::

   U-Boot> setenv serverip 192.168.1.10
   U-Boot> run circle_netboot

``circle_netboot`` runs ``net_up`` (which is ``net_pick`` then ``dhcp``),
fetches ``${circle_kernel}`` into ``${circle_addr}`` and hands it to
``bootcircle``.  To make it the default::

   U-Boot> setenv bootcmd 'run circle_netboot'
   U-Boot> saveenv

.. note::

   ``autoload=no`` is not cosmetic.  On the legacy network stack ``dhcp`` will
   otherwise go on to TFTP ``${bootfile}``, fail, and take ``circle_netboot``
   down with it.  It is a default in ``include/configs/rpi.h``, so a *saved*
   environment from an older build will not have it - set it by hand if you
   are upgrading in place.

With a static address instead of DHCP::

   U-Boot> setenv ipaddr 192.168.1.50
   U-Boot> setenv serverip 192.168.1.10
   U-Boot> setenv circle_netboot 'if run net_pick && run circle_tftp; then bootcircle ${circle_addr}; fi'
   U-Boot> saveenv

Network console
---------------

``CONFIG_NETCONSOLE`` adds a stdio device called ``nc`` that carries the U-Boot
console over UDP, in both directions.  It works on whichever interface
``netdev`` selects.

Bring the interface up and attach the console::

   U-Boot> run net_usb            # or: run net_onboard
   U-Boot> setenv ncip 192.168.1.10
   U-Boot> run nc

and on the host::

   $ tools/netconsole 192.168.1.50      # the board's address

``run nc`` is ``run net_up; run nc_on``: the interface has to have an address
before netconsole can send anything, which is why it runs ``dhcp`` first.
Leaving ``ncip`` unset is the easiest way to get first light - netconsole then
broadcasts, which takes ARP out of the equation.  ``run nc_off`` puts the
console back.

``nc_on`` adds ``nc`` to the existing console rather than replacing it, so the
serial port keeps working and a wrong ``ncip`` is not a lockout.  The base it
adds to is ``nc_base``, which defaults to ``serial``; set it to
``serial,vidconsole`` if you are also using the HDMI console.

To start netconsole automatically on every boot::

   U-Boot> setenv circle_netcon 1
   U-Boot> setenv ncip 192.168.1.10
   U-Boot> saveenv

``circle_preboot`` then brings it up before the boot options are evaluated, so
you can interrupt the autoboot from the network console.

.. warning::

   Do not ``saveenv`` with ``nc`` in ``stdout`` directly.  The console is
   assigned early in ``board_r``, long before any Ethernet device exists, so
   netconsole would silently discard all output until the interface came up.
   ``circle_netcon=1`` exists precisely to avoid that - it attaches the console
   at preboot, after the network is running.

.. note::

   Changing ``netdev`` while netconsole is attached does not take effect
   immediately.  Once netconsole has run, U-Boot's network loop stops
   re-selecting the active interface, so the first command after the switch
   still goes out the old one; it corrects itself on the second.  Switch
   cleanly instead::

      U-Boot> run nc_off
      U-Boot> run net_onboard
      U-Boot> run nc

``bootcircle`` puts the console back on the serial port and stops the interface
before handing over, so nothing is left doing DMA - or printing to freed
memory - across the jump into Circle.

USB Ethernet adapters
---------------------

``CONFIG_USB_ETHER_RTL8152`` covers the Realtek parts found in most cheap USB
gigabit dongles: **RTL8152B, RTL8153A and RTL8153B**.

**RTL8156 and RTL8156B - the 2.5GbE parts in newer adapters - are not
supported.**  The driver has neither their USB IDs nor their chip-version
handling, so such an adapter binds to nothing at all: it appears in ``usb
tree`` and is simply absent from ``net list``.  If an adapter's USB ID *is*
known but its silicon revision is not, the driver says so::

   r8152: unknown chip, TCR version 0x....
   r8152: unsupported chip version 0

Bringing it up the first time
-----------------------------

The chain is deep and a failure anywhere in it looks the same from ``tftp``.
``circle_netcheck`` walks the whole thing::

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

For a USB adapter, layers 3 and 4 are replaced by:

3. **USB host controllers.**  ``dm tree`` must show two ``xhci-dwc3`` devices
   below the RP1.  If the nodes are not there at all, the firmware device tree
   did not describe them; if they are bound but ``usb start`` errors, the
   controllers are not clocked or not out of reset.
4. **Enumeration.**  ``usb tree`` must show the adapter and not just the root
   hubs.  Root hubs only means the ports have no VBUS - try the other pair of
   ports, and see `Known limitations`_.

Then ``net list`` must list the adapter.  Present in ``usb tree`` but missing
from ``net list`` means the chip is not one the driver supports; see `USB
Ethernet adapters`_.

Failure signatures
~~~~~~~~~~~~~~~~~~

===================================================  ==============================
Symptom                                              Layer
===================================================  ==============================
No ``1de4`` device under ``pci``                     PCIe did not train
RP1 under ``pci`` with no children                   BAR window did not translate
No ``usb@...`` in ``dm tree``                        device tree / binding
``usb start`` errors out                             USB clock or reset
``usb tree`` shows root hubs only                    VBUS / pinmux
Adapter in ``usb tree``, absent from ``net list``    unsupported Ethernet chip
``mdio read`` returns ``0xffff``                     PHY held in reset
``net list`` fine, ``dhcp`` times out                cable, link or DHCP server
===================================================  ==============================

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

The network build also uses the **legacy** U-Boot network stack rather than
lwIP, because ``CONFIG_NETCONSOLE`` exists only there - on lwIP the symbol is
not even selectable.  The only thing given up is ``wget https``, whose TLS glue
is written against lwIP's TCP; plain ``wget`` and EFI HTTP boot still work.

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
* The RP1 USB controllers have never been exercised by U-Boot on real
  hardware.  They need no clock, reset or PHY properties from the device tree,
  so nothing had to be written for them - but that also means U-Boot leaves
  them in whatever state the firmware's own USB boot scan left them.
* The USB-3 ports' VBUS is pin-muxed.  ``bcm2712-rpi-5-b.dts`` gives
  ``rp1_usb0`` a ``pinctrl-0`` selecting the ``vbus1`` function on gpio42/43,
  and the RP1 GPIO driver here is a plain GPIO driver with no pinctrl uclass
  support, so that mux is never applied.  Failing to apply it is not fatal, and
  the firmware has usually powered the ports already, but if an adapter is not
  detected on one pair of ports try the other - the USB-2 pair has no such
  dependency.
* Only RTL8152B/RTL8153A/RTL8153B USB adapters are supported; RTL8156 and other
  2.5GbE parts are not.

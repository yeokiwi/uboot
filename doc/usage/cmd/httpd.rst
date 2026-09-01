.. SPDX-License-Identifier: GPL-2.0+:

.. index::
   single: httpd (command)

httpd command
=============

Synopsis
--------

::

    httpd [port]

Description
-----------

The httpd command serves a web page from which a file can be written to a
partition on the board's SD card or eMMC.  It is a way to get a kernel, a
boot script or a firmware image onto a board that has neither a card reader
in reach nor a TFTP server on the network - a browser on any machine on the
same network is enough.

The server runs on the currently active network interface, which must already
have an address (from ``dhcp``, or from a static ``ipaddr``).  It keeps the
board to itself until it is stopped, either from the page's *Continue boot*
button or with Ctrl-C on the console.  Both leave the board exactly where the
command was reached from, so a ``preboot`` script can offer the UI and then
carry on booting.

port
    TCP port to listen on.  Defaults to 80.

The page
--------

The page is served from U-Boot itself and fetches nothing from the internet.
It offers:

* the MMC partitions that can be written to, with the filesystem found on
  each one;
* a file chooser that also accepts a dropped file, and the name to save it
  under;
* an upload progress bar, and the result of the write;
* a listing of the chosen partition, to check what is there before and after;
* a *Continue boot* button, which stops the server.

Writing needs write support for the target filesystem, i.e.
``CONFIG_FAT_WRITE`` for the FAT partition that a Raspberry Pi boots from.

The file is uploaded into memory in one piece and only then written, so an
upload is limited by RAM rather than by the card.

Environment
-----------

httpd_addr
    Address of the buffer the upload is received into.  Defaults to
    ``$loadaddr``.  Whatever was at that address is overwritten.

httpd_maxsize
    Largest request accepted, in bytes; anything bigger is refused with
    *413 Payload Too Large* before it is received.  Defaults to 64 MiB.
    It has to cover the file plus a few hundred bytes of form overhead, and
    the region from ``httpd_addr`` to ``httpd_addr + httpd_maxsize`` must be
    memory that nothing else is using.

Example
-------

::

    U-Boot> dhcp
    U-Boot> httpd
    Using eth1 device
    Web UI on http://192.168.1.57/  (upload buffer 64 MiB at 0x1000000)
    Press Ctrl-C to stop the server and carry on booting
    httpd: receiving 8390144 of 8390144 bytes (100%)
    httpd: writing 8389632 bytes to mmc 0:1 as kernel_2712.img
    httpd: wrote 8389632 bytes to mmc 0:1 as kernel_2712.img

The same upload from a script, without a browser::

    $ curl -F iface=mmc -F part=0:1 -F name=kernel_2712.img \
           -F file=@kernel_2712.img http://192.168.1.57/api/upload

Configuration
-------------

The command is available if ``CONFIG_CMD_HTTPD=y``, which needs
``CONFIG_HTTPD=y``.  That in turn requires the legacy network stack
(``CONFIG_NET_LEGACY``, whose ``CONFIG_PROT_TCP`` it selects) and
``CONFIG_CMD_MMC``.

Limitations
-----------

U-Boot's legacy TCP has room for one connection at a time, so the server
answers one request at a time and closes each connection when it is done.
A browser that opens a second connection has its packet dropped and retries;
the page is written to avoid that as far as it can.  It follows that this is
a server for a trusted network and a single user, not one to leave running.

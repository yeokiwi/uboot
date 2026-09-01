// SPDX-License-Identifier: GPL-2.0+
/*
 * "httpd" - serve the storage loader web UI
 *
 * Copyright (C) 2026 U-Boot contributors
 */

#include <command.h>
#include <net.h>
#include <net/httpd.h>
#include <vsprintf.h>

static int do_httpd(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	ulong port = HTTPD_DEFAULT_PORT;

	if (argc > 2)
		return CMD_RET_USAGE;

	if (argc == 2) {
		port = dectoul(argv[1], NULL);
		if (!port || port > 65535) {
			printf("Invalid port '%s'\n", argv[1]);
			return CMD_RET_USAGE;
		}
	}

	httpd_set_port(port);

	/*
	 * net_loop() returns a negative errno when it is interrupted or the
	 * network is not usable.  Neither is a failure of the command: the
	 * server having been stopped is the normal way out of it.
	 */
	if (net_loop(HTTPD) < 0)
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	httpd,	2,	1,	do_httpd,
	"serve a web UI for writing files to SD/eMMC",
	"[port]\n"
	"    - Serve a web page on the current network device (port 80 by\n"
	"      default) that uploads a file into $httpd_addr and writes it to\n"
	"      an MMC partition.  Runs until the page's \"Continue boot\"\n"
	"      button is pressed or Ctrl-C is typed.\n"
	"\n"
	"    $httpd_addr    where the upload is buffered (default $loadaddr)\n"
	"    $httpd_maxsize largest upload accepted, in bytes (default 64 MiB)"
);

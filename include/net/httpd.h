/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * A small HTTP server: a browser is the user interface for writing a file
 * to the board's SD card or eMMC.
 *
 * Copyright (C) 2026 U-Boot contributors
 */

#ifndef __NET_HTTPD_H__
#define __NET_HTTPD_H__

#include <linux/types.h>

/** Default TCP port the server listens on. */
#define HTTPD_DEFAULT_PORT	80

/**
 * @httpd_page: The page the server serves, from net/httpd_page.c
 * @httpd_page_len: Its length, without the terminating NUL
 */
extern const char httpd_page[];
extern const unsigned int httpd_page_len;

/**
 * httpd_set_port() - Choose the TCP port for the next server run
 * @port: port number, 0 selects HTTPD_DEFAULT_PORT
 *
 * Has to be called before net_loop(HTTPD).
 */
void httpd_set_port(u16 port);

/**
 * httpd_get_port() - Port the server listens (or last listened) on
 *
 * Return: TCP port number
 */
u16 httpd_get_port(void);

/**
 * httpd_start_server() - Start listening for HTTP requests
 *
 * Called by net_loop() for the HTTPD protocol; the loop itself does the
 * receiving.  The server runs until the browser asks it to stop ("Continue
 * boot"), Ctrl-C is pressed or a network error occurs.
 */
void httpd_start_server(void);

#endif /* __NET_HTTPD_H__ */

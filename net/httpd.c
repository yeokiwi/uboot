// SPDX-License-Identifier: GPL-2.0+
/*
 * httpd - a browser as the front end for writing a file to SD/eMMC
 *
 * Copyright (C) 2026 U-Boot contributors
 *
 * The server speaks just enough HTTP/1.1 to talk to a browser and answers
 * five endpoints:
 *
 *	GET  /			the single page UI (net/httpd_page.c)
 *	GET  /api/targets	JSON: the MMC partitions a file can go to
 *	GET  /api/list		JSON: directory listing of one of them
 *	POST /api/upload	multipart/form-data, written to storage
 *	POST /api/boot		stop the server and let the boot carry on
 *
 * net/tcp.c holds exactly one struct tcp_stream, so this is a strictly
 * one-connection-at-a-time server: every response says "Connection: close",
 * a second connection arriving mid-request is dropped (the client's SYN
 * retry picks it up), and the page is built to ask for one thing at a time.
 *
 * The whole request - head and body - is received into a buffer in DRAM at
 * $httpd_addr.  TCP segments may arrive out of order, and the rx callback is
 * given the offset of each one within the stream, so writing every segment
 * at its own offset is both the simplest and the most robust arrangement.
 * It also means the uploaded file is already contiguous in memory when the
 * body is complete: fs_write() gets a pointer into that buffer and nothing
 * is copied a second time.
 */

#include <blk.h>
#include <console.h>
#include <env.h>
#include <fs.h>
#include <hexdump.h>
#include <image.h>
#include <mapmem.h>
#include <net.h>
#include <net/httpd.h>
#include <net/tcp.h>
#include <part.h>
#include <vsprintf.h>
#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/string.h>

/* Longest request head (request line plus headers) we will parse */
#define HTTPD_HEAD_MAX		2048
/*
 * Response head; the bodies live elsewhere.  Big enough for a "100 Continue"
 * and the real head after it, which are both built from string literals in
 * this file and so have a known length.
 */
#define HTTPD_RESP_MAX		512
/* Buffer for the generated JSON bodies */
#define HTTPD_JSON_MAX		4096
/* Request path, including the query string */
#define HTTPD_PATH_MAX		192
/* multipart/form-data boundary, RFC 2046 allows up to 70 characters */
#define HTTPD_BOUNDARY_MAX	80
/* Destination file name */
#define HTTPD_NAME_MAX		128
/* Default cap on a request, i.e. on the file that can be uploaded */
#define HTTPD_SIZE_MAX_DEF	SZ_64M
/* How far to look for MMC devices and their partitions */
#define HTTPD_MMC_DEVS		8
#define HTTPD_MMC_PARTS		16
/* Entries returned by /api/list */
#define HTTPD_LIST_MAX		256
/*
 * How long a connection may say nothing before it is dropped.  net/tcp.c's
 * 30 second default is meant for a client U-Boot is talking to on purpose;
 * here an idle connection is usually a browser that opened one speculatively
 * and never used it, and because there is only one stream to go round it
 * blocks every real request until it goes away.
 */
#define HTTPD_IDLE_TIMEOUT	5000UL

/**
 * struct httpd_state - everything about the connection being served
 *
 * @head_done:	the request head has been parsed
 * @replied:	a response has been queued; further request data is ignored
 * @too_big:	the request outgrew the receive buffer
 * @is_post:	the request method is POST
 * @head_len:	length of the request head, i.e. offset of the body
 * @body_len:	value of the Content-Length header
 * @path:	request path, query string stripped off
 * @query:	query string, or NULL
 * @boundary:	multipart/form-data boundary from Content-Type
 * @resp:	response head; a 100-continue may already be sitting in front
 * @resp_len:	bytes used in @resp
 * @body:	response body, not owned
 * @body_out:	length of @body
 * @percent:	last upload percentage printed on the console
 */
struct httpd_state {
	bool		head_done;
	bool		replied;
	bool		too_big;
	bool		is_post;
	u32		head_len;
	u32		body_len;
	char		path[HTTPD_PATH_MAX];
	char		*query;
	char		boundary[HTTPD_BOUNDARY_MAX];
	char		resp[HTTPD_RESP_MAX];
	u32		resp_len;
	const char	*body;
	u32		body_out;
	int		percent;
};

static struct httpd_state state;
static char httpd_head[HTTPD_HEAD_MAX];
static char httpd_json[HTTPD_JSON_MAX];
static u32 httpd_json_len;
static bool httpd_json_full;

static u16 httpd_port = HTTPD_DEFAULT_PORT;
static ulong httpd_addr;	/* receive buffer, a DRAM address */
static ulong httpd_size;	/* and its size */
static bool httpd_stop;		/* the page asked us to resume booting */

void httpd_set_port(u16 port)
{
	httpd_port = port ? port : HTTPD_DEFAULT_PORT;
}

u16 httpd_get_port(void)
{
	return httpd_port;
}

/*
 * Body handling helpers.
 */

/* memmem() is not in U-Boot's string library */
static char *httpd_memmem(const char *hay, u32 hay_len,
			  const char *needle, u32 needle_len)
{
	const char *end;

	if (!needle_len || hay_len < needle_len)
		return NULL;

	end = hay + hay_len - needle_len;
	for (; hay <= end; hay++) {
		if (*hay == *needle && !memcmp(hay, needle, needle_len))
			return (char *)hay;
	}

	return NULL;
}

/* Copy @len bytes of @src, which is not NUL terminated, into @dst */
static void httpd_copy_field(char *dst, size_t dst_size, const char *src,
			     u32 len)
{
	if (len >= dst_size)
		len = dst_size - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

/* Map the request buffer; on every supported board this is a no-op */
static char *httpd_buf(u32 offset, u32 len)
{
	return map_sysmem(httpd_addr + offset, len);
}

/*
 * JSON output.  Bodies are built in one static buffer; a listing that does
 * not fit is truncated rather than sent in pieces, and says so.
 */

static void json_reset(void)
{
	httpd_json_len = 0;
	httpd_json_full = false;
	httpd_json[0] = '\0';
}

static __printf(1, 2) void json_add(const char *fmt, ...)
{
	va_list args;
	int n, room = sizeof(httpd_json) - httpd_json_len;

	if (httpd_json_full || room <= 1)
		return;

	va_start(args, fmt);
	n = vsnprintf(httpd_json + httpd_json_len, room, fmt, args);
	va_end(args);

	if (n < 0 || n >= room) {
		httpd_json_full = true;
		httpd_json[httpd_json_len] = '\0';
		return;
	}
	httpd_json_len += n;
}

static void json_add_char(char c)
{
	if (httpd_json_full || httpd_json_len + 1 >= sizeof(httpd_json)) {
		httpd_json_full = true;
		return;
	}
	httpd_json[httpd_json_len++] = c;
	httpd_json[httpd_json_len] = '\0';
}

/* Add @str as a quoted JSON string, escaping what JSON does not allow */
static void json_add_str(const char *str)
{
	json_add_char('"');
	for (; *str; str++) {
		unsigned char c = *str;

		if (c == '"' || c == '\\') {
			json_add_char('\\');
			json_add_char(c);
		} else if (c < 0x20 || c == 0x7f) {
			json_add("\\u%04x", c);
		} else {
			json_add_char(c);
		}
	}
	json_add_char('"');
}

/*
 * Responses.  A queued response is a head in state.resp followed by a body
 * that lives somewhere stable (the page in rodata, or the JSON buffer), so
 * nothing has to be copied for the transmit callback.
 */

static void httpd_respond(const char *status, const char *type,
			  const char *body, u32 body_len)
{
	int room = sizeof(state.resp) - state.resp_len;
	int n;

	n = snprintf(state.resp + state.resp_len, room,
		     "HTTP/1.1 %s\r\n"
		     "Server: U-Boot\r\n"
		     "Content-Type: %s\r\n"
		     "Content-Length: %u\r\n"
		     "Cache-Control: no-store\r\n"
		     "Connection: close\r\n"
		     "\r\n", status, type, body_len);
	if (n > 0 && n < room)
		state.resp_len += n;

	state.body = body;
	state.body_out = body_len;
	state.replied = true;
}

static void httpd_respond_json(const char *status)
{
	httpd_respond(status, "application/json", httpd_json, httpd_json_len);
}

/* An error, both for the browser (JSON) and for whoever watches the console */
static __printf(2, 3) void httpd_error(const char *status,
				       const char *fmt, ...)
{
	char msg[128];
	va_list args;

	va_start(args, fmt);
	vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);

	printf("httpd: %s: %s\n", status, msg);

	json_reset();
	json_add("{\"ok\":false,\"error\":");
	json_add_str(msg);
	json_add("}");
	httpd_respond_json(status);
}

/*
 * Request parsing.
 */

/* Decode %xx and '+' in place, as found in a query string */
static void httpd_url_decode(char *str)
{
	char *out = str;

	for (; *str; str++) {
		if (*str == '+') {
			*out++ = ' ';
		} else if (*str == '%' && isxdigit(str[1]) && isxdigit(str[2])) {
			*out++ = (hex_to_bin(str[1]) << 4) | hex_to_bin(str[2]);
			str += 2;
		} else {
			*out++ = *str;
		}
	}
	*out = '\0';
}

/**
 * httpd_query_arg() - Look up one argument of the request's query string
 * @name: argument name
 * @out: where to copy the (URL-decoded) value
 * @out_len: size of @out
 *
 * Return: true if the argument was present
 */
static bool httpd_query_arg(const char *name, char *out, size_t out_len)
{
	size_t name_len = strlen(name);
	const char *p = state.query;

	*out = '\0';
	while (p && *p) {
		const char *end = strchrnul(p, '&');

		if (!strncmp(p, name, name_len) && p[name_len] == '=') {
			size_t len = end - p - name_len - 1;

			if (len >= out_len)
				len = out_len - 1;
			memcpy(out, p + name_len + 1, len);
			out[len] = '\0';
			httpd_url_decode(out);
			return true;
		}
		p = *end ? end + 1 : NULL;
	}

	return false;
}

/* Pull the value of a "key=value" or "key=\"value\"" parameter out of a header */
static void httpd_hdr_param(const char *hdr, const char *key,
			    char *out, size_t out_len)
{
	const char *p = strstr(hdr, key);
	size_t len = 0;

	*out = '\0';
	if (!p)
		return;
	p += strlen(key);

	if (*p == '"') {
		p++;
		while (p[len] && p[len] != '"')
			len++;
	} else {
		while (p[len] && p[len] != ';' && p[len] != ' ' &&
		       p[len] != '\r' && p[len] != '\n')
			len++;
	}

	if (len >= out_len)
		len = out_len - 1;
	memcpy(out, p, len);
	out[len] = '\0';
}

/**
 * httpd_parse_head() - Parse the request head once it is complete
 * @rx_bytes: contiguous bytes received so far
 *
 * Return: true once the head has been parsed or answered with an error,
 *	   false while more data is needed
 */
static bool httpd_parse_head(u32 rx_bytes)
{
	u32 scan = min_t(u32, rx_bytes, (u32)HTTPD_HEAD_MAX);
	char *buf = httpd_buf(0, scan);
	char *line, *next, *end, *sp;
	bool first = true;

	end = httpd_memmem(buf, scan, "\r\n\r\n", 4);
	if (!end) {
		if (rx_bytes >= HTTPD_HEAD_MAX)
			httpd_error("431 Request Header Fields Too Large",
				    "request head is longer than %d bytes",
				    HTTPD_HEAD_MAX);
		return state.replied;
	}

	state.head_len = end - buf + 4;
	memcpy(httpd_head, buf, state.head_len - 4);
	httpd_head[state.head_len - 4] = '\0';

	for (line = httpd_head; line && *line; line = next) {
		next = strstr(line, "\r\n");
		if (next) {
			*next = '\0';
			next += 2;
		}

		if (first) {
			first = false;
			sp = strchr(line, ' ');
			if (!sp) {
				httpd_error("400 Bad Request",
					    "malformed request line");
				return true;
			}
			*sp++ = '\0';
			state.is_post = !strcmp(line, "POST");
			if (!state.is_post && strcmp(line, "GET")) {
				httpd_error("405 Method Not Allowed",
					    "%s is not supported", line);
				return true;
			}
			line = sp;
			sp = strchr(line, ' ');
			if (sp)
				*sp = '\0';
			strlcpy(state.path, line, sizeof(state.path));
			state.query = strchr(state.path, '?');
			if (state.query)
				*state.query++ = '\0';
			httpd_url_decode(state.path);
			continue;
		}

		if (!strncasecmp(line, "Content-Length:", 15)) {
			state.body_len = simple_strtoul(line + 15, NULL, 10);
		} else if (!strncasecmp(line, "Content-Type:", 13)) {
			httpd_hdr_param(line, "boundary=", state.boundary,
					sizeof(state.boundary));
		} else if (!strncasecmp(line, "Transfer-Encoding:", 18) &&
			   strstr(line, "chunked")) {
			httpd_error("411 Length Required",
				    "a chunked body cannot be received");
			return true;
		} else if (!strncasecmp(line, "Expect:", 7) &&
			   strstr(line, "100-continue")) {
			/*
			 * curl waits for this before sending a large body.
			 * It goes out in front of the real response, which is
			 * exactly where appending it to state.resp puts it.
			 */
			state.resp_len = snprintf(state.resp, sizeof(state.resp),
						  "HTTP/1.1 100 Continue\r\n\r\n");
		}
	}

	if (state.head_len + state.body_len > httpd_size) {
		httpd_error("413 Payload Too Large",
			    "request is larger than httpd_maxsize (%lu bytes)",
			    httpd_size);
		return true;
	}

	state.head_done = true;

	return true;
}

/*
 * Endpoints.
 */

static void httpd_get_page(void)
{
	httpd_respond("200 OK", "text/html; charset=utf-8",
		      httpd_page, httpd_page_len);
}

/* Name of the filesystem on a partition, or NULL if there is none we know */
static const char *httpd_probe_fs(const char *iface, const char *part)
{
	const char *name;

	if (fs_set_blk_dev(iface, part, FS_TYPE_ANY))
		return NULL;

	name = fs_get_type_name();
	fs_close();

	return (name && strcmp(name, "unsupported")) ? name : NULL;
}

static void httpd_add_target(const char *iface, const char *id,
			     const char *name, u64 bytes, bool *first)
{
	const char *fs = httpd_probe_fs(iface, id);

	json_add("%s{\"iface\":\"%s\",\"id\":\"%s\",\"bytes\":%llu,\"fs\":",
		 *first ? "" : ",", iface, id, bytes);
	if (fs)
		json_add_str(fs);
	else
		json_add("null");
	json_add(",\"name\":");
	json_add_str(name);
	json_add("}");
	*first = false;
}

/* The partitions of every MMC device a file could be written to */
static void httpd_get_targets(void)
{
	struct disk_partition info;
	struct blk_desc *desc;
	char id[16];
	bool first = true;
	int dev, part;

	json_reset();
	json_add("{\"ok\":true,\"ip\":\"%pI4\",\"targets\":[", &net_ip);

	for (dev = 0; dev < HTTPD_MMC_DEVS; dev++) {
		bool any = false;

		desc = blk_get_devnum_by_uclass_id(UCLASS_MMC, dev);
		if (!desc || desc->type == DEV_TYPE_UNKNOWN)
			continue;

		for (part = 1; part <= HTTPD_MMC_PARTS; part++) {
			if (part_get_info(desc, part, &info))
				continue;
			snprintf(id, sizeof(id), "%d:%d", dev, part);
			httpd_add_target("mmc", id, (char *)info.name,
					 (u64)info.size * info.blksz, &first);
			any = true;
		}

		/* An unpartitioned card can still hold a filesystem */
		if (!any) {
			snprintf(id, sizeof(id), "%d:0", dev);
			if (httpd_probe_fs("mmc", id))
				httpd_add_target("mmc", id, "whole device",
						 (u64)desc->lba * desc->blksz,
						 &first);
		}
	}

	json_add("]}");
	httpd_respond_json("200 OK");
}

/* Contents of one directory on one target */
static void httpd_get_list(void)
{
	char iface[16], part[16], dir[HTTPD_NAME_MAX];
	struct fs_dir_stream *dirs;
	struct fs_dirent *dent;
	bool truncated = false;
	int count = 0;

	if (!httpd_query_arg("iface", iface, sizeof(iface)) ||
	    !httpd_query_arg("part", part, sizeof(part))) {
		httpd_error("400 Bad Request", "iface and part are required");
		return;
	}
	if (!httpd_query_arg("dir", dir, sizeof(dir)))
		strcpy(dir, "/");

	if (fs_set_blk_dev(iface, part, FS_TYPE_ANY)) {
		httpd_error("404 Not Found", "no filesystem on %s %s",
			    iface, part);
		return;
	}

	dirs = fs_opendir(dir);
	if (!dirs) {
		httpd_error("404 Not Found", "cannot read %s on %s %s",
			    dir, iface, part);
		return;
	}

	json_reset();
	json_add("{\"ok\":true,\"entries\":[");
	while ((dent = fs_readdir(dirs))) {
		/*
		 * Stop before an entry that might not fit: a half written
		 * entry would not be JSON at all.  Every character of a name
		 * can escape to six.
		 */
		if (count >= HTTPD_LIST_MAX ||
		    httpd_json_len + 6 * strlen(dent->name) + 64 >
		    sizeof(httpd_json) - 32) {
			truncated = true;
			break;
		}
		json_add("%s{\"name\":", count ? "," : "");
		json_add_str(dent->name);
		json_add(",\"dir\":%s,\"size\":%llu}",
			 dent->type == FS_DT_DIR ? "true" : "false",
			 (u64)dent->size);
		count++;
	}
	fs_closedir(dirs);

	json_add("],\"truncated\":%s}", truncated ? "true" : "false");
	httpd_respond_json("200 OK");
}

/**
 * httpd_write_file() - Write the uploaded data to storage
 * @iface: block interface, always "mmc" from the page
 * @part: "dev:part"
 * @name: destination file name
 * @offset: where the data starts in the receive buffer
 * @len: how much of it there is
 * @tcp: stream to keep alive across the write
 *
 * Return: 0 on success
 */
static int httpd_write_file(const char *iface, const char *part,
			    const char *name, u32 offset, u32 len,
			    struct tcp_stream *tcp)
{
	loff_t written = 0;
	int ret;

	if (fs_set_blk_dev(iface, part, FS_TYPE_ANY)) {
		httpd_error("404 Not Found", "no filesystem on %s %s",
			    iface, part);
		return -ENODEV;
	}

	printf("httpd: writing %u bytes to %s %s as %s\n", len, iface, part,
	       name);

	/*
	 * Writing megabytes to an SD card takes far longer than a packet
	 * round trip, and nothing is received meanwhile.  Tell TCP that the
	 * silence is ours, not the network's, or the stream is dropped as
	 * inactive underneath the reply.
	 */
	tcp_stream_restart_rx_timer(tcp);
	ret = fs_write(name, httpd_addr + offset, 0, len, &written);
	tcp_stream_restart_rx_timer(tcp);

	if (ret < 0) {
		httpd_error("500 Internal Server Error",
			    "writing %s failed (%d)", name, ret);
		return ret;
	}
	if (written != len) {
		httpd_error("507 Insufficient Storage",
			    "only %llu of %u bytes were written",
			    (u64)written, len);
		return -ENOSPC;
	}

	json_reset();
	json_add("{\"ok\":true,\"bytes\":%llu,\"iface\":\"%s\",\"part\":\"%s\","
		 "\"name\":", (u64)written, iface, part);
	json_add_str(name);
	json_add("}");
	httpd_respond_json("200 OK");

	printf("httpd: wrote %llu bytes to %s %s as %s\n", (u64)written, iface,
	       part, name);

	return 0;
}

/* Reject anything that would escape the target, or upset a filesystem */
static bool httpd_name_ok(const char *name)
{
	const char *p;

	if (!*name || strlen(name) >= HTTPD_NAME_MAX)
		return false;
	if (strstr(name, "..") || strchr(name, '\\'))
		return false;
	for (p = name; *p; p++) {
		if ((unsigned char)*p < 0x20 || *p == 0x7f || *p == '"')
			return false;
	}

	return true;
}

/**
 * httpd_post_upload() - Take the file out of a multipart/form-data body
 * @tcp: the stream the body arrived on
 *
 * The parts are walked in place in the receive buffer: the small form fields
 * are copied out, and the file is left where it is so that it can be written
 * from there.
 */
static void httpd_post_upload(struct tcp_stream *tcp)
{
	char iface[16] = "mmc", part[16] = "", name[HTTPD_NAME_MAX] = "";
	char sep[HTTPD_BOUNDARY_MAX + 8], head[192], field[64], fname[HTTPD_NAME_MAX];
	char *body, *p, *body_end, *hdr_end, *data, *data_end;
	u32 file_offs = 0, file_len = 0;
	int sep_len;

	if (!state.boundary[0]) {
		httpd_error("400 Bad Request",
			    "not a multipart/form-data upload");
		return;
	}

	body = httpd_buf(state.head_len, state.body_len);
	body_end = body + state.body_len;

	/* Parts are separated by CRLF, "--" and the boundary */
	sep_len = snprintf(sep, sizeof(sep), "\r\n--%s", state.boundary);

	/* The first one has no CRLF in front of it */
	p = httpd_memmem(body, state.body_len, sep + 2, sep_len - 2);
	if (!p) {
		httpd_error("400 Bad Request", "no form data in the request");
		return;
	}
	p += sep_len - 2;

	while (p + 2 <= body_end) {
		if (p[0] == '-' && p[1] == '-')	/* closing delimiter */
			break;
		if (p[0] != '\r' || p[1] != '\n') {
			httpd_error("400 Bad Request", "malformed form data");
			return;
		}
		p += 2;

		hdr_end = httpd_memmem(p, body_end - p, "\r\n\r\n", 4);
		if (!hdr_end) {
			httpd_error("400 Bad Request", "truncated form part");
			return;
		}
		data = hdr_end + 4;

		data_end = httpd_memmem(data, body_end - data, sep, sep_len);
		if (!data_end) {
			httpd_error("400 Bad Request", "truncated form data");
			return;
		}

		/* Part headers are short; a longer one is not ours */
		if (hdr_end - p < (int)sizeof(head)) {
			memcpy(head, p, hdr_end - p);
			head[hdr_end - p] = '\0';
			httpd_hdr_param(head, "name=", field, sizeof(field));
			httpd_hdr_param(head, "filename=", fname, sizeof(fname));

			if (!strcmp(field, "file") || fname[0]) {
				file_offs = state.head_len + (data - body);
				file_len = data_end - data;
			} else if (!strcmp(field, "iface")) {
				httpd_copy_field(iface, sizeof(iface), data,
						 data_end - data);
			} else if (!strcmp(field, "part")) {
				httpd_copy_field(part, sizeof(part), data,
						 data_end - data);
			} else if (!strcmp(field, "name")) {
				httpd_copy_field(name, sizeof(name), data,
						 data_end - data);
			}
		}

		p = data_end + sep_len;
	}

	if (!file_len) {
		httpd_error("400 Bad Request", "no file in the request");
		return;
	}
	if (!part[0]) {
		httpd_error("400 Bad Request", "no target partition given");
		return;
	}
	if (!name[0])
		strlcpy(name, fname, sizeof(name));
	if (!httpd_name_ok(name)) {
		httpd_error("400 Bad Request", "unusable destination name");
		return;
	}

	httpd_write_file(iface, part, name, file_offs, file_len, tcp);
}

static void httpd_post_boot(void)
{
	httpd_stop = true;

	json_reset();
	json_add("{\"ok\":true,\"booting\":true}");
	httpd_respond_json("200 OK");

	printf("httpd: resuming the boot on request from the browser\n");
}

static void httpd_dispatch(struct tcp_stream *tcp)
{
	const char *path = state.path;

	if (state.is_post) {
		if (!strcmp(path, "/api/upload"))
			httpd_post_upload(tcp);
		else if (!strcmp(path, "/api/boot"))
			httpd_post_boot();
		else
			httpd_error("404 Not Found", "no such endpoint: %s",
				    path);
		return;
	}

	if (!strcmp(path, "/") || !strcmp(path, "/index.html"))
		httpd_get_page();
	else if (!strcmp(path, "/api/targets"))
		httpd_get_targets();
	else if (!strcmp(path, "/api/list"))
		httpd_get_list();
	else
		httpd_error("404 Not Found", "no such path: %s", path);
}

/*
 * TCP callbacks.
 */

static int httpd_rx(struct tcp_stream *tcp, u32 rx_offs, void *buf, int len)
{
	if (state.replied)	/* answered already, swallow the remainder */
		return len;

	if (rx_offs + len > httpd_size) {
		state.too_big = true;
		return len;
	}

	memcpy(httpd_buf(rx_offs, len), buf, len);

	return len;
}

/* Report progress of a long upload, which is otherwise a silent minute */
static void httpd_progress(u32 got)
{
	int percent = state.body_len ? (int)((u64)got * 100 / state.body_len) : 0;

	if (percent / 10 == state.percent / 10)
		return;

	state.percent = percent;
	printf("\rhttpd: receiving %u of %u bytes (%d%%)", got, state.body_len,
	       percent);
	if (percent >= 100)
		printf("\n");
}

static void httpd_rcv_nxt_update(struct tcp_stream *tcp, u32 rx_bytes)
{
	if (state.replied)
		return;

	if (state.too_big) {
		httpd_error("413 Payload Too Large",
			    "request is larger than httpd_maxsize (%lu bytes)",
			    httpd_size);
		return;
	}

	if (!state.head_done && !httpd_parse_head(rx_bytes))
		return;
	if (!state.head_done || state.replied)
		return;

	if (rx_bytes < state.head_len + state.body_len) {
		httpd_progress(rx_bytes - state.head_len);
		return;
	}
	if (state.body_len)
		httpd_progress(state.body_len);

	httpd_dispatch(tcp);
}

static int httpd_tx(struct tcp_stream *tcp, u32 tx_offs, void *buf, int maxlen)
{
	u32 total = state.resp_len + state.body_out;
	int len = 0;

	while (maxlen > 0 && tx_offs < total) {
		const char *src;
		u32 chunk;

		if (tx_offs < state.resp_len) {
			src = state.resp + tx_offs;
			chunk = state.resp_len - tx_offs;
		} else {
			src = state.body + (tx_offs - state.resp_len);
			chunk = total - tx_offs;
		}
		if (chunk > (u32)maxlen)
			chunk = maxlen;

		memcpy((char *)buf + len, src, chunk);
		len += chunk;
		tx_offs += chunk;
		maxlen -= chunk;
	}

	return len;
}

static void httpd_snd_una_update(struct tcp_stream *tcp, u32 tx_bytes)
{
	/* The whole response is out and acknowledged: nothing more to say */
	if (state.replied && tx_bytes >= state.resp_len + state.body_out)
		tcp_stream_close(tcp);
}

static void httpd_closed(struct tcp_stream *tcp)
{
	if (httpd_stop)
		net_set_state(NETLOOP_SUCCESS);
}

static int httpd_on_create(struct tcp_stream *tcp)
{
	if (tcp->lport != httpd_port)
		return 0;

	memset(&state, 0, sizeof(state));
	state.percent = -100;

	tcp->rx = httpd_rx;
	tcp->tx = httpd_tx;
	tcp->on_rcv_nxt_update = httpd_rcv_nxt_update;
	tcp->on_snd_una_update = httpd_snd_una_update;
	tcp->on_closed = httpd_closed;
	tcp->rx_inactiv_timeout = HTTPD_IDLE_TIMEOUT;

	return 1;
}

void httpd_start_server(void)
{
	char url[48];

	httpd_addr = env_get_hex("httpd_addr", image_load_addr);
	httpd_size = env_get_hex("httpd_maxsize", HTTPD_SIZE_MAX_DEF);
	httpd_stop = false;
	memset(&state, 0, sizeof(state));

	tcp_stream_set_on_create_handler(httpd_on_create);

	if (httpd_port == HTTPD_DEFAULT_PORT)
		snprintf(url, sizeof(url), "http://%pI4/", &net_ip);
	else
		snprintf(url, sizeof(url), "http://%pI4:%u/", &net_ip,
			 httpd_port);

	printf("Using %s device\n", eth_get_name());
	printf("Web UI on %s  (upload buffer %lu MiB at %#lx)\n", url,
	       httpd_size >> 20, httpd_addr);
	printf("Press Ctrl-C to stop the server and carry on booting\n");
}

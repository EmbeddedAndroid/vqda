/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * Minimal synchronous xenstore client for vqda: XS_READ and XS_WRITE over
 * the xenstore wire protocol, either on /dev/xen/xenbus (a guest) or on
 * xenstored's unix socket (the domain that runs xenstored). One request
 * in flight per connection; no transactions, no watches (callers poll).
 */
#ifndef VQDA_XS_H
#define VQDA_XS_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define XS_READ		2
#define XS_WRITE	11
#define XS_ERROR	16

struct xsd_sockmsg {
	uint32_t type, req_id, tx_id, len;
};

static inline int xs_open(void)
{
	static const char *const socks[] = {
		"/run/xenstored/socket", "/var/run/xenstored/socket",
	};
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	unsigned int i;
	int fd;

	fd = open("/dev/xen/xenbus", O_RDWR | O_CLOEXEC);
	if (fd >= 0)
		return fd;
	for (i = 0; i < sizeof(socks) / sizeof(socks[0]); i++) {
		fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
			return -errno;
		strncpy(a.sun_path, socks[i], sizeof(a.sun_path) - 1);
		if (!connect(fd, (struct sockaddr *)&a, sizeof(a)))
			return fd;
		close(fd);
	}
	return -ENOENT;
}

static inline int xs_io(int fd, int w, void *b, size_t n)
{
	char *p = b;

	while (n) {
		ssize_t r = w ? write(fd, p, n) : read(fd, p, n);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return r ? -errno : -EIO;
		p += r;
		n -= r;
	}
	return 0;
}

/* Returns the reply length (reply NUL-terminated in @out) or -errno. */
static inline int xs_req(int fd, uint32_t type, const void *a, size_t alen,
		  const void *b, size_t blen, char *out, size_t outlen)
{
	static uint32_t id;
	struct xsd_sockmsg m = { .type = type, .req_id = ++id,
				 .len = alen + blen };
	char *buf;
	int ret;

	if (xs_io(fd, 1, &m, sizeof(m)) || xs_io(fd, 1, (void *)a, alen) ||
	    (blen && xs_io(fd, 1, (void *)b, blen)))
		return -EIO;
	if (xs_io(fd, 0, &m, sizeof(m)) || m.len > 65536)
		return -EIO;
	buf = malloc(m.len + 1);
	if (!buf)
		return -ENOMEM;
	if (xs_io(fd, 0, buf, m.len)) {
		free(buf);
		return -EIO;
	}
	buf[m.len] = 0;
	if (m.type == XS_ERROR) {
		ret = !strncmp(buf, "ENOENT", 6) ? -ENOENT :
		      !strncmp(buf, "EACCES", 6) ? -EACCES : -EIO;
	} else {
		ret = m.len;
		if (out && outlen) {
			size_t n = m.len < outlen - 1 ? m.len : outlen - 1;

			memcpy(out, buf, n);
			out[n] = 0;
		}
	}
	free(buf);
	return ret;
}

static inline int xs_read(int fd, const char *path, char *out, size_t outlen)
{
	return xs_req(fd, XS_READ, path, strlen(path) + 1, NULL, 0, out,
		      outlen);
}

static inline int xs_write(int fd, const char *path, const char *val)
{
	int r = xs_req(fd, XS_WRITE, path, strlen(path) + 1, val, strlen(val),
		       NULL, 0);

	return r < 0 ? r : 0;
}

#endif

// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * vqda_xs.h against a scripted xenstored on a socketpair: replies,
 * truncation, error mapping, oversized and short replies.
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include "vqda_xs.h"

struct reply {
	uint32_t type;
	const char *body;
	uint32_t len;		/* sent in the header; body bytes sent if <= 64 KiB */
	int hangup;		/* close instead of answering */
};

static const struct reply script[] = {
	{ XS_READ, "hello", 5, 0 },
	{ XS_READ, "hello", 5, 0 },
	{ XS_ERROR, "ENOENT", 7, 0 },
	{ XS_ERROR, "EACCES", 7, 0 },
	{ XS_ERROR, "EINVAL", 7, 0 },
	{ XS_WRITE, "OK", 3, 0 },
	{ XS_READ, "", 65537, 0 },
	{ 0, NULL, 0, 1 },
};
static char last_req[256];
static uint32_t last_len, last_type;

static void *server(void *arg)
{
	int fd = (int)(intptr_t)arg;
	unsigned int i;

	for (i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
		struct xsd_sockmsg m;

		if (xs_io(fd, 0, &m, sizeof(m)) || m.len > sizeof(last_req) ||
		    xs_io(fd, 0, last_req, m.len))
			break;
		last_len = m.len;
		last_type = m.type;
		if (script[i].hangup)
			break;
		m.type = script[i].type;
		m.len = script[i].len;
		if (xs_io(fd, 1, &m, sizeof(m)))
			break;
		if (m.len <= 65536 && xs_io(fd, 1, (void *)script[i].body, m.len))
			break;
	}
	close(fd);
	return NULL;
}

static int fails;

static void expect(const char *what, int ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	fails += !ok;
}

int main(void)
{
	char out[16];
	pthread_t t;
	int sv[2], r;

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);	/* the last case writes to a closed peer */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		return 2;
	pthread_create(&t, NULL, server, (void *)(intptr_t)sv[1]);

	r = xs_read(sv[0], "a/b", out, sizeof(out));
	expect("read returns the length and the value",
	       r == 5 && !strcmp(out, "hello") && last_type == XS_READ &&
	       last_len == 4 && !memcmp(last_req, "a/b", 4));
	r = xs_read(sv[0], "a/b", out, 3);
	expect("read truncates to the buffer", r == 5 && !strcmp(out, "he"));
	expect("ENOENT", xs_read(sv[0], "x", out, sizeof(out)) == -ENOENT);
	expect("EACCES", xs_read(sv[0], "x", out, sizeof(out)) == -EACCES);
	expect("other errors are EIO", xs_read(sv[0], "x", out, sizeof(out)) == -EIO);
	r = xs_write(sv[0], "p", "v1");
	expect("write sends path NUL value", r == 0 && last_type == XS_WRITE &&
	       last_len == 4 && !memcmp(last_req, "p\0v1", 4));
	expect("oversized reply is refused",
	       xs_read(sv[0], "x", out, sizeof(out)) == -EIO);
	expect("closed connection is EIO",
	       xs_read(sv[0], "x", out, sizeof(out)) == -EIO);
	pthread_join(t, NULL);
	close(sv[0]);
	return fails ? 1 : 0;
}

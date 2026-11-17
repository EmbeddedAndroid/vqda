// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * vqda-fuzz [iterations] [seed]: what a hostile guest can send its back
 * end, through vqda_raw (no guest library checks): random ops, session
 * ids, handles, sizes, offsets, lengths and argument counts, mostly on a
 * real session (unsigned PD, real buffers) so requests reach the host's
 * QDA checks; then an open/close storm. A ping after every 1000 requests
 * proves the back end still answers. One summary line:
 * "FUZZ n <n> refused <r> ok <o> pings <p> FAIL|PASS".
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "vqda_proto.h"

int vqda_raw(uint32_t op, uint32_t sid, void *slot);
int vqda_sid(int fd);
int vqda_ping(void);

static uint64_t rnd(void)
{
	return ((uint64_t)random() << 33) ^ ((uint64_t)random() << 2) ^ (uint64_t)random();
}

static uint64_t pick(void)
{
	static const uint64_t edge[] = { 0, 1, 4095, 4096, 4097, 1ull << 31,
					 1ull << 32, ~0ull, ~0ull - 4095, 0x7fffffff };

	return random() & 1 ? edge[random() % 10] : rnd() >> (random() % 64);
}

int main(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 20000, i, refused = 0, ok = 0, pings = 0;
	struct drm_qda_init_create ic = { .attrs = QDA_INIT_ATTR_UNSIGNED_MODULE };
	struct drm_qda_gem_create g = { .size = 65536 };
	struct vqda_slot s;
	int fd, sid, ret, k;

	srandom(argc > 2 ? atoi(argv[2]) : 1);
	fd = open("/dev/accel/accel0", O_RDWR);
	if (fd < 0 || ioctl(fd, DRM_IOCTL_QDA_REMOTE_SESSION_CREATE, &ic) ||
	    ioctl(fd, DRM_IOCTL_QDA_GEM_CREATE, &g) || (sid = vqda_sid(fd)) < 0) {
		printf("FUZZ setup FAIL errno %d\n", errno);
		return 2;
	}
	for (i = 0; i < n; i++) {
		uint32_t op = random() % 3 ? random() % VQDA_OP_LAST + 1 : random() % 256;
		uint32_t xs = random() % 4 ? (uint32_t)sid : (uint32_t)(random() % 80);
		uint32_t *w = (uint32_t *)&s.u;

		for (k = 0; k < (int)(sizeof(s.u) / 4); k++)
			w[k] = random() % 3 ? 0 : (uint32_t)pick();
		switch (op) {
		case VQDA_OP_INVOKE:
			s.u.invoke.handle = random() % 4;
			s.u.invoke.nargs = random() % 3 ? random() % 8 : (uint32_t)pick();
			s.u.invoke.sc = random() % 2 ? (s.u.invoke.nargs << 16) | ((random() % 32) << 24) : (uint32_t)pick();
			for (k = 0; k < 8; k++) {
				s.u.invoke.args[k].handle = random() % 2 ? g.handle : (uint32_t)pick();
				s.u.invoke.args[k].offset = pick();
				s.u.invoke.args[k].length = pick();
			}
			break;
		case VQDA_OP_HEAP_ALLOC:
			/* mostly small and sane, so imports and frees find real ids */
			s.u.heap.size = random() % 4 ? (uint64_t)(random() % 64 + 1) << 16 : (uint64_t)pick();
			break;
		case VQDA_OP_HEAP_FREE:
		case VQDA_OP_HEAP_IMPORT:
			s.u.heap.bid = random() % 4 ? (uint32_t)(random() % 64) : (uint32_t)pick();
			break;
		case VQDA_OP_GEM_CREATE:
			s.u.gem.size = pick();
			s.u.gem.npages = random() % 2 ? (uint32_t)((s.u.gem.size + 4095) / 4096) : (uint32_t)pick();
			s.u.gem.nlist = random() % 2 ? (s.u.gem.npages + 1023) / 1024 : (uint32_t)pick();
			break;
		case VQDA_OP_OPEN:
			/* never a real DSP name: an open that works leaks a session */
			memcpy(s.u.open.dsp, "cdspX\0\0\0\0\0\0\0\0\0\0\0", 16);
			if (random() % 2)
				memset(s.u.open.dsp, 'A' + random() % 26, 16);
			break;
		case VQDA_OP_CLOSE:
		case VQDA_OP_RESET:
			op = VQDA_OP_PING;	/* would end the real session */
			break;
		}
		ret = vqda_raw(op, xs, &s);
		ret ? refused++ : ok++;
		if (i % 1000 == 999) {
			if (vqda_ping()) {
				printf("FUZZ n %ld back end stopped answering FAIL\n", i);
				return 1;
			}
			pings++;
		}
	}
	close(fd);
	/* open/close storm: sessions on the real DSP, opened and dropped */
	for (i = 0; i < 300; i++) {
		int f = open("/dev/accel/accel0", O_RDWR);

		if (f >= 0 && !(i % 3))
			ioctl(f, DRM_IOCTL_QDA_REMOTE_SESSION_CREATE, &ic);
		if (f >= 0)
			close(f);
	}
	ret = vqda_ping();
	printf("FUZZ n %ld refused %ld ok %ld pings %ld storm 300 final ping %d %s\n",
	       n, refused, ok, pings, ret, ret ? "FAIL" : "PASS");
	return ret ? 1 : 0;
}

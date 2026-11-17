// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * vqda-neg: isolation checks a vqda guest runs against its back end. Each
 * line is "NEG <name> PASS|FAIL <detail>": PASS means the back end or the
 * host refused what a guest must not be able to do.
 *  dsp-not-assigned  open a DSP the back end does not give this guest
 *  signed-pd         create a signed (non-unsigned) DSP process
 *  attach-root-pd    attach to the DSP's root PD
 *  forged-sid        use a session id this guest never opened
 *  init-handle       invoke the DSP's process init handle
 *  forged-handle     invoke and close buffer handles this session has none of
 *  foreign-grants    make the back end map grant refs this guest never made
 *  shell-handle      touch the host-supplied shell buffer
 *  arg-past-end, arg-too-long, arg-offset-huge, arg-wrap
 *                    invoke with an argument outside a real 4 KiB buffer
 *  heap-*            heap buffers (back end -H): import or free an id the
 *                    guest never got, allocate nothing or more than the
 *                    region, use a freed id; a freed range a session still
 *                    imports is not handed out again, and allocation goes
 *                    on past it
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "vqda_proto.h"

int vqda_raw(uint32_t op, uint32_t sid, void *slot);
int vqda_sid(int fd);

static int fails;

static void check(const char *name, int refused, int ret)
{
	printf("NEG %s %s ret %d\n", name, refused ? "PASS" : "FAIL", ret);
	fails += !refused;
}

static int open_dsp(const char *dsp, uint32_t *sid)
{
	struct vqda_slot s;
	int ret;

	memset(&s, 0, sizeof(s));
	strncpy(s.u.open.dsp, dsp, sizeof(s.u.open.dsp) - 1);
	ret = vqda_raw(VQDA_OP_OPEN, 0, &s);
	*sid = s.sid;
	return ret;
}

int main(int argc, char **argv)
{
	const char *mine = argc > 1 ? argv[1] : "cdsp2";
	const char *other = argc > 2 ? argv[2] : "cdsp3";
	struct vqda_slot s;
	uint32_t sid, x;
	int ret, h, i;

	if (open_dsp(mine, &sid)) {
		printf("NEG setup FAIL cannot open %s\n", mine);
		return 2;
	}

	ret = open_dsp(other, &x);
	check("dsp-not-assigned", ret == -EACCES, ret);

	memset(&s, 0, sizeof(s));
	s.u.create.attrs = 0;
	ret = vqda_raw(VQDA_OP_SESSION_CREATE, sid, &s);
	check("signed-pd", ret == -EPERM, ret);

	memset(&s, 0, sizeof(s));
	ret = vqda_raw(VQDA_OP_SESSION_ATTACH, sid, &s);
	check("attach-root-pd", ret == -EPERM, ret);

	memset(&s, 0, sizeof(s));
	s.u.gem.handle = 1;
	ret = vqda_raw(VQDA_OP_GEM_CLOSE, 63, &s);
	check("forged-sid", ret == -EBADF, ret);

	memset(&s, 0, sizeof(s));
	s.u.gem.size = 4096;
	s.u.gem.npages = 1;
	s.u.gem.nlist = 1;
	for (i = 0, ret = 0; i < 8 && !ret; i++) {
		s.u.gem.list[0] = 100000 + i * 777;	/* refs this guest never made */
		ret = vqda_raw(VQDA_OP_GEM_CREATE, sid, &s);
	}
	check("foreign-grants", ret != 0, ret);

	/* an unsigned PD (host shell) so invocations reach buffer checks */
	memset(&s, 0, sizeof(s));
	s.u.create.attrs = QDA_INIT_ATTR_UNSIGNED_MODULE;
	ret = vqda_raw(VQDA_OP_SESSION_CREATE, sid, &s);
	if (ret) {
		printf("NEG setup FAIL unsigned create ret %d\n", ret);
		return 2;
	}

	memset(&s, 0, sizeof(s));
	s.u.invoke.handle = 1;
	ret = vqda_raw(VQDA_OP_INVOKE, sid, &s);
	check("init-handle", ret == -EPERM, ret);

	/* buffer handles 2..64 do not exist in this session (1 is the shell) */
	for (h = 2, ret = 0; h <= 64; h++) {
		memset(&s, 0, sizeof(s));
		s.u.invoke.handle = 0;
		s.u.invoke.sc = 1 << 16;	/* one input buffer */
		s.u.invoke.nargs = 1;
		s.u.invoke.args[0].handle = h;
		s.u.invoke.args[0].length = 16;
		ret = vqda_raw(VQDA_OP_INVOKE, sid, &s);
		if (!ret)
			break;
	}
	check("forged-handle", ret != 0, ret);

	/* the host shell must not be reachable as a buffer */
	for (h = 1; h <= 8; h++) {
		memset(&s, 0, sizeof(s));
		s.u.share.src_sid = sid;
		s.u.share.src_handle = h;
		ret = vqda_raw(VQDA_OP_GEM_SHARE, sid, &s);
		if (ret == -EPERM)
			break;
	}
	check("shell-handle", ret == -EPERM, ret);

	/*
	 * A real 4 KiB buffer, made through the ioctl path, then invocations
	 * whose argument runs past it, sent raw so the guest library's own
	 * checks are bypassed: the host must refuse them.
	 */
	{
		static const struct { const char *name; uint64_t off, len; } bad[] = {
			{ "arg-past-end", 4000, 4096 },
			{ "arg-too-long", 0, 8192 },
			{ "arg-offset-huge", 1ull << 40, 16 },
			{ "arg-wrap", ~0ull - 7, 16 },
		};
		struct drm_qda_init_create ic = { .attrs = QDA_INIT_ATTR_UNSIGNED_MODULE };
		struct drm_qda_gem_create g = { .size = 4096 };
		int fd = open("/dev/accel/accel0", O_RDWR), vs;
		unsigned int k;

		if (fd < 0 || ioctl(fd, DRM_IOCTL_QDA_REMOTE_SESSION_CREATE, &ic) ||
		    ioctl(fd, DRM_IOCTL_QDA_GEM_CREATE, &g) || (vs = vqda_sid(fd)) < 0) {
			printf("NEG setup FAIL real buffer (fd %d errno %d)\n", fd, errno);
			return 2;
		}
		for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
			memset(&s, 0, sizeof(s));
			s.u.invoke.handle = 0;
			s.u.invoke.sc = 1 << 16;
			s.u.invoke.nargs = 1;
			s.u.invoke.args[0].handle = g.handle;
			s.u.invoke.args[0].offset = bad[k].off;
			s.u.invoke.args[0].length = bad[k].len;
			ret = vqda_raw(VQDA_OP_INVOKE, vs, &s);
			check(bad[k].name, ret != 0, ret);
		}
		close(fd);
	}

	vqda_raw(VQDA_OP_CLOSE, sid, &s);
	/* heap cases on a live session: earlier cases may have closed sid */
	if (open_dsp(mine, &sid)) {
		printf("NEG setup FAIL cannot reopen %s\n", mine);
		return 2;
	}
	memset(&s, 0, sizeof(s));
	s.u.heap.bid = 4095;
	ret = vqda_raw(VQDA_OP_HEAP_IMPORT, sid, &s);
	check("heap-import-forged", ret == -EBADF || ret == -ENODEV, ret);
	s.u.heap.bid = 100000;
	ret = vqda_raw(VQDA_OP_HEAP_FREE, 0, &s);
	check("heap-free-forged", ret == -EBADF, ret);
	memset(&s, 0, sizeof(s));
	ret = vqda_raw(VQDA_OP_HEAP_ALLOC, 0, &s);
	check("heap-alloc-zero", ret == -EINVAL || ret == -ENODEV, ret);
	s.u.heap.size = 1ull << 44;
	ret = vqda_raw(VQDA_OP_HEAP_ALLOC, 0, &s);
	check("heap-alloc-huge", ret == -EINVAL || ret == -ENODEV, ret);
	memset(&s, 0, sizeof(s));
	s.u.heap.size = 2u << 20;
	if (!vqda_raw(VQDA_OP_HEAP_ALLOC, 0, &s)) {
		uint32_t bid = s.u.heap.bid, hnd;
		uint64_t off = s.u.heap.off;

		memset(&s, 0, sizeof(s));
		s.u.heap.bid = bid;
		ret = vqda_raw(VQDA_OP_HEAP_IMPORT, sid, &s);
		hnd = s.u.heap.handle;
		check("heap-import-own", ret == 0, ret);
		s.u.heap.bid = bid;
		vqda_raw(VQDA_OP_HEAP_FREE, 0, &s);
		memset(&s, 0, sizeof(s));
		s.u.heap.bid = bid;
		ret = vqda_raw(VQDA_OP_HEAP_IMPORT, sid, &s);
		check("heap-import-freed", ret == -EBADF, ret);
		/* the freed range is still imported: never handed out again yet */
		memset(&s, 0, sizeof(s));
		s.u.heap.size = 2u << 20;
		ret = vqda_raw(VQDA_OP_HEAP_ALLOC, 0, &s);
		check("heap-alloc-past-busy", ret == 0, ret);
		check("heap-busy-not-reused", ret || s.u.heap.off != off, (int)(s.u.heap.off >> 20));
		if (!ret) {
			bid = s.u.heap.bid;
			s.u.heap.bid = bid;
			vqda_raw(VQDA_OP_HEAP_FREE, 0, &s);
		}
		memset(&s, 0, sizeof(s));
		s.u.gem.handle = hnd;
		vqda_raw(VQDA_OP_GEM_CLOSE, sid, &s);
	} else {
		printf("NEG heap-* SKIP no heap (back end without -H)\n");
	}

	printf("NEG summary %s %d failed\n", fails ? "FAIL" : "PASS", fails);
	return fails ? 1 : 0;
}

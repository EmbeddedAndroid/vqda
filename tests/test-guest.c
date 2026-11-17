// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * libvqda outside a Xen guest: calls it does not own pass through to
 * libc unchanged, and the ones it owns fail with an errno instead of
 * crashing or hanging when there is no back end to reach. In a guest
 * with Xen devices the no-back-end cases are skipped.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-heap.h>

int vqda_sid(int fd);

static int fails, xen;

static void expect(const char *what, int ok)
{
	printf("%s %s (errno %d)\n", ok ? "PASS" : "FAIL", what, errno);
	fails += !ok;
}

static void expect_nb(const char *what, int ok)
{
	if (xen)
		printf("SKIP %s (Xen devices present)\n", what);
	else
		expect(what, ok);
}

int main(int argc, char **argv)
{
	struct dma_heap_allocation_data a = { 0 };
	int fd, p[2], n = -1;
	char c;
	void *m;

	(void)argc;
	setvbuf(stdout, NULL, _IOLBF, 0);
	xen = !access("/dev/xen/gntalloc", F_OK) || !access("/dev/xen/evtchn", F_OK);
	unsetenv("VQDA_RING");
	unsetenv("VQDA_DSPS");

	fd = open(argv[0], O_RDONLY);
	expect("open of a file passes through", fd >= 0 && read(fd, &c, 1) == 1);
	m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
	expect("mmap of a file passes through", m != MAP_FAILED && !memcmp(m, &c, 1));
	expect("close passes through", !close(fd) && close(fd) && errno == EBADF);
	expect("ioctl passes through", !pipe(p) && write(p[1], "x", 1) == 1 &&
	       !ioctl(p[0], FIONREAD, &n) && n == 1);
	close(p[0]);
	close(p[1]);
	expect("no session for a plain fd", vqda_sid(0) == -1 && vqda_sid(-1) == -1);

	errno = 0;
	if (!xen) {
		fd = open("/dev/accel/accel0", O_RDWR);
		expect("accel open without a back end fails", fd == -1 && errno == ENODEV);
	} else {
		expect_nb("accel open without a back end fails", 0);
	}
	fd = open("/dev/accel/accel9", O_RDWR);
	expect("accel past the DSP list is not ours", fd == -1 && errno != ENODEV);

	setenv("VQDA_DSPS", "cdsp", 1);
	fd = open("/dev/dma_heap/system", O_RDWR);
	expect("dma heap of a vqda guest is ours", fd >= 0);
	a.len = 0;
	expect("heap alloc of 0 bytes", ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &a) == -1 &&
	       errno == EINVAL);
	a.len = 1ull << 44;
	expect("heap alloc past the grant limit", ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &a) == -1 &&
	       errno == E2BIG);
	a.len = 4096;
	if (!xen)
		expect("heap alloc without a back end", ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &a) == -1 &&
		       errno == ENODEV);
	else
		expect_nb("heap alloc without a back end", 0);
	expect("heap fd closes", !close(fd));
	return fails ? 1 : 0;
}

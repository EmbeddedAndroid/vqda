// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * vqda-bulk send <ring phys> <file> | recv <ring phys> <file>: move a file
 * from the hardware domain into a vqda test guest through the payload
 * window of its static ring (ring + 0x20000, 0xd0000 bytes), one chunk at a
 * time. A test tool for guests without a network or a disk (models and
 * tools for the guest); the static shared memory region must be at least
 * 1 MiB. The control page's harness words carry the handshake:
 *   harness[1] seq   written by send after a chunk is in the window
 *   harness[2] len   length of that chunk; BULK_HDR set: a header chunk
 *                    (struct hdr: magic and file size) starting a file
 *   harness[3] ack   written by recv once it has taken chunk seq
 * harness[0] is left alone.
 * recv acknowledges and drops every chunk before a header, so state left
 * by an earlier, interrupted transfer cannot shift a file; it ends after
 * the size the header announced. Either side gives up after 120 s without
 * progress from the other.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "vqda_proto.h"

#define WIN_OFF		0x20000
#define WIN_SIZE	0xd0000
#define BULK_HDR	0x80000000u
#define BULK_MAGIC	0x6b6c7562	/* "bulk" */

struct hdr {
	uint32_t magic;
	uint32_t pad;
	uint64_t size;
};

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void pause_us(void)
{
	struct timespec t = { 0, 20000 };

	nanosleep(&t, NULL);
}

static volatile uint32_t *seq, *len, *ack;

/* send side: wait until chunk *s was taken; 0 on timeout */
static int taken(uint32_t s)
{
	double t0 = now();

	while (__atomic_load_n(ack, __ATOMIC_ACQUIRE) != s) {
		if (now() - t0 > 120)
			return 0;
		pause_us();
	}
	return 1;
}

/* send side: publish the chunk now in the window */
static void put(uint32_t *s, uint32_t n)
{
	__atomic_store_n(len, n, __ATOMIC_RELEASE);
	__atomic_store_n(seq, ++*s, __ATOMIC_RELEASE);
}

/* recv side: wait for the chunk after *s; 0 on timeout */
static int get(uint32_t *s, uint32_t *n)
{
	double t0 = now();

	while (__atomic_load_n(seq, __ATOMIC_ACQUIRE) == *s) {
		if (now() - t0 > 120)
			return 0;
		pause_us();
	}
	*s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
	*n = __atomic_load_n(len, __ATOMIC_ACQUIRE);
	return 1;
}

int main(int argc, char **argv)
{
	unsigned long long total = 0;
	struct vqda_ctrl *ctrl;
	int send, mem, fd;
	struct hdr h;
	uint32_t s, n;
	char *ring, *win;
	double t0;

	if (argc != 4)
		return 2;
	send = !strcmp(argv[1], "send");
	mem = open("/dev/mem", O_RDWR | O_SYNC);
	if (mem < 0)
		return 1;
	ring = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_SHARED,
		    mem, strtoull(argv[2], NULL, 0));
	if (ring == MAP_FAILED)
		return 1;
	ctrl = (struct vqda_ctrl *)(ring + VQDA_CTRL_OFF);
	seq = &ctrl->harness[1];
	len = &ctrl->harness[2];
	ack = &ctrl->harness[3];
	win = ring + WIN_OFF;
	fd = send ? open(argv[3], O_RDONLY) : open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror(argv[3]);
		return 1;
	}
	t0 = now();
	if (send) {
		struct stat st;
		ssize_t r;

		if (fstat(fd, &st))
			return 1;
		s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
		__atomic_store_n(ack, s, __ATOMIC_RELEASE);
		h = (struct hdr){ .magic = BULK_MAGIC, .size = st.st_size };
		memcpy(win, &h, sizeof(h));
		put(&s, BULK_HDR | sizeof(h));
		while (total < (unsigned long long)st.st_size) {
			if (!taken(s))
				goto timeout;
			r = read(fd, win, WIN_SIZE);
			if (r <= 0) {
				perror(argv[3]);
				return 1;
			}
			put(&s, (uint32_t)r);
			total += r;
		}
		if (!taken(s))
			goto timeout;
	} else {
		unsigned long long size = 0;
		int started = 0;

		s = __atomic_load_n(ack, __ATOMIC_ACQUIRE);
		while (!started || total < size) {
			if (!get(&s, &n)) {
				fprintf(stderr, "vqda-bulk: timeout after %llu bytes\n", total);
				return 1;
			}
			if (n & BULK_HDR) {
				memcpy(&h, win, sizeof(h));
				if (h.magic == BULK_MAGIC) {
					started = 1;
					size = h.size;
					total = 0;
				}
			} else if (started) {
				if (n > WIN_SIZE || n > size - total ||
				    write(fd, win, n) != (ssize_t)n)
					return 1;
				total += n;
			}
			__atomic_store_n(ack, s, __ATOMIC_RELEASE);
		}
		if (fsync(fd) && errno != EINVAL)
			return 1;
	}
	printf("VQDABULK %s %llu bytes %.1f s\n", argv[1], total, now() - t0);
	return 0;
timeout:
	fprintf(stderr, "vqda-bulk: no ack after %llu bytes\n", total);
	return 1;
}

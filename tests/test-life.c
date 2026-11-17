// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * The guest library's buffer lifetimes against a fake back end on a ring
 * in memory and a fake gntalloc on a memfd: pooling, PRIME pins, copies
 * in other sessions and session close, checked by what the back end and
 * gntalloc see (host handles closed once, grants freed once, all of them
 * by the end).
 */
#include "../src/vqda-guest.c"

#include <sys/syscall.h>

#define NPAGES_MAX (1 << 16)
static char granted[NPAGES_MAX];	/* by gntalloc index page */
static uint64_t next_index;
static long grants, bad_deallocs;
static int (*libc_ioctl)(int, unsigned long, ...);

static int fake_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	uint64_t i;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (fd != gal)
		return libc_ioctl(fd, req, arg);
	if (req == IOCTL_GNTALLOC_ALLOC_GREF) {
		struct ioctl_gntalloc_alloc_gref *g = arg;

		if (next_index / PAGE + g->count > NPAGES_MAX) {
			errno = ENOSPC;
			return -1;
		}
		g->index = next_index;
		for (i = 0; i < g->count; i++) {
			g->gref_ids[i] = next_index / PAGE + i + 1;
			granted[next_index / PAGE + i] = 1;
		}
		next_index += (uint64_t)g->count * PAGE;
		grants += g->count;
		return ftruncate(gal, next_index);
	}
	if (req == IOCTL_GNTALLOC_DEALLOC_GREF) {
		struct ioctl_gntalloc_dealloc_gref *d = arg;

		for (i = 0; i < d->count; i++) {
			if (!granted[d->index / PAGE + i])
				bad_deallocs++;
			granted[d->index / PAGE + i] = 0;
		}
		grants -= d->count;
		return 0;
	}
	errno = ENOTTY;
	return -1;
}

/* The fake back end: sessions own handles naming objects. */
#define OBJS 256
#define HANDLES 64
static int obj_live[OBJS], nobj, host_closes, bad_closes;
static int sess_open[8];
static int handle_obj[8][HANDLES];	/* sid, handle -> object + 1 */

static uint32_t new_handle(uint32_t sid, int obj)
{
	uint32_t h;

	for (h = 1; h < HANDLES; h++)
		if (!handle_obj[sid][h]) {
			handle_obj[sid][h] = obj + 1;
			return h;
		}
	return 0;
}

static int obj_users(int obj)
{
	int s, h, n = 0;

	for (s = 0; s < 8; s++)
		for (h = 0; h < HANDLES; h++)
			n += handle_obj[s][h] == obj + 1;
	return n;
}

static void drop_handle(uint32_t sid, uint32_t h)
{
	int obj = handle_obj[sid][h] - 1;

	handle_obj[sid][h] = 0;
	host_closes++;
	if (!obj_users(obj))
		obj_live[obj] = 0;
}

static int serve(struct vqda_slot *r)
{
	uint32_t sid = r->sid, h;
	int i;

	if (r->op != VQDA_OP_OPEN && r->op != VQDA_OP_PING && r->op != VQDA_OP_RESET &&
	    (sid >= 8 || !sess_open[sid]))
		return -EBADF;
	switch (r->op) {
	case VQDA_OP_PING:
	case VQDA_OP_RESET:
		return 0;
	case VQDA_OP_OPEN:
		for (i = 0; i < 8 && sess_open[i]; i++)
			;
		if (i == 8)
			return -EMFILE;
		sess_open[i] = 1;
		r->sid = i;
		return 0;
	case VQDA_OP_CLOSE:
		for (h = 1; h < HANDLES; h++)
			if (handle_obj[sid][h])
				drop_handle(sid, h);
		sess_open[sid] = 0;
		return 0;
	case VQDA_OP_GEM_CREATE:
		if (!r->u.gem.npages || nobj == OBJS)
			return -EINVAL;
		obj_live[nobj] = 1;
		r->u.gem.handle = new_handle(sid, nobj++);
		return r->u.gem.handle ? 0 : -ENOMEM;
	case VQDA_OP_GEM_CLOSE:
		h = r->u.gem.handle;
		if (h >= HANDLES || !handle_obj[sid][h]) {
			bad_closes++;
			return -EINVAL;
		}
		drop_handle(sid, h);
		return 0;
	case VQDA_OP_GEM_SHARE: {
		uint32_t ss = r->u.share.src_sid, sh = r->u.share.src_handle;
		int obj;

		if (ss >= 8 || sh >= HANDLES || !handle_obj[ss][sh])
			return -ENOENT;
		obj = handle_obj[ss][sh] - 1;
		for (h = 1; h < HANDLES; h++)
			if (handle_obj[sid][h] == obj + 1)
				break;
		r->u.share.handle = h < HANDLES ? h : new_handle(sid, obj);
		return 0;
	}
	case VQDA_OP_HEAP_ALLOC:
	case VQDA_OP_HEAP_FREE:
	case VQDA_OP_HEAP_IMPORT:
		return -ENODEV;
	}
	return 0;
}

static void *back(void *arg)
{
	(void)arg;
	for (;;) {
		int i, n = 0;

		for (i = 0; i < VQDA_SLOTS; i++) {
			struct vqda_slot *r = (void *)((char *)ring + i * VQDA_SLOT_SIZE);

			if (__atomic_load_n(&r->state, __ATOMIC_SEQ_CST) != VQDA_REQ)
				continue;
			r->ret = serve(r);
			__atomic_store_n(&r->state, VQDA_DONE, __ATOMIC_SEQ_CST);
			n++;
		}
		if (n) {
			pthread_mutex_lock(&lock);
			pthread_cond_broadcast(&done);
			pthread_mutex_unlock(&lock);
		} else {
			usleep(100);
		}
	}
	return NULL;
}

static int fails;

static void expect(const char *what, int ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	fails += !ok;
}

static int live_objects(void)
{
	int i, n = 0;

	for (i = 0; i < nobj; i++)
		n += obj_live[i];
	return n;
}

static uint32_t create(int fd, uint64_t size)
{
	struct drm_qda_gem_create c = { .size = size };

	return ioctl(fd, DRM_IOCTL_QDA_GEM_CREATE, &c) ? 0 : c.handle;
}

static int gem_close(int fd, uint32_t h)
{
	struct drm_gem_close g = { .handle = h };

	return ioctl(fd, DRM_IOCTL_GEM_CLOSE, &g) ? -errno : 0;
}

static char *map(int fd, uint32_t h, size_t len)
{
	struct drm_qda_gem_mmap_offset o = { .handle = h };
	char *p;

	if (ioctl(fd, DRM_IOCTL_QDA_GEM_MMAP_OFFSET, &o))
		return NULL;
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, o.offset);
	return p == MAP_FAILED ? NULL : p;
}

static int export(int fd, uint32_t h)
{
	struct drm_prime_handle ph = { .handle = h, .flags = O_RDWR };

	return ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph) ? -1 : ph.fd;
}

static uint32_t import(int fd, int pfd)
{
	struct drm_prime_handle ph = { .fd = pfd };

	return ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) ? 0 : ph.handle;
}

static void test_big_buffer(int a)
{
	int c0 = host_closes;
	uint32_t h = create(a, 1 << 20);
	char *p = map(a, h, 1 << 20);

	expect("create and map a 1 MiB buffer", h && p);
	memset(p, 0x5a, 1 << 20);
	munmap(p, 1 << 20);
	expect("close of a big buffer", !gem_close(a, h) && host_closes == c0 + 1);
	expect("its grants are freed", grants == 0);
}

static void test_pool(int a)
{
	uint32_t h = create(a, 4096), h2;
	char *p = map(a, h, 4096);
	int c0 = host_closes;

	memset(p, 0x77, 4096);
	munmap(p, 4096);
	expect("small buffer close keeps it pooled",
	       !gem_close(a, h) && host_closes == c0 && grants == 1);
	expect("second close of a pooled handle", gem_close(a, h) == -EINVAL &&
	       host_closes == c0);
	h2 = create(a, 4096);
	p = map(a, h2, 4096);
	expect("pooled buffer comes back zeroed", h2 == h && p && !p[0] && !p[4095]);
	munmap(p, 4096);
	gem_close(a, h2);
}

static void test_prime_pin(int a)
{
	uint32_t h = create(a, 1 << 20);
	char *p = map(a, h, 1 << 20);
	int pfd, c0;

	p[100] = 42;
	munmap(p, 1 << 20);
	pfd = export(a, h);
	c0 = host_closes;
	expect("close under a prime fd only hides the handle",
	       pfd >= 0 && !gem_close(a, h) && host_closes == c0);
	expect("second close of a hidden handle", gem_close(a, h) == -EINVAL);
	p = mmap(NULL, 1 << 20, PROT_READ, MAP_SHARED, pfd, 0);
	expect("the prime fd still maps the buffer", p != MAP_FAILED && p[100] == 42);
	munmap(p, 1 << 20);
	close(pfd);
	expect("last prime fd closes it on the host", host_closes == c0 + 1);
}

static void test_small_export(int a)
{
	uint32_t h = create(a, 4096);
	int pfd = export(a, h), c0 = host_closes;

	/* the prime fd goes first, so the close finds the buffer unpinned */
	close(pfd);
	gem_close(a, h);
	expect("an exported buffer is never pooled", host_closes == c0 + 1);
}

static void test_copy(int a, int b)
{
	uint32_t h = create(a, 1 << 20), hb;
	char *p = map(a, h, 1 << 20);
	int pfd;

	p[7] = 7;
	munmap(p, 1 << 20);
	pfd = export(a, h);
	hb = import(b, pfd);
	expect("import into another session", hb != 0);
	gem_close(a, h);
	close(pfd);
	p = map(b, hb, 1 << 20);
	expect("the copy outlives the exporter's handle", p && p[7] == 7);
	munmap(p, 1 << 20);
	expect("grants kept for the copy", grants >= 256);
	gem_close(b, hb);
}

static void test_session_close(void)
{
	int a = open("/dev/accel/accel0", O_RDWR);
	uint32_t h1 = create(a, 1 << 20), h2 = create(a, 4096);

	expect("buffers of a session", h1 && h2);
	close(a);
	expect("session close frees its buffers", live_objects() == 0 && grants == 0);
}

static void test_grant_heap(int a, int b)
{
	struct dma_heap_allocation_data d = { .len = 1 << 20 };
	int heap = open("/dev/dma_heap/system", O_RDWR), c0;
	long g0 = grants;
	uint32_t ha, hb;
	char *p;

	expect("grant heap alloc", heap >= 0 && !ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &d));
	p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd, 0);
	expect("heap buffer maps", p != MAP_FAILED);
	p[9] = 9;
	munmap(p, 1 << 20);
	ha = import(a, d.fd);
	hb = import(b, d.fd);
	expect("imports into two sessions", ha && hb && import(a, d.fd) == ha);
	close(d.fd);
	p = map(b, hb, 1 << 20);
	expect("imports outlive the heap fd", p && p[9] == 9 && grants >= 256);
	munmap(p, 1 << 20);
	c0 = host_closes;
	gem_close(a, ha);
	gem_close(b, hb);
	expect("last import frees the grants", host_closes == c0 + 2 && grants == g0);
	close(heap);
}

int main(void)
{
	pthread_t t;
	int a, b;

	setvbuf(stdout, NULL, _IOLBF, 0);
	resolve();
	libc_ioctl = real_ioctl;
	real_ioctl = fake_ioctl;
	gal = syscall(SYS_memfd_create, "gntalloc", 0);
	evt = -1;
	ring = aligned_alloc(PAGE, VQDA_MAP_SIZE);
	memset(ring, 0, VQDA_MAP_SIZE);
	ctrl = ctrl_of(ring);
	up = 1;
	setenv("VQDA_DSPS", "cdsp,cdsp1", 1);
	pthread_create(&t, NULL, back, NULL);

	a = open("/dev/accel/accel0", O_RDWR);
	b = open("/dev/accel/accel1", O_RDWR);
	expect("two sessions", a >= 0 && b >= 0);
	test_big_buffer(a);
	test_pool(a);
	test_prime_pin(a);
	test_small_export(a);
	test_copy(a, b);
	test_grant_heap(a, b);
	close(a);
	close(b);
	test_session_close();
	expect("nothing left on the host", live_objects() == 0);
	expect("no grant left", grants == 0);
	expect("no grant freed twice", bad_deallocs == 0);
	expect("no host close of a dead handle", bad_closes == 0);
	return fails ? 1 : 0;
}

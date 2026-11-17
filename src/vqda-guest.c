// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * libvqda.so: LD_PRELOAD transport for a QDA (accel/qda) client in a Xen
 * guest. Opening /dev/accel/accelN gives a local fd whose QDA ioctls go to
 * the hardware domain's vqda-back over the ring of vqda_proto.h; GEM
 * buffers are this guest's pages, granted to the back end (gntalloc), and
 * mmap of a GEM offset maps those same pages. Applications and the FastRPC
 * library (accel build) run unchanged.
 *
 * Environment: VQDA_DSPS="cdsp2,cdsp3" names the DSP behind accel0,
 * accel1, ... (default cdsp,cdsp1,cdsp2,cdsp3); VQDA_BACKEND domain id
 * (default 0); VQDA_RING (guest physical address of the static ring) and
 * VQDA_PORT (its static event channel); without VQDA_RING the channel
 * comes from xenstore instead (vqda_proto.h "Xenstore channel") and the
 * DSP list, without VQDA_DSPS, from /etc/vqda.conf. VQDA_POLL_US:
 * spin that long for each answer before sleeping, and tell the back end
 * not to send the completion event meanwhile (vqda_proto.h "Poll mode").
 *
 * Heap buffers (vqda_proto.h): with VQDA_HEAP (or heap= in /etc/vqda.conf)
 * naming the guest's static shared memory region, /dev/dma_heap/system is
 * served by the back end's heap in that region: DMA_HEAP_IOCTL_ALLOC
 * returns a local fd that mmap maps from /dev/xen-shm/<heap>, and PRIME
 * import of that fd into any accel fd imports the same host buffer into
 * that session. The FastRPC library then allocates device independent
 * buffers, mapped only into the context banks of the sessions using them.
 *
 * Without a heap (a guest a toolstack creates) /dev/dma_heap/system is
 * served from grants: DMA_HEAP_IOCTL_ALLOC grants the pages and returns a
 * local fd that mmap maps from gntalloc; the back end sees the buffer only
 * when a session imports it (PRIME), as a GEM_CREATE of those grants into
 * that session, so it takes room only in the banks of the sessions using
 * it. Created on the default device instead, every buffer would be mapped
 * into that device's context bank whichever DSP uses it, and the buffers
 * of a model split over several DSPs can fill that one bank.
 *
 * Limit: the fds this library hands out (accel, PRIME, heap buffers) are
 * tracked by number and dup, dup2, dup3, F_DUPFD and close_range are not
 * intercepted. A duplicate is not one of ours, and replacing or closing
 * one of ours that way skips its teardown (grants and host handles stay
 * until the process exits).
 */
/*
 * This file defines open and open64, mmap and mmap64 itself: the libc
 * headers must not redirect one name to the other.
 */
#undef _FILE_OFFSET_BITS
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
/* types the Xen uapi headers leave to userspace */
typedef uint16_t domid_t;
typedef uint32_t grant_ref_t;
#include <xen/evtchn.h>
#include <xen/gntalloc.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "vqda_proto.h"
#include "vqda_xs.h"

#define PAGE 4096
#define MAX_FD 1024
#define MAX_BUF 4096
#define OFF_SHIFT 32
_Static_assert(sizeof(off_t) == 8, "GEM offsets need a 64-bit off_t");

struct buf {
	uint32_t handle;	/* 0 = free */
	int fd;
	uint64_t size, index;
	void *va;
	int alias;		/* shares another entry's grants: never ungrant */
	int gfd;		/* imported grant heap buffer: its local fd + 1 */
	int heap;		/* a heap buffer: no grants, mapped from shm at index */
	int pooled;		/* closed by the caller, kept for reuse */
	int exported;		/* PRIME exported: other sessions may use it */
	int pins;		/* prime fds naming it */
	int closing;		/* handle closed by the caller while pinned */
	int dying;		/* claimed for teardown: only its claimer drops it */
	struct buf *owner;	/* a copy: the entry whose grants it maps */
	int users;		/* copies mapping this entry's grants */
};

/*
 * The FastRPC library creates and closes a small GEM buffer around every
 * invocation. Through vqda each create costs grants, a gntdev DMA-BUF, a
 * PRIME import and an SMMU map in the back end, so small buffers are kept
 * per session after the caller closes them and handed out again (zeroed)
 * for a create of the same size. They never leave their session.
 */
#define POOL_MAX_SIZE	(64 * 1024)
#define POOL_PER_FD	16

/* local stand-ins for PRIME fds: which accel fd and handle they export */
struct pfd {
	int accel_fd;		/* 0 = free */
	uint32_t handle;
	struct buf *buf;	/* pinned while this fd lives */
};
static struct pfd pfds[MAX_FD];

/* the system DMA heap: its fd, and local fds standing for heap buffers */
struct hfd {
	int used;
	uint32_t bid;
	uint64_t off, size;
};
static struct hfd hfds[MAX_FD];
static char heap_dev[MAX_FD];	/* fds standing for /dev/dma_heap/system */
#define HEAP_DEV_GRANTS	2	/* heap_dev value: served from grants */
/* grant heap buffers: the fd's own reference plus one per session import */
struct gfd {
	int refs;
	int held;		/* a dup this library holds after the caller closed */
	uint32_t npages;
	uint64_t size, index;
	void *va;
	uint32_t *grefs;
};
static struct gfd gfds[MAX_FD];
/* one buffer's refs fit in VQDA_MAX_LIST granted list pages */
#define MAX_GRANT_BYTES ((uint64_t)VQDA_MAX_LIST * VQDA_REFS_PER_PAGE * PAGE)
static int shm = -1;		/* /dev/xen-shm/<heap> */

static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_close)(int);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done = PTHREAD_COND_INITIALIZER;
/* prime fd teardowns in flight per accel fd: its session id stays ours */
static int teardowns[MAX_FD];
static pthread_cond_t torn = PTHREAD_COND_INITIALIZER;
static int up, gal, evt, port, backend;
static unsigned int poll_us;
const char vqda_build_id[] __attribute__((used)) = VQDA_BUILD_ID;
static struct vqda_ctrl *ctrl;

static uint64_t now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}
static struct vqda_slot *ring;
static int sid_of[MAX_FD];	/* fd -> session id + 1, 0 = not ours */
static struct buf bufs[MAX_BUF];

static void resolve(void)
{
	if (real_ioctl)
		return;
	real_open = dlsym(RTLD_NEXT, "open");
	real_openat = dlsym(RTLD_NEXT, "openat");
	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	real_mmap = dlsym(RTLD_NEXT, "mmap");
	real_close = dlsym(RTLD_NEXT, "close");
}

static void *rx(void *a)
{
	(void)a;
	for (;;) {
		unsigned int p;

		if (read(evt, &p, sizeof(p)) != sizeof(p))
			continue;
		/* unmask the port again */
		if (write(evt, &p, sizeof(p)) != sizeof(p))
			perror("vqda: evtchn unmask");
		pthread_mutex_lock(&lock);
		pthread_cond_broadcast(&done);
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

/*
 * gntalloc takes one array slot per page in a single kmalloc, which a
 * guest with fragmented memory refuses above PAGE_ALLOC_COSTLY_ORDER:
 * ask for at most that many pages at a time.
 */
#define GRANT_CHUNK ((PAGE << 3) / sizeof(uint32_t))
/* Keeps the chunks of one grant consecutive in the file's index space. */
static pthread_mutex_t grant_lock = PTHREAD_MUTEX_INITIALIZER;

/* gntalloc @n pages writable by the back end: index, refs and mapping */
static int grant(uint32_t n, uint64_t *index, uint32_t *refs, void **va)
{
	struct ioctl_gntalloc_alloc_gref *g;
	uint32_t done = 0, c;
	void *p;
	int e = 0;

	g = calloc(1, sizeof(*g) + (n < GRANT_CHUNK ? n : GRANT_CHUNK) * sizeof(uint32_t));
	if (!g)
		return -ENOMEM;
	pthread_mutex_lock(&grant_lock);
	while (done < n) {
		c = n - done < GRANT_CHUNK ? n - done : GRANT_CHUNK;
		g->domid = backend;
		g->flags = GNTALLOC_FLAG_WRITABLE;
		g->count = c;
		if (real_ioctl(gal, IOCTL_GNTALLOC_ALLOC_GREF, g)) {
			e = -errno;
			break;
		}
		if (!done)
			*index = g->index;
		if (refs)
			memcpy(refs + done, g->gref_ids, c * sizeof(uint32_t));
		done += c;
	}
	pthread_mutex_unlock(&grant_lock);
	free(g);
	if (e) {
		if (done) {
			struct ioctl_gntalloc_dealloc_gref d = { .index = *index, .count = done };

			real_ioctl(gal, IOCTL_GNTALLOC_DEALLOC_GREF, &d);
		}
		return e;
	}
	p = real_mmap(NULL, (size_t)n * PAGE, PROT_READ | PROT_WRITE,
		      MAP_SHARED, gal, *index);
	if (p == MAP_FAILED) {
		struct ioctl_gntalloc_dealloc_gref d = { .index = *index,
							 .count = n };

		e = -errno;
		real_ioctl(gal, IOCTL_GNTALLOC_DEALLOC_GREF, &d);
		return e;
	}
	*va = p;
	return 0;
}

static void ungrant(void *va, uint32_t n, uint64_t index)
{
	struct ioctl_gntalloc_dealloc_gref d = { .index = index, .count = n };

	munmap(va, (size_t)n * PAGE);
	real_ioctl(gal, IOCTL_GNTALLOC_DEALLOC_GREF, &d);
}

/*
 * Xenstore channel (vqda_proto.h): grant a fresh ring to the back end,
 * open an unbound event channel for it, publish both under a new
 * ring-gen and wait until the back end says it serves that generation.
 */
static int connect_xs(void)
{
	const char *w = getenv("VQDA_CONNECT_MS");
	char v[64], bp[160], refs[VQDA_MAP_SIZE / PAGE * 11 + 1], *q = refs;
	uint32_t ref[VQDA_MAP_SIZE / PAGE];
	struct ioctl_evtchn_bind_unbound_port u;
	unsigned long gen, i, ms;
	uint64_t index = 0;
	void *va = NULL;
	int xs, ret;

	xs = xs_open();
	if (xs < 0)
		return xs;
	ret = -ENOENT;
	if (xs_read(xs, VQDA_XS_FE "/backend-id", v, sizeof(v)) <= 0 ||
	    xs_read(xs, VQDA_XS_FE "/backend", bp, sizeof(bp)) <= 0)
		goto out;
	backend = atoi(v);
	if (strlen(bp) + sizeof("/ring-gen") > sizeof(bp))
		goto out;
	gen = xs_read(xs, VQDA_XS_FE "/ring-gen", v, sizeof(v)) > 0 ? strtoul(v, NULL, 10) + 1 : 1;
	ret = grant(VQDA_MAP_SIZE / PAGE, &index, ref, &va);
	if (ret)
		goto out;
	u.remote_domain = backend;
	ret = real_ioctl(evt, IOCTL_EVTCHN_BIND_UNBOUND_PORT, &u);
	if (ret < 0) {
		ret = -errno;
		goto out;
	}
	port = ret;
	/*
	 * However this process ends, gntalloc then clears client_alive and
	 * kicks the back end, which closes what this process left open.
	 */
	{
		struct ioctl_gntalloc_unmap_notify un = {
			.action = UNMAP_NOTIFY_CLEAR_BYTE | UNMAP_NOTIFY_SEND_EVENT,
			.event_channel_port = port,
		};

		un.index = index + VQDA_CTRL_OFF + __builtin_offsetof(struct vqda_ctrl, client_alive);
		ctrl_of(va)->client_alive = VQDA_CLIENT_ALIVE;
		/* without it the back end only cleans up when the next client comes */
		if (real_ioctl(gal, IOCTL_GNTALLOC_SET_UNMAP_NOTIFY, &un) < 0)
			ctrl_of(va)->client_alive = 0;
	}
	for (i = 0; i < VQDA_MAP_SIZE / PAGE; i++)
		q += sprintf(q, "%s%u", i ? "," : "", ref[i]);
	snprintf(v, sizeof(v), "%d", port);
	ret = xs_write(xs, VQDA_XS_FE "/ring-refs", refs);
	ret = ret ? ret : xs_write(xs, VQDA_XS_FE "/event-channel", v);
	snprintf(v, sizeof(v), "%lu", gen);
	ret = ret ? ret : xs_write(xs, VQDA_XS_FE "/ring-gen", v);
	ret = ret ? ret : xs_write(xs, VQDA_XS_FE "/state", "3");
	if (ret)
		goto out;
	ms = w ? strtoul(w, NULL, 10) : 30000;
	strcat(bp, "/ring-gen");
	ret = -ETIMEDOUT;
	for (i = 0; i < ms / 10; i++) {
		if (xs_read(xs, bp, v, sizeof(v)) > 0 && strtoul(v, NULL, 10) == gen &&
		    ctrl_of(va)->back_magic == VQDA_BACK_MAGIC) {
			ring = va;
			ret = 0;
			break;
		}
		usleep(10000);
	}
out:
	if (ret && va)
		ungrant(va, VQDA_MAP_SIZE / PAGE, index);
	close(xs);
	return ret;
}

/* Undo a connect that failed: the next open tries again from scratch. */
static void disconnect(int static_ring)
{
	if (static_ring && ring && ring != MAP_FAILED)
		munmap(ring, VQDA_MAP_SIZE);
	ring = NULL;
	ctrl = NULL;
	if (gal >= 0)
		real_close(gal);
	if (evt >= 0)
		real_close(evt);	/* unbinds the port */
	gal = evt = -1;
}

static int connect_back(void)
{
	const char *e = getenv("VQDA_BACKEND"), *r = getenv("VQDA_RING"),
		   *p = getenv("VQDA_PORT"), *w = getenv("VQDA_CONNECT_MS");
	struct ioctl_evtchn_bind b = { 0 };
	struct pollfd pf;
	unsigned int got;
	pthread_t t;
	int fd, i, ret;

	gal = real_open("/dev/xen/gntalloc", O_RDWR | O_CLOEXEC);
	evt = real_open("/dev/xen/evtchn", O_RDWR | O_CLOEXEC);
	poll_us = getenv("VQDA_POLL_US") ? atoi(getenv("VQDA_POLL_US")) : 0;
	if (!r) {
		ret = gal < 0 || evt < 0 ? -ENODEV : connect_xs();
		if (ret)
			goto fail;
		ctrl = ctrl_of(ring);
		ret = -EAGAIN;
		if (pthread_create(&t, NULL, rx, NULL))
			goto fail;
		return 0;
	}
	ret = -EINVAL;
	if (!p)
		goto fail;
	backend = e ? atoi(e) : 0;
	ret = -ENODEV;
	fd = real_open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (gal < 0 || evt < 0 || fd < 0) {
		if (fd >= 0)
			real_close(fd);
		goto fail;
	}
	ring = real_mmap(NULL, VQDA_MAP_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, strtoull(r, NULL, 0));
	real_close(fd);
	if (ring == MAP_FAILED) {
		ret = -errno;
		goto fail;
	}
	memset(ring, 0, VQDA_RING_PAGES * PAGE);
	ctrl = ctrl_of(ring);
	memset((void *)ctrl->guest_polling, 0, sizeof(ctrl->guest_polling));
	b.port = atoi(p);
	/* EBUSY: the previous client process is still exiting */
	for (i = 0; real_ioctl(evt, IOCTL_EVTCHN_BIND_STATIC, &b) < 0; i++) {
		if (errno != EBUSY || i >= 600) {
			ret = -errno;
			goto fail;
		}
		usleep(50000);
	}
	port = b.port;
	/* the back end says hello once it has bound its end; a back end that
	 * is already up has left its magic in the control page */
	pf.fd = evt;
	pf.events = POLLIN;
	if (ctrl->back_magic != VQDA_BACK_MAGIC) {
		ret = -ETIMEDOUT;
		if (poll(&pf, 1, w ? atoi(w) : 1800000) != 1 ||
		    read(evt, &got, sizeof(got)) != sizeof(got))
			goto fail;
		ret = -EIO;
		if (write(evt, &got, sizeof(got)) != sizeof(got))
			goto fail;
	}
	ret = -EAGAIN;
	if (pthread_create(&t, NULL, rx, NULL))
		goto fail;
	return 0;
fail:
	disconnect(!!r);
	return ret;
}

/* Runs one request: @fill fills the slot, @out gets the slot back. */
static int call(uint32_t op, uint32_t sid, const struct vqda_slot *in,
		size_t inlen, struct vqda_slot *out)
{
	struct ioctl_evtchn_notify n;
	struct vqda_slot *r = NULL;
	int i, ret;

	pthread_mutex_lock(&lock);
	for (;;) {
		for (i = 0; i < VQDA_SLOTS; i++) {
			r = (void *)((char *)ring + i * VQDA_SLOT_SIZE);
			if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) == VQDA_FREE)
				break;
		}
		if (i < VQDA_SLOTS)
			break;
		pthread_cond_wait(&done, &lock);
	}
	if (in)
		memcpy((char *)r + 4, (const char *)in + 4, inlen - 4);
	r->op = op;
	r->sid = sid;
	r->ret = -EIO;
	if (poll_us)
		__atomic_store_n(&ctrl->guest_polling[i], 1, __ATOMIC_SEQ_CST);
	__atomic_store_n(&r->state, VQDA_REQ, __ATOMIC_SEQ_CST);
	n.port = port;
	if (!__atomic_load_n(&ctrl->back_polling, __ATOMIC_SEQ_CST))
		real_ioctl(evt, IOCTL_EVTCHN_NOTIFY, &n);
	if (poll_us) {
		uint64_t end = now_us() + poll_us;

		/* the slot is this thread's until it is FREE again */
		pthread_mutex_unlock(&lock);
		while (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) != VQDA_DONE && now_us() < end)
			;
		__atomic_store_n(&ctrl->guest_polling[i], 0, __ATOMIC_SEQ_CST);
		pthread_mutex_lock(&lock);
	}
	while (__atomic_load_n(&r->state, __ATOMIC_SEQ_CST) != VQDA_DONE) {
		struct timespec ts;

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 20000000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&done, &lock, &ts);
	}
	if (out)
		memcpy(out, r, sizeof(*out));
	ret = r->ret;
	if (getenv("VQDA_DEBUG"))
		fprintf(stderr, "vqda: op %u sid %u slot %d ret %d\n", op, sid, i, ret);
	__atomic_store_n(&r->state, VQDA_FREE, __ATOMIC_RELEASE);
	pthread_cond_broadcast(&done);
	pthread_mutex_unlock(&lock);
	return ret;
}

/*
 * /etc/vqda.conf, written by whatever provisions the guest: "dsps=" is the
 * DSP list without VQDA_DSPS ("dsps=cdsp2,cdsp3"), "heap=" the heap
 * without VQDA_HEAP. Read once.
 */
static char conf_dsps_v[128], conf_heap_v[64];
static pthread_once_t conf_once = PTHREAD_ONCE_INIT;

static void conf_read(void)
{
	char line[160];
	FILE *f;

	f = fopen("/etc/vqda.conf", "re");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "dsps=", 5) &&
		    sscanf(line + 5, "%127[a-z0-9,]", conf_dsps_v) != 1)
			conf_dsps_v[0] = 0;
		if (!strncmp(line, "heap=", 5) &&
		    sscanf(line + 5, "%63[a-zA-Z0-9_-]", conf_heap_v) != 1)
			conf_heap_v[0] = 0;
	}
	fclose(f);
}

static const char *conf_dsps(void)
{
	pthread_once(&conf_once, conf_read);
	return conf_dsps_v[0] ? conf_dsps_v : NULL;
}

static const char *conf_heap(void)
{
	if (getenv("VQDA_HEAP"))
		return getenv("VQDA_HEAP");
	pthread_once(&conf_once, conf_read);
	return conf_heap_v[0] ? conf_heap_v : NULL;
}

/*
 * Connect once and RESET the back end before any other request of this
 * process goes out: a request racing the RESET would see its session or
 * buffer closed under it.
 */
static pthread_mutex_t up_lock = PTHREAD_MUTEX_INITIALIZER;

static int ensure_up(void)
{
	int ret = 0;

	if (__atomic_load_n(&up, __ATOMIC_ACQUIRE))
		return 0;
	pthread_mutex_lock(&up_lock);
	if (!up) {
		pthread_mutex_lock(&lock);
		ret = connect_back();
		pthread_mutex_unlock(&lock);
		if (!ret) {
			call(VQDA_OP_RESET, 0, NULL, 0, NULL);
			__atomic_store_n(&up, 1, __ATOMIC_RELEASE);
		}
	}
	pthread_mutex_unlock(&up_lock);
	return ret;
}

static int open_heap(void)
{
	const char *h = conf_heap();
	char p[96];
	int fd;

	if (!h) {
		/* a vqda guest without a heap: serve the heap from grants */
		if (!getenv("VQDA_DSPS") && !conf_dsps())
			return -2;	/* not ours */
		fd = eventfd(0, EFD_CLOEXEC);
		if (fd >= MAX_FD) {
			real_close(fd);
			errno = EMFILE;
			return -1;
		}
		if (fd >= 0)
			heap_dev[fd] = HEAP_DEV_GRANTS;
		return fd;
	}
	pthread_mutex_lock(&up_lock);
	if (shm < 0) {
		snprintf(p, sizeof(p), "/dev/xen-shm/%s", h);
		shm = real_open(p, O_RDWR | O_CLOEXEC);
	}
	pthread_mutex_unlock(&up_lock);
	if (shm < 0)
		return -1;
	fd = eventfd(0, EFD_CLOEXEC);
	if (fd >= MAX_FD) {
		real_close(fd);
		errno = EMFILE;
		return -1;
	}
	if (fd >= 0)
		heap_dev[fd] = 1;
	return fd;
}

static int heap_alloc(struct dma_heap_allocation_data *a)
{
	struct vqda_slot s;
	int fd, ret;

	ret = ensure_up();
	if (ret)
		return ret;
	memset(&s, 0, sizeof(s));
	s.u.heap.size = a->len;
	ret = call(VQDA_OP_HEAP_ALLOC, 0, &s, sizeof(s), &s);
	if (ret)
		return ret;
	fd = eventfd(0, EFD_CLOEXEC);
	if (fd < 0 || fd >= MAX_FD) {
		if (fd >= 0)
			real_close(fd);
		call(VQDA_OP_HEAP_FREE, 0, &s, sizeof(s), NULL);
		return -EMFILE;
	}
	hfds[fd].bid = s.u.heap.bid;
	hfds[fd].off = s.u.heap.off;
	hfds[fd].size = a->len;
	hfds[fd].used = 1;
	a->fd = fd;
	return 0;
}

static int gheap_alloc(struct dma_heap_allocation_data *a)
{
	uint32_t npages, *refs;
	uint64_t index;
	void *va;
	int fd, ret;

	if (!a->len)
		return -EINVAL;
	if (a->len > MAX_GRANT_BYTES)
		return -E2BIG;
	npages = (a->len + PAGE - 1) / PAGE;
	ret = ensure_up();
	if (ret)
		return ret;
	refs = calloc(npages, sizeof(uint32_t));
	if (!refs)
		return -ENOMEM;
	ret = grant(npages, &index, refs, &va);
	if (ret) {
		free(refs);
		return ret;
	}
	fd = eventfd(0, EFD_CLOEXEC);
	if (fd < 0 || fd >= MAX_FD) {
		if (fd >= 0)
			real_close(fd);
		ungrant(va, npages, index);
		free(refs);
		return -EMFILE;
	}
	pthread_mutex_lock(&lock);
	gfds[fd] = (struct gfd){ .refs = 1, .npages = npages, .size = a->len,
				 .index = index, .va = va, .grefs = refs };
	pthread_mutex_unlock(&lock);
	a->fd = fd;
	return 0;
}

/* Drop a reference with lock held; the last one moves the entry to @g. */
static void gfd_unref_locked(int fd, struct gfd *g)
{
	if (gfds[fd].refs && !--gfds[fd].refs) {
		*g = gfds[fd];
		memset(&gfds[fd], 0, sizeof(gfds[fd]));
	}
}

/* Free what gfd_unref_locked() moved out, without the lock. */
static void gfd_release(int fd, struct gfd *g)
{
	if (g->va) {
		ungrant(g->va, g->npages, g->index);
		free(g->grefs);
	}
	if (g->held)
		real_close(fd);
}

static int ours(const char *path, char *dsp)
{
	static const char *const def = "cdsp,cdsp1,cdsp2,cdsp3";
	const char *l = getenv("VQDA_DSPS");
	int n, i;

	if (!l)
		l = conf_dsps();

	if (!path || strncmp(path, "/dev/accel/accel", 16))
		return 0;
	n = atoi(path + 16);
	l = l ? l : def;
	for (i = 0; i < n && l; i++) {
		l = strchr(l, ',');
		l = l ? l + 1 : NULL;
	}
	if (!l || !*l)
		return 0;
	for (i = 0; i < 15 && l[i] && l[i] != ','; i++)
		dsp[i] = l[i];
	dsp[i] = 0;
	return 1;
}

static int open_accel(const char *dsp)
{
	struct vqda_slot s;
	int fd, ret;

	ret = ensure_up();
	if (ret) {
		errno = -ret;
		return -1;
	}
	memset(&s, 0, sizeof(s));
	strncpy(s.u.open.dsp, dsp, sizeof(s.u.open.dsp) - 1);
	ret = call(VQDA_OP_OPEN, 0, &s, sizeof(s), &s);
	if (ret) {
		errno = -ret;
		return -1;
	}
	fd = eventfd(0, EFD_CLOEXEC);
	if (fd < 0 || fd >= MAX_FD) {
		if (fd >= 0)
			real_close(fd);
		call(VQDA_OP_CLOSE, s.sid, NULL, 0, NULL);
		errno = EMFILE;
		return -1;
	}
	sid_of[fd] = s.sid + 1;
	return fd;
}

int open(const char *path, int flags, ...)
{
	char dsp[16];
	mode_t mode = 0;
	va_list ap;

	resolve();
	if (ours(path, dsp))
		return open_accel(dsp);
	if (!strcmp(path, "/dev/dma_heap/system")) {
		int h = open_heap();

		if (h != -2)
			return h;
	}
	va_start(ap, flags);
	if (flags & (O_CREAT | O_TMPFILE))
		mode = va_arg(ap, mode_t);
	va_end(ap);
	return real_open(path, flags, mode);
}
int open64(const char *path, int flags, ...) __attribute__((alias("open")));

int openat(int dirfd, const char *path, int flags, ...)
{
	char dsp[16];
	mode_t mode = 0;
	va_list ap;

	resolve();
	if (ours(path, dsp))
		return open_accel(dsp);
	if (!strcmp(path, "/dev/dma_heap/system")) {
		int h = open_heap();

		if (h != -2)
			return h;
	}
	va_start(ap, flags);
	if (flags & (O_CREAT | O_TMPFILE))
		mode = va_arg(ap, mode_t);
	va_end(ap);
	return real_openat(dirfd, path, flags, mode);
}

static struct buf *find_buf(int fd, uint32_t handle)
{
	int i;

	for (i = 0; i < MAX_BUF; i++)
		if (bufs[i].handle == handle && bufs[i].fd == fd &&
		    !bufs[i].pooled && !bufs[i].closing && !bufs[i].dying)
			return &bufs[i];
	return NULL;
}

/*
 * The entry for a handle the back end returned again on an import, with
 * the lock held: one the caller closed (pooled, or kept for a prime fd)
 * is a handle again. Not one being torn down.
 */
static struct buf *reclaim_locked(int fd, uint32_t handle)
{
	int i;

	for (i = 0; i < MAX_BUF; i++)
		if (bufs[i].handle && bufs[i].handle == handle &&
		    bufs[i].fd == fd && !bufs[i].dying) {
			bufs[i].pooled = 0;
			bufs[i].closing = 0;
			return &bufs[i];
		}
	return NULL;
}

static struct buf *pool_take(int fd, uint64_t size)
{
	struct buf *b = NULL;
	int i;

	pthread_mutex_lock(&lock);
	for (i = 0; i < MAX_BUF; i++)
		if (bufs[i].pooled && !bufs[i].dying && bufs[i].fd == fd &&
		    bufs[i].size == size) {
			b = &bufs[i];
			b->pooled = 0;
			break;
		}
	pthread_mutex_unlock(&lock);
	if (b)
		memset(b->va, 0, b->size);
	return b;
}

static int pool_put_locked(struct buf *b)
{
	int i, n = 0;

	if (b->alias || b->heap || b->exported || b->size > POOL_MAX_SIZE)
		return 0;
	for (i = 0; i < MAX_BUF; i++)
		n += bufs[i].pooled && bufs[i].fd == b->fd;
	if (n < POOL_PER_FD)
		b->pooled = 1;
	return b->pooled;
}

/*
 * Have the back end import @npages granted pages (@refs) as one buffer of
 * @size bytes into session @sid; the session's handle in @handle.
 */
static int host_gem(uint32_t sid, const uint32_t *refs, uint32_t npages,
		    uint64_t size, uint32_t *handle)
{
	uint32_t nlist = (npages + VQDA_REFS_PER_PAGE - 1) / VQDA_REFS_PER_PAGE;
	struct vqda_slot s;
	uint64_t lindex;
	void *list;
	int ret;

	if (nlist > VQDA_MAX_LIST)
		return -E2BIG;
	memset(&s, 0, sizeof(s));
	ret = grant(nlist, &lindex, s.u.gem.list, &list);
	if (ret)
		return ret;
	memcpy(list, refs, (size_t)npages * sizeof(uint32_t));
	s.u.gem.size = size;
	s.u.gem.npages = npages;
	s.u.gem.nlist = nlist;
	ret = call(VQDA_OP_GEM_CREATE, sid, &s, sizeof(s), &s);
	ungrant(list, nlist, lindex);
	if (!ret)
		*handle = s.u.gem.handle;
	return ret;
}

static int gem_create(int fd, uint32_t sid, struct drm_qda_gem_create *c)
{
	uint32_t npages, nlist, *refs, handle = 0;
	struct buf *b = NULL;
	int i, ret;

	if (!c->size)
		return -EINVAL;
	if (c->size > MAX_GRANT_BYTES)
		return -E2BIG;
	npages = (c->size + PAGE - 1) / PAGE;
	b = pool_take(fd, c->size);
	if (b) {
		c->handle = b->handle;
		return 0;
	}
	nlist = (npages + VQDA_REFS_PER_PAGE - 1) / VQDA_REFS_PER_PAGE;
	pthread_mutex_lock(&lock);
	for (i = 0; i < MAX_BUF; i++)
		if (!bufs[i].handle && !bufs[i].va) {
			b = &bufs[i];
			b->va = (void *)1;
			break;
		}
	pthread_mutex_unlock(&lock);
	if (!b)
		return -ENOMEM;
	refs = calloc(nlist * VQDA_REFS_PER_PAGE, sizeof(uint32_t));
	ret = refs ? grant(npages, &b->index, refs, &b->va) : -ENOMEM;
	if (ret)
		goto fail;
	ret = host_gem(sid, refs, npages, c->size, &handle);
	if (ret) {
		ungrant(b->va, npages, b->index);
		goto fail;
	}
	free(refs);
	b->size = c->size;
	b->fd = fd;
	b->handle = handle;
	c->handle = b->handle;
	return 0;
fail:
	free(refs);
	b->va = NULL;
	return ret;
}

/*
 * Forget @b. Its grants go with the last entry mapping them: an owner
 * whose copies remain stays behind, without a handle, until the last
 * copy drops it.
 */
static void drop_buf(struct buf *b)
{
	struct buf me, *o, gone = { 0 };
	struct gfd gg = { 0 };
	int g, owns;

	/* close() may move the grant heap entry: read and drop it at once */
	pthread_mutex_lock(&lock);
	me = *b;
	g = b->gfd;
	owns = !b->alias && !b->heap;
	if (owns && b->users) {
		b->handle = 0;
		b->fd = -1;
		b->dying = b->pins = b->closing = b->exported = 0;
		owns = 0;
	} else {
		memset(b, 0, sizeof(*b));
	}
	o = me.owner;
	if (o && !--o->users && !o->handle) {
		gone = *o;
		memset(o, 0, sizeof(*o));
	}
	if (g)
		gfd_unref_locked(g - 1, &gg);
	pthread_mutex_unlock(&lock);
	if (owns)
		ungrant(me.va, (me.size + PAGE - 1) / PAGE, me.index);
	if (gone.va)
		ungrant(gone.va, (gone.size + PAGE - 1) / PAGE, gone.index);
	if (g)
		gfd_release(g - 1, &gg);
}

/* close claimed buffer @b of session @sid on the host and forget it */
static void buf_close(struct buf *b, uint32_t sid)
{
	struct vqda_slot s;

	memset(&s, 0, sizeof(s));
	s.u.gem.handle = b->handle;
	call(VQDA_OP_GEM_CLOSE, sid, &s, sizeof(s), NULL);
	drop_buf(b);
}

/* drop a pin with lock held: is @b now closed and unpinned? */
static int unpin_locked(struct buf *b)
{
	return b && !--b->pins && b->closing;
}

/*
 * A dma-buf fd keeps its object after the handle is closed: the prime fd
 * pins the buffer, and a GEM_CLOSE meanwhile only hides the handle.
 */
static int prime_to_fd(int fd, struct drm_prime_handle *ph)
{
	struct buf *b;
	int e;

	e = eventfd(0, EFD_CLOEXEC);
	if (e < 0 || e >= MAX_FD) {
		if (e >= 0)
			real_close(e);
		return -EMFILE;
	}
	pthread_mutex_lock(&lock);
	b = find_buf(fd, ph->handle);
	if (b) {
		b->exported = 1;
		b->pins++;
		pfds[e] = (struct pfd){ .accel_fd = fd, .handle = ph->handle, .buf = b };
	}
	pthread_mutex_unlock(&lock);
	if (!b) {
		real_close(e);
		return -ENOENT;
	}
	ph->fd = e;
	return 0;
}

/* close prime fd @e: its pin goes, and with the last one a closed buffer */
static void pfd_put(int e)
{
	struct buf *b;
	uint32_t sid = 0;
	int last, afd = 0;

	pthread_mutex_lock(&lock);
	b = pfds[e].buf;
	memset(&pfds[e], 0, sizeof(pfds[e]));
	last = unpin_locked(b);
	if (last) {
		b->dying = 1;
		afd = b->fd;
		sid = sid_of[afd] - 1;
		teardowns[afd]++;
	}
	pthread_mutex_unlock(&lock);
	if (!last)
		return;
	buf_close(b, sid);
	pthread_mutex_lock(&lock);
	if (!--teardowns[afd])
		pthread_cond_broadcast(&torn);
	pthread_mutex_unlock(&lock);
}

/*
 * PRIME import of grant heap buffer ph->fd into session @sid: the back end
 * imports its grants into that session once; a second import of the same
 * buffer into the same session returns the same handle, as DRM does.
 */
static int gheap_import(int fd, uint32_t sid, struct drm_prime_handle *ph)
{
	struct buf *b = NULL;
	uint32_t handle, npages = 0, *grefs = NULL;
	uint64_t size = 0, index = 0;
	void *va = NULL;
	int i, ret;

	pthread_mutex_lock(&lock);
	if (!gfds[ph->fd].refs || gfds[ph->fd].held) {
		/* closed since the caller looked */
		pthread_mutex_unlock(&lock);
		return -EBADF;
	}
	for (i = 0; i < MAX_BUF; i++)
		if (bufs[i].handle && bufs[i].fd == fd && bufs[i].gfd == ph->fd + 1 &&
		    !bufs[i].dying) {
			bufs[i].pooled = 0;
			bufs[i].closing = 0;
			ph->handle = bufs[i].handle;
			pthread_mutex_unlock(&lock);
			return 0;
		}
	for (i = 0; i < MAX_BUF; i++)
		if (!bufs[i].handle && !bufs[i].va) {
			b = &bufs[i];
			b->va = (void *)1;
			break;
		}
	if (b) {
		struct gfd *g = &gfds[ph->fd];

		/* the reference and the entry's fields, before close() can move it */
		g->refs++;
		b->gfd = ph->fd + 1;
		b->alias = 1;
		grefs = g->grefs;
		npages = g->npages;
		size = g->size;
		index = g->index;
		va = g->va;
	}
	pthread_mutex_unlock(&lock);
	if (!b)
		return -ENOMEM;
	ret = host_gem(sid, grefs, npages, size, &handle);
	if (ret) {
		drop_buf(b);
		return ret;
	}
	pthread_mutex_lock(&lock);
	b->handle = handle;
	b->fd = fd;
	b->size = size;
	b->index = index;
	b->va = va;
	b->alias = 1;
	pthread_mutex_unlock(&lock);
	ph->handle = handle;
	return 0;
}

/* close a handle of session @sid that nothing tracks */
static void share_undo(uint32_t sid, uint32_t handle)
{
	struct vqda_slot s;

	memset(&s, 0, sizeof(s));
	s.u.gem.handle = handle;
	call(VQDA_OP_GEM_CLOSE, sid, &s, sizeof(s), NULL);
}

static int prime_to_handle(int fd, uint32_t sid, struct drm_prime_handle *ph)
{
	struct buf *src, *b = NULL, was = { 0 };
	struct vqda_slot s;
	int i, ret, src_fd;

	if (ph->fd >= 0 && ph->fd < MAX_FD && hfds[ph->fd].used) {
		struct hfd *h = &hfds[ph->fd];

		memset(&s, 0, sizeof(s));
		s.u.heap.bid = h->bid;
		ret = call(VQDA_OP_HEAP_IMPORT, sid, &s, sizeof(s), &s);
		if (ret)
			return ret;
		ph->handle = s.u.heap.handle;
		pthread_mutex_lock(&lock);
		b = reclaim_locked(fd, ph->handle);
		if (!b)
			for (i = 0; i < MAX_BUF; i++)
				if (!bufs[i].handle && !bufs[i].va) {
					b = &bufs[i];
					b->handle = ph->handle;
					b->fd = fd;
					b->size = h->size;
					b->index = h->off;
					b->va = (void *)1;
					b->heap = 1;
					break;
				}
		pthread_mutex_unlock(&lock);
		if (!b) {
			/* untracked, the handle could not be mapped or closed later */
			share_undo(sid, ph->handle);
			return -ENOMEM;
		}
		return 0;
	}
	if (ph->fd >= 0 && ph->fd < MAX_FD && gfds[ph->fd].refs)
		return gheap_import(fd, sid, ph);
	if (ph->fd < 0 || ph->fd >= MAX_FD || !pfds[ph->fd].accel_fd)
		return -EINVAL;	/* only buffers this library exported */
	pthread_mutex_lock(&lock);
	src_fd = pfds[ph->fd].accel_fd;
	src = pfds[ph->fd].buf;
	if (src) {
		was = *src;
		if (src_fd == fd)
			src->closing = 0;	/* imported back: a handle again */
	}
	pthread_mutex_unlock(&lock);
	if (!src)
		return -ENOENT;
	if (src_fd == fd) {
		ph->handle = was.handle;
		return 0;
	}
	memset(&s, 0, sizeof(s));
	s.u.share.src_sid = sid_of[src_fd] - 1;
	s.u.share.src_handle = was.handle;
	ret = call(VQDA_OP_GEM_SHARE, sid, &s, sizeof(s), &s);
	if (ret)
		return ret;
	pthread_mutex_lock(&lock);
	/*
	 * The back end shares through a PRIME import, which returns the
	 * handle this session already has for the object, live, pooled or
	 * closed under a prime fd.
	 */
	if (reclaim_locked(fd, s.u.share.handle)) {
		pthread_mutex_unlock(&lock);
		ph->handle = s.u.share.handle;
		return 0;
	}
	/*
	 * The source may have been closed during the round trip, and its
	 * handle number reused by a new buffer: it must be the same one.
	 */
	if (pfds[ph->fd].accel_fd != src_fd || pfds[ph->fd].buf != src ||
	    src->dying || src->handle != was.handle || src->index != was.index ||
	    src->size != was.size || src->gfd != was.gfd || src->va != was.va)
		src = NULL;
	if (!src) {
		pthread_mutex_unlock(&lock);
		share_undo(sid, s.u.share.handle);
		return -ENOENT;
	}
	for (i = 0; i < MAX_BUF; i++)
		if (!bufs[i].handle && !bufs[i].va) {
			b = &bufs[i];
			break;
		}
	if (b) {
		*b = *src;
		b->fd = fd;
		b->handle = s.u.share.handle;
		b->alias = 1;
		/* the exporter's state is not the copy's */
		b->pooled = b->exported = b->pins = b->closing = 0;
		b->users = 0;
		/* the copy holds its own reference on the grants it maps */
		if (b->gfd)
			gfds[b->gfd - 1].refs++;
		else if (!b->heap && !b->owner)
			b->owner = src;
		if (b->owner)
			b->owner->users++;
	}
	pthread_mutex_unlock(&lock);
	if (!b) {
		share_undo(sid, s.u.share.handle);
		return -ENOMEM;
	}
	ph->handle = s.u.share.handle;
	return 0;
}

int ioctl(int fd, unsigned long req, ...)
{
	struct vqda_slot s;
	uint32_t sid;
	va_list ap;
	void *arg;
	int ret;

	resolve();
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (fd >= 0 && fd < MAX_FD && heap_dev[fd] && req == DMA_HEAP_IOCTL_ALLOC) {
		ret = heap_dev[fd] == HEAP_DEV_GRANTS ? gheap_alloc(arg) : heap_alloc(arg);
		goto out;
	}
	if (fd >= 0 && fd < MAX_FD &&
	    (hfds[fd].used || (gfds[fd].refs && !gfds[fd].held)) &&
	    req == DMA_BUF_IOCTL_SYNC)
		return 0;	/* cacheable memory, coherent banks */
	if (fd < 0 || fd >= MAX_FD || !sid_of[fd])
		return real_ioctl(fd, req, arg);
	sid = sid_of[fd] - 1;
	memset(&s, 0, sizeof(s));

	switch (req) {
	case DRM_IOCTL_QDA_QUERY:
		memcpy(&s.u.query, arg, sizeof(s.u.query));
		ret = call(VQDA_OP_QUERY, sid, &s, sizeof(s), &s);
		if (ret >= 0)
			memcpy(arg, &s.u.query, sizeof(s.u.query));
		break;
	case DRM_IOCTL_QDA_GEM_CREATE:
		ret = gem_create(fd, sid, arg);
		break;
	case DRM_IOCTL_QDA_GEM_MMAP_OFFSET: {
		struct drm_qda_gem_mmap_offset *o = arg;

		pthread_mutex_lock(&lock);
		ret = find_buf(fd, o->handle) ? 0 : -ENOENT;
		pthread_mutex_unlock(&lock);
		if (!ret)
			o->offset = (uint64_t)o->handle << OFF_SHIFT;
		break;
	}
	case DRM_IOCTL_GEM_CLOSE: {
		struct drm_gem_close *g = arg;
		struct buf *b;
		int i, held = 0, drop = 0;

		pthread_mutex_lock(&lock);
		b = find_buf(fd, g->handle);
		if (!b) {
			/* closed already: kept for a prime fd or the pool, or
			 * another thread's close of it is in flight */
			for (i = 0; i < MAX_BUF; i++)
				held |= bufs[i].handle == g->handle && bufs[i].fd == fd &&
					(bufs[i].closing || bufs[i].pooled || bufs[i].dying);
		} else if (b->pins) {
			b->closing = 1;	/* closed with its last prime fd */
		} else if (!pool_put_locked(b)) {
			b->dying = 1;
			drop = 1;
		}
		pthread_mutex_unlock(&lock);
		if (held) {
			ret = -EINVAL;
			break;
		}
		if (b && !drop) {
			ret = 0;
			break;
		}
		s.u.gem.handle = g->handle;
		ret = call(VQDA_OP_GEM_CLOSE, sid, &s, sizeof(s), NULL);
		if (b)
			drop_buf(b);
		break;
	}
	case DRM_IOCTL_PRIME_HANDLE_TO_FD:
		ret = prime_to_fd(fd, arg);
		break;
	case DRM_IOCTL_PRIME_FD_TO_HANDLE:
		ret = prime_to_handle(fd, sid, arg);
		break;
	case DRM_IOCTL_QDA_REMOTE_INVOKE: {
		struct drm_qda_invoke_args *inv = arg;
		int n = vqda_sc_nargs(inv->sc);

		if (n > VQDA_MAX_ARGS) {
			ret = -E2BIG;
			break;
		}
		s.u.invoke.handle = inv->handle;
		s.u.invoke.sc = inv->sc;
		s.u.invoke.nargs = n;
		if (n)
			memcpy(s.u.invoke.args, (void *)(uintptr_t)inv->args,
			       n * sizeof(struct vqda_arg));
		ret = call(VQDA_OP_INVOKE, sid, &s, sizeof(s), NULL);
		break;
	}
	case DRM_IOCTL_QDA_REMOTE_SESSION_CREATE: {
		struct drm_qda_init_create *c = arg;

		s.u.create.attrs = c->attrs;
		s.u.create.siglen = c->siglen;
		ret = call(VQDA_OP_SESSION_CREATE, sid, &s, sizeof(s), NULL);
		break;
	}
	case DRM_IOCTL_QDA_REMOTE_SESSION_ATTACH:
		ret = call(VQDA_OP_SESSION_ATTACH, sid, &s, sizeof(s), NULL);
		break;
	case DRM_IOCTL_QDA_REMOTE_MMAP:
		memcpy(&s.u.map, arg, sizeof(s.u.map));
		ret = call(VQDA_OP_REMOTE_MMAP, sid, &s, sizeof(s), &s);
		memcpy(arg, &s.u.map, sizeof(s.u.map));
		break;
	case DRM_IOCTL_QDA_REMOTE_MUNMAP:
		memcpy(&s.u.unmap, arg, sizeof(s.u.unmap));
		ret = call(VQDA_OP_REMOTE_MUNMAP, sid, &s, sizeof(s), &s);
		memcpy(arg, &s.u.unmap, sizeof(s.u.unmap));
		break;
	case DRM_IOCTL_QDA_SET_OPTION:
		memcpy(&s.u.opt, arg, sizeof(s.u.opt));
		ret = call(VQDA_OP_SET_OPTION, sid, &s, sizeof(s), NULL);
		break;
	default:
		if (getenv("VQDA_DEBUG"))
			fprintf(stderr, "vqda: ioctl %#lx not forwarded\n", req);
		ret = -ENOTTY;
	}
out:
	/* only -errno values are errors; DSP status codes pass through */
	if (ret < 0 && ret >= -4095) {
		errno = -ret;
		return -1;
	}
	return ret;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	struct buf *b, p;

	resolve();
	if (fd >= 0 && fd < MAX_FD && hfds[fd].used) {
		if (off < 0 || (uint64_t)off + len > ((hfds[fd].size + PAGE - 1) & ~(uint64_t)(PAGE - 1))) {
			errno = EINVAL;
			return MAP_FAILED;
		}
		return real_mmap(addr, len, prot, flags, shm, hfds[fd].off + off);
	}
	if (fd >= 0 && fd < MAX_FD && gfds[fd].refs && !gfds[fd].held) {
		if (off < 0 || (off & (PAGE - 1)) ||
		    (uint64_t)off + len > (uint64_t)gfds[fd].npages * PAGE) {
			errno = EINVAL;
			return MAP_FAILED;
		}
		return real_mmap(addr, len, prot, flags, gal, gfds[fd].index + off);
	}
	if (fd >= 0 && fd < MAX_FD && pfds[fd].accel_fd) {
		memset(&p, 0, sizeof(p));
		pthread_mutex_lock(&lock);
		if (pfds[fd].buf)
			p = *pfds[fd].buf;
		pthread_mutex_unlock(&lock);
		if (!p.handle || p.dying || off ||
		    len > ((p.size + PAGE - 1) & ~(uint64_t)(PAGE - 1))) {
			errno = EINVAL;
			return MAP_FAILED;
		}
		return real_mmap(addr, len, prot, flags, p.heap ? shm : gal, p.index);
	}
	if (fd < 0 || fd >= MAX_FD || !sid_of[fd])
		return real_mmap(addr, len, prot, flags, fd, off);
	pthread_mutex_lock(&lock);
	b = off & (((off_t)1 << OFF_SHIFT) - 1) ? NULL : find_buf(fd, (uint64_t)off >> OFF_SHIFT);
	if (b)
		p = *b;
	pthread_mutex_unlock(&lock);
	if (!b || len > ((p.size + PAGE - 1) & ~(uint64_t)(PAGE - 1))) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	return real_mmap(addr, len, prot, flags, p.heap ? shm : gal, p.index);
}
void *mmap64(void *, size_t, int, int, int, off_t) __attribute__((alias("mmap")));

int close(int fd)
{
	resolve();
	if (fd >= 0 && fd < MAX_FD && pfds[fd].accel_fd)
		pfd_put(fd);
	if (fd >= 0 && fd < MAX_FD && hfds[fd].used) {
		struct vqda_slot s;

		memset(&s, 0, sizeof(s));
		s.u.heap.bid = hfds[fd].bid;
		hfds[fd].used = 0;
		call(VQDA_OP_HEAP_FREE, 0, &s, sizeof(s), NULL);
	}
	if (fd >= 0 && fd < MAX_FD && gfds[fd].refs) {
		struct gfd g = { 0 };
		int keep = -1, stray, kept = 0;

		/*
		 * The caller's number is reused once closed: while imports
		 * still use the buffer, its entry moves to a private number,
		 * high so that the caller is unlikely to name it.
		 */
		if (gfds[fd].refs > 1)
			keep = fcntl(fd, F_DUPFD_CLOEXEC, MAX_FD / 2);
		if (keep >= MAX_FD) {
			real_close(keep);
			keep = -1;
		}
		pthread_mutex_lock(&lock);
		stray = gfds[fd].held;
		if (!stray && gfds[fd].refs > 1 && keep >= 0) {
			gfds[keep] = gfds[fd];
			gfds[keep].held = 1;
			memset(&gfds[fd], 0, sizeof(gfds[fd]));
			for (int i = 0; i < MAX_BUF; i++)
				if (bufs[i].gfd == fd + 1)
					bufs[i].gfd = keep + 1;
			gfd_unref_locked(keep, &g);
			keep = -1;
		} else if (!stray && gfds[fd].refs > 1) {
			/* no private number: the caller's stays open instead */
			gfds[fd].held = 1;
			gfd_unref_locked(fd, &g);
			kept = 1;
		} else if (!stray) {
			gfd_unref_locked(fd, &g);
		}
		pthread_mutex_unlock(&lock);
		if (keep >= 0)
			real_close(keep);
		if (stray) {
			errno = EBADF;	/* closed already, held for imports */
			return -1;
		}
		gfd_release(fd, &g);
		if (kept)
			return 0;
	}
	if (fd >= 0 && fd < MAX_FD)
		heap_dev[fd] = 0;
	if (fd >= 0 && fd < MAX_FD && sid_of[fd]) {
		uint32_t sid = sid_of[fd] - 1;
		char mine[MAX_BUF];
		int i;

		/*
		 * The session's objects go with it, and so do its prime fds'.
		 * Claim its buffers first: one a prime fd or GEM_CLOSE claimed
		 * already is torn down by that caller.
		 */
		pthread_mutex_lock(&lock);
		/* the back end hands this session id to the next open */
		while (teardowns[fd])
			pthread_cond_wait(&torn, &lock);
		for (i = 0; i < MAX_FD; i++)
			if (pfds[i].buf && pfds[i].buf->fd == fd)
				pfds[i].buf = NULL;
		for (i = 0; i < MAX_BUF; i++) {
			mine[i] = bufs[i].handle && bufs[i].fd == fd && !bufs[i].dying;
			if (mine[i])
				bufs[i].dying = 1;
		}
		pthread_mutex_unlock(&lock);
		call(VQDA_OP_CLOSE, sid, NULL, 0, NULL);
		for (i = 0; i < MAX_BUF; i++)
			if (mine[i])
				drop_buf(&bufs[i]);
		sid_of[fd] = 0;
	}
	return real_close(fd);
}

/* vqda_ping(): one no-op round trip through the back end (latency probe) */
int vqda_ping(void)
{
	char dsp[16];

	resolve();
	if (!up && ours("/dev/accel/accel0", dsp)) {
		int fd = open_accel(dsp);

		if (fd < 0)
			return -errno;
		close(fd);
	}
	return call(VQDA_OP_PING, 0, NULL, 0, NULL);
}

/* vqda_sid(): the back-end session behind accel fd @fd, -1 if none (vqda-neg) */
int vqda_sid(int fd)
{
	return fd >= 0 && fd < MAX_FD && sid_of[fd] ? sid_of[fd] - 1 : -1;
}

/* vqda_raw(): one request exactly as given, for vqda-neg (isolation tests) */
int vqda_raw(uint32_t op, uint32_t sid, void *slot)
{
	char dsp[16];

	resolve();
	if (!up && ours("/dev/accel/accel0", dsp)) {
		int fd = open_accel(dsp);

		if (fd < 0)
			return -errno;
		close(fd);
	}
	return call(op, sid, slot, sizeof(struct vqda_slot), slot);
}

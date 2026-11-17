// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * The back end's request handling without Xen or a DSP: every policy
 * check of vqda-back.c refuses before it reaches the host, session and
 * mapping lifetimes, the xenstore ring-refs parser, a fuzz pass of random
 * requests, and requests racing session closes through the real workers
 * on a ring in memory. Sessions are /dev/null, so whatever passes the
 * checks fails at the host ioctl; no grant device is open.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <drm/drm.h>
typedef uint16_t domid_t;
typedef uint32_t grant_ref_t;
#include <xen/gntdev.h>
#include "qda_accel.h"
#include "xen-static-shm.h"

/*
 * Host calls on the session file named by block_fd block until the test
 * writes to block_pipe, or a signal interrupts them, as a DSP call that
 * does not return would.
 */
static int block_fd = -1, block_pipe[2];
static int host_calls;
/*
 * A fake host: QDA files are dups of fake_qda (a memfd, so the shell can
 * be mapped), handing out the lowest free GEM handle as DRM does; gntdev
 * is fake_gnt.
 */
static int fake_qda = -1, fake_gnt = -1;
static char qda_handles[256];
static pthread_mutex_t qda_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_fake_qda(int fd)
{
	struct stat a, b;

	return fake_qda >= 0 && !fstat(fd, &a) && !fstat(fake_qda, &b) &&
	       a.st_ino == b.st_ino && a.st_dev == b.st_dev;
}

static uint32_t qda_new_handle(void)
{
	uint32_t h;

	pthread_mutex_lock(&qda_lock);
	for (h = 1; h < sizeof(qda_handles) && qda_handles[h]; h++)
		;
	if (h < sizeof(qda_handles))
		qda_handles[h] = 1;
	pthread_mutex_unlock(&qda_lock);
	return h < sizeof(qda_handles) ? h : 0;
}

static int fake_host(int fd, unsigned long req, void *arg)
{
	if (fd == fake_gnt) {
		if (req == IOCTL_GNTDEV_MAP_GRANT_REF)
			((struct ioctl_gntdev_map_grant_ref *)arg)->index = 0;
		if (req == IOCTL_GNTDEV_DMABUF_EXP_FROM_REFS)
			((struct ioctl_gntdev_dmabuf_exp_from_refs *)arg)->fd =
				open("/dev/null", O_RDONLY | O_CLOEXEC);
		return 0;
	}
	switch (req) {
	case DRM_IOCTL_QDA_GEM_CREATE:
		((struct drm_qda_gem_create *)arg)->handle = qda_new_handle();
		return 0;
	case DRM_IOCTL_QDA_GEM_MMAP_OFFSET:
		((struct drm_qda_gem_mmap_offset *)arg)->offset = 0;
		return 0;
	case DRM_IOCTL_PRIME_FD_TO_HANDLE:
		((struct drm_prime_handle *)arg)->handle = qda_new_handle();
		return 0;
	case DRM_IOCTL_GEM_CLOSE: {
		uint32_t h = ((struct drm_gem_close *)arg)->handle;
		int ok;

		pthread_mutex_lock(&qda_lock);
		ok = h && h < sizeof(qda_handles) && qda_handles[h];
		if (ok)
			qda_handles[h] = 0;
		pthread_mutex_unlock(&qda_lock);
		if (ok)
			return 0;
		errno = EINVAL;
		return -1;
	}
	case DRM_IOCTL_QDA_REMOTE_MMAP:
		return 0;
	}
	errno = ENOTTY;
	return -1;
}

/* the heap region's driver: exports refused (EBUSY) over [busy_off, +busy_len) */
static int fake_heap_fd = -1;
static uint64_t busy_off, busy_len;

static int test_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	char c;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (fd >= 0 && fd == __atomic_load_n(&block_fd, __ATOMIC_SEQ_CST)) {
		__atomic_fetch_add(&host_calls, 1, __ATOMIC_SEQ_CST);
		struct pollfd pf = { .fd = block_pipe[0], .events = POLLIN };

		/* poll is never restarted, so a signal ends the call */
		if (poll(&pf, 1, -1) < 0)
			return -1;
		return read(block_pipe[0], &c, 1) == 1 ? 0 : -1;
	}
	if (fd >= 0 && (fd == fake_gnt || is_fake_qda(fd)))
		return fake_host(fd, req, arg);
	if (fd >= 0 && fd == fake_heap_fd) {
		struct xshm_export *x = arg;

		if (busy_len && x->offset < busy_off + busy_len &&
		    busy_off < x->offset + x->size) {
			errno = EBUSY;
			return -1;
		}
		x->fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		return 0;
	}
	return ioctl(fd, req, arg);
}

/*
 * Set by the test: the next work_lock unlock of the main thread (the one
 * dispatch makes before cancelling a busy worker) waits until worker 0
 * runs a call of the current epoch, so the old call's return lands
 * exactly between dispatch and the cancel.
 */
/*
 * Set by the test: the next SIGUSR1 a cancel sends to worker 0 first
 * releases the old call and gives the worker time to start a new one.
 */
static int hold_kill;

static int test_kill(pthread_t t, int sig)
{
	if (__atomic_exchange_n(&hold_kill, 0, __ATOMIC_SEQ_CST)) {
		(void)!write(block_pipe[1], "x", 1);
		usleep(20000);
	}
	return pthread_kill(t, sig);
}

static int hold_unlock;
static pthread_t main_tid;
static pthread_mutex_t *hold_lock;
static unsigned int *hold_epoch, *hold_call;
static int *hold_busy;

static int test_unlock(pthread_mutex_t *m)
{
	int r = pthread_mutex_unlock(m);
	int k;

	if (m == hold_lock && pthread_equal(pthread_self(), main_tid) &&
	    __atomic_exchange_n(&hold_unlock, 0, __ATOMIC_SEQ_CST))
		for (k = 0; k < 2000; k++) {
			if (__atomic_load_n(hold_busy, __ATOMIC_SEQ_CST) &&
			    __atomic_load_n(hold_call, __ATOMIC_SEQ_CST) ==
			    __atomic_load_n(hold_epoch, __ATOMIC_SEQ_CST))
				break;
			usleep(1000);
		}
	return r;
}

#define ioctl test_ioctl
#define pthread_kill test_kill
#define pthread_mutex_unlock test_unlock
#define main vqda_back_main
#include "../src/vqda-back.c"
#undef main
#undef pthread_mutex_unlock
#undef pthread_kill
#undef ioctl

static int fails;

static void expect(const char *what, int got, int want)
{
	printf("%s %s ret %d want %d\n", got == want ? "PASS" : "FAIL", what,
	       got, want);
	fails += got != want;
}

static void fake_session(uint32_t sid, uint32_t shell)
{
	pthread_mutex_lock(&sess_lock);
	if (sess[sid].fd < 0)
		sess[sid] = (struct session){
			.fd = open("/dev/null", O_RDWR | O_CLOEXEC) };
	if (!sess[sid].closing)
		sess[sid].shell = shell;
	pthread_mutex_unlock(&sess_lock);
}

static int req(uint32_t op, uint32_t sid, struct vqda_slot *s)
{
	s->op = op;
	s->sid = sid;
	return handle(s);
}

static void policy(void)
{
	struct vqda_ctrl c = { 0 };
	struct vqda_slot s;

	allow = "cdsp2,cdsp3";
	expect("allowed first", allowed("cdsp2"), 0);
	expect("allowed second", allowed("cdsp3"), 1);
	expect("not allowed", allowed("cdsp"), -1);
	expect("prefix is not a match", allowed("cdsp2x"), -1);
	expect("empty name", allowed(""), -1);
	expect("unterminated name", allowed("cdsp2cdsp2cdsp2c"), -1);

	memset(&s, 0, sizeof(s));
	strcpy(s.u.open.dsp, "cdsp1");
	expect("open of an unassigned DSP", req(VQDA_OP_OPEN, 0, &s), -EACCES);
	memset(&s, 0, sizeof(s));
	memset(s.u.open.dsp, 'c', sizeof(s.u.open.dsp));
	expect("open of an unterminated name", req(VQDA_OP_OPEN, 0, &s), -EACCES);

	memset(&s, 0, sizeof(s));
	expect("session never opened", req(VQDA_OP_QUERY, 5, &s), -EBADF);
	expect("session id out of range", req(VQDA_OP_QUERY, MAX_SESSIONS, &s), -EBADF);
	expect("session id ~0", req(VQDA_OP_GEM_CLOSE, ~0u, &s), -EBADF);

	fake_session(0, 7);
	memset(&s, 0, sizeof(s));
	expect("attach to a static PD", req(VQDA_OP_SESSION_ATTACH, 0, &s), -EPERM);
	expect("signed process", req(VQDA_OP_SESSION_CREATE, 0, &s), -EPERM);
	s.u.gem.handle = 7;
	expect("close of the shell", req(VQDA_OP_GEM_CLOSE, 0, &s), -EPERM);

	memset(&s, 0, sizeof(s));
	s.u.invoke.sc = 2 << 16;
	s.u.invoke.nargs = 2;
	s.u.invoke.args[1].handle = 7;
	expect("shell as an argument", req(VQDA_OP_INVOKE, 0, &s), -EPERM);
	s.u.invoke.nargs = 1;
	expect("nargs not what sc says", req(VQDA_OP_INVOKE, 0, &s), -EINVAL);
	s.u.invoke.sc = 0xff << 16;
	s.u.invoke.nargs = 0xff;
	expect("more args than a slot holds", req(VQDA_OP_INVOKE, 0, &s), -EINVAL);
	memset(&s, 0, sizeof(s));
	s.u.invoke.handle = QDA_INIT_HANDLE;
	expect("invoke on the init handle", req(VQDA_OP_INVOKE, 0, &s), -EPERM);

	memset(&s, 0, sizeof(s));
	s.u.map.handle = 7;
	expect("DSP map of the shell", req(VQDA_OP_REMOTE_MMAP, 0, &s), -EPERM);
	memset(&s, 0, sizeof(s));
	s.u.share.src_sid = 0;
	s.u.share.src_handle = 7;
	expect("share of the shell", req(VQDA_OP_GEM_SHARE, 0, &s), -EPERM);
	s.u.share.src_sid = 9;
	expect("share from a closed session", req(VQDA_OP_GEM_SHARE, 0, &s), -EBADF);

	memset(&s, 0, sizeof(s));
	expect("buffer of size 0", req(VQDA_OP_GEM_CREATE, 0, &s), -EINVAL);
	s.u.gem.size = 8192;
	s.u.gem.npages = 1;
	s.u.gem.nlist = 1;
	expect("page count not the size", req(VQDA_OP_GEM_CREATE, 0, &s), -EINVAL);
	s.u.gem.npages = 2;
	s.u.gem.nlist = 0;
	expect("no list pages", req(VQDA_OP_GEM_CREATE, 0, &s), -EINVAL);
	s.u.gem.nlist = 2;
	expect("list pages not the page count", req(VQDA_OP_GEM_CREATE, 0, &s), -EINVAL);
	s.u.gem.size = (uint64_t)(VQDA_MAX_LIST + 1) * VQDA_REFS_PER_PAGE * PAGE;
	s.u.gem.npages = (VQDA_MAX_LIST + 1) * VQDA_REFS_PER_PAGE;
	s.u.gem.nlist = VQDA_MAX_LIST + 1;
	expect("more list pages than a slot holds", req(VQDA_OP_GEM_CREATE, 0, &s), -EINVAL);
	s.u.gem.size = 1ull << 44;
	s.u.gem.npages = 0;
	s.u.gem.nlist = 0;
	expect("page count past 32 bits", req(VQDA_OP_GEM_CREATE, 0, &s), -EINVAL);
	s.u.gem.size = ~0ull - 100;
	expect("size rounding wraps", req(VQDA_OP_GEM_CREATE, 0, &s), -EINVAL);
	s.u.gem.size = 2 * PAGE;
	s.u.gem.npages = 2;
	s.u.gem.nlist = 1;
	max_mapped_pages = 1;
	expect("buffer over the mapping budget", req(VQDA_OP_GEM_CREATE, 0, &s), -ENOSPC);
	max_mapped_pages = 0;
	expect("budget returned", (int)mapped_pages, 0);

	memset(&s, 0, sizeof(s));
	s.u.heap.size = 4096;
	expect("heap alloc without a heap", req(VQDA_OP_HEAP_ALLOC, 0, &s), -ENODEV);
	s.u.heap.bid = 3;
	expect("heap free of a bad id", req(VQDA_OP_HEAP_FREE, 0, &s), -EBADF);
	s.u.heap.bid = ~0u;
	expect("heap import of a bad id", req(VQDA_OP_HEAP_IMPORT, 0, &s), -EBADF);

	memset(&s, 0, sizeof(s));
	expect("unknown op", req(VQDA_OP_LAST + 1, 0, &s), -ENOSYS);
	expect("ping", req(VQDA_OP_PING, 0, &s), 0);

	ctrl = &c;
	c.client_alive = VQDA_CLIENT_GONE;
	expect("request of a dead client", req(VQDA_OP_PING, 0, &s), -ECONNRESET);
	c.client_alive = VQDA_CLIENT_ALIVE;
	expect("request of a live client", req(VQDA_OP_PING, 0, &s), 0);
	ctrl = NULL;
}

static void lifetimes(void)
{
	struct vqda_slot s;
	struct session *h;
	int m, fd;

	/* a request in flight keeps a closed session's file open */
	fake_session(2, 0);
	fd = sess[2].fd;
	h = get(2);
	memset(&s, 0, sizeof(s));
	expect("close", req(VQDA_OP_CLOSE, 2, &s), 0);
	expect("closed session refuses requests", req(VQDA_OP_QUERY, 2, &s), -EBADF);
	expect("file open while a request holds it", fcntl(fd, F_GETFD) >= 0, 1);
	put(h);
	expect("file closed with the last request", fcntl(fd, F_GETFD), -1);
	expect("slot free again", sess[2].fd, -1);

	/* mappings go with their handle or their session */
	fake_session(3, 0);
	m = map_reserve(10);
	map_commit(m, 3, 42);
	expect("mapping counted", (int)map_pages(3, 42), 10);
	map_drop(3, 42);
	expect("handle close drops it", (int)map_pages(3, 42), 0);
	m = map_reserve(10);
	map_commit(m, 3, 43);
	memset(&s, 0, sizeof(s));
	req(VQDA_OP_CLOSE, 3, &s);
	expect("session close drops it", (int)map_pages(3, 43), 0);
	expect("budget empty", (int)mapped_pages, 0);
	max_mapped_pages = 15;
	m = map_reserve(10);
	expect("second reservation over budget", map_reserve(10), -ENOSPC);
	map_cancel(m);
	max_mapped_pages = 0;
	expect("budget empty after cancel", (int)mapped_pages, 0);
}

static void ring_refs(void)
{
	uint32_t r[3];

	expect("refs", parse_refs("1,22,333", 3, r) == 0 && r[2] == 333, 1);
	expect("refs: too few", parse_refs("1,22", 3, r), -EINVAL);
	expect("refs: no comma", parse_refs("1", 3, r), -EINVAL);
	expect("refs: trailing", parse_refs("1,2,3,", 3, r), -EINVAL);
	expect("refs: sign", parse_refs("1,-2,3", 3, r), -EINVAL);
	expect("refs: past 32 bits", parse_refs("1,2,4294967296", 3, r), -EINVAL);
	expect("refs: empty", parse_refs("", 3, r), -EINVAL);
}

/*
 * The workers on a ring in memory: clients post requests on their own
 * slots while others close and reopen the sessions those requests name.
 * Run under TSan, this is the check that no request reaches a session
 * file after its close.
 */
#define RACE_CLIENTS 8
static volatile int race_stop;

static void *race_client(void *arg)
{
	int c = (int)(intptr_t)arg, k;
	unsigned int seed = c;

	for (k = 0; k < 3000; k++) {
		struct vqda_slot *r = (void *)((char *)ring + c * VQDA_SLOT_SIZE);
		uint32_t sid = rand_r(&seed) % 4;

		if (rand_r(&seed) % 8 == 0)
			fake_session(sid, 1);
		memset((void *)&r->u, 0, sizeof(r->u));
		r->op = rand_r(&seed) % 5 ? VQDA_OP_QUERY : VQDA_OP_CLOSE;
		r->sid = sid;
		__atomic_store_n(&r->state, VQDA_REQ, __ATOMIC_SEQ_CST);
		dispatch();
		while (__atomic_load_n(&r->state, __ATOMIC_SEQ_CST) != VQDA_DONE)
			sched_yield();
		if (r->ret != -EBADF && r->ret != -ENOTTY && r->ret != 0) {
			printf("FAIL race op %u ret %d\n", r->op, r->ret);
			__atomic_fetch_add(&fails, 1, __ATOMIC_SEQ_CST);
		}
		__atomic_store_n(&r->state, VQDA_FREE, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

static void race(void)
{
	static struct vqda_ctrl c;
	pthread_t t[RACE_CLIENTS], w;
	int i, before = fails;

	ring = aligned_alloc(PAGE, VQDA_MAP_SIZE);
	memset(ring, 0, VQDA_MAP_SIZE);
	ctrl = &c;
	for (i = 0; i < VQDA_SLOTS; i++)
		pthread_create(&w, NULL, worker, (void *)(uintptr_t)i);
	for (i = 0; i < RACE_CLIENTS; i++)
		pthread_create(&t[i], NULL, race_client, (void *)(intptr_t)i);
	for (i = 0; i < RACE_CLIENTS; i++)
		pthread_join(t[i], NULL);
	printf("%s race %d clients\n", fails == before ? "PASS" : "FAIL", RACE_CLIENTS);
}

/* Post @op on slot @i and wait up to 2 s for its answer; DONE or not. */
static int post(int i, uint32_t op, uint32_t sid, int *ret)
{
	struct vqda_slot *r = (void *)((char *)ring + i * VQDA_SLOT_SIZE);
	int k;

	memset((void *)&r->u, 0, sizeof(r->u));
	r->op = op;
	r->sid = sid;
	__atomic_store_n(&r->state, VQDA_REQ, __ATOMIC_SEQ_CST);
	dispatch();
	for (k = 0; k < 2000 && __atomic_load_n(&r->state, __ATOMIC_SEQ_CST) != VQDA_DONE; k++)
		usleep(1000);
	*ret = r->ret;
	return __atomic_load_n(&r->state, __ATOMIC_SEQ_CST) == VQDA_DONE;
}

/*
 * A new client reuses a slot whose worker is still in an old client's
 * call: the old call is interrupted, its result dropped, and the new
 * request answered, also when the old call returns on its own just as
 * the new request is dispatched.
 */
static void cancels(void)
{
	struct vqda_slot *r = (void *)ring;
	int k, ret, hung = 0, wrong = 0;

	if (pipe(block_pipe) || fcntl(block_pipe[0], F_SETFL, O_NONBLOCK))
		return;
	for (k = 0; k < 40; k++) {
		int calls = __atomic_load_n(&host_calls, __ATOMIC_SEQ_CST);
		char c;

		/* a byte an interrupted call did not take */
		while (read(block_pipe[0], &c, 1) == 1)
			;
		fake_session(5, 1);
		__atomic_store_n(&block_fd, sess[5].fd, __ATOMIC_SEQ_CST);
		memset((void *)&r->u, 0, sizeof(r->u));
		r->op = VQDA_OP_QUERY;
		r->sid = 5;
		__atomic_store_n(&r->state, VQDA_REQ, __ATOMIC_SEQ_CST);
		dispatch();
		while (__atomic_load_n(&host_calls, __ATOMIC_SEQ_CST) == calls)
			sched_yield();
		/* the new client zeroes the ring, then posts */
		__atomic_store_n(&r->state, VQDA_FREE, __ATOMIC_SEQ_CST);
		if (k & 1)
			(void)!write(block_pipe[1], "x", 1);	/* returns on its own */
		if (!post(0, VQDA_OP_PING, 0, &ret))
			hung++;
		else if (ret)
			wrong++;
		__atomic_store_n(&r->state, VQDA_FREE, __ATOMIC_SEQ_CST);
		while (__atomic_load_n(&wbusy[0], __ATOMIC_SEQ_CST))
			sched_yield();
	}
	__atomic_store_n(&block_fd, -1, __ATOMIC_SEQ_CST);
	expect("new request on a cancelled slot answered", hung, 0);
	expect("new request gets its own result", wrong, 0);
	expect("old calls reached the host", host_calls >= 40, 1);
}

/*
 * The old call returns just after dispatch handed its slot to a new
 * request and before the cancel: the cancel must leave the new call
 * alone, and the new request must get its answer.
 */
static void cancel_window(void)
{
	struct vqda_slot *r = (void *)ring;
	int calls, k, ok;
	char c;

	while (read(block_pipe[0], &c, 1) == 1)
		;
	fake_session(5, 1);
	__atomic_store_n(&block_fd, sess[5].fd, __ATOMIC_SEQ_CST);
	calls = __atomic_load_n(&host_calls, __ATOMIC_SEQ_CST);
	r->op = VQDA_OP_QUERY;
	r->sid = 5;
	__atomic_store_n(&r->state, VQDA_REQ, __ATOMIC_SEQ_CST);
	dispatch();
	while (__atomic_load_n(&host_calls, __ATOMIC_SEQ_CST) == calls)
		sched_yield();
	/* new client: same slot, another blocking call; the old one returns */
	__atomic_store_n(&r->state, VQDA_FREE, __ATOMIC_SEQ_CST);
	r->op = VQDA_OP_QUERY;
	r->sid = 5;
	(void)!write(block_pipe[1], "x", 1);
	__atomic_store_n(&r->state, VQDA_REQ, __ATOMIC_SEQ_CST);
	__atomic_store_n(&hold_unlock, 1, __ATOMIC_SEQ_CST);
	dispatch();
	/* release the new call, unless the cancel already ended it */
	usleep(50000);
	(void)!write(block_pipe[1], "x", 1);
	for (k = 0; k < 2000 && __atomic_load_n(&r->state, __ATOMIC_SEQ_CST) != VQDA_DONE; k++)
		usleep(1000);
	ok = __atomic_load_n(&r->state, __ATOMIC_SEQ_CST) == VQDA_DONE && r->ret == 0;
	expect("old call returning before the cancel", ok, 1);
	__atomic_store_n(&r->state, VQDA_FREE, __ATOMIC_SEQ_CST);
	__atomic_store_n(&block_fd, -1, __ATOMIC_SEQ_CST);
}

static uint64_t halloc(uint64_t size, int *ret)
{
	struct vqda_slot s;

	memset(&s, 0, sizeof(s));
	s.u.heap.size = size;
	*ret = req(VQDA_OP_HEAP_ALLOC, 0, &s);
	return *ret ? ~0ull : s.u.heap.off | (uint64_t)s.u.heap.bid << 48;
}

static void heap(void)
{
	uint64_t a, b, c;
	struct vqda_slot s;
	int ret;

	fake_heap_fd = heap_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	heap_size = 16 << 20;
	a = halloc(4096, &ret);
	expect("heap: small buffer at the start", (int)(a & 0xffffffffffff), 0);
	b = halloc(3 << 20, &ret);
	expect("heap: 2 MiB and up start on 2 MiB", (int)((b & 0xffffffffffff) >> 20), 2);
	halloc(0, &ret);
	expect("heap: zero size", ret, -EINVAL);
	halloc(17 << 20, &ret);
	expect("heap: larger than the region", ret, -EINVAL);
	/* b freed while a session still imports it */
	busy_off = 2 << 20;
	busy_len = 3 << 20;
	memset(&s, 0, sizeof(s));
	s.u.heap.bid = b >> 48;
	expect("heap: free", req(VQDA_OP_HEAP_FREE, 0, &s), 0);
	c = halloc(3 << 20, &ret);
	expect("heap: an imported range is not handed out again",
	       !ret && (c & 0xffffffffffff) != (b & 0xffffffffffff), 1);
	halloc(12 << 20, &ret);
	expect("heap: full", ret, -ENOMEM);
	busy_len = 0;
	c = halloc(3 << 20, &ret);
	expect("heap: the range comes back once released",
	       (int)((c & 0xffffffffffff) >> 20), 2);
	memset(&s, 0, sizeof(s));
	req(VQDA_OP_RESET, 0, &s);
	halloc(16 << 20, &ret);
	expect("heap: reset frees everything", ret, 0);
	req(VQDA_OP_RESET, 0, &s);
	close(heap_fd);
	fake_heap_fd = heap_fd = -1;
	heap_size = 0;
}

/*
 * The old call returns while the cancel is about to signal it: the
 * signal must not land in the new call the worker starts.
 */
static void cancel_kill(void)
{
	struct vqda_slot *r = (void *)ring;
	int calls, k;
	char c;

	while (read(block_pipe[0], &c, 1) == 1)
		;
	fake_session(5, 1);
	__atomic_store_n(&block_fd, sess[5].fd, __ATOMIC_SEQ_CST);
	calls = __atomic_load_n(&host_calls, __ATOMIC_SEQ_CST);
	r->op = VQDA_OP_QUERY;
	r->sid = 5;
	__atomic_store_n(&r->state, VQDA_REQ, __ATOMIC_SEQ_CST);
	dispatch();
	while (__atomic_load_n(&host_calls, __ATOMIC_SEQ_CST) == calls)
		sched_yield();
	__atomic_store_n(&r->state, VQDA_FREE, __ATOMIC_SEQ_CST);
	r->op = VQDA_OP_QUERY;
	r->sid = 5;
	__atomic_store_n(&r->state, VQDA_REQ, __ATOMIC_SEQ_CST);
	__atomic_store_n(&hold_kill, 1, __ATOMIC_SEQ_CST);
	dispatch();
	usleep(50000);
	(void)!write(block_pipe[1], "x", 1);	/* release the new call */
	for (k = 0; k < 2000 && __atomic_load_n(&r->state, __ATOMIC_SEQ_CST) != VQDA_DONE; k++)
		usleep(1000);
	expect("old call returning inside the cancel",
	       __atomic_load_n(&r->state, __ATOMIC_SEQ_CST) == VQDA_DONE && r->ret == 0, 1);
	__atomic_store_n(&r->state, VQDA_FREE, __ATOMIC_SEQ_CST);
	__atomic_store_n(&block_fd, -1, __ATOMIC_SEQ_CST);
}

static void options(void)
{
	unsigned long u;

	expect("num_arg decimal", num_arg("42", 10, 100, &u) == 0 && u == 42, 1);
	expect("num_arg hex", num_arg("0x10", 0, 100, &u) == 0 && u == 16, 1);
	expect("num_arg over max", num_arg("101", 10, 100, &u), -1);
	expect("num_arg trailing", num_arg("4k", 10, 100, &u), -1);
	expect("num_arg sign", num_arg("-1", 10, 100, &u), -1);
	expect("num_arg empty", num_arg("", 10, 100, &u), -1);
	expect("num_arg 08 is decimal", num_arg("08", 10, 100, &u) == 0 && u == 8, 1);
}

/* Session @sid on the fake host. */
static void host_session(uint32_t sid)
{
	pthread_mutex_lock(&sess_lock);
	sess[sid] = (struct session){ .fd = dup(fake_qda), .shell = 200 };
	pthread_mutex_unlock(&sess_lock);
}

static void shell(void)
{
	char path[] = "/tmp/vqda-shellXXXXXX", buf[5] = { 0 };
	uint32_t h = 0, len = 0;
	int f = mkstemp(path), q = dup(fake_qda);

	(void)!write(f, "SHELL", 5);
	close(f);
	shell_path = path;
	expect("shell loads", load_shell(q, &h, &len), 0);
	expect("shell handle and length", h != 0 && len == 5, 1);
	(void)!pread(fake_qda, buf, 5, 0);
	expect("shell contents in its buffer", !memcmp(buf, "SHELL", 5), 1);
	unlink(path);
	shell_path = "/nonexistent/vqda-shell";
	expect("missing shell", load_shell(q, &h, &len) < 0, 1);
	close(q);
	pthread_mutex_lock(&qda_lock);
	memset(qda_handles, 0, sizeof(qda_handles));
	pthread_mutex_unlock(&qda_lock);
}

/* REMOTE_MMAP keeps a buffer charged after GEM_CLOSE, until the session goes. */
static void dsp_mapped(void)
{
	struct vqda_slot s;
	uint32_t h;

	host_session(7);
	memset(&s, 0, sizeof(s));
	s.u.gem.size = 3 * PAGE;
	s.u.gem.npages = 3;
	s.u.gem.nlist = 1;
	expect("buffer on the fake host", req(VQDA_OP_GEM_CREATE, 7, &s), 0);
	h = s.u.gem.handle;
	memset(&s, 0, sizeof(s));
	s.u.map.handle = h;
	req(VQDA_OP_REMOTE_MMAP, 7, &s);
	memset(&s, 0, sizeof(s));
	s.u.gem.handle = h;
	req(VQDA_OP_GEM_CLOSE, 7, &s);
	expect("a DSP-mapped buffer stays charged", (int)mapped_pages, 3);
	memset(&s, 0, sizeof(s));
	req(VQDA_OP_CLOSE, 7, &s);
	expect("until its session closes", (int)mapped_pages, 0);
}

/* Closes and imports that reuse a handle number never lose a charge. */
static long handle_errors;

static void *handle_racer(void *arg)
{
	int k;

	(void)arg;
	for (k = 0; k < 3000; k++) {
		struct vqda_slot s;
		uint32_t h;

		memset(&s, 0, sizeof(s));
		s.u.gem.size = PAGE;
		s.u.gem.npages = 1;
		s.u.gem.nlist = 1;
		if (req(VQDA_OP_GEM_CREATE, 8, &s))
			continue;
		h = s.u.gem.handle;
		if (map_pages(8, h) != 1)
			__atomic_fetch_add(&handle_errors, 1, __ATOMIC_SEQ_CST);
		memset(&s, 0, sizeof(s));
		s.u.gem.handle = h;
		req(VQDA_OP_GEM_CLOSE, 8, &s);
	}
	return NULL;
}

static void handle_race(void)
{
	pthread_t t[4];
	struct vqda_slot s;
	int i;

	host_session(8);
	for (i = 0; i < 4; i++)
		pthread_create(&t[i], NULL, handle_racer, NULL);
	for (i = 0; i < 4; i++)
		pthread_join(t[i], NULL);
	expect("handle reuse keeps every charge", (int)handle_errors, 0);
	expect("all charges returned", (int)mapped_pages, 0);
	memset(&s, 0, sizeof(s));
	req(VQDA_OP_CLOSE, 8, &s);
}

static void fuzz(long n)
{
	long i, bad = 0;
	int k;

	srandom(1);
	for (i = 0; i < n; i++) {
		struct vqda_slot s;
		uint32_t *w = (uint32_t *)&s;
		int ret;

		/* sessions 0 and 1 live, 1 with a shell */
		fake_session(0, 0);
		fake_session(1, 1 + random() % 4);
		for (k = 0; k < (int)(sizeof(s) / 4); k++)
			w[k] = random() % 3 ? random() % 8 : (uint32_t)random();
		s.op = random() % (VQDA_OP_LAST + 3);
		s.sid = random() % 4 ? random() % 3 : (uint32_t)random();
		if (s.op == VQDA_OP_GEM_CREATE && random() % 2) {
			s.u.gem.size = (uint64_t)(random() % 2048 + 1) * PAGE;
			s.u.gem.npages = s.u.gem.size / PAGE;
			s.u.gem.nlist = (s.u.gem.npages + VQDA_REFS_PER_PAGE - 1) /
					VQDA_REFS_PER_PAGE;
		}
		ret = handle(&s);
		if (ret > 0 || ret < -4095) {
			printf("FAIL fuzz op %u sid %u ret %d\n", s.op, s.sid, ret);
			bad++;
		}
	}
	printf("%s fuzz %ld requests, %ld bad results\n", bad ? "FAIL" : "PASS",
	       n, bad);
	fails += bad != 0;
}

int main(void)
{
	struct sigaction sa = { .sa_handler = on_cancel };	/* as vqda-back */
	int i;

	setvbuf(stdout, NULL, _IOLBF, 0);
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	/* before any worker reads them */
	main_tid = pthread_self();
	hold_lock = &work_lock;
	hold_epoch = &wepoch[0];
	hold_call = &wcall[0];
	hold_busy = &wbusy[0];
	for (i = 0; i < MAX_SESSIONS; i++)
		sess[i].fd = -1;
	gnt = evt = -1;
	shell_path = "/nonexistent/vqda-shell";
	policy();
	lifetimes();
	ring_refs();
	heap();
	options();
	fake_qda = syscall(SYS_memfd_create, "qda", 0);
	fake_gnt = syscall(SYS_memfd_create, "gntdev", 0);
	if (ftruncate(fake_qda, PAGE) || ftruncate(fake_gnt, VQDA_MAX_LIST * PAGE))
		return 2;
	gnt = fake_gnt;
	shell();
	dsp_mapped();
	handle_race();
	gnt = -1;
	fuzz(200000);
	race();
	cancels();
	cancel_window();
	cancel_kill();
	return fails ? 1 : 0;
}

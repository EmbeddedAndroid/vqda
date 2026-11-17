// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * vqda-back: the hardware domain's back end for one guest's QDA clients
 * (vqda_proto.h). Every guest session is one open file of the host's
 * /dev/accel device for that DSP, so it gets its own context bank and DSP
 * process, as a host process would. Guest buffers are the guest's granted
 * pages, turned into a DMA-BUF by gntdev and imported into that session:
 * nothing is copied.
 *
 * vqda-back -l
 *	print the DSP names of this host's QDA devices, one per line (what a
 *	back end could serve), and exit.
 * vqda-back <domid> (-m <ring phys> -p <port> | -x) [-d dsp,dsp...]
 *	     [-s shell] [-H heap] [-M MiB] [-P us] [-v]
 *	-m, -p	the static ring and its static event channel
 *	-x	the xenstore channel of vqda_proto.h instead of the static
 *		ring; -d then defaults to the backend node's dsps
 *	-d	the host DSPs this guest may open
 *	-s	the DSP shell to run (default below)
 *	-H	serve heap buffers from the static shared memory region
 *		/dev/xen-shm/<heap> the guest owns (vqda_proto.h "Heap buffers")
 *	-M	at most this much guest memory mapped at a time (default: no
 *		limit)
 *	-P	after each request, poll the ring and the workers' queues for
 *		that long before blocking (vqda_proto.h "Poll mode")
 *	-v	log every request
 *
 * Policy enforced here, not trusted to the guest:
 *  - only the DSPs named with -d (default: none) may be opened; QUERY
 *    names them "cdsp", "cdsp1", ... in that order;
 *  - DSP processes are unsigned and run the host's shell (-s), whatever
 *    ELF the guest names; attaching to root, sensors or audio PDs and
 *    invoking the process init handle are refused;
 *  - requests, argument arrays included, are copied out of the shared
 *    ring before use;
 *  - a session is closed only once no request uses it, so a request
 *    never reaches another session's file.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
/* types the Xen uapi headers leave to userspace */
typedef uint16_t domid_t;
typedef uint32_t grant_ref_t;
#include <xen/evtchn.h>
#include <xen/gntdev.h>

#include "vqda_proto.h"
#include "vqda_xs.h"
#include "xen-static-shm.h"

#define MAX_SESSIONS 64
#define MAX_DSPS 16
#define PAGE 4096
/*
 * The DSP's process init handle: an invocation on it creates or attaches
 * DSP processes behind every check of SESSION_CREATE and SESSION_ATTACH.
 */
#define QDA_INIT_HANDLE 1

#define SESSION_OPENING (-2)	/* fd of a slot taken by an OPEN in progress */

struct session {
	int fd;			/* host accel file, -1 when free */
	int vidx;		/* position of the DSP in the -d list */
	uint32_t shell;		/* host GEM handle of the shell */
	uint32_t shell_len;
	int refs;		/* requests using the session */
	int closing;		/* closed by the guest: the last request closes fd */
};

static int domid, gnt, evt, port, verbose;
static const char *allow = "";
static const char *shell_path = "/usr/lib/dsp/cdsp/fastrpc_shell_unsigned";
static struct vqda_slot *ring;
static struct session sess[MAX_SESSIONS];
static pthread_mutex_t sess_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t work_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER;
static int work_pending[VQDA_SLOTS];
/*
 * A worker can sit in a DSP call that never returns (a guest skel that
 * loops, a client that exited mid-call). The guest frees such a slot
 * only by zeroing the ring for its next client, which then sends
 * VQDA_OP_RESET: the call is cancelled by interrupting its worker (the
 * ioctl returns EINTR and closing the session makes QDA tear the DSP
 * process down), and a result that arrives after the slot changed hands
 * is dropped (epoch).
 */
static pthread_t wtid[VQDA_SLOTS];
static int wbusy[VQDA_SLOTS];
static unsigned int wepoch[VQDA_SLOTS];
static unsigned int wcall[VQDA_SLOTS];	/* epoch the worker's call runs under */
static __thread int my_slot = -1;
static unsigned int poll_us;
static struct vqda_ctrl *ctrl;
static unsigned long nrefused;

/*
 * Log a line the guest can cause: at most one a second, with the count of
 * the lines left out since the last one.
 */
static void __attribute__((format(printf, 1, 2))) logrl(const char *fmt, ...)
{
	static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
	static unsigned long quiet;
	static time_t last = -1;
	struct timespec now;
	va_list ap;

	pthread_mutex_lock(&m);
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (now.tv_sec == last) {
		quiet++;
	} else {
		last = now.tv_sec;
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
		if (quiet)
			printf(" (%lu lines not logged)", quiet);
		printf("\n");
		fflush(stdout);
		quiet = 0;
	}
	pthread_mutex_unlock(&m);
}

static void on_cancel(int sig)
{
	(void)sig;
}

/* Whether worker @i is in a call from before epoch @target. */
static int stale_call(int i, unsigned int target)
{
	return __atomic_load_n(&wbusy[i], __ATOMIC_SEQ_CST) &&
	       (int)(__atomic_load_n(&wcall[i], __ATOMIC_SEQ_CST) - target) < 0;
}

/*
 * Interrupt worker @i until it has left its call from before epoch
 * @target (at most ~5 s); a call the worker started since is left alone.
 */
static void cancel_worker(int i, unsigned int target)
{
	int k;

	for (k = 0; k < 500; k++) {
		int stale;

		/* the worker starts a call under work_lock: no new call in between */
		pthread_mutex_lock(&work_lock);
		stale = stale_call(i, target);
		if (stale)
			pthread_kill(wtid[i], SIGUSR1);
		pthread_mutex_unlock(&work_lock);
		if (!stale)
			break;
		usleep(10000);
	}
	logrl("VQDABACK d%d slot %d call cancelled%s", domid, i,
	      stale_call(i, target) ? " (still in the call)" : "");
}

/*
 * Once the client has died the guest revokes its grants, so every ref a
 * request names fails to map, one grant op each. Requests still queued or
 * arriving before the restart are refused instead of mapped.
 */
static int client_gone(void)
{
	if (!ctrl || __atomic_load_n(&ctrl->client_alive, __ATOMIC_ACQUIRE) != VQDA_CLIENT_GONE)
		return 0;
	__atomic_fetch_add(&nrefused, 1, __ATOMIC_RELAXED);
	return 1;
}

/*
 * The host's result as the guest's ioctl would see it: -errno for a failed
 * call, otherwise the value itself (QDA returns DSP status codes, which are
 * neither 0 nor in the errno range, as the ioctl result).
 */
static int xioctl(int fd, unsigned long req, void *arg)
{
	int r = ioctl(fd, req, arg);

	return r == -1 ? -errno : r;
}

static uint64_t now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static void start_thread(void *(*fn)(void *), void *arg)
{
	pthread_t t;

	if (pthread_create(&t, NULL, fn, arg)) {
		fprintf(stderr, "vqda-back: d%d no thread\n", domid);
		exit(1);
	}
}

static void notify(void)
{
	struct ioctl_evtchn_notify n = { .port = port };

	ioctl(evt, IOCTL_EVTCHN_NOTIFY, &n);
}

/* Position of @dsp in the -d list, or -1 when the guest may not use it. */
static int allowed(const char *dsp)
{
	size_t n = strnlen(dsp, 16);
	const char *p = allow;
	int i = 0;

	if (!n || n == 16)
		return -1;
	while (*p) {
		const char *e = strchr(p, ',');
		size_t l = e ? (size_t)(e - p) : strlen(p);

		if (l == n && !strncmp(p, dsp, n))
			return i;
		p += l + (e ? 1 : 0);
		i++;
	}
	return -1;
}

/* Whether host accel file @f is the DSP @dsp (16 bytes, NUL padded). */
static int is_dsp(int f, const char *dsp)
{
	struct drm_qda_query q = { .query_type = QDA_QUERY_DSP_NAME };

	return !ioctl(f, DRM_IOCTL_QDA_QUERY, &q) &&
	       !strncmp((char *)q.dsp_name, dsp, 16);
}

/*
 * Open the next /dev/accel device of the scan in @d, its path in @p; -1
 * when the scan is done.
 */
static int next_accel(DIR *d, char *p, size_t len)
{
	struct dirent *e;
	int f;

	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "accel", 5))
			continue;
		snprintf(p, len, "/dev/accel/%s", e->d_name);
		f = open(p, O_RDWR | O_CLOEXEC);
		if (f >= 0)
			return f;
	}
	return -1;
}

static char dsp_path[MAX_DSPS][300];
static pthread_mutex_t dsp_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * The host accel device of @dsp, position @v in the -d list. Only the
 * first open scans the devices: the path found is kept, so a guest's
 * opens touch no other DSP's device. It is checked again on every open,
 * since devices can be renumbered.
 */
static int open_dsp(const char *dsp, int v)
{
	char p[300];
	DIR *d;
	int f;

	pthread_mutex_lock(&dsp_lock);
	if (v < MAX_DSPS && dsp_path[v][0]) {
		f = open(dsp_path[v], O_RDWR | O_CLOEXEC);
		if (f >= 0 && is_dsp(f, dsp))
			goto out;
		if (f >= 0)
			close(f);
	}
	f = -ENODEV;
	d = opendir("/dev/accel");
	while (d && (f = next_accel(d, p, sizeof(p))) >= 0) {
		if (is_dsp(f, dsp)) {
			if (v < MAX_DSPS)
				memcpy(dsp_path[v], p, sizeof(p));
			break;
		}
		close(f);
	}
	if (d)
		closedir(d);
	if (f < 0)
		f = -ENODEV;
out:
	pthread_mutex_unlock(&dsp_lock);
	return f;
}

static int list_dsps(void)
{
	char p[300];
	DIR *d = opendir("/dev/accel");
	int f;

	while (d && (f = next_accel(d, p, sizeof(p))) >= 0) {
		struct drm_qda_query q = { .query_type = QDA_QUERY_DSP_NAME };

		if (!ioctl(f, DRM_IOCTL_QDA_QUERY, &q))
			printf("%.16s\n", (char *)q.dsp_name);
		close(f);
	}
	if (d)
		closedir(d);
	return 0;
}

/*
 * Guest pages this back end keeps mapped: a GEM_CREATE maps its grants
 * until its handle is closed, in the hardware domain's maptrack, which
 * every back end in the domain shares, and gntdev's limit only bounds one
 * request. -M caps the total. A shared buffer counts once per handle,
 * since either handle keeps the mapping alive.
 */
#define MAX_MAPS 16384
#define MAP_RESERVED UINT32_MAX
static struct {
	uint32_t sid, handle, npages;	/* npages 0: free entry */
	int dsp_mapped;		/* stays charged after its handle closes */
} maps[MAX_MAPS];
static uint64_t mapped_pages, max_mapped_pages;
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
/*
 * Held from a host call that makes or closes a grant-backed handle to its
 * map table update: DRM reuses handle numbers, so a close and the import
 * that gets its number back must not interleave.
 */
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

/* Take room for @npages; the entry to commit or cancel, or -errno. */
static int map_reserve(uint32_t npages)
{
	int i, ret = -ENOMEM;

	pthread_mutex_lock(&map_lock);
	if (max_mapped_pages && mapped_pages + npages > max_mapped_pages) {
		ret = -ENOSPC;
		goto out;
	}
	for (i = 0; i < MAX_MAPS; i++)
		if (!maps[i].npages) {
			maps[i].sid = MAP_RESERVED;
			maps[i].npages = npages;
			mapped_pages += npages;
			ret = i;
			break;
		}
out:
	pthread_mutex_unlock(&map_lock);
	return ret;
}

static void map_commit(int i, uint32_t sid, uint32_t handle)
{
	pthread_mutex_lock(&map_lock);
	maps[i].sid = sid;
	maps[i].handle = handle;
	pthread_mutex_unlock(&map_lock);
}

static void map_free_locked(int i)
{
	mapped_pages -= maps[i].npages;
	maps[i].npages = 0;
	maps[i].dsp_mapped = 0;
}

static void map_cancel(int i)
{
	pthread_mutex_lock(&map_lock);
	map_free_locked(i);
	pthread_mutex_unlock(&map_lock);
}

/* Pages handle @handle of session @sid maps, 0 if none (or a heap buffer). */
static uint32_t map_pages(uint32_t sid, uint32_t handle)
{
	uint32_t n = 0;
	int i;

	pthread_mutex_lock(&map_lock);
	for (i = 0; i < MAX_MAPS; i++)
		if (maps[i].npages && maps[i].sid == sid && maps[i].handle == handle)
			n = maps[i].npages;
	pthread_mutex_unlock(&map_lock);
	return n;
}

/*
 * Forget handle @handle of session @sid, or with @handle 0 all of @sid's.
 * A buffer the DSP has mapped keeps its pages mapped after its handle
 * closes, so it stays charged, without a handle, until the session goes.
 */
static void map_drop(uint32_t sid, uint32_t handle)
{
	int i;

	pthread_mutex_lock(&map_lock);
	for (i = 0; i < MAX_MAPS; i++) {
		if (!maps[i].npages || maps[i].sid != sid)
			continue;
		if (!handle)
			map_free_locked(i);
		else if (maps[i].handle == handle && maps[i].dsp_mapped)
			maps[i].handle = 0;
		else if (maps[i].handle == handle)
			map_free_locked(i);
	}
	pthread_mutex_unlock(&map_lock);
}

/* Handle @handle of session @sid is mapped on the DSP. */
static void map_dsp(uint32_t sid, uint32_t handle)
{
	int i;

	pthread_mutex_lock(&map_lock);
	for (i = 0; i < MAX_MAPS; i++)
		if (maps[i].npages && maps[i].sid == sid && maps[i].handle == handle)
			maps[i].dsp_mapped = 1;
	pthread_mutex_unlock(&map_lock);
}

/* A reference on session @sid for one request, or NULL. */
static struct session *get(uint32_t sid)
{
	struct session *s = NULL;

	pthread_mutex_lock(&sess_lock);
	if (sid < MAX_SESSIONS && sess[sid].fd >= 0 && !sess[sid].closing) {
		s = &sess[sid];
		s->refs++;
	}
	pthread_mutex_unlock(&sess_lock);
	return s;
}

/*
 * Free closed session @s once no request uses it, with sess_lock held;
 * returns the file to close after unlocking, or -1.
 */
static int reap_locked(struct session *s)
{
	int fd = s->fd;

	if (!s->closing || s->refs)
		return -1;
	map_drop(s - sess, 0);
	s->fd = -1;
	s->closing = 0;
	s->shell = 0;
	s->shell_len = 0;
	return fd;
}

static void put(struct session *s)
{
	int fd;

	if (!s)
		return;
	pthread_mutex_lock(&sess_lock);
	s->refs--;
	fd = reap_locked(s);
	pthread_mutex_unlock(&sess_lock);
	if (fd >= 0)
		close(fd);
}

static int map_grants(const uint32_t *refs, uint32_t n, int prot, void **va,
		      uint64_t *index)
{
	struct ioctl_gntdev_map_grant_ref *m;
	uint32_t i;
	void *p;

	m = calloc(1, sizeof(*m) + n * sizeof(m->refs[0]));
	if (!m)
		return -ENOMEM;
	m->count = n;
	for (i = 0; i < n; i++) {
		m->refs[i].domid = domid;
		m->refs[i].ref = refs[i];
	}
	if (ioctl(gnt, IOCTL_GNTDEV_MAP_GRANT_REF, m)) {
		int e = -errno;

		free(m);
		return e;
	}
	*index = m->index;
	free(m);
	p = mmap(NULL, (size_t)n * PAGE, prot, MAP_SHARED, gnt, *index);
	if (p == MAP_FAILED) {
		struct ioctl_gntdev_unmap_grant_ref u = { .index = *index,
							  .count = n };
		int e = -errno;

		ioctl(gnt, IOCTL_GNTDEV_UNMAP_GRANT_REF, &u);
		return e;
	}
	*va = p;
	return 0;
}

static void unmap_grants(void *va, uint32_t n, uint64_t index)
{
	struct ioctl_gntdev_unmap_grant_ref u = { .index = index, .count = n };

	munmap(va, (size_t)n * PAGE);
	ioctl(gnt, IOCTL_GNTDEV_UNMAP_GRANT_REF, &u);
}

static int gem_create(struct session *s, struct vqda_slot *r)
{
	uint64_t size = r->u.gem.size, index = 0;
	uint32_t npages = r->u.gem.npages, nlist = r->u.gem.nlist;
	struct ioctl_gntdev_dmabuf_exp_from_refs *x;
	struct drm_prime_handle ph = { 0 };
	uint32_t lrefs[VQDA_MAX_LIST];
	uint32_t *list = NULL;
	int ret, m;

	if (!size || npages != (size + PAGE - 1) / PAGE || !nlist ||
	    nlist > VQDA_MAX_LIST ||
	    nlist != (npages + VQDA_REFS_PER_PAGE - 1) / VQDA_REFS_PER_PAGE)
		return -EINVAL;
	memcpy(lrefs, r->u.gem.list, nlist * sizeof(lrefs[0]));
	if (client_gone())
		return -ECONNRESET;
	m = map_reserve(npages);
	if (m < 0)
		return m;
	ret = map_grants(lrefs, nlist, PROT_READ, (void **)&list, &index);
	if (ret) {
		map_cancel(m);
		return ret;
	}
	x = calloc(1, sizeof(*x) + npages * sizeof(x->refs[0]));
	if (!x) {
		unmap_grants(list, nlist, index);
		map_cancel(m);
		return -ENOMEM;
	}
	memcpy(x->refs, list, npages * sizeof(x->refs[0]));
	unmap_grants(list, nlist, index);
	x->count = npages;
	x->domid = domid;
	if (client_gone()) {
		free(x);
		map_cancel(m);
		return -ECONNRESET;
	}
	if (ioctl(gnt, IOCTL_GNTDEV_DMABUF_EXP_FROM_REFS, x)) {
		ret = -errno;
		free(x);
		map_cancel(m);
		return ret;
	}
	ph.fd = x->fd;
	free(x);
	pthread_mutex_lock(&handle_lock);
	ret = xioctl(s->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph);
	if (ret)
		map_cancel(m);
	else
		map_commit(m, s - sess, ph.handle);
	pthread_mutex_unlock(&handle_lock);
	close(ph.fd);
	if (ret)
		return ret;
	r->u.gem.handle = ph.handle;
	return 0;
}

/*
 * Load the shell into a buffer of host accel file @fd, before the session
 * is published: no request can name the handle until it is protected.
 */
static int load_shell(int fd, uint32_t *handle, uint32_t *len)
{
	struct drm_qda_gem_create c = { 0 };
	struct drm_qda_gem_mmap_offset o = { 0 };
	struct stat st;
	void *p;
	int f, ret = 0;

	f = open(shell_path, O_RDONLY | O_CLOEXEC);
	if (f < 0)
		return -errno;
	if (fstat(f, &st) || st.st_size <= 0 || st.st_size > (64 << 20)) {
		close(f);
		return -EINVAL;
	}
	c.size = st.st_size;
	if (ioctl(fd, DRM_IOCTL_QDA_GEM_CREATE, &c)) {
		close(f);
		return -errno;
	}
	o.handle = c.handle;
	if (ioctl(fd, DRM_IOCTL_QDA_GEM_MMAP_OFFSET, &o)) {
		ret = -errno;
		goto out;
	}
	p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 o.offset);
	if (p == MAP_FAILED) {
		ret = -errno;
		goto out;
	}
	if (pread(f, p, c.size, 0) != (ssize_t)c.size)
		ret = -EIO;
	munmap(p, c.size);
out:
	close(f);
	if (ret) {
		struct drm_gem_close g = { .handle = c.handle };

		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &g);
		return ret;
	}
	*handle = c.handle;
	*len = c.size;
	return 0;
}

/*
 * Heap buffers: first fit over the guest's region. Buffers of 2 MiB and
 * more start on a 2 MiB boundary, so the SMMU can map them with blocks.
 */
#define MAX_HBUF 4096
struct hbuf {
	uint64_t off, size;	/* size 0 = free entry */
	int fd;			/* DMA-BUF of the slice */
};
static struct hbuf hb[MAX_HBUF];
/*
 * Ranges the guest freed: a session may still hold one imported, which
 * the driver reports as -EBUSY until QDA lets it go. They are only search
 * hints (their bounds are candidates); a successful export over one
 * retires it.
 */
#define MAX_ZOMB 1024
static struct { uint64_t off, size; } zb[MAX_ZOMB];
static unsigned int zb_next;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static int heap_fd = -1;
static uint64_t heap_size;
static const char *heap_id;

static int heap_fits(uint64_t off, uint64_t size)
{
	int i;

	if (off > heap_size || size > heap_size - off)
		return 0;
	for (i = 0; i < MAX_HBUF; i++)
		if (hb[i].size && off < hb[i].off + hb[i].size && hb[i].off < off + size)
			return 0;
	return 1;
}

static int heap_alloc(struct vqda_slot *r)
{
	uint64_t size = (r->u.heap.size + PAGE - 1) & ~(uint64_t)(PAGE - 1);
	uint64_t a = size >= (2u << 20) ? 2u << 20 : PAGE, off = 0;
	struct xshm_export x = { 0 };
	int i, b = -1, found = 0;

	if (heap_fd < 0)
		return -ENODEV;
	if (!size || size > heap_size)
		return -EINVAL;
	pthread_mutex_lock(&heap_lock);
	for (i = 0; i < MAX_HBUF && b < 0; i++)
		if (!hb[i].size)
			b = i;
	/*
	 * candidates: the start of the region, the end of every buffer, the
	 * start and end of every freed range
	 */
	for (i = -1; b >= 0 && i < MAX_HBUF + 2 * MAX_ZOMB && !found; i++) {
		if (i < 0)
			off = 0;
		else if (i < MAX_HBUF && hb[i].size)
			off = hb[i].off + hb[i].size;
		else if (i >= MAX_HBUF && zb[(i - MAX_HBUF) / 2].size)
			off = zb[(i - MAX_HBUF) / 2].off +
			      ((i - MAX_HBUF) & 1 ? zb[(i - MAX_HBUF) / 2].size : 0);
		else
			continue;
		off = (off + a - 1) & ~(a - 1);
		if (!heap_fits(off, size))
			continue;
		x.offset = off;
		x.size = size;
		if (!ioctl(heap_fd, XSHM_IOCTL_EXPORT, &x))
			found = 1;
		else if (errno != EBUSY) {
			int e = -errno;

			pthread_mutex_unlock(&heap_lock);
			return e;
		}
	}
	if (b < 0 || !found) {
		pthread_mutex_unlock(&heap_lock);
		return -ENOMEM;
	}
	hb[b].off = off;
	hb[b].size = size;
	hb[b].fd = x.fd;
	for (i = 0; i < MAX_ZOMB; i++)
		if (zb[i].size && off < zb[i].off + zb[i].size && zb[i].off < off + size)
			zb[i].size = 0;
	pthread_mutex_unlock(&heap_lock);
	r->u.heap.bid = b;
	r->u.heap.off = off;
	return 0;
}

static void heap_free_locked(uint32_t b)
{
	unsigned int z;

	close(hb[b].fd);
	/* an empty hint slot first, else the oldest hint */
	for (z = 0; z < MAX_ZOMB && zb[z].size; z++)
		;
	if (z == MAX_ZOMB) {
		z = zb_next;
		zb_next = (zb_next + 1) % MAX_ZOMB;
	}
	zb[z].off = hb[b].off;
	zb[z].size = hb[b].size;
	hb[b].size = 0;
}

static int heap_free(uint32_t b)
{
	int ret = -EBADF;

	pthread_mutex_lock(&heap_lock);
	if (b < MAX_HBUF && hb[b].size) {
		heap_free_locked(b);
		ret = 0;
	}
	pthread_mutex_unlock(&heap_lock);
	return ret;
}

static int heap_import(struct session *s, struct vqda_slot *r)
{
	struct drm_prime_handle ph = { 0 };
	uint32_t b = r->u.heap.bid;
	int ret;

	pthread_mutex_lock(&heap_lock);
	if (b >= MAX_HBUF || !hb[b].size) {
		pthread_mutex_unlock(&heap_lock);
		return -EBADF;
	}
	ph.fd = hb[b].fd;
	ret = xioctl(s->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph);
	pthread_mutex_unlock(&heap_lock);
	if (!ret)
		r->u.heap.handle = ph.handle;
	return ret;
}

/*
 * Copy a request out of the ring. The guest may rewrite the slot while
 * this runs, by design: everything after works on the copy.
 */
static void __attribute__((noinline)) copy_from_guest(void *dst, const void *src, size_t n)
{
	memcpy(dst, src, n);
}

/* VQDA_OP_RESET: cancel every call of an earlier client, close everything. */
static int reset(void)
{
	int i, fds[MAX_SESSIONS], n = 0;

	for (i = 0; i < VQDA_SLOTS; i++)
		if (i != my_slot && __atomic_load_n(&wbusy[i], __ATOMIC_SEQ_CST))
			cancel_worker(i, __atomic_add_fetch(&wepoch[i], 1, __ATOMIC_SEQ_CST));
	pthread_mutex_lock(&sess_lock);
	for (i = 0; i < MAX_SESSIONS; i++)
		if (sess[i].fd >= 0 && !sess[i].closing) {
			sess[i].closing = 1;
			fds[n] = reap_locked(&sess[i]);
			n += fds[n] >= 0;
		}
	pthread_mutex_unlock(&sess_lock);
	while (n)
		close(fds[--n]);
	pthread_mutex_lock(&heap_lock);
	for (i = 0; i < MAX_HBUF; i++)
		if (hb[i].size)
			heap_free_locked(i);
	pthread_mutex_unlock(&heap_lock);
	return 0;
}

static int open_session(struct vqda_slot *r)
{
	uint32_t shell = 0, len = 0;
	char dsp[16];
	int fd, v, i, ret;

	memcpy(dsp, r->u.open.dsp, sizeof(dsp));
	v = allowed(dsp);
	if (v < 0)
		return -EACCES;
	/* a slot first: a guest at its limit costs no device open or shell load */
	pthread_mutex_lock(&sess_lock);
	for (i = 0; i < MAX_SESSIONS && sess[i].fd != -1; i++)
		;
	if (i < MAX_SESSIONS)
		sess[i].fd = SESSION_OPENING;
	pthread_mutex_unlock(&sess_lock);
	if (i == MAX_SESSIONS)
		return -EMFILE;
	fd = open_dsp(dsp, v);
	ret = fd < 0 ? fd : load_shell(fd, &shell, &len);
	pthread_mutex_lock(&sess_lock);
	if (ret)
		sess[i].fd = -1;
	else
		sess[i] = (struct session){ .fd = fd, .vidx = v, .shell = shell,
					    .shell_len = len };
	pthread_mutex_unlock(&sess_lock);
	if (ret) {
		if (fd >= 0)
			close(fd);
		return ret;
	}
	r->sid = i;
	return 0;
}

/* GEM_SHARE: import buffer src_handle of session src_sid into @s. */
static int share(struct session *s, struct vqda_slot *r)
{
	struct drm_prime_handle ex = { 0 }, im = { 0 };
	struct session *src;
	uint32_t n;
	int ret, m = -1;

	src = get(r->u.share.src_sid);
	if (!src)
		return -EBADF;
	if (r->u.share.src_handle == src->shell) {
		put(src);
		return -EPERM;
	}
	pthread_mutex_lock(&handle_lock);
	n = map_pages(src - sess, r->u.share.src_handle);
	if (n) {
		m = map_reserve(n);
		ret = m;
		if (m < 0)
			goto out;
	}
	ex.handle = r->u.share.src_handle;
	ex.flags = O_RDWR | O_CLOEXEC;
	ret = xioctl(src->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ex);
	if (ret)
		goto out;
	im.fd = ex.fd;
	ret = xioctl(s->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &im);
	close(ex.fd);
	if (ret)
		goto out;
	r->u.share.handle = im.handle;
	/* an import of an object the session has returns the same handle */
	if (m >= 0 && !map_pages(s - sess, im.handle)) {
		map_commit(m, s - sess, im.handle);
		m = -1;
	}
out:
	if (m >= 0)
		map_cancel(m);
	pthread_mutex_unlock(&handle_lock);
	put(src);
	return ret;
}

static int serve(struct session *s, struct vqda_slot *r)
{
	uint32_t sid = s ? (uint32_t)(s - sess) : 0;
	int ret, i;

	switch (r->op) {
	case VQDA_OP_PING:
		return 0;
	case VQDA_OP_RESET:
		return reset();
	case VQDA_OP_HEAP_ALLOC:
		return heap_alloc(r);
	case VQDA_OP_HEAP_FREE:
		return heap_free(r->u.heap.bid);
	case VQDA_OP_HEAP_IMPORT:
		return heap_import(s, r);
	case VQDA_OP_OPEN:
		return open_session(r);
	case VQDA_OP_CLOSE:
		/* the file closes when this request drops its reference */
		pthread_mutex_lock(&sess_lock);
		s->closing = 1;
		pthread_mutex_unlock(&sess_lock);
		return 0;
	case VQDA_OP_QUERY: {
		struct drm_qda_query q;

		memcpy(&q, &r->u.query, sizeof(q));
		ret = xioctl(s->fd, DRM_IOCTL_QDA_QUERY, &q);
		if (!ret && q.query_type == QDA_QUERY_DSP_NAME) {
			memset(q.dsp_name, 0, sizeof(q.dsp_name));
			if (s->vidx)
				snprintf((char *)q.dsp_name, sizeof(q.dsp_name),
					 "cdsp%d", s->vidx);
			else
				strcpy((char *)q.dsp_name, "cdsp");
		}
		if (!ret)
			memcpy(&r->u.query, &q, sizeof(q));
		return ret;
	}
	case VQDA_OP_GEM_CREATE:
		return gem_create(s, r);
	case VQDA_OP_GEM_CLOSE: {
		struct drm_gem_close g = { .handle = r->u.gem.handle };

		if (g.handle == s->shell)
			return -EPERM;
		pthread_mutex_lock(&handle_lock);
		ret = xioctl(s->fd, DRM_IOCTL_GEM_CLOSE, &g);
		if (!ret)
			map_drop(sid, g.handle);
		pthread_mutex_unlock(&handle_lock);
		return ret;
	}
	case VQDA_OP_INVOKE: {
		struct vqda_arg args[VQDA_MAX_ARGS];
		struct drm_qda_invoke_args inv;
		uint32_t n;

		inv.handle = r->u.invoke.handle;
		inv.sc = r->u.invoke.sc;
		n = r->u.invoke.nargs;
		if (inv.handle == QDA_INIT_HANDLE)
			return -EPERM;
		if (n > VQDA_MAX_ARGS || (int)n != vqda_sc_nargs(inv.sc))
			return -EINVAL;
		memcpy(args, r->u.invoke.args, n * sizeof(args[0]));
		for (i = 0; i < (int)n; i++)
			if (args[i].handle == s->shell)
				return -EPERM;
		inv.args = (uintptr_t)args;
		return xioctl(s->fd, DRM_IOCTL_QDA_REMOTE_INVOKE, &inv);
	}
	case VQDA_OP_SESSION_CREATE: {
		struct drm_qda_init_create c = { 0 };

		if (!(r->u.create.attrs & QDA_INIT_ATTR_UNSIGNED_MODULE))
			return -EPERM;
		c.filehandle = s->shell;
		c.filelen = s->shell_len;
		/* the only attribute the QDA UAPI defines */
		c.attrs = QDA_INIT_ATTR_UNSIGNED_MODULE;
		return xioctl(s->fd, DRM_IOCTL_QDA_REMOTE_SESSION_CREATE, &c);
	}
	case VQDA_OP_SESSION_ATTACH:
		return -EPERM;
	case VQDA_OP_REMOTE_MMAP: {
		struct drm_qda_mem_map m;

		memcpy(&m, &r->u.map, sizeof(m));
		if (m.handle == s->shell)
			return -EPERM;
		pthread_mutex_lock(&handle_lock);
		ret = xioctl(s->fd, DRM_IOCTL_QDA_REMOTE_MMAP, &m);
		if (!ret)
			map_dsp(sid, m.handle);
		pthread_mutex_unlock(&handle_lock);
		memcpy(&r->u.map, &m, sizeof(m));
		return ret;
	}
	case VQDA_OP_REMOTE_MUNMAP: {
		struct drm_qda_mem_unmap m;

		memcpy(&m, &r->u.unmap, sizeof(m));
		ret = xioctl(s->fd, DRM_IOCTL_QDA_REMOTE_MUNMAP, &m);
		memcpy(&r->u.unmap, &m, sizeof(m));
		return ret;
	}
	case VQDA_OP_GEM_SHARE:
		return share(s, r);
	case VQDA_OP_SET_OPTION: {
		struct drm_qda_set_option o;

		memcpy(&o, &r->u.opt, sizeof(o));
		return xioctl(s->fd, DRM_IOCTL_QDA_SET_OPTION, &o);
	}
	}
	return -ENOSYS;
}

/* One request, on a copy of its slot: the session it names is held. */
static int handle(struct vqda_slot *r)
{
	struct session *s = NULL;
	int ret;

	if (client_gone())
		return -ECONNRESET;
	if (r->op != VQDA_OP_OPEN && r->op != VQDA_OP_PING &&
	    r->op != VQDA_OP_RESET && r->op != VQDA_OP_HEAP_ALLOC &&
	    r->op != VQDA_OP_HEAP_FREE) {
		s = get(r->sid);
		if (!s)
			return -EBADF;
	}
	ret = serve(s, r);
	put(s);
	return ret;
}

static void *worker(void *arg)
{
	int i = (int)(uintptr_t)arg;
	struct vqda_slot loc;
	unsigned int e;

	my_slot = i;
	wtid[i] = pthread_self();
	for (;;) {
		struct vqda_slot *r = (void *)((char *)ring + i * VQDA_SLOT_SIZE);
		uint32_t busy = VQDA_BUSY;
		int done;

		if (poll_us) {
			uint64_t end = now_us() + poll_us;

			while (!__atomic_load_n(&work_pending[i], __ATOMIC_ACQUIRE) && now_us() < end)
				;
		}
		pthread_mutex_lock(&work_lock);
		while (!work_pending[i])
			pthread_cond_wait(&work_cond, &work_lock);
		work_pending[i] = 0;
		e = __atomic_load_n(&wepoch[i], __ATOMIC_SEQ_CST);
		__atomic_store_n(&wcall[i], e, __ATOMIC_SEQ_CST);
		__atomic_store_n(&wbusy[i], 1, __ATOMIC_SEQ_CST);
		pthread_mutex_unlock(&work_lock);

		/* hand back only the result fields of the copy */
		copy_from_guest(&loc, r, sizeof(loc));
		loc.ret = handle(&loc);
		/*
		 * Under work_lock, so dispatch cannot hand the slot to a new
		 * request in between: a cancelled call, or a slot the guest
		 * took back (zeroed for its next client), gets no result.
		 */
		pthread_mutex_lock(&work_lock);
		done = __atomic_load_n(&wepoch[i], __ATOMIC_SEQ_CST) == e &&
		       __atomic_load_n(&r->state, __ATOMIC_SEQ_CST) == VQDA_BUSY;
		if (done) {
			r->sid = loc.sid;
			memcpy((void *)&r->u, &loc.u, sizeof(loc.u));
			r->ret = loc.ret;
			done = __atomic_compare_exchange_n((uint32_t *)&r->state, &busy,
							   VQDA_DONE, 0, __ATOMIC_SEQ_CST,
							   __ATOMIC_SEQ_CST);
		}
		__atomic_store_n(&wbusy[i], 0, __ATOMIC_SEQ_CST);
		pthread_mutex_unlock(&work_lock);
		if (!done)
			continue;
		if (verbose) {
			printf("VQDAREQ d%d slot %d op %u sid %u ret %d\n", domid, i, loc.op, loc.sid, loc.ret);
			fflush(stdout);
		}
		if (!__atomic_load_n(&ctrl->guest_polling[i], __ATOMIC_SEQ_CST))
			notify();
	}
	return NULL;
}

/* Hand every slot in VQDA_REQ to its worker; returns how many it found. */
static int dispatch(void)
{
	int i, n = 0;

	for (i = 0; i < VQDA_SLOTS; i++) {
		struct vqda_slot *r = (void *)((char *)ring + i * VQDA_SLOT_SIZE);
		uint32_t want = VQDA_REQ;
		unsigned int target = 0;
		int busy = 0;

		/* sequentially consistent: poll mode orders it after back_polling */
		if (__atomic_load_n(&r->state, __ATOMIC_SEQ_CST) != VQDA_REQ)
			continue;
		pthread_mutex_lock(&work_lock);
		if (__atomic_compare_exchange_n((uint32_t *)&r->state, &want, VQDA_BUSY,
						0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
			/* the worker is still in a call of an earlier client */
			busy = __atomic_load_n(&wbusy[i], __ATOMIC_SEQ_CST);
			if (busy)
				target = __atomic_add_fetch(&wepoch[i], 1, __ATOMIC_SEQ_CST);
			__atomic_store_n(&work_pending[i], 1, __ATOMIC_RELEASE);
			pthread_cond_broadcast(&work_cond);
			n++;
		}
		pthread_mutex_unlock(&work_lock);
		if (busy)
			cancel_worker(i, target);
	}
	return n;
}

static uint64_t ring_pa;
static int static_port, use_xs;

/* Map the static ring and bind the static event channel (vqda_proto.h). */
static int connect_guest(void)
{
	struct ioctl_evtchn_bind b = { .port = static_port };
	int fd, ret, i;

	fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ring = mmap(NULL, VQDA_MAP_SIZE, PROT_READ | PROT_WRITE,
		    MAP_SHARED, fd, ring_pa);
	close(fd);
	if (ring == MAP_FAILED)
		return -errno;
	/* EBUSY: an earlier back end still holds the port, wait for it */
	for (i = 0; (ret = ioctl(evt, IOCTL_EVTCHN_BIND_STATIC, &b)) < 0 &&
		    errno == EBUSY && i < 600; i++)
		usleep(50000);
	if (ret < 0)
		return -errno;
	port = static_port;
	ctrl = ctrl_of(ring);
	ctrl->back_polling = 0;
	ctrl->back_magic = VQDA_BACK_MAGIC;
	notify();
	return 0;
}

static char **self_argv;
static char xs_fe[96], xs_be[160];
static unsigned long xs_gen;
static uint64_t start_us;

/*
 * Start over: a new process closes every session of the old client. The
 * guest decides how often this happens, so at most once a second.
 */
static void restart(void)
{
	uint64_t t = now_us();

	if (t < start_us + 1000000)
		usleep(start_us + 1000000 - t);
	/* heap DMA-BUFs cannot be close-on-exec */
	pthread_mutex_lock(&heap_lock);
	for (int i = 0; i < MAX_HBUF; i++)
		if (hb[i].size)
			close(hb[i].fd);
	execv("/proc/self/exe", self_argv);
	_exit(1);
}

/*
 * -x: one back-end process serves one ring generation. A new generation
 * (a new guest client) re-executes it, which closes every session of the
 * old one, and so does the old client ending (client_alive dropping):
 * nothing else would close its sessions or end an invocation it left
 * waiting on the DSP. The guest's frontend node going away ends it.
 */
static void *xs_watch(void *arg)
{
	char v[32], p[128];
	int xs = (int)(uintptr_t)arg, r;

	snprintf(p, sizeof(p), "%s/ring-gen", xs_fe);
	for (;;) {
		usleep(100000);
		if (__atomic_load_n(&ctrl->client_alive, __ATOMIC_ACQUIRE) == VQDA_CLIENT_GONE) {
			char g[24];

			/* the dead client's nodes stay published: wait for the next one */
			snprintf(g, sizeof(g), "%lu", xs_gen);
			setenv("VQDA_XS_DEAD_GEN", g, 1);
			printf("VQDABACK d%d client gone, restarting (%lu requests refused)\n", domid,
			       __atomic_load_n(&nrefused, __ATOMIC_RELAXED));
			fflush(stdout);
			restart();
		}
		r = xs_read(xs, p, v, sizeof(v));
		if (r == -ENOENT || r == -EACCES) {
			printf("VQDABACK d%d frontend gone, exiting\n", domid);
			fflush(stdout);
			_exit(0);
		}
		if (r > 0 && strtoul(v, NULL, 10) != xs_gen) {
			printf("VQDABACK d%d ring-gen %lu after %lu, restarting\n", domid,
			       strtoul(v, NULL, 10), xs_gen);
			fflush(stdout);
			restart();
		}
	}
	return NULL;
}

/* @n comma separated grant refs, nothing else, from @v into @refs. */
static int parse_refs(const char *v, uint32_t n, uint32_t *refs)
{
	const char *q = v;
	uint32_t i;

	for (i = 0; i < n; i++) {
		unsigned long r;
		char *end;

		if (*q < '0' || *q > '9')
			return -EINVAL;
		r = strtoul(q, &end, 10);
		if (r > UINT32_MAX)
			return -EINVAL;
		refs[i] = r;
		q = end;
		if (i + 1 < n && *q++ != ',')
			return -EINVAL;
	}
	return *q ? -EINVAL : 0;
}

static int connect_xs(void)
{
	static char dsps[128];
	struct ioctl_gntdev_map_grant_ref *m;
	struct ioctl_evtchn_bind_interdomain b;
	char v[VQDA_MAP_SIZE / PAGE * 11 + 1], p[192], me[16], ev[24], g2[24];
	uint32_t refs[VQDA_MAP_SIZE / PAGE];
	unsigned int n = VQDA_MAP_SIZE / PAGE, i;
	unsigned long dead;
	int mapped;
	int xs, ret;

	xs = xs_open();
	if (xs < 0)
		return xs;
	/*
	 * The backend path is this domain's own, never what the guest wrote
	 * in its frontend: the guest owns that node, and a root back end
	 * writing where it says would write anywhere this domain may.
	 */
	if ((ret = xs_read(xs, "domid", me, sizeof(me))) <= 0) {
		fprintf(stderr, "vqda-back: d%d read domid: %d\n", domid, ret);
		return ret ? ret : -EIO;
	}
	snprintf(xs_fe, sizeof(xs_fe), "/local/domain/%d/" VQDA_XS_FE, domid);
	snprintf(xs_be, sizeof(xs_be), "/local/domain/%d/backend/vqda/%d/0", atoi(me), domid);
	snprintf(p, sizeof(p), "%s/frontend-id", xs_be);
	if ((ret = xs_read(xs, p, v, sizeof(v))) <= 0 || atoi(v) != domid) {
		fprintf(stderr, "vqda-back: d%d %s: %d\n", domid, p, ret);
		return -ENOENT;
	}
	if (!*allow) {
		snprintf(p, sizeof(p), "%s/dsps", xs_be);
		if (xs_read(xs, p, dsps, sizeof(dsps)) > 0)
			allow = dsps;
	}
	dead = getenv("VQDA_XS_DEAD_GEN") ? strtoul(getenv("VQDA_XS_DEAD_GEN"), NULL, 10) : 0;
	/*
	 * Wait for a client to publish a ring, one newer than a client this
	 * back end saw die. The nodes are read one at a time, so take them
	 * again if ring-gen moved meanwhile: a client is replacing them.
	 */
retry:
	for (;;) {
		snprintf(p, sizeof(p), "%s/state", xs_fe);
		ret = xs_read(xs, p, v, sizeof(v));
		if (ret == -ENOENT || ret == -EACCES) {
			/* only the frontend going away (the guest) ends the wait */
			snprintf(p, sizeof(p), "%s/backend-id", xs_fe);
			if (xs_read(xs, p, g2, sizeof(g2)) <= 0) {
				printf("VQDABACK d%d frontend gone, exiting\n", domid);
				fflush(stdout);
				_exit(0);
			}
			usleep(50000);
			continue;
		}
		snprintf(p, sizeof(p), "%s/ring-gen", xs_fe);
		if (ret > 0 && !strcmp(v, "3") && xs_read(xs, p, v, sizeof(v)) > 0 &&
		    (xs_gen = strtoul(v, NULL, 10)) != dead) {
			snprintf(p, sizeof(p), "%s/event-channel", xs_fe);
			ret = xs_read(xs, p, ev, sizeof(ev));
			snprintf(p, sizeof(p), "%s/ring-refs", xs_fe);
			ret = ret > 0 ? xs_read(xs, p, v, sizeof(v)) : ret;
			snprintf(p, sizeof(p), "%s/ring-gen", xs_fe);
			if (ret > 0 && xs_read(xs, p, g2, sizeof(g2)) > 0 &&
			    strtoul(g2, NULL, 10) == xs_gen)
				break;
		}
		usleep(50000);
	}
	b.remote_domain = domid;
	b.remote_port = strtoul(ev, NULL, 10);
	m = calloc(1, sizeof(*m) + n * sizeof(m->refs[0]));
	if (!m)
		return -ENOMEM;
	m->count = n;
	ret = parse_refs(v, n, refs);
	for (i = 0; !ret && i < n; i++) {
		m->refs[i].domid = domid;
		m->refs[i].ref = refs[i];
	}
	if (!ret && ioctl(gnt, IOCTL_GNTDEV_MAP_GRANT_REF, m))
		ret = -errno;
	mapped = !ret;
	ring = ret ? MAP_FAILED : mmap(NULL, VQDA_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, gnt, m->index);
	if (!ret && ring == MAP_FAILED)
		ret = -errno;
	if (!ret && (ret = ioctl(evt, IOCTL_EVTCHN_BIND_INTERDOMAIN, &b)) < 0)
		ret = -errno;
	if (ret < 0) {
		/* a client that died between publishing and now: wait for the next */
		struct ioctl_gntdev_unmap_grant_ref u = { .index = m->index, .count = n };

		if (ring != MAP_FAILED)
			munmap(ring, VQDA_MAP_SIZE);
		if (mapped)
			ioctl(gnt, IOCTL_GNTDEV_UNMAP_GRANT_REF, &u);
		free(m);
		ring = NULL;
		logrl("VQDABACK d%d ring-gen %lu unusable (%d), waiting for the next",
		      domid, xs_gen, ret);
		dead = xs_gen;
		goto retry;
	}
	free(m);
	unsetenv("VQDA_XS_DEAD_GEN");
	port = ret;
	ctrl = ctrl_of(ring);
	ctrl->back_polling = 0;
	ctrl->back_magic = VQDA_BACK_MAGIC;
	snprintf(v, sizeof(v), "%lu", xs_gen);
	snprintf(p, sizeof(p), "%s/ring-gen", xs_be);
	ret = xs_write(xs, p, v);
	snprintf(p, sizeof(p), "%s/state", xs_be);
	ret = ret ? ret : xs_write(xs, p, "4");
	if (ret)
		return ret;
	notify();
	start_thread(xs_watch, (void *)(uintptr_t)xs);
	return 0;
}

/* A number option in @base: digits only, at most @max. */
static int num_arg(const char *s, int base, unsigned long max, unsigned long *out)
{
	char *end;

	if (*s < '0' || *s > '9')
		return -1;
	errno = 0;
	*out = strtoul(s, &end, base);
	return errno || *end || *out > max ? -1 : 0;
}


int main(int argc, char **argv)
{
	unsigned long u;
	unsigned int orphaned = 0;
	int i, c;

	if (argc < 2)
		return 2;
	if (!strcmp(argv[1], "-l"))
		return list_dsps();
	start_us = now_us();
	if (num_arg(argv[1], 10, 0x7fef, &u))	/* DOMID_FIRST_RESERVED - 1 */
		return 2;
	domid = u;
	self_argv = argv;
	while ((c = getopt(argc - 1, argv + 1, "d:s:H:m:p:M:P:vx")) != -1) {
		if (c == 'x')
			use_xs = 1;
		else if (c == 'v')
			verbose = 1;
		else if (c == 'd')
			allow = optarg;
		else if (c == 's')
			shell_path = optarg;
		else if (c == 'H')
			heap_id = optarg;
		else if (c == 'm' && !num_arg(optarg, 0, ~0ul, &u))
			ring_pa = u;
		else if (c == 'p' && !num_arg(optarg, 10, 1u << 20, &u))
			static_port = u;
		else if (c == 'M' && !num_arg(optarg, 10, 1ul << 30, &u))
			max_mapped_pages = (uint64_t)u * (1 << 20) / PAGE;
		else if (c == 'P' && !num_arg(optarg, 10, 1000000, &u))
			poll_us = u;
		else
			return 2;
	}
	if (!use_xs && (!ring_pa || !static_port))
		return 2;
	if (heap_id) {
		struct xshm_info info = { 0 };
		char hp[96];

		snprintf(hp, sizeof(hp), "/dev/xen-shm/%s", heap_id);
		heap_fd = open(hp, O_RDWR | O_CLOEXEC);
		if (heap_fd < 0 || ioctl(heap_fd, XSHM_IOCTL_INFO, &info)) {
			fprintf(stderr, "vqda-back: heap %s: %s\n", hp, strerror(errno));
			return 1;
		}
		heap_size = info.size;
	}
	for (i = 0; i < MAX_SESSIONS; i++)
		sess[i].fd = -1;
	{
		/* one fd per heap buffer and per session file */
		struct rlimit rl;

		if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < rl.rlim_max) {
			rl.rlim_cur = rl.rlim_max;
			setrlimit(RLIMIT_NOFILE, &rl);
		}
	}
	gnt = open("/dev/xen/gntdev", O_RDWR | O_CLOEXEC);
	evt = open("/dev/xen/evtchn", O_RDWR | O_CLOEXEC);
	if (gnt < 0 || evt < 0) {
		fprintf(stderr, "vqda-back: gntdev %d evtchn %d\n", gnt, evt);
		return 1;
	}
	c = use_xs ? connect_xs() : connect_guest();
	if (c) {
		fprintf(stderr, "vqda-back: d%d connect: %s\n", domid,
			strerror(-c));
		return 1;
	}
	printf("VQDABACK %s d%d ring %#llx port %d dsps '%s' shell %s heap %s %llu MiB\n",
	       VQDA_BUILD_ID, domid, (unsigned long long)ring_pa, port, allow, shell_path,
	       heap_id ? heap_id : "none", (unsigned long long)(heap_size >> 20));
	fflush(stdout);
	{
		struct sigaction sa = { .sa_handler = on_cancel };	/* no SA_RESTART */

		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR1, &sa, NULL);
	}
	for (i = 0; i < VQDA_SLOTS; i++)
		start_thread(worker, (void *)(uintptr_t)i);
	/*
	 * A slot left BUSY was taken by a back end that died before it
	 * answered: the request is lost with that back end's sessions, so
	 * fail it instead of leaving the guest waiting. Requests a guest
	 * posted while no back end ran have had their event: serve them.
	 */
	for (i = 0; i < VQDA_SLOTS; i++) {
		struct vqda_slot *r = (void *)((char *)ring + i * VQDA_SLOT_SIZE);
		uint32_t want = VQDA_BUSY;

		if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) != VQDA_BUSY)
			continue;
		r->ret = -ECONNRESET;
		if (__atomic_compare_exchange_n((uint32_t *)&r->state, &want, VQDA_DONE,
						0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			orphaned++;
	}
	if (orphaned) {
		printf("VQDABACK d%d %u orphaned requests failed\n", domid, orphaned);
		fflush(stdout);
		notify();
	}
	dispatch();

	for (;;) {
		unsigned int p;

		/* in poll mode, block only after a scan with the flag clear found nothing */
		if (!poll_us || (__atomic_store_n(&ctrl->back_polling, 0, __ATOMIC_SEQ_CST),
				 !dispatch())) {
			if (read(evt, &p, sizeof(p)) != sizeof(p)) {
				if (errno == EINTR)
					continue;
				return 1;
			}
			if (write(evt, &p, sizeof(p)) != sizeof(p))
				return 1;
		}
		if (!dispatch() && !poll_us)
			continue;
		if (poll_us) {
			uint64_t end = now_us() + poll_us;

			__atomic_store_n(&ctrl->back_polling, 1, __ATOMIC_SEQ_CST);
			while (now_us() < end)
				if (dispatch())
					end = now_us() + poll_us;
		}
	}
}

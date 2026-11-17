/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * vqda wire format: a Xen guest's QDA (accel/qda) client talks to a back
 * end in the hardware domain through one ring of granted pages and one
 * event channel. The ring carries the QDA ioctls one to one; buffers are
 * never copied: a guest GEM buffer is a set of guest pages granted to the
 * back end, which turns them into a DMA-BUF (gntdev) and imports it into
 * the guest's own QDA session on the host.
 *
 * Two ways to set the ring up: through xenstore for guests a toolstack
 * creates ("Xenstore channel" below), or, since in a dom0less system the
 * hardware domain may not read guest nodes, statically: the ring is a
 * static shared-memory region
 * (xen,domain-shared-memory-v1, owner the hardware domain, 1:1 in it) and
 * the doorbell a static event channel (xen,evtchn-v1), both declared in
 * the host device tree. Each side maps the region with /dev/mem at its
 * own address and binds its static port; the back end announces itself
 * with one event once bound. The guest zeroes the ring before its first
 * request.
 */
#ifndef VQDA_PROTO_H
#define VQDA_PROTO_H

#include <stdint.h>
#include <drm/drm.h>
#include "qda_accel.h"

#ifndef VQDA_BUILD_ID
#define VQDA_BUILD_ID		"unknown"
#endif
#define VQDA_RING_PAGES		16
#define VQDA_SLOT_SIZE		2048
#define VQDA_SLOTS		(VQDA_RING_PAGES * 4096 / VQDA_SLOT_SIZE)
#define VQDA_REFS_PER_PAGE	(4096 / sizeof(uint32_t))
#define VQDA_MAX_ARGS		80	/* buffers of one invocation */
#define VQDA_MAX_LIST		448	/* list pages: 1.75 GiB per buffer */

/*
 * One control page after the slots. The back end sets back_magic once it
 * has bound its port, so a later guest process need not wait for the
 * hello event. One vqda client
 * process per guest at a time: each one zeroes the slots and sends
 * VQDA_OP_RESET first, which closes the sessions an earlier process left.
 */
#define VQDA_CTRL_OFF		(VQDA_RING_PAGES * 4096)
#define VQDA_MAP_SIZE		(VQDA_CTRL_OFF + 4096)
#define VQDA_BACK_MAGIC		0x61647176	/* "vqda" */
/*
 * Poll mode (both sides opt in): a side that is spinning on the ring says
 * so here, and the other side then skips the event it would send. Each
 * side clears its flag and looks at the ring once more before it blocks,
 * and the other stores its ring update before it reads the flag (both
 * sequentially consistent), so no wake-up is lost.
 */
struct vqda_ctrl {
	volatile uint32_t back_magic;
	volatile uint32_t reserved;
	volatile uint32_t harness[62];			/* free for test tools (vqda-bulk) */
	volatile uint32_t back_polling;			/* back end scans for VQDA_REQ */
	volatile uint32_t guest_polling[VQDA_SLOTS];	/* guest spins for VQDA_DONE */
	/*
	 * Xenstore channel: the client stores VQDA_CLIENT_ALIVE here once
	 * connected and has gntalloc clear its low byte (and send an event)
	 * when the grant goes away, i.e. when the client process ends for any
	 * reason, leaving VQDA_CLIENT_GONE. The back end restarts on that and
	 * so closes the sessions a dead client left, with their pending
	 * invocations; 0 is a client that predates this word.
	 */
	volatile uint32_t client_alive;
};

#define VQDA_CLIENT_ALIVE	0x76716101u	/* "vqa" + 1 */
#define VQDA_CLIENT_GONE	(VQDA_CLIENT_ALIVE & ~0xffu)

_Static_assert(__builtin_offsetof(struct vqda_ctrl, back_polling) == 0x100, "ctrl");
_Static_assert(sizeof(struct vqda_ctrl) <= 4096, "ctrl page");
_Static_assert(__builtin_offsetof(struct vqda_ctrl, client_alive) == 0x184, "ctrl");

/* Arguments a FastRPC scalars value describes (buffers and handles). */
static inline int vqda_sc_nargs(uint32_t sc)
{
	return ((sc >> 16) & 0xff) + ((sc >> 8) & 0xff) + ((sc >> 4) & 0xf) +
	       (sc & 0xf);
}

static inline struct vqda_ctrl *ctrl_of(void *ring)
{
	return (struct vqda_ctrl *)((char *)ring + VQDA_CTRL_OFF);
}

/*
 * Xenstore channel, for guests a toolstack creates (no static ring): the
 * toolstack writes the device pair, the frontend readable by the back
 * end's domain and the backend readable by the guest:
 *   <FE> = /local/domain/<guest>/device/vqda/0: backend, backend-id
 *   <BE> = /local/domain/<back>/backend/vqda/<guest>/0: frontend,
 *          frontend-id, dsps (host DSP names, the guest's accel0, ...)
 * Each guest client process grants a fresh ring (VQDA_MAP_SIZE) to the
 * back end, binds an unbound event channel for it and writes ring-refs
 * (comma separated), event-channel, ring-gen (one more than before) and
 * state 3 under <FE>. The back end maps that ring, binds the channel,
 * sets back_magic, writes the same ring-gen and state 4 under <BE>, and
 * starts over (its sessions closed) when ring-gen moves again, and when
 * the client ends (client_alive reads VQDA_CLIENT_GONE): then it waits
 * for a ring-gen newer than the dead one. It exits when <FE> goes away
 * with the guest. The static ring has no such signal: there the sessions
 * a client left stay open until the next client's VQDA_OP_RESET. The
 * back end never takes <BE> from the guest's backend node: it builds it
 * from its own domid and requires frontend-id there to name the guest.
 */
#define VQDA_XS_FE	"device/vqda/0"

/* slot states; the owner of the next transition writes them */
#define VQDA_FREE		0	/* guest may take the slot */
#define VQDA_REQ		1	/* guest filled it, back end may take it */
#define VQDA_BUSY		2	/* back end working on it */
#define VQDA_DONE		3	/* result written, guest may read */

enum vqda_op {
	VQDA_OP_OPEN = 1,	/* u.open: dsp name in, sid out */
	VQDA_OP_CLOSE,
	VQDA_OP_QUERY,		/* u.query: struct drm_qda_query in/out */
	VQDA_OP_GEM_CREATE,	/* u.gem: size, page and list-page refs */
	VQDA_OP_GEM_CLOSE,	/* u.gem.handle */
	VQDA_OP_INVOKE,		/* u.invoke + args[] */
	VQDA_OP_SESSION_CREATE,	/* u.create; the back end supplies the shell */
	VQDA_OP_SESSION_ATTACH,	/* refused for guests */
	VQDA_OP_REMOTE_MMAP,	/* u.map: struct drm_qda_mem_map in/out */
	VQDA_OP_REMOTE_MUNMAP,	/* u.unmap: struct drm_qda_mem_unmap in/out */
	VQDA_OP_SET_OPTION,	/* u.opt: struct drm_qda_set_option */
	VQDA_OP_PING,		/* no-op round trip through the back end */
	VQDA_OP_RESET,		/* close every session of this guest */
	VQDA_OP_GEM_SHARE,	/* u.share: import a buffer of another session */
	VQDA_OP_HEAP_ALLOC,	/* u.heap: size in, bid and off out */
	VQDA_OP_HEAP_FREE,	/* u.heap.bid */
	VQDA_OP_HEAP_IMPORT,	/* u.heap.bid in, handle out in session sid */
	VQDA_OP_LAST = VQDA_OP_HEAP_IMPORT,
};

/*
 * Heap buffers (back end -H): the guest owns a static shared memory region
 * that the hardware domain borrows (Xen xen,id given to both). The back
 * end allocates buffers in it, so a guest's DSP buffers are bounded by the
 * region and not by its grant table, and each one is a single DMA-BUF of
 * contiguous memory (/dev/xen-shm) that any of the guest's sessions can
 * import. The guest maps a buffer at u.heap.off in its view of the region.
 * Buffers are per guest, not per session: VQDA_OP_RESET frees them all.
 */

#define vqda_arg drm_qda_fastrpc_invoke_args

struct vqda_slot {
	volatile uint32_t state;
	uint32_t op;
	uint32_t sid;		/* back end session, from VQDA_OP_OPEN */
	int32_t ret;		/* 0 or -errno */
	union {
		struct { char dsp[16]; } open;
		struct drm_qda_query query;
		struct {
			uint64_t size;
			uint32_t handle;	/* out */
			uint32_t npages;	/* pages of the buffer */
			uint32_t nlist;		/* pages holding the page refs */
			uint32_t list[VQDA_MAX_LIST];	/* refs of those list pages */
		} gem;
		struct {
			uint32_t handle;
			uint32_t sc;
			uint32_t nargs;
			uint32_t pad;
			struct vqda_arg args[VQDA_MAX_ARGS];
		} invoke;
		struct {
			uint32_t attrs;
			uint32_t siglen;
		} create;
		struct drm_qda_mem_map map;
		struct drm_qda_mem_unmap unmap;
		struct drm_qda_set_option opt;
		struct {
			uint32_t src_sid, src_handle;
			uint32_t handle;	/* out, in this session */
		} share;
		struct {
			uint64_t size;		/* in, alloc */
			uint64_t off;		/* out, alloc: offset in the region */
			uint32_t bid;		/* out of alloc, in otherwise */
			uint32_t handle;	/* out, import */
		} heap;
	} u;
};

_Static_assert(sizeof(struct vqda_slot) <= VQDA_SLOT_SIZE, "slot");
/* gntalloc's unmap notify clears the lowest addressed byte of client_alive */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little endian");

#endif

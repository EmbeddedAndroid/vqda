// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * The ring layout is an ABI between a guest's libvqda and the hardware
 * domain's vqda-back, which may be built apart: pin every offset, size
 * and op value either side reads.
 */
#include <stddef.h>
#include <stdio.h>
#include "vqda_proto.h"

#define AT(t, f, o) _Static_assert(offsetof(t, f) == (o), #t "." #f)

_Static_assert(VQDA_SLOTS == 32, "slots");
_Static_assert(VQDA_CTRL_OFF == 0x10000, "ctrl offset");
_Static_assert(VQDA_MAP_SIZE == 0x11000, "map size");
_Static_assert(VQDA_REFS_PER_PAGE == 1024, "refs per page");
_Static_assert(sizeof(struct vqda_slot) == 1952, "slot");
_Static_assert(sizeof(struct vqda_ctrl) == 392, "ctrl");
_Static_assert(sizeof(struct vqda_arg) == 24, "arg");
_Static_assert(sizeof(struct drm_qda_query) == 20, "query");
_Static_assert(sizeof(struct drm_qda_mem_map) == 48, "mem_map");
_Static_assert(sizeof(struct drm_qda_mem_unmap) == 32, "mem_unmap");

AT(struct vqda_slot, state, 0);
AT(struct vqda_slot, op, 4);
AT(struct vqda_slot, sid, 8);
AT(struct vqda_slot, ret, 12);
AT(struct vqda_slot, u, 16);
AT(struct vqda_slot, u.gem.size, 16);
AT(struct vqda_slot, u.gem.handle, 24);
AT(struct vqda_slot, u.gem.npages, 28);
AT(struct vqda_slot, u.gem.nlist, 32);
AT(struct vqda_slot, u.gem.list, 36);
AT(struct vqda_slot, u.invoke.handle, 16);
AT(struct vqda_slot, u.invoke.sc, 20);
AT(struct vqda_slot, u.invoke.nargs, 24);
AT(struct vqda_slot, u.invoke.args, 32);
AT(struct vqda_slot, u.create.siglen, 20);
AT(struct vqda_slot, u.share.src_handle, 20);
AT(struct vqda_slot, u.share.handle, 24);
AT(struct vqda_slot, u.heap.off, 24);
AT(struct vqda_slot, u.heap.bid, 32);
AT(struct vqda_slot, u.heap.handle, 36);

AT(struct vqda_ctrl, back_magic, 0);
AT(struct vqda_ctrl, reserved, 4);
AT(struct vqda_ctrl, harness, 8);
AT(struct vqda_ctrl, back_polling, 256);
AT(struct vqda_ctrl, guest_polling, 260);
AT(struct vqda_ctrl, client_alive, 388);

_Static_assert(VQDA_OP_OPEN == 1 && VQDA_OP_CLOSE == 2 && VQDA_OP_QUERY == 3 &&
	       VQDA_OP_GEM_CREATE == 4 && VQDA_OP_GEM_CLOSE == 5 &&
	       VQDA_OP_INVOKE == 6 && VQDA_OP_SESSION_CREATE == 7 &&
	       VQDA_OP_SESSION_ATTACH == 8 && VQDA_OP_REMOTE_MMAP == 9 &&
	       VQDA_OP_REMOTE_MUNMAP == 10 && VQDA_OP_SET_OPTION == 11 &&
	       VQDA_OP_PING == 12 && VQDA_OP_RESET == 13 &&
	       VQDA_OP_GEM_SHARE == 14 && VQDA_OP_HEAP_ALLOC == 15 &&
	       VQDA_OP_HEAP_FREE == 16 && VQDA_OP_HEAP_IMPORT == 17 &&
	       VQDA_OP_LAST == 17, "ops");
_Static_assert(VQDA_BACK_MAGIC == 0x61647176 && VQDA_CLIENT_ALIVE == 0x76716101u,
	       "magic words");
_Static_assert(VQDA_FREE == 0 && VQDA_REQ == 1 && VQDA_BUSY == 2 &&
	       VQDA_DONE == 3, "slot states");
_Static_assert((VQDA_CLIENT_GONE & 0xffu) == 0 &&
	       VQDA_CLIENT_GONE == (VQDA_CLIENT_ALIVE & ~0xffu), "client words");

int main(void)
{
	/* ctrl_of() is what both sides use to find the control page */
	static char ring[VQDA_MAP_SIZE];

	if ((char *)ctrl_of(ring) != ring + VQDA_CTRL_OFF) {
		printf("ctrl_of: wrong page\n");
		return 1;
	}
	printf("proto: layout ok, slot %zu bytes, %d slots\n",
	       sizeof(struct vqda_slot), VQDA_SLOTS);
	return 0;
}

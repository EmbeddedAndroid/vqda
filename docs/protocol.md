# vqda protocol

`include/vqda_proto.h` is the normative definition; this page explains
it. Both sides must be built from the same `vqda_proto.h`; `meson test`
pins its layout (`tests/test-proto.c`). There is no version exchange.

## Ring

One mapping of `VQDA_MAP_SIZE` (68 KiB, 4 KiB pages):

| offset    | size   | content                                   |
|-----------|--------|-------------------------------------------|
| 0x00000   | 64 KiB | 32 slots of `VQDA_SLOT_SIZE` (2 KiB)       |
| 0x10000   | 4 KiB  | control page, `struct vqda_ctrl`           |

A slot is a `struct vqda_slot`: `state`, `op`, `sid` (back end session),
`ret` (0, a DSP status code, or -errno) and a per-op union `u`.

### Slot states

```
 FREE --guest fills, sets--> REQ --back end takes--> BUSY
   ^                                                  |
   +--guest reads result, sets-- DONE <--back end sets+
```

The writer of each transition is fixed: the guest moves FREE to REQ and
DONE to FREE, the back end REQ to BUSY and BUSY to DONE (both compare
and exchange, so a slot the guest took back is left alone). After
setting REQ the guest notifies the event channel; after setting DONE
the back end does. The back end copies the whole slot before acting on
it and writes back only `sid`, `u` and `ret`, so a guest rewriting a
slot mid-request cannot change what is executed.

A back end that starts and finds slots left BUSY by a back end that
died fails them with -ECONNRESET; slots in REQ are served.

### Control page

| field             | written by | meaning                                    |
|-------------------|------------|--------------------------------------------|
| `back_magic`      | back end   | `VQDA_BACK_MAGIC` once it serves the ring  |
| `reserved`        |            | unused                                     |
| `harness[]`       | tools      | free for test tools (`vqda-bulk`)          |
| `back_polling`    | back end   | it is spinning on the ring (poll mode)     |
| `guest_polling[i]`| guest      | it is spinning on slot i (poll mode)       |
| `client_alive`    | guest      | `VQDA_CLIENT_ALIVE` while the client lives |

## Operations

| op                  | request (`u`)                    | reply                |
|---------------------|----------------------------------|----------------------|
| `OPEN`              | `open.dsp`: host DSP name        | `sid`                |
| `CLOSE`             |                                  |                      |
| `QUERY`             | `struct drm_qda_query`           | same, DSP name as the guest sees it |
| `GEM_CREATE`        | `gem.size`, `npages`, `nlist`, `list[]` | `gem.handle`  |
| `GEM_CLOSE`         | `gem.handle`                     |                      |
| `INVOKE`            | `invoke.handle` (not 1), `sc`, `nargs`, `args[]` | DSP status |
| `SESSION_CREATE`    | `create.attrs` (`siglen` ignored) | DSP status          |
| `SESSION_ATTACH`    |                                  | always -EPERM        |
| `REMOTE_MMAP`       | `struct drm_qda_mem_map`         | same                 |
| `REMOTE_MUNMAP`     | `struct drm_qda_mem_unmap`       | same                 |
| `SET_OPTION`        | `struct drm_qda_set_option`      |                      |
| `PING`              |                                  | 0, no DSP involved   |
| `RESET`             |                                  | closes every session and heap buffer of the guest |
| `GEM_SHARE`         | `share.src_sid`, `src_handle`    | `share.handle` in `sid` |
| `HEAP_ALLOC`        | `heap.size`                      | `heap.bid`, `heap.off` |
| `HEAP_FREE`         | `heap.bid`                       |                      |
| `HEAP_IMPORT`       | `heap.bid`                       | `heap.handle` in `sid` |

Every op except `OPEN`, `PING`, `RESET`, `HEAP_ALLOC` and `HEAP_FREE`
names a session (`sid`) of this guest's back end. The QDA structures are
those of `include/qda_accel.h`; QDA returns DSP status codes, neither 0
nor -errno, as the ioctl result, and `ret` carries them unchanged.

Each guest session is one open file of the host's accel device for that
DSP; `OPEN` names the DSP by its host name. A session stays open until
`CLOSE` or `RESET` and every request still using it has finished. A
client sends `RESET` before anything else, which closes what an earlier
client of the same guest left.

### Buffers

`GEM_CREATE` describes a buffer of `npages` = ceil(`size` / 4096) guest
pages. Their grant references are written into `nlist` = ceil(`npages` /
1024) granted list pages, whose own references are in `list[]` (at most
`VQDA_MAX_LIST` = 448, so 1.75 GiB per buffer). The back end maps the
list pages, copies the references out, unmaps them, exports the
references as a DMA-BUF (`IOCTL_GNTDEV_DMABUF_EXP_FROM_REFS`) and
imports that into the session (`DRM_IOCTL_PRIME_FD_TO_HANDLE`); the
reply's handle is the session's GEM handle. The guest maps the buffer
from its own grants, so both sides see the same pages.

`GEM_SHARE` imports a buffer of another session of the same guest
(PRIME export and import on the host), for a guest buffer used by two
DSPs.

Heap buffers (`HEAP_*`, back end `-H`) live in a static shared memory
region the guest owns and the hardware domain borrows. The back end
allocates them (first fit; 2 MiB alignment from 2 MiB up), each is one
DMA-BUF of contiguous memory any session of the guest can import, and
the guest maps it at `heap.off` in its own view of the region. They are
per guest, not per session; `RESET` frees them all. A freed range that a
session still has imported is not handed out again until the host lets
go of it.

## Setting up the ring

### Xenstore channel

For guests a toolstack creates. The toolstack writes

```
<FE> = /local/domain/<guest>/device/vqda/0: backend, backend-id
<BE> = /local/domain/<back>/backend/vqda/<guest>/0: frontend,
       frontend-id, dsps
```

with `<FE>` writable by the guest and `<BE>` readable by it. Each client
process grants a fresh ring (17 pages) to the back end, binds an unbound
event channel for it, and writes `ring-refs` (comma separated),
`event-channel`, `ring-gen` (one more than before) and `state` 3 under
`<FE>`. The back end maps the ring, binds the channel, sets
`back_magic`, writes the same `ring-gen` and `state` 4 under `<BE>`, and
notifies. The client waits for both before its first request. Only
xenstore clients set `client_alive`; a static ring client leaves it 0.

The back end builds `<BE>` from its own domain id and requires
`frontend-id` there to name the guest; it never follows a path the guest
wrote. It re-executes itself (closing every session) when `ring-gen`
moves, and when the client dies: the client arms gntalloc's unmap notify
on `client_alive`, so however the process ends the guest kernel clears
the word's low byte (`VQDA_CLIENT_GONE`) and notifies. Requests that
arrive after that are refused with -ECONNRESET. The back end then waits
for a `ring-gen` newer than the dead one, and exits when `<FE>` goes
away.

### Static ring

For dom0less guests, where the hardware domain may not read guest
nodes. The ring is a static shared memory region
(`xen,domain-shared-memory-v1`, owner the hardware domain, mapped 1:1 in
it) and the doorbell a static event channel (`xen,evtchn-v1`), both in
the Xen device tree. Each side maps the region with `/dev/mem` and binds
its static port; the back end sets `back_magic` and notifies once bound.
The guest zeroes the slots before its first request. There is no
client-exit signal: a client's sessions stay open until the next
client's `RESET`.

## Poll mode

Both sides opt in (`vqda-back -P`, `VQDA_POLL_US`). A side spinning on
the ring sets its flag in the control page and the other side then skips
the event it would send. Each side clears its flag and looks at the ring
once more before it blocks, and the other stores its ring update before
it reads the flag (both sequentially consistent), so no wake-up is lost.

## Cancelling calls

A worker can sit in a DSP call that never returns. When a request
arrives for a slot whose worker is still busy (a new client reusing the
ring), and on `RESET`, the back end interrupts that worker (SIGUSR1
without SA_RESTART, so the QDA ioctl returns EINTR). A result is written
back only if the slot still holds the same request (per-slot epoch, and
the slot still BUSY), checked under the lock the dispatcher hands slots
out with; a cancel signals only a call from before the new request,
under the same lock. Closing the session makes QDA tear the DSP process
down.

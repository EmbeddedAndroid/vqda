# vqda

vqda lets Xen guests use the Qualcomm Hexagon DSPs (CDSP/NSP) that the
hardware domain drives through the Linux QDA accel driver (`accel/qda`,
`/dev/accel/accelN`). A back end in the hardware domain serves each
guest's QDA requests over a ring of Xen shared pages and an event
channel; a guest library presents `/dev/accel/accelN` to unmodified
user space, such as a FastRPC library built for QDA and the programs on
top of it (llama.cpp's Hexagon backend, for example). Buffers are never
copied: a guest buffer is the guest's own pages, granted to the back end
and imported into the DSP session as a DMA-BUF.

## The problem

On a Xen system the DSPs belong to the hardware domain. Their firmware
is loaded and authenticated there through the secure world (remoteproc),
the hardware domain's kernel programs the SMMU context banks the DSPs
reach memory through, and the shared-memory IPC channel to the DSP
subsystem ends there. None of these is a device Xen can pass through to
a guest on its own. A guest that wants a DSP therefore has two choices
without vqda: run the DSP work in the hardware domain instead (losing
the isolation the guest was for), or put a device model between the
guest and the driver, which needs a VMM in the hardware domain and, to
avoid copying model weights and tensors, host memory mapped into the
guest.

## How vqda solves it

```
 guest                                    hardware domain
 application (llama.cpp, ...)             vqda-back <guest domid>
 FastRPC library (QDA build)               |  one host /dev/accel file per
 libvqda.so (LD_PRELOAD)                   |  guest session: own context
   /dev/accel/accelN -> ring requests      |  bank, own DSP process
   buffers = guest pages granted to   ---> |  granted pages -> gntdev DMA-BUF
   the back end (gntalloc)                 |  -> PRIME import into the session
            ring (16 pages, 32 slots) + event channel
```

- **One ring per guest client.** The QDA ioctls travel one to one in
  2 KiB slots; each slot is a small state machine (free, request, busy,
  done) so up to 32 requests are in flight. The back end runs one worker
  thread per slot, so a long DSP call does not block the others.
- **Two ways to set the ring up.** For guests a toolstack creates, each
  client process grants a fresh ring and binds an unbound event channel,
  and publishes both in xenstore. For dom0less guests (where the hardware
  domain may not read guest xenstore nodes), the ring is a static shared
  memory region and the doorbell a static event channel, both declared
  in the host device tree.
- **Zero-copy buffers.** `QDA_GEM_CREATE` in the guest grants the
  buffer's pages and sends their grant references (through granted list
  pages, so one request describes up to 1.75 GiB). The back end exports
  those references as one DMA-BUF (gntdev) and imports it into the guest's
  host session. `mmap` in the guest maps the same pages.
- **The system DMA heap.** libvqda also serves `/dev/dma_heap/system`,
  so a FastRPC library that allocates from it gets device independent
  buffers that are mapped only into the context banks of the sessions
  importing them. Optionally (back end `-H`), these come from a static
  shared memory region the guest owns instead of from grants, so a
  guest's DSP memory is bounded by that region and not by its grant
  table, and each buffer is one contiguous DMA-BUF.
- **One back end per guest.** Every guest session is one open file of the
  host's accel device, so it gets its own context bank and DSP process as
  a host process would. The back end decides which DSPs a guest may
  use; the guest opens them by their host names, and a DSP name query
  reports them as `cdsp`, `cdsp1`, ... in the order given.

The wire format is in [docs/protocol.md](docs/protocol.md) and
`include/vqda_proto.h`; the security model in
[docs/security.md](docs/security.md).

## Security model in brief

The back end treats everything in the ring as hostile. It copies every
request out of the shared slot before looking at it, lets a guest open
only the DSPs it was given, creates only unsigned DSP processes running
the hardware domain's own DSP shell (whatever ELF the guest names),
refuses attaching to the root, sensors and audio protection domains and
invoking the DSP's process init handle, keeps the shell's buffer out of
the guest's reach, validates sizes, page counts and argument counts
before any host call, and closes a session only once no request uses
it. With `-M` it caps the guest memory it keeps mapped. Grant references
are always mapped as the guest's domain, so a guest can only hand the
back end its own memory. Argument bounds within a buffer are checked by
the host QDA driver. A guest can run unsigned code on its own DSPs, use
the DSP time and the memory it was given, and nothing else; see
[docs/security.md](docs/security.md) for what it can and cannot do.

## Building

vqda is C (gnu11) with a meson build. It needs the Linux UAPI headers for
`xen/evtchn.h` (with `IOCTL_EVTCHN_BIND_STATIC`), `xen/gntdev.h`,
`xen/gntalloc.h` and `drm/drm.h`; the Linux 6.12 headers of Debian
trixie, which the container uses, have all of them.

```
meson setup build
meson compile -C build
meson test -C build
meson install -C build
```

For aarch64 from another host, add
`--cross-file cross/aarch64-linux-gnu.ini` to `meson setup`. The
`static` option (default on) links `vqda-back` and `vqda-bulk`
statically for minimal root file systems.

`scripts/ci.sh` builds and tests natively, again under AddressSanitizer
and UndefinedBehaviorSanitizer and under ThreadSanitizer, then builds
for aarch64 and installs that tree to `out/`. The container runs it and
exports the install tree:

```
docker build --output out .
```

ThreadSanitizer needs ASLR off, which a default container refuses, so
`docker build` skips that stage. To run it too:

```
docker run --rm --security-opt seccomp=unconfined -v "$PWD:/src" -w /src \
    <image with the Dockerfile's packages> scripts/ci.sh
```

Installed: `vqda-back` (hardware domain), `libvqda.so` (guest, in the
library directory), and the tools `vqda-ping`, `vqda-neg`, `vqda-fuzz`
and `vqda-bulk`.

### Continuous integration

`.github/workflows/build.yml` runs `scripts/ci.sh` on GitHub, ThreadSanitizer
included, and keeps the aarch64 tree as an artifact. It only runs when
started by hand (Actions, build, Run workflow). To build every push and
pull request, add `push:` and `pull_request:` under `on:`.

## Requirements

Hardware domain kernel: the QDA accel driver, `CONFIG_XEN_GNTDEV` with
`CONFIG_XEN_GNTDEV_DMABUF`, `CONFIG_XEN_DEV_EVTCHN`. The DSPs' context
banks must be IO coherent: libvqda answers `DMA_BUF_IOCTL_SYNC` without
doing anything. gntdev's `xen_gntdev.limit` (pages, default 65536) bounds
one mapping request, so it must cover the largest single buffer; it does
not bound the total. Exporting a large buffer allocates 4 bytes per page
in one piece in the hardware domain kernel (1.75 MiB for the largest),
which a fragmented hardware domain can refuse.

Guest kernel: `CONFIG_XEN_GNTALLOC`, `CONFIG_XEN_DEV_EVTCHN`, and for the
xenstore channel `/dev/xen/xenbus`. gntalloc's `xen_gntalloc.limit`
(grants, default 1024, so 4 MiB) bounds all of a guest's grant-backed
buffers together: raise it to what the guest's models need.

Xen: a guest's grant table bounds its grant-backed buffers
(`max_grant_frames`); the hardware domain's maptrack
(`max_maptrack_frames`) bounds what all its back ends map together, vqda,
block and network back ends alike, which is what `vqda-back -M` protects.
The static ring needs Xen built with static shared memory and static
event channel support (dom0less).

Both sides use 4 KiB pages.

## Running

List the DSPs the hardware domain can serve:

```
vqda-back -l
```

### Guests created by a toolstack (xenstore channel)

The toolstack writes the device nodes, the frontend writable by the
guest and the backend readable by it, then starts one back end per
guest (guest domid `G`, back end domain `B`):

```
FE=/local/domain/$G/device/vqda/0
BE=/local/domain/$B/backend/vqda/$G/0
xenstore-write $FE/backend $BE $FE/backend-id $B
xenstore-chmod -r $FE n$G r$B
xenstore-write $BE/frontend $FE $BE/frontend-id $G $BE/dsps cdsp2,cdsp3
xenstore-chmod -r $BE n$B r$G
vqda-back $G -x -M 16384
```

The back end serves one client process at a time and starts over, with
the previous client's sessions closed, for each new client and whenever
the client exits (however it exits). It exits when the frontend node
goes away with the guest.

In the guest:

```
echo dsps=cdsp2,cdsp3 > /etc/vqda.conf
LD_PRELOAD=libvqda.so llama-cli ...
```

### dom0less guests (static ring)

Declare a static shared memory region of at least 68 KiB (1 MiB for
`vqda-bulk`), owned by the hardware domain and mapped 1:1 in it, and a
static event channel between the hardware domain and the guest, in the
Xen device tree. Then, with `RING` the region's physical address and
`PORT` the static port of each side:

```
vqda-back $G -m $RING -p $PORT -d cdsp2,cdsp3            # hardware domain
VQDA_RING=$RING VQDA_PORT=$PORT VQDA_DSPS=cdsp2,cdsp3 \
    LD_PRELOAD=libvqda.so llama-cli ...          # guest
```

Both sides map the region through `/dev/mem`, so neither kernel may
restrict `/dev/mem` to non-RAM (`CONFIG_STRICT_DEVMEM`). Start a new back
end for a guest only after the previous one has exited: two processes
bound to the same static port break the port in some kernels.

### Guest library settings

- `VQDA_DSPS` (or `dsps=` in `/etc/vqda.conf`): the host DSP names behind
  `accel0`, `accel1`, ..., matching the back end's `-d` list (default
  `cdsp,cdsp1,cdsp2,cdsp3`). `/dev/dma_heap/system` is taken over only
  when this list or a heap is configured.
- `VQDA_RING`, `VQDA_PORT`, `VQDA_BACKEND`: the static ring, its port and
  the back end's domain id (default 0); without `VQDA_RING` the
  xenstore channel is used.
- `VQDA_HEAP` (or `heap=`): the guest's static shared memory region for
  heap buffers (back end `-H`), seen as `/dev/xen-shm/<heap>`.
- `VQDA_POLL_US`: spin this long for each answer before sleeping (with
  back end `-P`); trades CPU time for round trip latency.
- `VQDA_CONNECT_MS`, `VQDA_DEBUG`: connect timeout, per-request log.

### Back end options

- `-d dsp,...`: the host DSPs this guest may open.
- `-M MiB`: at most this much guest memory mapped at a time (default, or
  0: no limit). A buffer counts while its handle is open, or until its
  session closes once it was mapped on the DSP; a buffer shared between
  two sessions counts once per session.
- `-P us`: poll mode. Each worker that answered a request and the main
  loop spin this long before sleeping, so a guest keeping its slots busy
  keeps up to 33 hardware domain threads spinning; leave it off for
  guests that should not have that.
- `-s shell`, `-H heap`, `-v`: the DSP shell, the heap region, a log line
  per request.

The FastRPC library looks for a DSP shell file in the guest before it
opens a session; any placeholder file will do, since the back end runs
its own (`-s`, default `/usr/lib/dsp/cdsp/fastrpc_shell_unsigned`).

## Tools

All guest tools are linked against libvqda.

- `vqda-ping [n]`: n no-op round trips through the back end, without a
  DSP; prints min, p50, p90, p99 and max in microseconds.
- `vqda-neg [assigned dsp] [other dsp]`: isolation checks; each line is
  `NEG <case> PASS|FAIL`, PASS meaning the back end or the host refused
  what a guest must not do (unassigned DSP, signed process, root PD, the
  init handle, forged session ids, handles and grants, the shell buffer,
  arguments outside a buffer, forged or freed heap ids). Cases the host
  refuses pass on any error, since the errno is the host driver's.
- `vqda-fuzz [iterations] [seed]`: random hostile requests sent raw (no
  library checks) on a real session, then an open/close storm, with a
  ping every 1000 requests proving the back end still answers.
- `vqda-bulk send|recv <ring phys> <file>`: copies a file from the
  hardware domain into a dom0less guest through the static ring region,
  for guests without network or storage.

`meson test` runs what needs no Xen or DSP: the wire layout, the
xenstore client against a scripted peer, every back end policy check,
session and mapping lifetimes, a fuzz pass of the request handler and
requests racing session closes through the real workers, the guest
library's pass-through and failure paths, and its buffer lifetimes
(pooling, PRIME pins, copies in other sessions, the grant heap) against
a fake back end and a fake gntalloc.

## Limitations

- One client process per guest at a time; a second one resets the
  first one's sessions.
- libvqda tracks its file descriptors by number and does not intercept
  `dup`, `dup2`, `dup3`, `F_DUPFD` or `close_range`: a duplicate is not
  recognised, and closing one of its descriptors that way skips the
  teardown (grants and host handles stay until the process exits).
  Descriptors numbered 1024 or above cannot be used.
- At most 64 sessions per guest and 1.75 GiB per grant-backed buffer.
- On the static ring there is no client-exit signal: sessions a client
  leaves stay open until the next client connects.
- A DSP call that never returns holds its slot's worker until the
  client goes away and the back end restarts (xenstore channel) or the
  next client resets the back end (static ring), which interrupts it.
- Heap buffers (`-H`, `VQDA_HEAP`) need a Linux driver exposing Xen
  static shared memory as `/dev/xen-shm/<id>`. It is not upstream yet:
  the patch is on the `xen/static-shm` branch of
  https://github.com/EmbeddedAndroid/linux (on Linux 6.19), and
  `include/xen-static-shm.h` is its user API. vqda relies on it refusing
  (EBUSY) to export a range a session still imports.
- The QDA accel driver is not yet merged in Linux; vqda follows its
  posted UAPI (`include/qda_accel.h`).

## License

vqda is BSD-3-Clause (`LICENSES/BSD-3-Clause.txt`). Two headers are the
user API of Linux drivers and keep the kernel's
`GPL-2.0-only WITH Linux-syscall-note` (`LICENSES/GPL-2.0-only.txt`,
`LICENSES/Linux-syscall-note.txt`), which permits including them in
programs under any license: `include/qda_accel.h`, copied from the posted
QDA driver, and `include/xen-static-shm.h`, the user API of the static
shared memory driver above.

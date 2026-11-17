# vqda security model

The guest is untrusted. `vqda-back` runs as root in the hardware domain
with access to the DSPs, gntdev and the host's accel devices, and serves
exactly one guest, named by domain id on its command line. Everything
the guest controls (ring contents, xenstore nodes under its frontend,
grant references, sizes, handles, session ids, and the timing of all of
them across 32 slots) is input to be checked.

## What the back end enforces

- **DSPs.** Only the DSPs given with `-d` (or the backend node's `dsps`,
  which the guest can read but not write) can be opened; any other name
  is -EACCES. The guest opens them by their host names; a DSP name query
  answers `cdsp`, `cdsp1`, ... in the order given. After the first open
  of a DSP, the back end opens only that DSP's device for it.
- **DSP processes.** `SESSION_CREATE` without
  `QDA_INIT_ATTR_UNSIGNED_MODULE` is -EPERM: no signed process, which
  the root back end could otherwise create on the guest's behalf. The
  process is created with that attribute only, whatever else the guest
  set. Its ELF is the back end's own shell file (`-s`), loaded when the
  session opens, before the guest can name the session, into a buffer
  the guest cannot name: closing it, passing it as
  an invocation argument, mapping it on the DSP or sharing it is -EPERM.
  `SESSION_ATTACH` (root, sensors and audio protection domains) is always
  -EPERM, and so is an invocation of handle 1, the DSP's process init
  handle, through which a guest could create or attach processes behind
  those checks.
- **Sessions.** Session ids index this back end's own table, so a guest
  can only name its own sessions; a free, closed or out of range id is
  -EBADF. A request holds the session it names; a close or reset marks
  it closed and the last request using it closes the host file, so a
  request never reaches a file that was closed and its number reused. At
  most 64 per guest.
- **Request copies.** Each request is copied out of the shared slot
  before it is examined, and only the result fields are written back,
  only while the slot is still the request's (a slot the guest took back
  or a cancelled call gets nothing); the invocation argument array is
  part of that copy. `nargs` must equal the count `sc` encodes and fit
  the slot.
- **Buffers.** A buffer's size, page count and list page count must agree
  and fit `VQDA_MAX_LIST` before any grant is touched. Grant references
  are mapped as the guest's domain, so Xen only resolves the guest's own
  grants; a forged reference fails its grant operation. Offsets and
  lengths of invocation arguments are checked against the buffer by the
  host QDA driver.
- **Mapped memory.** Every grant-backed buffer stays mapped in the
  hardware domain while its handle lives, in a maptrack every back end
  of the domain shares, and gntdev's limit only bounds one request; the
  same grants can be named by any number of buffers. With `-M` the back
  end refuses a buffer (-ENOSPC) that would take the guest's mapped
  memory over the cap; a buffer shared into another session counts again
  there. A buffer mapped on the DSP (REMOTE_MMAP) stays charged after its
  handle closes, until its session closes, since the DSP mapping keeps
  the grants mapped; after a REMOTE_MUNMAP this over-counts, which errs
  on the safe side. Without `-M` there is no cap: set
  it for any guest that must not be able to exhaust the hardware domain's
  maptrack.
- **Heap buffers.** Heap ids are checked against the back end's table;
  allocations are bounded by the guest's own region and never overlap a
  live buffer. A freed range a session still imports is not handed out
  again because the static shared memory driver refuses to export it
  (EBUSY).
- **Xenstore.** The backend path is derived from the back end's own
  domain id and the guest id it was started with, never read from the
  guest's frontend node, and `frontend-id` under it must name the guest.
  From the guest's node it reads only the ring references (exactly 17
  decimal numbers), event channel, generation, state and backend-id. A
  new generation or a client exit restarts the back end at most once a
  second.
- **Logs.** Every line a guest can cause (buffer import failures,
  cancelled calls, unusable rings) goes through one limiter: at most one
  a second, with the count of the ones left out. Restarts, at most one a
  second, print two lines each.

`vqda-neg` checks the refusals against a live back end; `meson test`
checks the back end's side without hardware (`tests/test-back.c`),
including requests racing session closes under ThreadSanitizer.

## Signals that depend on the guest

The guest's library sets `client_alive` and arms gntalloc to clear it when
the client exits. A guest that does not cooperate only loses what the
signal buys: its dead client's sessions stay open until its next client
connects, and requests queued after the exit are mapped (and fail)
instead of refused.

## What a guest can do

- Run unsigned code on the DSPs it was given, inside the DSP's unsigned
  process sandbox, with the host's shell.
- Use DSP time on those DSPs, up to 64 sessions, and memory up to its
  grant table (or heap region), gntalloc's limit and `-M`.
- Keep the back end's threads busy: up to 32 workers in DSP calls, one
  thread in a heap allocation search (up to about 25 million comparisons
  and 6000 export calls when the region is fragmented and full), and with
  `-P` up to 33 threads spinning.
- Hold a worker in a DSP call that does not return, until its client
  goes away or the next client resets the back end.

## What a guest cannot do

- Open a DSP it was not given, create a signed process, attach to a
  static protection domain or invoke the process init handle.
- Name another guest's sessions or buffers: each guest has its own back
  end process, and grants resolve only for the guest's domain.
- Make the back end read or write memory other than its own ring, its
  own grants and its own heap region.

## Not covered

- Two guests given the same DSP share its time and its failures: a DSP
  crash and restart ends both guests' sessions on it. Assign each DSP to
  one guest for isolation.
- The DSP's own protection of one process from another is the DSP
  firmware's; vqda adds nothing there.
- REMOTE_MMAP flags and attributes and SET_OPTION values reach the QDA
  driver unfiltered: their meaning is the driver's (its UAPI does not
  define the mapping flags), and so is checking them.
- The back end keeps every privilege of root after setup. A bug in its
  request handling is a hardware domain bug; it is built with `-Wall
  -Wextra` and `_FORTIFY_SOURCE`, and tested under AddressSanitizer,
  UndefinedBehaviorSanitizer and ThreadSanitizer with a fuzz pass of its
  request handler.

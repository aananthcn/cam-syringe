# 0004. Never synchronously join a stream/network thread from the GUI thread

Status: Accepted

## Context

`DispatcherClient::disconnect()` already discovered and fixed one
instance of this: joining a background thread synchronously from the
GUI thread freezes the whole window for however long that thread's own
blocking call takes to unblock, if it ever does (see its own header
comment for the full story).

Confirmed a second, independent instance of the exact same class of bug
this session, via a real all-threads gdb backtrace captured from a live,
undisturbed, actually-stuck process (not a guess): `StreamPool::stopAll()`
joined a `CameraStream`'s own thread synchronously, called from
`MainWindow::stopEverything()` on the GUI thread — including
*automatically*, via the new Play-time reachability auto-stop (see
0003) — and that thread was blocked inside ffmpeg's own
`av_interleaved_write_frame()` → `poll()`, writing an RTP packet to an
unreachable destination. There is no `AVIOContext` timeout configured
for this write, so it had no bound of its own;
`CameraStream::requestStop()`'s flag is only checked BETWEEN writes in
`run()`'s own loop, never inside one already in flight. The result was
indistinguishable from a hard crash (window manager "Not
Responding"/force-quit) until — if ever — the write happened to unblock
on its own.

## Decision

Any thread that streams to, or otherwise talks over, a connection whose
other end might be gone/unreachable/slow must never be
`std::thread::join()`ed synchronously from the GUI thread. Tear-down
instead hands the thread — plus whatever object it's still running,
e.g. `StreamPool::stopAll()`'s own `CameraStream` `unique_ptr` — off to
a newly-spawned, detached cleanup thread that joins it there; the
calling (GUI) thread returns immediately regardless of how long the
real teardown actually takes. A late/slow finish is tolerated silently:
nothing outside that detached lambda references the moved-out
thread/object afterward, so this is safe even if the owning object
(`StreamPool`, `DispatcherClient`) is itself destroyed the moment the
call returns.

## Consequences

- Any future per-camera/per-connection worker thread this project adds
  must follow the same non-blocking hand-off before its owner's
  stop/teardown path can be trusted not to freeze the GUI — confirmed
  twice now (`DispatcherClient`, `StreamPool`) that "just `join()` it,
  teardown is fast" is wrong the moment the underlying call is a
  blocking network write with no timeout.
- This is a mitigation, not a full fix: it guarantees the GUI never
  blocks on a stuck writer thread, not that the thread itself ever
  cleanly exits. A deeper fix — bounding
  `CameraStream::run()`'s own `av_interleaved_write_frame()`/`avio_open()`
  call with an explicit ffmpeg-level timeout, so a stuck writer doesn't
  linger indefinitely across repeated Play/Stop cycles against a dead
  target — is not yet done.

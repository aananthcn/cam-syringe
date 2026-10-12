# 0008. The PC sizes each stream to the camera's own resolution, read from the target

Status: Accepted. The PC side is implemented. The target side does not yet
follow it, see "Implementation status".

## Context

A camera id's injected frames are only useful if they match what that camera
really is on the target:

- The target's display pipe for at least one camera layer **cannot scale**
  (`WFD_PIPELINE_SCALE_RANGE` upScale = downScale = 1.0, confirmed live via
  `slog2info`). Content that isn't pixel-identical to the camera's negotiated
  size renders blank (`qcarcam-injector` ADR 0010, formerly `ARCHITECTURE.md`
  item 38).
- The target's hardware H.264 decoder and its shared-memory frame ring are
  sized from the stream and the publish resolution, so an oversized stream
  costs real decoder memory. On 2026-10-12 a run failed because `videoCore`
  could not allocate a 12.6 MB decoder buffer (`vidc_allocate_device_mem`,
  size 12591104, about 4K-class) while the ring was sized for 1080p.
- Camera resolutions are authored by the vehicle engineer on the target and
  differ per camera id. A human cannot be relied on to know and match them.

Earlier, `qcarcam-injector` made the target accept any incoming resolution and
rescale it to a fixed publish size (formerly `ARCHITECTURE.md` item 20). That
was a convenience for arbitrary senders. It does not make the content match the
camera.

## Decision

- **CamSyringe discovers each camera's real resolution from the target**
  (`CameraGeometryResolver`, run on each Configure apply, passwordless SSH
  only), per camera id, in ascending order:
  1. the `<output_setting width height>` in the `/mnt/scripts/1cam_*.xml`
     template that `camtest` would pick for that id (mirrors `camtest`'s own
     board-variant branching);
  2. otherwise a brief live query: `qcarcam_test` run against the real-ref
     copy of `libqcxclient.so`, parsing its "Queried Inputs" dump;
  3. otherwise the last successful earlier id's result (flagged
     `wasFallback`);
  4. otherwise `ok=false`, reported plainly and never guessed.
  Results are cached per target (`CameraConfigStore`), and learned only on
  demand, see ADR 0011.
- **The PC adjusts the stream to that size.** `StreamPool::setTargetGeometry()`
  feeds `CameraConfig::targetWidth/targetHeight`, and `CameraStream` encodes
  at exactly that canvas: the source is aspect-fitted and centred
  (letterboxed), and **may be scaled up**, because the goal is an exact match.
  The target is not expected to rescale CamSyringe's streams.
- **When the size is unknown**, the stream falls back to the old cap-only
  behaviour: never above 1920×1080, aspect preserved.

## Implementation status

- **Done (this repo):** resolution discovery, the cache, and canvas-exact
  encoding as described above.
- **Not done (target side, `qcarcam-injector`):** the control-channel
  declaration carries only `CAM <id> <port>` and `FLAGS`, and the dispatcher
  publishes every camera at one fixed `--width/--height` (default 1920×1080)
  that the receiver rescales to. So a camera-sized stream is still decoded at
  its own size and then rescaled to 1080p before injection. Making the ring
  and the injector's buffer lists follow each camera's real size needs the
  size to travel with the declaration. This has not been designed yet.
- **Not verified:** whether the 2026-10-12 decoder failure came from a stream
  larger than the camera size, from a camera that really is 4K, or from a
  `wasFallback` or `ok=false` result silently using the wrong size. The first
  thing to read is CamSyringe's stderr line for that run:
  `camsyringe[camN]: WxH -> WxH canvas (target-matched ...)` versus the plain
  `WxH -> WxH` of the cap-only path.

## Consequences

- A `wasFallback` size is a guess borrowed from another id. It can be wrong
  for a camera that really differs, and it is not surfaced as a warning
  today.
- A failed discovery silently sends a capped 1080p stream. Whether that
  matches the camera is unknown in that case.
- A camera that is truly 4K asks the hardware decoder for 4K-class memory, and
  that can fail independently of this decision (the 2026-10-12 symptom).
- `qcarcam-injector`'s receiver rescale stays as a safety net for other
  senders such as `camera-streamer-pc.sh`. For CamSyringe it is a fallback,
  not the design.
- Related: `qcarcam-injector` ADRs 0007 (receiver resolution policy) and 0010
  (shim declared size and the non-scaling display pipe).

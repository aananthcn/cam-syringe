# 0015. The stream canvas is capped at 1920×1080, even for a larger camera

Status: Accepted. Implemented; the capped path was built but not yet re-run
against the board. Interim, see "Revert condition".

Refines ADR 0008 (the stream is sized to the camera's resolution).

## Context

ADR 0008 says to encode at exactly the camera's own size, read from the
target. On 2026-10-13 that was tried with the bench's cam 2, which the
target reports as **3840×2160** (read from the target, `wasFallback` false,
`ok` true), and it failed:

- CamSyringe sent a 4K canvas. The target's receiver logged
  `HwVideoDecoder: START(output) failed`, `FrameReceiver: failed to open
  hardware video decoder` and `Failed to open RTP receiver on port 5004`, then
  exited before reporting READY. The dispatcher failed cam 2 at once, and the
  injector delivered 0 frames.
- The same camera, sent at the old 1080p cap on the same board, opened the
  decoder at 1920×1080 and ran at about 30 fps for four loop passes.

This is the 2026-10-12 failure (`vidc_allocate_device_mem`, size 12591104,
about 4K-class), now explained: the camera really is 4K, so matching it exactly
asks the board's hardware H.264 decoder for memory it can't provide. It was
not a fallback size or a failed read.

## Decision

- **`CameraStream` never encodes a canvas larger than 1920×1080**
  (`kMaxWidth`, `kMaxHeight`). When the camera size from the target exceeds
  either limit, the canvas is scaled down to fit, aspect preserved, with even
  dimensions. 3840×2160 becomes 1920×1080.
- **A camera at or under the cap is unchanged:** the canvas still matches it
  exactly (ADR 0008), and the source is still letterboxed into it.
- **The cap is a hard upper bound on the stream, not a camera setting.** It does
  not change what is read from the target or cached (`camera_configs.json`
  still holds 3840×2160 for cam 2).
- **It is visible only in stderr:** the line reads
  `camsyringe[camN]: WxH -> WxH canvas (target-matched, capped from camera size;
  content …)`.

## Consequences

- A camera larger than 1080p is injected at 1080p. The target's receiver
  rescales to its ring size anyway (ADR 0008), so the content reaching the
  client is no worse than before; it just isn't 4K.
- No status-bar warning is shown when the cap applies. Adding one is cheap and
  has not been done.
- The cap is the same number as the "size unknown" fallback in ADR 0008, so a
  stream is never above 1080p either way.
- What the target does with a camera-sized stream above 1080p stays unknown,
  and it is the thing that would let this be lifted.

## Revert condition

Remove the cap once the target can size its decoder and ring per camera (the
"not done" target-side item in ADR 0008) and a 4K stream is shown to open the
hardware decoder on the board. Until then, do not raise `kMaxWidth` or
`kMaxHeight` without re-running a 4K Play on a board.

# 0009. Re-encode the source to what the target decoder expects, never remux

Status: Accepted

Formerly `qcarcam-injector`'s `ARCHITECTURE.md` item 23 and item 31 "Bug 3".

## Context

An arbitrary source file (B-frames, a multi-second GOP, native 4K) doesn't match
what `qcarcam_receiver`'s hardware decoder pipeline assumes. Two simpler
designs were tried and rejected: remuxing the file's own H.264 bitstream over
plain RTP, then over plain MPEG-TS/UDP. Separately, after the source looped,
every pass ran unthrottled (80-115 fps instead of 30), and the receiver reported
sustained `Packet corrupt`.

## Decision

- **`CameraStream` is a real decode → `sws_scale` → encode → mux pipeline**
  built on libavcodec/libswscale, not a shell-out to the `ffmpeg` CLI. The
  encoder is libx264 with `tune=zerolatency`, no B-frames
  (`max_b_frames = 0`), a fixed GOP (`kGopSize`), `repeat-headers=1`, and CRF
  with a fixed preset (`kCrf`, `kPreset`). The output is `rtp_mpegts`. These
  match `sender/camera-streamer-pc.sh` in `qcarcam-injector`. The frame size is
  decided by ADR 0008.
- **Output timestamps are a plain incrementing frame counter**, not carried
  over from the source, so looping doesn't regress DTS at the receiver.
- **Pacing is relative to a wall-clock origin that advances at each loop
  restart** (`streamStartNs_`, advanced by the previous pass's last
  DTS-derived deadline). Without that, every deadline after `av_seek_frame`
  rewound the DTS landed in the past and `clock_nanosleep(TIMER_ABSTIME)`
  returned immediately.
- **One continuous session per receiver.** A looping source stays inside one
  stream. Starting a second sender session against an already-fed receiver
  isn't supported (`qcarcam-injector` ADR 0014).

## Relevance check

`CameraStream.cpp` has `tune`, `repeat-headers`, `max_b_frames`, `gop_size`,
`crf`, `preset`, `rtp_mpegts` and the `streamStartNs_` pacing. Confirmed on
hardware at the time: zero `Packet corrupt` over a multi-loop 40 s run at
about 30 fps.

## Consequences

- Re-encoding costs PC CPU and loses quality versus the source, which is the
  price of a stream the target decoder accepts reliably.
- A different sender (such as `camera-streamer-pc.sh`) must reproduce these
  settings, or the target will garble or stall.

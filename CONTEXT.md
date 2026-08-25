# Project: CamSyringe

Camera & sensor data injector for automotive ADAS HIL testing.

## What it is

A PC-side tool that replays recorded camera and sensor data over Ethernet, making a target ECU (QNX/Snapdragon Ride-class board) believe it is seeing live sensor input. Think of it as a HIL replay injector — it "syringes" pre-recorded data into the perception stack.

It is the PC-side companion to an existing project called `qcarcam-injector` running on the target side.

## Tech stack (decided)

| Layer | Choice |
|---|---|
| Language | C++17 |
| Media / RTP streaming | FFmpeg (libavformat, libavcodec, libavutil) — static linked |
| UI + live preview | Qt6 Widgets |
| BLF parsing | tobylorenz/vector_blf (C++, GPLv3) |
| Raw Ethernet frame injection | AF_PACKET (Linux) / Npcap SDK (Windows) |
| Build system | CMake + vcpkg |
| Packaging | AppImage (Linux), windeployqt + static FFmpeg (Windows) |

## Requirements

- Stream camera data via RTP/UDP over Ethernet from video files (MP4 preferred; ask providers to supply in this format)
- Support 2–4+ cameras simultaneously, each on its own thread and UDP port (5004, 5006, 5008, 5010...)
- Parse BLF (Vector Binary Logging Format) files and inject the Ethernet frames over the same NIC simultaneously with camera streams
- Future: add ultrasound (USS) sensor data — same BLF replayer thread pattern
- Camera view and USS data must be correlated: steering angle from BLF drives camera playback, USS distance values change with vehicle movement direction
- Live preview of all camera feeds and sensor signals on the host PC while streaming
- Must run on Linux and Windows
- Must ship as a single executable binary (AppImage on Linux, bundled exe on Windows)

## Architecture

```
┌─────────────────────────────────────────────────┐
│  UI Layer  (Qt6 — camera grid + signal monitor) │
└───────────────────┬─────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  Timeline Orchestrator                           │
│  Master steady_clock · sync all subsystems       │
└──────┬────────────────────────────┬──────────────┘
       │                            │
┌──────▼──────────┐   ┌─────────────▼─────────────┐
│ FFmpeg camera    │   │ BLF/ETH replayer          │
│ streamer         │   │ Pre-load → priority_queue │
│ 1 thread/camera  │   │ RT thread · raw socket    │
│ MP4→H264→RTP/UDP │   │ vector_blf library        │
└──────┬───────────┘   └─────────────┬─────────────┘
       │                             │
┌──────▼─────────────────────────────▼─────────────┐
│  Correlation engine                               │
│  SteeringAngle → camera speed                     │
│  USS distance → PDC overlay  (Phase 4)            │
└───────────────────┬────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  Real-time timing layer                          │
│  Linux: clock_nanosleep + SCHED_FIFO             │
│  Windows: timeBeginPeriod(1) + WaitableTimer      │
└────────────────────┬────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│  Ethernet NIC — RTP streams + raw ETH frames     │
└───────────────────────────────────────────────────┘
```

## Jitter mitigation (critical)

- Pre-load entire BLF into `std::priority_queue` sorted by timestamp before playback — no disk I/O on the hot path
- Camera demux runs in a producer thread filling a lock-free ring buffer; sender thread only calls `clock_nanosleep` + `sendto`
- Linux: `SCHED_FIFO` + `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` — absolute wakeup, no drift accumulation
- Windows: `timeBeginPeriod(1)` at startup, `WaitableTimer` for dispatch
- No `new`/`malloc` inside dispatch loops — pre-allocate all buffers
- All threads (camera + BLF) slave to the same `steady_clock` origin captured at playback start

## Known gotchas

Found while bringing up Phase 1 against the real `qcarcam-injector` target (a QNX/Snapdragon Ride-class board, `root@192.168.1.1`). Diagnosed by cross-referencing our sender's behavior against `/tmp/qcarcam_receiver.log` on the target — that log (especially its `TIMING PROBE` lines) is the single most useful debugging tool for this integration; read it before guessing. The target-side project lives at `~/labs/qnx/qnx_toolkit/work-dir/qcarcam-injector/`, and **its own `ARCHITECTURE.md` is the authoritative record of target-side behavior/bugs** — check it (especially the numbered "Resuming next session" list) before re-diagnosing something that's likely already characterized there.

- **A pure remux does not work on this target — it must be a transcode, matching `qcarcam-injector/sender/camera-streamer-pc.sh` exactly.** That script (the actual, proven reference PC-side tool) doesn't stream-copy the source's H.264; it re-encodes with `libx264 -tune zerolatency -g 30 -keyint_min 30 -x264-params repeat-headers=1`, scales down-only to fit 1920x1080, and muxes with `-f rtp_mpegts rtp://host:port` (MPEG-TS-in-RTP, RFC 2250 — not plain RTP/H.264, and not plain MPEG-TS/UDP either; both were tried first and both partially/fully failed). `qcarcam_receiver`'s hardware decode pipeline assumes zero-latency encode (no B-frames, PTS==DTS, so it can pace off DTS alone), a short self-describing GOP, and bounded resolution/bitrate — an arbitrary source file (B-frames, MP4's own multi-second GOP, native 4K) starves/corrupts the decoder's input queue no matter how correct the container/pacing is. `CameraStream` now runs a full decode → `sws_scale` → `libx264` encode → `rtp_mpegts` mux pipeline (see `src/camera/CameraStream.cpp`), not a remux. Validated on real hardware: 571+ continuously-decoded frames over a 30s run, varying content, hardware decoder correctly auto-sized at 1920x1080.

- **Pace the input read by DTS (decode order), not PTS.** `av_read_frame()` delivers packets in DTS order; with B-frames present in the source, PTS is not monotonic in that order, so pacing `clock_nanosleep` off it bursts frames through out of real-time order. Always compute the sleep deadline from `pkt->dts` before decode. (Downstream of decode, the zerolatency encoder produces its own monotonic PTS==DTS by construction, so this only matters for the read/decode stage.)

- **The receiver's stream probe alone can take 20-30 seconds — the sender must keep streaming (loop), not send one pass and exit.** `qcarcam_receiver` logs `TIMING PROBE: avformat_open_input+find_stream_info took ~25000 ms (... PC keeps sending during this)` — the target team's own comment stating the architecture assumption directly. `CameraStream::run()` loops the input file indefinitely (Ctrl+C to stop). Output timestamps are a plain incrementing frame counter (encoder `time_base = 1/fps`) rather than carried over from the source, so they stay monotonic across any number of loops with no special-casing needed.

- **`qcarcam_receiver` only tolerates ONE continuous stream per process run — this is a target-side limitation, not fixable from the sender (`qcarcam-injector/ARCHITECTURE.md` item 22).** A second, independent streaming session hands the still-open hardware decoder session fresh SPS/PPS and timestamps restarting near 0, which can wedge the VIDC hardware decode session at the OS driver level (`videoCore`), not just in `qcarcam_receiver`'s own process. Symptom: `qcarcam_injector` freezes on the last successfully-decoded frame forever (identical `byte_sum` across many "new" frames), and further `HwVideoDecoder: EMPTY_INPUT_BUFFER failed`/`Packet corrupt` errors never self-recover. **Restarting `run_qcarcam.sh` on the target is NOT always enough to fix this** — per that item, if the picture is garbled after a stream-source switch, an app-level restart does recover it; if it's frozen/black, only a full target power-cycle (which restarts `videoCore` itself) clears it. Practical rule: don't run a second sender session against an already-fed receiver process; restart `run_qcarcam.sh` first, and if a fresh `run_qcarcam.sh` still shows a frozen/black picture, stop retrying app restarts and power-cycle the target.

## Proposed directory structure

```
camsyringe/
├── CMakeLists.txt
├── vcpkg.json                  # deps: ffmpeg, qt6, vector-blf, npcap (win)
├── src/
│   ├── main.cpp
│   ├── orchestrator/
│   │   ├── Timeline.h/.cpp     # master clock, start/stop/seek
│   │   └── Config.h/.cpp       # JSON config (camera files, IPs, BLF path)
│   ├── camera/
│   │   ├── CameraStream.h/.cpp # one FFmpeg pipeline per camera
│   │   └── StreamPool.h/.cpp   # manages 2–4 CameraStream instances
│   ├── blf/
│   │   ├── BlfLoader.h/.cpp    # BLF → priority_queue
│   │   └── BlfReplayer.h/.cpp  # RT thread + raw socket injection
│   ├── correlation/
│   │   └── SignalBridge.h/.cpp # steering angle → camera speed
│   └── ui/
│       ├── MainWindow.h/.cpp
│       └── CameraWidget.h/.cpp # decoded frames → Qt display
└── .github/workflows/
    ├── build-linux.yml
    └── build-windows.yml
```

## Phased delivery

| Phase | Scope |
|---|---|
| 1 | Single camera · FFmpeg MP4→RTP/UDP · `clock_nanosleep` timing loop · Wireshark validation |
| 2 | Multi-camera (StreamPool) · BLF pre-load + RT replayer · raw socket injection · simultaneous sync |
| 3 | Qt6 UI · live camera grid preview · BLF signal monitor panel |
| 4 | Correlation engine · USS sensor data · PDC overlay · steering-angle-driven camera speed |

## Key reference repos

- github.com/FFmpeg/FFmpeg — study `doc/examples/remuxing.c` (your Phase 1 skeleton)
- github.com/Tobi1kenobi/vector_blf — C++ BLF library with examples
- github.com/leixiaohua1020/simplest_ffmpeg_streamer — closest existing C++ MP4→RTP example
- github.com/microsoft/vcpkg — dependency manager

## Good first Claude Code prompt to use after pasting this

> Let's start Phase 1. Scaffold the CMakeLists.txt and vcpkg.json for CamSyringe, then implement CameraStream.h and CameraStream.cpp — a class that opens an MP4 file with libavformat and streams it as RTP/H264 to a given rtp://host:port URL at native frame rate using clock_nanosleep for pacing. Linux only for now.

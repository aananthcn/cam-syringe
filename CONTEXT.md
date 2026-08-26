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

## UI requirements

- All playback/session controls live as plain top-level actions in the menu bar (`QMainWindow::menuBar()->addAction(...)`, not dropdowns) — **no button row or other competing control surface elsewhere in the window.** User-specified explicitly; anything added later (more session controls, etc.) goes in the menu bar too.
- The window builds idle (grid of camera tiles showing a placeholder) and does not auto-start streaming on launch — streaming only begins when the user presses Play (except `--playall`, see below).
- Three menu-bar actions, and a three-state playback model (`Idle` / `Playing` / `Paused`) — not just a running/not-running bool, because Configure's enabled state depends on distinguishing Paused from Idle:
  - **`▶ Play` / `⏸ Pause`** (toggle, existing): `Idle → Playing` starts all cameras; `Playing → Paused` stops them but leaves each tile's last frame visible (`Paused → Playing` restarts them — **there is no true pause/resume in `CameraStream`/`StreamPool`, "resuming" is actually starting fresh from the beginning of each file**, not resuming playback position; this is a known, accepted limitation, not a bug).
  - **`⏹ Stop`** (new): from `Playing` or `Paused`, fully stops all cameras, resets every tile back to its idle placeholder (unlike Pause, which keeps the last frame), and returns to `Idle`. This is the only action that unlocks Configure.
  - **`⚙ Configure`** (new): opens a single dialog for all session settings — target, control port, number of cameras (1-4), each camera's video file + QCarCam id, and the two session-wide target-side flags (`--inject-only`, `--qcx-bypass`). **Deliberately one dialog for everything, not one action per setting** — a separate quick "change just the target" action existed briefly and was removed in favor of this, specifically so future settings get a new field in this same dialog rather than another menu action. **Enabled only in `Idle` state — greyed out and non-interactive during both `Playing` and `Paused`.** Applying it replaces the camera list (and rebuilds the tile grid) for the next Play. Note: called "target" everywhere (CLI flag, internal naming, dialog field) — "host" was explicitly rejected as the wrong word for this project's domain (it's a HIL replay *target* board, not a generic network host).
- CLI arguments are now all optional: `camsyringe [--target [user@]<target>] [--control-port N] [--cam-ids IDS] [--inject-only] [--qcx-bypass] [--playall] [<video1> [<video2> [<video3> [<video4>]]]]`.
  - No arguments at all → launches with an empty camera list and immediately opens the Configure dialog so the user can set everything up from the UI, instead of showing an unexplained empty window.
  - `--target` defaults to `192.168.1.1` (the project's standard target address) when omitted — still editable via Configure before pressing Play.
  - `--cam-ids IDS` is **required** the moment any video files are given on the CLI (matches the UI, where every camera row has a QCarCam id field with no "unset" state) — comma-separated, same order as the video files, e.g. `--cam-ids 8,9`; a dash within one entry expands to an inclusive range, e.g. `1-3,8` → `1,2,3,8`. Comma (not dash) is the entry separator specifically because a bare dash is ambiguous the instant ids go double-digit or a range is wanted.
  - `--playall` starts streaming immediately on launch (skips waiting for a Play click) — **requires at least one video file to also be given; `--playall` with zero video files is a usage error**, not silently ignored.

## Target-side coordination (control channel)

Added to remove a real, error-prone bookkeeping problem: the QCarCam id each camera's stream should be injected into used to be chosen entirely on the target (a human typing `run_qcarcam.sh <id>` in a separate SSH session), completely disconnected from whatever port CamSyringe happened to be streaming to. Now CamSyringe is the single source of truth for both "how many cameras" and "which id each one uses," and tells the target automatically.

- **Protocol**: a plain TCP connection to the target's `qcarcam_dispatcher` (default port 5000, see `~/labs/qnx/qnx_toolkit/work-dir/qcarcam-injector/ARCHITECTURE.md` items 29-31 for the full target-side story) — one `CAM <id> <port>` line per camera, an optional `FLAGS --inject-only --qcx-bypass` line, then `END`. The target replies `READY <id>` / `ERROR <id> <reason>` per camera then `DONE`, and the connection is held open for the whole session — **closing it (Stop, Pause, or window close) is itself the target-side teardown signal**, not a separate message (`src/net/DispatcherClient.h/.cpp`).
- **Critical ordering, confirmed the hard way on real hardware: streaming starts BEFORE the declaration completes, never after.** The target's own receiver can't report `READY` until it has actually received and probed real RTP data — gating `StreamPool::startAll()` on `READY` first deadlocks both sides waiting on each other (confirmed for real: the very first end-to-end test hung until a 90s client-side timeout, with the target's receiver log showing `failed to retrieve stream info` the whole time because no data had ever arrived). `MainWindow::startStreaming()` therefore calls `pool_->startAll()` immediately, and the declaration runs asynchronously on top of already-live streaming; a camera the target later reports `ERROR` for gets `StreamPool::setEnabled(i, false)` (skips it on a future Pause→Play restart) and an error banner on its tile, but its already-running LOCAL encode/preview is deliberately left alone rather than torn down mid-stream (no per-camera stop exists yet, only `stopAll()`). If the control channel itself is unreachable, streaming still proceeds locally regardless — this feature only ever adds target-side coordination on top of what already worked, never blocks it.
- **Networking is plain POSIX sockets on a background `std::thread`, not `QTcpSocket`** — matches `StreamPool`'s own existing convention (background thread + a callback the caller marshals to the GUI thread via `QMetaObject::invokeMethod`) rather than introducing a second async style alongside it.
- `DispatcherClient::declareAsync()` is self-cleaning (calls `disconnect()`/joins any previous session's thread first) — assigning a new `std::thread` over a still-joinable one calls `std::terminate()`, so callers never need to remember the ordering themselves.

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

- **A 4K source (e.g. `~/Downloads/drive-12635403_3840_2160_30fps.mp4`) decoded at only ~10-12fps on the PC despite the target rendering it at ~24fps — root cause: the decoder was running single-threaded.** `AVCodecContext`'s actual compiled-in default for `thread_count` is **1**, not 0/auto — unlike `ffmpeg`'s own CLI, which explicitly negotiates auto-threading per codec before opening it. Since `CameraStream::openDecoder()` never touched `decCtx_->thread_count`, every decode ran on a single core; at 4K that alone made decode the bottleneck (measured: 6.1s of an 11.1s wall-clock budget for 120 frames, one core pegged while 31 sat idle). The *encoder's* thread count was a red herring — it already defaulted to 0/auto correctly and was extensively tested (0/1/4/16/32) without effect, because it was never the bottleneck. Fix: explicitly set `decCtx_->thread_count = 0` before `avcodec_open2()` in `openDecoder()` (one line). Verified with a headless `StreamPool`-only repro (no Qt): steady-state cam0 fps went from a flat ~10.6fps to ~21-24fps, matching both the target's observed rendering rate and an equivalent `ffmpeg` CLI run almost exactly. Lower-resolution cameras (e.g. 1024x576) never showed this because single-threaded software H.264 decode is fast enough at that resolution to stay real-time regardless.

- **A pure remux does not work on this target — it must be a transcode, matching `qcarcam-injector/sender/camera-streamer-pc.sh` exactly.** That script (the actual, proven reference PC-side tool) doesn't stream-copy the source's H.264; it re-encodes with `libx264 -tune zerolatency -g 30 -keyint_min 30 -x264-params repeat-headers=1`, scales down-only to fit 1920x1080, and muxes with `-f rtp_mpegts rtp://host:port` (MPEG-TS-in-RTP, RFC 2250 — not plain RTP/H.264, and not plain MPEG-TS/UDP either; both were tried first and both partially/fully failed). `qcarcam_receiver`'s hardware decode pipeline assumes zero-latency encode (no B-frames, PTS==DTS, so it can pace off DTS alone), a short self-describing GOP, and bounded resolution/bitrate — an arbitrary source file (B-frames, MP4's own multi-second GOP, native 4K) starves/corrupts the decoder's input queue no matter how correct the container/pacing is. `CameraStream` now runs a full decode → `sws_scale` → `libx264` encode → `rtp_mpegts` mux pipeline (see `src/camera/CameraStream.cpp`), not a remux. Validated on real hardware: 571+ continuously-decoded frames over a 30s run, varying content, hardware decoder correctly auto-sized at 1920x1080.

- **Pace the input read by DTS (decode order), not PTS.** `av_read_frame()` delivers packets in DTS order; with B-frames present in the source, PTS is not monotonic in that order, so pacing `clock_nanosleep` off it bursts frames through out of real-time order. Always compute the sleep deadline from `pkt->dts` before decode. (Downstream of decode, the zerolatency encoder produces its own monotonic PTS==DTS by construction, so this only matters for the read/decode stage.)

- **The receiver's stream probe alone can take 20-30 seconds — the sender must keep streaming (loop), not send one pass and exit.** `qcarcam_receiver` logs `TIMING PROBE: avformat_open_input+find_stream_info took ~25000 ms (... PC keeps sending during this)` — the target team's own comment stating the architecture assumption directly. `CameraStream::run()` loops the input file indefinitely (Ctrl+C to stop). Output timestamps are a plain incrementing frame counter (encoder `time_base = 1/fps`) rather than carried over from the source, so they stay monotonic across any number of loops with no special-casing needed.

- **`qcarcam_receiver` only tolerates ONE continuous stream per process run — this is a target-side limitation, not fixable from the sender (`qcarcam-injector/ARCHITECTURE.md` item 22).** A second, independent streaming session hands the still-open hardware decoder session fresh SPS/PPS and timestamps restarting near 0, which can wedge the VIDC hardware decode session at the OS driver level (`videoCore`), not just in `qcarcam_receiver`'s own process. Symptom: `qcarcam_injector` freezes on the last successfully-decoded frame forever (identical `byte_sum` across many "new" frames), and further `HwVideoDecoder: EMPTY_INPUT_BUFFER failed`/`Packet corrupt` errors never self-recover. **Restarting `run_qcarcam.sh` on the target is NOT always enough to fix this** — per that item, if the picture is garbled after a stream-source switch, an app-level restart does recover it; if it's frozen/black, only a full target power-cycle (which restarts `videoCore` itself) clears it. Practical rule: don't run a second sender session against an already-fed receiver process; restart `run_qcarcam.sh` first, and if a fresh `run_qcarcam.sh` still shows a frozen/black picture, stop retrying app restarts and power-cycle the target.

- **The build still uses system FFmpeg via `pkg-config`, not vcpkg, despite `vcpkg.json` existing.** `vcpkg` is not installed/bootstrapped anywhere on the dev machine as of Phase 2; `CMakeLists.txt` locates FFmpeg/Qt6 through `pkg_check_modules`/`find_package` against system packages. `vcpkg.json` stays as a forward-looking manifest (matching the "CMake + vcpkg" tech-stack decision) but isn't wired into the actual CMake configure yet — don't assume `vcpkg install` does anything today.

- **This dev machine has more than one Qt6 installation** (system `qt6-base-dev` package, plus unrelated Qt Online Installer copies under `~/Qt/*` and `~/sdk/rpi/*`). `find_package(Qt6 COMPONENTS Widgets REQUIRED)` resolves the system one by default with a clean/incremental `cmake ..` — but if a build ever picks up the wrong Qt (odd link errors, mismatched Qt version in the configure log), check `CMAKE_PREFIX_PATH`/stale cache first rather than assuming a code bug.

- **An unreachable destination host causes ~1fps (not a crash, not an error) — easily mistaken for a CPU/multi-camera performance problem, but it's neither.** First seen running 4 cameras and assumed to be CPU contention from 4 simultaneous `preset=medium` libx264 encodes — **wrong theory, disproven**: CPU stayed idle (`load average: 0.14`) the whole time, and a minimal headless repro (no Qt, no GUI, 1-4 cameras, `StreamPool` directly) showed the exact same ~1fps whenever the destination was the (at-the-time-unreachable) real target `192.168.1.1`, and a clean 30fps to `127.0.0.1`. `strace` on the repro showed the real mechanism: `sendto()` on the RTP/UDP socket bursts through one keyframe's worth of packets in ~1ms, then **stalls for ~1.5 seconds** before the next burst — the OS's ARP/neighbor-resolution retry cycle for an unreachable host (`ip neigh show <host>` reads `FAILED`) blocking outgoing UDP sends, not our encode or pacing code. This happens with even ONE camera pointed at a dead host, nothing to do with camera count. **Always check `ping <host>` before debugging a low-fps report** — `CameraStream`'s per-camera FPS log (`camsyringe[cam0]: 24.3 fps (target 30.0 fps)`, roughly once/second) makes the symptom visible, but doesn't by itself distinguish "target unreachable" from "genuinely CPU-bound"; `ip neigh show <host>` / `ping` does. Not yet addressed: `open()` succeeds regardless of reachability (UDP is connectionless, nothing to fail at open time) and there's no code-level detection/warning for this case yet.

- **`CameraStream`'s loop-restart pacing was completely broken — every pass after the first ran fully unthrottled, producing garbled/corrupted video on the receiving end.** Found via real end-to-end testing against the target's new `qcarcam_dispatcher` (see "Target-side coordination" above): the target's receiver log showed sustained `Packet corrupt` throughout the whole run (not just a transient startup burst), and CamSyringe's own achieved-fps log jumped from a correct ~30fps on the first pass to 80-115fps on every pass after the source video looped. Root cause: `streamStartNs_` (the origin every DTS-derived pacing deadline is computed relative to) was set once before the outer loop and never advanced — after `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)` rewinds to the file start, the next pass's packets have DTS values starting near 0 again too, so every deadline (`streamStartNs_ + tiny_value`) lands well in the PAST relative to real wall-clock time (which has moved on by the previous pass's actual duration) — `clock_nanosleep(TIMER_ABSTIME, ...)` on an already-past deadline returns immediately, so pacing silently stopped working on every loop after the first. Fixed in `CameraStream::run()`: advance `streamStartNs_` by the pass's own last DTS-derived deadline at each loop restart. Verified on real hardware: zero `Packet corrupt` over a full 40s run spanning multiple loop passes, steady ~29-30fps throughout, clean video on both CamSyringe's own preview and the target panel. **This bug predates the dispatcher work entirely** — Phase 1/2 testing never happened to watch fps across a loop boundary closely enough to notice it; if a similarly-described corruption report ever comes up again, check this fix is actually present before re-diagnosing from scratch.

- **Never send synthetic mouse/keyboard input (e.g. via `python-xlib`/XTest, `xdotool`) to `DISPLAY=:1` on this machine to test the UI — it's the user's live, actively-used desktop session, not an isolated test display.** Confirmed the hard way during Phase 2 UI verification: a screenshot mid-test showed the user's own Firefox/other windows in front, meaning synthetic clicks had been landing on whatever was frontmost, not necessarily CamSyringe. Screenshots for passive visual verification (`import -window root`) are fine; synthetic input is not. If GUI interaction needs testing, ask the user to click it themselves, or use a separate isolated display (e.g. `Xvfb`) instead.

## Directory structure

```
camsyringe/
├── CMakeLists.txt
├── vcpkg.json                  # deps: ffmpeg[x264] (system pkg-config used for the actual
│                                # build today, not vcpkg -- see "Known gotchas")
├── src/
│   ├── main.cpp                 # argv parsing -> StreamPool -> MainWindow -> app.exec()
│   ├── util/
│   │   └── Clock.h               # DONE -- shared monotonicNowNs(), header-only
│   ├── orchestrator/
│   │   ├── Timeline.h/.cpp       # DONE -- shared CLOCK_MONOTONIC origin for all cameras
│   │   └── Config.h/.cpp         # NOT BUILT -- JSON config (camera files, IPs, BLF path),
│   │                              # deferred until BLF integration needs multi-field config;
│   │                              # StreamPool::addCamera(CameraConfig) is the seam for it
│   ├── camera/
│   │   ├── CameraStream.h/.cpp   # DONE -- one decode/scale/encode/mux pipeline per camera
│   │   └── StreamPool.h/.cpp     # DONE -- owns N CameraStream + N std::thread, Qt-free
│   ├── blf/                      # NOT BUILT -- deferred phase, see Phased delivery
│   │   ├── BlfLoader.h/.cpp      # BLF → priority_queue
│   │   └── BlfReplayer.h/.cpp    # RT thread + raw socket injection
│   ├── correlation/              # NOT BUILT -- deferred phase
│   │   └── SignalBridge.h/.cpp   # steering angle → camera speed
│   └── ui/
│       ├── MainWindow.h/.cpp     # DONE -- camera grid, menu-bar Play/Pause (see UI requirements)
│       └── CameraWidget.h/.cpp   # DONE -- one tile: title + live frame + status/error
└── .github/workflows/            # NOT BUILT
    ├── build-linux.yml
    └── build-windows.yml
```

## Phased delivery

| Phase | Scope | Status |
|---|---|---|
| 1 | Single camera · FFmpeg transcode → RTP-MPEG-TS · `clock_nanosleep` timing loop | **Done**, validated on real target hardware |
| 2 | Multi-camera `StreamPool` · Qt6 live preview grid · menu-bar Play/Pause · shared `Timeline` clock | **Done**, validated locally (see "Known gotchas") |
| 3 | BLF pre-load + RT replayer · raw Ethernet (`AF_PACKET`) socket injection · BLF signal monitor panel | Not started |
| 4 | Correlation engine · USS sensor data · PDC overlay · steering-angle-driven camera speed | Not started |

**Note on reordering**: the original roadmap put BLF/raw-socket work right after single-camera Phase 1. It was swapped for multi-camera StreamPool + Qt6 UI instead, because the real target's physical display can only show one camera at a time (each `qcarcam_receiver`/camera id needs its own `run_qcarcam.sh <id>` process) — there was no way to visually confirm multiple simultaneous camera streams without a local preview. BLF/raw-socket injection (originally "Phase 2") is now Phase 3.

## Key reference repos

- github.com/FFmpeg/FFmpeg — study `doc/examples/remuxing.c` (your Phase 1 skeleton)
- github.com/Tobi1kenobi/vector_blf — C++ BLF library with examples
- github.com/leixiaohua1020/simplest_ffmpeg_streamer — closest existing C++ MP4→RTP example
- github.com/microsoft/vcpkg — dependency manager

## Good first Claude Code prompt to use after pasting this

> Let's start Phase 1. Scaffold the CMakeLists.txt and vcpkg.json for CamSyringe, then implement CameraStream.h and CameraStream.cpp — a class that opens an MP4 file with libavformat and streams it as RTP/H264 to a given rtp://host:port URL at native frame rate using clock_nanosleep for pacing. Linux only for now.

# CamSyringe

Camera & sensor data injector for automotive ADAS HIL testing. See [CONTEXT.md](CONTEXT.md) for the full design and roadmap.

Phase 3 (current): a Qt6 app that streams 1-4 cameras simultaneously, each transcoded to H.264 (zero-latency, scaled to fit 1920x1080) and sent as MPEG-TS-in-RTP at native frame rate on its own port (5004, 5006, 5008, 5010), looping until stopped, **and** (new) replays a Vector BLF file's captured Ethernet frames as raw `AF_PACKET` traffic, original timing, verbatim — see [CONTEXT.md](CONTEXT.md) for the full design. Camera streams are auto-configured on the target via a control-channel handshake with `qcarcam_dispatcher` (see `qcarcam-injector`'s own `ARCHITECTURE.md`) — no more manually typing a QCarCam id into a target-side SSH session. A live preview grid lets you confirm all cameras are streaming correctly from the PC side, independent of the target's single-camera-at-a-time display. Linux only.

## Prerequisites

- CMake >= 3.20
- A C++17 compiler (g++ or clang++)
- pkg-config
- FFmpeg development headers with libx264 support (`libavformat`, `libavcodec`, `libavutil`, `libswscale`)
- Qt6 development headers (Widgets module)
- zlib development headers (for `vector_blf`, the BLF-parsing dependency)
- git (to fetch the `vector_blf` submodule)

Ubuntu/Debian:

```
sudo apt-get install -y build-essential cmake pkg-config libavformat-dev libavcodec-dev libavutil-dev libswscale-dev qt6-base-dev zlib1g-dev
```

Fedora:

```
sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config ffmpeg-devel qt6-qtbase-devel zlib-devel
```

Arch:

```
sudo pacman -S --needed base-devel cmake pkgconf ffmpeg qt6-base zlib
```

## Build

```
cd cam-syringe
git submodule update --init --recursive   # fetches third_party/vector_blf
mkdir build
cd build
cmake ..
cmake --build .
```

The `camsyringe` binary is written to `build/`. If you have other Qt installations on your machine (e.g. the Qt online installer under `~/Qt`), check `cmake ..`'s configure log resolves `Qt6` from your system package (`/usr/lib/.../cmake/Qt6`), not one of those.

**BLF replay needs `CAP_NET_RAW`** to open a raw `AF_PACKET` socket at all — plain user privileges aren't enough, and running the whole app as root isn't necessary either. One-time setup after each build:

```
sudo setcap cap_net_raw+ep build/camsyringe
```

Camera streaming and everything else works fine without this; it's only needed if you actually check "Replay BLF Ethernet capture" in Configure (or pass `--blf-file`).

## Run

```
./camsyringe [--target [user@]<target>] [--control-port N] [--cam-ids IDS] [--playall]
             [--inject-only] [--blf-file PATH] [--blf-interface IFACE]
             [<video1> [<video2> <video3> <video4>]]
```

Every argument is optional, EXCEPT `--cam-ids`, which becomes required the moment any video files are given (comma-separated QCarCam ids, same order as the video files — a dash within one entry expands to an inclusive range, e.g. `1-3,8` means `1,2,3,8`; each id must be unique). Ports are assigned automatically in order: 5004, 5006, 5008, 5010. `--target` defaults to `192.168.1.1`; an optional `user@` prefix is accepted (and ignored) so you can paste the same address you SSH to the target with. `--control-port` (default 5000) is `qcarcam_dispatcher`'s control-channel port on the target. `--inject-only` is forwarded to the target's declaration (see `qcarcam-injector`'s own `ARCHITECTURE.md` items 29-31 for what it does). `--blf-file`/`--blf-interface` enable BLF/Ethernet replay (see below). Run with no arguments at all and the window opens with the **Configure** dialog already up, so you can set everything from the UI instead. `--playall` starts streaming immediately on launch instead of waiting for a Play click — it requires at least one video file also be given.

Examples:

```
./camsyringe                                                          # opens the Configure dialog on launch
./camsyringe --target root@192.168.1.1 --cam-ids 8,9 cam-front.mp4 cam-rear.mp4
./camsyringe --cam-ids 8,9 --playall cam-front.mp4 cam-rear.mp4       # streams immediately, no click needed
./camsyringe --blf-file capture.blf --blf-interface eth0             # BLF replay only, no cameras (via Configure)
```

The menu bar has three controls:
- **▶ Play / ⏸ Pause** — starts everything (camera streams AND BLF replay, if configured) immediately, THEN asynchronously declares each camera to the target's `qcarcam_dispatcher` — see `CONTEXT.md`'s "Target-side coordination" section for why streaming can't wait on that response first (it would deadlock). Pausing leaves each tile's last frame on screen and closes the target control connection (the target's teardown signal); pressing Play again restarts fresh (there's no true pause/resume, just stop-and-restart) and re-declares.
- **⏹ Stop** — fully stops everything and resets every tile back to its idle placeholder.
- **⚙ Configure** — the single dialog for all session settings: target, control port, number of cameras (1-4) with each one's video file + QCarCam id, `--inject-only`, and BLF file + network interface. Only enabled while stopped (`Idle` state) — greyed out during both Play and Pause, since changing settings mid-stream isn't supported.

Each camera input can be any video file libavformat can decode; every camera is independently re-encoded (not remuxed) to zero-latency H.264, scaled down (never up) to fit within 1920x1080, matching what the target's hardware decoder pipeline expects, and looped indefinitely while playing. A camera the target rejects (wrong/unconfigured QCarCam id) shows an error on just its own tile but keeps streaming locally — the others are unaffected.

Each camera prints its achieved encode/send FPS to stderr roughly once a second (`camsyringe[cam0]: 24.3 fps (target 30.0 fps)`), so if the PC can't keep multiple cameras real-time (e.g. 4 cameras on a modest CPU), you'll see exactly which ones are falling behind rather than just a vague "it feels laggy."

**Important**: the target's `qcarcam_receiver` only tolerates one continuous stream per process run — restart `run_qcarcam.sh` on the target before each new CamSyringe session; don't start a second streaming session against an already-fed receiver (see `CONTEXT.md`'s "Known gotchas").

Validate a stream is on the wire with Wireshark or `tcpdump -i <iface> udp portrange 5004-5010` (camera RTP) / `tcpdump -i <iface> ether ...` (BLF replay, whatever MAC/protocol the capture used).

# CamSyringe

Camera & sensor data injector for automotive ADAS HIL testing. See [CONTEXT.md](CONTEXT.md) for the full design and roadmap.

Phase 2 (current): a Qt6 app that streams 1-4 cameras simultaneously, each transcoded to H.264 (zero-latency, scaled to fit 1920x1080) and sent as MPEG-TS-in-RTP at native frame rate on its own port (5004, 5006, 5008, 5010), looping until stopped. Matches the parameters of the target's proven reference PC-side tool, `qcarcam-injector/sender/camera-streamer-pc.sh`. A live preview grid lets you confirm all cameras are streaming correctly from the PC side, independent of the target's single-camera-at-a-time display. Linux only.

## Prerequisites

- CMake >= 3.20
- A C++17 compiler (g++ or clang++)
- pkg-config
- FFmpeg development headers with libx264 support (`libavformat`, `libavcodec`, `libavutil`, `libswscale`)
- Qt6 development headers (Widgets module)

Ubuntu/Debian:

```
sudo apt-get install -y build-essential cmake pkg-config libavformat-dev libavcodec-dev libavutil-dev libswscale-dev qt6-base-dev
```

Fedora:

```
sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config ffmpeg-devel qt6-qtbase-devel
```

Arch:

```
sudo pacman -S --needed base-devel cmake pkgconf ffmpeg qt6-base
```

## Build

```
cd cam-syringe
mkdir build
cd build
cmake ..
cmake --build .
```

The `camsyringe` binary is written to `build/`. If you have other Qt installations on your machine (e.g. the Qt online installer under `~/Qt`), check `cmake ..`'s configure log resolves `Qt6` from your system package (`/usr/lib/.../cmake/Qt6`), not one of those.

## Run

```
./camsyringe [--target [user@]<target>] [--playall] [<video1> [<video2> <video3> <video4>]]
```

Every argument is optional. Ports are assigned automatically in order: 5004, 5006, 5008, 5010. `--target` defaults to `192.168.1.1`; an optional `user@` prefix is accepted (and ignored) so you can paste the same address you SSH to the target with. Run with no arguments at all and the window opens with the **Configure** dialog already up, so you can set the target, camera count, and video files from the UI instead. `--playall` starts streaming immediately on launch instead of waiting for a Play click — it requires at least one video file also be given.

Examples:

```
./camsyringe                                                # opens the Configure dialog on launch
./camsyringe --target root@192.168.1.1 cam-front.mp4 cam-rear.mp4
./camsyringe --playall cam-front.mp4 cam-rear.mp4           # streams immediately, no click needed
```

The menu bar has three controls:
- **▶ Play / ⏸ Pause** — start/pause all cameras. Pausing leaves each tile's last frame on screen; pressing Play again restarts each camera from the beginning of its file (there's no true pause/resume, just stop-and-restart).
- **⏹ Stop** — fully stops all cameras and resets every tile back to its idle placeholder.
- **⚙ Configure** — the single dialog for all session settings: target, number of cameras (1-4), and each camera's video file. Only enabled while stopped (`Idle` state) — greyed out during both Play and Pause, since changing cameras mid-stream isn't supported.

Each input can be any video file libavformat can decode; every camera is independently re-encoded (not remuxed) to zero-latency H.264, scaled down (never up) to fit within 1920x1080, matching what the target's hardware decoder pipeline expects, and looped indefinitely while playing. A camera that fails to open (e.g. an unreachable host) shows an error on just its own tile — the others keep streaming.

Each camera prints its achieved encode/send FPS to stderr roughly once a second (`camsyringe[cam0]: 24.3 fps (target 30.0 fps)`), so if the PC can't keep multiple cameras real-time (e.g. 4 cameras on a modest CPU), you'll see exactly which ones are falling behind rather than just a vague "it feels laggy."

**Important**: the target's `qcarcam_receiver` only tolerates one continuous stream per process run — restart `run_qcarcam.sh` on the target before each new CamSyringe session; don't start a second streaming session against an already-fed receiver (see `CONTEXT.md`'s "Known gotchas").

Validate a stream is on the wire with Wireshark or `tcpdump -i <iface> udp portrange 5004-5010`.

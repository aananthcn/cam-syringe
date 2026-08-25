# CamSyringe

Camera & sensor data injector for automotive ADAS HIL testing. See [CONTEXT.md](CONTEXT.md) for the full design and roadmap.

Phase 1 (current): transcode a video file to H.264 (zero-latency, scaled to fit 1920x1080) and stream it as MPEG-TS-in-RTP at native frame rate, looping until stopped. Matches the parameters of the target's proven reference PC-side tool, `qcarcam-injector/sender/camera-streamer-pc.sh`. Linux only.

## Prerequisites

- CMake >= 3.20
- A C++17 compiler (g++ or clang++)
- pkg-config
- FFmpeg development headers with libx264 support (`libavformat`, `libavcodec`, `libavutil`, `libswscale`)

Ubuntu/Debian:

```
sudo apt-get install -y build-essential cmake pkg-config libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

Fedora:

```
sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config ffmpeg-devel
```

Arch:

```
sudo pacman -S --needed base-devel cmake pkgconf ffmpeg
```

## Build

```
cd cam-syringe
mkdir build
cd build
cmake ..
cmake --build .
```

The `camsyringe` binary is written to `build/`.

## Run

```
./camsyringe <input-video> [user@]<host>
```

The port (5004) is fixed to match the target's camera receiver — only the host is needed, and an optional `user@` prefix is accepted (and ignored) so you can paste the same address you SSH to the target with.

Example:

```
./camsyringe input.mp4 root@192.168.1.1
```

Loops the file indefinitely at native frame rate — Ctrl+C to stop. The input can be any video file libavformat can decode; it's re-encoded (not remuxed) to zero-latency H.264, scaled down (never up) to fit within 1920x1080, matching what the target's hardware decoder pipeline expects.

**Important**: the target's `qcarcam_receiver` only tolerates one continuous stream per process run — restart `run_qcarcam.sh` on the target before each new CamSyringe session; don't start a second streaming session against an already-fed receiver (see `CONTEXT.md`'s "Known gotchas").

Validate the stream is on the wire with Wireshark or `tcpdump -i <iface> udp port 5004`.

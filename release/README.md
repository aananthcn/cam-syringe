# cam-syringe releases

This directory holds the PC-side release process: one script that
packages a built `camsyringe` into something a teammate can install and
run on their own Linux PC without needing the C++ build toolchain (Qt6/
FFmpeg `-dev` headers, cmake, the `vector_blf` submodule) at all.

## What `create-bundle.sh` does

Builds `camsyringe` and packages it together with `libVector_BLF` --
**this project's own vendored/patched library, the one thing that
genuinely can't come from `apt install`** -- into a single
self-extracting `.bin` file, plus a small `run-camsyringe.sh` launcher
that points the bundled library at the binary correctly.

It deliberately does **not** bundle Qt6/FFmpeg/X11 themselves, unlike
qcarcam-injector's target-side bundle (which has no equivalent of
`apt install` on a fixed QNX BSP image). On a PC, those are standard
Ubuntu packages -- bundling FFmpeg's own full transitive dependency
closure by hand would mean vendoring 100+ shared libraries (confirmed via
`ldd` against the real binary: every optional codec/muxer/protocol the
distro's `libavformat` package pulls in, from x264 to librsvg to
zeromq), which would be both enormous and fragile across Ubuntu point
releases. `run-camsyringe.sh` checks the handful of runtime packages
camsyringe actually links against directly are installed, and prints the
exact `apt-get install` command if anything's missing, rather than
failing with a cryptic "error while loading shared libraries."

See the script's own header comment for the full reasoning and exactly
how the runtime package list was determined (`dpkg -S` against the real
linked libraries, not guessed).

## Building a bundle

From this directory, on your Linux build machine (needs this project's
own build Prerequisites -- see the main `README.md`):

```bash
./create-bundle.sh --version 0.5
```

This builds `camsyringe` fresh (pass `--skip-build` to package an
already-built `build/` as-is instead) and produces
`release/artifacts/camsyringe_bundle_v0.5.bin` (gitignored -- release
bundles are build output, not source; distribute them separately, e.g.
attached to a GitHub Release or shared drive, not committed to this
repo).

Useful options (see `./create-bundle.sh --help` for the full list):
- `--output PATH` -- write somewhere other than the default
  `release/artifacts/camsyringe_bundle_vX.Y.bin`.
- `--build-dir PATH` -- use a build directory other than `build/`.
- `--compress` -- gzip the payload for a smaller file.

## Installing and running the bundle

### What you need

- **A Linux PC** (this bundle is not portable to Windows/macOS --
  camsyringe itself is Linux-only, see the main `README.md`).
- The one file: `camsyringe_bundle_vX.Y.bin`.
- `sudo` access, ONLY if the Qt6/FFmpeg runtime packages listed below
  aren't already installed -- the bundle itself needs no special
  privileges to install or run.

### 1. Copy the bundle over and install it

```bash
scp camsyringe_bundle_v0.5.bin teammate@pc:/tmp/
ssh teammate@pc
/tmp/camsyringe_bundle_v0.5.bin
```

Installs into `$HOME/camsyringe` by default (no root needed). It's
**additive** -- never deletes anything already there, so it's always safe
to re-run. To install somewhere else:

```bash
CAMSYRINGE_INSTALL_DIR=/some/other/dir /tmp/camsyringe_bundle_v0.5.bin
```

### 2. Run it

```bash
~/camsyringe/run-camsyringe.sh
```

The first time, this may print something like:

```
camsyringe: missing runtime package(s): libqt6widgets6 libavformat58
Install with:
  sudo apt-get install -y libqt6widgets6 libavformat58
```

Run the printed command once, then re-run `run-camsyringe.sh` -- after
that, everything's in place for good (these are normal system packages,
not something this bundle needs to redo). With no arguments, camsyringe
opens its Configure dialog on launch; see the main `README.md`'s "Run"
section for the full CLI flags (`--target`, `--cam-ids`, `--blf-file`,
etc.) if you want to script a session instead.

**BLF/Ethernet replay** (the "Replay BLF Ethernet capture" checkbox in
Configure, or `--blf-file`) additionally needs `CAP_NET_RAW` on the
installed binary -- this is NOT something the bundle installer can set
for you (capabilities are per-file and this bundle doesn't run as root),
so if you plan to use it:

```bash
sudo setcap cap_net_raw+ep ~/camsyringe/bin/camsyringe
```

Camera streaming works fine without this; it's only needed for BLF
replay. Re-run this after every fresh bundle install (a new binary means
the capability needs setting again).

### What "it's working" looks like

Point it at a real qcarcam-injector target (see that project's own
`release/README.md` for getting the target side running first), press
Play, and confirm each configured camera tile shows a live, updating
local preview -- independent of whether the target itself is reachable,
since camsyringe streams locally regardless (see the main `README.md`'s
own note on this). A target-side rejection (wrong/unconfigured QCarCam
id) shows an error banner on just that camera's tile, not a crash.

## Files here

```
release/
  create-bundle.sh   the packaging script (see its own header comment
                      for the full runtime-dependency reasoning)
  README.md          this file
  artifacts/          gitignored -- where built bundles land
```

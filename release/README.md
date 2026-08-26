# cam-syringe releases

This directory holds the PC-side release process: one script that
packages a built `camsyringe` into something a teammate can install and
run on their own Linux PC with **zero extra steps** -- no `apt install`,
no C++ build toolchain, nothing beyond the one file. Bundles could end up
anywhere on a receiving machine that may not have this source tree, or
any dev tooling, on it at all, so the bundle carries everything it needs.

## What `create-cam-syringe-bundle.sh` does

Builds `camsyringe` and packages it together with its **entire runtime
dependency closure** -- Qt6, FFmpeg, X11/xcb, and everything they need in
turn, ~140 shared libraries, computed dynamically via `ldd` against the
real built binaries every time this script runs (not a hand-maintained
list, which would go stale the moment Qt/FFmpeg gets updated on the
build machine) -- plus Qt's `xcb` platform plugin (required to open a
window at all; Qt `dlopen()`s this at runtime, so it doesn't show up in
camsyringe's own `ldd` output the way a normal dependency would, and it
pulls in real additional libraries of its own). Only the true kernel/
glibc syscall-ABI set (libc, libm, libdl, libpthread, the dynamic linker
itself) is left out -- that's the one category actually risky to bundle
across different machines, and safe to assume present on any x86_64
Linux system. Everything else -- including this project's own vendored
`libVector_BLF` -- travels with the bundle.

See the script's own header comment for the full reasoning, and its
`EXCLUDE_SONAME_RE` for the exact (short) exclusion list.

## Building a bundle

From this directory, on your Linux build machine (needs this project's
own build Prerequisites -- see the main `README.md`):

```bash
./create-cam-syringe-bundle.sh 0.5
```

This builds `camsyringe` fresh (pass `--skip-build` to package an
already-built `build/` as-is instead) and produces
`release/artifacts/camsyringe_bundle_v0.5.bin` (gitignored -- release
bundles are build output, not source; distribute them separately, e.g.
attached to a GitHub Release or shared drive, not committed to this
repo).

Note the bundle is considerably larger than just the binary now (~180MB,
vs ~8MB for a version that relied on the receiving machine's own Qt6/
FFmpeg packages) -- that's the real cost of "zero extra install steps,"
not a bug.

Useful options (see `./create-cam-syringe-bundle.sh --help` for the full list):
- `--output PATH` -- write somewhere other than the default
  `release/artifacts/camsyringe_bundle_vX.Y.bin`.
- `--build-dir PATH` -- use a build directory other than `build/`.
- `--qt-plugin-dir PATH` -- if Qt6's plugins aren't at the default
  `/usr/lib/x86_64-linux-gnu/qt6/plugins` on your build machine, point
  this at wherever `platforms/libqxcb.so` actually lives.
- `--compress` -- gzip the payload for a smaller file.

## Installing and running the bundle

### What you need

- **A Linux PC, x86_64** (this bundle is not portable to Windows/macOS --
  camsyringe itself is Linux-only, see the main `README.md` -- nor to
  ARM). Doesn't need to be Ubuntu specifically, or have anything from
  this project on it already -- the bundle carries its own Qt6/FFmpeg/X11
  closure rather than relying on the receiving machine's own packages.
- The one file: `camsyringe_bundle_vX.Y.bin`. No `sudo` needed for
  anything -- installing and running both work as a normal user.

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

That's it -- no package install step, nothing else to check first. With
no arguments, camsyringe opens its Configure dialog on launch; see the
main `README.md`'s "Run" section for the full CLI flags (`--target`,
`--cam-ids`, `--blf-file`, etc.) if you want to script a session instead.

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
  create-cam-syringe-bundle.sh   the packaging script (see its own header
                                  comment for the full runtime-dependency
                                  reasoning)
  README.md                      this file
  artifacts/                     gitignored -- where built bundles land
```

#!/bin/env bash
# Author: Aananth C N
#
# create-cam-syringe-bundle.sh
#
# Packages a built camsyringe binary together with its ENTIRE runtime
# dependency closure (Qt6, FFmpeg, X11/xcb, and everything THEY need in
# turn -- confirmed via `ldd`, not guessed) into a single self-extracting
# .bin installer, for a team member's Linux PC to install and run with
# ZERO extra steps -- no `apt install`, no C++ build toolchain, nothing
# beyond the one file. Same self-extracting-.bin technique
# qcarcam-injector's own release/create-qcarcam-inj-bundle.sh uses (header
# script + embedded tar payload after an __ARCHIVE_BELOW__ marker), for a
# consistent experience across both this project's and that one's release
# process.
#
# CHANGED from an earlier version of this script: that version bundled
# ONLY libVector_BLF (this project's own non-distro dependency) and had
# run-camsyringe.sh check for Qt6/FFmpeg via `dpkg -s`, printing an apt
# command if missing -- i.e. it still required the receiving machine to
# `apt install` a handful of packages first. Explicitly reversed: a
# bundle that still requires *any* install step on the receiving machine
# defeats the actual point of sharing a single file with a team whose
# machines may not have this source tree, or any dev tooling, at all.
# Every `.so` camsyringe (or Qt's xcb platform plugin, see below) actually
# needs is now bundled directly.
#
# What gets bundled, and why:
#   - build/camsyringe itself.
#   - The FULL transitive `ldd` closure of camsyringe AND of Qt's `xcb`
#     platform plugin (libqxcb.so -- REQUIRED to open a window at all;
#     Qt dlopen()s it at runtime based on QT_QPA_PLATFORM, so it does
#     NOT show up in camsyringe's own `ldd` output the way a normal link
#     dependency would, and confirmed via `ldd` against libqxcb.so
#     itself to pull in real additional libraries beyond camsyringe's own
#     closure: libQt6XcbQpa.so.6, a dozen libxcb-* extension libraries,
#     libX11-xcb/libSM/libICE, etc). Computed dynamically at BUNDLE-BUILD
#     time via collect_deps() below, not a hand-maintained static list --
#     confirmed the hard way that a static list goes stale the moment
#     Qt/FFmpeg/their transitive deps get updated on the build machine;
#     re-deriving it from the real binaries every time this script runs
#     is the only way to keep it honest.
#   - EXCLUDED from that closure: only the true kernel/glibc syscall-ABI
#     set (libc, libm, libdl, libpthread, librt, the ld.so dynamic linker
#     itself, linux-vdso which isn't a real file at all) -- see
#     EXCLUDE_SONAME_RE below. These are the one category of library
#     that's actually RISKY to bundle across different machines (glibc is
#     tightly coupled to the kernel's syscall ABI; a foreign libc/ld.so
#     can break things worse than not bundling at all) and safe to assume
#     present on literally any x86_64 Linux system -- unlike everything
#     else in the closure (Qt, FFmpeg's ~140 optional-codec transitive
#     libraries, X11/xcb, cairo/pango/fontconfig/freetype, gnutls/
#     openssl, systemd, even a stray /usr/local/cuda-*/lib64/libOpenCL.so
#     this particular build machine happens to have installed), none of
#     which is safe to assume present on a teammate's machine, hence
#     bundled.
#   - Qt's xcb platform plugin itself
#     (plugins/platforms/libqxcb.so) -- without it, Qt can't create any
#     window at all on a normal X11 desktop, regardless of how complete
#     the rest of the library closure is.
#
# Running the produced .bin:
#   - Installs into <install-dir>/bin/camsyringe, <install-dir>/lib/*.so*
#     (the full closure), <install-dir>/plugins/platforms/libqxcb.so, and
#     <install-dir>/run-camsyringe.sh (default install-dir
#     $HOME/camsyringe -- no root needed, unlike the QNX bundle's /aos).
#   - Removes any previous bin/lib/plugins under <install-dir> before
#     extracting -- confirmed live as a real, hard-to-diagnose bug: this
#     used to be additive (same convention as qcarcam-injector's bundle --
#     never rm -rf's <install-dir>, just creates bin/lib/plugins if
#     missing and extracts on top), which let a stale file from an OLDER
#     install survive a reinstall of a NEWER bundle (`tar x` overwrites a
#     path that's present in BOTH the old and new archive, but never
#     deletes one that's simply absent from the new one) -- a tester
#     re-running this exact installer kept reproducing an already-fixed
#     bug until a manual `rm -rf <install-dir>` first. Safe to wipe:
#     bin/lib/plugins are entirely this bundle's own content -- nothing
#     user-authored ever lives there (run-camsyringe.sh and
#     .install-source, the only other files under <install-dir>, are
#     themselves fully rewritten by every install too).
#   - run-camsyringe.sh is the entry point testers actually use: sets
#     LD_LIBRARY_PATH to the bundled lib/ (camsyringe's own embedded
#     RUNPATH points at this BUILD MACHINE's absolute build directory --
#     confirmed via readelf, useless once relocated -- LD_LIBRARY_PATH
#     takes priority over a DT_RUNPATH entry, so this reliably overrides
#     it) and QT_QPA_PLATFORM_PLUGIN_PATH to the bundled plugins/platforms
#     (so Qt finds the bundled libqxcb.so instead of looking for a system
#     Qt install that may not exist), then execs the real binary with
#     every argument forwarded. No package-manager check of any kind --
#     there is nothing left for the receiving machine to install.
#
# Usage:
#   ./create-cam-syringe-bundle.sh [options]
#
# Example:
#   ./create-cam-syringe-bundle.sh   # version comes from version.txt
#   scp artifacts/camsyringe_bundle_v0.5.bin test-pc@test-pc-ip:/tmp/
#   ssh test-pc@test-pc-ip '/tmp/camsyringe_bundle_v0.5.bin && ~/camsyringe/run-camsyringe.sh'
#
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build}"
OUTPUT_BIN=""
INSTALL_DIR_DEFAULT="\$HOME/camsyringe"
COMPRESS=0
SKIP_BUILD=0

# Single source of truth for CamSyringe's own version: version.txt at the
# repo root (also compiled into the binary itself, see CMakeLists.txt --
# CAMSYRINGE_VERSION, shown by Help > About). No version argument here
# anymore -- one file, one number, nothing to independently type (and
# nothing to drift out of sync with what the binary reports) on every
# release.
VERSION="$(cat "$PROJECT_DIR/version.txt")"

# Qt dlopen()s its platform plugin at runtime -- not a normal link
# dependency, so it must be located separately from camsyringe's own
# `ldd` output. Overridable in case a different machine's Qt6 packaging
# puts plugins somewhere else (confirmed via `find / -path '*qt6/plugins*'`
# if this default doesn't resolve).
QT_PLUGIN_DIR="${QT_PLUGIN_DIR:-/usr/lib/x86_64-linux-gnu/qt6/plugins}"

# The one category of library actually risky to bundle across different
# machines (glibc/ld.so are tightly coupled to the kernel's syscall ABI)
# and safe to assume present on any x86_64 Linux system -- see header
# comment. Everything else `ldd` reports gets bundled.
EXCLUDE_SONAME_RE='^(linux-vdso\.so|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|ld-linux.*\.so)'

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Packages the built camsyringe binary together with its ENTIRE runtime
dependency closure (Qt6, FFmpeg, X11/xcb, everything they need in turn --
computed via ldd, not a static list) into a self-extracting .bin
installer for a teammate's Linux PC. Zero extra install steps on the
receiving machine.

Version comes from version.txt at the repo root (v$VERSION right now) --
the same version compiled into the binary itself (Help > About). Baked
into the output filename (artifacts/camsyringe_bundle_v${VERSION}.bin,
unless --output overrides it). Bump version.txt to release a new version;
nothing here takes a version argument of its own.

Options:
  --build-dir PATH     Build directory to pull camsyringe/libVector_BLF from.
                        (default: $BUILD_DIR)
  --qt-plugin-dir PATH  Where to find Qt6's plugins/platforms/libqxcb.so.
                        (default: $QT_PLUGIN_DIR)
  --output PATH        Output path for the generated self-extracting .bin,
                        overriding the default versioned filename.
                        (default: release/artifacts/camsyringe_bundle_v${VERSION}.bin)
  --skip-build          Don't (re)build first -- package whatever's already
                        in --build-dir as-is. Default: runs a clean
                        cmake --build first, so the bundle always reflects
                        the current source tree.
  --compress            gzip the embedded payload (smaller .bin; needs
                        gunzip on the installing machine -- present on any
                        normal Ubuntu desktop, default: no compression).
  -h, --help            Show this help and exit.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --qt-plugin-dir) QT_PLUGIN_DIR="$2"; shift 2 ;;
        --output) OUTPUT_BIN="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --compress) COMPRESS=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage; exit 1 ;;
    esac
done

if [[ -z "$OUTPUT_BIN" ]]; then
    OUTPUT_BIN="$SCRIPT_DIR/artifacts/camsyringe_bundle_v${VERSION}.bin"
fi

if [[ $SKIP_BUILD -eq 0 ]]; then
    echo "Building camsyringe (cmake configure + build)..."
    mkdir -p "$BUILD_DIR"
    ( cd "$BUILD_DIR" && cmake .. >/dev/null && cmake --build . -j"$(nproc)" )
fi

CAMSYRINGE_BIN="$BUILD_DIR/camsyringe"
VECTOR_BLF_DIR="$BUILD_DIR/third_party/vector_blf/src/Vector/BLF"
QXCB_PLUGIN="$QT_PLUGIN_DIR/platforms/libqxcb.so"

if [[ ! -f "$CAMSYRINGE_BIN" ]]; then
    echo "ERROR: $CAMSYRINGE_BIN not found -- build it first (see README.md) or drop --skip-build." >&2
    exit 1
fi
if ! compgen -G "$VECTOR_BLF_DIR/libVector_BLF.so*" >/dev/null; then
    echo "ERROR: no libVector_BLF.so* found under $VECTOR_BLF_DIR -- build it first (see README.md) or drop --skip-build." >&2
    exit 1
fi
if [[ ! -f "$QXCB_PLUGIN" ]]; then
    echo "ERROR: Qt6 xcb platform plugin not found at $QXCB_PLUGIN -- override with --qt-plugin-dir (find it with: find / -path '*qt6/plugins/platforms/libqxcb.so' 2>/dev/null)." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. Stage the payload
# ---------------------------------------------------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/plugins/platforms"

echo "Staging camsyringe binary..."
install -m 0755 "$CAMSYRINGE_BIN" "$STAGE/bin/camsyringe"

echo "Staging libVector_BLF.so* (this project's own, with SONAME symlinks)..."
cp -a "$VECTOR_BLF_DIR"/libVector_BLF.so* "$STAGE/lib/"

echo "Staging Qt xcb platform plugin..."
install -m 0755 "$QXCB_PLUGIN" "$STAGE/plugins/platforms/libqxcb.so"

# Real dependency closure, computed now against the actual binaries --
# see header comment for why this is derived, not a static list. Skips
# anything already staged (libVector_BLF, self-references) and the
# kernel/glibc exclusion set.
echo "Resolving full runtime dependency closure (ldd)..."
declare -A STAGED
for f in "$STAGE/lib"/*; do
    STAGED["$(basename "$f")"]=1
done

collect_and_copy() {
    local bin="$1"
    local soname resolved
    while read -r soname resolved; do
        [[ -z "$soname" ]] && continue
        [[ "$soname" =~ $EXCLUDE_SONAME_RE ]] && continue
        [[ -n "${STAGED[$soname]:-}" ]] && continue
        if [[ -z "$resolved" || ! -e "$resolved" ]]; then
            echo "  WARNING: could not resolve '$soname' (needed by $(basename "$bin")) -- skipping, camsyringe may fail to start if this is actually required" >&2
            continue
        fi
        cp -L "$resolved" "$STAGE/lib/$soname"
        STAGED["$soname"]=1
    done < <(ldd "$bin" 2>/dev/null | awk '{print $1, $3}')
}

collect_and_copy "$CAMSYRINGE_BIN"
collect_and_copy "$QXCB_PLUGIN"
echo "  -> ${#STAGED[@]} shared librar$([[ ${#STAGED[@]} -eq 1 ]] && echo y || echo ies) staged (closure + libVector_BLF)"

echo "Staging run-camsyringe.sh..."
cat > "$STAGE/run-camsyringe.sh" <<'RUNNER_EOF'
#!/bin/sh
# Launcher for the camsyringe bundle -- points LD_LIBRARY_PATH at the
# bundled lib/ (camsyringe's own embedded RUNPATH points at the ORIGINAL
# build machine's absolute path, useless here -- LD_LIBRARY_PATH
# correctly overrides it) and QT_QPA_PLATFORM_PLUGIN_PATH at the bundled
# xcb plugin (Qt dlopen()s this at runtime; without pointing it here, Qt
# would look for a system Qt6 install that may not exist), then execs the
# real binary with every argument forwarded. No package-manager check --
# see release/create-cam-syringe-bundle.sh's own header comment for why
# this bundle needs nothing else installed.
#
# QT_QPA_PLATFORM is pinned to "xcb" -- the only platform plugin this
# bundle ships (see the dependency-closure reasoning in
# create-cam-syringe-bundle.sh's own header comment). Without this, Qt's
# platform autodetection picks "wayland" whenever WAYLAND_DISPLAY is set
# alongside DISPLAY -- true on any Wayland session, and always true under
# WSLg (release/windows/), which runs its own Wayland compositor plus
# XWayland and exports both variables. Qt then fails outright with
# "Could not find the Qt platform plugin 'wayland'" since only xcb is
# bundled, even though XWayland is right there and would have worked.
#
# Two extra checks below, both no-ops on a normal Linux desktop:
#   - Under WSL (detected via /proc/version), bail out with an actionable
#     message BEFORE launching if neither DISPLAY nor WAYLAND_DISPLAY is
#     set -- that means WSLg isn't active in this shell, camsyringe's
#     window has nowhere to go, and it would otherwise just hang or exit
#     with no visible output at all (exactly the "nothing happened"
#     failure mode this is here to prevent).
#   - If camsyringe exits non-zero for any reason, automatically re-run
#     it once with QT_DEBUG_PLUGINS=1 and show the tail of that output --
#     Qt's own platform-plugin-loading log, which pinpoints *why* no
#     window appeared far better than a silent failure would.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

export LD_LIBRARY_PATH="$SCRIPT_DIR/lib:${LD_LIBRARY_PATH:-}"
export QT_QPA_PLATFORM_PLUGIN_PATH="$SCRIPT_DIR/plugins/platforms"
export QT_QPA_PLATFORM="xcb"

if grep -qi microsoft /proc/version 2>/dev/null; then
    if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
        cat >&2 <<'EOF'
=================================================================
  WSLg doesn't look active in this shell -- both DISPLAY and
  WAYLAND_DISPLAY are unset. camsyringe's window has nowhere to
  display; it would hang or exit with no visible output.

  Try, in order:
    1. Close this WSL terminal and open a new one (WSLg sets these
       per-session; a stale/old session can be missing them).
    2. From Windows: wsl --shutdown, then reopen a WSL terminal.
    3. Confirm WSLg itself works, independent of camsyringe:
         sudo apt install -y x11-apps && xeyes
       If xeyes ALSO shows nothing, it's a WSLg/Windows problem,
       not camsyringe -- see release/windows/README.md.
=================================================================
EOF
        exit 1
    fi
fi

set +e
"$SCRIPT_DIR/bin/camsyringe" "$@"
STATUS=$?
set -e

if [ $STATUS -ne 0 ]; then
    echo "" >&2
    echo "camsyringe exited with status $STATUS. Re-running once with" >&2
    echo "QT_DEBUG_PLUGINS=1 to show why no window appeared:" >&2
    echo "" >&2
    QT_DEBUG_PLUGINS=1 "$SCRIPT_DIR/bin/camsyringe" "$@" 2>&1 | tail -n 40 >&2
fi

exit $STATUS
RUNNER_EOF
chmod +x "$STAGE/run-camsyringe.sh"

echo "-----------------------------------------------------------"
echo "Payload contents:"
( cd "$STAGE" && find . -maxdepth 2 | sort )
echo "-----------------------------------------------------------"

# ---------------------------------------------------------------------------
# 2. Build the payload archive
# ---------------------------------------------------------------------------
PAYLOAD="$(mktemp)"
trap 'rm -rf "$STAGE" "$PAYLOAD"' EXIT

echo "Building payload archive..."
if [[ $COMPRESS -eq 1 ]]; then
    tar czf "$PAYLOAD" -C "$STAGE" .
else
    tar cf "$PAYLOAD" -C "$STAGE" .
fi
echo "Payload size: $(du -h "$PAYLOAD" | cut -f1)"

# ---------------------------------------------------------------------------
# 3. Build the self-extracting .bin: header script + embedded payload
# ---------------------------------------------------------------------------
mkdir -p "$(dirname "$OUTPUT_BIN")"
echo "Building self-extracting installer: $OUTPUT_BIN"

HEADER="$(mktemp)"
trap 'rm -rf "$STAGE" "$PAYLOAD" "$HEADER"' EXIT

DECOMPRESS_CMD="cat"
[[ $COMPRESS -eq 1 ]] && DECOMPRESS_CMD="gunzip -dc"

BUNDLE_VERSION="$VERSION"

# Header is plain POSIX sh, same convention as qcarcam-injector's own
# installer -- not strictly required on a PC (bash is a safe assumption
# there), but keeping both projects' installers structurally identical is
# worth more than the (zero, in practice) bash-isms this one would
# actually use.
cat > "$HEADER" <<HEADER_EOF
#!/bin/sh
# Self-extracting camsyringe bundle installer (v${BUNDLE_VERSION}):
# camsyringe + its full runtime dependency closure (Qt6/FFmpeg/X11/xcb)
# + run-camsyringe.sh. Nothing else needs installing on this machine.
# Generated by release/create-cam-syringe-bundle.sh.
set -e

INSTALL_DIR="\${CAMSYRINGE_INSTALL_DIR:-${INSTALL_DIR_DEFAULT}}"

echo "camsyringe bundle installer (v${BUNDLE_VERSION})"
echo "Install directory: \$INSTALL_DIR"

# Wipe any previous install's bin/lib/plugins before extracting -- see
# this script's own header comment for the real, confirmed bug a plain
# additive install (tar x over old content, never deleting what the new
# archive doesn't have) caused. Nothing user-authored ever lives under
# these three -- entirely this bundle's own content, safe to remove.
rm -rf "\$INSTALL_DIR/bin" "\$INSTALL_DIR/lib" "\$INSTALL_DIR/plugins"
mkdir -p "\$INSTALL_DIR/bin" "\$INSTALL_DIR/lib" "\$INSTALL_DIR/plugins"

# Record where this .bin was actually run from -- CamSyringe's own
# "Install Injector" menu action (InjectorBundleFinder) reads this to
# default its file picker there instead of this install directory's own
# bin/ folder, since a sibling qcarcam_injector_bundle_vX.Y.bin is
# typically sitting wherever THIS .bin was handed over/downloaded to
# (see release/README.md's "Packaging" notes), not inside the install.
SOURCE_DIR="\$(cd "\$(dirname "\$0")" && pwd)"
echo "\$SOURCE_DIR" > "\$INSTALL_DIR/.install-source"

echo "Extracting bundle..."
ARCHIVE_LINE=\$(awk '/^__ARCHIVE_BELOW__\$/ { print NR + 1; exit 0 }' "\$0")
tail -n +\$ARCHIVE_LINE "\$0" | ${DECOMPRESS_CMD} | tar xf - -C "\$INSTALL_DIR"

echo "Installation complete."
echo ""
echo "Run with:"
echo "  \$INSTALL_DIR/run-camsyringe.sh"
echo "(fully self-contained -- nothing else needs installing)"

exit 0
__ARCHIVE_BELOW__
HEADER_EOF

cat "$HEADER" "$PAYLOAD" > "$OUTPUT_BIN"
chmod +x "$OUTPUT_BIN"

echo "-----------------------------------------------------------"
echo "Bundle created: $OUTPUT_BIN ($(du -h "$OUTPUT_BIN" | cut -f1))"
echo "-----------------------------------------------------------"
echo "To install on a teammate's PC (default install dir $INSTALL_DIR_DEFAULT, override with CAMSYRINGE_INSTALL_DIR=...):"
echo "  scp $OUTPUT_BIN test-pc@test-pc-ip:/tmp/"
echo "  ssh test-pc@test-pc-ip '/tmp/$(basename "$OUTPUT_BIN") && ~/camsyringe/run-camsyringe.sh'"

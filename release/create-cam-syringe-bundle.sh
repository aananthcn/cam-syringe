#!/bin/env bash
# Author: Aananth C N
#
# create-cam-syringe-bundle.sh
#
# Packages a built camsyringe binary together with its ONE non-standard
# runtime dependency (libVector_BLF, this project's own vendored BLF
# parser -- not a distro package) into a single self-extracting .bin
# installer, for a team member's Linux PC to install and run without
# needing the C++ build toolchain (Qt6/FFmpeg -dev headers, cmake, the
# vector_blf submodule) at all -- same self-extracting-.bin technique
# qcarcam-injector's own release/create-bundle.sh uses (header script +
# embedded tar payload after an __ARCHIVE_BELOW__ marker), for a
# consistent experience across both this project's and that one's
# release process.
#
# What gets bundled, and why:
#   - build/camsyringe itself.
#   - build/third_party/vector_blf/.../libVector_BLF.so* (all three --
#     the real file plus its SONAME and unversioned symlinks) -- this is
#     THIS project's own vendored/patched library (see
#     third_party/vector_blf's own commit fixing a real
#     heap-use-after-free race), not a distro package, so it's the one
#     thing that genuinely can't come from `apt install` on the
#     tester's own machine.
#
# Deliberately NOT bundled -- unlike qcarcam-injector's bundle (which
# bundles ffmpeg/SDL2 because there's no equivalent of "apt install" on a
# QNX target's fixed BSP image), Qt6/FFmpeg/X11 here are ALL standard
# Ubuntu packages any team PC either already has or can install with one
# apt command -- see RUNTIME_PACKAGES below, and the main README.md's own
# Prerequisites section (same packages, just -dev variants for building
# vs. plain runtime variants for just running). Bundling FFmpeg's full
# transitive dependency closure by hand (confirmed via `ldd` against the
# real built binary: over 100 shared libraries once every optional
# codec/muxer/protocol libavformat's Ubuntu package pulls in is counted --
# x264, x265, aom, dav1d, gnutls, zmq, rsvg, cairo, pango, and many more)
# would be both enormous and extremely fragile across even slightly
# different Ubuntu point releases -- `apt install` already solves exactly
# this problem correctly, so this script leans on it instead of
# reinventing it. run-camsyringe.sh (staged into the bundle, see below)
# checks for these packages at RUN time and prints the exact apt command
# if anything's missing, rather than a cryptic "error while loading
# shared libraries".
#
# Running the produced .bin:
#   - Installs into <install-dir>/bin/camsyringe,
#     <install-dir>/lib/libVector_BLF.so* , and
#     <install-dir>/run-camsyringe.sh (default install-dir
#     $HOME/camsyringe -- no root needed, unlike the QNX bundle's /aos).
#   - ADDITIVE, same convention as qcarcam-injector's bundle -- never
#     rm -rf's <install-dir>, just creates bin/lib if missing and
#     extracts on top.
#   - run-camsyringe.sh is the entry point testers actually use: checks
#     the runtime apt packages are present (warns + prints the install
#     command if not, doesn't silently fail), sets LD_LIBRARY_PATH so
#     the bundled libVector_BLF.so.2 is found (camsyringe's own embedded
#     RUNPATH points at this BUILD MACHINE's absolute build directory --
#     confirmed via readelf, useless once relocated -- LD_LIBRARY_PATH
#     takes priority over a DT_RUNPATH entry, so this reliably overrides
#     it), then execs the real binary with all args forwarded.
#
# Usage:
#   ./create-cam-syringe-bundle.sh <version-no> [options]
#
# Example:
#   ./create-cam-syringe-bundle.sh 0.5
#   scp artifacts/camsyringe_bundle_v0.5.bin teammate@pc:/tmp/
#   ssh teammate@pc '/tmp/camsyringe_bundle_v0.5.bin && ~/camsyringe/run-camsyringe.sh'
#
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build}"
VERSION=""
OUTPUT_BIN=""
INSTALL_DIR_DEFAULT="\$HOME/camsyringe"
COMPRESS=0
SKIP_BUILD=0
POSITIONAL=()

# Confirmed via `dpkg -S "$(realpath ...)"` against the real linked
# libraries on this build machine (Ubuntu 22.04/jammy) -- the runtime
# (not -dev) packages that own each shared library camsyringe actually
# links against directly (libavdevice/libavfilter/etc are FFmpeg's own
# transitive deps, pulled in automatically by these packages' own
# dependencies, not needed here explicitly). Package names/versions are
# distro-specific -- if a tester's PC is on a different Ubuntu release
# with different SONAMEs (e.g. libavformat59 instead of 58), update this
# list and re-verify with the same dpkg -S technique.
RUNTIME_PACKAGES=(libqt6widgets6 libqt6core6 libqt6gui6 libavformat58 libavcodec58 libavutil56 libswscale5)

usage() {
    cat <<EOF
Usage: $(basename "$0") <version-no> [options]

Packages the built camsyringe binary + libVector_BLF (this project's own
non-distro dependency) into a self-extracting .bin installer for a
teammate's Linux PC.

Arguments:
  <version-no>      Bundle version (e.g. 0.5) -- REQUIRED, always the
                     first argument. Baked into the output filename
                     (artifacts/camsyringe_bundle_v<version-no>.bin,
                     unless --output overrides it) and printed by the
                     installer/run script.

Options:
  --build-dir PATH  Build directory to pull camsyringe/libVector_BLF from.
                     (default: $BUILD_DIR)
  --output PATH     Output path for the generated self-extracting .bin,
                     overriding the default versioned filename.
                     (default: release/artifacts/camsyringe_bundle_v<version-no>.bin)
  --skip-build      Don't (re)build first -- package whatever's already
                     in --build-dir as-is. Default: runs a clean
                     cmake --build first, so the bundle always reflects
                     the current source tree.
  --compress        gzip the embedded payload (smaller .bin; needs
                     gunzip on the installing machine -- present on any
                     normal Ubuntu desktop, default: no compression).
  -h, --help        Show this help and exit.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --output) OUTPUT_BIN="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --compress) COMPRESS=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --*) echo "Unknown argument: $1" >&2; usage; exit 1 ;;
        *) POSITIONAL+=("$1"); shift ;;
    esac
done

if [[ ${#POSITIONAL[@]} -eq 0 ]]; then
    echo "ERROR: <version-no> is required as the first argument (e.g. '$(basename "$0") 0.5')." >&2
    usage
    exit 1
fi
VERSION="${POSITIONAL[0]}"

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

if [[ ! -f "$CAMSYRINGE_BIN" ]]; then
    echo "ERROR: $CAMSYRINGE_BIN not found -- build it first (see README.md) or drop --skip-build." >&2
    exit 1
fi
if ! compgen -G "$VECTOR_BLF_DIR/libVector_BLF.so*" >/dev/null; then
    echo "ERROR: no libVector_BLF.so* found under $VECTOR_BLF_DIR -- build it first (see README.md) or drop --skip-build." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. Stage the payload
# ---------------------------------------------------------------------------
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/bin" "$STAGE/lib"

echo "Staging camsyringe binary..."
install -m 0755 "$CAMSYRINGE_BIN" "$STAGE/bin/camsyringe"

echo "Staging libVector_BLF.so* (with SONAME symlinks)..."
cp -a "$VECTOR_BLF_DIR"/libVector_BLF.so* "$STAGE/lib/"

echo "Staging run-camsyringe.sh (dependency-checking launcher)..."
cat > "$STAGE/run-camsyringe.sh" <<'RUNNER_EOF'
#!/bin/sh
# Launcher for the camsyringe bundle -- checks the runtime apt packages
# this bundle deliberately does NOT vendor itself are present (see
# release/create-cam-syringe-bundle.sh's own header comment for why), points
# LD_LIBRARY_PATH at the bundled libVector_BLF (camsyringe's own embedded
# RUNPATH points at the ORIGINAL build machine's absolute path, useless
# here -- LD_LIBRARY_PATH correctly overrides it), then execs the real
# binary with every argument forwarded.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

RUNTIME_PACKAGES="libqt6widgets6 libqt6core6 libqt6gui6 libavformat58 libavcodec58 libavutil56 libswscale5"
MISSING=""
for pkg in $RUNTIME_PACKAGES; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        MISSING="$MISSING $pkg"
    fi
done
if [ -n "$MISSING" ]; then
    echo "camsyringe: missing runtime package(s):$MISSING" >&2
    echo "Install with:" >&2
    echo "  sudo apt-get install -y$MISSING" >&2
    echo "(if your distro's package names/versions differ, see release/create-cam-syringe-bundle.sh's" >&2
    echo " own RUNTIME_PACKAGES comment for how these were determined)" >&2
    exit 1
fi

export LD_LIBRARY_PATH="$SCRIPT_DIR/lib:${LD_LIBRARY_PATH:-}"
exec "$SCRIPT_DIR/bin/camsyringe" "$@"
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
# camsyringe + libVector_BLF + run-camsyringe.sh.
# Generated by release/create-cam-syringe-bundle.sh.
set -e

INSTALL_DIR="\${CAMSYRINGE_INSTALL_DIR:-${INSTALL_DIR_DEFAULT}}"

echo "camsyringe bundle installer (v${BUNDLE_VERSION})"
echo "Install directory: \$INSTALL_DIR"

# Additive install, same convention as qcarcam-injector's bundle -- never
# rm -rf's \$INSTALL_DIR itself, just ensures bin/lib exist and extracts
# on top of whatever's already there.
mkdir -p "\$INSTALL_DIR/bin" "\$INSTALL_DIR/lib"

echo "Extracting bundle..."
ARCHIVE_LINE=\$(awk '/^__ARCHIVE_BELOW__\$/ { print NR + 1; exit 0 }' "\$0")
tail -n +\$ARCHIVE_LINE "\$0" | ${DECOMPRESS_CMD} | tar xf - -C "\$INSTALL_DIR"

echo "Installation complete."
echo ""
echo "Run with:"
echo "  \$INSTALL_DIR/run-camsyringe.sh"
echo "(checks required Qt6/FFmpeg runtime packages are installed, then launches)"

exit 0
__ARCHIVE_BELOW__
HEADER_EOF

cat "$HEADER" "$PAYLOAD" > "$OUTPUT_BIN"
chmod +x "$OUTPUT_BIN"

echo "-----------------------------------------------------------"
echo "Bundle created: $OUTPUT_BIN ($(du -h "$OUTPUT_BIN" | cut -f1))"
echo "-----------------------------------------------------------"
echo "To install on a teammate's PC (default install dir $INSTALL_DIR_DEFAULT, override with CAMSYRINGE_INSTALL_DIR=...):"
echo "  scp $OUTPUT_BIN teammate@pc:/tmp/"
echo "  ssh teammate@pc '/tmp/$(basename "$OUTPUT_BIN") && ~/camsyringe/run-camsyringe.sh'"

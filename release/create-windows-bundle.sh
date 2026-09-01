#!/usr/bin/env bash
# Author: Aananth C N
#
# create-windows-bundle.sh
#
# Wraps the Windows-side pieces (release/windows/install-camsyringe.cmd,
# release/windows/setup-camsyringe-wsl.ps1) together with an already-built
# Linux camsyringe_bundle_vX.Y.bin (see create-cam-syringe-bundle.sh) into
# a single .zip -- the one artifact a Windows teammate needs: download,
# extract, double-click install-camsyringe.cmd. No separate script + .bin
# + README to hand over piecemeal, and setup-camsyringe-wsl.ps1
# auto-detects the .bin sitting next to it in the extracted folder, so no
# arguments either. Also picks up a qcarcam_injector_bundle_vX.Y.bin from
# artifacts/ if one's there (optional -- see below), for CamSyringe's own
# "Install Injector" feature.
#
# This does NOT build a Windows binary -- camsyringe still isn't a native
# Windows app (see release/windows/README.md for why: BLF/Ethernet replay
# needs AF_PACKET, a Linux-only socket family). What's zipped up here runs
# the exact same Linux bundle inside WSL2, via
# release/windows/setup-camsyringe-wsl.ps1.
#
# UNTESTED on a real Windows machine, same caveat as
# release/windows/setup-camsyringe-wsl.ps1 and its own README -- this
# script only assembles files, it doesn't change that.
#
# Usage:
#   ./create-windows-bundle.sh [options]
#
# Example:
#   ./create-cam-syringe-bundle.sh   # builds the Linux bundle first
#   ./create-windows-bundle.sh       # wraps it for Windows
#
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WINDOWS_DIR="$SCRIPT_DIR/windows"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Single source of truth for CamSyringe's own version: version.txt at the
# repo root (see CMakeLists.txt and create-cam-syringe-bundle.sh, which
# read the same file) -- no version argument here either, so this and the
# Linux bundle it wraps can never disagree about which version they are.
VERSION="$(cat "$PROJECT_DIR/version.txt")"
LINUX_BUNDLE=""
OUTPUT_ZIP=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Wraps the already-built Linux camsyringe bundle plus this project's WSL2
installer/launcher into a single .zip for a Windows teammate to download
and double-click. Build the Linux bundle first with
./create-cam-syringe-bundle.sh.

Version comes from version.txt at the repo root (v$VERSION right now) --
used to locate the default input bundle and to name the output .zip.

Options:
  --linux-bundle PATH   Path to the built camsyringe_bundle_vX.Y.bin.
                         (default: release/artifacts/camsyringe_bundle_v${VERSION}.bin)
  --output PATH         Output path for the generated .zip, overriding
                         the default versioned filename.
                         (default: release/artifacts/camsyringe_windows_bundle_v${VERSION}.zip)
  -h, --help            Show this help and exit.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --linux-bundle) LINUX_BUNDLE="$2"; shift 2 ;;
        --output) OUTPUT_ZIP="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage; exit 1 ;;
    esac
done

if [[ -z "$LINUX_BUNDLE" ]]; then
    LINUX_BUNDLE="$SCRIPT_DIR/artifacts/camsyringe_bundle_v${VERSION}.bin"
fi
if [[ -z "$OUTPUT_ZIP" ]]; then
    OUTPUT_ZIP="$SCRIPT_DIR/artifacts/camsyringe_windows_bundle_v${VERSION}.zip"
fi

if [[ ! -f "$LINUX_BUNDLE" ]]; then
    echo "ERROR: $LINUX_BUNDLE not found -- build it first: ./create-cam-syringe-bundle.sh" >&2
    exit 1
fi
if [[ ! -f "$WINDOWS_DIR/install-camsyringe.cmd" ]]; then
    echo "ERROR: missing $WINDOWS_DIR/install-camsyringe.cmd" >&2
    exit 1
fi
if [[ ! -f "$WINDOWS_DIR/setup-camsyringe-wsl.ps1" ]]; then
    echo "ERROR: missing $WINDOWS_DIR/setup-camsyringe-wsl.ps1" >&2
    exit 1
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp "$LINUX_BUNDLE" "$STAGE/"
cp "$WINDOWS_DIR/install-camsyringe.cmd" "$STAGE/"
cp "$WINDOWS_DIR/setup-camsyringe-wsl.ps1" "$STAGE/"

# Optional: the qcarcam-injector bundle (built separately, see
# ~/labs/qnx/qnx_toolkit/release/create-qcarcam-inj-bundle.sh), if a copy
# has been placed in this same artifacts/ dir alongside the Linux
# camsyringe bundle. Not required -- CamSyringe's own "Install Injector"
# menu action falls back to a file picker (and remembers wherever the
# user points it) when this isn't already sitting next to it -- but
# shipping it here means it Just Works for a teammate with zero extra
# setup, landing as a sibling of run-camsyringe.sh inside WSL (see
# setup-camsyringe-wsl.ps1).
INJECTOR_BUNDLES=("$SCRIPT_DIR"/artifacts/qcarcam_injector_bundle_v*.bin)
if [[ -e "${INJECTOR_BUNDLES[0]}" ]]; then
    cp "${INJECTOR_BUNDLES[@]}" "$STAGE/"
else
    echo "NOTE: no qcarcam_injector_bundle_v*.bin found in $SCRIPT_DIR/artifacts -- shipping without it (optional)." >&2
fi

mkdir -p "$(dirname "$OUTPUT_ZIP")"
rm -f "$OUTPUT_ZIP"
( cd "$STAGE" && zip -q -r "$OUTPUT_ZIP" . )

echo "-----------------------------------------------------------"
echo "Windows bundle created: $OUTPUT_ZIP ($(du -h "$OUTPUT_ZIP" | cut -f1))"
echo "-----------------------------------------------------------"
echo "Contents:"
unzip -l "$OUTPUT_ZIP"
echo ""
echo "Hand this single .zip to a Windows teammate. They extract it"
echo "anywhere and double-click install-camsyringe.cmd -- see"
echo "release/windows/README.md for what that does, including its"
echo "UNTESTED-on-real-hardware caveat."

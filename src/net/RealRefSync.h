#pragma once

#include <QString>

namespace camsyringe {

// Shell command that (re)populates the target's real-ref reference copy
// of libqcxclient.so (/var/opt/lib/real-ref/libqcxclient.so) from
// whichever board-native file is CURRENTLY the genuine real driver --
// /mnt/lib64/camera/libqcxclient.so.real if the SHIM/REAL toggle has
// already swapped it away, else /mnt/lib64/camera/libqcxclient.so itself
// (still genuine in that case). Safe to run anytime, unconditionally,
// idempotent -- only ever reads + copies, never renames or removes
// anything.
//
// MUST always be sourced from the board's own current file this way,
// NEVER from a copy shipped inside the injector bundle -- confirmed live
// that a build-machine-sourced copy (qcarcam-injector's ext/lib/, used at
// BUILD time only to satisfy the linker, per that project's own Makefile
// comment) can silently mismatch whatever board the bundle actually gets
// installed on: one bundle's real-ref needed libc.so.6/libc++.so.2,
// neither of which exist on a QNX 7.1 board, and even where dlopen did
// succeed against the board's own genuinely-matching file the API
// version mismatch still made QCarCamInitialize() reject with
// QCARCAM_RET_UNSUPPORTED. A build machine's ext/ snapshot can never be
// guaranteed to match whatever board a bundle ends up on, so a copy
// travelling that path is fundamentally unable to promise
// board-correctness the way copying straight from the board's own
// currently-genuine file can. See qcarcam-injector/ARCHITECTURE.md's
// real-ref item for the fuller story.
//
// Two callers, both needing this independently of each other:
//   - InjectorBundleInstaller, right after every install -- so
//     CameraGeometryResolver's own real-ref-dependent per-camera
//     resolution discovery (net/CameraGeometryResolver.cpp) works
//     regardless of whether the SHIM/REAL toggle has ever been used this
//     session at all.
//   - MainWindow's REAL->SHIM toggle -- so real-ref is refreshed from the
//     board's own file at the exact moment it's about to be swapped away
//     (matters if this is the first toggle since a fresh install, before
//     InjectorBundleInstaller's own sync above has had a board-side
//     REAL/SHIM state change to react to).
QString realRefSyncCommand();

}  // namespace camsyringe

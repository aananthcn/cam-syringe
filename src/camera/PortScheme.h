#pragma once

namespace camsyringe {

// Shared by main.cpp (initial CLI-provided cameras) and ui/MainWindow.cpp
// (cameras added/replaced via the Configure dialog) so both use
// the exact same port-assignment scheme, not independently duplicated
// magic numbers.
constexpr int kBasePort = 5004;
constexpr int kPortStep = 2;
constexpr int kMaxCameras = 4;

// The full QCarCam id range this platform's chip can address over GMSL2
// (its theoretical max -- the current board's actual wiring supports
// fewer, up to 12, but the chip itself can go to 16). Shared by
// CameraConfigDialog (a manually-typed camera id's valid spinbox range)
// and CameraSettingsDialog/MainWindow::onCameraSettingsTriggered() (the
// full set of ids "Settings > Camera" reads target configuration for,
// deliberately independent of whichever ids are actually chosen for
// injection in the current session -- see that dialog's own comment).
// QCarCam id range any camera may use (docs/adr/0016): 0 to 16, since it isn't
// known whether a board's developers count from 0 or 1. The single definition --
// every id check and id sweep derives from these two, never from a literal.
// 0 is a real id, never "unset". qcarcam-injector's own dispatcher still
// accepts 1-16 only; see ADR 0016's implementation status.
constexpr int kMinCamId = 0;
constexpr int kMaxCamId = 16;

} // namespace camsyringe

#pragma once

namespace camsyringe {

// Shared by main.cpp (initial CLI-provided cameras) and ui/MainWindow.cpp
// (cameras added/replaced via the Configure dialog) so both use
// the exact same port-assignment scheme, not independently duplicated
// magic numbers.
constexpr int kBasePort = 5004;
constexpr int kPortStep = 2;
constexpr int kMaxCameras = 4;

} // namespace camsyringe

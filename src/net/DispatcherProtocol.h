#pragma once

namespace camsyringe {

// Version of qcarcam_dispatcher's control protocol that THIS CamSyringe speaks
// (the CAM/FLAGS/END declaration, its replies, and the VERSION probe). Must
// equal the dispatcher's own kProtocolVersion (qcarcam-injector
// src/main_dispatcher.cpp). Bump both only when a change would make an older or
// newer peer misbehave; adding an optional line that peers ignore needs no
// bump. A mismatch blocks Play (docs/adr/0014).
constexpr int kDispatcherProtocolVersion = 1;

} // namespace camsyringe

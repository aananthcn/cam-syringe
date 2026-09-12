#pragma once

namespace camsyringe {

// Shared by main.cpp (CLI --target default) and ui/CameraConfigDialog.cpp
// (the "Force IPv4" checkbox's target-field rewrite) so both name the
// exact same two addresses, not independently duplicated literals.
//
// TEMPORARY workaround while this board's IPv6 socket creation is broken
// at the platform level (confirmed not a qcarcam_dispatcher/CamSyringe
// bug -- see main.cpp's own comment on kDefaultTarget). kDefaultTargetIPv4
// is 192.168.1.1, deliberately NOT this specific bench's own live IPv4
// address (192.168.1.4, set by /persist/net/early-net-iosock.sh) --
// 192.168.1.1 is the generic default IPv4 address across this board
// family (see qcarcam-injector's own emac_iosock_network.sh), while
// 192.168.1.4 is a one-off override specific to this one bench.
constexpr const char* kDefaultTargetIPv4 = "192.168.1.1";
constexpr const char* kDefaultTargetIPv6 = "fd53:7cb8:383:2::172";

} // namespace camsyringe

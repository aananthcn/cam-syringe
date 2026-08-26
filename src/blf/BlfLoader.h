#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace camsyringe {

// One captured Ethernet frame, ready to replay: a complete raw frame
// (destination MAC + source MAC + [802.1Q VLAN tag] + EtherType +
// payload -- exactly what a SOCK_RAW AF_PACKET send() expects on the
// wire) plus its capture-relative timestamp. timestampNs is relative to
// the EARLIEST Ethernet frame across the whole file (that frame reads
// 0), NOT wall-clock and NOT the BLF file's own internal time base --
// BlfReplayer adds its own shared Timeline origin at playback time,
// matching CameraStream's identical "deadlineNs = originNs + relativeNs"
// pattern (see BlfReplayer.h).
struct BlfEthernetFrame {
    int64_t timestampNs = 0;
    std::vector<uint8_t> rawFrame;
};

// Loads every Ethernet-frame object (the legacy ETHERNET_FRAME type and
// the newer ETHERNET_FRAME_EX type -- see BlfLoader.cpp for why both need
// separate handling) out of a BLF file, fully into memory, sorted by
// timestamp ascending -- see CONTEXT.md's "Jitter mitigation" section for
// why (no disk I/O on the replay hot path; vector_blf's own File class
// already decompresses/parses on a background thread while this drains
// it, so this is not simply "read the whole file into a buffer", it's a
// real decode pass). This intentionally returns a plain sorted
// std::vector, not literally a std::priority_queue as CONTEXT.md's
// earlier design note phrased it -- functionally identical for a
// fully-preloaded, strictly-drained-front-to-back workload (which this
// is; nothing is ever inserted after loading), and simpler/more
// cache-friendly than a heap for that access pattern.
//
// Every OTHER BLF object type (CAN/LIN/FlexRay/etc -- of which a real
// automotive capture usually has far more objects than Ethernet frames)
// is read and immediately discarded, not an oversight: Phase 3's own
// scope is raw Ethernet replay only. A future signal-monitor panel would
// read the same file again for CAN/LIN decoding, not extend this loader
// to also collect those.
//
// Returns an empty vector (logging the reason to stderr) if the file
// can't be opened or contains zero Ethernet-frame objects.
std::vector<BlfEthernetFrame> LoadBlfEthernetFrames(const std::string& path);

} // namespace camsyringe

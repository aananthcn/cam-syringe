#include "blf/BlfLoader.h"

#include <Vector/BLF/EthernetFrame.h>
#include <Vector/BLF/EthernetFrameEx.h>
#include <Vector/BLF/File.h>
#include <Vector/BLF/ObjectHeader.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>

namespace camsyringe {

namespace {

// ObjectHeader::objectTimeStamp's unit depends on objectFlags -- either
// multiples of 10 microseconds (TimeTenMics) or nanoseconds directly
// (TimeOneNans, also this field's own documented default and by far the
// more common flag in practice). Normalized to nanoseconds either way so
// every frame's timestampNs is directly comparable/subtractable
// regardless of which flag a given object happened to use.
int64_t NormalizeTimestampNs(const Vector::BLF::ObjectHeader& header) {
    if (header.objectFlags & Vector::BLF::ObjectHeader::ObjectFlags::TimeTenMics) {
        return static_cast<int64_t>(header.objectTimeStamp) * 10000;
    }
    return static_cast<int64_t>(header.objectTimeStamp);
}

// Writes `value` into `out` MSB-first (network/wire byte order) --
// deliberately NOT htons()+memcpy: htons() only matters when a uint16_t
// is later reinterpreted as raw memory elsewhere, not when manually
// appending individual bytes to a vector one at a time as this does.
// Correct regardless of this machine's own host endianness.
void PushBigEndian16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

// The legacy ETHERNET_FRAME object stores the Ethernet header fields
// (source/destination MAC, EtherType, VLAN tag) SEPARATELY from the
// payload -- unlike ETHERNET_FRAME_EX below, whose frameData already IS
// the complete captured frame. Reconstructs the actual wire bytes:
// dest(6) + src(6) + [802.1Q tag(4), only when tpid != 0] + EtherType(2)
// + payload. vector_blf's own read() does NOT byte-swap (the library
// only supports little-endian host machines, matching the file's own
// on-disk layout 1:1) -- so `type`/`tpid`/`tci` are correct HOST-native
// integer values (e.g. type == 0x0800 for IPv4) once read into memory,
// and still need converting to wire (big-endian) byte order here via
// PushBigEndian16(), same as building any other raw Ethernet frame by
// hand would.
std::vector<uint8_t> BuildRawFrame(const Vector::BLF::EthernetFrame& f) {
    std::vector<uint8_t> raw;
    raw.reserve(12 + (f.tpid != 0 ? 4 : 0) + 2 + f.payLoad.size());
    raw.insert(raw.end(), f.destinationAddress.begin(), f.destinationAddress.end());
    raw.insert(raw.end(), f.sourceAddress.begin(), f.sourceAddress.end());
    if (f.tpid != 0) {
        PushBigEndian16(raw, f.tpid);
        PushBigEndian16(raw, f.tci);
    }
    PushBigEndian16(raw, f.type);
    raw.insert(raw.end(), f.payLoad.begin(),
               f.payLoad.begin() + std::min<size_t>(f.payLoadLength, f.payLoad.size()));
    return raw;
}

} // namespace

std::vector<BlfEthernetFrame> LoadBlfEthernetFrames(const std::string& path) {
    std::vector<BlfEthernetFrame> frames;

    Vector::BLF::File file;
    try {
        file.open(path.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "BlfLoader: failed to open '%s': %s\n", path.c_str(), e.what());
        return frames;
    }
    if (!file.is_open()) {
        std::fprintf(stderr, "BlfLoader: failed to open '%s'\n", path.c_str());
        return frames;
    }

    size_t ethernetFrameCount = 0;
    size_t ethernetFrameExCount = 0;
    size_t otherObjectCount = 0;

    for (;;) {
        // Ownership transfers to us (per File::read()'s own documented
        // contract) -- wrapped immediately so every early-continue below
        // (the vast majority of objects, which aren't Ethernet frames at
        // all) can't leak. Looping on read() returning nullptr directly,
        // not file.eof(), avoids any ambiguity about exactly when the eof
        // flag becomes true relative to the last successful read.
        std::unique_ptr<Vector::BLF::ObjectHeaderBase> obj(file.read());
        if (!obj) {
            break;
        }

        switch (obj->objectType) {
            case Vector::BLF::ObjectType::ETHERNET_FRAME: {
                auto* f = static_cast<Vector::BLF::EthernetFrame*>(obj.get());
                BlfEthernetFrame frame;
                frame.timestampNs = NormalizeTimestampNs(*f);
                frame.rawFrame = BuildRawFrame(*f);
                frames.push_back(std::move(frame));
                ++ethernetFrameCount;
                break;
            }
            case Vector::BLF::ObjectType::ETHERNET_FRAME_EX: {
                auto* f = static_cast<Vector::BLF::EthernetFrameEx*>(obj.get());
                BlfEthernetFrame frame;
                frame.timestampNs = NormalizeTimestampNs(*f);
                // frameData already IS the complete captured frame
                // (header + payload) -- no reconstruction needed, unlike
                // the legacy type above.
                const size_t len = std::min<size_t>(f->frameLength, f->frameData.size());
                frame.rawFrame.assign(f->frameData.begin(), f->frameData.begin() + len);
                frames.push_back(std::move(frame));
                ++ethernetFrameExCount;
                break;
            }
            default:
                ++otherObjectCount;
                break;
        }
    }
    file.close();

    if (frames.empty()) {
        std::fprintf(stderr,
                      "BlfLoader: '%s' contains zero Ethernet-frame objects (%zu other objects "
                      "skipped -- CAN/LIN/FlexRay/etc, not this loader's scope)\n",
                      path.c_str(), otherObjectCount);
        return frames;
    }

    std::sort(frames.begin(), frames.end(),
              [](const BlfEthernetFrame& a, const BlfEthernetFrame& b) {
                  return a.timestampNs < b.timestampNs;
              });
    const int64_t firstTs = frames.front().timestampNs;
    for (auto& frame : frames) {
        frame.timestampNs -= firstTs; // normalize so the first frame reads 0
    }

    std::fprintf(stderr,
                  "BlfLoader: loaded %zu Ethernet frame(s) from '%s' (%zu legacy + %zu Ex, %zu "
                  "other objects skipped), spanning %.3fs\n",
                  frames.size(), path.c_str(), ethernetFrameCount, ethernetFrameExCount,
                  otherObjectCount, frames.back().timestampNs / 1e9);
    return frames;
}

} // namespace camsyringe

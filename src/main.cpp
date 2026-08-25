#include "camera/CameraStream.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
constexpr int kCameraPort = 5004;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.mp4> [user@]<host>\n", argv[0]);
        return EXIT_FAILURE;
    }

    std::string hostArg = argv[2];
    auto at = hostArg.find('@');
    std::string host = at == std::string::npos ? hostArg : hostArg.substr(at + 1);
    std::string destUrl = "rtp://" + host + ":" + std::to_string(kCameraPort);

    camsyringe::CameraStream stream(argv[1], destUrl);
    if (!stream.open()) {
        return EXIT_FAILURE;
    }

    std::fprintf(stderr,
                 "camsyringe: streaming '%s' -> %s (H.264/MPEG-TS-in-RTP, zerolatency, looping "
                 "-- Ctrl+C to stop)\n",
                 argv[1], destUrl.c_str());
    stream.run();
    std::fprintf(stderr, "camsyringe: done\n");
    return EXIT_SUCCESS;
}

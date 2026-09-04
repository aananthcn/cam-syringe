#include "camera/PortScheme.h"
#include "camera/StreamPool.h"
#include "net/TcpConnect.h"
#include "ui/MainWindow.h"

#include <QApplication>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char* kDefaultTarget = "192.168.1.1";
constexpr const char* kDefaultSshUser = "root";
constexpr int kDefaultControlPort = 5000;
constexpr int kMinCamId = 1;
constexpr int kMaxCamId = 16; // this target's allcamtest range, see qcarcam-injector/ARCHITECTURE.md
constexpr const char* kDefaultBlfInterface = "enp6s0";

void printUsage(const char* prog) {
    std::fprintf(
        stderr,
        "usage: %s [--target [user@]<target>] [--control-port N] [--ssh-key PATH] [--cam-ids IDS]\n"
        "       %*s[--playall] [--inject-only] [--blf-file PATH]\n"
        "       %*s[--blf-interface IFACE] [<video1> [<video2> [<video3> [<video4>]]]]\n"
        "  --target [USER@]TARGET  target host (default: %s), optionally prefixed with the SSH\n"
        "                     username CamSyringe uses for it (default: %s) -- also settable/\n"
        "                     changeable later from Configure's own \"SSH user\" field.\n"
        "  --control-port N   qcarcam_dispatcher's control-channel port on the target\n"
        "                     (default: %d)\n"
        "  --ssh-key PATH     SSH private key (-i) to use for that same target, instead of\n"
        "                     ssh's own default identity/agent -- also settable/changeable\n"
        "                     later from Configure's own \"SSH key\" field. Omit for the\n"
        "                     default (no explicit key).\n"
        "  --cam-ids IDS      QCarCam id for each video file, comma-separated, in the same\n"
        "                     order -- REQUIRED if any video files are given. A dash within\n"
        "                     one entry expands to an inclusive range (e.g. 1-3,8 means\n"
        "                     1,2,3,8). Each id must be 1-%d and unique.\n"
        "  --inject-only      Target-side: skip the local Screen/EGL preview render on the\n"
        "                     target entirely -- pure injection into qcxserver (see\n"
        "                     qcarcam_dispatcher's own --inject-only flag). On by default;\n"
        "                     uncheck it in Settings > CamSyringe to disable\n"
        "  --blf-file PATH    Vector BLF file to replay (Ethernet-frame objects only, see\n"
        "                     src/blf/BlfLoader.h) as raw AF_PACKET frames, original timing,\n"
        "                     verbatim (no header rewriting). Needs CAP_NET_RAW -- see\n"
        "                     README.md. Requires --blf-interface too.\n"
        "  --blf-interface IFACE  Network interface to replay onto (default: %s)\n"
        "  --playall          start streaming immediately (requires at least one video file)\n"
        "  With no arguments at all, opens the camera configuration dialog on launch.\n",
        prog, static_cast<int>(std::string("usage: ").size() + std::string(prog).size() + 1), "",
        static_cast<int>(std::string("usage: ").size() + std::string(prog).size() + 1), "",
        kDefaultTarget, kDefaultSshUser, kDefaultControlPort, kMaxCamId, kDefaultBlfInterface);
}

// Comma-separated QCarCam ids, e.g. "8,9,1,2" -- a dash within one entry
// expands to an inclusive range, e.g. "1-3,8" -> {1,2,3,8}. Comma (not
// dash) is the entry separator specifically because a dash by itself is
// ambiguous the moment ids go double-digit or a range is wanted (e.g.
// "1-2-8-9" can't tell a delimiter dash from a range dash apart) -- see
// qcarcam-injector's own CAM/FLAGS line protocol for the same reasoning.
bool parseCamIds(const std::string& spec, std::vector<int>& outIds, std::string& error) {
    outIds.clear();
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string token =
            spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? spec.size() : comma + 1;
        if (token.empty()) {
            error = "empty entry in --cam-ids";
            return false;
        }
        size_t dash = token.find('-');
        if (dash != std::string::npos && dash > 0 && dash + 1 < token.size()) {
            int a = std::atoi(token.substr(0, dash).c_str());
            int b = std::atoi(token.substr(dash + 1).c_str());
            if (a <= 0 || b <= 0 || a > b) {
                error = "invalid range '" + token + "' in --cam-ids";
                return false;
            }
            for (int v = a; v <= b; ++v) {
                outIds.push_back(v);
            }
        } else {
            int v = std::atoi(token.c_str());
            if (v <= 0) {
                error = "invalid id '" + token + "' in --cam-ids";
                return false;
            }
            outIds.push_back(v);
        }
    }
    if (outIds.empty()) {
        error = "--cam-ids must not be empty";
        return false;
    }
    std::set<int> seen;
    for (int id : outIds) {
        if (id < kMinCamId || id > kMaxCamId) {
            error = "camera id " + std::to_string(id) + " out of range (" +
                     std::to_string(kMinCamId) + "-" + std::to_string(kMaxCamId) + ")";
            return false;
        }
        if (!seen.insert(id).second) {
            error = "duplicate camera id " + std::to_string(id) + " in --cam-ids";
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string targetArg;
    int controlPort = kDefaultControlPort;
    std::string camIdsArg;
    bool playAll = false;
    bool injectOnly = true;
    std::string sshKeyPath;
    std::string blfFile;
    std::string blfInterface = kDefaultBlfInterface;
    std::vector<std::string> videoFiles;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--target") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return EXIT_FAILURE;
            }
            targetArg = argv[++i];
        } else if (arg.rfind("--target=", 0) == 0) {
            targetArg = arg.substr(9);
        } else if (arg == "--control-port") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return EXIT_FAILURE;
            }
            controlPort = std::atoi(argv[++i]);
        } else if (arg == "--cam-ids") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return EXIT_FAILURE;
            }
            camIdsArg = argv[++i];
        } else if (arg == "--playall") {
            playAll = true;
        } else if (arg == "--inject-only") {
            injectOnly = true;
        } else if (arg == "--ssh-key") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return EXIT_FAILURE;
            }
            sshKeyPath = argv[++i];
        } else if (arg == "--blf-file") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return EXIT_FAILURE;
            }
            blfFile = argv[++i];
        } else if (arg == "--blf-interface") {
            if (i + 1 >= argc) {
                printUsage(argv[0]);
                return EXIT_FAILURE;
            }
            blfInterface = argv[++i];
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            printUsage(argv[0]);
            return EXIT_FAILURE;
        } else {
            if (static_cast<int>(videoFiles.size()) >= camsyringe::kMaxCameras) {
                std::fprintf(stderr, "too many video files (max %d)\n", camsyringe::kMaxCameras);
                return EXIT_FAILURE;
            }
            videoFiles.push_back(arg);
        }
    }

    if (playAll && videoFiles.empty()) {
        std::fprintf(stderr, "--playall requires at least one video file\n");
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    std::vector<int> camIds;
    if (!videoFiles.empty()) {
        if (camIdsArg.empty()) {
            std::fprintf(stderr, "--cam-ids is required when video files are given\n");
            printUsage(argv[0]);
            return EXIT_FAILURE;
        }
        std::string error;
        if (!parseCamIds(camIdsArg, camIds, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return EXIT_FAILURE;
        }
        if (camIds.size() != videoFiles.size()) {
            std::fprintf(stderr, "--cam-ids gave %zu id(s) but %zu video file(s) were given\n",
                          camIds.size(), videoFiles.size());
            return EXIT_FAILURE;
        }
    }

    std::string target = targetArg.empty() ? kDefaultTarget : targetArg;
    std::string sshUser = kDefaultSshUser;
    // `--target user@host` -- the "user@" part used to be silently
    // discarded here (parsed by usage text, never actually used anywhere)
    // until Configure gained its own SSH User field; now both feed the
    // same MainWindow::sshUser_, so this CLI form and Configure agree.
    auto at = target.find('@');
    if (at != std::string::npos) {
        sshUser = target.substr(0, at);
        target = target.substr(at + 1);
    }

    QApplication app(argc, argv);

    camsyringe::StreamPool pool;
    for (size_t i = 0; i < videoFiles.size(); ++i) {
        int port = camsyringe::kBasePort + static_cast<int>(i) * camsyringe::kPortStep;
        camsyringe::CameraConfig cfg;
        cfg.inputPath = videoFiles[i];
        cfg.destUrl =
            "rtp://" + camsyringe::bracketHostIfIPv6(target) + ":" + std::to_string(port);
        cfg.label = "cam" + std::to_string(i);
        cfg.index = static_cast<int>(i);
        cfg.camId = camIds[i];
        cfg.port = port;
        pool.addCamera(std::move(cfg));
    }

    camsyringe::ui::MainWindow window(&pool, QString::fromStdString(target), controlPort,
                                       QString::fromStdString(sshUser),
                                       QString::fromStdString(sshKeyPath), injectOnly,
                                       QString::fromStdString(blfFile),
                                       QString::fromStdString(blfInterface), playAll);
    window.resize(1280, 720);
    window.show();

    return app.exec();
}

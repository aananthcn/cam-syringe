#pragma once

#include <string>
#include <vector>

#include "util/SessionConfigStore.h"

namespace camsyringe {

class StreamPool;

// What the command line said. Only the options the user actually passed count as "given": a given option
// overrides the remembered Camera Injection configuration for that launch (docs/adr/0013).
struct LaunchInput {
    std::string target;               // as typed: "host" or "user@host"; empty if not given
    int controlPort = 0;
    bool controlPortGiven = false;
    bool injectOnlyGiven = false;     // --inject-only (it can only turn the option ON)
    std::string sshKey;
    bool sshKeyGiven = false;
    std::string blfFile;
    bool blfFileGiven = false;
    std::string blfInterface;
    bool blfInterfaceGiven = false;
    std::vector<std::string> videoFiles;
    bool camIdsGiven = false;         // --cam-ids was passed (its parsing stays with the caller)
};

// Built-in defaults, used where neither the command line nor the saved configuration says anything.
struct LaunchDefaults {
    std::string target;
    std::string sshUser;
    int controlPort = 0;
    std::string blfInterface;
};

struct LaunchConfig {
    std::string target;               // host only: any "user@" has been split into sshUser
    std::string sshUser;
    int controlPort = 0;
    bool injectOnly = true;
    std::string sshKeyPath;
    std::string blfFile;
    std::string blfInterface;
    std::vector<std::string> videoFiles;
    // Filled only when the cameras came from the saved configuration; otherwise the caller parses
    // --cam-ids for videoFiles.
    std::vector<int> camIds;
    bool camerasFromSaved = false;
};

// Combines the command line with the saved configuration, option by option: command line, then saved,
// then default. Saved cameras are used only when the command line gave no video files and no --cam-ids
// (a partial mix would misalign files and ids). The saved SSH user and key belong to the saved target, so
// they are reused only when this launch targets the same machine or names none. `saved` may be invalid
// (no file), in which case this is just the command line over the defaults.
LaunchConfig mergeLaunchConfig(const LaunchInput& cli, const SessionConfig& saved, const LaunchDefaults& defaults);

// Adds one camera per video file to `pool`, the way a launch does (ports from PortScheme, RTP destination
// from the target). camIds must be parallel to config.videoFiles.
void populatePool(StreamPool& pool, const LaunchConfig& config, const std::vector<int>& camIds);

} // namespace camsyringe

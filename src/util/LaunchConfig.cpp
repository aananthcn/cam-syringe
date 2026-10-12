#include "util/LaunchConfig.h"

#include <QString>

#include "camera/PortScheme.h"
#include "camera/StreamPool.h"
#include "net/TcpConnect.h"

namespace camsyringe {

LaunchConfig mergeLaunchConfig(const LaunchInput& cli, const SessionConfig& saved, const LaunchDefaults& defaults)
{
    LaunchConfig out;
    out.controlPort = cli.controlPortGiven ? cli.controlPort : defaults.controlPort;
    out.injectOnly = true;
    out.sshKeyPath = cli.sshKeyGiven ? cli.sshKey : std::string();
    out.blfFile = cli.blfFileGiven ? cli.blfFile : std::string();
    out.blfInterface = cli.blfInterfaceGiven ? cli.blfInterface : defaults.blfInterface;
    out.videoFiles = cli.videoFiles;

    std::string targetArg = cli.target;
    bool sameTargetAsSaved = false;
    if (saved.valid) {
        std::string cliHost = targetArg;
        const auto cliAt = cliHost.find('@');
        if (cliAt != std::string::npos) cliHost = cliHost.substr(cliAt + 1);
        sameTargetAsSaved = cliHost.empty() || QString::fromStdString(cliHost) == saved.target;

        if (targetArg.empty() && !saved.target.isEmpty()) targetArg = saved.target.toStdString();
        if (!cli.controlPortGiven && saved.controlPort > 0) out.controlPort = saved.controlPort;
        if (!cli.injectOnlyGiven) out.injectOnly = saved.injectOnly;
        if (!cli.sshKeyGiven && sameTargetAsSaved) out.sshKeyPath = saved.sshKeyPath.toStdString();
        if (!cli.blfFileGiven) out.blfFile = saved.blfPath.toStdString();
        if (!cli.blfInterfaceGiven && !saved.blfInterface.isEmpty()) out.blfInterface = saved.blfInterface.toStdString();

        if (cli.videoFiles.empty() && !cli.camIdsGiven && !saved.videoFiles.isEmpty() &&
            saved.videoFiles.size() == static_cast<int>(saved.camIds.size())) {
            for (const QString& f : saved.videoFiles) out.videoFiles.push_back(f.toStdString());
            out.camIds = saved.camIds;
            out.camerasFromSaved = true;
        }
    }

    out.target = targetArg.empty() ? defaults.target : targetArg;
    out.sshUser = defaults.sshUser;
    if (saved.valid && sameTargetAsSaved && !saved.sshUser.isEmpty()) out.sshUser = saved.sshUser.toStdString();
    // `--target user@host`: the user part wins over everything else for the SSH user.
    const auto at = out.target.find('@');
    if (at != std::string::npos) {
        out.sshUser = out.target.substr(0, at);
        out.target = out.target.substr(at + 1);
    }
    return out;
}

void populatePool(StreamPool& pool, const LaunchConfig& config, const std::vector<int>& camIds)
{
    for (size_t i = 0; i < config.videoFiles.size(); ++i) {
        const int port = kBasePort + static_cast<int>(i) * kPortStep;
        CameraConfig cfg;
        cfg.inputPath = config.videoFiles[i];
        cfg.destUrl = "rtp://" + bracketHostIfIPv6(config.target) + ":" + std::to_string(port);
        cfg.label = "cam" + std::to_string(i);
        cfg.index = static_cast<int>(i);
        cfg.camId = camIds[i];
        cfg.port = port;
        pool.addCamera(std::move(cfg));
    }
}

} // namespace camsyringe

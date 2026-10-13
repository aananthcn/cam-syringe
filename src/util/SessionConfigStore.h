#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace camsyringe {

// The "Settings > Camera Injection" dialog's configuration, as remembered
// across restarts (docs/adr/0013). Never holds a password or any credential
// typed into the credentials prompt, and never playback state.
struct SessionConfig {
    // False when there is no usable saved file (first launch, deleted, or
    // unparsable) -- every other field is then meaningless.
    bool valid = false;
    QString target;
    int controlPort = 0;
    QString sshUser;
    QString sshKeyPath;
    // Parallel and equal in length, 0..kMaxCameras entries -- load() drops
    // both together if they would disagree or hold an out-of-range id.
    QStringList videoFiles;
    std::vector<int> camIds;
    bool injectOnly = true;
    QString blfPath;
    QString blfInterface;
};

// Persists SessionConfig to a plain JSON file next to the running binary
// (see ConfigFiles for where: the install directory and ~/.cache), beside
// CameraConfigStore's camera_configs.json: readable and editable by hand,
// best effort. A write failure (e.g. a read-only install directory) is
// logged to stderr, never surfaced as a user-facing error, and never
// blocks Accept or Play -- this is a convenience cache, not a dependency.
class SessionConfigStore {
public:
    static QString filePath();  // where it is written first (ConfigFiles::primaryPath)

    // Writes `config` to every location ConfigFiles offers (the install directory and ~/.cache), so the
    // remembered configuration survives a reinstall or an upgrade (docs/adr/0013). The file carries a
    // schema version, and any keys it already holds that this release doesn't know are kept, so a newer
    // release's settings aren't lost when an older one saves. Returns false only if NO location took it;
    // `written`/`errors` are optional and say where it went and what failed.
    static bool save(const SessionConfig& config, QStringList* written = nullptr,
                     QStringList* errors = nullptr);

    // Returns a config with valid == false if no copy exists or none parses. Reads the newest copy and
    // falls back to older ones; missing fields take their defaults and unknown ones are ignored, so a file
    // written by an older or newer release loads. A saved video path that no longer exists is kept as-is, so
    // the user sees what was configured.
    static SessionConfig load();
};

} // namespace camsyringe

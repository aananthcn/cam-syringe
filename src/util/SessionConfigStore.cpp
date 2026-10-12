#include "util/SessionConfigStore.h"

#include <algorithm>
#include <cstdio>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "camera/PortScheme.h"
#include "util/ConfigFiles.h"

namespace camsyringe {

namespace {
constexpr int kSchemaVersion = 1;  // bump only for a change an older release can't read around
constexpr const char* kFileName = "camera_injection.json";
}  // namespace

QString SessionConfigStore::filePath() { return ConfigFiles::primaryPath(kFileName); }

bool SessionConfigStore::save(const SessionConfig& config, QStringList* written, QStringList* errors) {
    // Start from what is already there, so keys a newer release added survive an older one saving.
    QJsonObject root;
    for (const QByteArray& existing : ConfigFiles::readAll(kFileName)) {
        const QJsonDocument doc = QJsonDocument::fromJson(existing);
        if (doc.isObject()) {
            root = doc.object();
            break;
        }
    }
    // Never lower a version a newer release wrote: it may rely on it to read its own keys.
    root["schemaVersion"] = std::max(kSchemaVersion, root.value("schemaVersion").toInt(0));
    root["target"] = config.target;
    root["controlPort"] = config.controlPort;
    root["sshUser"] = config.sshUser;
    root["sshKeyPath"] = config.sshKeyPath;
    root["injectOnly"] = config.injectOnly;
    root["blfPath"] = config.blfPath;
    root["blfInterface"] = config.blfInterface;

    QJsonArray cameras;
    for (int i = 0; i < config.videoFiles.size() && i < static_cast<int>(config.camIds.size()); ++i) {
        QJsonObject cam;
        cam["videoFile"] = config.videoFiles[i];
        cam["camId"] = config.camIds[static_cast<size_t>(i)];
        cameras.append(cam);
    }
    root["cameras"] = cameras;

    QStringList w, e;
    const bool ok = ConfigFiles::write(kFileName, QJsonDocument(root).toJson(QJsonDocument::Indented), &w, &e);
    for (const QString& line : e) {
        std::fprintf(stderr, "SessionConfigStore: couldn't write %s\n", line.toUtf8().constData());
    }
    if (written) *written = w;
    if (errors) *errors = e;
    return ok;
}

SessionConfig SessionConfigStore::load() {
    SessionConfig config;

    // Newest copy first; if it doesn't parse, an older copy beats nothing.
    QJsonObject root;
    QStringList paths;
    const QList<QByteArray> copies = ConfigFiles::readAll(kFileName, &paths);
    bool parsed = false;
    for (int i = 0; i < copies.size() && !parsed; ++i) {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(copies[i], &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            root = doc.object();
            parsed = true;
            std::fprintf(stderr, "SessionConfigStore: loaded %s\n", paths[i].toUtf8().constData());
        } else {
            std::fprintf(stderr, "SessionConfigStore: couldn't parse %s: %s\n", paths[i].toUtf8().constData(),
                         parseError.errorString().toUtf8().constData());
        }
    }
    if (!parsed) {
        return config;  // no usable copy -- first launch, or everything was lost
    }

    config.target = root.value("target").toString();
    config.controlPort = root.value("controlPort").toInt();
    // Out-of-range port: treat the field as unset, not the whole file as bad.
    if (config.controlPort < 1 || config.controlPort > 65535) {
        config.controlPort = 0;
    }
    config.sshUser = root.value("sshUser").toString();
    config.sshKeyPath = root.value("sshKeyPath").toString();
    config.injectOnly = root.value("injectOnly").toBool(true);
    config.blfPath = root.value("blfPath").toString();
    config.blfInterface = root.value("blfInterface").toString();

    // Cameras are all-or-nothing: if any entry is unusable, the whole list is
    // dropped rather than loading a half-configured, misaligned set.
    const QJsonArray cameras = root.value("cameras").toArray();
    bool camerasOk = cameras.size() <= kMaxCameras;
    QStringList files;
    std::vector<int> ids;
    for (const QJsonValue& v : cameras) {
        const QJsonObject cam = v.toObject();
        const QString videoFile = cam.value("videoFile").toString();
        const int camId = cam.value("camId").toInt();
        if (camId < kMinCamId || camId > kMaxCamId) {
            camerasOk = false;
            break;
        }
        files << videoFile;
        ids.push_back(camId);
    }
    if (camerasOk) {
        config.videoFiles = files;
        config.camIds = ids;
    }

    config.valid = true;
    return config;
}

} // namespace camsyringe

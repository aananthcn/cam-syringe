#include "util/CameraConfigStore.h"

#include "util/ConfigFiles.h"

#include <algorithm>
#include <cstdio>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace camsyringe {

namespace {
constexpr int kSchemaVersion = 1;
constexpr const char* kFileName = "camera_configs.json";
}  // namespace

QString CameraConfigStore::filePath() { return ConfigFiles::primaryPath(kFileName); }

void CameraConfigStore::save(const QMap<QString, QMap<int, ResolvedCameraGeometry>>& cache,
                              const QMap<QString, QDateTime>& readTimestamps) {
    QJsonObject targets;
    for (auto targetIt = cache.constBegin(); targetIt != cache.constEnd(); ++targetIt) {
        const QString& target = targetIt.key();
        QJsonArray cameras;
        for (const ResolvedCameraGeometry& r : targetIt.value()) {
            QJsonObject cam;
            cam["camId"] = r.camId;
            cam["ok"] = r.ok;
            cam["wasFallback"] = r.wasFallback;
            cam["width"] = static_cast<qint64>(r.width);
            cam["height"] = static_cast<qint64>(r.height);
            cameras.append(cam);
        }
        QJsonObject targetObj;
        targetObj["cameras"] = cameras;
        const QDateTime when = readTimestamps.value(target);
        if (when.isValid()) {
            targetObj["lastRead"] = when.toString(Qt::ISODate);
        }
        targets[target] = targetObj;
    }
    // Keep any keys another release wrote that this one doesn't know about (see ConfigFiles).
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
    root["targets"] = targets;

    QStringList errors;
    if (!ConfigFiles::write(kFileName, QJsonDocument(root).toJson(QJsonDocument::Indented), nullptr, &errors)) {
        for (const QString& line : errors) {
            std::fprintf(stderr, "CameraConfigStore: couldn't write %s\n", line.toUtf8().constData());
        }
    }
}

void CameraConfigStore::load(QMap<QString, QMap<int, ResolvedCameraGeometry>>* cache,
                              QMap<QString, QDateTime>* readTimestamps) {
    cache->clear();
    readTimestamps->clear();

    QJsonObject root;
    bool parsed = false;
    for (const QByteArray& copy : ConfigFiles::readAll(kFileName)) {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(copy, &parseError);
        if (parseError.error == QJsonParseError::NoError && doc.isObject()) {
            root = doc.object();
            parsed = true;
            break;
        }
    }
    if (!parsed) {
        return;  // no usable copy yet -- first launch, or nothing has ever been read
    }

    const QJsonObject targets = root.value("targets").toObject();
    for (auto it = targets.constBegin(); it != targets.constEnd(); ++it) {
        const QString target = it.key();
        const QJsonObject targetObj = it.value().toObject();

        const QString lastRead = targetObj.value("lastRead").toString();
        if (!lastRead.isEmpty()) {
            const QDateTime when = QDateTime::fromString(lastRead, Qt::ISODate);
            if (when.isValid()) {
                (*readTimestamps)[target] = when;
            }
        }

        QMap<int, ResolvedCameraGeometry> perCam;
        for (const QJsonValue& v : targetObj.value("cameras").toArray()) {
            const QJsonObject cam = v.toObject();
            ResolvedCameraGeometry r;
            r.camId = cam.value("camId").toInt();
            r.ok = cam.value("ok").toBool();
            r.wasFallback = cam.value("wasFallback").toBool();
            r.width = static_cast<uint32_t>(cam.value("width").toInt());
            r.height = static_cast<uint32_t>(cam.value("height").toInt());
            perCam[r.camId] = r;
        }
        (*cache)[target] = perCam;
    }
}

} // namespace camsyringe

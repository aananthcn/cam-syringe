#include "util/CameraConfigStore.h"

#include <cstdio>

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace camsyringe {

QString CameraConfigStore::filePath() {
    return QCoreApplication::applicationDirPath() + "/camera_configs.json";
}

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
    QJsonObject root;
    root["targets"] = targets;

    const QString path = filePath();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "CameraConfigStore: couldn't write %s: %s\n", path.toUtf8().constData(),
                     file.errorString().toUtf8().constData());
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void CameraConfigStore::load(QMap<QString, QMap<int, ResolvedCameraGeometry>>* cache,
                              QMap<QString, QDateTime>* readTimestamps) {
    cache->clear();
    readTimestamps->clear();

    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return; // no file yet -- first launch, or nothing has ever been read
    }
    QJsonParseError parseError{};
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        std::fprintf(stderr, "CameraConfigStore: couldn't parse %s: %s\n",
                     filePath().toUtf8().constData(), parseError.errorString().toUtf8().constData());
        return;
    }

    const QJsonObject targets = doc.object().value("targets").toObject();
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

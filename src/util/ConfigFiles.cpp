#include "util/ConfigFiles.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace camsyringe {

namespace {

// The directory the running binary lives in. QCoreApplication::applicationDirPath() is only valid once the
// application object exists (before that it warns and returns nothing), and the remembered configuration is
// needed BEFORE it does -- main() reads it to seed the camera pool -- so on Linux ask the kernel instead.
QString executableDir() {
    if (QCoreApplication::instance() != nullptr) {
        return QCoreApplication::applicationDirPath();
    }
#ifdef Q_OS_LINUX
    const QString exe = QFile::symLinkTarget(QStringLiteral("/proc/self/exe"));
    if (!exe.isEmpty()) return QFileInfo(exe).absolutePath();
#endif
    return QString();
}

// ~/.cache/camsyringe, spelled out rather than taken from QStandardPaths::CacheLocation, whose last path
// component comes from the application name -- which is empty until the application object exists.
QString cacheDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/camsyringe");
}

}  // namespace

QStringList ConfigFiles::candidateDirs() {
    const QByteArray override = qgetenv("CAMSYRINGE_CONFIG_DIRS");
    if (!override.isEmpty()) {
        QStringList dirs;
        for (const QString& d : QString::fromLocal8Bit(override).split(':', Qt::SkipEmptyParts)) {
            dirs << QDir::cleanPath(d);
        }
        return dirs;
    }

    QStringList dirs;
    const QString appDir = executableDir();
    if (!appDir.isEmpty()) {
        // An installed copy: <install-dir>/bin/camsyringe with run-camsyringe.sh in <install-dir>. The
        // installer rewrites bin/, lib/ and plugins/ but not the install directory itself.
        const QString installDir = QDir::cleanPath(appDir + "/..");
        if (QFileInfo::exists(installDir + "/run-camsyringe.sh")) {
            dirs << installDir;
        } else {
            dirs << appDir;  // a development build, or any layout we don't recognise
        }
    }
    const QString cache = cacheDir();
    if (!cache.isEmpty() && !dirs.contains(cache)) {
        dirs << cache;
    }
    return dirs;
}

QStringList ConfigFiles::readDirs() {
    QStringList dirs = candidateDirs();
    if (qgetenv("CAMSYRINGE_CONFIG_DIRS").isEmpty()) {
        const QString appDir = QDir::cleanPath(executableDir());
        if (!appDir.isEmpty() && !dirs.contains(appDir)) dirs << appDir;
    }
    return dirs;
}

QString ConfigFiles::primaryPath(const QString& name) {
    const QStringList dirs = candidateDirs();
    return dirs.isEmpty() ? name : dirs.first() + "/" + name;
}

bool ConfigFiles::write(const QString& name, const QByteArray& bytes, QStringList* written,
                        QStringList* errors) {
    bool any = false;
    for (const QString& dir : candidateDirs()) {
        if (!QDir().mkpath(dir)) {
            if (errors) *errors << QStringLiteral("%1: can't create the directory").arg(dir);
            continue;
        }
        const QString path = dir + "/" + name;
        QSaveFile file(path);  // writes a temporary file and renames it, so a crash never leaves half a file
        if (!file.open(QIODevice::WriteOnly)) {
            if (errors) *errors << QStringLiteral("%1: %2").arg(path, file.errorString());
            continue;
        }
        file.write(bytes);
        if (!file.commit()) {
            if (errors) *errors << QStringLiteral("%1: %2").arg(path, file.errorString());
            continue;
        }
        any = true;
        if (written) *written << path;
    }
    return any;
}

QList<QByteArray> ConfigFiles::readAll(const QString& name, QStringList* paths) {
    struct Found { QDateTime when; QString path; };
    QList<Found> found;
    for (const QString& dir : readDirs()) {
        const QFileInfo info(dir + "/" + name);
        if (info.isFile()) found.append({info.lastModified(), info.absoluteFilePath()});
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.when > b.when; });

    QList<QByteArray> out;
    for (const Found& f : found) {
        QFile file(f.path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        out << file.readAll();
        if (paths) *paths << f.path;
    }
    return out;
}

} // namespace camsyringe

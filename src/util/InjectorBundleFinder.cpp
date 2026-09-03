#include "util/InjectorBundleFinder.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>

namespace camsyringe {

namespace {

constexpr const char* kBundleFilter = "qcarcam_injector_bundle_v*.bin";
constexpr const char* kInstallSourceMarker = ".install-source";

QSettings makeSettings() { return QSettings("CamSyringe", "CamSyringe"); }

} // namespace

QStringList InjectorBundleFinder::candidateDirs() {
    QDir appDir(QCoreApplication::applicationDirPath());
    QString installRoot = appDir.absoluteFilePath("..");

    QStringList dirs;
    // Where the camsyringe_bundle_vX.Y.bin that installed this copy was
    // actually run from -- written by create-cam-syringe-bundle.sh's own
    // installer (see its header script). Checked FIRST: a sibling
    // qcarcam_injector_bundle_vX.Y.bin is typically sitting right there
    // (same download/handoff), which this install directory's own bin/
    // folder knows nothing about.
    QFile marker(QDir(installRoot).filePath(kInstallSourceMarker));
    if (marker.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString sourceDir = QString::fromUtf8(marker.readAll()).trimmed();
        if (!sourceDir.isEmpty()) {
            dirs << sourceDir;
        }
    }

    dirs << appDir.absolutePath()                          // e.g. ~/camsyringe/bin
         << installRoot                                    // e.g. ~/camsyringe (bundle install root)
         << appDir.absoluteFilePath("../release/artifacts"); // dev tree: build/.. -> release/artifacts
    dirs.append(makeSettings().value("injectorBundle/knownDirs").toStringList());

    QStringList unique;
    for (const QString& d : dirs) {
        QString normalized = QDir(d).absolutePath();
        if (!unique.contains(normalized)) {
            unique << normalized;
        }
    }
    return unique;
}

std::optional<QString> InjectorBundleFinder::findBundle() {
    QStringList matches;
    for (const QString& dir : candidateDirs()) {
        QDir d(dir);
        if (!d.exists()) continue;
        for (const QString& name : d.entryList({kBundleFilter}, QDir::Files)) {
            matches << d.absoluteFilePath(name);
        }
    }
    if (matches.size() == 1) {
        return matches.first();
    }
    return std::nullopt;
}

QString InjectorBundleFinder::extractVersion(const QString& bundlePath) {
    static const QRegularExpression kPattern("qcarcam_injector_bundle_v([0-9.]+)\\.bin$");
    QRegularExpressionMatch m = kPattern.match(bundlePath);
    return m.hasMatch() ? m.captured(1) : QString();
}

void InjectorBundleFinder::rememberDir(const QString& dir) {
    QString normalized = QDir(dir).absolutePath();
    QSettings settings = makeSettings();
    QStringList known = settings.value("injectorBundle/knownDirs").toStringList();
    if (!known.contains(normalized)) {
        known << normalized;
        settings.setValue("injectorBundle/knownDirs", known);
    }
}

} // namespace camsyringe

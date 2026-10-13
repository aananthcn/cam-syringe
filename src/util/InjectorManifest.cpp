#include "util/InjectorManifest.h"

#include <QFile>
#include <QRegularExpression>

namespace camsyringe {

namespace {

// Paths go into a shell command on the target, so only plain names are
// accepted: no spaces, quotes, `$`, backticks or `..` components.
bool isSafePath(const QString& p) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_./-]+$"));
    return !p.isEmpty() && re.match(p).hasMatch() && !p.contains(QStringLiteral(".."));
}

constexpr int kMaxHeaderLines = 400;

} // namespace

InjectorManifest InjectorManifestReader::read(const QString& bundlePath) {
    InjectorManifest m;
    QFile file(bundlePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return m;
    }

    static const QRegularExpression md5Re(QStringLiteral("^[0-9a-f]{32}$"));
    for (int i = 0; i < kMaxHeaderLines && !file.atEnd(); ++i) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line == QStringLiteral("__ARCHIVE_BELOW__")) {
            break; // the archive starts after this line -- nothing further is text
        }
        if (line.startsWith(QStringLiteral("# BUILD_ID="))) {
            m.buildId = line.mid(11);
        } else if (line.startsWith(QStringLiteral("# INSTALL_DIR="))) {
            m.installDir = line.mid(14);
        } else if (line.startsWith(QStringLiteral("# MANIFEST "))) {
            const QStringList parts = line.mid(11).split(' ', Qt::SkipEmptyParts);
            if (parts.size() != 2 || !md5Re.match(parts[0]).hasMatch() || !isSafePath(parts[1])) {
                return InjectorManifest(); // malformed or unsafe: ignore the whole manifest
            }
            m.paths << parts[1];
            m.md5[parts[1]] = parts[0];
        }
    }

    if (m.paths.isEmpty() || m.buildId.isEmpty() || !isSafePath(m.installDir)) {
        return InjectorManifest(); // an older bundle without a manifest
    }
    m.valid = true;
    return m;
}

QString InjectorManifestReader::verifyCommand(const InjectorManifest& manifest) {
    return QStringLiteral("cd %1 2>/dev/null && md5sum %2 2>/dev/null")
        .arg(manifest.installDir, manifest.paths.join(' '));
}

InstallDiff InjectorManifestReader::compare(const InjectorManifest& manifest,
                                            const QString& md5sumOutput) {
    static const QRegularExpression lineRe(QStringLiteral("^([0-9a-fA-F]{32})\\s+(\\S+)\\s*$"));
    QMap<QString, QString> actual;
    for (const QString& line : md5sumOutput.split('\n')) {
        const auto match = lineRe.match(line.trimmed());
        if (match.hasMatch()) {
            actual[match.captured(2)] = match.captured(1).toLower();
        }
    }

    InstallDiff diff;
    for (const QString& path : manifest.paths) {
        auto it = actual.constFind(path);
        if (it == actual.constEnd()) {
            diff.missing << path;
        } else if (*it != manifest.md5.value(path)) {
            diff.differing << path;
        }
    }
    return diff;
}

} // namespace camsyringe

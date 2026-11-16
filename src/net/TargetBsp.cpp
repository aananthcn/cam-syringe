#include "net/TargetBsp.h"

#include <QStringList>

namespace camsyringe {

namespace {
const int kMinimum[3] = {14, 4, 1};
// Releases the bundle has actually been run on (the bench target).
const char* const kVerified[] = {"v14.4.1"};
} // namespace

QString TargetBsp::minimumVersion() {
    return QString("v%1.%2.%3").arg(kMinimum[0]).arg(kMinimum[1]).arg(kMinimum[2]);
}

QString TargetBsp::osReleaseValue(const QString& text, const QString& key) {
    const QString prefix = key + "=";
    for (const QString& raw : text.split('\n')) {
        const QString line = raw.trimmed();
        if (!line.startsWith(prefix)) continue;
        QString v = line.mid(prefix.size()).trimmed();
        if (v.size() >= 2 && (v.startsWith('"') || v.startsWith('\'')) && v.endsWith(v.at(0))) {
            v = v.mid(1, v.size() - 2);
        }
        return v;
    }
    return QString();
}

bool TargetBsp::parseVersion(const QString& text, int out[3]) {
    QString t = text.trimmed();
    if (t.startsWith('v') || t.startsWith('V')) t = t.mid(1);
    const QStringList parts = t.split('.');
    if (parts.size() < 2 || parts.size() > 3) return false;
    int v[3] = {0, 0, 0};
    for (int i = 0; i < parts.size(); ++i) {
        bool ok = false;
        v[i] = parts[i].toInt(&ok);
        if (!ok || v[i] < 0) return false;
    }
    for (int i = 0; i < 3; ++i) out[i] = v[i];
    return true;
}

TargetBsp TargetBsp::classify(const QString& text) {
    TargetBsp r;
    r.versionId = osReleaseValue(text, "VERSION_ID");
    r.prettyName = osReleaseValue(text, "PRETTY_NAME");
    r.buildId = osReleaseValue(text, "BUILD_ID");
    const QString shown = r.prettyName.isEmpty() ? r.versionId : r.prettyName;

    int v[3];
    if (r.versionId.isEmpty() || !parseVersion(r.versionId, v)) {
        r.verdict = Verdict::Unknown;
        r.message = "Could not read a BSP version (VERSION_ID) from the target's /etc/os-release, "
                    "so the install is not blocked on it.";
        return r;
    }

    bool older = false;
    for (int i = 0; i < 3; ++i) {
        if (v[i] != kMinimum[i]) { older = v[i] < kMinimum[i]; break; }
    }
    if (older) {
        r.verdict = Verdict::TooOld;
        r.message =
            QString("This board runs %1%2, which is older than %3, the oldest BSP the injector "
                    "bundle is built for. The bundle's programs link against the board's own "
                    "libraries, and on an older BSP they fail to load (seen on v14.2.6: \"unknown "
                    "symbol: cache_init referenced from qcarcam_injector\"). Nothing was changed on "
                    "the target.\n\nUpdate the board's BSP to %3 or newer, or install an injector "
                    "bundle built for this BSP. To install anyway, start CamSyringe with "
                    "CAMSYRINGE_ALLOW_UNSUPPORTED_BSP=1.")
                .arg(shown, r.buildId.isEmpty() ? QString() : " (build " + r.buildId + ")",
                     minimumVersion());
        return r;
    }

    bool verified = false;
    for (const char* ok : kVerified) {
        int k[3];
        verified = verified || (parseVersion(QLatin1String(ok), k) && k[0] == v[0] && k[1] == v[1] && k[2] == v[2]);
    }
    r.verdict = verified ? Verdict::Verified : Verdict::Untested;
    r.message = verified
                    ? QString("BSP %1 is a release the injector bundle has been run on.").arg(shown)
                    : QString("BSP %1 is newer than %2 but the injector bundle has not been run on "
                              "it; installing, and the load check afterwards will say if it works.")
                          .arg(shown, minimumVersion());
    return r;
}

} // namespace camsyringe

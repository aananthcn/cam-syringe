#pragma once

#include <QString>

namespace camsyringe {

// docs/adr/0020: which CARIAD OneBSP releases the injector bundle can be installed on.
// The bundle's programs are linked against the target's own libraries (/mnt/lib64/camera ...),
// so a board whose BSP is older than the one the bundle was built against fails at load time
// ("unknown symbol: cache_init referenced from qcarcam_injector" on v14.2.6), after the install has
// already deleted the previous one.
struct TargetBsp {
    enum class Verdict {
        Unknown,   // /etc/os-release unreadable or has no parsable VERSION_ID: not blocked, logged
        Verified,  // a release the bundle has been run on
        Untested,  // newer than the minimum but never run: allowed, logged
        TooOld,    // older than the minimum: refused, because the bundle is known not to load
    };

    Verdict verdict = Verdict::Unknown;
    QString versionId;   // "v14.4.1"
    QString prettyName;  // "CARIAD OneBSP v14.4.1"
    QString buildId;     // "2026_10_12-06_18"
    QString message;     // one paragraph for the log and, for TooOld, the dialog

    // `osReleaseText` is the output of `cat /etc/os-release` (extra text after it is ignored).
    static TargetBsp classify(const QString& osReleaseText);

    // The value of KEY= in os-release text, quotes removed; empty if absent.
    static QString osReleaseValue(const QString& osReleaseText, const QString& key);

    // "v14.4.1" / "14.4.1" -> {14,4,1}; false if it is not dotted numbers.
    static bool parseVersion(const QString& text, int out[3]);

    static QString minimumVersion();   // "v14.4.1"
};

} // namespace camsyringe

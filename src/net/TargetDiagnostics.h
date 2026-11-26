#pragma once

#include "net/TargetSsh.h"

#include <QString>

namespace camsyringe {

// docs/adr/0020: when the injector bundle exits non-zero on the target, a diagnostic log is
// written ON THE TARGET, in /var/opt/tmp, so whoever is at that board can copy one file and send
// it. It records the BSP version, the library search path, every installed file against the
// bundle's manifest, and for each program what happened when it was started (the loader's exact
// message) together with where the missing library or symbol is, or that it is nowhere.
class TargetDiagnostics {
public:
    struct Result {
        bool written = false;     // the log file exists on the target
        QString path;             // its path on the target
        QString text;             // its content, as read back
        QStringList summary;      // its "SUMMARY:" lines, one per finding
    };

    static constexpr const char* kInstallDir = "/var/opt";
    static constexpr const char* kLogName = "qcarcam_install_diag.log";

    // The remote shell script (POSIX sh / QNX ksh; no printf, no bash-isms). Pure, so it is
    // tested on the host against a fake install tree.
    static QString script(const QString& installDir, const QString& bundleName);

    // Runs script() on the target, then reads the log back. Best effort: never throws away the
    // original failure, `written` is false if anything here failed.
    static Result collect(TargetSsh& ssh, const QString& target, const QString& bundleName);
};

} // namespace camsyringe

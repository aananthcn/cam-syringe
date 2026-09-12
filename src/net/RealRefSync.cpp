#include "net/RealRefSync.h"

namespace camsyringe {

QString realRefSyncCommand() {
    // Plain POSIX sh (this target's shell, no bash-isms) -- $(...) command
    // substitution picks .real when the toggle has already backed it up,
    // else the live file (still genuine in that case).
    return QStringLiteral(
        "mkdir -p /var/opt/lib/real-ref && "
        "cp \"$([ -e /mnt/lib64/camera/libqcxclient.so.real ] && "
        "echo /mnt/lib64/camera/libqcxclient.so.real || "
        "echo /mnt/lib64/camera/libqcxclient.so)\" "
        "/var/opt/lib/real-ref/libqcxclient.so");
}

}  // namespace camsyringe

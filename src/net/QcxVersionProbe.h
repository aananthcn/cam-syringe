#pragma once

#include <QString>

#include <functional>

namespace camsyringe {

class TargetSsh;

// Checks, via a lightweight live functional test over SSH, whether
// `target`'s real QCX driver reports an API version incompatible with
// this build -- see qcarcam-injector's docs/adr/
// 0002-qcx-client-api-version-must-match-board.md for the full root
// cause. A DEDICATED, PROACTIVE check -- deliberately not folded into
// CameraGeometryResolver's own per-camera resolution flow (that only
// fires when a specific camera's static XML resolution is unavailable
// AND needs the live-query fallback, so it can legitimately stay silent
// even when this mismatch genuinely exists) or camtest's own manual
// output (which a user has to know to go look at). This runs regardless
// of whether any camera's resolution flow happens to touch the real
// driver at all.
//
// IMPORTANT, confirmed directly from QcxClientShim.cpp's own source:
// this mismatch does NOT affect CamSyringe's actual injection pipeline.
// The shim's own exported QCarCamInitialize() ignores apiVersion
// entirely for every caller (writer -- qcarcam_injector -- and any real
// downstream consumer alike) -- nothing on the injection path is ever
// rejected because of this. The mismatch only bites two things: the
// shim's own OPTIONAL real-input auto-discovery probe (probeRealInputs(),
// used by CameraGeometryResolver and by the shim's own default input
// list), and a manual camtest/qcarcam_test run while REAL (not SHIM) is
// active. This is why the UI warns rather than blocks Play -- see the
// ADR above for the full reasoning, including when blocking WOULD be
// correct (if --inject-only is unchecked, the local Screen/EGL preview
// render is a still-valid reason to keep streaming even in a
// hypothetical where some consumer app rejected the frames; if a real
// rejection path is ever found, that distinction is what should decide
// it, not a blanket block).
//
// The probe itself: briefly runs /var/opt/bin/qcarcam_injector with a
// scratch camera id (99, safely outside kMaxCamId's real 1-16 range) and
// a scratch ring name, over SSH, and reads the shim's own
// "[shim] input probe: ... QCARCAM_RET_UNSUPPORTED ..." diagnostic lines
// from its output (see QcxClientShim.cpp's probeRealInputs()) --
// confirmed live to reach this point in ~2s with NO display-service
// dependency (unlike qcarcam_test, which needs a display up first and
// proved flaky to drive reliably from here). Killed after a couple of
// seconds regardless of outcome; never disturbs a real, already-running
// session (distinct scratch id/ring, never the real control port).
//
// Passwordless-only, same convention as Ipv6SupportProbe/
// MainWindow::refreshShimStatus() -- NEVER prompts for credentials,
// since this is an automatic check, not a deliberate user action.
class QcxVersionProbe {
public:
    struct Result {
        // true: the probe reached a conclusion (SSH + the injector binary
        // both worked). false: inconclusive (no passwordless auth, SSH
        // failure, binary not found) -- callers must NOT warn in that
        // case; the other fields are meaningless when this is false.
        bool ok = false;
        // true: the shim's own probe reported QCARCAM_RET_UNSUPPORTED
        // from the real driver -- the known API version mismatch.
        bool versionMismatch = false;
        // This build's own compiled QCX API version, e.g. "13828 (v6.4.3)"
        // -- parsed from the shim's own diagnostic line, empty if that
        // line wasn't seen (e.g. versionMismatch is false, or an
        // unexpectedly different failure occurred).
        QString builtVersion;
    };
    using Callback = std::function<void(Result)>;

    // Runs on its own background thread; callback fires from that same
    // thread, NOT marshaled to the GUI thread -- callers must do that
    // themselves (same convention as Ipv6SupportProbe::checkAsync).
    static void checkAsync(TargetSsh& ssh, QString target, QString sshUser, QString sshKeyPath,
                            Callback callback);
};

}  // namespace camsyringe

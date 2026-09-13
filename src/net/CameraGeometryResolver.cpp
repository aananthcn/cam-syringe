#include "net/CameraGeometryResolver.h"

#include "net/TargetSsh.h"

#include <QRegularExpression>

#include <thread>

namespace camsyringe {

namespace {

const QString kRealRefLibDir = QStringLiteral("/var/opt/lib/real-ref");

// Mirrors /mnt/scripts/camtest's own board-variant + `case $CAM in`
// branching VERBATIM (re-read directly from the real script this
// session, not guessed) -- re-derive this from camtest's own source if
// that script's branching ever changes in a future release, don't
// hand-patch a diff against it.
QString xmlFilenameForCamId(const QString& unameM, int camId) {
    static const QRegularExpression kAeB0B2(QStringLiteral("CARIAD_AE_B0|CARIAD_AE_B2"));
    static const QRegularExpression kAeB3(QStringLiteral("CARIAD_AE_B3"));

    if (unameM.contains(kAeB0B2)) {
        switch (camId) {
            case 0: return QStringLiteral("1cam_728_dual.xml");
            case 2: return QStringLiteral("1cam_ifcd.xml");
            default: return QStringLiteral("1cam_623.xml");
        }
    }
    if (unameM.contains(kAeB3)) {
        switch (camId) {
            case 0: return QStringLiteral("1cam_ifcd.xml");
            case 2: return QStringLiteral("1cam_728_dual.xml");
            default: return QStringLiteral("1cam_623.xml");
        }
    }
    // else branch
    switch (camId) {
        case 0:
        case 3:
            return QStringLiteral("1cam_728_dual.xml");
        case 8:
        case 9:
        case 10:
            return QStringLiteral("1cam_728.xml");
        case 12:
            return QStringLiteral("1cam_ifcd.xml");
        case 13:
            return QStringLiteral("1cam_ifcc.xml");
        default:
            return QStringLiteral("1cam_623.xml");
    }
}

// Parses <output_setting ... width="N" height="M" .../> out of a
// resolved XML template's text. Two separate regexes (not one combined,
// ordered one) since attribute order varies across templates (confirmed
// by reading 1cam_ifcc.xml/1cam_728.xml directly). Returns false if no
// width/height is declared at all (e.g. 1cam_728.xml) -- NOT an error,
// just "this id needs the live-query fallback instead" (see class
// comment's step 3).
bool parseDeclaredResolution(const QString& xmlText, uint32_t* outWidth, uint32_t* outHeight) {
    static const QRegularExpression kWidthRe(
        QStringLiteral(R"RX(<output_setting[^>]*\bwidth="(\d+)")RX"));
    static const QRegularExpression kHeightRe(
        QStringLiteral(R"RX(<output_setting[^>]*\bheight="(\d+)")RX"));
    QRegularExpressionMatch wm = kWidthRe.match(xmlText);
    QRegularExpressionMatch hm = kHeightRe.match(xmlText);
    if (!wm.hasMatch() || !hm.hasMatch()) {
        return false;
    }
    *outWidth = wm.captured(1).toUInt();
    *outHeight = hm.captured(1).toUInt();
    return true;
}

// Parses qcarcam_test's own "--- QCarCam Queried Inputs ----" dump for
// camId's CURRENT mode's primary (srcId 0) resolution. Confirmed live
// this is NOT as simple as grabbing the first "| WxH fmt=" in the dump:
// a real input can report several modes (e.g. numModes=6) with the
// hardware's own currMode field pointing at a NON-ZERO index, and each
// mode lists multiple sources (srcId 0 == the real image, srcId 1+
// observed live to be much thinner "embedded metadata" lines, e.g.
// 3840x21 alongside a real 3840x2160 srcId 0 -- not real image
// content). Deliberately NOT correlating by input_id vs. camId beyond
// confirming the real hardware reported this id at all: the XML
// template's own <input ... src_id="0"/> is hardcoded to 0 regardless
// of qcarcam_id (confirmed by reading 1cam_728.xml directly), so
// input_id is the only reliable link back to camId, srcId is not.
bool parseQueriedResolution(const QString& output, int camId, uint32_t* outWidth,
                             uint32_t* outHeight) {
    static const QRegularExpression kInputHeaderRe(
        QStringLiteral(R"RX(\d+: input_id=(\d+),[^\n]*currMode=0x([0-9a-fA-F]+))RX"));
    static const QRegularExpression kNextInputRe(QStringLiteral(R"RX(\n\d+: input_id=)RX"));
    static const QRegularExpression kModeIdRe(QStringLiteral(R"RX(modeId=(\d+))RX"));
    static const QRegularExpression kSrc0Re(
        QStringLiteral(R"RX(srcId 0 \|\s*(\d+)x(\d+)\s+fmt=)RX"));

    QRegularExpressionMatch header;
    for (auto it = kInputHeaderRe.globalMatch(output); it.hasNext();) {
        QRegularExpressionMatch m = it.next();
        if (m.captured(1).toInt() == camId) {
            header = m;
            break;
        }
    }
    if (!header.hasMatch()) {
        return false; // the real hardware never reported this id at all
    }
    bool ok = false;
    int currMode = header.captured(2).toInt(&ok, 16);
    if (!ok) {
        return false;
    }

    // This input's own block: from right after its header line up to
    // (not including) the next "N: input_id=" line, or end of dump --
    // multiple inputs' blocks are concatenated in one dump (the real
    // hardware reports every input it discovers, not just the one this
    // probe's resolved single-<input> XML declared).
    qsizetype blockStart = header.capturedEnd();
    QRegularExpressionMatch nextInput = kNextInputRe.match(output, blockStart);
    qsizetype blockEnd = nextInput.hasMatch() ? nextInput.capturedStart() : output.size();
    QString inputBlock = output.mid(blockStart, blockEnd - blockStart);

    // Within that block, this input's CURRENT mode's own sub-block
    // (modeId == currMode from the header above, NOT just the first
    // mode listed -- confirmed live this camera's currMode pointed at
    // index 4 out of 6, not 0).
    qsizetype modeStart = -1;
    qsizetype modeEnd = inputBlock.size();
    for (auto it = kModeIdRe.globalMatch(inputBlock); it.hasNext();) {
        QRegularExpressionMatch m = it.next();
        if (m.captured(1).toInt() == currMode) {
            modeStart = m.capturedEnd();
            QRegularExpressionMatch nextMode = kModeIdRe.match(inputBlock, modeStart);
            modeEnd = nextMode.hasMatch() ? nextMode.capturedStart() : inputBlock.size();
            break;
        }
    }
    if (modeStart < 0) {
        return false;
    }
    QString modeBlock = inputBlock.mid(modeStart, modeEnd - modeStart);

    QRegularExpressionMatch src = kSrc0Re.match(modeBlock);
    if (!src.hasMatch()) {
        return false;
    }
    *outWidth = src.captured(1).toUInt();
    *outHeight = src.captured(2).toUInt();
    return true;
}

} // namespace

void CameraGeometryResolver::resolveAsync(TargetSsh& ssh, QString target, QString sshUser,
                                           QString sshKeyPath, std::vector<int> camIds,
                                           Callback callback, ProgressCallback progressCallback) {
    std::thread([&ssh, target = std::move(target), sshUser = std::move(sshUser),
                 sshKeyPath = std::move(sshKeyPath), camIds = std::move(camIds),
                 callback = std::move(callback), progressCallback = std::move(progressCallback)]() {
        std::vector<ResolvedCameraGeometry> results;
        results.reserve(camIds.size());

        // Passwordless-only -- see class comment. A target needing
        // credentials just fails every id below (ok=false), rather than
        // popping an unprompted credentials dialog.
        auto declineCredentials = [](const QString&, QString*, QString*) { return false; };
        if (!ssh.ensureAuth(target, sshUser, sshKeyPath, declineCredentials)) {
            for (int id : camIds) {
                results.push_back(ResolvedCameraGeometry{id, false, false, 0, 0});
                if (progressCallback) {
                    progressCallback(static_cast<int>(results.size()), static_cast<int>(camIds.size()));
                }
            }
            callback(std::move(results));
            return;
        }

        TargetSsh::Result unameResult = ssh.run(target, QStringLiteral("uname -m"), 5000);
        QString unameM = unameResult.ok() ? unameResult.stdOut : QString();

        ResolvedCameraGeometry lastSuccess{};
        bool haveLastSuccess = false;
        // Checked at most once per resolveAsync() batch, not once per
        // camera -- if the board's QCX driver has an API version
        // mismatch (qcarcam-injector's docs/adr/
        // 0002-qcx-client-api-version-must-match-board.md), it affects
        // every camera's live query uniformly, so one slog2info check
        // covers the whole batch instead of repeating it per id.
        bool checkedQcxVersionReason = false;
        QString qcxVersionMismatchReason;

        for (int id : camIds) {
            ResolvedCameraGeometry geo;
            geo.camId = id;

            QString xmlFile = xmlFilenameForCamId(unameM, id);
            TargetSsh::Result catResult =
                ssh.run(target,
                        QStringLiteral("cat /mnt/scripts/%1 | sed 's/CAM_INPUT/%2/g'")
                            .arg(xmlFile)
                            .arg(id),
                        5000);

            uint32_t w = 0;
            uint32_t h = 0;
            bool resolved = false;
            if (catResult.ok() && parseDeclaredResolution(catResult.stdOut, &w, &h)) {
                resolved = true;
            } else {
                // Live-query fallback -- mirrors camtest's own preamble
                // (source start-qcxserver.sh, cd, start_server, kill any
                // already-running qcarcam_test -- same side effect
                // camtest itself already has every time it's invoked, not
                // a new risk this introduces) then runs qcarcam_test
                // against the dedicated always-real reference copy via
                // LD_LIBRARY_PATH (see kRealRefLibDir), bounded by a real
                // timeout -- just long enough to reach the "Queried
                // Inputs" dump, never long enough to actually stream.
                // PREPENDED to the existing $LD_LIBRARY_PATH, not a plain
                // assignment -- confirmed live that a plain assignment
                // clobbers the target's own rich default (which already
                // includes /mnt/lib64/camera, where libqcxclient.so's OWN
                // real dependencies -- libqcxosal.so etc -- live), making
                // even the real libqcxclient.so itself fail to load
                // ("ldd:FATAL: Could not load library libqcxosal.so").
                // Prepending puts real-ref FIRST (so libqcxclient.so
                // still resolves to this override) while everything else
                // still falls through to the target's own default.
                // qcarcam_test also needs the display service up
                // (confirmed live: without it, it fails at its own
                // "test_util_init failed 1" step, well before reaching
                // the Queried Inputs dump this probe needs) -- run_qcarcam.sh's
                // own ensure-block is ported here VERBATIM (guarded on
                // /dev/openwfd_server_0 first: calling
                // start-display-service.sh a SECOND time while already
                // running corrupts the display -- see run_qcarcam.sh's
                // own comment on this exact hazard).
                QString probePath = QStringLiteral("/tmp/camsyringe_probe_%1.xml").arg(id);
                QString cmd = QStringLiteral(
                                  ". /mnt/scripts/start-qcxserver.sh; cd /mnt/bin/camera; "
                                  "if [ -e /dev/openwfd_server_0 ]; then :; "
                                  "elif [ -x /mnt/scripts/start-display-service.sh ]; then "
                                  "sh /mnt/scripts/start-display-service.sh "
                                  ">/tmp/camsyringe_display.log 2>&1 & "
                                  "i=0; while [ $i -lt 30 ]; do "
                                  "[ -e /dev/openwfd_server_0 ] && break; sleep 1; i=$((i+1)); done; "
                                  "fi; "
                                  "start_server; "
                                  "if pidin | grep -q qcarcam_test; then slay -f qcarcam_test; "
                                  "sleep 1; fi; "
                                  "cat /mnt/scripts/%1 | sed 's/CAM_INPUT/%2/g' > %3 && "
                                  // `timeout` -- confirmed absent anywhere on this board
                                  // (find / -iname timeout finds nothing; PATH search
                                  // fails with "cannot execute") -- NOT just this one
                                  // board's variant, per this project's own established
                                  // portable-bounding idiom used everywhere else a command
                                  // needs a hard runtime cap on this target (run_qcarcam.sh's
                                  // own dispatcher wait, DispatcherRemoteControl, etc.):
                                  // background + sleep + kill + wait, not `timeout`. Output
                                  // is never redirected away, so it still flows into this
                                  // whole command's own captured stdout exactly as a plain
                                  // foreground run would.
                                  "LD_LIBRARY_PATH=%4:$LD_LIBRARY_PATH "
                                  "./qcarcam_test/qcarcam_test -config=%3 & QT_PID=$!; "
                                  "sleep 8; kill $QT_PID 2>/dev/null; wait $QT_PID 2>/dev/null; "
                                  "rm -f %3")
                                  .arg(xmlFile)
                                  .arg(id)
                                  .arg(probePath)
                                  .arg(kRealRefLibDir);
                // Up to ~30s for a cold display-service start (see the
                // command's own comment) plus the 8s qcarcam_test bound
                // plus slack for the rest of the preamble.
                TargetSsh::Result probeResult = ssh.run(target, cmd, 45000);
                if (probeResult.ok() && parseQueriedResolution(probeResult.stdOut, id, &w, &h)) {
                    resolved = true;
                } else if (!checkedQcxVersionReason) {
                    // The live query just failed for this id -- before
                    // giving up, check ONCE per batch whether this
                    // board's qcxserver log shows the known QCX API
                    // version mismatch (same signature the injector
                    // shim's own probeRealInputs() diagnostic looks for,
                    // see QcxClientShim.cpp) as the likely cause, so the
                    // UI can say something more useful than a bare
                    // "unknown" to whoever's looking at it.
                    checkedQcxVersionReason = true;
                    TargetSsh::Result slog = ssh.run(
                        target, QStringLiteral("slog2info | grep -i qcarcam | tail -20"), 8000);
                    if (slog.stdOut.contains(QStringLiteral(
                            "is not compatible with version of lib_QCXClient"))) {
                        qcxVersionMismatchReason = QStringLiteral(
                            "board's QCX driver reports an API version mismatch between this "
                            "build and the real driver -- see qcarcam-injector's "
                            "docs/adr/0002-qcx-client-api-version-must-match-board.md");
                    }
                }
            }

            if (resolved) {
                geo.ok = true;
                geo.wasFallback = false;
                geo.width = w;
                geo.height = h;
                lastSuccess = geo;
                haveLastSuccess = true;
            } else if (haveLastSuccess) {
                geo.ok = true;
                geo.wasFallback = true;
                geo.width = lastSuccess.width;
                geo.height = lastSuccess.height;
            } else {
                geo.ok = false;
                geo.wasFallback = false;
                geo.failureReason = qcxVersionMismatchReason; // empty unless found above
            }

            results.push_back(geo);
            if (progressCallback) {
                progressCallback(static_cast<int>(results.size()), static_cast<int>(camIds.size()));
            }
        }

        callback(std::move(results));
    }).detach();
}

} // namespace camsyringe

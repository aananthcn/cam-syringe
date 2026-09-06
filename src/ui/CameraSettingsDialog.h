#pragma once

#include <QDateTime>
#include <QDialog>
#include <QMap>
#include <QString>

#include <vector>

#include "net/CameraGeometryResolver.h"
#include "net/TargetSsh.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace camsyringe::ui {

// Modal, read-only viewer for "Settings > Camera": shows EVERY QCarCam id
// the chip can address over GMSL2 (camsyringe::kMinCamId/kMaxCamId, 1-16
// -- see MainWindow::onCameraSettingsTriggered()'s own comment), each
// with its REAL, target-authored resolution -- the same data
// CameraGeometryResolver already resolves for MainWindow's own
// letterboxing (see net/CameraGeometryResolver.h). Explicit requirement:
// this is deliberately independent of whichever camera ids the current
// session happens to be injecting into -- the current hardware wires up
// to 12 of the chip's 16 possible ids, and the point of this dialog is
// to see the target's real configuration for the whole range regardless
// of what's actually chosen for injection right now. A "Read" button
// re-queries the target on demand (always fresh; unlike
// MainWindow::resolveCameraGeometry() this never skips an id just
// because it's already cached) and a bottom-left timestamp shows the
// last successful read. Reachable any time a target is configured --
// unlike CameraConfigDialog ("Settings > CamSyringe"), this is pure
// read-back with no editable session settings, so it isn't restricted to
// Idle state.
class CameraSettingsDialog : public QDialog {
    Q_OBJECT

public:
    // ssh: MainWindow's own sshSession_ (caller-owned, outlives this
    // modal dialog). camIds: the full set of ids to show a row for --
    // MainWindow passes every id 1-16 (see class comment), not just the
    // currently configured ones; sorted ascending internally before any
    // resolve (CameraGeometryResolver's own fallback-order requirement).
    // initialResults/initialReadTime: seeds
    // the table from MainWindow's existing geometryCache_/read-timestamp
    // for `target`, if any, so opening this dialog shows last-known data
    // immediately without forcing a fresh target round-trip.
    explicit CameraSettingsDialog(camsyringe::TargetSsh& ssh, QString target, QString sshUser,
                                   QString sshKeyPath, std::vector<int> camIds,
                                   const QMap<int, camsyringe::ResolvedCameraGeometry>& initialResults,
                                   const QDateTime& initialReadTime, QWidget* parent = nullptr);

signals:
    // Emitted after a successful Read, already marshaled onto the GUI
    // thread -- MainWindow connects this to fold fresh results back into
    // its own geometryCache_/read-timestamp, so a Read here benefits a
    // later Configure/Play too, not just this dialog's own table.
    void geometryResolved(QString target, std::vector<camsyringe::ResolvedCameraGeometry> results,
                           QDateTime readTime);

private slots:
    void onReadClicked();

private:
    void populateTable(const QMap<int, camsyringe::ResolvedCameraGeometry>& results);
    void setReadTime(const QDateTime& when); // updates readTimeLabel_'s text ("Never read yet" if invalid)

    camsyringe::TargetSsh& ssh_;
    QString target_;
    QString sshUser_;
    QString sshKeyPath_;
    std::vector<int> camIds_;

    QTableWidget* table_ = nullptr;
    // Bottom-left corner: readTimeLabel_ normally, swapped for
    // progressBar_ for the duration of a Read (a full 1-16 sweep can take
    // tens of seconds -- see CameraGeometryResolver::ProgressCallback's
    // own comment -- so a static "Reading from target..." wasn't enough
    // feedback that it's still actually making progress, not stuck).
    // Both occupy the same layout position; exactly one is visible at a
    // time.
    QLabel* readTimeLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QPushButton* readButton_ = nullptr;
};

} // namespace camsyringe::ui

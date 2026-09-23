#pragma once

#include <QDialog>
#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

#include "camera/PortScheme.h"
#include "net/CameraGeometryResolver.h"

class QSpinBox;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QWidget;
class QLabel;

namespace camsyringe::ui {

// Modal dialog for the "Configure" menu action: target, control port,
// number of cameras (1-4), each camera's video file + QCarCam id, the
// session-wide target-side flag (--inject-only), and (Phase 3) an
// optional BLF/Ethernet replay (file + network interface) --
// deliberately the single place for all session settings, so a future
// setting gets a new field here rather than another menu action. Only
// reachable while MainWindow is in its Idle state.
class CameraConfigDialog : public QDialog {
    Q_OBJECT

public:
    // camIds: parallel to initialFiles (same length or shorter -- missing
    // entries default to a distinct id, see .cpp). controlPort/sshUser/
    // sshKeyPath/injectOnly/blf*: the previous session's
    // settings, so re-opening Configure doesn't reset them. initialBlfPath
    // empty means BLF replay starts unchecked.
    // resolvedGeometry: MainWindow's geometryCache_ entry for
    // initialTarget (empty map if none resolved yet, or if
    // initialTarget is new/empty) -- purely informational, shown next to
    // each row's Cam ID field and refreshed live as that field is edited
    // (see updateResolvedLabel()); never itself triggers a live query
    // (that's MainWindow::resolveCameraGeometry(), which only ever runs
    // AFTER this dialog is accepted and closed).
    explicit CameraConfigDialog(const QString& initialTarget, int initialControlPort,
                                 const QString& initialSshUser, const QString& initialSshKeyPath,
                                 const QStringList& initialFiles, const std::vector<int>& initialCamIds,
                                 bool initialInjectOnly,
                                 const QString& initialBlfPath, const QString& initialBlfInterface,
                                 const QMap<int, camsyringe::ResolvedCameraGeometry>& resolvedGeometry,
                                 QWidget* parent = nullptr);

    QString target() const;
    int controlPort() const;
    // SSH username MainWindow's sshSession_ authenticates as for this
    // target (install, Play-time dispatcher-start retry, Stop's
    // target-process kill) -- this field, not the Install dialog itself,
    // is the one place to change it (see MainWindow::sshUser_'s own
    // comment).
    QString sshUser() const;
    // Optional SSH private key path (-i) for that same authentication --
    // empty means "no explicit key, use ssh's own default identity/agent"
    // (today's behavior, unchanged). Browsable via the field's own
    // "Browse..." button; can also be typed/pasted directly.
    QString sshKeyPath() const;
    QStringList videoFiles() const;  // exactly count() entries, in order
    std::vector<int> camIds() const; // exactly count() entries, in order, parallel to videoFiles()
    bool injectOnly() const;
    // Empty blfPath() means BLF/Ethernet replay is disabled for this
    // session -- MainWindow checks that, not a separate "enabled" flag.
    QString blfPath() const;
    QString blfInterface() const;

private slots:
    void onCountChanged(int count);
    void onBrowseClicked(int row);
    void onSshKeyBrowseClicked();
    void onBlfBrowseClicked();
    void onBlfEnabledChanged(Qt::CheckState state);
    void onForceIpv4Changed(Qt::CheckState state);
    void onAccept();

private:
    void updateRowVisibility();
    // Sets rows_[row].resolvedLabel's text from resolvedGeometry_,
    // looked up by that row's CURRENT camIdSpin value -- called once at
    // construction per row and again on that row's own camIdSpin
    // valueChanged.
    void updateResolvedLabel(int row);

    QLineEdit* targetEdit_ = nullptr;
    // "Force IPv4" -- purely a convenience that rewrites targetEdit_ to
    // one of the two known default addresses (net/TargetDefaults.h) when
    // toggled; never read directly elsewhere. The actual IPv4-vs-IPv6
    // choice everywhere else (bracketHostIfIPv6, DispatcherRemoteControl's
    // own --ipv4 selection) is still derived purely from whatever target()
    // string ends up in play, so this checkbox can never silently
    // disagree with a manually-typed target.
    QCheckBox* forceIpv4Check_ = nullptr;
    QSpinBox* controlPortSpin_ = nullptr;
    QLineEdit* sshUserEdit_ = nullptr;
    QLineEdit* sshKeyPathEdit_ = nullptr;
    QPushButton* sshKeyBrowseButton_ = nullptr;
    QSpinBox* countSpin_ = nullptr;
    QCheckBox* injectOnlyCheck_ = nullptr;

    struct Row {
        QWidget* container = nullptr;
        QLineEdit* pathEdit = nullptr;
        QPushButton* browseButton = nullptr;
        QSpinBox* camIdSpin = nullptr;
        // Read-only, purely informational -- see updateResolvedLabel().
        QLabel* resolvedLabel = nullptr;
    };
    Row rows_[camsyringe::kMaxCameras];
    QMap<int, camsyringe::ResolvedCameraGeometry> resolvedGeometry_;

    QCheckBox* blfEnabledCheck_ = nullptr;
    QWidget* blfRowContainer_ = nullptr;
    QLineEdit* blfPathEdit_ = nullptr;
    QPushButton* blfBrowseButton_ = nullptr;
    QLineEdit* blfInterfaceEdit_ = nullptr;
};

} // namespace camsyringe::ui

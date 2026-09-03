#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <vector>

#include "camera/PortScheme.h"

class QSpinBox;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QWidget;

namespace camsyringe::ui {

// Modal dialog for the "Configure" menu action: target, control port,
// number of cameras (1-4), each camera's video file + QCarCam id, the two
// session-wide target-side flags (--inject-only, --qcx-bypass), and
// (Phase 3) an optional BLF/Ethernet replay (file + network interface) --
// deliberately the single place for all session settings, so a future
// setting gets a new field here rather than another menu action. Only
// reachable while MainWindow is in its Idle state.
class CameraConfigDialog : public QDialog {
    Q_OBJECT

public:
    // camIds: parallel to initialFiles (same length or shorter -- missing
    // entries default to a distinct id, see .cpp). controlPort/sshUser/
    // sshKeyPath/injectOnly/qcxBypass/blf*: the previous session's
    // settings, so re-opening Configure doesn't reset them. initialBlfPath
    // empty means BLF replay starts unchecked.
    explicit CameraConfigDialog(const QString& initialTarget, int initialControlPort,
                                 const QString& initialSshUser, const QString& initialSshKeyPath,
                                 const QStringList& initialFiles, const std::vector<int>& initialCamIds,
                                 bool initialInjectOnly, bool initialQcxBypass,
                                 const QString& initialBlfPath, const QString& initialBlfInterface,
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
    bool qcxBypass() const;
    // Empty blfPath() means BLF/Ethernet replay is disabled for this
    // session -- MainWindow checks that, not a separate "enabled" flag.
    QString blfPath() const;
    QString blfInterface() const;

private slots:
    void onCountChanged(int count);
    void onBrowseClicked(int row);
    void onSshKeyBrowseClicked();
    void onBlfBrowseClicked();
    void onBlfEnabledChanged(int state);
    void onAccept();

private:
    void updateRowVisibility();

    QLineEdit* targetEdit_ = nullptr;
    QSpinBox* controlPortSpin_ = nullptr;
    QLineEdit* sshUserEdit_ = nullptr;
    QLineEdit* sshKeyPathEdit_ = nullptr;
    QPushButton* sshKeyBrowseButton_ = nullptr;
    QSpinBox* countSpin_ = nullptr;
    QCheckBox* injectOnlyCheck_ = nullptr;
    QCheckBox* qcxBypassCheck_ = nullptr;

    struct Row {
        QWidget* container = nullptr;
        QLineEdit* pathEdit = nullptr;
        QPushButton* browseButton = nullptr;
        QSpinBox* camIdSpin = nullptr;
    };
    Row rows_[camsyringe::kMaxCameras];

    QCheckBox* blfEnabledCheck_ = nullptr;
    QWidget* blfRowContainer_ = nullptr;
    QLineEdit* blfPathEdit_ = nullptr;
    QPushButton* blfBrowseButton_ = nullptr;
    QLineEdit* blfInterfaceEdit_ = nullptr;
};

} // namespace camsyringe::ui

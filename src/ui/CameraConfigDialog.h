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
// number of cameras (1-4), each camera's video file + QCarCam id, and the
// two session-wide target-side flags (--inject-only, --qcx-bypass) --
// deliberately the single place for all session settings, so a future
// setting gets a new field here rather than another menu action. Only
// reachable while MainWindow is in its Idle state.
class CameraConfigDialog : public QDialog {
    Q_OBJECT

public:
    // camIds: parallel to initialFiles (same length or shorter -- missing
    // entries default to a distinct id, see .cpp). controlPort/injectOnly/
    // qcxBypass: the target-side control-channel session settings from
    // the PREVIOUS session, so re-opening Configure doesn't reset them.
    explicit CameraConfigDialog(const QString& initialTarget, int initialControlPort,
                                 const QStringList& initialFiles, const std::vector<int>& initialCamIds,
                                 bool initialInjectOnly, bool initialQcxBypass,
                                 QWidget* parent = nullptr);

    QString target() const;
    int controlPort() const;
    QStringList videoFiles() const;  // exactly count() entries, in order
    std::vector<int> camIds() const; // exactly count() entries, in order, parallel to videoFiles()
    bool injectOnly() const;
    bool qcxBypass() const;

private slots:
    void onCountChanged(int count);
    void onBrowseClicked(int row);
    void onAccept();

private:
    void updateRowVisibility();

    QLineEdit* targetEdit_ = nullptr;
    QSpinBox* controlPortSpin_ = nullptr;
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
};

} // namespace camsyringe::ui

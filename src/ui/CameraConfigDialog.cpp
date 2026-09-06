#include "ui/CameraConfigDialog.h"

#include <algorithm>
#include <set>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace camsyringe::ui {

namespace {
// Shared across every Browse button in this dialog (all camera rows AND
// the BLF row) AND across every time Configure is re-opened -- process
// lifetime, not persisted to disk, so the very first Browse click of a
// fresh run still has no prior value to fall back on; that case uses the
// binary's own launch cwd instead (QDir::currentPath(), which stays
// accurate as long as nothing in this process chdir()s, true here).
// Static (not a dialog member) deliberately -- CameraConfigDialog itself
// is destroyed and recreated every time Configure is opened, but the
// "remember where I last browsed" expectation spans the whole session.
QString& lastBrowsedDir() {
    static QString dir;
    return dir;
}

QString BrowseStartDir() {
    return lastBrowsedDir().isEmpty() ? QDir::currentPath() : lastBrowsedDir();
}

void RememberBrowsedFile(const QString& file) {
    lastBrowsedDir() = QFileInfo(file).absolutePath();
}
} // namespace

CameraConfigDialog::CameraConfigDialog(const QString& initialTarget, int initialControlPort,
                                        const QString& initialSshUser, const QString& initialSshKeyPath,
                                        const QStringList& initialFiles,
                                        const std::vector<int>& initialCamIds, bool initialInjectOnly,
                                        const QString& initialBlfPath,
                                        const QString& initialBlfInterface,
                                        const QMap<int, camsyringe::ResolvedCameraGeometry>& resolvedGeometry,
                                        QWidget* parent)
    : QDialog(parent), resolvedGeometry_(resolvedGeometry) {
    setWindowTitle(tr("CamSyringe Configurations"));

    auto* rootLayout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    rootLayout->addLayout(form);

    // First field, deliberately -- everything below (which rows are even
    // visible) depends on this value, so it needs to be the obvious
    // starting point rather than buried after Target/Control port where
    // it's easy to mistake for one of the per-row Cam ID spinboxes
    // instead (confirmed a real point of confusion in practice).
    countSpin_ = new QSpinBox(this);
    countSpin_->setRange(1, camsyringe::kMaxCameras);
    countSpin_->setValue(initialFiles.isEmpty()
                              ? 1
                              : std::min(static_cast<int>(initialFiles.size()), camsyringe::kMaxCameras));
    form->addRow(tr("Number of cameras:"), countSpin_);
    connect(countSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &CameraConfigDialog::onCountChanged);

    // SSH user/key deliberately come BEFORE Target (user-specified
    // reordering) -- everything else below still reads top-to-bottom the
    // same way, this pair just moved as a unit.
    //
    // Everything CamSyringe does over SSH against `target` (install,
    // Play-time dispatcher-start retry, Stop's target-process kill) uses
    // this as the username to try passwordless first -- see
    // MainWindow::sshUser_'s own comment. This is the one place to change
    // it; the Install confirmation dialog only ever displays it.
    sshUserEdit_ = new QLineEdit(initialSshUser.isEmpty() ? QStringLiteral("root") : initialSshUser, this);
    form->addRow(tr("SSH user:"), sshUserEdit_);

    // Optional -- empty (the default) leaves ssh's own default identity/
    // agent behavior untouched, exactly as before this field existed.
    // Browsable since a key file's real path is rarely something worth
    // typing by hand (~/.ssh/id_rsa, a board-specific .pem, etc.).
    auto* sshKeyRow = new QWidget(this);
    auto* sshKeyRowLayout = new QHBoxLayout(sshKeyRow);
    sshKeyRowLayout->setContentsMargins(0, 0, 0, 0);
    sshKeyPathEdit_ = new QLineEdit(initialSshKeyPath, sshKeyRow);
    sshKeyBrowseButton_ = new QPushButton(tr("Browse..."), sshKeyRow);
    connect(sshKeyBrowseButton_, &QPushButton::clicked, this,
            &CameraConfigDialog::onSshKeyBrowseClicked);
    sshKeyRowLayout->addWidget(sshKeyPathEdit_, /*stretch=*/1);
    sshKeyRowLayout->addWidget(sshKeyBrowseButton_);
    form->addRow(tr("SSH key (optional):"), sshKeyRow);

    targetEdit_ = new QLineEdit(initialTarget, this);
    form->addRow(tr("Target:"), targetEdit_);

    controlPortSpin_ = new QSpinBox(this);
    controlPortSpin_->setRange(1, 65535);
    controlPortSpin_->setValue(initialControlPort);
    form->addRow(tr("Control port:"), controlPortSpin_);

    for (int i = 0; i < camsyringe::kMaxCameras; ++i) {
        rows_[i].container = new QWidget(this);
        auto* rowLayout = new QHBoxLayout(rows_[i].container);
        rowLayout->setContentsMargins(0, 0, 0, 0);

        rows_[i].pathEdit = new QLineEdit(rows_[i].container);
        rows_[i].pathEdit->setReadOnly(true);
        // Generous fixed floor, NOT left to sizeHint()/stretch alone --
        // real report: a QLineEdit's sizeHint() doesn't grow with its
        // actual text, so this dialog's own one-time widen-by-20% (below,
        // computed off whatever was on screen AT CONSTRUCTION -- often an
        // empty path, one visible row) left no real guarantee of enough
        // room once a real (possibly long) file path got set, especially
        // with multiple camera rows all needing it at once. Sized off a
        // realistic long example path, not a raw pixel guess, so it scales
        // with the actual font/DPI the same way resolvedLabel's fixed
        // width does below.
        rows_[i].pathEdit->setMinimumWidth(rows_[i].pathEdit->fontMetrics().horizontalAdvance(
            tr("/mnt/c/Users/aananth/Videos/camera_footage_3840x2160_30fps.mp4")));
        if (i < initialFiles.size()) {
            rows_[i].pathEdit->setText(initialFiles[i]);
        }
        rows_[i].browseButton = new QPushButton(tr("Browse..."), rows_[i].container);
        connect(rows_[i].browseButton, &QPushButton::clicked, this,
                [this, i]() { onBrowseClicked(i); });

        auto* camIdLabel = new QLabel(tr("Cam ID:"), rows_[i].container);
        rows_[i].camIdSpin = new QSpinBox(rows_[i].container);
        rows_[i].camIdSpin->setRange(camsyringe::kMinCamId, camsyringe::kMaxCamId);
        // Default 1,2,3,4 for a row with no prior value -- a generic
        // placeholder, not tied to any specific board's actual working
        // ids (those vary per target, see qcarcam-injector's own
        // allcamtest output) -- the user is expected to set real values.
        rows_[i].camIdSpin->setValue(static_cast<int>(i) < static_cast<int>(initialCamIds.size())
                                          ? initialCamIds[static_cast<size_t>(i)]
                                          : i + 1);

        // Read-only, purely informational -- shows what
        // MainWindow::resolveCameraGeometry() already found for this
        // row's CURRENT Cam ID against resolvedGeometry_ (a snapshot
        // from when this dialog was opened -- no live query happens
        // while it's up, see updateResolvedLabel()'s own comment).
        // Fixed width (sized off the longest text this label ever shows,
        // "9999x9999 (from another cam)") -- real report: without this,
        // switching a row's Cam ID to one whose resolved text is longer
        // (e.g. "3840x2160") visibly shrank pathEdit (the only OTHER
        // widget in this row with any stretch) to make room, since
        // nothing here otherwise reserves stable space for this label.
        rows_[i].resolvedLabel = new QLabel(rows_[i].container);
        rows_[i].resolvedLabel->setFixedWidth(
            rows_[i].resolvedLabel->fontMetrics().horizontalAdvance(
                tr("9999x9999 (from another cam)")));
        connect(rows_[i].camIdSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this, i](int) { updateResolvedLabel(i); });

        rowLayout->addWidget(rows_[i].pathEdit, /*stretch=*/1);
        rowLayout->addWidget(rows_[i].browseButton);
        rowLayout->addWidget(camIdLabel);
        rowLayout->addWidget(rows_[i].camIdSpin);
        rowLayout->addWidget(rows_[i].resolvedLabel);

        form->addRow(tr("Camera %1 video:").arg(i), rows_[i].container);
        updateResolvedLabel(i);
    }

    injectOnlyCheck_ = new QCheckBox(
        tr("Inject only (no local preview render on target -- see qcarcam_dispatcher's --inject-only)"),
        this);
    injectOnlyCheck_->setChecked(initialInjectOnly);
    rootLayout->addWidget(injectOnlyCheck_);

    // Phase 3: BLF/Ethernet replay -- session-wide, not per-camera (one
    // BLF file replayed over one network interface, independent of how
    // many cameras are configured). See BlfReplayer.h for what "replay"
    // means here (verbatim raw AF_PACKET injection, original timing).
    blfEnabledCheck_ = new QCheckBox(tr("Replay BLF Ethernet capture"), this);
    blfEnabledCheck_->setChecked(!initialBlfPath.isEmpty());
    rootLayout->addWidget(blfEnabledCheck_);
    connect(blfEnabledCheck_, &QCheckBox::stateChanged, this,
            &CameraConfigDialog::onBlfEnabledChanged);

    blfRowContainer_ = new QWidget(this);
    auto* blfLayout = new QFormLayout(blfRowContainer_);
    blfLayout->setContentsMargins(20, 0, 0, 0); // indented under the checkbox above

    auto* blfPathRow = new QWidget(blfRowContainer_);
    auto* blfPathRowLayout = new QHBoxLayout(blfPathRow);
    blfPathRowLayout->setContentsMargins(0, 0, 0, 0);
    blfPathEdit_ = new QLineEdit(initialBlfPath, blfPathRow);
    blfPathEdit_->setReadOnly(true);
    blfBrowseButton_ = new QPushButton(tr("Browse..."), blfPathRow);
    connect(blfBrowseButton_, &QPushButton::clicked, this, &CameraConfigDialog::onBlfBrowseClicked);
    blfPathRowLayout->addWidget(blfPathEdit_, /*stretch=*/1);
    blfPathRowLayout->addWidget(blfBrowseButton_);
    blfLayout->addRow(tr("BLF file:"), blfPathRow);

    blfInterfaceEdit_ = new QLineEdit(
        initialBlfInterface.isEmpty() ? QStringLiteral("enp6s0") : initialBlfInterface,
        blfRowContainer_);
    blfLayout->addRow(tr("Network interface:"), blfInterfaceEdit_);

    rootLayout->addWidget(blfRowContainer_);
    blfRowContainer_->setVisible(blfEnabledCheck_->isChecked());

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &CameraConfigDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rootLayout->addWidget(buttons);

    updateRowVisibility(); // its own adjustSize() runs first; widen AFTER, so it isn't undone by it

    // 20% wider than this dialog would otherwise be, user-specified --
    // the natural sizeHint (computed off the longest row's content, e.g.
    // a video file path) still left the resolution/Cam ID columns
    // visually cramped against the window's right edge.
    const QSize hint = sizeHint();
    resize(static_cast<int>(hint.width() * 1.2), hint.height());
}

void CameraConfigDialog::onCountChanged(int) { updateRowVisibility(); }

void CameraConfigDialog::updateResolvedLabel(int row) {
    int camId = rows_[row].camIdSpin->value();
    auto it = resolvedGeometry_.find(camId);
    if (it == resolvedGeometry_.end()) {
        rows_[row].resolvedLabel->setText(tr("(unresolved)"));
        rows_[row].resolvedLabel->setStyleSheet(QStringLiteral("color: gray;"));
        return;
    }
    if (!it->ok) {
        rows_[row].resolvedLabel->setText(tr("unknown"));
        rows_[row].resolvedLabel->setStyleSheet(QStringLiteral("color: gray;"));
        return;
    }
    rows_[row].resolvedLabel->setStyleSheet(QString());
    rows_[row].resolvedLabel->setText(it->wasFallback
                                           ? tr("%1x%2 (from another cam)").arg(it->width).arg(it->height)
                                           : tr("%1x%2").arg(it->width).arg(it->height));
}

void CameraConfigDialog::updateRowVisibility() {
    const int count = countSpin_->value();
    for (int i = 0; i < camsyringe::kMaxCameras; ++i) {
        rows_[i].container->setVisible(i < count);
    }
    // Belt-and-suspenders: explicitly re-request the window's size after
    // newly-visible rows change the layout's preferred size, rather than
    // relying on the layout's own invalidation to reach the window
    // manager implicitly. NOTE: tested against a real modal, parented
    // exec() (matching MainWindow's own usage) on this project's own dev
    // machine/window manager and the window resized correctly even
    // WITHOUT this call -- so this alone does not explain a real report
    // of "increasing camera count has no visible effect" seen on a
    // different machine; kept as cheap insurance, not the fix.
    adjustSize();
}

void CameraConfigDialog::onBrowseClicked(int row) {
    QString file = QFileDialog::getOpenFileName(this, tr("Select video file"), BrowseStartDir(),
                                                 tr("Video files (*.mp4 *.mkv *.mov *.avi *.webm);;"
                                                    "All files (*)"));
    if (!file.isEmpty()) {
        rows_[row].pathEdit->setText(file);
        RememberBrowsedFile(file);
    }
}

void CameraConfigDialog::onSshKeyBrowseClicked() {
    // Default to ~/.ssh (the conventional location) when nothing's set
    // yet, rather than the video-file browse dir -- an SSH key is rarely
    // sitting next to a camera capture file. No filename filter -- key
    // files don't have a consistent extension (id_rsa, id_ed25519, a
    // board-specific .pem, ...).
    QString startDir = sshKeyPathEdit_->text().trimmed().isEmpty()
                            ? QDir::homePath() + "/.ssh"
                            : QFileInfo(sshKeyPathEdit_->text().trimmed()).absolutePath();
    if (!QDir(startDir).exists()) {
        startDir = QDir::homePath();
    }
    QString file = QFileDialog::getOpenFileName(this, tr("Select SSH Private Key"), startDir);
    if (!file.isEmpty()) {
        sshKeyPathEdit_->setText(file);
    }
}

void CameraConfigDialog::onBlfBrowseClicked() {
    QString file = QFileDialog::getOpenFileName(this, tr("Select BLF file"), BrowseStartDir(),
                                                 tr("Vector BLF files (*.blf);;All files (*)"));
    if (!file.isEmpty()) {
        blfPathEdit_->setText(file);
        RememberBrowsedFile(file);
    }
}

void CameraConfigDialog::onBlfEnabledChanged(int) {
    blfRowContainer_->setVisible(blfEnabledCheck_->isChecked());
}

void CameraConfigDialog::onAccept() {
    if (targetEdit_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Missing target"), tr("Enter a target host."));
        return;
    }
    if (sshUserEdit_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Missing SSH user"), tr("Enter an SSH username (e.g. root)."));
        return;
    }
    std::set<int> seenIds;
    for (int i = 0; i < countSpin_->value(); ++i) {
        if (rows_[i].pathEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Missing video file"),
                                  tr("Select a video file for camera %1.").arg(i));
            return;
        }
        int camId = rows_[i].camIdSpin->value();
        if (!seenIds.insert(camId).second) {
            QMessageBox::warning(
                this, tr("Duplicate camera ID"),
                tr("Camera ID %1 is used by more than one camera -- each must be unique "
                   "(the target rejects a declaration with a duplicate id).")
                    .arg(camId));
            return;
        }
    }
    if (blfEnabledCheck_->isChecked()) {
        if (blfPathEdit_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Missing BLF file"),
                                  tr("Select a BLF file, or uncheck \"Replay BLF Ethernet capture\"."));
            return;
        }
        if (blfInterfaceEdit_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, tr("Missing network interface"),
                                  tr("Enter a network interface (e.g. eth0) to replay onto."));
            return;
        }
    }
    accept();
}

QString CameraConfigDialog::target() const { return targetEdit_->text().trimmed(); }

int CameraConfigDialog::controlPort() const { return controlPortSpin_->value(); }

QString CameraConfigDialog::sshUser() const { return sshUserEdit_->text().trimmed(); }

QString CameraConfigDialog::sshKeyPath() const { return sshKeyPathEdit_->text().trimmed(); }

QStringList CameraConfigDialog::videoFiles() const {
    QStringList files;
    for (int i = 0; i < countSpin_->value(); ++i) {
        files << rows_[i].pathEdit->text().trimmed();
    }
    return files;
}

std::vector<int> CameraConfigDialog::camIds() const {
    std::vector<int> ids;
    for (int i = 0; i < countSpin_->value(); ++i) {
        ids.push_back(rows_[i].camIdSpin->value());
    }
    return ids;
}

bool CameraConfigDialog::injectOnly() const { return injectOnlyCheck_->isChecked(); }

QString CameraConfigDialog::blfPath() const {
    return blfEnabledCheck_->isChecked() ? blfPathEdit_->text().trimmed() : QString();
}

QString CameraConfigDialog::blfInterface() const { return blfInterfaceEdit_->text().trimmed(); }

} // namespace camsyringe::ui

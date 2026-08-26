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
constexpr int kMinCamId = 1;
constexpr int kMaxCamId = 16; // this target's allcamtest range, see qcarcam-injector/ARCHITECTURE.md

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
                                        const QStringList& initialFiles,
                                        const std::vector<int>& initialCamIds, bool initialInjectOnly,
                                        bool initialQcxBypass, const QString& initialBlfPath,
                                        const QString& initialBlfInterface, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Configure"));

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
        if (i < initialFiles.size()) {
            rows_[i].pathEdit->setText(initialFiles[i]);
        }
        rows_[i].browseButton = new QPushButton(tr("Browse..."), rows_[i].container);
        connect(rows_[i].browseButton, &QPushButton::clicked, this,
                [this, i]() { onBrowseClicked(i); });

        auto* camIdLabel = new QLabel(tr("Cam ID:"), rows_[i].container);
        rows_[i].camIdSpin = new QSpinBox(rows_[i].container);
        rows_[i].camIdSpin->setRange(kMinCamId, kMaxCamId);
        // Default 1,2,3,4 for a row with no prior value -- a generic
        // placeholder, not tied to any specific board's actual working
        // ids (those vary per target, see qcarcam-injector's own
        // allcamtest output) -- the user is expected to set real values.
        rows_[i].camIdSpin->setValue(static_cast<int>(i) < static_cast<int>(initialCamIds.size())
                                          ? initialCamIds[static_cast<size_t>(i)]
                                          : i + 1);

        rowLayout->addWidget(rows_[i].pathEdit, /*stretch=*/1);
        rowLayout->addWidget(rows_[i].browseButton);
        rowLayout->addWidget(camIdLabel);
        rowLayout->addWidget(rows_[i].camIdSpin);

        form->addRow(tr("Camera %1 video:").arg(i), rows_[i].container);
    }

    injectOnlyCheck_ = new QCheckBox(
        tr("Inject only (no local preview render on target -- see qcarcam_dispatcher's --inject-only)"),
        this);
    injectOnlyCheck_->setChecked(initialInjectOnly);
    rootLayout->addWidget(injectOnlyCheck_);

    qcxBypassCheck_ =
        new QCheckBox(tr("QCX bypass (diagnostic -- skip qcxserver entirely on target, see "
                          "qcarcam_injector's --qcx-bypass)"),
                      this);
    qcxBypassCheck_->setChecked(initialQcxBypass);
    rootLayout->addWidget(qcxBypassCheck_);

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

    updateRowVisibility();
}

void CameraConfigDialog::onCountChanged(int) { updateRowVisibility(); }

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

bool CameraConfigDialog::qcxBypass() const { return qcxBypassCheck_->isChecked(); }

QString CameraConfigDialog::blfPath() const {
    return blfEnabledCheck_->isChecked() ? blfPathEdit_->text().trimmed() : QString();
}

QString CameraConfigDialog::blfInterface() const { return blfInterfaceEdit_->text().trimmed(); }

} // namespace camsyringe::ui

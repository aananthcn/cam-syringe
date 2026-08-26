#include "ui/CameraConfigDialog.h"

#include <algorithm>
#include <set>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
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
} // namespace

CameraConfigDialog::CameraConfigDialog(const QString& initialTarget, int initialControlPort,
                                        const QStringList& initialFiles,
                                        const std::vector<int>& initialCamIds, bool initialInjectOnly,
                                        bool initialQcxBypass, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Configure"));

    auto* rootLayout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    rootLayout->addLayout(form);

    targetEdit_ = new QLineEdit(initialTarget, this);
    form->addRow(tr("Target:"), targetEdit_);

    controlPortSpin_ = new QSpinBox(this);
    controlPortSpin_->setRange(1, 65535);
    controlPortSpin_->setValue(initialControlPort);
    form->addRow(tr("Control port:"), controlPortSpin_);

    countSpin_ = new QSpinBox(this);
    countSpin_->setRange(1, camsyringe::kMaxCameras);
    countSpin_->setValue(initialFiles.isEmpty()
                              ? 1
                              : std::min(static_cast<int>(initialFiles.size()), camsyringe::kMaxCameras));
    form->addRow(tr("Number of cameras:"), countSpin_);
    connect(countSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &CameraConfigDialog::onCountChanged);

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
}

void CameraConfigDialog::onBrowseClicked(int row) {
    QString file = QFileDialog::getOpenFileName(this, tr("Select video file"), QString(),
                                                 tr("Video files (*.mp4 *.mkv *.mov *.avi *.webm);;"
                                                    "All files (*)"));
    if (!file.isEmpty()) {
        rows_[row].pathEdit->setText(file);
    }
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

} // namespace camsyringe::ui

#include "ui/CameraSettingsDialog.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace camsyringe::ui {

CameraSettingsDialog::CameraSettingsDialog(
    camsyringe::TargetSsh& ssh, QString target, QString sshUser, QString sshKeyPath,
    std::vector<int> camIds, const QMap<int, camsyringe::ResolvedCameraGeometry>& initialResults,
    const QDateTime& initialReadTime, QWidget* parent)
    : QDialog(parent),
      ssh_(ssh),
      target_(std::move(target)),
      sshUser_(std::move(sshUser)),
      sshKeyPath_(std::move(sshKeyPath)),
      camIds_(std::move(camIds)) {
    setWindowTitle(tr("Camera Configurations"));

    // Ascending + deduplicated once here, not re-derived per Read click --
    // CameraGeometryResolver::resolveAsync()'s fallback semantics depend
    // on ascending order (see its own comment), and a duplicate id would
    // just resolve/display the same row twice for no reason.
    std::sort(camIds_.begin(), camIds_.end());
    camIds_.erase(std::unique(camIds_.begin(), camIds_.end()), camIds_.end());

    auto* rootLayout = new QVBoxLayout(this);

    table_ = new QTableWidget(static_cast<int>(camIds_.size()), 3, this);
    table_->setHorizontalHeaderLabels({tr("Camera ID"), tr("Resolution"), tr("Source")});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    rootLayout->addWidget(table_);

    // Bottom row: read-timestamp (or, during a Read, a progress bar in
    // that exact same spot -- see the header comment) at the far left,
    // Read/Cancel/OK at the right with Read immediately left of Cancel
    // (user-specified order).
    auto* bottomRow = new QHBoxLayout();
    readTimeLabel_ = new QLabel(this);
    bottomRow->addWidget(readTimeLabel_);
    progressBar_ = new QProgressBar(this);
    progressBar_->setTextVisible(true);
    progressBar_->hide();
    bottomRow->addWidget(progressBar_);
    bottomRow->addStretch(1);

    readButton_ = new QPushButton(style()->standardIcon(QStyle::SP_FileIcon), tr("Read"), this);
    connect(readButton_, &QPushButton::clicked, this, &CameraSettingsDialog::onReadClicked);
    bottomRow->addWidget(readButton_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    bottomRow->addWidget(buttons);

    rootLayout->addLayout(bottomRow);

    populateTable(initialResults);
    setReadTime(initialReadTime);

    resize(520, 320);
}

void CameraSettingsDialog::populateTable(const QMap<int, camsyringe::ResolvedCameraGeometry>& results) {
    for (int row = 0; row < static_cast<int>(camIds_.size()); ++row) {
        const int camId = camIds_[static_cast<size_t>(row)];
        table_->setItem(row, 0, new QTableWidgetItem(QString::number(camId)));

        auto it = results.find(camId);
        QString resolution = tr("(not read yet)");
        QString source = tr("—");
        if (it != results.end()) {
            if (it->ok) {
                resolution = tr("%1 x %2").arg(it->width).arg(it->height);
                source = it->wasFallback ? tr("Fallback (from another camera)") : tr("Target");
            } else {
                resolution = tr("Unknown");
                source = tr("Target (unresolved)");
            }
        }
        table_->setItem(row, 1, new QTableWidgetItem(resolution));
        table_->setItem(row, 2, new QTableWidgetItem(source));
    }
}

void CameraSettingsDialog::setReadTime(const QDateTime& when) {
    readTimeLabel_->setText(when.isValid() ? tr("Last read: %1").arg(when.toString(Qt::TextDate))
                                            : tr("Last read: never"));
}

void CameraSettingsDialog::onReadClicked() {
    if (camIds_.empty()) {
        return; // nothing configured to read
    }
    readButton_->setEnabled(false);
    // Swap the timestamp label for the progress bar in the same bottom-
    // left spot -- a full sweep across every id can take tens of seconds
    // (see CameraGeometryResolver::ProgressCallback's own comment), so a
    // static "Reading..." label alone left no visible sign it was still
    // actually making progress rather than stuck (real user report).
    readTimeLabel_->hide();
    progressBar_->setRange(0, static_cast<int>(camIds_.size()));
    progressBar_->setValue(0);
    progressBar_->setFormat(tr("Reading target… %v/%m"));
    progressBar_->show();

    // QPointer, not a raw `this` capture: this dialog is modal but can
    // still be closed (Cancel/OK/window-close) while this background
    // resolve is in flight -- CameraGeometryResolver's own contract runs
    // both callbacks on ITS OWN thread regardless, so the marshaled
    // lambdas below must not touch a possibly-destroyed dialog.
    QPointer<CameraSettingsDialog> guard(this);
    camsyringe::CameraGeometryResolver::resolveAsync(
        ssh_, target_, sshUser_, sshKeyPath_, camIds_,
        [guard, target = target_](std::vector<camsyringe::ResolvedCameraGeometry> results) {
            const QDateTime now = QDateTime::currentDateTime();
            QMetaObject::invokeMethod(
                qApp,
                [guard, target, results, now]() {
                    if (!guard) {
                        return; // dialog closed/destroyed before this ran
                    }
                    QMap<int, camsyringe::ResolvedCameraGeometry> byId;
                    for (const auto& r : results) {
                        byId[r.camId] = r;
                    }
                    guard->populateTable(byId);
                    guard->progressBar_->hide();
                    guard->readTimeLabel_->show();
                    guard->setReadTime(now);
                    guard->readButton_->setEnabled(true);
                    emit guard->geometryResolved(target, results, now);
                },
                Qt::QueuedConnection);
        },
        [guard](int completed, int total) {
            QMetaObject::invokeMethod(
                qApp,
                [guard, completed, total]() {
                    if (!guard) {
                        return; // dialog closed/destroyed before this ran
                    }
                    guard->progressBar_->setRange(0, total);
                    guard->progressBar_->setValue(completed);
                },
                Qt::QueuedConnection);
        });
}

} // namespace camsyringe::ui

#include "ui/CameraWidget.h"

#include <QLabel>
#include <QPixmap>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace camsyringe::ui {

CameraWidget::CameraWidget(int index, const QString& title, QWidget* parent)
    : QWidget(parent), index_(index) {
    titleLabel_ = new QLabel(title, this);
    titleLabel_->setStyleSheet("font-weight: bold; padding: 2px;");

    videoLabel_ = new QLabel(this);
    videoLabel_->setAlignment(Qt::AlignCenter);
    videoLabel_->setMinimumSize(160, 90);
    videoLabel_->setStyleSheet("background-color: black; color: #888;");
    videoLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    statusLabel_ = new QLabel(this);
    statusLabel_->setStyleSheet("padding: 2px;");
    statusLabel_->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(titleLabel_);
    layout->addWidget(videoLabel_, /*stretch=*/1);
    layout->addWidget(statusLabel_);

    resetIdle();
}

void CameraWidget::resetIdle() {
    lastFrame_ = QImage();
    videoLabel_->setPixmap(QPixmap());
    videoLabel_->setText(tr("Press Start"));
}

void CameraWidget::updateFrame(const QImage& frame) {
    lastFrame_ = frame;
    refreshPixmap();
}

void CameraWidget::refreshPixmap() {
    if (lastFrame_.isNull()) {
        return;
    }
    videoLabel_->setPixmap(QPixmap::fromImage(lastFrame_).scaled(
        videoLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void CameraWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refreshPixmap();
}

void CameraWidget::showError(const QString& message) {
    statusLabel_->setStyleSheet("color: #e05050; padding: 2px;");
    statusLabel_->setText(message);
    statusLabel_->show();
}

void CameraWidget::showStatus(const QString& message) {
    statusLabel_->setStyleSheet("color: #888; padding: 2px;");
    statusLabel_->setText(message);
    statusLabel_->show();
}

void CameraWidget::clearError() { statusLabel_->hide(); }

} // namespace camsyringe::ui

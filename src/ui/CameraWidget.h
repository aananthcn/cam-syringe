#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

class QLabel;

namespace camsyringe::ui {

// One camera's tile in MainWindow's grid: a title bar, the latest preview
// frame (or an idle/error placeholder), and a status line. All methods
// must be called on the GUI thread.
class CameraWidget : public QWidget {
    Q_OBJECT

public:
    explicit CameraWidget(int index, const QString& title, QWidget* parent = nullptr);

    void updateFrame(const QImage& frame);
    void showError(const QString& message);
    // Same status line as showError(), but neutral-colored -- for
    // transient, non-error states (e.g. "Connecting to target...") that
    // shouldn't read as a problem.
    void showStatus(const QString& message);
    void clearError();
    void resetIdle();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void refreshPixmap();

    int index_;
    QLabel* titleLabel_ = nullptr;
    QLabel* videoLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QImage lastFrame_;
};

} // namespace camsyringe::ui

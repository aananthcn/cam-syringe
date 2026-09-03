#include "ui/SshCredentialsDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace camsyringe::ui {

SshCredentialsDialog::SshCredentialsDialog(const QString& target, const QString& initialUsername,
                                            QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Target Login Required"));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->addWidget(new QLabel(
        tr("Passwordless SSH to %1 failed. Enter login credentials to continue the install.")
            .arg(target)));

    auto* form = new QFormLayout();
    rootLayout->addLayout(form);

    usernameEdit_ = new QLineEdit(initialUsername.isEmpty() ? QStringLiteral("root") : initialUsername,
                                   this);
    form->addRow(tr("Username:"), usernameEdit_);

    passwordEdit_ = new QLineEdit(this);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Password:"), passwordEdit_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rootLayout->addWidget(buttons);
}

QString SshCredentialsDialog::username() const { return usernameEdit_->text(); }

QString SshCredentialsDialog::password() const { return passwordEdit_->text(); }

} // namespace camsyringe::ui

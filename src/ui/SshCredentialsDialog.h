#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;

namespace camsyringe::ui {

// Fallback credentials prompt for InjectorBundleInstaller, shown only
// when passwordless SSH (root/key auth) to the target fails -- see
// InjectorBundleInstaller's own class comment for the full auth flow.
class SshCredentialsDialog : public QDialog {
    Q_OBJECT

public:
    // initialUsername pre-fills the username field -- MainWindow passes
    // its current sshUser_ (from Configure's "SSH user" field / --target
    // user@host), so a non-default username doesn't have to be retyped
    // here just because passwordless auth happened to fail.
    explicit SshCredentialsDialog(const QString& target, const QString& initialUsername,
                                   QWidget* parent = nullptr);

    QString username() const;
    QString password() const;

private:
    QLineEdit* usernameEdit_ = nullptr;
    QLineEdit* passwordEdit_ = nullptr;
};

} // namespace camsyringe::ui

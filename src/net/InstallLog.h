#pragma once

#include <QString>

namespace camsyringe {

// One timestamped line to the PC terminal (stderr) AND appended to
// ~/.cache/camsyringe/install.log, so a failed install or dispatcher start on someone else's
// machine can be diagnosed from a file they can send, not from terminal scrollback. Thread-safe
// (the install runs on its own thread). `tag` says which flow wrote it, e.g. "install".
void installLog(const QString& tag, const QString& text);

// The PC-side facts that differ between setups and decide how ssh/scp behave: OS and kernel,
// whether this is WSL, the ssh and scp client versions and paths, and the user. One log block.
QString describePcEnvironment();

} // namespace camsyringe

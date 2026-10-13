#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace camsyringe {

// Where CamSyringe keeps the small JSON files it remembers between runs (camera_injection.json,
// camera_configs.json) -- docs/adr/0013.
//
// The files used to live in QCoreApplication::applicationDirPath(), but for an INSTALLED copy that is
// <install-dir>/bin, which the bundle installer deletes and recreates on every reinstall, taking the
// remembered configuration with it. So the candidates, most preferred first, are:
//   1. the install directory itself (the parent of bin/, recognised by run-camsyringe.sh sitting there),
//      which the installer leaves alone; for a development build with no such layout, the binary's own
//      directory;
//   2. the per-user cache directory (~/.cache/camsyringe on Linux), which needs no particular install
//      layout and survives reinstalling or even deleting the install directory.
// The files are meant to outlive any one build: the location doesn't depend on the version, a newer or
// older release reads what another wrote (see SessionConfigStore/CameraConfigStore: a schema version, and
// unknown keys kept), and the installer must never delete them.
// CAMSYRINGE_CONFIG_DIRS (a ':'-separated list of directories) replaces the list outright, for tests and
// for anyone who wants the files somewhere specific.
class ConfigFiles {
public:
    static QStringList candidateDirs();

    // The candidates plus the legacy location (the binary's own directory) when it isn't already one.
    // Only ever READ from: it is where earlier releases kept these files, so a configuration saved
    // by one is still found after upgrading. The next save writes it to a candidate dir.
    static QStringList readDirs();

    // The path `name` would be written to first: <first candidate>/<name>. Informational.
    static QString primaryPath(const QString& name);

    // Writes `bytes` as `name` into every candidate that will take it (atomically, creating the
    // directory if needed): the redundancy is deliberate, so losing one location loses nothing.
    // Returns true if at least one write succeeded. `written` receives the paths written, `errors` one
    // line per candidate that failed (both optional).
    static bool write(const QString& name, const QByteArray& bytes, QStringList* written = nullptr,
                      QStringList* errors = nullptr);

    // Every existing copy of `name`, newest first, so a reader can fall through to the next if the
    // newest doesn't parse. Empty if none exists.
    static QList<QByteArray> readAll(const QString& name, QStringList* paths = nullptr);
};

} // namespace camsyringe

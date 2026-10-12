#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace camsyringe {

// What an injector bundle says it contains: a build id and an MD5 for each of
// the files the project itself builds (see create-qcarcam-inj-bundle.py's
// MANIFEST_FILES), read from the plain-text comment lines at the top of the
// bundle's installer header -- no unpacking needed. Used to tell whether what
// is installed on a target is the bundle this PC has (docs/adr/0014).
struct InjectorManifest {
    bool valid = false;          // false: no manifest lines found (an older bundle) or unreadable
    QString buildId;             // first 12 hex digits of the md5 of the manifest text
    QString installDir;          // where the bundle installs by default, e.g. /var/opt
    QStringList paths;           // relative paths, in manifest order
    QMap<QString, QString> md5;  // relative path -> lowercase md5 hex
};

// Result of comparing a manifest to what is actually on a target.
struct InstallDiff {
    QStringList differing;  // present on the target, but with different content
    QStringList missing;    // in the manifest, absent on the target
    bool matches() const { return differing.isEmpty() && missing.isEmpty(); }
};

class InjectorManifestReader {
public:
    // Reads the header of `bundlePath`. Returns valid == false (never an
    // error) for a missing file, a bundle without manifest lines, or one whose
    // paths aren't plain relative names (they end up in a shell command, so
    // anything else is rejected outright).
    static InjectorManifest read(const QString& bundlePath);

    // The shell command that prints "<md5>  <path>" for each manifest file on
    // the target, from inside the install directory. A missing install
    // directory or file simply produces no line for it.
    static QString verifyCommand(const InjectorManifest& manifest);

    // Compares md5sum's output (lines "<32 hex>  <path>") with the manifest.
    // Anything in the manifest with no matching output line counts as missing.
    static InstallDiff compare(const InjectorManifest& manifest, const QString& md5sumOutput);
};

} // namespace camsyringe

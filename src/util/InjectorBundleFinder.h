#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace camsyringe {

// Locates the qcarcam_injector_bundle_vX.Y.bin this PC has available to
// push to a target (see release/create-windows-bundle.sh and
// ~/labs/qnx/qnx_toolkit/release/create-qcarcam-inj-bundle.sh, which
// builds it) -- NOT a source-tree sibling of this repo (it's built by a
// different, unrelated project), but meant to travel as a sibling of
// wherever the CAMSYRINGE bundle itself ends up (see release/README.md's
// "Packaging" notes): dropped into this repo's own release/artifacts/
// for local dev, or packed alongside camsyringe's own install for a
// teammate. Checks a few built-in candidate directories first (so a
// normal dev/install layout needs zero configuration) -- including
// wherever the camsyringe_bundle_vX.Y.bin that installed this copy was
// actually run from, recorded by create-cam-syringe-bundle.sh's own
// installer into a ".install-source" marker in the install directory --
// then falls back to directories the user has pointed at manually before
// (persisted via QSettings) -- see rememberDir(). Never guesses between
// multiple
// matches; callers fall back to a file picker in that case (see
// MainWindow's "Install Injector" action).
class InjectorBundleFinder {
public:
    // Returns the bundle path only if exactly one
    // qcarcam_injector_bundle_v*.bin exists across every candidate
    // directory (built-in + learned). nullopt otherwise (none found, or
    // more than one -- ambiguous).
    static std::optional<QString> findBundle();

    // "0.5" from ".../qcarcam_injector_bundle_v0.5.bin"; empty if the
    // filename doesn't match the expected pattern.
    static QString extractVersion(const QString& bundlePath);

    // Adds `dir` to the persisted list of known bundle directories (no-op
    // if already present, whether learned before or already a built-in
    // candidate). Call after a user manually picks a bundle via the file
    // picker so future findBundle() calls succeed without prompting again.
    static void rememberDir(const QString& dir);

    // Exposed for MainWindow's file-picker fallback, so it can default to
    // the most promising directory instead of always starting at $HOME.
    static QStringList candidateDirs();
};

} // namespace camsyringe

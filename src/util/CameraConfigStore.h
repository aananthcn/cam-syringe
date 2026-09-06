#pragma once

#include <QDateTime>
#include <QMap>
#include <QString>

#include "net/CameraGeometryResolver.h"

namespace camsyringe {

// Persists MainWindow's geometryCache_/geometryReadTimestamps_ (each
// configured target's per-camera-id resolved resolution, from "Settings
// > CamSyringe Configurations"'s automatic resolve and "Settings >
// Camera Configs"'s explicit Read) to a plain JSON file next to the
// running binary -- explicit requirement: readable/editable without
// digging into an OS-specific config location (QSettings, used
// elsewhere in this app for genuinely app-preference-shaped data like
// InjectorBundleFinder's learned directories, is deliberately NOT reused
// here), and surviving an app restart, which it didn't before this
// (both maps were purely in-memory, so a fresh launch always started
// from an empty cache despite the target's own configuration not having
// changed).
class CameraConfigStore {
public:
    // QCoreApplication::applicationDirPath()/camera_configs.json -- the
    // directory the running binary itself lives in (this app's own
    // "installation path", not a subdirectory or the install root one
    // level up), matching where a teammate would actually look for it
    // sitting next to camsyringe/camsyringe.exe.
    static QString filePath();

    // Overwrites filePath() with the ENTIRE current state of both maps
    // (not a partial merge) -- called right after every successful
    // resolve, from both call sites that update geometryCache_/
    // geometryReadTimestamps_, so a crash or unexpected close never
    // loses a read that already completed. Best-effort: a write failure
    // (e.g. read-only install directory) is logged to stderr, never
    // surfaced as a user-facing error -- this is a convenience cache,
    // not something Play/Configure should ever depend on succeeding.
    static void save(const QMap<QString, QMap<int, ResolvedCameraGeometry>>& cache,
                      const QMap<QString, QDateTime>& readTimestamps);

    // Reads filePath() back into *cache/*readTimestamps (cleared first,
    // then populated -- never merged with whatever the caller already
    // had). Leaves both empty (not an error) if the file doesn't exist
    // yet (first-ever launch) or fails to parse -- same "cache, not a
    // dependency" philosophy as save().
    static void load(QMap<QString, QMap<int, ResolvedCameraGeometry>>* cache,
                      QMap<QString, QDateTime>* readTimestamps);
};

} // namespace camsyringe

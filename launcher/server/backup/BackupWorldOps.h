// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Private helpers for ServerBackups / RestoreBackupTask / WorldImportTask: the verify -> extract/copy -> check -> swap
// pipeline. Everything in here is plain blocking code without GUI objects, so it can run on a worker thread.

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <functional>

namespace ServerBackupOps {

/// Progress sink for worker-thread code (the task glue forwards it to Task::setStatus/setProgress on the GUI thread).
struct Progress {
    std::function<void(const QString&)> status;
    std::function<void(qint64, qint64)> progress;
    void setStatus(const QString& text) const
    {
        if (status)
            status(text);
    }
    void setProgress(qint64 current, qint64 total) const
    {
        if (progress)
            progress(current, total);
    }
};

/// "22AD67686eb0..." / "{...}" / dashed -> lowercase dashed uuid; empty if not a uuid
QString normalizeUuid(const QString& uuid);

/// worldId ServerUtilities stores in <worldDir>/serverutilities/universe.dat (root string tag "UUID"), normalized.
QString readWorldId(const QString& worldDir);
/// same, from the raw (possibly gzipped) universe.dat bytes
QString worldIdFromUniverseDat(const QByteArray& data);

/// ServerUtilities .serverutilities-backup.properties
struct SuProperties {
    bool valid = false;
    QDateTime created;
    bool customName = false;
    QString worldId;
};
SuProperties parseSuProperties(const QByteArray& data);
inline constexpr const char* suPropertiesName = ".serverutilities-backup.properties";

/// Result of reading a whole world archive.
struct ZipScan {
    QString error;              // non-empty = unusable
    QString worldPrefix;        // entries starting with this are the world ("" = world at the archive root)
    qint64 worldBytes = 0;      // uncompressed size of the world entries
    int entries = 0;            // all entries
    int worldFiles = 0;         // file entries inside the world
    QStringList outsideWorld;   // entries that are not part of the world (skipped on extract)
    SuProperties properties;    // .serverutilities-backup.properties, if present
    QString worldId;            // properties worldId, else from the world's serverutilities/universe.dat
};

/// Opens the archive and reads every entry's data (libarchive checks the zip CRCs while reading), validates every path
/// (no absolute paths, no "..") and finds the world folder inside (the folder containing level.dat).
ZipScan verifyArchive(const QString& archivePath, const Progress& progress);

/// Extracts the world entries (scan.worldPrefix stripped) into targetDir (must exist and be empty). Returns an error
/// message, empty on success.
QString extractWorld(const QString& archivePath, const ZipScan& scan, const QString& targetDir, const Progress& progress);

/// The world inside a folder: the folder itself if it has a level.dat, else its only subfolder that has one.
QString findWorldRoot(const QString& folder, QString* error = nullptr);
/// Sum of all file sizes below path.
qint64 folderSize(const QString& path, int* fileCount = nullptr);
/// Copies a world folder (files and folders, symlinks skipped) to dst (must exist and be empty). Error message or empty.
QString copyFolder(const QString& src, const QString& dst, qint64 totalBytes, const Progress& progress);

/// Error message if the volume of dir doesn't have neededBytes free (plus margin), else empty.
QString checkFreeSpace(const QString& dir, qint64 neededBytes);

/// Light sanity check of an extracted world: level.dat exists and parses. Error message or empty.
QString checkWorld(const QString& worldDir);

/// Singleplayer -> server: moves level.dat Data/Player to playerdata/<uuid>.dat (dashed) and removes it from level.dat.
/// Returns an error message (empty on success; "nothing to do" is success).
QString moveHostPlayer(const QString& worldDir, const QString& playerUuid);

/// <serverDir>/elytra-pre-restore
QString preRestoreRoot(const QString& serverDir);
/// whether a folder name looks like one of our pre-restore timestamps (yyyy-MM-dd-HH-mm-ss[-n])
bool isPreRestoreName(const QString& name);
QDateTime preRestoreTime(const QString& name);

/// Everything needed to replace a server's world. Built on the GUI thread, used on the worker.
struct ReplaceRequest {
    enum class Source { Archive, Folder };
    Source source = Source::Archive;
    QString sourcePath;
    QString serverDir;
    QString worldDir;
    bool checkWorldId = false;
    bool allowWorldMismatch = false;
    QString playerUuid;  // non-empty: move the singleplayer host's data to playerdata/
    QString what;        // "restore" / "import", for logs
};

struct ReplaceResult {
    bool ok = false;
    QString error;
    bool worldMismatch = false;
    QString stagingDir;      // after prepare: the ready world
    QString preRestorePath;  // after swap: where the previous world went (empty if there was none)
};

/// Steps 1-3 (+ host player fix): verify, worldId check, space check, extract/copy into <world>.elytra-restoring,
/// sanity check. Never touches the current world. On failure the staging folder is removed.
ReplaceResult prepare(const ReplaceRequest& request, const Progress& progress);
/// Steps 4-5: current world -> elytra-pre-restore/<timestamp>/<world>, staging -> world, rollback on failure, prune.
ReplaceResult swap(const ReplaceRequest& request, const QString& stagingDir, const Progress& progress);
/// Removes a staging folder (only if it is really one of ours).
void discardStaging(const QString& stagingDir);

inline constexpr int keepPreRestoreCopies = 3;

}  // namespace ServerBackupOps

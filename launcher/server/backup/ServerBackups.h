// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include "tasks/Task.h"

class ServerInstance;

/// A restorable backup of a server's world.
struct ServerBackup {
    enum class Kind {
        ServerUtilities,  // zip made by the ServerUtilities mod (has .serverutilities-backup.properties)
        PreRestore,       // folder Elytra saved right before a restore (<serverDir>/elytra-pre-restore/<timestamp>/)
        Zip,              // any other world zip the user added
    };
    Kind kind = Kind::Zip;
    QString path;  // zip file or folder
    QDateTime created;
    qint64 sizeBytes = 0;
    QString customName;  // ServerUtilities "/backup start <name>"
    QString worldId;     // ServerUtilities worldId (empty if unknown)
};

namespace ServerBackups {
/// Backup folder ServerUtilities writes to for this server: S:backup_folder_path from
/// <serverDir>/serverutilities/serverutilities.cfg resolved against serverDir (default "./backups/").
QString serverUtilitiesFolder(ServerInstance* server);
/// Whether the ServerUtilities mod is installed (a ServerUtilities*.jar in <serverDir>/mods).
bool hasServerUtilities(ServerInstance* server);
/// All backups for this server, newest first: ServerUtilities zips + Elytra pre-restore copies.
QList<ServerBackup> list(ServerInstance* server);
/// worldId of the server's current world as ServerUtilities records it (empty if unknown).
QString currentWorldId(ServerInstance* server);
}  // namespace ServerBackups

/// Restores a backup over the server's world. Refuses to start while the server process is active.
/// Steps (each must succeed before the next touches anything):
///   1. verify: open the archive and read every entry (catches corrupt/truncated zips); check free disk space
///   2. extract into <serverDir>/<world>.elytra-restoring (paths remapped: "saves/<any name>/..." or "<any top folder>/..."
///      -> world root, so singleplayer backups restore onto a server)
///   3. sanity check: level.dat exists in the extracted world; worldId matches unless allowWorldMismatch
///   4. move the current world to <serverDir>/elytra-pre-restore/<yyyy-MM-dd-HH-mm-ss>/<world> (same volume rename)
///   5. rename the extracted folder to the world folder
///   any failure after 4 moves the original world back. Keeps the newest 3 pre-restore copies.
class RestoreBackupTask : public Task {
    Q_OBJECT
   public:
    RestoreBackupTask(ServerInstance* server, const ServerBackup& backup, bool allowWorldMismatch = false);

    /// set when the task failed because of a worldId mismatch (UI can then ask and retry with allowWorldMismatch)
    bool failedOnWorldMismatch() const { return m_worldMismatch; }

   protected:
    void executeTask() override;

   private:
    ServerInstance* m_server;
    ServerBackup m_backup;
    bool m_allowWorldMismatch;
    bool m_worldMismatch = false;
};

/// Imports a world (a world folder, e.g. a singleplayer save, or a world/backup zip) as the server's world.
/// Uses the same safe extract -> check -> swap as RestoreBackupTask (the existing world, if any, goes to
/// elytra-pre-restore). Then, for singleplayer worlds: moves level.dat's Data/Player compound into
/// <world>/playerdata/<playerUuid>.dat (dashed UUID) and removes it from level.dat, so the host keeps their inventory
/// and position on the server. Skipped if playerUuid is empty or the world has no Data/Player tag.
/// Refuses to start while the server process is active.
class WorldImportTask : public Task {
    Q_OBJECT
   public:
    WorldImportTask(ServerInstance* server, const QString& source, const QString& playerUuid);

   protected:
    void executeTask() override;

   private:
    ServerInstance* m_server;
    QString m_source;
    QString m_playerUuid;
};

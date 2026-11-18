// SPDX-License-Identifier: GPL-3.0-only
#include "server/backup/ServerBackups.h"

#include <QDebug>
#include <QFileInfo>

#include "server/ServerInstance.h"
#include "server/ServerProcess.h"
#include "server/backup/WorldReplace.h"

RestoreBackupTask::RestoreBackupTask(ServerInstance* server, const ServerBackup& backup, bool allowWorldMismatch)
    : m_server(server), m_backup(backup), m_allowWorldMismatch(allowWorldMismatch)
{}

void RestoreBackupTask::executeTask()
{
    m_worldMismatch = false;
    if (!m_server) {
        emitFailed(tr("No server selected."));
        return;
    }
    if (m_server->process() && m_server->process()->isActive()) {
        qWarning() << "[Server Backup] Refusing to restore while the server is running";
        emitFailed(tr("Stop the server before restoring a backup."));
        return;
    }
    const QFileInfo source(m_backup.path);
    if (!source.exists()) {
        emitFailed(tr("The backup %1 does not exist anymore.").arg(m_backup.path));
        return;
    }

    ServerBackupOps::ReplaceRequest request;
    request.source = source.isDir() ? ServerBackupOps::ReplaceRequest::Source::Folder : ServerBackupOps::ReplaceRequest::Source::Archive;
    request.sourcePath = source.absoluteFilePath();
    request.serverDir = m_server->serverDir();
    request.worldDir = m_server->worldDir();
    request.checkWorldId = true;
    request.allowWorldMismatch = m_allowWorldMismatch;
    request.what = "restore";

    qInfo().noquote() << "[Server Backup] Restoring" << request.sourcePath << "to" << request.worldDir
                      << (m_allowWorldMismatch ? "(world mismatch allowed)" : "");
    setStatus(tr("Checking the backup..."));
    setProgress(0, 1000);
    ServerBackupOps::runReplace(this, m_server, request, [this](const ServerBackupOps::ReplaceResult& result) {
        if (result.ok) {
            emitSucceeded();
            return;
        }
        m_worldMismatch = result.worldMismatch;
        qWarning().noquote() << "[Server Backup] Restore failed:" << result.error;
        emitFailed(result.error);
    });
}

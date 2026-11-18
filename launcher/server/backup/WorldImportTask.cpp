// SPDX-License-Identifier: GPL-3.0-only
#include "server/backup/ServerBackups.h"

#include <QDebug>
#include <QFileInfo>

#include "server/ServerInstance.h"
#include "server/ServerProcess.h"
#include "server/backup/WorldReplace.h"

WorldImportTask::WorldImportTask(ServerInstance* server, const QString& source, const QString& playerUuid)
    : m_server(server), m_source(source), m_playerUuid(playerUuid)
{}

void WorldImportTask::executeTask()
{
    if (!m_server) {
        emitFailed(tr("No server selected."));
        return;
    }
    if (m_server->process() && m_server->process()->isActive()) {
        qWarning() << "[Server Backup] Refusing to import a world while the server is running";
        emitFailed(tr("Stop the server before importing a world."));
        return;
    }
    const QFileInfo source(m_source);
    if (!source.exists()) {
        emitFailed(tr("%1 does not exist.").arg(m_source));
        return;
    }

    ServerBackupOps::ReplaceRequest request;
    request.source = source.isDir() ? ServerBackupOps::ReplaceRequest::Source::Folder : ServerBackupOps::ReplaceRequest::Source::Archive;
    request.sourcePath = source.absoluteFilePath();
    request.serverDir = m_server->serverDir();
    request.worldDir = m_server->worldDir();
    request.checkWorldId = false;  // importing a different world is the point
    request.playerUuid = m_playerUuid.trimmed();
    request.what = "import";

    qInfo().noquote() << "[Server Backup] Importing world" << request.sourcePath << "to" << request.worldDir << "host player"
                      << (request.playerUuid.isEmpty() ? "-" : request.playerUuid);
    if (request.playerUuid.isEmpty())
        qInfo() << "[Server Backup] No player UUID given: level.dat's Data/Player (singleplayer host) will not be moved";
    setStatus(tr("Checking the world..."));
    setProgress(0, 1000);
    ServerBackupOps::runReplace(this, m_server, request, [this](const ServerBackupOps::ReplaceResult& result) {
        if (result.ok) {
            emitSucceeded();
            return;
        }
        qWarning().noquote() << "[Server Backup] Import failed:" << result.error;
        emitFailed(result.error);
    });
}

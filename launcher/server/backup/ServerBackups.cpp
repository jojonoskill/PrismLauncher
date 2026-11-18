// SPDX-License-Identifier: GPL-3.0-only
#include "server/backup/ServerBackups.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

#include "archive/ArchiveReader.h"
#include "server/ServerInstance.h"
#include "server/backup/BackupWorldOps.h"

namespace ServerBackups {

namespace {

/// S:backup_folder_path from the "backups { ... }" block of a Forge config file; empty if not found.
QString readBackupFolderSetting(const QString& cfgPath)
{
    QFile file(cfgPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    int depth = 0;
    bool inBackups = false;
    int backupsDepth = -1;
    while (!file.atEnd()) {
        const auto line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (line.endsWith('{')) {
            depth++;
            if (depth == 1 && line.left(line.size() - 1).trimmed().compare("backups", Qt::CaseInsensitive) == 0) {
                inBackups = true;
                backupsDepth = depth;
            }
            continue;
        }
        if (line == "}") {
            if (inBackups && depth == backupsDepth)
                inBackups = false;
            depth = qMax(0, depth - 1);
            continue;
        }
        if (inBackups && depth == backupsDepth && line.startsWith("S:backup_folder_path=")) {
            return line.mid(QString("S:backup_folder_path=").size()).trimmed();
        }
    }
    return {};
}

}  // namespace

QString serverUtilitiesFolder(ServerInstance* server)
{
    const QDir serverDir(server->serverDir());
    QString setting;
    for (const auto& candidate : { QStringLiteral("serverutilities/serverutilities.cfg"), QStringLiteral("config/serverutilities/serverutilities.cfg"),
                                   QStringLiteral("config/serverutilities.cfg") }) {
        const auto path = serverDir.filePath(candidate);
        if (!QFileInfo::exists(path))
            continue;
        setting = readBackupFolderSetting(path);
        if (!setting.isEmpty())
            break;
    }
    if (setting.isEmpty())
        setting = "./backups/";
    return QDir::cleanPath(serverDir.absoluteFilePath(setting));
}

bool hasServerUtilities(ServerInstance* server)
{
    const QDir mods(QDir(server->serverDir()).filePath("mods"));
    const auto jars = mods.entryList({ "*.jar", "*.JAR" }, QDir::Files);
    return std::any_of(jars.begin(), jars.end(), [](const QString& name) { return name.startsWith("serverutilities", Qt::CaseInsensitive); });
}

QString currentWorldId(ServerInstance* server)
{
    return ServerBackupOps::readWorldId(server->worldDir());
}

QList<ServerBackup> list(ServerInstance* server)
{
    QList<ServerBackup> backups;

    // ServerUtilities zips (only the small properties entry is read; it is the first entry)
    const QDir suDir(serverUtilitiesFolder(server));
    if (suDir.exists()) {
        for (const auto& info : suDir.entryInfoList({ "*.zip", "*.ZIP" }, QDir::Files)) {
            ServerBackup backup;
            backup.path = info.absoluteFilePath();
            backup.sizeBytes = info.size();
            backup.created = info.lastModified();
            backup.kind = ServerBackup::Kind::Zip;

            MMCZip::ArchiveReader reader(backup.path);
            if (auto entry = reader.goToFile(ServerBackupOps::suPropertiesName)) {
                int status = 0;
                const auto data = entry->readAll(&status);
                const auto props = ServerBackupOps::parseSuProperties(data);
                if (props.valid) {
                    backup.kind = ServerBackup::Kind::ServerUtilities;
                    if (props.created.isValid())
                        backup.created = props.created;
                    if (props.customName)
                        backup.customName = info.completeBaseName();
                    backup.worldId = props.worldId;
                }
            }
            backups.append(backup);
        }
    }

    // Elytra's own copies made right before a restore
    const QDir preDir(ServerBackupOps::preRestoreRoot(server->serverDir()));
    if (preDir.exists()) {
        for (const auto& info : preDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!ServerBackupOps::isPreRestoreName(info.fileName()))
                continue;
            ServerBackup backup;
            backup.kind = ServerBackup::Kind::PreRestore;
            backup.path = info.absoluteFilePath();
            backup.created = ServerBackupOps::preRestoreTime(info.fileName());
            if (!backup.created.isValid())
                backup.created = info.lastModified();
            backup.sizeBytes = ServerBackupOps::folderSize(backup.path);
            const auto world = ServerBackupOps::findWorldRoot(backup.path);
            if (!world.isEmpty())
                backup.worldId = ServerBackupOps::readWorldId(world);
            backups.append(backup);
        }
    }

    std::stable_sort(backups.begin(), backups.end(), [](const ServerBackup& a, const ServerBackup& b) { return a.created > b.created; });
    return backups;
}

}  // namespace ServerBackups

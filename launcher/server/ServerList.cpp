// SPDX-License-Identifier: GPL-3.0-only
#include "server/ServerList.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QIcon>

#include <algorithm>

#include "FileSystem.h"
#include "server/ServerInstance.h"
#include "server/ServerProcess.h"

namespace {

QString idFromName(const QString& name)
{
    auto id = FS::RemoveInvalidFilenameChars(name.trimmed().toLower(), '-');
    id.replace(' ', '-');
    id.replace('\t', '-');
    // collapse runs of '-' and trim characters Windows doesn't like at the ends
    while (id.contains("--"))
        id.replace("--", "-");
    while (!id.isEmpty() && (id.endsWith('.') || id.endsWith('-')))
        id.chop(1);
    while (!id.isEmpty() && (id.startsWith('.') || id.startsWith('-')))
        id.remove(0, 1);
    static const QStringList reserved = { "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8",
                                          "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9" };
    if (id.isEmpty() || reserved.contains(id))
        id = "server" + (id.isEmpty() ? QString() : "-" + id);
    if (id.size() > 64)
        id = id.left(64);
    return id;
}

}  // namespace

ServerList::ServerList(const QString& serversDir, QObject* parent) : QAbstractListModel(parent), m_dir(QDir(serversDir).absolutePath()) {}

ServerList::~ServerList()
{
    // deleting the instances stops still running servers gracefully (see ~ServerProcess)
    auto servers = m_servers;
    m_servers.clear();
    qDeleteAll(servers);
}

void ServerList::load()
{
    QDir dir(m_dir);
    if (!dir.exists() && !dir.mkpath(".")) {
        qWarning() << "[Server] Could not create servers folder" << m_dir;
        return;
    }

    QStringList present;
    for (const auto& entry : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        if (QFileInfo::exists(FS::PathCombine(entry.absoluteFilePath(), "server.cfg")))
            present << entry.fileName();
    }

    beginResetModel();
    QStringList removedIds;
    for (auto it = m_servers.begin(); it != m_servers.end();) {
        auto server = *it;
        if (present.contains(server->id()) || server->process()->isActive()) {
            ++it;
            continue;
        }
        removedIds << server->id();
        it = m_servers.erase(it);
        server->deleteLater();
    }
    QList<ServerInstance*> added;
    for (const auto& id : present) {
        if (byId(id))
            continue;
        auto server = new ServerInstance(FS::PathCombine(m_dir, id), this);
        m_servers.append(server);
        watch(server);
        added << server;
    }
    std::sort(m_servers.begin(), m_servers.end(),
              [](ServerInstance* a, ServerInstance* b) { return a->name().compare(b->name(), Qt::CaseInsensitive) < 0; });
    endResetModel();

    for (const auto& id : removedIds)
        emit serverRemoved(id);
    for (auto server : added)
        emit serverAdded(server);
    qDebug() << "[Server] Loaded" << m_servers.size() << "servers from" << m_dir;
}

int ServerList::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_servers.size());
}

QVariant ServerList::data(const QModelIndex& index, int role) const
{
    auto server = at(index.row());
    if (!index.isValid() || !server)
        return {};
    switch (role) {
        case Qt::DisplayRole:
        case Qt::EditRole:
            return server->name();
        case Qt::ToolTipRole:
            return QDir::toNativeSeparators(server->serverDir());
        case Qt::DecorationRole:
            switch (server->process()->state()) {
                case ServerProcess::State::Starting:
                case ServerProcess::State::Stopping:
                    return QIcon::fromTheme("status-yellow");
                case ServerProcess::State::Running:
                    return QIcon::fromTheme("status-good");
                case ServerProcess::State::Crashed:
                    return QIcon::fromTheme("status-bad");
                case ServerProcess::State::Stopped:
                    break;
            }
            return QIcon::fromTheme("server");
        case StateRole:
            return int(server->process()->state());
        case InstanceRole:
            return QVariant::fromValue(server);
        default:
            return {};
    }
}

ServerInstance* ServerList::at(int row) const
{
    if (row < 0 || row >= m_servers.size())
        return nullptr;
    return m_servers.at(row);
}

ServerInstance* ServerList::byId(const QString& id) const
{
    for (auto server : m_servers) {
        if (server->id() == id)
            return server;
    }
    return nullptr;
}

ServerInstance* ServerList::create(const QString& name)
{
    QDir dir(m_dir);
    if (!dir.exists() && !dir.mkpath(".")) {
        qWarning() << "[Server] Could not create servers folder" << m_dir;
        return nullptr;
    }

    auto base = idFromName(name);
    auto id = base;
    for (int n = 2; dir.exists(id) || byId(id); ++n) {
        if (n > 9999)
            return nullptr;
        id = QString("%1-%2").arg(base).arg(n);
    }

    auto root = dir.filePath(id);
    if (!dir.mkpath(id + "/server")) {
        qWarning() << "[Server] Could not create server folder" << root;
        return nullptr;
    }

    auto server = new ServerInstance(root, this);
    server->setName(name.trimmed().isEmpty() ? id : name.trimmed());
    if (!QFileInfo::exists(FS::PathCombine(root, "server.cfg"))) {
        qWarning() << "[Server] Could not write server.cfg in" << root;
        delete server;
        FS::deletePath(root);
        return nullptr;
    }

    auto row = int(m_servers.size());
    beginInsertRows(QModelIndex(), row, row);
    m_servers.append(server);
    endInsertRows();
    watch(server);
    qInfo() << "[Server] Created server" << id;
    emit serverAdded(server);
    return server;
}

bool ServerList::remove(ServerInstance* server)
{
    auto row = int(m_servers.indexOf(server));
    if (!server || row < 0)
        return false;
    if (server->process()->isActive()) {
        qWarning() << "[Server] Refusing to delete running server" << server->id();
        return false;
    }

    auto id = server->id();
    auto root = server->rootDir();
    if (QFileInfo::exists(root)) {
        if (!FS::trash(root) && !FS::deletePath(root)) {
            qWarning() << "[Server] Could not delete server folder" << root;
            return false;
        }
    }

    beginRemoveRows(QModelIndex(), row, row);
    m_servers.removeAt(row);
    endRemoveRows();
    disconnect(server, nullptr, this, nullptr);
    disconnect(server->process(), nullptr, this, nullptr);
    server->deleteLater();
    qInfo() << "[Server] Deleted server" << id;
    emit serverRemoved(id);
    return true;
}

bool ServerList::anyActive() const
{
    return std::any_of(m_servers.begin(), m_servers.end(), [](ServerInstance* s) { return s->process()->isActive(); });
}

void ServerList::watch(ServerInstance* server)
{
    auto changed = [this, server] {
        auto row = int(m_servers.indexOf(server));
        if (row < 0)
            return;
        auto idx = index(row);
        emit dataChanged(idx, idx);
    };
    connect(server, &ServerInstance::nameChanged, this, changed);
    connect(server->process(), &ServerProcess::stateChanged, this, changed);
}

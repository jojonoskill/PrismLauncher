// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QAbstractListModel>
#include <QList>

class ServerInstance;

/// All servers under <data>/servers/. Owned by Application (APPLICATION->servers()).
/// Model rows: Qt::DisplayRole = name, Qt::DecorationRole = icon, StateRole = ServerProcess::State (int), InstanceRole = ServerInstance*.
class ServerList : public QAbstractListModel {
    Q_OBJECT
   public:
    enum Roles { StateRole = Qt::UserRole + 1, InstanceRole };

    explicit ServerList(const QString& serversDir, QObject* parent = nullptr);
    ~ServerList() override;

    QString serversDir() const { return m_dir; }

    /// (re)scans serversDir; keeps existing ServerInstance objects (and running processes) for folders still present
    void load();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    QList<ServerInstance*> servers() const { return m_servers; }
    ServerInstance* at(int row) const;
    ServerInstance* byId(const QString& id) const;

    /// Creates <serversDir>/<unique id from name>/ with server.cfg (Name set) and an empty server/ folder, adds it to
    /// the list and returns it. nullptr on failure.
    ServerInstance* create(const QString& name);
    /// Deletes the server folder (moves to the OS trash when possible). Refuses (returns false) while it is running.
    bool remove(ServerInstance* server);

    bool anyActive() const;

   signals:
    void serverAdded(ServerInstance* server);
    void serverRemoved(const QString& id);

   private:
    QString m_dir;
    QList<ServerInstance*> m_servers;
    void watch(ServerInstance* server);
};

// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QList>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "server/backup/ServerBackups.h"

class ServerInstance;
class QFileSystemWatcher;
class QLabel;
class QPushButton;
class QToolButton;
class QTreeWidget;

/// World backups of one server (ServerUtilities zips + Elytra's pre-restore copies), restore and world import.
class ServerBackupsPage : public QWidget {
    Q_OBJECT
   public:
    explicit ServerBackupsPage(QWidget* parent = nullptr);
    ~ServerBackupsPage() override = default;

    void setServer(ServerInstance* server);

    /// time of the newest backup (invalid if there are none)
    QDateTime lastBackup() const { return m_lastBackup; }

   public slots:
    void refresh();

   signals:
    /// emitted after every refresh
    void lastBackupChanged(const QDateTime& when);

   protected:
    void showEvent(QShowEvent* event) override;

   private:
    void scheduleRefresh();
    void updateWatcher();
    void updateSummary();
    void updateButtons();
    void backupNow();
    void restoreSelected();
    void importWorld(bool fromZip);
    void openBackupsFolder();
    bool runTask(Task* task);

   private:
    QPointer<ServerInstance> m_server;
    QList<QMetaObject::Connection> m_connections;
    QList<ServerBackup> m_backups;
    QDateTime m_lastBackup;

    QFileSystemWatcher* m_watcher = nullptr;
    QTimer m_refreshTimer;  // debounce for the watcher (zips are written over a few seconds)
    QTimer m_summaryTimer;  // keeps "12 minutes ago" current

    QLabel* m_summary = nullptr;
    QLabel* m_info = nullptr;
    QTreeWidget* m_tree = nullptr;
    QPushButton* m_backupNowButton = nullptr;
    QPushButton* m_restoreButton = nullptr;
    QToolButton* m_importButton = nullptr;
    QPushButton* m_openFolderButton = nullptr;
};

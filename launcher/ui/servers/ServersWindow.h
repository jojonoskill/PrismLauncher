// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QDateTime>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

class ServerInstance;
class ServerList;
class ServerConsolePage;
class ServerBackupsPage;
class ServerSettingsPage;
class QFrame;
class QLabel;
class QListView;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QAction;

/// Window for managing the dedicated servers hosted by the launcher: list on the left, the selected server's status,
/// controls and console / backups / settings on the right.
/// The servers' processes are owned by their ServerInstance, so closing this window doesn't affect running servers.
class ServersWindow : public QMainWindow {
    Q_OBJECT
   public:
    explicit ServersWindow(QWidget* parent = nullptr);
    ~ServersWindow() override;

    void selectServer(ServerInstance* server);
    ServerInstance* currentServer() const { return m_current; }

   public slots:
    void createServer();

   protected:
    void closeEvent(QCloseEvent* event) override;

   private:
    QWidget* createEmptyPage();
    QWidget* createMainPage();
    QFrame* createStatCard(const QString& objectName, QLabel*& title, QLabel*& value);

    void watchServer(ServerInstance* server);
    void currentRowChanged();
    void updateEmptyState();
    void updateHeader();
    void updateTimes();
    void serverChanged();  // slot for any watched server's signals (uses sender())

    void startServer();
    void stopServer();
    void restartServer();
    void killServer();
    void openServerFolder(ServerInstance* server);
    void deleteServer(ServerInstance* server);
    void showListContextMenu(const QPoint& pos);
    void saveWindowState();

   private:
    ServerList* m_list = nullptr;
    QPointer<ServerInstance> m_current;
    QDateTime m_lastBackup;
    QTimer m_clock;  // uptime / "x minutes ago"

    QStackedWidget* m_stack = nullptr;
    QWidget* m_emptyPage = nullptr;
    QWidget* m_mainPage = nullptr;
    QSplitter* m_splitter = nullptr;
    QListView* m_listView = nullptr;

    QLabel* m_nameLabel = nullptr;
    QLabel* m_stateLabel = nullptr;
    QLabel* m_uptimeTitle = nullptr;
    QLabel* m_uptimeValue = nullptr;
    QLabel* m_playersTitle = nullptr;
    QLabel* m_playersValue = nullptr;
    QLabel* m_backupTitle = nullptr;
    QLabel* m_backupValue = nullptr;
    QFrame* m_playersCard = nullptr;

    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_restartButton = nullptr;
    QToolButton* m_moreButton = nullptr;
    QAction* m_killAction = nullptr;
    QAction* m_openFolderAction = nullptr;
    QAction* m_deleteAction = nullptr;

    QTabWidget* m_tabs = nullptr;
    ServerConsolePage* m_consolePage = nullptr;
    ServerBackupsPage* m_backupsPage = nullptr;
    ServerSettingsPage* m_settingsPage = nullptr;
};

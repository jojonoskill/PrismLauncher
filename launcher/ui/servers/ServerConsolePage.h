// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QPointer>
#include <QStringList>
#include <QWidget>

#include "server/ServerProcess.h"

class ServerInstance;
class LogView;
class LogFormatProxyModel;
class QLineEdit;
class QListWidget;
class QLabel;
class QPushButton;

/// Live console of one server: output log, command line with history, online players.
class ServerConsolePage : public QWidget {
    Q_OBJECT
   public:
    explicit ServerConsolePage(QWidget* parent = nullptr);
    ~ServerConsolePage() override = default;

    void setServer(ServerInstance* server);
    /// moves keyboard focus to the command line (if usable)
    void focusCommandLine();

   protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

   private:
    void sendCurrentCommand();
    void copyLog();
    void clearLog();
    void updateState();
    void updatePlayers();

   private:
    QPointer<ServerInstance> m_server;
    QList<QMetaObject::Connection> m_connections;

    LogView* m_logView = nullptr;
    LogFormatProxyModel* m_proxy = nullptr;
    QLineEdit* m_commandEdit = nullptr;
    QPushButton* m_sendButton = nullptr;
    QPushButton* m_copyButton = nullptr;
    QPushButton* m_clearButton = nullptr;
    QLabel* m_playersTitle = nullptr;
    QListWidget* m_playersList = nullptr;

    QStringList m_history;
    int m_historyPos = 0;
    QString m_pendingInput;  // what was typed before browsing the history
};

// SPDX-License-Identifier: GPL-3.0-only
#include "ServerConsolePage.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "Application.h"
#include "launch/LogModel.h"
#include "server/ServerInstance.h"
#include "ui/GuiUtil.h"
#include "ui/pages/instance/LogPage.h"
#include "ui/widgets/LogView.h"

namespace {
constexpr int maxHistory = 100;
}

ServerConsolePage::ServerConsolePage(QWidget* parent) : QWidget(parent)
{
    setObjectName("serverConsolePage");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(10);

    // tool row
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        auto* title = new QLabel(tr("Server console"), this);
        title->setObjectName("serverSectionTitle");
        row->addWidget(title);
        row->addStretch(1);

        m_copyButton = new QPushButton(QIcon::fromTheme("copy"), tr("Copy log"), this);
        m_copyButton->setToolTip(tr("Copy the whole console output to the clipboard."));
        connect(m_copyButton, &QPushButton::clicked, this, &ServerConsolePage::copyLog);
        row->addWidget(m_copyButton);

        m_clearButton = new QPushButton(QIcon::fromTheme("delete"), tr("Clear"), this);
        m_clearButton->setToolTip(tr("Clear the console view. The server's own log files are not touched."));
        connect(m_clearButton, &QPushButton::clicked, this, &ServerConsolePage::clearLog);
        row->addWidget(m_clearButton);

        layout->addLayout(row);
    }

    // log + players
    {
        auto* splitter = new QSplitter(Qt::Horizontal, this);
        splitter->setChildrenCollapsible(false);

        m_proxy = new LogFormatProxyModel(this);
        {
            QString fontFamily = APPLICATION->settings()->get("ConsoleFont").toString();
            bool conversionOk = false;
            int fontSize = APPLICATION->settings()->get("ConsoleFontSize").toInt(&conversionOk);
            if (!conversionOk)
                fontSize = 11;
            m_proxy->setFont(QFont(fontFamily, fontSize));
        }

        m_logView = new LogView(splitter);
        m_logView->setObjectName("serverConsoleLog");
        m_logView->setReadOnly(true);
        m_logView->setWordWrap(true);
        m_logView->setModel(m_proxy);
        splitter->addWidget(m_logView);

        auto* playersPanel = new QWidget(splitter);
        playersPanel->setObjectName("serverPlayersPanel");
        auto* playersLayout = new QVBoxLayout(playersPanel);
        playersLayout->setContentsMargins(8, 0, 0, 0);
        playersLayout->setSpacing(6);
        m_playersTitle = new QLabel(playersPanel);
        m_playersTitle->setObjectName("serverSectionTitle");
        playersLayout->addWidget(m_playersTitle);
        m_playersList = new QListWidget(playersPanel);
        m_playersList->setObjectName("serverPlayersList");
        m_playersList->setSelectionMode(QAbstractItemView::NoSelection);
        m_playersList->setFocusPolicy(Qt::NoFocus);
        playersLayout->addWidget(m_playersList, 1);
        splitter->addWidget(playersPanel);

        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 0);
        splitter->setSizes({ 900, 220 });
        layout->addWidget(splitter, 1);
    }

    // command line
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        m_commandEdit = new QLineEdit(this);
        m_commandEdit->setObjectName("serverCommandEdit");
        m_commandEdit->setClearButtonEnabled(true);
        m_commandEdit->installEventFilter(this);
        connect(m_commandEdit, &QLineEdit::returnPressed, this, &ServerConsolePage::sendCurrentCommand);
        row->addWidget(m_commandEdit, 1);

        m_sendButton = new QPushButton(tr("Send"), this);
        m_sendButton->setObjectName("serverSendButton");
        connect(m_sendButton, &QPushButton::clicked, this, &ServerConsolePage::sendCurrentCommand);
        row->addWidget(m_sendButton);
        layout->addLayout(row);
    }

    setServer(nullptr);
}

void ServerConsolePage::setServer(ServerInstance* server)
{
    for (auto& connection : m_connections)
        disconnect(connection);
    m_connections.clear();

    m_server = server;
    m_history.clear();
    m_historyPos = 0;
    m_pendingInput.clear();
    m_commandEdit->clear();

    if (server && server->process()) {
        auto* process = server->process();
        m_proxy->setSourceModel(process->log());
        m_connections << connect(process, &ServerProcess::stateChanged, this, [this] {
            updateState();
            updatePlayers();
        });
        m_connections << connect(process, &ServerProcess::playersChanged, this, &ServerConsolePage::updatePlayers);
        m_logView->scrollToBottom();
    } else {
        m_proxy->setSourceModel(nullptr);
    }
    updateState();
    updatePlayers();
}

void ServerConsolePage::focusCommandLine()
{
    if (m_commandEdit->isEnabled())
        m_commandEdit->setFocus();
}

bool ServerConsolePage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_commandEdit && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Up) {
            if (m_history.isEmpty() || m_historyPos <= 0)
                return true;
            if (m_historyPos == m_history.size())
                m_pendingInput = m_commandEdit->text();
            m_historyPos--;
            m_commandEdit->setText(m_history.at(m_historyPos));
            return true;
        }
        if (keyEvent->key() == Qt::Key_Down) {
            if (m_historyPos >= m_history.size())
                return true;
            m_historyPos++;
            m_commandEdit->setText(m_historyPos == m_history.size() ? m_pendingInput : m_history.at(m_historyPos));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ServerConsolePage::sendCurrentCommand()
{
    if (!m_server || !m_server->process())
        return;
    auto state = m_server->process()->state();
    if (state != ServerProcess::State::Running && state != ServerProcess::State::Starting)
        return;

    QString command = m_commandEdit->text().trimmed();
    if (command.startsWith('/'))
        command.remove(0, 1);
    if (command.isEmpty())
        return;

    m_server->process()->sendCommand(command);

    if (m_history.isEmpty() || m_history.last() != command)
        m_history.append(command);
    while (m_history.size() > maxHistory)
        m_history.removeFirst();
    m_historyPos = m_history.size();
    m_pendingInput.clear();
    m_commandEdit->clear();
    m_logView->scrollToBottom();
}

void ServerConsolePage::copyLog()
{
    if (!m_server || !m_server->process() || !m_server->process()->log())
        return;
    GuiUtil::setClipboardText(m_server->process()->log()->toPlainText());
}

void ServerConsolePage::clearLog()
{
    if (!m_server || !m_server->process() || !m_server->process()->log())
        return;
    m_server->process()->log()->clear();
}

void ServerConsolePage::updateState()
{
    const auto state = m_server && m_server->process() ? m_server->process()->state() : ServerProcess::State::Stopped;
    const bool canSend = state == ServerProcess::State::Running || state == ServerProcess::State::Starting;
    m_commandEdit->setEnabled(canSend);
    m_sendButton->setEnabled(canSend);
    m_copyButton->setEnabled(m_server != nullptr);
    m_clearButton->setEnabled(m_server != nullptr);
    if (canSend)
        m_commandEdit->setPlaceholderText(tr("Type a server command (e.g. list, say Hello, time set day) and press Enter"));
    else
        m_commandEdit->setPlaceholderText(tr("Start the server to send commands"));
}

void ServerConsolePage::updatePlayers()
{
    QStringList players;
    bool running = false;
    if (m_server && m_server->process()) {
        players = m_server->process()->onlinePlayers();
        running = m_server->process()->isActive();
    }
    m_playersTitle->setText(tr("Online (%1)").arg(players.size()));
    m_playersList->clear();
    if (players.isEmpty()) {
        auto* item = new QListWidgetItem(running ? tr("Nobody online") : tr("Server offline"), m_playersList);
        item->setFlags(Qt::NoItemFlags);
        return;
    }
    players.sort(Qt::CaseInsensitive);
    for (const auto& player : players)
        new QListWidgetItem(QIcon::fromTheme("accounts"), player, m_playersList);
}

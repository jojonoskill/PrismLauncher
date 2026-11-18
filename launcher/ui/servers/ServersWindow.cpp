// SPDX-License-Identifier: GPL-3.0-only
#include "ServersWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "Application.h"
#include "BuildConfig.h"
#include "DesktopServices.h"
#include "server/ServerInstance.h"
#include "server/ServerList.h"
#include "server/ServerProcess.h"
#include "settings/SettingsObject.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/servers/NewServerDialog.h"
#include "ui/servers/ServerBackupsPage.h"
#include "ui/servers/ServerConsolePage.h"
#include "ui/servers/ServerSettingsPage.h"
#include "ui/servers/ServerUiCommon.h"

namespace {
const char* geometrySetting = "ServersWindowGeometry";
const char* splitterSetting = "ServersWindowSplitter";

/// One row of the server list: state dot, name, and "Running - 2 online" below it.
class ServerItemDelegate : public QStyledItemDelegate {
   public:
    explicit ServerItemDelegate(ServerList* list, QObject* parent) : QStyledItemDelegate(parent), m_list(list) {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        const QFontMetrics fm(option.font);
        return QSize(220, qMax(64, fm.height() * 2 + 30));
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        opt.features.setFlag(QStyleOptionViewItem::HasDecoration, false);
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        ServerInstance* server = m_list->at(index.row());
        auto state = static_cast<ServerProcess::State>(index.data(ServerList::StateRole).toInt());
        int players = 0;
        if (server && server->process()) {
            state = server->process()->state();
            players = server->process()->onlinePlayers().size();
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect rect = opt.rect.adjusted(14, 8, -12, -8);
        const int dotSize = 12;
        const QRect dot(rect.left(), rect.center().y() - dotSize / 2, dotSize, dotSize);
        painter->setPen(Qt::NoPen);
        painter->setBrush(ServerUi::stateColor(state));
        painter->drawEllipse(dot);

        const bool selected = opt.state & QStyle::State_Selected;
        const QColor textColor = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        QColor mutedColor = textColor;
        mutedColor.setAlphaF(0.6f);

        const int textLeft = dot.right() + 14;
        const QRect textRect(textLeft, rect.top(), rect.right() - textLeft, rect.height());

        QFont nameFont = opt.font;
        nameFont.setBold(true);
        nameFont.setPointSizeF(nameFont.pointSizeF() * 1.08);
        QFont subFont = opt.font;
        subFont.setPointSizeF(subFont.pointSizeF() * 0.88);
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics subMetrics(subFont);
        const int total = nameMetrics.height() + 2 + subMetrics.height();
        const int top = textRect.top() + (textRect.height() - total) / 2;

        const QString name = index.data(Qt::DisplayRole).toString();
        painter->setFont(nameFont);
        painter->setPen(textColor);
        painter->drawText(QRect(textRect.left(), top, textRect.width(), nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          nameMetrics.elidedText(name, Qt::ElideRight, textRect.width()));

        QString sub = ServerUi::stateText(state);
        if (state == ServerProcess::State::Running || state == ServerProcess::State::Starting)
            sub += QString::fromUtf8(" · ") + ServersWindow::tr("%n online", nullptr, players);
        painter->setFont(subFont);
        painter->setPen(mutedColor);
        painter->drawText(QRect(textRect.left(), top + nameMetrics.height() + 2, textRect.width(), subMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, subMetrics.elidedText(sub, Qt::ElideRight, textRect.width()));

        painter->restore();
    }

   private:
    ServerList* m_list;
};
}  // namespace

ServersWindow::ServersWindow(QWidget* parent) : QMainWindow(parent), m_list(APPLICATION->servers())
{
    setObjectName("serversWindow");
    setAttribute(Qt::WA_DeleteOnClose);
    // the application appends " - <launcher name> <version>" to every window title
    setWindowTitle(tr("Servers"));
    setWindowIcon(QIcon::fromTheme("server"));

    m_stack = new QStackedWidget(this);
    m_emptyPage = createEmptyPage();
    m_mainPage = createMainPage();
    m_stack->addWidget(m_emptyPage);
    m_stack->addWidget(m_mainPage);
    setCentralWidget(m_stack);

    // geometry
    ServerUi::ensureLauncherSetting(geometrySetting);
    ServerUi::ensureLauncherSetting(splitterSetting);
    const QByteArray geometry = QByteArray::fromBase64(APPLICATION->settings()->get(geometrySetting).toString().toUtf8());
    if (geometry.isEmpty() || !restoreGeometry(geometry))
        resize(1500, 950);
    const QByteArray splitter = QByteArray::fromBase64(APPLICATION->settings()->get(splitterSetting).toString().toUtf8());
    if (splitter.isEmpty() || !m_splitter->restoreState(splitter))
        m_splitter->setSizes({ 320, 1180 });

    // follow the list
    for (auto* server : m_list->servers())
        watchServer(server);
    connect(m_list, &ServerList::serverAdded, this, [this](ServerInstance* server) {
        watchServer(server);
        updateEmptyState();
    });
    connect(m_list, &ServerList::serverRemoved, this, [this] {
        updateEmptyState();
        currentRowChanged();
    });
    connect(m_list, &QAbstractItemModel::modelReset, this, [this] {
        for (auto* server : m_list->servers())
            watchServer(server);
        updateEmptyState();
        if (m_current)
            selectServer(m_current);
        else if (m_list->rowCount() > 0)
            m_listView->setCurrentIndex(m_list->index(0));
        currentRowChanged();
    });
    connect(m_list, &QAbstractItemModel::rowsInserted, this, &ServersWindow::updateEmptyState);
    connect(m_list, &QAbstractItemModel::rowsRemoved, this, &ServersWindow::updateEmptyState);
    connect(m_list, &QAbstractItemModel::dataChanged, this, [this] { updateHeader(); });

    m_clock.setInterval(1000);
    connect(&m_clock, &QTimer::timeout, this, &ServersWindow::updateTimes);
    m_clock.start();

    updateEmptyState();
    if (m_list->rowCount() > 0)
        m_listView->setCurrentIndex(m_list->index(0));
    currentRowChanged();
}

ServersWindow::~ServersWindow() = default;

QWidget* ServersWindow::createEmptyPage()
{
    auto* page = new QWidget(this);
    page->setObjectName("serversEmptyPage");
    auto* layout = new QVBoxLayout(page);
    layout->setSpacing(18);
    layout->addStretch(2);

    auto* icon = new QLabel(page);
    icon->setPixmap(QIcon::fromTheme("server").pixmap(96, 96));
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);

    auto* title = new QLabel(tr("No servers yet"), page);
    title->setObjectName("serversEmptyTitle");
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto* text = new QLabel(tr("Host a Minecraft server on this computer and play on it from another PC.\n"
                               "Install a server pack, start it with one click, see who's online and keep your world backed up."),
                            page);
    text->setObjectName("serversEmptyText");
    text->setAlignment(Qt::AlignCenter);
    text->setWordWrap(true);
    layout->addWidget(text);

    auto* button = new QPushButton(QIcon::fromTheme("new"), tr("Create your first server"), page);
    button->setObjectName("serverStartButton");
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QPushButton::clicked, this, &ServersWindow::createServer);
    auto* row = new QHBoxLayout();
    row->addStretch(1);
    row->addWidget(button);
    row->addStretch(1);
    layout->addLayout(row);

    layout->addStretch(3);
    return page;
}

QFrame* ServersWindow::createStatCard(const QString& objectName, QLabel*& title, QLabel*& value)
{
    auto* card = new QFrame(this);
    card->setObjectName(objectName);
    card->setProperty("statCard", true);
    card->setMinimumWidth(170);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 10, 14, 10);
    cardLayout->setSpacing(2);
    title = new QLabel(card);
    title->setObjectName("statTitle");
    value = new QLabel(card);
    value->setObjectName("statValue");
    cardLayout->addWidget(title);
    cardLayout->addWidget(value);
    return card;
}

QWidget* ServersWindow::createMainPage()
{
    auto* page = new QWidget(this);
    auto* pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);

    m_splitter = new QSplitter(Qt::Horizontal, page);
    m_splitter->setChildrenCollapsible(false);
    pageLayout->addWidget(m_splitter);

    // left: server list
    {
        auto* left = new QWidget(m_splitter);
        left->setObjectName("serversListPanel");
        auto* layout = new QVBoxLayout(left);
        layout->setContentsMargins(16, 18, 12, 16);
        layout->setSpacing(12);

        auto* title = new QLabel(tr("Servers"), left);
        title->setObjectName("serverSectionTitle");
        layout->addWidget(title);

        m_listView = new QListView(left);
        m_listView->setObjectName("serverList");
        m_listView->setModel(m_list);
        m_listView->setItemDelegate(new ServerItemDelegate(m_list, m_listView));
        m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
        m_listView->setUniformItemSizes(true);
        connect(m_listView->selectionModel(), &QItemSelectionModel::currentChanged, this, &ServersWindow::currentRowChanged);
        // follow the selection too: the highlighted server must always be the one the pages act on
        connect(m_listView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ServersWindow::currentRowChanged);
        connect(m_listView, &QListView::customContextMenuRequested, this, &ServersWindow::showListContextMenu);
        layout->addWidget(m_listView, 1);

        auto* newButton = new QPushButton(QIcon::fromTheme("new"), tr("New server"), left);
        newButton->setObjectName("serverNewButton");
        connect(newButton, &QPushButton::clicked, this, &ServersWindow::createServer);
        layout->addWidget(newButton);

        m_splitter->addWidget(left);
    }

    // right: details
    {
        auto* right = new QWidget(m_splitter);
        right->setObjectName("serverDetailsPanel");
        auto* layout = new QVBoxLayout(right);
        layout->setContentsMargins(20, 18, 20, 16);
        layout->setSpacing(14);

        // header: name + state, stat cards on the right
        {
            auto* header = new QHBoxLayout();
            header->setSpacing(16);

            auto* titles = new QVBoxLayout();
            titles->setSpacing(4);
            m_nameLabel = new QLabel(right);
            m_nameLabel->setObjectName("serverNameLabel");
            m_nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
            titles->addWidget(m_nameLabel);
            m_stateLabel = new QLabel(right);
            m_stateLabel->setObjectName("serverStateLabel");
            m_stateLabel->setTextFormat(Qt::RichText);
            titles->addWidget(m_stateLabel);
            header->addLayout(titles, 1);

            header->addWidget(createStatCard("serverUptimeCard", m_uptimeTitle, m_uptimeValue));
            m_playersCard = createStatCard("serverPlayersCard", m_playersTitle, m_playersValue);
            header->addWidget(m_playersCard);
            header->addWidget(createStatCard("serverBackupCard", m_backupTitle, m_backupValue));
            layout->addLayout(header);

            m_uptimeTitle->setText(tr("Uptime"));
            m_playersTitle->setText(tr("Players"));
            m_backupTitle->setText(tr("Last backup"));
        }

        // actions
        {
            auto* row = new QHBoxLayout();
            row->setSpacing(10);

            m_startButton = new QPushButton(QIcon::fromTheme("launch"), tr("Start"), right);
            m_startButton->setObjectName("serverStartButton");
            m_startButton->setCursor(Qt::PointingHandCursor);
            connect(m_startButton, &QPushButton::clicked, this, &ServersWindow::startServer);
            row->addWidget(m_startButton);

            m_stopButton = new QPushButton(QIcon::fromTheme("status-bad"), tr("Stop"), right);
            m_stopButton->setObjectName("serverActionButton");
            m_stopButton->setToolTip(tr("Save the world and shut the server down cleanly."));
            connect(m_stopButton, &QPushButton::clicked, this, &ServersWindow::stopServer);
            row->addWidget(m_stopButton);

            m_restartButton = new QPushButton(QIcon::fromTheme("refresh"), tr("Restart"), right);
            m_restartButton->setObjectName("serverActionButton");
            m_restartButton->setToolTip(tr("Stop the server cleanly and start it again."));
            connect(m_restartButton, &QPushButton::clicked, this, &ServersWindow::restartServer);
            row->addWidget(m_restartButton);

            row->addStretch(1);

            auto* menu = new QMenu(this);
            m_openFolderAction = menu->addAction(QIcon::fromTheme("viewfolder"), tr("Open server folder"), this,
                                                 [this] { openServerFolder(m_current); });
            menu->addSeparator();
            m_killAction = menu->addAction(QIcon::fromTheme("status-bad"), tr("Kill (force stop)..."), this, &ServersWindow::killServer);
            m_deleteAction =
                menu->addAction(QIcon::fromTheme("delete"), tr("Delete server..."), this, [this] { deleteServer(m_current); });

            m_moreButton = new QToolButton(right);
            m_moreButton->setObjectName("serverMoreButton");
            m_moreButton->setText(tr("More"));
            m_moreButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
            m_moreButton->setPopupMode(QToolButton::InstantPopup);
            m_moreButton->setMenu(menu);
            row->addWidget(m_moreButton);

            layout->addLayout(row);
        }

        m_tabs = new QTabWidget(right);
        m_tabs->setObjectName("serverTabs");
        m_consolePage = new ServerConsolePage(m_tabs);
        m_backupsPage = new ServerBackupsPage(m_tabs);
        m_settingsPage = new ServerSettingsPage(m_tabs);
        m_tabs->addTab(m_consolePage, QIcon::fromTheme("log"), tr("Console"));
        m_tabs->addTab(m_backupsPage, QIcon::fromTheme("worlds"), tr("Backups"));
        m_tabs->addTab(m_settingsPage, QIcon::fromTheme("settings"), tr("Settings"));
        layout->addWidget(m_tabs, 1);

        connect(m_backupsPage, &ServerBackupsPage::lastBackupChanged, this, [this](const QDateTime& when) {
            m_lastBackup = when;
            updateTimes();
        });

        m_splitter->addWidget(right);
    }

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    return page;
}

void ServersWindow::watchServer(ServerInstance* server)
{
    if (!server || !server->process())
        return;
    // UniqueConnection (member functions only): watchServer is called again on model resets
    connect(server->process(), &ServerProcess::stateChanged, this, &ServersWindow::serverChanged, Qt::UniqueConnection);
    connect(server->process(), &ServerProcess::playersChanged, this, &ServersWindow::serverChanged, Qt::UniqueConnection);
    connect(server, &ServerInstance::nameChanged, this, &ServersWindow::serverChanged, Qt::UniqueConnection);
    connect(server, &ServerInstance::launchProfileChanged, this, &ServersWindow::serverChanged, Qt::UniqueConnection);
}

void ServersWindow::serverChanged()
{
    m_listView->viewport()->update();
    if (!m_current)
        return;
    QObject* source = sender();
    if (!source || source == m_current.data() || source == m_current->process())
        updateHeader();
}

void ServersWindow::selectServer(ServerInstance* server)
{
    const int row = server ? m_list->servers().indexOf(server) : -1;
    if (row < 0)
        return;
    m_listView->setCurrentIndex(m_list->index(row));
    m_listView->scrollTo(m_list->index(row));
}

void ServersWindow::currentRowChanged()
{
    const auto selected = m_listView->selectionModel()->selectedIndexes();
    const QModelIndex index = selected.isEmpty() ? m_listView->currentIndex() : selected.constFirst();
    ServerInstance* server = index.isValid() ? m_list->at(index.row()) : nullptr;
    if (server == m_current && server)
        return;
    m_current = server;
    m_lastBackup = QDateTime();
    m_consolePage->setServer(server);
    m_backupsPage->setServer(server);
    m_settingsPage->setServer(server);
    updateHeader();
}

void ServersWindow::updateEmptyState()
{
    m_stack->setCurrentWidget(m_list->rowCount() == 0 ? m_emptyPage : m_mainPage);
    if (m_list->rowCount() > 0 && !m_listView->currentIndex().isValid())
        m_listView->setCurrentIndex(m_list->index(0));
}

void ServersWindow::updateHeader()
{
    ServerInstance* server = m_current;
    const bool hasServer = server != nullptr;
    const auto state = hasServer && server->process() ? server->process()->state() : ServerProcess::State::Stopped;
    const bool active = hasServer && server->process() && server->process()->isActive();

    m_tabs->setEnabled(hasServer);
    if (!hasServer) {
        m_nameLabel->setText(tr("No server selected"));
        m_stateLabel->clear();
    } else {
        m_nameLabel->setText(server->name());
        QString line = ServerUi::stateRichText(state);
        const QString version = server->settings()->get("MinecraftVersion").toString();
        if (!version.isEmpty())
            line += QString("&nbsp;&nbsp;&middot;&nbsp;&nbsp;") + tr("Minecraft %1").arg(version).toHtmlEscaped();
        if (!server->launchProfile().isValid())
            line += QString("&nbsp;&nbsp;&middot;&nbsp;&nbsp;") + tr("start command not set up").toHtmlEscaped();
        m_stateLabel->setText(line);
    }

    const bool stopped = state == ServerProcess::State::Stopped || state == ServerProcess::State::Crashed;
    m_startButton->setEnabled(hasServer && stopped);
    m_stopButton->setEnabled(hasServer && (state == ServerProcess::State::Running || state == ServerProcess::State::Starting));
    m_restartButton->setEnabled(hasServer && state == ServerProcess::State::Running);
    m_moreButton->setEnabled(hasServer);
    m_killAction->setEnabled(active);
    m_openFolderAction->setEnabled(hasServer);
    m_deleteAction->setEnabled(hasServer && !active);
    m_deleteAction->setToolTip(active ? tr("Stop the server before deleting it.") : QString());

    // players
    QStringList players = hasServer && server->process() ? server->process()->onlinePlayers() : QStringList();
    if (!active)
        m_playersValue->setText(tr("Offline"));
    else
        m_playersValue->setText(tr("%n online", nullptr, static_cast<int>(players.size())));
    players.sort(Qt::CaseInsensitive);
    m_playersCard->setToolTip(players.isEmpty() ? QString() : players.join('\n'));

    updateTimes();
}

void ServersWindow::updateTimes()
{
    ServerInstance* server = m_current;
    const bool active = server && server->process() && server->process()->isActive();
    if (active && server->process()->startedAt().isValid())
        m_uptimeValue->setText(ServerUi::formatDuration(server->process()->startedAt().secsTo(QDateTime::currentDateTime())));
    else
        m_uptimeValue->setText(QString::fromUtf8("—"));

    if (!server)
        m_backupValue->setText(QString::fromUtf8("—"));
    else if (!m_lastBackup.isValid())
        m_backupValue->setText(tr("None yet"));
    else
        m_backupValue->setText(ServerUi::formatRelative(m_lastBackup));
    m_backupValue->setToolTip(m_lastBackup.isValid() ? ServerUi::formatDateTime(m_lastBackup) : QString());
}

void ServersWindow::createServer()
{
    NewServerDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted || !dialog.createdServer())
        return;

    ServerInstance* server = dialog.createdServer();
    updateEmptyState();
    selectServer(server);
    if (!server->launchProfile().isValid()) {
        m_tabs->setCurrentWidget(m_settingsPage);
        QMessageBox::information(this, tr("Choose the server jar"),
                                 tr("The server was installed, but Elytra could not work out how to start it.\n\n"
                                    "In Settings > Start command, click \"Choose jar...\" and pick the server's jar file."));
    } else {
        m_tabs->setCurrentWidget(m_consolePage);
    }
}

void ServersWindow::startServer()
{
    ServerInstance* server = m_current;
    if (!server || !server->process())
        return;
    QString error;
    if (!server->process()->start(&error)) {
        if (error.isEmpty())
            error = tr("Unknown error.");
        CustomMessageBox::selectable(this, tr("Could not start \"%1\"").arg(server->name()), error, QMessageBox::Critical)->exec();
        if (!server->launchProfile().isValid())
            m_tabs->setCurrentWidget(m_settingsPage);
        return;
    }
    m_tabs->setCurrentWidget(m_consolePage);
    m_consolePage->focusCommandLine();
}

void ServersWindow::stopServer()
{
    if (m_current && m_current->process())
        m_current->process()->stop();
}

void ServersWindow::restartServer()
{
    if (m_current && m_current->process())
        m_current->process()->restart();
}

void ServersWindow::killServer()
{
    ServerInstance* server = m_current;
    if (!server || !server->process() || !server->process()->isActive())
        return;
    auto answer = QMessageBox::warning(this, tr("Kill server"),
                                       tr("Kill \"%1\" immediately?\n\n"
                                          "The world is not saved first: everything since the last autosave can be lost, and chunks "
                                          "that were being written can get corrupted. Use Stop instead unless the server is stuck.")
                                           .arg(server->name()),
                                       QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Yes && server->process())
        server->process()->kill();
}

void ServersWindow::openServerFolder(ServerInstance* server)
{
    if (server)
        DesktopServices::openPath(server->serverDir(), true);
}

void ServersWindow::deleteServer(ServerInstance* server)
{
    if (!server)
        return;
    if (server->process() && server->process()->isActive()) {
        QMessageBox::warning(this, tr("Server is running"), tr("Stop the server before deleting it."));
        return;
    }
    QPointer<ServerInstance> guard(server);
    auto answer = QMessageBox::warning(this, tr("Delete server"),
                                       tr("Delete \"%1\"?\n\nThe folder\n%2\nwith its world, mods and backups will be moved to the "
                                          "recycle bin.")
                                           .arg(server->name(), QDir::toNativeSeparators(server->rootDir())),
                                       QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes || !guard)
        return;
    if (!m_list->remove(server)) {
        QMessageBox::critical(this, tr("Delete server"),
                              tr("The server could not be deleted. Make sure it is stopped and no program is using its folder."));
    }
}

void ServersWindow::showListContextMenu(const QPoint& pos)
{
    const QModelIndex index = m_listView->indexAt(pos);
    ServerInstance* server = index.isValid() ? m_list->at(index.row()) : nullptr;
    QMenu menu(this);
    if (server) {
        const bool active = server->process() && server->process()->isActive();
        menu.addAction(QIcon::fromTheme("viewfolder"), tr("Open folder"), this, [this, server] { openServerFolder(server); });
        menu.addSeparator();
        auto* del = menu.addAction(QIcon::fromTheme("delete"), tr("Delete..."), this, [this, server] { deleteServer(server); });
        del->setEnabled(!active);
    } else {
        menu.addAction(QIcon::fromTheme("new"), tr("New server..."), this, &ServersWindow::createServer);
    }
    menu.exec(m_listView->viewport()->mapToGlobal(pos));
}

void ServersWindow::saveWindowState()
{
    APPLICATION->settings()->set(geometrySetting, QString::fromUtf8(saveGeometry().toBase64()));
    APPLICATION->settings()->set(splitterSetting, QString::fromUtf8(m_splitter->saveState().toBase64()));
}

void ServersWindow::closeEvent(QCloseEvent* event)
{
    saveWindowState();
    QMainWindow::closeEvent(event);
}

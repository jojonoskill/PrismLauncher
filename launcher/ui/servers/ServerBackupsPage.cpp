// SPDX-License-Identifier: GPL-3.0-only
#include "ServerBackupsPage.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QStandardPaths>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <memory>

#include "DesktopServices.h"
#include "server/ServerInstance.h"
#include "server/ServerProcess.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui/servers/ServerUiCommon.h"

namespace {
constexpr int BackupIndexRole = Qt::UserRole + 1;

QString kindText(ServerBackup::Kind kind)
{
    switch (kind) {
        case ServerBackup::Kind::ServerUtilities:
            return ServerBackupsPage::tr("ServerUtilities");
        case ServerBackup::Kind::PreRestore:
            return ServerBackupsPage::tr("Before restore");
        case ServerBackup::Kind::Zip:
            return ServerBackupsPage::tr("World zip");
    }
    return {};
}
}  // namespace

ServerBackupsPage::ServerBackupsPage(QWidget* parent) : QWidget(parent)
{
    setObjectName("serverBackupsPage");

    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &ServerBackupsPage::scheduleRefresh);

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(1500);
    connect(&m_refreshTimer, &QTimer::timeout, this, &ServerBackupsPage::refresh);

    m_summaryTimer.setInterval(30000);
    connect(&m_summaryTimer, &QTimer::timeout, this, &ServerBackupsPage::updateSummary);
    m_summaryTimer.start();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(10);

    m_summary = new QLabel(this);
    m_summary->setObjectName("serverBackupSummary");
    layout->addWidget(m_summary);

    m_info = new QLabel(this);
    m_info->setObjectName("serverInfoNote");
    m_info->setWordWrap(true);
    m_info->setText(
        tr("Backups are made by the ServerUtilities mod, which is not installed on this server. "
           "Install it (GT New Horizons ships with it) to get automatic world backups here."));
    layout->addWidget(m_info);

    // buttons
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);

        m_backupNowButton = new QPushButton(tr("Backup now"), this);
        m_backupNowButton->setToolTip(tr("Ask ServerUtilities to make a backup right now (runs \"backup start\" on the server)."));
        connect(m_backupNowButton, &QPushButton::clicked, this, &ServerBackupsPage::backupNow);
        row->addWidget(m_backupNowButton);

        m_restoreButton = new QPushButton(tr("Restore..."), this);
        m_restoreButton->setToolTip(tr("Replace the server's world with the selected backup. The server must be stopped."));
        connect(m_restoreButton, &QPushButton::clicked, this, &ServerBackupsPage::restoreSelected);
        row->addWidget(m_restoreButton);

        m_importButton = new QToolButton(this);
        m_importButton->setObjectName("serverImportButton");
        m_importButton->setText(tr("Import world..."));
        m_importButton->setToolTip(tr("Use another world (for example your singleplayer world) as this server's world."));
        m_importButton->setPopupMode(QToolButton::InstantPopup);
        m_importButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        auto* importMenu = new QMenu(m_importButton);
        importMenu->addAction(tr("From a world folder (e.g. a singleplayer save)..."), this, [this] { importWorld(false); });
        importMenu->addAction(tr("From a world or backup .zip file..."), this, [this] { importWorld(true); });
        m_importButton->setMenu(importMenu);
        row->addWidget(m_importButton);

        row->addStretch(1);

        m_openFolderButton = new QPushButton(QIcon::fromTheme("viewfolder"), tr("Open backups folder"), this);
        connect(m_openFolderButton, &QPushButton::clicked, this, &ServerBackupsPage::openBackupsFolder);
        row->addWidget(m_openFolderButton);

        layout->addLayout(row);
    }

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName("serverBackupsTree");
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setHeaderLabels({ tr("Date"), tr("Type"), tr("Name"), tr("Size") });
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &ServerBackupsPage::updateButtons);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (m_restoreButton->isEnabled())
            restoreSelected();
    });
    layout->addWidget(m_tree, 1);

    setServer(nullptr);
}

void ServerBackupsPage::setServer(ServerInstance* server)
{
    for (auto& connection : m_connections)
        disconnect(connection);
    m_connections.clear();

    m_server = server;
    if (server && server->process()) {
        m_connections << connect(server->process(), &ServerProcess::stateChanged, this, [this](ServerProcess::State state) {
            updateButtons();
            // the mod may have been added while the server was down; and a stop is a good moment to look again
            if (state == ServerProcess::State::Running || state == ServerProcess::State::Stopped)
                scheduleRefresh();
        });
    }
    refresh();
}

void ServerBackupsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!event->spontaneous())
        refresh();
}

void ServerBackupsPage::scheduleRefresh()
{
    m_refreshTimer.start();
}

void ServerBackupsPage::refresh()
{
    m_refreshTimer.stop();

    QString selectedPath;
    if (auto* item = m_tree->currentItem()) {
        const int index = item->data(0, BackupIndexRole).toInt();
        if (index >= 0 && index < m_backups.size())
            selectedPath = m_backups.at(index).path;
    }

    m_tree->clear();
    m_backups.clear();
    m_lastBackup = QDateTime();

    if (m_server) {
        m_backups = ServerBackups::list(m_server);
        const QLocale locale;
        for (int i = 0; i < m_backups.size(); i++) {
            const auto& backup = m_backups.at(i);
            auto* item = new QTreeWidgetItem(m_tree);
            item->setText(0, ServerUi::formatDateTime(backup.created));
            item->setToolTip(0, ServerUi::formatRelative(backup.created));
            item->setText(1, kindText(backup.kind));
            item->setText(2, backup.customName);
            item->setToolTip(2, QDir::toNativeSeparators(backup.path));
            item->setText(3, backup.sizeBytes > 0 ? locale.formattedDataSize(backup.sizeBytes) : QString());
            item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
            item->setData(0, BackupIndexRole, i);
            if (backup.path == selectedPath)
                m_tree->setCurrentItem(item);
            if (backup.kind != ServerBackup::Kind::PreRestore && (!m_lastBackup.isValid() || backup.created > m_lastBackup))
                m_lastBackup = backup.created;
        }
    }

    updateWatcher();
    updateSummary();
    updateButtons();
    emit lastBackupChanged(m_lastBackup);
}

void ServerBackupsPage::updateWatcher()
{
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());
    if (!m_server)
        return;

    QStringList paths;
    const QString folder = ServerBackups::serverUtilitiesFolder(m_server);
    if (!folder.isEmpty() && QFileInfo(folder).isDir()) {
        paths << folder;
    } else if (QFileInfo(m_server->serverDir()).isDir()) {
        // backups folder doesn't exist yet: watch the server folder so we notice when it appears
        paths << m_server->serverDir();
    }
    const QString preRestore = QDir(m_server->serverDir()).filePath("elytra-pre-restore");
    if (QFileInfo(preRestore).isDir())
        paths << preRestore;
    if (!paths.isEmpty())
        m_watcher->addPaths(paths);
}

void ServerBackupsPage::updateSummary()
{
    if (!m_server) {
        m_summary->clear();
        m_info->hide();
        return;
    }
    if (m_lastBackup.isValid()) {
        m_summary->setText(tr("Last backup: <b>%1</b> &nbsp;(%2)")
                               .arg(ServerUi::formatRelative(m_lastBackup).toHtmlEscaped(),
                                    ServerUi::formatDateTime(m_lastBackup).toHtmlEscaped()));
    } else {
        m_summary->setText(tr("No backups yet"));
    }
    m_info->setVisible(!ServerBackups::hasServerUtilities(m_server));
}

void ServerBackupsPage::updateButtons()
{
    const auto state = m_server && m_server->process() ? m_server->process()->state() : ServerProcess::State::Stopped;
    const bool stopped = m_server && (state == ServerProcess::State::Stopped || state == ServerProcess::State::Crashed);
    const bool running = m_server && state == ServerProcess::State::Running;
    const bool hasSU = m_server && ServerBackups::hasServerUtilities(m_server);

    m_backupNowButton->setEnabled(running && hasSU);
    if (!hasSU)
        m_backupNowButton->setToolTip(tr("Needs the ServerUtilities mod."));
    else if (!running)
        m_backupNowButton->setToolTip(tr("The server must be running to make a backup."));
    else
        m_backupNowButton->setToolTip(tr("Ask ServerUtilities to make a backup right now (runs \"backup start\" on the server)."));

    m_restoreButton->setEnabled(stopped && !m_tree->selectedItems().isEmpty());
    m_restoreButton->setToolTip(stopped ? tr("Replace the server's world with the selected backup.")
                                        : tr("Stop the server before restoring a backup."));
    m_importButton->setEnabled(stopped);
    m_importButton->setToolTip(stopped ? tr("Use another world (for example your singleplayer world) as this server's world.")
                                       : tr("Stop the server before importing a world."));
    m_openFolderButton->setEnabled(m_server != nullptr);
}

void ServerBackupsPage::backupNow()
{
    if (!m_server || !m_server->process() || m_server->process()->state() != ServerProcess::State::Running)
        return;
    m_server->process()->sendCommand("backup start");
    // the new zip shows up through the watcher; make sure we look again even if the folder is new
    QTimer::singleShot(5000, this, &ServerBackupsPage::refresh);
}

bool ServerBackupsPage::runTask(Task* task)
{
    ProgressDialog dialog(this);
    dialog.execWithTask(task);
    return task->wasSuccessful();
}

void ServerBackupsPage::restoreSelected()
{
    auto* item = m_tree->selectedItems().isEmpty() ? nullptr : m_tree->selectedItems().constFirst();
    if (!m_server || !item)
        return;
    const int index = item->data(0, BackupIndexRole).toInt();
    if (index < 0 || index >= m_backups.size())
        return;
    const ServerBackup backup = m_backups.at(index);
    if (m_server->process() && m_server->process()->isActive()) {
        QMessageBox::warning(this, tr("Server is running"), tr("Stop the server before restoring a backup."));
        return;
    }

    auto answer = QMessageBox::question(
        this, tr("Restore backup"),
        tr("Restore the backup from %1 (%2)?\n\n"
           "The current world is not deleted: it is moved to the \"elytra-pre-restore\" folder first and shows up in this list "
           "as \"Before restore\", so you can go back to it at any time.")
            .arg(ServerUi::formatDateTime(backup.created), ServerUi::formatRelative(backup.created)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;

    bool allowMismatch = false;
    while (true) {
        auto task = std::make_unique<RestoreBackupTask>(m_server, backup, allowMismatch);
        if (runTask(task.get())) {
            refresh();
            QMessageBox::information(this, tr("Backup restored"),
                                     tr("The backup was restored. The previous world was saved in \"elytra-pre-restore\"."));
            return;
        }
        if (!allowMismatch && task->failedOnWorldMismatch()) {
            auto retry = QMessageBox::warning(this, tr("Different world"),
                                              tr("This backup is from a different world than the server's current one.\n\n"
                                                 "Restore it anyway? The current world is still kept in \"elytra-pre-restore\"."),
                                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (retry == QMessageBox::Yes) {
                allowMismatch = true;
                continue;
            }
            refresh();
            return;
        }
        refresh();
        CustomMessageBox::selectable(this, tr("Restore failed"),
                                     tr("The backup could not be restored. The server's world was not changed.\n\n%1").arg(task->failReason()),
                                     QMessageBox::Critical)
            ->exec();
        return;
    }
}

void ServerBackupsPage::importWorld(bool fromZip)
{
    if (!m_server)
        return;
    if (m_server->process() && m_server->process()->isActive()) {
        QMessageBox::warning(this, tr("Server is running"), tr("Stop the server before importing a world."));
        return;
    }

    const QString startDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString source;
    if (fromZip) {
        source = QFileDialog::getOpenFileName(this, tr("Choose a world or backup zip"), startDir, tr("Zip files (*.zip)"));
    } else {
        source = QFileDialog::getExistingDirectory(this, tr("Choose the world folder (the one containing level.dat)"), startDir);
        if (!source.isEmpty() && !QFileInfo(QDir(source).filePath("level.dat")).isFile()) {
            auto answer = QMessageBox::warning(this, tr("No level.dat"),
                                               tr("The folder %1 has no level.dat, so it doesn't look like a Minecraft world.\n\n"
                                                  "Import it anyway?")
                                                   .arg(QDir::toNativeSeparators(source)),
                                               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes)
                return;
        }
    }
    if (source.isEmpty())
        return;

    const QString uuid = ServerUi::defaultAccountUuid();
    QString playerNote;
    if (uuid.isEmpty()) {
        playerNote = tr("No Minecraft account is selected in the launcher, so a singleplayer inventory and position stay in the "
                        "world file and are not moved to your player on the server.");
    } else {
        playerNote = tr("If this is a singleplayer world, your inventory and position are moved to your account (%1) on the server.")
                         .arg(ServerUi::defaultAccountName());
    }

    const bool hasWorld = QFileInfo(m_server->worldDir()).isDir();
    QString text = tr("Import %1 as the world of \"%2\"?").arg(QDir::toNativeSeparators(source), m_server->name());
    if (hasWorld)
        text += "\n\n" + tr("The current world is not deleted: it is moved to the \"elytra-pre-restore\" folder first.");
    text += "\n\n" + playerNote;
    auto answer = QMessageBox::question(this, tr("Import world"), text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;

    auto task = std::make_unique<WorldImportTask>(m_server, source, uuid);
    const bool ok = runTask(task.get());
    refresh();
    if (ok) {
        QMessageBox::information(this, tr("World imported"), tr("The world was imported. Start the server to play on it."));
    } else {
        CustomMessageBox::selectable(this, tr("Import failed"),
                                     tr("The world could not be imported. The server's world was not changed.\n\n%1").arg(task->failReason()),
                                     QMessageBox::Critical)
            ->exec();
    }
}

void ServerBackupsPage::openBackupsFolder()
{
    if (!m_server)
        return;
    QString folder = ServerBackups::serverUtilitiesFolder(m_server);
    if (folder.isEmpty())
        folder = m_server->serverDir();
    DesktopServices::openPath(folder, true);
}

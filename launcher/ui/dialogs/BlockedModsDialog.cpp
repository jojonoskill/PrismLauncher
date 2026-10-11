// SPDX-FileCopyrightText: 2022 Sefa Eyeoglu <contact@scrumplex.net>
// SPDX-FileCopyrightText: 2022 Rachel Powers <508861+Ryex@users.noreply.github.com>
// SPDX-FileCopyrightText: 2022 kumquat-ir <66188216+kumquat-ir@users.noreply.github.com>
//
// SPDX-License-Identifier: GPL-3.0-only

/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *  Copyright (C) 2022 Rachel Powers <508861+Ryex@users.noreply.github.com>
 *  Copyright (C) 2022 kumquat-ir <66188216+kumquat-ir@users.noreply.github.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "BlockedModsDialog.h"
#include "ui_BlockedModsDialog.h"

#include "Application.h"
#include "settings/SettingsObject.h"
#include "modplatform/helpers/HashUtils.h"

#include <QDebug>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>

BlockedModsDialog::BlockedModsDialog(QWidget* parent, const QString& title, const QString& text, QList<BlockedMod>& mods, QString hash_type)
    : QDialog(parent), ui(new Ui::BlockedModsDialog), m_mods(mods), m_hashType(hash_type)
{
    m_hashingTask = shared_qobject_ptr<ConcurrentTask>(
        new ConcurrentTask("MakeHashesTask", APPLICATION->settings()->get("NumberOfConcurrentTasks").toInt()));
    connect(m_hashingTask.get(), &Task::finished, this, &BlockedModsDialog::hashTaskFinished);

    ui->setupUi(this);

    ui->buttonBox->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    ui->buttonBox->button(QDialogButtonBox::Ok)->setAutoDefault(false);
    ui->openMissingButton->setDefault(true);
    connect(ui->openMissingButton, &QPushButton::clicked, this, [this]() {
        m_guided = true;
        openNextMissing();
    });
    connect(ui->labelWatched, &QLabel::linkActivated, this, [this](const QString& link) {
        if (link == "add-folder")
            addDownloadFolder();
        else
            QDesktopServices::openUrl(QUrl(link));
    });

    // once everything is found, carry on with the install by itself (a short pause shows the result)
    m_autoAccept = new QTimer(this);
    m_autoAccept->setSingleShot(true);
    m_autoAccept->setInterval(1500);
    connect(m_autoAccept, &QTimer::timeout, this, [this] {
        if (allModsMatched())
            accept();
    });

    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &BlockedModsDialog::directoryChanged);

    qDebug() << "[Blocked Mods Dialog]" << text;
    qDebug() << "[Blocked Mods Dialog] Mods List:" << mods;

    // defer setup of file system watchers until after the dialog is shown
    // this allows OS (namely macOS) permission prompts to show after the relevant dialog appears
    QTimer::singleShot(0, this, [this] {
        setupWatch();
        scanPaths();
        update();
    });

    this->setWindowTitle(title);

    QFont titleFont = ui->labelTitle->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.4);
    titleFont.setBold(true);
    ui->labelTitle->setFont(titleFont);
    ui->labelDescription->setText(
        tr("Their authors only allow downloading them from the website, so the launcher can't fetch them for you.<br/><br/>"
           "Click <b>Download missing files</b>. Each download page opens in your browser and the download usually starts "
           "by itself after a few seconds (if not, click the download button on the page). The launcher picks the files up "
           "from your downloads folder, opens the next page, and continues the install when everything is there."));

    buildRows();
    setAcceptDrops(true);

    update();
}

void BlockedModsDialog::buildRows()
{
    ui->listContainer->setStyleSheet(
        "QFrame#blockedRow { background: rgba(127, 127, 127, 0.12); border-radius: 8px; }"
        "QLabel#blockedRowState { color: palette(placeholder-text); }");

    for (int i = 0; i < m_mods.size(); i++) {
        auto row = new QFrame(ui->listContainer);
        row->setObjectName("blockedRow");
        auto layout = new QHBoxLayout(row);
        layout->setContentsMargins(12, 8, 8, 8);
        layout->setSpacing(10);

        auto status = new QLabel(row);
        status->setFixedWidth(20);
        status->setAlignment(Qt::AlignCenter);
        QFont statusFont = status->font();
        statusFont.setPointSizeF(statusFont.pointSizeF() * 1.3);
        statusFont.setBold(true);
        status->setFont(statusFont);

        auto textBox = new QVBoxLayout();
        textBox->setSpacing(1);
        auto name = new QLabel(m_mods[i].name, row);
        name->setToolTip(m_mods[i].websiteUrl);
        // long file names get clipped instead of stretching the dialog
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto state = new QLabel(row);
        state->setObjectName("blockedRowState");
        state->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        textBox->addWidget(name);
        textBox->addWidget(state);

        auto button = new QPushButton(tr("Download"), row);
        button->setAutoDefault(false);
        connect(button, &QPushButton::clicked, this, [this, i] { openMod(i); });

        layout->addWidget(status);
        layout->addLayout(textBox, 1);
        layout->addWidget(button);
        ui->listLayout->addWidget(row);
        m_rows.append({ status, state, button });
    }
    ui->listLayout->addStretch(1);
}

void BlockedModsDialog::openMod(int index)
{
    m_opened.insert(index);
    QDesktopServices::openUrl(m_mods[index].websiteUrl);
    update();
}

/// @brief open the first missing file's page that hasn't been opened yet (or the first missing one, if all were)
void BlockedModsDialog::openNextMissing()
{
    int firstMissing = -1;
    for (int i = 0; i < m_mods.size(); i++) {
        if (m_mods[i].matched)
            continue;
        if (!m_opened.contains(i)) {
            openMod(i);
            return;
        }
        if (firstMissing < 0)
            firstMissing = i;
    }
    if (firstMissing >= 0)
        openMod(firstMissing);
}

BlockedModsDialog::~BlockedModsDialog()
{
    delete ui;
}

void BlockedModsDialog::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) {
        e->acceptProposedAction();
    }
}

void BlockedModsDialog::dropEvent(QDropEvent* e)
{
    for (QUrl& url : e->mimeData()->urls()) {
        if (url.scheme().isEmpty()) {  // ensure isLocalFile() works correctly
            url.setScheme("file");
        }

        if (!url.isLocalFile()) {  // can't drop external files here.
            continue;
        }

        QString filePath = url.toLocalFile();
        qDebug() << "[Blocked Mods Dialog] Dropped file:" << filePath;
        addHashTask(filePath);

        // watch for changes
        QFileInfo file = QFileInfo(filePath);
        QString path = file.dir().absolutePath();
        qDebug() << "[Blocked Mods Dialog] Adding watch path:" << path;
        m_watcher.addPath(path);
    }
    scanPaths();
    update();
}

void BlockedModsDialog::done(int r)
{
    QDialog::done(r);
    disconnect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &BlockedModsDialog::directoryChanged);
}

void BlockedModsDialog::addDownloadFolder()
{
    QString dir =
        QFileDialog::getExistingDirectory(this, tr("Select directory where you downloaded the mods"),
                                          QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), QFileDialog::ShowDirsOnly);
    qDebug() << "[Blocked Mods Dialog] Adding watch path:" << dir;
    m_watcher.addPath(dir);
    scanPath(dir, true);
    update();
}

/// @brief update UI with current status of the blocked mod detection
void BlockedModsDialog::update()
{
    const QColor found(0x4c, 0xbb, 0x6c);
    int foundCount = 0;
    for (int i = 0; i < m_mods.size() && i < m_rows.size(); i++) {
        const auto& mod = m_mods[i];
        auto& row = m_rows[i];
        if (mod.matched) {
            foundCount++;
            row.status->setText(QString::fromUtf8("✓"));
            row.status->setStyleSheet(QString("color: %1;").arg(found.name()));
            row.state->setText(tr("Found: %1").arg(QDir::toNativeSeparators(mod.localPath)));
            row.state->setToolTip(QDir::toNativeSeparators(mod.localPath));
            row.button->hide();
        } else if (m_opened.contains(i)) {
            row.status->setText(QString::fromUtf8("↓"));
            row.status->setStyleSheet("");
            row.state->setText(tr("Opened in your browser, waiting for the file..."));
            row.button->setText(tr("Open again"));
            row.button->show();
        } else {
            row.status->setText(QString::fromUtf8("○"));
            row.status->setStyleSheet("");
            row.state->setText(tr("Not downloaded yet"));
            row.button->setText(tr("Download"));
            row.button->show();
        }
    }

    const int total = m_mods.size();
    const int missing = total - foundCount;
    ui->progressBar->setRange(0, total);
    ui->progressBar->setValue(foundCount);
    ui->progressBar->setFormat(tr("%1 of %2 found").arg(foundCount).arg(total));
    ui->labelTitle->setText(missing == 0 ? tr("All files found") : missing == 1 ? tr("1 file needs a manual download")
                                                                                : tr("%1 files need a manual download").arg(missing));

    QStringList watching;
    for (auto& dir : m_watcher.directories()) {
        // the watcher also holds every subfolder; only list the top-level ones
        bool isSubfolder = std::any_of(watching.begin(), watching.end(), [&dir](const QString& w) { return dir.startsWith(w + "/"); });
        if (!isSubfolder)
            watching << dir;
    }
    QStringList links;
    for (auto& dir : watching)
        links << QString("<a href=\"%1\">%2</a>").arg(QUrl::fromLocalFile(dir).toString(), QDir::toNativeSeparators(dir).toHtmlEscaped());
    ui->labelWatched->setText(tr("Looking for files in %1. Downloaded somewhere else? <a href=\"add-folder\">Add a folder</a> or drag "
                                 "the files onto this window.")
                                  .arg(links.isEmpty() ? tr("your downloads folder") : links.join(", ")));

    auto okButton = ui->buttonBox->button(QDialogButtonBox::Ok);
    if (missing == 0) {
        ui->labelModsFound->setText(tr("Continuing the install..."));
        ui->openMissingButton->hide();
        okButton->setText(tr("Continue"));
        if (!m_autoAccept->isActive())
            m_autoAccept->start();
    } else {
        m_autoAccept->stop();
        ui->labelModsFound->clear();
        ui->openMissingButton->show();
        ui->openMissingButton->setText(m_guided ? tr("Open next file") : tr("Download missing files"));
        // the theme's default-button style is bolder than what the size hint measures, so leave some room
        ui->openMissingButton->setMinimumWidth(ui->openMissingButton->fontMetrics().horizontalAdvance(ui->openMissingButton->text()) + 48);
        okButton->setText(tr("Skip these files"));
    }
}

/// @brief Signal fired when a watched directory has changed
/// @param path the path to the changed directory
void BlockedModsDialog::directoryChanged(QString path)
{
    qDebug() << "[Blocked Mods Dialog] Directory changed:" << path;
    validateMatchedMods();
    scanPath(path, true);
}

/// @brief add the user downloads folder and the global mods folder to the filesystem watcher
void BlockedModsDialog::setupWatch()
{
    const QString downloadsFolder = APPLICATION->settings()->get("DownloadsDir").toString();
    const QString modsFolder = APPLICATION->settings()->get("CentralModsDir").toString();
    const bool downloadsFolderWatchRecursive = APPLICATION->settings()->get("DownloadsDirWatchRecursive").toBool();
    watchPath(downloadsFolder, downloadsFolderWatchRecursive);
    watchPath(modsFolder, true);
}

void BlockedModsDialog::watchPath(QString path, bool watch_recursive)
{
    auto to_watch = QFileInfo(path);
    if (!to_watch.isReadable()) {
        qWarning() << "[Blocked Mods Dialog] Failed to add Watch Path (unable to read):" << path;
        return;
    }
    auto to_watch_path = to_watch.canonicalFilePath();
    if (m_watcher.directories().contains(to_watch_path))
        return;  // don't watch the same path twice (no loops!)

    qDebug() << "[Blocked Mods Dialog] Adding Watch Path:" << path;
    m_watcher.addPath(to_watch_path);

    if (!to_watch.isDir() || !watch_recursive)
        return;

    QDirIterator it(to_watch_path, QDir::Filter::Dirs | QDir::Filter::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        QString watch_dir = QDir(it.next()).canonicalPath();  // resolve symlinks and relative paths
        watchPath(watch_dir, watch_recursive);
    }
}

/// @brief scan all watched folder
void BlockedModsDialog::scanPaths()
{
    for (auto& dir : m_watcher.directories()) {
        scanPath(dir, false);
    }
    runHashTask();
}

/// @brief Scan the directory at path, skip paths that do not contain a file name
///        of a blocked mod we are looking for
/// @param path the directory to scan
void BlockedModsDialog::scanPath(QString path, bool start_task)
{
    QDir scan_dir(path);
    QDirIterator scan_it(path, QDir::Filter::Files | QDir::Filter::Hidden, QDirIterator::NoIteratorFlags);
    while (scan_it.hasNext()) {
        QString file = scan_it.next();

        if (!checkValidPath(file)) {
            continue;
        }

        addHashTask(file);
    }

    if (start_task) {
        runHashTask();
    }
}

/// @brief add a hashing task for the file located at path, add the path to the pending set if the hashing task is already running
/// @param path the path to the local file being hashed
void BlockedModsDialog::addHashTask(QString path)
{
    qDebug() << "[Blocked Mods Dialog] adding a Hash task for" << path << "to the pending set.";
    m_pendingHashPaths.insert(path);
}

/// @brief add a hashing task for the file located at path and connect it to check that hash against
///        our blocked mods list
/// @param path the path to the local file being hashed
void BlockedModsDialog::buildHashTask(QString path)
{
    auto hash_task = Hashing::createHasher(path, m_hashType);

    qDebug() << "[Blocked Mods Dialog] Creating Hash task for path:" << path;

    connect(hash_task.get(), &Task::succeeded, this, [this, hash_task, path] { checkMatchHash(hash_task->getResult(), path); });
    connect(hash_task.get(), &Task::failed, this, [path] { qDebug() << "Failed to hash path:" << path; });

    m_hashingTask->addTask(hash_task);
}

/// @brief check if the computed hash for the provided path matches a blocked
///        mod we are looking for
/// @param hash the computed hash for the provided path
/// @param path the path to the local file being compared
void BlockedModsDialog::checkMatchHash(QString hash, QString path)
{
    bool match = false;

    qDebug() << "[Blocked Mods Dialog] Checking for match on hash:" << hash << "| From path:" << path;

    auto downloadDir = QFileInfo(APPLICATION->settings()->get("DownloadsDir").toString()).absoluteFilePath();
    auto moveFiles = APPLICATION->settings()->get("MoveModsFromDownloadsDir").toBool();
    for (auto& mod : m_mods) {
        if (mod.matched) {
            continue;
        }
        if (mod.hash.compare(hash, Qt::CaseInsensitive) == 0) {
            mod.matched = true;
            mod.localPath = path;
            if (moveFiles) {
                mod.move = QFileInfo(path).absoluteFilePath().startsWith(downloadDir);
            }
            match = true;

            qDebug() << "[Blocked Mods Dialog] Hash match found:" << mod.name << hash << "| From path:" << path;

            break;
        }
    }

    if (match) {
        update();
        // guided download: once every page we opened has delivered its file, open the next one
        if (m_guided) {
            QTimer::singleShot(700, this, [this] {
                for (int i : m_opened) {
                    if (!m_mods[i].matched)
                        return;
                }
                openNextMissing();
            });
        }
    }
}

/// @brief Check if the name of the file at path matches the name of a blocked mod we are searching for
/// @param path the path to check
/// @return boolean: did the path match the name of a blocked mod?
bool BlockedModsDialog::checkValidPath(QString path)
{
    const QFileInfo file = QFileInfo(path);
    const QString filename = file.fileName();

    auto compare = [](QString fsFilename, QString metadataFilename) {
        return metadataFilename.compare(fsFilename, Qt::CaseInsensitive) == 0;
    };

    // super lax compare (but not fuzzy)
    // convert to lowercase
    // convert all speratores to whitespace
    // simplify sequence of internal whitespace to a single space
    // efectivly compare two strings ignoring all separators and case
    auto laxCompare = [](QString fsfilename, QString metadataFilename) {
        // allowed character seperators
        QList<QChar> allowedSeperators = { '-', '+', '.', '_' };

        // copy in lowercase
        auto fsName = fsfilename.toLower();
        auto metaName = metadataFilename.toLower();

        // replace all potential allowed seperatores with whitespace
        for (auto sep : allowedSeperators) {
            fsName = fsName.replace(sep, ' ');
            metaName = metaName.replace(sep, ' ');
        }

        // remove extraneous whitespace
        fsName = fsName.simplified();
        metaName = metaName.simplified();

        return fsName.compare(metaName) == 0;
    };

    auto downloadDir = QFileInfo(APPLICATION->settings()->get("DownloadsDir").toString()).absoluteFilePath();
    auto moveFiles = APPLICATION->settings()->get("MoveModsFromDownloadsDir").toBool();
    for (auto& mod : m_mods) {
        if (compare(filename, mod.name)) {
            // if the mod is not yet matched and doesn't have a hash then
            // just match it with the file that has the exact same name
            if (!mod.matched && mod.hash.isEmpty()) {
                mod.matched = true;
                mod.localPath = path;
                if (moveFiles) {
                    mod.move = QFileInfo(path).absoluteFilePath().startsWith(downloadDir);
                }
                return false;
            }
            qDebug() << "[Blocked Mods Dialog] Name match found:" << mod.name << "| From path:" << path;
            return true;
        }
        if (laxCompare(filename, mod.name)) {
            qDebug() << "[Blocked Mods Dialog] Lax name match found:" << mod.name << "| From path:" << path;
            return true;
        }
    }

    return false;
}

bool BlockedModsDialog::allModsMatched()
{
    return std::all_of(m_mods.begin(), m_mods.end(), [](auto const& mod) { return mod.matched; });
}

/// @brief ensure matched file paths still exist
void BlockedModsDialog::validateMatchedMods()
{
    bool changed = false;
    for (auto& mod : m_mods) {
        if (mod.matched) {
            QFileInfo file = QFileInfo(mod.localPath);
            if (!file.exists() || !file.isFile()) {
                qDebug() << "[Blocked Mods Dialog] File" << mod.localPath << "for mod" << mod.name
                         << "has vanshed! marking as not matched.";
                mod.localPath = "";
                mod.matched = false;
                changed = true;
            }
        }
    }
    if (changed) {
        update();
    }
}

/// @brief run hash task or mark a pending run if it is already running
void BlockedModsDialog::runHashTask()
{
    if (!m_hashingTask->isRunning()) {
        m_rehashPending = false;

        if (!m_pendingHashPaths.isEmpty()) {
            qDebug() << "[Blocked Mods Dialog] there are pending hash tasks, building and running tasks";

            auto path = m_pendingHashPaths.begin();
            while (path != m_pendingHashPaths.end()) {
                buildHashTask(*path);
                path = m_pendingHashPaths.erase(path);
            }

            m_hashingTask->start();
        }
    } else {
        qDebug() << "[Blocked Mods Dialog] queueing another run of the hashing task";
        qDebug() << "[Blocked Mods Dialog] pending hash tasks:" << m_pendingHashPaths;
        m_rehashPending = true;
    }
}

void BlockedModsDialog::hashTaskFinished()
{
    qDebug() << "[Blocked Mods Dialog] All hash tasks finished";
    if (m_rehashPending) {
        qDebug() << "[Blocked Mods Dialog] task finished with a rehash pending, rerunning";
        runHashTask();
    }
}

/// qDebug print support for the BlockedMod struct
QDebug operator<<(QDebug debug, const BlockedMod& m)
{
    QDebugStateSaver saver(debug);

    debug.nospace() << "{ name: " << m.name << ", websiteUrl: " << m.websiteUrl << ", hash: " << m.hash << ", matched: " << m.matched
                    << ", localPath: " << m.localPath << "}";

    return debug;
}

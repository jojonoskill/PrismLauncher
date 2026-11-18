// SPDX-License-Identifier: GPL-3.0-only
#include "server/ServerInstallTask.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QtConcurrent>

#include "Application.h"
#include "FileSystem.h"
#include "archive/ArchiveReader.h"
#include "archive/ExtractZipTask.h"
#include "net/NetJob.h"
#include "server/ServerInstance.h"
#include "server/ServerList.h"
#include "settings/SettingsObject.h"

namespace {

struct ZipScan {
    bool ok = false;
    QString subdir;  // "Folder/" when the archive holds exactly one top-level folder, else empty
};

ZipScan scanZip(const QString& zipPath)
{
    ZipScan scan;
    MMCZip::ArchiveReader reader(zipPath);
    if (!reader.collectFiles(false))
        return scan;
    scan.ok = true;

    QSet<QString> topLevel;
    bool rootFiles = false;
    for (auto name : reader.getFiles()) {
        name = QDir::fromNativeSeparators(name);
        while (name.startsWith("./"))
            name = name.mid(2);
        while (name.startsWith('/'))
            name = name.mid(1);
        if (name.isEmpty() || name.startsWith("__MACOSX/") || name == "__MACOSX")
            continue;
        auto slash = name.indexOf('/');
        if (slash < 0) {
            rootFiles = true;
            break;
        }
        topLevel.insert(name.left(slash));
    }
    if (!rootFiles && topLevel.size() == 1) {
        auto folder = *topLevel.begin();
        // a pack that only consists of e.g. mods/ must keep its structure
        static const QStringList serverFolders = { "mods", "config", "libraries", "world", "plugins", "scripts", "defaultconfigs" };
        if (!serverFolders.contains(folder.toLower()))
            scan.subdir = folder + '/';
    }
    return scan;
}

struct CopyResult {
    bool ok = false;
    QStringList failed;
};

QString guessMinecraftVersion(const QString& serverDir, const ServerLaunchProfile& profile)
{
    static const QRegularExpression forgeLib(R"(libraries/net/(?:minecraftforge|neoforged)/forge/(1\.\d+(?:\.\d+)?)-)");
    if (auto m = forgeLib.match(profile.target); m.hasMatch())
        return m.captured(1);

    QDir root(serverDir);
    static const QRegularExpression vanillaJar(R"(^minecraft_server\.(1\.\d+(?:\.\d+)?)\.jar$)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression forgeJar(R"(^forge-(1\.\d+(?:\.\d+)?)-)", QRegularExpression::CaseInsensitiveOption);
    const auto jars = root.entryList({ "*.jar" }, QDir::Files);
    for (const auto* re : { &vanillaJar, &forgeJar }) {
        for (const auto& jar : jars) {
            if (auto m = re->match(jar); m.hasMatch())
                return m.captured(1);
        }
    }
    // modern Forge / NeoForge installs keep the vanilla server in libraries/net/minecraft/server/<version>/
    QDir mcLibs(root.filePath("libraries/net/minecraft/server"));
    if (mcLibs.exists()) {
        static const QRegularExpression version(R"(^(1\.\d+(?:\.\d+)?)(?:-|$))");
        for (const auto& dir : mcLibs.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (auto m = version.match(dir); m.hasMatch())
                return m.captured(1);
        }
    }
    return {};
}

}  // namespace

ServerInstallTask::ServerInstallTask(ServerList* list, const QString& name, SourceKind kind, const QString& source)
    : m_list(list), m_name(name), m_kind(kind), m_source(source)
{}

void ServerInstallTask::executeTask()
{
    setAbortable(true);
    setProgress(0, 0);

    QString sourceText;
    switch (m_kind) {
        case SourceKind::ZipFile: {
            QFileInfo info(m_source);
            if (!info.isFile()) {
                emitFailed(tr("The server pack %1 does not exist.").arg(QDir::toNativeSeparators(m_source)));
                return;
            }
            sourceText = tr("Server pack: %1").arg(info.fileName());
            break;
        }
        case SourceKind::Url: {
            QUrl url(m_source.trimmed());
            if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) {
                emitFailed(tr("%1 is not a valid download link.").arg(m_source));
                return;
            }
            sourceText = url.toString();
            break;
        }
        case SourceKind::Folder: {
            QFileInfo info(m_source);
            if (!info.isDir()) {
                emitFailed(tr("The folder %1 does not exist.").arg(QDir::toNativeSeparators(m_source)));
                return;
            }
            auto src = QDir::cleanPath(info.absoluteFilePath());
            auto servers = QDir::cleanPath(QDir(m_list->serversDir()).absolutePath());
#ifdef Q_OS_WIN
            const auto cs = Qt::CaseInsensitive;
#else
            const auto cs = Qt::CaseSensitive;
#endif
            // copying a folder into itself would never end
            if (servers.startsWith(src + '/', cs) || servers.compare(src, cs) == 0) {
                emitFailed(tr("The folder %1 contains Elytra's servers folder and cannot be copied.").arg(QDir::toNativeSeparators(src)));
                return;
            }
            sourceText = tr("Copied from %1").arg(QDir::toNativeSeparators(src));
            break;
        }
    }

    setStatus(tr("Creating server..."));
    m_result = m_list->create(m_name);
    if (!m_result) {
        emitFailed(tr("Could not create the server folder in %1.").arg(QDir::toNativeSeparators(m_list->serversDir())));
        return;
    }
    m_result->settings()->set("Source", sourceText);
    qInfo() << "[Server] Installing" << m_result->id() << "from" << sourceText;

    switch (m_kind) {
        case SourceKind::ZipFile:
            extractZip(m_source, false);
            break;
        case SourceKind::Folder:
            copyFolder();
            break;
        case SourceKind::Url: {
            QUrl url(m_source.trimmed());
            auto zipPath = FS::PathCombine(m_result->rootDir(), "download.zip");
            auto job = makeShared<NetJob>(tr("Server pack download"), APPLICATION->network());
            job->addNetAction(Net::Download::makeFile(url, zipPath));
            connect(job.get(), &NetJob::succeeded, this, [this, zipPath] {
                m_current.reset();
                if (m_aborting) {
                    abortInstall();
                    return;
                }
                extractZip(zipPath, true);
            });
            connect(job.get(), &NetJob::failed, this, [this](const QString& reason) { failInstall(tr("Download failed: %1").arg(reason)); });
            connect(job.get(), &NetJob::aborted, this, &ServerInstallTask::abortInstall);
            connect(job.get(), &NetJob::progress, this, &ServerInstallTask::setProgress);
            connect(job.get(), &NetJob::stepProgress, this, &ServerInstallTask::propagateStepProgress);
            setStatus(tr("Downloading %1...").arg(url.fileName().isEmpty() ? url.host() : url.fileName()));
            m_current = job;
            job->start();
            break;
        }
    }
}

void ServerInstallTask::extractZip(const QString& zipPath, bool deleteZip)
{
    setStatus(tr("Reading server pack..."));
    setProgress(0, 0);
    m_busy = true;

    auto watcher = new QFutureWatcher<ZipScan>(this);
    connect(watcher, &QFutureWatcher<ZipScan>::finished, this, [this, watcher, zipPath, deleteZip] {
        watcher->deleteLater();
        m_busy = false;
        if (m_aborting) {
            abortInstall();
            return;
        }
        auto scan = watcher->result();
        if (!scan.ok) {
            failInstall(tr("Could not read %1. Is it a zip file?").arg(QFileInfo(zipPath).fileName()));
            return;
        }
        if (!scan.subdir.isEmpty())
            qDebug() << "[Server] Server pack has a single top-level folder, extracting" << scan.subdir;

        setStatus(tr("Extracting server pack..."));
        auto task = makeShared<MMCZip::ExtractZipTask>(zipPath, QDir(m_result->serverDir()), scan.subdir);
        connect(task.get(), &Task::progress, this, &ServerInstallTask::setProgress);
        connect(task.get(), &Task::status, this, &ServerInstallTask::setDetails);
        connect(task.get(), &Task::warningLogged, this, &ServerInstallTask::logWarning);
        connect(task.get(), &Task::succeeded, this, [this, zipPath, deleteZip] {
            m_current.reset();
            if (deleteZip && !QFile::remove(zipPath))
                qWarning() << "[Server] Could not delete downloaded zip" << zipPath;
            if (m_aborting) {
                abortInstall();
                return;
            }
            finishInstall();
        });
        connect(task.get(), &Task::failed, this, [this](const QString& reason) { failInstall(tr("Extraction failed: %1").arg(reason)); });
        connect(task.get(), &Task::aborted, this, &ServerInstallTask::abortInstall);
        m_current = task;
        task->start();
    });
    watcher->setFuture(QtConcurrent::run([zipPath] { return scanZip(zipPath); }));
}

void ServerInstallTask::copyFolder()
{
    const auto src = QDir(m_source).absolutePath();
    const auto dst = m_result->serverDir();
    setStatus(tr("Copying server files..."));
    setProgress(0, 0);
    m_busy = true;

    auto watcher = new QFutureWatcher<CopyResult>(this);
    connect(watcher, &QFutureWatcher<CopyResult>::finished, this, [this, watcher] {
        watcher->deleteLater();
        m_busy = false;
        if (m_aborting) {
            abortInstall();
            return;
        }
        auto result = watcher->result();
        // a running server keeps session.lock open; that file is not needed in the copy
        result.failed.removeIf([](const QString& path) { return QFileInfo(path).fileName() == "session.lock"; });
        if (!result.failed.isEmpty()) {
            auto shown = result.failed.mid(0, 5);
            for (auto& p : shown)
                p = QDir::toNativeSeparators(p);
            failInstall(tr("Could not copy %1 files, for example:\n%2").arg(result.failed.size()).arg(shown.join('\n')));
            return;
        }
        finishInstall();
    });

    QPointer<ServerInstallTask> self(this);
    watcher->setFuture(QtConcurrent::run([self, src, dst] {
        CopyResult result;
        qint64 total = 0;
        {
            QDirIterator it(src, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                ++total;
            }
        }
        qint64 done = 0;
        FS::copy copy(src, dst);
        // runs in this worker thread (no receiver context): only post progress to the task
        QObject::connect(&copy, &FS::copy::fileCopied, [&done, total, self](const QString&) {
            ++done;
            if (done % 25 == 0 || done == total) {
                QMetaObject::invokeMethod(
                    self, [self, done, total] {
                        if (self)
                            self->setProgress(done, total);
                    },
                    Qt::QueuedConnection);
            }
        });
        copy();
        result.failed = copy.failed();
        result.ok = result.failed.isEmpty();
        return result;
    }));
}

void ServerInstallTask::finishInstall()
{
    if (!isRunning())
        return;
    setStatus(tr("Detecting how to start the server..."));
    setDetails(QString());
    auto profile = m_result->redetectLaunchProfile();
    if (!profile.isValid())
        logWarning(tr("Could not detect how to start this server. Pick the server jar in the server's settings."));

    auto mcVersion = guessMinecraftVersion(m_result->serverDir(), profile);
    if (!mcVersion.isEmpty())
        m_result->settings()->set("MinecraftVersion", mcVersion);

    qInfo() << "[Server] Installed" << m_result->id() << "profile:" << profile.toJson() << "Minecraft" << mcVersion;
    setAbortable(false);
    emitSucceeded();
}

void ServerInstallTask::removeHalfInstalled()
{
    if (!m_result)
        return;
    auto server = m_result;
    m_result = nullptr;
    qInfo() << "[Server] Removing half-installed server" << server->id();
    if (!m_list->remove(server))
        qWarning() << "[Server] Could not remove half-installed server" << server->rootDir();
}

void ServerInstallTask::failInstall(const QString& reason)
{
    if (!isRunning())
        return;
    m_current.reset();
    removeHalfInstalled();
    emitFailed(reason);
}

void ServerInstallTask::abortInstall()
{
    if (!isRunning())
        return;
    m_current.reset();
    removeHalfInstalled();
    emitAborted();
}

bool ServerInstallTask::abort()
{
    if (!isRunning())
        return false;
    m_aborting = true;
    if (m_current) {
        auto current = m_current;  // the sub task's aborted handler resets m_current
        return current->abort();
    }
    if (m_busy) {
        // the background work can't be interrupted; it is cleaned up once it finishes
        setStatus(tr("Cancelling..."));
        return true;
    }
    abortInstall();
    return true;
}

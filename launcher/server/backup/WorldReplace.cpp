// SPDX-License-Identifier: GPL-3.0-only
#include "server/backup/WorldReplace.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFutureWatcher>
#include <QPointer>
#include <QtConcurrent>
#include <algorithm>
#include <atomic>
#include <memory>

#include "server/ServerInstance.h"
#include "server/ServerProcess.h"
#include "tasks/Task.h"

namespace ServerBackupOps {

namespace {

/// Progress sink usable from a worker thread: everything is posted to the GUI thread and only touches the task there.
Progress makeProgress(Task* task)
{
    QPointer<Task> ptr(task);
    auto lastPermille = std::make_shared<std::atomic<int>>(-1);
    Progress progress;
    progress.status = [ptr](const QString& text) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [ptr, text]() {
                if (ptr)
                    ptr->setStatus(text);
            },
            Qt::QueuedConnection);
    };
    progress.progress = [ptr, lastPermille](qint64 current, qint64 total) {
        const int permille = total > 0 ? static_cast<int>(std::clamp<qint64>(current * 1000 / total, 0, 1000)) : 0;
        if (permille == lastPermille->exchange(permille))
            return;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [ptr, permille]() {
                if (ptr)
                    ptr->setProgress(permille, 1000);
            },
            Qt::QueuedConnection);
    };
    return progress;
}

bool serverActive(const QPointer<ServerInstance>& server)
{
    return !server || (server->process() && server->process()->isActive());
}

}  // namespace

void runReplace(Task* task, ServerInstance* server, const ReplaceRequest& request, std::function<void(const ReplaceResult&)> done)
{
    const auto progress = makeProgress(task);
    QPointer<ServerInstance> srv(server);

    auto* prepareWatcher = new QFutureWatcher<ReplaceResult>(task);
    QObject::connect(prepareWatcher, &QFutureWatcherBase::finished, task, [task, srv, request, progress, done, prepareWatcher]() {
        const auto prepared = prepareWatcher->result();
        prepareWatcher->deleteLater();
        if (!prepared.ok) {
            done(prepared);
            return;
        }
        if (serverActive(srv)) {
            qWarning() << "[Server Backup] The server was started while the new world was being prepared; aborting before the swap";
            auto* discardWatcher = new QFutureWatcher<void>(task);
            QObject::connect(discardWatcher, &QFutureWatcherBase::finished, task, [discardWatcher, done]() {
                discardWatcher->deleteLater();
                ReplaceResult result;
                result.error = QCoreApplication::translate("ServerBackups", "The server was started in the meantime. Nothing was changed.");
                done(result);
            });
            discardWatcher->setFuture(QtConcurrent::run(QThreadPool::globalInstance(),
                                                        [staging = prepared.stagingDir]() { discardStaging(staging); }));
            return;
        }
        auto* swapWatcher = new QFutureWatcher<ReplaceResult>(task);
        QObject::connect(swapWatcher, &QFutureWatcherBase::finished, task, [swapWatcher, done]() {
            const auto result = swapWatcher->result();
            swapWatcher->deleteLater();
            done(result);
        });
        swapWatcher->setFuture(QtConcurrent::run(QThreadPool::globalInstance(), [request, staging = prepared.stagingDir, progress]() {
            return swap(request, staging, progress);
        }));
    });
    prepareWatcher->setFuture(QtConcurrent::run(QThreadPool::globalInstance(), [request, progress]() { return prepare(request, progress); }));
}

}  // namespace ServerBackupOps

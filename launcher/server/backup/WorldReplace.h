// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>

#include "server/backup/BackupWorldOps.h"

class Task;
class ServerInstance;

namespace ServerBackupOps {

/// Runs prepare() on a worker thread, re-checks on the GUI thread that the server is still stopped, then runs swap() on a
/// worker thread. Status/progress go to task. done() is called on the GUI thread (not at all if task was destroyed).
void runReplace(Task* task, ServerInstance* server, const ReplaceRequest& request, std::function<void(const ReplaceResult&)> done);

}  // namespace ServerBackupOps

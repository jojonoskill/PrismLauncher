// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

#include "tasks/Task.h"

class ServerInstance;
class ServerList;

/// Installs a new server:
///   create entry (ServerList::create) -> download (Url) -> extract zip into serverDir (if the zip has a single top-level
///   folder, its contents go to serverDir) / or copy the folder (Folder) -> ServerLaunchDetector -> store profile.
/// The EULA is NOT accepted here; the UI calls ServerInstance::acceptEula() after the user agreed.
/// On failure or abort the half-created server entry is removed again.
class ServerInstallTask : public Task {
    Q_OBJECT
   public:
    enum class SourceKind {
        ZipFile,  // local server pack zip
        Url,      // http(s) link to a server pack zip
        Folder,   // existing server folder: copied into the new server (the original is left untouched)
    };

    ServerInstallTask(ServerList* list, const QString& name, SourceKind kind, const QString& source);

    /// valid after succeeded()
    ServerInstance* result() const { return m_result; }

   protected:
    void executeTask() override;
    bool abort() override;

   private:
    ServerList* m_list;
    QString m_name;
    SourceKind m_kind;
    QString m_source;
    ServerInstance* m_result = nullptr;
    Task::Ptr m_current;  // currently running sub task (download / extract), for abort()
    bool m_aborting = false;  // abort requested while background work (zip scan / folder copy) runs
    bool m_busy = false;      // background work is running

    void extractZip(const QString& zipPath, bool deleteZip);
    void copyFolder();
    void finishInstall();
    void failInstall(const QString& reason);
    void abortInstall();
    void removeHalfInstalled();
};

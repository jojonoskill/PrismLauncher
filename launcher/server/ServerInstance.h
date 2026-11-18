// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "server/ServerLaunchProfile.h"

class SettingsObject;
class ServerProcess;

/// One managed server. On disk:
///   <data>/servers/<id>/server.cfg   Elytra's settings for it (INI)
///   <data>/servers/<id>/server/      the actual server installation (server.properties, mods, world, backups...)
///
/// server.cfg keys (registered in the constructor):
///   Name (QString), JavaPath (QString, empty = launcher's global Java), MinMemAlloc (int MiB, 0 = same as max),
///   MaxMemAlloc (int MiB, default 4096), JvmArgs (QString, extra, appended after the profile's),
///   LaunchProfile (QString JSON of ServerLaunchProfile), AutoRestart (bool, default false),
///   Source (QString, free text: where it was installed from), MinecraftVersion (QString, may be empty)
class ServerInstance : public QObject {
    Q_OBJECT
   public:
    explicit ServerInstance(const QString& rootDir, QObject* parent = nullptr);
    ~ServerInstance() override;

    QString id() const;         // folder name under servers/
    QString rootDir() const;    // <data>/servers/<id>
    QString serverDir() const;  // <data>/servers/<id>/server

    QString name() const;
    void setName(const QString& name);

    SettingsObject* settings() const { return m_settings.get(); }

    ServerLaunchProfile launchProfile() const;
    void setLaunchProfile(const ServerLaunchProfile& profile);
    /// Runs ServerLaunchDetector on serverDir() and stores the result. Returns it.
    ServerLaunchProfile redetectLaunchProfile();

    /// Java executable to use: JavaPath setting, else the launcher's global JavaPath setting.
    QString javaPath() const;

    /// level-name from server.properties ("world" if missing)
    QString worldName() const;
    /// serverDir()/worldName()
    QString worldDir() const;

    bool eulaAccepted() const;
    /// writes eula=true to serverDir()/eula.txt (only call after the user ticked the EULA box)
    void acceptEula();

    ServerProcess* process() const { return m_process; }

   signals:
    void nameChanged(const QString& name);
    /// the start command changed (install finished, "Detect again", "Choose jar...")
    void launchProfileChanged();

   private:
    QString m_rootDir;
    std::unique_ptr<SettingsObject> m_settings;
    ServerProcess* m_process = nullptr;
};

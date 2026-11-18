// SPDX-License-Identifier: GPL-3.0-only
#include "server/ServerInstance.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "Application.h"
#include "FileSystem.h"
#include "server/ServerProcess.h"
#include "settings/INISettingsObject.h"

ServerInstance::ServerInstance(const QString& rootDir, QObject* parent) : QObject(parent), m_rootDir(QDir(rootDir).absolutePath())
{
    m_settings = std::make_unique<INISettingsObject>(FS::PathCombine(m_rootDir, "server.cfg"));
    m_settings->registerSetting("Name", "");
    m_settings->registerSetting("JavaPath", "");
    m_settings->registerSetting("MinMemAlloc", 0);
    m_settings->registerSetting("MaxMemAlloc", 4096);
    m_settings->registerSetting("JvmArgs", "");
    m_settings->registerSetting("LaunchProfile", "");
    m_settings->registerSetting("AutoRestart", false);
    m_settings->registerSetting("Source", "");
    m_settings->registerSetting("MinecraftVersion", "");

    m_process = new ServerProcess(this);
}

ServerInstance::~ServerInstance()
{
    // the process may still need the instance (settings, paths) while shutting down, so it goes first
    delete m_process;
    m_process = nullptr;
}

QString ServerInstance::id() const
{
    return QFileInfo(m_rootDir).fileName();
}

QString ServerInstance::rootDir() const
{
    return m_rootDir;
}

QString ServerInstance::serverDir() const
{
    return FS::PathCombine(m_rootDir, "server");
}

QString ServerInstance::name() const
{
    auto name = m_settings->get("Name").toString();
    return name.isEmpty() ? id() : name;
}

void ServerInstance::setName(const QString& name)
{
    auto trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == m_settings->get("Name").toString())
        return;
    m_settings->set("Name", trimmed);
    emit nameChanged(trimmed);
}

ServerLaunchProfile ServerInstance::launchProfile() const
{
    return ServerLaunchProfile::fromJson(m_settings->get("LaunchProfile").toString());
}

void ServerInstance::setLaunchProfile(const ServerLaunchProfile& profile)
{
    m_settings->set("LaunchProfile", profile.toJson());
    emit launchProfileChanged();
}

ServerLaunchProfile ServerInstance::redetectLaunchProfile()
{
    auto profile = ServerLaunchDetector::detect(serverDir());
    setLaunchProfile(profile);
    return profile;
}

QString ServerInstance::javaPath() const
{
    auto path = m_settings->get("JavaPath").toString().trimmed();
    if (path.isEmpty())
        path = APPLICATION->settings()->get("JavaPath").toString().trimmed();
    if (path.isEmpty())
        return {};
    // keep the configured value when it can't be resolved, so error messages can show it
    auto resolved = FS::ResolveExecutable(path);
    return resolved.isEmpty() ? path : resolved;
}

QString ServerInstance::worldName() const
{
    QFile file(FS::PathCombine(serverDir(), "server.properties"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return "world";
    while (!file.atEnd()) {
        auto line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith('!'))
            continue;
        auto sep = line.indexOf('=');
        if (sep < 0)
            continue;
        if (line.left(sep).trimmed() != "level-name")
            continue;
        // undo java.util.Properties escaping for the common cases
        QString value;
        auto raw = line.mid(sep + 1).trimmed();
        for (qsizetype i = 0; i < raw.size(); ++i) {
            if (raw.at(i) == '\\' && i + 1 < raw.size())
                ++i;
            value += raw.at(i);
        }
        return value.isEmpty() ? QString("world") : value;
    }
    return "world";
}

QString ServerInstance::worldDir() const
{
    return FS::PathCombine(serverDir(), worldName());
}

bool ServerInstance::eulaAccepted() const
{
    QFile file(FS::PathCombine(serverDir(), "eula.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    while (!file.atEnd()) {
        auto line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith('#'))
            continue;
        auto sep = line.indexOf('=');
        if (sep < 0)
            continue;
        if (line.left(sep).trimmed().compare("eula", Qt::CaseInsensitive) == 0)
            return line.mid(sep + 1).trimmed().compare("true", Qt::CaseInsensitive) == 0;
    }
    return false;
}

void ServerInstance::acceptEula()
{
    FS::ensureFolderPathExists(serverDir());
    QSaveFile file(FS::PathCombine(serverDir(), "eula.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[Server] Could not write eula.txt for" << id() << file.errorString();
        return;
    }
    QByteArray content;
    content += "#By changing the setting below to TRUE you are indicating your agreement to our EULA "
               "(https://aka.ms/MinecraftEULA).\n";
    content += "#Accepted in Elytra Launcher on " + QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8() + "\n";
    content += "eula=true\n";
    file.write(content);
    if (!file.commit())
        qWarning() << "[Server] Could not write eula.txt for" << id() << file.errorString();
}

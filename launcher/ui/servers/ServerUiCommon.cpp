// SPDX-License-Identifier: GPL-3.0-only
#include "ServerUiCommon.h"

#include <QCoreApplication>
#include <QLocale>
#include <QStyle>
#include <QUuid>
#include <QWidget>

#include "Application.h"
#include "minecraft/auth/AccountList.h"
#include "minecraft/auth/MinecraftAccount.h"
#include "settings/SettingsObject.h"

namespace {
QString tr(const char* text, int n = -1)
{
    return QCoreApplication::translate("ServerUi", text, nullptr, n);
}
}  // namespace

namespace ServerUi {

QString stateText(ServerProcess::State state)
{
    switch (state) {
        case ServerProcess::State::Stopped:
            return tr("Stopped");
        case ServerProcess::State::Starting:
            return tr("Starting...");
        case ServerProcess::State::Running:
            return tr("Running");
        case ServerProcess::State::Stopping:
            return tr("Stopping...");
        case ServerProcess::State::Crashed:
            return tr("Crashed");
    }
    return {};
}

QColor stateColor(ServerProcess::State state)
{
    switch (state) {
        case ServerProcess::State::Running:
            return QColor(0x4a, 0xde, 0x80);
        case ServerProcess::State::Starting:
        case ServerProcess::State::Stopping:
            return QColor(0xfb, 0xbf, 0x24);
        case ServerProcess::State::Crashed:
            return QColor(0xf8, 0x71, 0x71);
        case ServerProcess::State::Stopped:
            break;
    }
    return QColor(0x71, 0x71, 0x7a);
}

QString stateRichText(ServerProcess::State state)
{
    return QString("<span style=\"color:%1\">&#9679;</span>&nbsp;%2").arg(stateColor(state).name(), stateText(state).toHtmlEscaped());
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 0)
        seconds = 0;
    const qint64 days = seconds / 86400;
    const qint64 hours = (seconds % 86400) / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (days > 0)
        return tr("%1 d %2 h").arg(days).arg(hours);
    if (hours > 0)
        return tr("%1 h %2 min").arg(hours).arg(minutes);
    if (minutes > 0)
        return tr("%1 min %2 s").arg(minutes).arg(secs);
    return tr("%1 s").arg(secs);
}

QString formatRelative(const QDateTime& when)
{
    if (!when.isValid())
        return tr("never");
    const qint64 secs = when.secsTo(QDateTime::currentDateTime());
    if (secs < 60)
        return tr("just now");
    // explicit singular/plural: "%n ...(s)" only gets proper plurals with a translation loaded, and English has none
    const int minutes = static_cast<int>(secs / 60);
    if (minutes < 60)
        return minutes == 1 ? tr("1 minute ago") : tr("%1 minutes ago").arg(minutes);
    const int hours = minutes / 60;
    if (hours < 48)
        return hours == 1 ? tr("1 hour ago") : tr("%1 hours ago").arg(hours);
    return tr("%1 days ago").arg(hours / 24);
}

QString formatDateTime(const QDateTime& when)
{
    if (!when.isValid())
        return tr("unknown");
    const QDateTime local = when.toLocalTime();
    const QLocale locale;
    return QString("%1, %2").arg(locale.toString(local.date(), "ddd d MMM yyyy"), locale.toString(local.time(), QLocale::ShortFormat));
}

QString defaultAccountUuid()
{
    auto accounts = APPLICATION->accounts();
    if (!accounts)
        return {};
    auto account = accounts->defaultAccount();
    if (!account || !account->hasProfile())
        return {};
    QString id = account->profileId().trimmed();
    if (id.size() == 32 && !id.contains('-')) {
        id = QString("%1-%2-%3-%4-%5").arg(id.mid(0, 8), id.mid(8, 4), id.mid(12, 4), id.mid(16, 4), id.mid(20, 12));
    }
    const QUuid uuid = QUuid::fromString(id);
    if (uuid.isNull())
        return {};
    return uuid.toString(QUuid::WithoutBraces);
}

QString defaultAccountName()
{
    auto accounts = APPLICATION->accounts();
    if (!accounts)
        return {};
    auto account = accounts->defaultAccount();
    if (!account || !account->hasProfile())
        return {};
    return account->profileName();
}

void ensureLauncherSetting(const QString& id, const QVariant& defaultValue)
{
    auto settings = APPLICATION->settings();
    if (!settings->contains(id))
        settings->registerSetting(id, defaultValue);
}

void repolish(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

}  // namespace ServerUi

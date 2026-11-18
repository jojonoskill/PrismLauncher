// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QColor>
#include <QDateTime>
#include <QString>
#include <QVariant>

#include "server/ServerProcess.h"

class QWidget;

/// Small helpers shared by the server management screens.
namespace ServerUi {
/// "Running", "Starting...", ...
QString stateText(ServerProcess::State state);
/// state dot color: green running, yellow starting/stopping, red crashed, grey stopped
QColor stateColor(ServerProcess::State state);
/// "<dot> Running" as rich text, for labels
QString stateRichText(ServerProcess::State state);

/// "1 h 23 min", "4 min 10 s"
QString formatDuration(qint64 seconds);
/// "just now", "12 minutes ago", "3 days ago"
QString formatRelative(const QDateTime& when);
/// "Tue 18 Nov 2026, 14:32" in local time
QString formatDateTime(const QDateTime& when);

/// The default Minecraft account's profile UUID in dashed form, or empty if there is no usable account.
QString defaultAccountUuid();
/// Name of the default account's profile (empty if none)
QString defaultAccountName();

/// Registers a launcher setting (window geometry etc.) on first use if Application didn't register it.
void ensureLauncherSetting(const QString& id, const QVariant& defaultValue = QString());

/// re-applies the style sheet after changing an objectName or dynamic property
void repolish(QWidget* widget);
}  // namespace ServerUi

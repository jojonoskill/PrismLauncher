// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
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

#include "ModernDarkTheme.h"

#include <QFile>
#include <QObject>

QString ModernDarkTheme::id()
{
    return "modern-dark";
}

QString ModernDarkTheme::name()
{
    return QObject::tr("Modern Dark");
}

QPalette ModernDarkTheme::colorScheme()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#1b1b1f"));
    palette.setColor(QPalette::WindowText, QColor("#e4e4e7"));
    palette.setColor(QPalette::Base, QColor("#141417"));
    palette.setColor(QPalette::AlternateBase, QColor("#1a1a1e"));
    palette.setColor(QPalette::ToolTipBase, QColor("#26262b"));
    palette.setColor(QPalette::ToolTipText, QColor("#e4e4e7"));
    palette.setColor(QPalette::Text, QColor("#e4e4e7"));
    palette.setColor(QPalette::Button, QColor("#26262b"));
    palette.setColor(QPalette::ButtonText, QColor("#e4e4e7"));
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Link, QColor("#4ade80"));
    palette.setColor(QPalette::Highlight, QColor("#24502f"));
    palette.setColor(QPalette::HighlightedText, QColor("#f4f4f5"));
    palette.setColor(QPalette::PlaceholderText, QColor("#71717a"));
    return fadeInactive(palette, fadeAmount(), fadeColor());
}

double ModernDarkTheme::fadeAmount()
{
    return 0.5;
}

QColor ModernDarkTheme::fadeColor()
{
    return QColor("#1b1b1f");
}

LogColors ModernDarkTheme::logColorScheme()
{
    LogColors colors = defaultLogColors(colorScheme());
    colors.foreground[MessageLevel::Launcher] = QColor("#a78bfa");
    colors.foreground[MessageLevel::Debug] = QColor("#71717a");
    colors.foreground[MessageLevel::Warning] = QColor("#fbbf24");
    colors.foreground[MessageLevel::Error] = QColor("#f87171");
    colors.foreground[MessageLevel::Fatal] = QColor("#fca5a5");
    colors.background[MessageLevel::Fatal] = QColor("#450a0a");
    return colors;
}

bool ModernDarkTheme::hasStyleSheet()
{
    return true;
}

QString ModernDarkTheme::appStyleSheet()
{
    // For fast iteration: point PRISM_THEME_DEV_QSS at the source .qss and just restart, no rebuild needed.
    QString path = qEnvironmentVariable("PRISM_THEME_DEV_QSS");
    if (path.isEmpty() || !QFile::exists(path)) {
        path = ":/themes/modern-dark/modern-dark.qss";
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QString ModernDarkTheme::tooltip()
{
    return "";
}

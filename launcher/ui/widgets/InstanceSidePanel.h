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

#pragma once

#include <QWidget>

class BaseInstance;
class QAction;
class QLabel;
class QFrame;
class QToolButton;
class QPushButton;

/// Details panel for the selected instance: header, play stats, a big launch button and a compact row of actions.
/// All buttons are driven by the main window's existing QActions, so enabled state, menus and shortcuts stay in sync.
class InstanceSidePanel : public QWidget {
    Q_OBJECT

   public:
    struct Actions {
        QAction* launch;
        QAction* kill;
        QAction* changeIcon;
        QAction* rename;
        QAction* edit;
        QAction* changeGroup;
        QAction* viewFolder;
        QAction* exportInstance;
        QAction* copy;
        QAction* deleteInstance;
        QAction* createShortcut;
    };

    explicit InstanceSidePanel(const Actions& actions, QWidget* parent = nullptr);

    /// Refreshes the name, subtitle and stats. Pass nullptr when nothing is selected.
    void setInstance(BaseInstance* instance);
    void setIcon(const QIcon& icon);

    void retranslate();

   private:
    QToolButton* makeActionButton(QAction* action);
    QFrame* makeStatCard(const QString& objectName, QLabel*& title, QLabel*& value);

    Actions m_actions;

    QToolButton* m_iconButton;
    QPushButton* m_nameButton;
    QLabel* m_subtitle;

    QWidget* m_stats;
    QLabel* m_playedTitle;
    QLabel* m_playedValue;
    QLabel* m_lastPlayedTitle;
    QLabel* m_lastPlayedValue;

    QToolButton* m_launchButton;
    QToolButton* m_moreButton;
};

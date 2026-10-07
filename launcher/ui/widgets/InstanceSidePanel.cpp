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

#include "InstanceSidePanel.h"

#include <QAction>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "Application.h"
#include "BaseInstance.h"
#include "MMCTime.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "settings/SettingsObject.h"

namespace {

// "Minecraft 1.20.1 · Fabric"
QString describeInstance(BaseInstance* instance)
{
    auto* minecraft = dynamic_cast<MinecraftInstance*>(instance);
    if (!minecraft) {
        return instance->typeName();
    }

    auto* profile = minecraft->getPackProfile();
    QString mcVersion = profile->getComponentVersion("net.minecraft");
    if (mcVersion.isEmpty()) {
        // components are loaded lazily; same as MinecraftInstance::getStatusbarDescription()
        profile->reload(Net::Mode::Offline);
        mcVersion = profile->getComponentVersion("net.minecraft");
    }
    QString text = QObject::tr("Minecraft %1").arg(mcVersion);

    static const QList<std::pair<QString, QString>> loaders = {
        { "net.neoforged", "NeoForge" },           { "net.minecraftforge", "Forge" }, { "net.fabricmc.fabric-loader", "Fabric" },
        { "org.quiltmc.quilt-loader", "Quilt" }, { "com.mumfrey.liteloader", "LiteLoader" },
    };
    for (const auto& [uid, name] : loaders) {
        if (!profile->getComponentVersion(uid).isEmpty()) {
            text += QStringLiteral(" · ") + name;
            break;
        }
    }
    return text;
}

// "Today", "Yesterday", "3 days ago" or a short date
QString describeLastLaunch(qint64 msecsSinceEpoch)
{
    if (msecsSinceEpoch <= 0) {
        return QObject::tr("Never");
    }
    const QDate date = QDateTime::fromMSecsSinceEpoch(msecsSinceEpoch).date();
    const qint64 days = date.daysTo(QDate::currentDate());
    if (days <= 0) {
        return QObject::tr("Today");
    }
    if (days == 1) {
        return QObject::tr("Yesterday");
    }
    if (days < 7) {
        return QObject::tr("%1 days ago").arg(days);
    }
    return QLocale().toString(date, QLocale::ShortFormat);
}

// three dots, drawn so it matches whatever icon theme is in use
QIcon makeMoreIcon(const QColor& color)
{
    QPixmap pixmap(QSize(40, 40));
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    for (int x : { 8, 20, 32 }) {
        painter.drawEllipse(QPointF(x, 20), 3.6, 3.6);
    }
    return QIcon(pixmap);
}

}  // namespace

InstanceSidePanel::InstanceSidePanel(const Actions& actions, QWidget* parent) : QWidget(parent), m_actions(actions)
{
    setObjectName("instanceSidePanel");
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // header: icon next to name and subtitle
    {
        auto* header = new QHBoxLayout();
        header->setSpacing(12);

        m_iconButton = new QToolButton(this);
        m_iconButton->setObjectName("instanceIconButton");
        m_iconButton->setIconSize(QSize(64, 64));
        m_iconButton->setAutoRaise(true);
        connect(m_iconButton, &QToolButton::clicked, m_actions.changeIcon, &QAction::trigger);
        header->addWidget(m_iconButton, 0, Qt::AlignVCenter);

        auto* titles = new QVBoxLayout();
        titles->setSpacing(2);
        titles->addStretch(1);
        // a push button, because tool buttons ignore text-align in stylesheets
        m_nameButton = new QPushButton(this);
        m_nameButton->setObjectName("instanceNameButton");
        m_nameButton->setFlat(true);
        m_nameButton->setCursor(Qt::PointingHandCursor);
        m_nameButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        connect(m_nameButton, &QPushButton::clicked, m_actions.rename, &QAction::trigger);
        titles->addWidget(m_nameButton);

        m_subtitle = new QLabel(this);
        m_subtitle->setObjectName("instanceSubtitle");
        m_subtitle->setWordWrap(true);
        titles->addWidget(m_subtitle);
        titles->addStretch(1);
        header->addLayout(titles, 1);

        layout->addLayout(header);
    }

    // play stats
    {
        m_stats = new QWidget(this);
        auto* stats = new QHBoxLayout(m_stats);
        stats->setContentsMargins(0, 0, 0, 0);
        stats->setSpacing(8);
        stats->addWidget(makeStatCard("playedCard", m_playedTitle, m_playedValue));
        stats->addWidget(makeStatCard("lastPlayedCard", m_lastPlayedTitle, m_lastPlayedValue));
        layout->addWidget(m_stats);
    }

    // launch
    {
        m_launchButton = new QToolButton(this);
        m_launchButton->setObjectName("launchButton");
        m_launchButton->setDefaultAction(m_actions.launch);
        m_launchButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_launchButton->setPopupMode(QToolButton::MenuButtonPopup);
        m_launchButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout->addWidget(m_launchButton);
    }

    // compact action row; rarely used actions live in the "more" menu
    {
        auto* row = new QHBoxLayout();
        row->setSpacing(4);
        row->addWidget(makeActionButton(m_actions.edit));
        row->addWidget(makeActionButton(m_actions.viewFolder));
        row->addWidget(makeActionButton(m_actions.changeGroup));
        auto* exportButton = makeActionButton(m_actions.exportInstance);
        exportButton->setPopupMode(QToolButton::InstantPopup);
        row->addWidget(exportButton);
        row->addWidget(makeActionButton(m_actions.copy));

        // kill only shows up while the game is running
        auto* killButton = makeActionButton(m_actions.kill);
        killButton->setObjectName("killButton");
        auto updateKill = [this, killButton] { killButton->setVisible(m_actions.kill->isEnabled()); };
        connect(m_actions.kill, &QAction::changed, killButton, updateKill);
        updateKill();
        row->addWidget(killButton);

        row->addStretch(1);

        auto* moreMenu = new QMenu(this);
        moreMenu->addAction(m_actions.createShortcut);
        moreMenu->addSeparator();
        moreMenu->addAction(m_actions.deleteInstance);
        m_moreButton = new QToolButton(this);
        m_moreButton->setObjectName("instanceActionButton");
        m_moreButton->setIcon(makeMoreIcon(palette().color(QPalette::WindowText)));
        m_moreButton->setIconSize(QSize(26, 26));
        m_moreButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        m_moreButton->setPopupMode(QToolButton::InstantPopup);
        m_moreButton->setMenu(moreMenu);
        row->addWidget(m_moreButton);

        layout->addLayout(row);
    }

    retranslate();
    setInstance(nullptr);
}

QToolButton* InstanceSidePanel::makeActionButton(QAction* action)
{
    auto* button = new QToolButton(this);
    button->setObjectName("instanceActionButton");
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(26, 26));
    return button;
}

QFrame* InstanceSidePanel::makeStatCard(const QString& objectName, QLabel*& title, QLabel*& value)
{
    auto* card = new QFrame(this);
    card->setObjectName(objectName);
    card->setProperty("statCard", true);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(10, 8, 10, 8);
    cardLayout->setSpacing(2);

    title = new QLabel(card);
    title->setObjectName("statTitle");
    value = new QLabel(card);
    value->setObjectName("statValue");
    cardLayout->addWidget(title);
    cardLayout->addWidget(value);
    return card;
}

void InstanceSidePanel::setIcon(const QIcon& icon)
{
    m_iconButton->setIcon(icon);
}

void InstanceSidePanel::setInstance(BaseInstance* instance)
{
    if (!instance) {
        m_nameButton->setText(tr("No instance selected"));
        m_subtitle->clear();
        m_subtitle->hide();
        m_stats->hide();
        return;
    }

    m_nameButton->setText(instance->name());
    m_subtitle->setText(describeInstance(instance));
    m_subtitle->show();

    const bool noDays = APPLICATION->settings()->get("ShowGameTimeWithoutDays").toBool();
    m_playedValue->setText(instance->totalTimePlayed() > 0 ? Time::prettifyDuration(instance->totalTimePlayed(), noDays) : tr("Not yet"));
    m_lastPlayedValue->setText(describeLastLaunch(instance->lastLaunch()));
    m_stats->setVisible(instance->settings()->get("ShowGameTime").toBool());
}

void InstanceSidePanel::retranslate()
{
    m_iconButton->setToolTip(m_actions.changeIcon->toolTip());
    m_nameButton->setToolTip(m_actions.rename->toolTip());
    m_moreButton->setToolTip(tr("More actions"));
    m_playedTitle->setText(tr("Played"));
    m_lastPlayedTitle->setText(tr("Last played"));
}

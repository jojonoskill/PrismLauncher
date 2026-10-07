// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2024 TheKodeToad <TheKodeToad@proton.me>
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

#include "HintOverrideProxyStyle.h"

#include <QPainter>
#include <QToolButton>

HintOverrideProxyStyle::HintOverrideProxyStyle(QStyle* style) : QProxyStyle(style)
{
    setObjectName(baseStyle()->objectName());
}

int HintOverrideProxyStyle::styleHint(QStyle::StyleHint hint,
                                      const QStyleOption* option,
                                      const QWidget* widget,
                                      QStyleHintReturn* returnData) const
{
    if (hint == QStyle::SH_ItemView_ActivateItemOnSingleClick)
        return 0;

    if (hint == QStyle::SH_Slider_AbsoluteSetButtons)
        return Qt::LeftButton | Qt::MiddleButton;

    if (hint == QStyle::SH_Slider_PageSetButtons)
        return Qt::RightButton;

    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

// Qt hardcodes a 4px gap between a tool button's icon and its text, which looks cramped.
// Both the plain and the stylesheet code paths draw the label through drawItemText(), so we
// nudge the text right here. QToolButton::sizeHint() doesn't know about the extra space,
// so themes should give such buttons at least ToolButtonExtraIconSpacing px of right padding.
static constexpr int ToolButtonExtraIconSpacing = 6;

void HintOverrideProxyStyle::drawItemText(QPainter* painter,
                                          const QRect& rect,
                                          int flags,
                                          const QPalette& pal,
                                          bool enabled,
                                          const QString& text,
                                          QPalette::ColorRole textRole) const
{
    QRect textRect = rect;
    if (auto* button = dynamic_cast<QToolButton*>(painter->device())) {
        if (button->toolButtonStyle() == Qt::ToolButtonTextBesideIcon && !button->icon().isNull()) {
            textRect.translate(button->layoutDirection() == Qt::RightToLeft ? -ToolButtonExtraIconSpacing : ToolButtonExtraIconSpacing, 0);
        }
    }
    QProxyStyle::drawItemText(painter, textRect, flags, pal, enabled, text, textRole);
}

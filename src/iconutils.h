// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "mainwindow_utils.h"

#include <QIcon>
#include <QPalette>
#include <QString>
#include <initializer_list>

// Defined in mainwindow_utils.cpp.
bool systemUsesDarkColorScheme();
bool widgetChromeUsesDarkColorScheme();

namespace IconUtils {

// Returns a QIcon: the bundled SVG recolored to match QPalette::WindowText.
inline QIcon themeIcon(std::initializer_list<const char*> /*names*/,
                       const QString& lightResource,
                       const QString& /*darkResource*/)
{
    const QColor color = qApp
        ? qApp->palette().color(QPalette::WindowText)
        : QColor(QStringLiteral("#444444"));
    return makeRecoloredSvgIcon(lightResource, color);
}

inline QIcon themeIcon(const char* name,
                       const QString& lightResource,
                       const QString& darkResource)
{
    return themeIcon({name}, lightResource, darkResource);
}

inline QIcon themeIcon(const QString& /*name*/,
                       const QString& lightResource,
                       const QString& /*darkResource*/)
{
    const QColor color = qApp
        ? qApp->palette().color(QPalette::WindowText)
        : QColor(QStringLiteral("#444444"));
    return makeRecoloredSvgIcon(lightResource, color);
}

} // namespace IconUtils

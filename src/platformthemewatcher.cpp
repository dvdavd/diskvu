// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "platformthemewatcher.h"

#ifdef DISKSCAPE_HAS_QT_DBUS
#include <QDBusConnection>
#include <QDBusVariant>

namespace {
constexpr auto kPortalService   = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath      = "/org/freedesktop/portal/desktop";
constexpr auto kPortalInterface = "org.freedesktop.portal.Settings";
constexpr auto kAppearanceNs    = "org.freedesktop.appearance";
constexpr auto kColorSchemeKey  = "color-scheme";
} // namespace

FreedesktopColorSchemeWatcher::FreedesktopColorSchemeWatcher(QObject* parent)
    : QObject(parent)
{
    m_available = QDBusConnection::sessionBus().connect(
        QLatin1String(kPortalService),
        QLatin1String(kPortalPath),
        QLatin1String(kPortalInterface),
        QStringLiteral("SettingChanged"),
        this,
        SLOT(onSettingChanged(QString, QString, QDBusVariant)));
}

void FreedesktopColorSchemeWatcher::onSettingChanged(
    const QString& ns, const QString& key, const QDBusVariant& value)
{
    if (ns != QLatin1String(kAppearanceNs) || key != QLatin1String(kColorSchemeKey))
        return;
    // org.freedesktop.appearance color-scheme: 0 = no preference, 1 = dark, 2 = light
    emit colorSchemeChanged(value.variant().toUInt() == 1);
}

#else // DISKSCAPE_HAS_QT_DBUS

FreedesktopColorSchemeWatcher::FreedesktopColorSchemeWatcher(QObject* parent)
    : QObject(parent)
{}

#endif // DISKSCAPE_HAS_QT_DBUS

// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#ifdef DISKSCAPE_HAS_QT_DBUS
#include <QDBusVariant>
#endif

// Subscribes to org.freedesktop.portal.Settings.SettingChanged on the session
// bus and re-emits color-scheme transitions as colorSchemeChanged(bool dark).
// Used as a fallback when Qt platform plugins do not deliver PaletteChange /
// StyleChange events (e.g. AppImage distributions without a bundled platform
// theme plugin). isAvailable() returns false and the signal never fires when
// Qt6::DBus is unavailable at build time or the portal is not on the bus.
class FreedesktopColorSchemeWatcher : public QObject {
    Q_OBJECT
public:
    explicit FreedesktopColorSchemeWatcher(QObject* parent = nullptr);
    bool isAvailable() const { return m_available; }

signals:
    void colorSchemeChanged(bool dark);

private:
    bool m_available = false;

#ifdef DISKSCAPE_HAS_QT_DBUS
private slots:
    void onSettingChanged(const QString& ns, const QString& key,
                          const QDBusVariant& value);
#endif
};

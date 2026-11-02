// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "treemapsettings.h"

#include <QList>
#include <QSettings>
#include <QString>
#include <QStringList>

// A remote machine (reached over ssh, scanned by diskvu-agent) plus the
// options used when scanning it. Saved profiles appear on the landing page.
// A default-constructed profile (empty host) means "local scan".
struct RemoteProfile {
    QString name;                 // display name; empty = derive from host
    QString host;                 // ssh destination (user@host or ssh config alias)
    QString path = QStringLiteral("/");
    QString agentCommand = QStringLiteral("diskvu-agent");
    bool compress = true;

    // Per-host overrides of the global scan settings. When the "use global"
    // flag is set the matching global value applies and the override is kept
    // (but ignored) so toggling back and forth does not lose the list.
    bool useGlobalExcludes = true;
    QStringList excludedPaths;
    bool useGlobalSameFilesystem = true;
    bool limitToSameFilesystem = false;

    bool isRemote() const { return !host.isEmpty(); }
    QString displayName() const { return name.trimmed().isEmpty() ? host : name.trimmed(); }

    // Identity of a saved profile (a host can be saved with several paths).
    bool sameTarget(const RemoteProfile& other) const
    {
        return host == other.host && path == other.path;
    }

    // `global` with this profile's overrides applied.
    TreemapSettings effectiveSettings(const TreemapSettings& global) const;

    // Blank profile seeded with the last-used agent command / compression.
    static RemoteProfile defaultsFromSettings();

    static QList<RemoteProfile> loadAll(QSettings& store);
    static void saveAll(QSettings& store, const QList<RemoteProfile>& profiles);
};

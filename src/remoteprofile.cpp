// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "remoteprofile.h"

namespace {
const QString kGroup = QStringLiteral("remote/profiles");
}

TreemapSettings RemoteProfile::effectiveSettings(const TreemapSettings& global) const
{
    TreemapSettings s = global;
    if (!useGlobalExcludes)
        s.excludedPaths = excludedPaths;
    if (!useGlobalSameFilesystem)
        s.limitToSameFilesystem = limitToSameFilesystem;
    return s;
}

QList<RemoteProfile> RemoteProfile::loadAll(QSettings& store)
{
    QList<RemoteProfile> out;
    const int n = store.beginReadArray(kGroup);
    for (int i = 0; i < n; ++i) {
        store.setArrayIndex(i);
        RemoteProfile p;
        p.host = store.value(QStringLiteral("host")).toString().trimmed();
        if (p.host.isEmpty())
            continue;
        p.name = store.value(QStringLiteral("name")).toString();
        p.path = store.value(QStringLiteral("path"), QStringLiteral("/")).toString();
        p.agentCommand = store.value(QStringLiteral("agentCommand"),
                                     QStringLiteral("diskvu-agent")).toString();
        p.compress = store.value(QStringLiteral("compress"), true).toBool();
        p.useGlobalExcludes = store.value(QStringLiteral("useGlobalExcludes"), true).toBool();
        p.excludedPaths = store.value(QStringLiteral("excludedPaths")).toStringList();
        p.useGlobalSameFilesystem = store.value(QStringLiteral("useGlobalSameFilesystem"), true).toBool();
        p.limitToSameFilesystem = store.value(QStringLiteral("limitToSameFilesystem"), false).toBool();
        out.push_back(p);
    }
    store.endArray();
    return out;
}

void RemoteProfile::saveAll(QSettings& store, const QList<RemoteProfile>& profiles)
{
    store.beginWriteArray(kGroup, profiles.size());
    for (int i = 0; i < profiles.size(); ++i) {
        store.setArrayIndex(i);
        const RemoteProfile& p = profiles.at(i);
        store.setValue(QStringLiteral("host"), p.host);
        store.setValue(QStringLiteral("name"), p.name);
        store.setValue(QStringLiteral("path"), p.path);
        store.setValue(QStringLiteral("agentCommand"), p.agentCommand);
        store.setValue(QStringLiteral("compress"), p.compress);
        store.setValue(QStringLiteral("useGlobalExcludes"), p.useGlobalExcludes);
        store.setValue(QStringLiteral("excludedPaths"), p.excludedPaths);
        store.setValue(QStringLiteral("useGlobalSameFilesystem"), p.useGlobalSameFilesystem);
        store.setValue(QStringLiteral("limitToSameFilesystem"), p.limitToSameFilesystem);
    }
    store.endArray();
}

RemoteProfile RemoteProfile::defaultsFromSettings()
{
    QSettings settings;
    RemoteProfile p;
    p.agentCommand = settings.value(QStringLiteral("remote/agentCommand"),
                                    QStringLiteral("diskvu-agent")).toString();
    p.compress = settings.value(QStringLiteral("remote/compress"), true).toBool();
    return p;
}

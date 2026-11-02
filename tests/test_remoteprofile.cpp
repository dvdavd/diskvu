// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "remoteprofile.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestRemoteProfile : public QObject {
    Q_OBJECT
private slots:
    void effectiveSettings_followsGlobalByDefault()
    {
        TreemapSettings global = TreemapSettings::defaults();
        global.excludedPaths = {QStringLiteral("/proc")};
        global.limitToSameFilesystem = true;
        RemoteProfile p;
        p.excludedPaths = {QStringLiteral("/ignored")};
        const TreemapSettings s = p.effectiveSettings(global);
        QCOMPARE(s.excludedPaths, global.excludedPaths);
        QVERIFY(s.limitToSameFilesystem);
    }

    void effectiveSettings_overrides()
    {
        TreemapSettings global = TreemapSettings::defaults();
        global.excludedPaths = {QStringLiteral("/proc")};
        global.limitToSameFilesystem = true;
        RemoteProfile p;
        p.useGlobalExcludes = false;
        p.excludedPaths = {QStringLiteral("/mnt/nas")};
        p.useGlobalSameFilesystem = false;
        p.limitToSameFilesystem = false;
        const TreemapSettings s = p.effectiveSettings(global);
        QCOMPARE(s.excludedPaths, QStringList{QStringLiteral("/mnt/nas")});
        QVERIFY(!s.limitToSameFilesystem);
    }

    void saveLoad_roundTrip()
    {
        QTemporaryDir dir;
        QSettings store(dir.filePath(QStringLiteral("t.ini")), QSettings::IniFormat);
        RemoteProfile a;
        a.name = QStringLiteral("NAS");
        a.host = QStringLiteral("me@nas");
        a.path = QStringLiteral("/srv");
        a.compress = false;
        a.useGlobalExcludes = false;
        a.excludedPaths = {QStringLiteral("/srv/tmp"), QStringLiteral("/srv/cache")};
        a.useGlobalSameFilesystem = false;
        a.limitToSameFilesystem = true;
        RemoteProfile b;
        b.host = QStringLiteral("box");
        RemoteProfile::saveAll(store, {a, b});

        const QList<RemoteProfile> loaded = RemoteProfile::loadAll(store);
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded[0].displayName(), QStringLiteral("NAS"));
        QCOMPARE(loaded[0].path, a.path);
        QVERIFY(!loaded[0].compress);
        QVERIFY(!loaded[0].useGlobalExcludes);
        QCOMPARE(loaded[0].excludedPaths, a.excludedPaths);
        QVERIFY(!loaded[0].useGlobalSameFilesystem);
        QVERIFY(loaded[0].limitToSameFilesystem);
        QCOMPARE(loaded[1].displayName(), QStringLiteral("box"));
        QVERIFY(loaded[1].useGlobalExcludes);
    }
};

QTEST_MAIN(TestRemoteProfile)
#include "test_remoteprofile.moc"

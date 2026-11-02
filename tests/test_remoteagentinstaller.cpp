// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "remoteagentinstaller.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using Installer = RemoteAgentInstaller;

class TestRemoteAgentInstaller : public QObject {
    Q_OBJECT
private slots:
    void parseUname_normalisesArch()
    {
        auto p = Installer::parseUname(QStringLiteral("Linux x86_64\n"));
        QCOMPARE(p.os, QStringLiteral("linux"));
        QCOMPARE(p.arch, QStringLiteral("x86_64"));
        QCOMPARE(Installer::parseUname(QStringLiteral("Linux amd64")).arch, QStringLiteral("x86_64"));
        QCOMPARE(Installer::parseUname(QStringLiteral("Linux arm64")).arch, QStringLiteral("aarch64"));
        QCOMPARE(Installer::parseUname(QStringLiteral("Linux aarch64")).arch, QStringLiteral("aarch64"));
    }

    void parseUname_rejectsGarbage()
    {
        QVERIFY(!Installer::parseUname(QString()).isValid());
        QVERIFY(!Installer::parseUname(QStringLiteral("bash: uname: command not found")).isValid());
        QVERIFY(!Installer::parseUname(QStringLiteral("Linux")).isValid());
    }

    void binaryName_onlyForShippedPlatforms()
    {
        QCOMPARE(Installer::binaryName({QStringLiteral("linux"), QStringLiteral("x86_64")}),
                 QStringLiteral("diskvu-agent-linux-x86_64"));
        QCOMPARE(Installer::binaryName({QStringLiteral("linux"), QStringLiteral("aarch64")}),
                 QStringLiteral("diskvu-agent-linux-aarch64"));
        QVERIFY(Installer::binaryName({QStringLiteral("linux"), QStringLiteral("riscv64")}).isEmpty());
        QVERIFY(Installer::binaryName({QStringLiteral("darwin"), QStringLiteral("arm64")}).isEmpty());
        QVERIFY(Installer::binaryName({}).isEmpty());
    }

    void findBinary_searchesInOrder()
    {
        QTemporaryDir first, second;
        const QString name = QStringLiteral("diskvu-agent-linux-x86_64");
        QFile f(second.filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();

        QCOMPARE(Installer::findBinary(name, {first.path(), second.path()}),
                 QFileInfo(f).absoluteFilePath());
        QVERIFY(Installer::findBinary(name, {first.path()}).isEmpty());
        QVERIFY(Installer::findBinary(QString(), {second.path()}).isEmpty());
    }

    void parseVersion_readsAgentOutput()
    {
        QString version;
        int protocol = 0;
        QVERIFY(Installer::parseVersion(QStringLiteral("diskvu-agent 0.9.28 (protocol 2)\n"),
                                        &version, &protocol));
        QCOMPARE(version, QStringLiteral("0.9.28"));
        QCOMPARE(protocol, 2);
        QVERIFY(!Installer::parseVersion(QStringLiteral("command not found"), &version, &protocol));
    }

    void install_rejectsBadHost()
    {
        const auto r = Installer::install(QStringLiteral("-oProxyCommand=evil"));
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
    }
};

QTEST_MAIN(TestRemoteAgentInstaller)
#include "test_remoteagentinstaller.moc"

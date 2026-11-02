// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "remotescanner.h"
#include "scanner.h"
#include "treemapsettings.h"

#include "../agent/protocol.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <atomic>
#include <map>
#include <unistd.h>

#ifndef DISKVU_AGENT_PATH
#error "DISKVU_AGENT_PATH must be defined by the build"
#endif

static bool writeFile(const QString& path, qint64 size)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QByteArray(static_cast<int>(size), 'x'));
    return true;
}

// Flattens a tree to {relative path -> "d|f:size:displaySize"} for comparison.
static void flatten(const FileNode* n, const QString& prefix, std::map<QString, QString>& out)
{
    for (const FileNode* c = n->firstChild; c; c = c->nextSibling) {
        const QString path = prefix + QLatin1Char('/') + c->name;
        out[path] = QStringLiteral("%1:%2:%3")
                        .arg(c->isDirectory() ? 'd' : 'f').arg(c->size).arg(c->displaySize);
        flatten(c, path, out);
    }
}

static RemoteScanner::Command agentCommand(const QString& path, const QStringList& extra = {})
{
    RemoteScanner::Command cmd;
    cmd.program = QStringLiteral(DISKVU_AGENT_PATH);
    cmd.arguments = extra;
    cmd.arguments << QStringLiteral("--") << path;
    return cmd;
}

class TestRemoteScanner : public QObject {
    Q_OBJECT

private slots:
    void protocol_varintRoundTrip()
    {
        std::string buf;
        const uint64_t values[] = {0, 1, 127, 128, 300, 1ull << 35, UINT64_MAX};
        for (uint64_t v : values) dva::putVarint(buf, v);
        const int64_t signedValues[] = {0, -1, 1, -123456789, INT64_MAX, INT64_MIN};
        for (int64_t v : signedValues) dva::putZigzag(buf, v);
        dva::putString(buf, "héllo");

        dva::Reader r(buf.data(), buf.size());
        for (uint64_t v : values) { uint64_t got; QVERIFY(r.getVarint(got)); QCOMPARE(got, v); }
        for (int64_t v : signedValues) { int64_t got; QVERIFY(r.getZigzag(got)); QCOMPARE(got, v); }
        std::string_view s; QVERIFY(r.getString(s)); QCOMPARE(std::string(s), std::string("héllo"));
        QVERIFY(r.atEnd());
    }

    void protocol_truncatedInputFailsCleanly()
    {
        std::string buf;
        dva::putString(buf, "abcdef");
        dva::Reader r(buf.data(), buf.size() - 2);
        std::string_view s;
        QVERIFY(!r.getString(s));
        QVERIFY(!r.ok());
    }

    void scan_matchesLocalScanner()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString base = dir.path();
        QVERIFY(writeFile(base + "/a.txt", 100));
        QVERIFY(writeFile(base + "/sub/b.bin", 2000));
        QVERIFY(writeFile(base + "/sub/deep/er/c.dat", 30));
        QVERIFY(writeFile(base + "/sub/deep/er/ünï.txt", 7));
        QVERIFY(writeFile(base + "/other/d.log", 5000));
        QDir().mkpath(base + "/emptydir");
        // hard link: second name must not be double-counted
        QVERIFY(::link(QFile::encodeName(base + "/a.txt"), QFile::encodeName(base + "/sub/a-link.txt")) == 0);
        // symlinks are skipped
        QVERIFY(QFile::link(base + "/a.txt", base + "/symlink.txt"));

        // Force a deep skeleton and many tasks by creating many directories.
        for (int i = 0; i < 40; ++i)
            QVERIFY(writeFile(QStringLiteral("%1/many/d%2/f.dat").arg(base).arg(i), 10 + i));

        const ScanResult local = Scanner::scan(base);
        QString err;
        const ScanResult remote = RemoteScanner::scan(agentCommand(base), TreemapSettings::defaults(),
                                                      {}, {}, {}, {}, nullptr, &err);
        QVERIFY2(remote.root != nullptr, qPrintable(err));
        QCOMPARE(remote.rootPath, local.rootPath);

        std::map<QString, QString> a, b;
        flatten(local.root, QString(), a);
        flatten(remote.root, QString(), b);
        QCOMPARE(b.size(), a.size());
        for (const auto& [path, value] : a) {
            QVERIFY2(b.count(path), qPrintable("missing in remote: " + path));
            QCOMPARE(b[path], value);
        }
        QCOMPARE(remote.root->size, local.root->size);
        // File counts must agree with the local scanner and with ground truth.
        QCOMPARE(remote.root->subtreeFileCount, local.root->subtreeFileCount);
        int files = 0;
        for (const auto& [path, value] : b)
            if (value.startsWith('f')) ++files;
        QCOMPARE(remote.root->subtreeFileCount, files);
        QCOMPARE(remote.fileCount, qint64(files));
        QVERIFY(remote.totalBytes > 0);
        QVERIFY(!remote.filesystems.isEmpty());
    }

    void scan_reportsProgressSnapshots()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 0; i < 30; ++i)
            QVERIFY(writeFile(QStringLiteral("%1/d%2/f.dat").arg(dir.path()).arg(i), 100));

        int snapshots = 0;
        const ScanResult remote = RemoteScanner::scan(
            agentCommand(dir.path()), TreemapSettings::defaults(),
            [&](ScanResult) { ++snapshots; });
        QVERIFY(remote.root != nullptr);
        QVERIFY(snapshots >= 1); // at least the forced final snapshot
    }

    // With --compress the stream is zstd-wrapped; the resulting tree must be
    // identical to the plain stream's.
    void scan_compressedStreamMatchesPlain()
    {
#ifndef DISKVU_HAVE_ZSTD
        QSKIP("built without zstd");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 0; i < 200; ++i)
            QVERIFY(writeFile(QStringLiteral("%1/dir%2/sub%3/file-%4.dat").arg(dir.path()).arg(i % 17).arg(i % 5).arg(i), 1 + i));

        QString err;
        const ScanResult plain = RemoteScanner::scan(agentCommand(dir.path()), TreemapSettings::defaults(),
                                                     {}, {}, {}, {}, nullptr, &err);
        QVERIFY2(plain.root != nullptr, qPrintable(err));
        const ScanResult packed = RemoteScanner::scan(agentCommand(dir.path(), {"--compress"}),
                                                      TreemapSettings::defaults(), {}, {}, {}, {},
                                                      nullptr, &err);
        QVERIFY2(packed.root != nullptr, qPrintable(err));

        std::map<QString, QString> a, b;
        flatten(plain.root, QString(), a);
        flatten(packed.root, QString(), b);
        QCOMPARE(b.size(), a.size());
        QVERIFY(a == b);
        QCOMPARE(packed.root->size, plain.root->size);
        QCOMPARE(packed.root->subtreeFileCount, plain.root->subtreeFileCount);
#endif
    }

    void agent_compressionShrinksTheStream()
    {
#ifndef DISKVU_HAVE_ZSTD
        QSKIP("built without zstd");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 0; i < 400; ++i)
            QVERIFY(writeFile(QStringLiteral("%1/project/src/module%2/component-%3.cpp").arg(dir.path()).arg(i % 9).arg(i), 4));

        auto wireBytes = [&](const QString& flag) {
            QProcess p;
            p.start("sh", {"-c", QStringLiteral("'%1' %2 -- '%3' </dev/null | wc -c")
                                      .arg(DISKVU_AGENT_PATH, flag, dir.path())});
            if (!p.waitForFinished(20000))
                return qint64(-1);
            return p.readAllStandardOutput().trimmed().toLongLong();
        };
        const qint64 plain = wireBytes(QString());
        const qint64 packed = wireBytes("--compress");
        QVERIFY(plain > 0 && packed > 0);
        QVERIFY2(packed * 2 < plain, qPrintable(QString("plain=%1 compressed=%2").arg(plain).arg(packed)));
#endif
    }

    void scan_corruptCompressedDataReportsError()
    {
#ifndef DISKVU_HAVE_ZSTD
        QSKIP("built without zstd");
#else
        // magic + a 'Z' message whose 5-byte payload is not a zstd stream
        RemoteScanner::Command cmd{QStringLiteral("sh"),
                                   {QStringLiteral("-c"), QStringLiteral("printf 'DVA1Z\\005abcde'")}};
        QString err;
        const ScanResult r = RemoteScanner::scan(cmd, TreemapSettings::defaults(), {}, {}, {}, {},
                                                 nullptr, &err);
        QVERIFY(r.root == nullptr);
        QVERIFY2(err.contains("ompress"), qPrintable(err));
#endif
    }

    // ── agent version / capability handshake ───────────────────────────────

    void scan_currentAgentProducesNoCompatibilityWarning()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.path() + "/a", 5));
        QStringList warnings;
        const ScanResult r = RemoteScanner::scan(
            agentCommand(dir.path(), {"--compress"}), TreemapSettings::defaults(), {}, {}, {},
            [&](const ScanWarning& w) { if (w.path == "diskvu-agent") warnings << w.message; });
        QVERIFY(r.root != nullptr);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join("; ")));
    }

    // A v1 agent sends a hello without version info: still works, but warns.
    void scan_oldAgentWorksButWarns()
    {
        // magic, hello(v1, "/x"), done(0, 0, 0)
        RemoteScanner::Command cmd{QStringLiteral("sh"),
            {QStringLiteral("-c"),
             QStringLiteral("printf 'DVA1H\\004\\001\\002/xD\\003\\000\\000\\000'")}};
        QStringList warnings;
        QString err;
        const ScanResult r = RemoteScanner::scan(
            cmd, TreemapSettings::defaults(), {}, {}, {},
            [&](const ScanWarning& w) { warnings << w.path + ": " + w.message; }, nullptr, &err);
        QVERIFY2(r.root != nullptr, qPrintable(err));
        QCOMPARE(r.rootPath, QString("/x"));
        QCOMPARE(warnings.size(), 1);
        QVERIFY2(warnings.first().contains("out of date"), qPrintable(warnings.first()));
    }

    void scan_newerAgentIsRejectedWithClearMessage()
    {
        RemoteScanner::Command cmd{QStringLiteral("sh"),
            {QStringLiteral("-c"), QStringLiteral("printf 'DVA1H\\004\\011\\002/x'")}};
        QString err;
        const ScanResult r = RemoteScanner::scan(cmd, TreemapSettings::defaults(), {}, {}, {}, {},
                                                 nullptr, &err);
        QVERIFY(r.root == nullptr);
        QVERIFY2(err.contains("newer") && err.contains("Update diskvu"), qPrintable(err));
    }

    void agent_versionFlagReportsVersionAndProtocol()
    {
        QProcess p;
        p.start(QStringLiteral(DISKVU_AGENT_PATH), {QStringLiteral("--version")});
        QVERIFY(p.waitForFinished(5000));
        const QString out = QString::fromUtf8(p.readAllStandardOutput());
        QVERIFY2(out.startsWith("diskvu-agent ") && out.contains("protocol 2"), qPrintable(out));
    }

    // Mirrors MainWindow: the preview slot starts closed, only a forced snapshot
    // gets through, and processing a snapshot reopens the slot. A scanner that
    // never forces its first snapshot shows no live preview at all.
    void scan_livePreviewOpensWithGuiStyleSlot()
    {
        auto slotOpen = std::make_shared<std::atomic_bool>(false);
        int snapshots = 0;
        qint64 smallestPreviewSize = -1;
        const ScanResult r = RemoteScanner::scan(
            agentCommand("/usr"), TreemapSettings::defaults(),
            [&](ScanResult snap) {
                ++snapshots;
                if (snap.root && (smallestPreviewSize < 0 || snap.root->size < smallestPreviewSize))
                    smallestPreviewSize = snap.root->size;
                slotOpen->store(true); // what processQueuedScanProgress does
            },
            [slotOpen]() { return slotOpen->load(); });
        QVERIFY(r.root != nullptr);
        QVERIFY2(snapshots >= 2, qPrintable(QString("only %1 snapshot(s)").arg(snapshots)));
        QVERIFY2(smallestPreviewSize < r.root->size, "no partial (in-progress) preview was delivered");
    }

    // The agent sends periodic progress so the status line can show live
    // activity (current directory + bytes seen) instead of jumping per chunk.
    void scan_reportsLiveActivity()
    {
        QElapsedTimer t;
        t.start();
        int calls = 0;
        qint64 lastBytes = 0;
        QString lastPath;
        const ScanResult r = RemoteScanner::scan(
            agentCommand("/usr"), TreemapSettings::defaults(), {}, {},
            [&](const QString& path, qint64 bytes) { ++calls; lastPath = path; lastBytes = bytes; });
        QVERIFY(r.root != nullptr);
        if (calls == 0 && t.elapsed() < 600)
            QSKIP("scan finished before the first progress interval");
        QVERIFY2(calls > 0, "no activity callbacks during a long scan");
        QVERIFY2(lastPath.startsWith("/usr"), qPrintable(lastPath));
        QVERIFY(lastBytes > 0);
    }

    void scan_excludeAndOneFileSystemFlags()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.path() + "/keep/a", 10));
        QVERIFY(writeFile(dir.path() + "/skip/b", 10));

        const ScanResult r = RemoteScanner::scan(
            agentCommand(dir.path(), {"--exclude", dir.path() + "/skip", "--one-file-system"}));
        QVERIFY(r.root != nullptr);
        std::map<QString, QString> m;
        flatten(r.root, QString(), m);
        QVERIFY(m.count("/keep/a"));
        QVERIFY(!m.count("/skip"));
    }

    void scan_missingDirectoryReportsError()
    {
        QString err;
        const ScanResult r = RemoteScanner::scan(agentCommand("/definitely/not/here"),
                                                 TreemapSettings::defaults(), {}, {}, {}, {},
                                                 nullptr, &err);
        QVERIFY(r.root == nullptr);
        QVERIFY2(err.contains("/definitely/not/here"), qPrintable(err));
    }

    void scan_nonAgentProgramReportsError()
    {
        RemoteScanner::Command cmd{QStringLiteral("echo"), {QStringLiteral("hello world")}};
        QString err;
        const ScanResult r = RemoteScanner::scan(cmd, TreemapSettings::defaults(), {}, {}, {}, {},
                                                 nullptr, &err);
        QVERIFY(r.root == nullptr);
        QVERIFY(!err.isEmpty());
    }

    void scan_missingProgramReportsError()
    {
        RemoteScanner::Command cmd{QStringLiteral("/no/such/program"), {}};
        QString err;
        const ScanResult r = RemoteScanner::scan(cmd, TreemapSettings::defaults(), {}, {}, {}, {},
                                                 nullptr, &err);
        QVERIFY(r.root == nullptr);
        QVERIFY(!err.isEmpty());
    }

    void scan_cancelStopsPromptly()
    {
        auto cancel = std::make_shared<std::atomic_bool>(true);
        QElapsedTimer t;
        t.start();
        const ScanResult r = RemoteScanner::scan(agentCommand("/usr"), TreemapSettings::defaults(),
                                                 {}, {}, {}, {}, cancel);
        QVERIFY(r.root == nullptr);
        QVERIFY(t.elapsed() < 5000);
    }

    // ssh hands the remote command string to the login shell. Run the exact
    // string sshCommand() produces through `sh -c` to prove quoting survives
    // awkward paths (space + apostrophe) and that --exclude reaches the agent.
    void sshCommand_remoteStringWorksUnderShell()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString base = dir.path() + QStringLiteral("/it's a dir");
        QVERIFY(writeFile(base + "/sub/f.txt", 10));
        QVERIFY(writeFile(base + "/skipme/g.txt", 10));

        TreemapSettings settings = TreemapSettings::defaults();
        settings.excludedPaths = {base + "/skipme"};
        QString agent = QStringLiteral(DISKVU_AGENT_PATH);
        agent = QStringLiteral("'") + agent.replace('\'', QStringLiteral("'\\''")) + QStringLiteral("'");

        QString err;
        const auto ssh = RemoteScanner::sshCommand("host", base, agent, settings, &err);
        QVERIFY2(!ssh.program.isEmpty(), qPrintable(err));

        RemoteScanner::Command viaShell{QStringLiteral("sh"), {QStringLiteral("-c"), ssh.arguments.last()}};
        const ScanResult r = RemoteScanner::scan(viaShell, settings, {}, {}, {}, {}, nullptr, &err);
        QVERIFY2(r.root != nullptr, qPrintable(err));
        std::map<QString, QString> m;
        flatten(r.root, QString(), m);
        QVERIFY(m.count("/sub/f.txt"));
        QVERIFY(!m.count("/skipme"));
        QVERIFY(r.rootPath.endsWith("it's a dir"));
    }

    // Opt-in timing comparison: DISKVU_BENCH_PATH=/some/big/tree ./test_remotescanner
    void benchmark_localVsAgent()
    {
        const QString path = qEnvironmentVariable("DISKVU_BENCH_PATH");
        if (path.isEmpty())
            QSKIP("set DISKVU_BENCH_PATH to run");
        QElapsedTimer t;
        t.start();
        const ScanResult local = Scanner::scan(path);
        const qint64 localMs = t.restart();
        QString err;
        const ScanResult remote = RemoteScanner::scan(agentCommand(path), TreemapSettings::defaults(),
                                                      {}, {}, {}, {}, nullptr, &err);
        const qint64 remoteMs = t.restart();
        QVERIFY2(remote.root != nullptr, qPrintable(err));
        const ScanResult packed = RemoteScanner::scan(agentCommand(path, {"--compress"}),
                                                      TreemapSettings::defaults(), {}, {}, {}, {},
                                                      nullptr, &err);
        const qint64 packedMs = t.restart();
        QVERIFY2(packed.root != nullptr, qPrintable(err));
        qInfo().noquote() << QStringLiteral("agent --compress + rebuild: %1 ms, tree equal: %2")
                                 .arg(packedMs).arg(packed.root->size == remote.root->size
                                                    && packed.root->subtreeFileCount == remote.root->subtreeFileCount);
        // Cadence of live preview snapshots during an agent scan.
        QElapsedTimer pt;
        pt.start();
        QList<qint64> stamps;
        QList<qint64> counts;
        RemoteScanner::scan(agentCommand(path), TreemapSettings::defaults(),
                            [&](ScanResult snap) { stamps << pt.elapsed(); counts << snap.fileCount; },
                            []() { return true; });
        qInfo().noquote() << "preview snapshots:" << stamps.size()
                          << "first at ms:" << (stamps.isEmpty() ? -1 : stamps.first())
                          << "files in first/mid/last:" << (counts.isEmpty() ? 0 : counts.first())
                          << (counts.isEmpty() ? 0 : counts[counts.size() / 2])
                          << (counts.isEmpty() ? 0 : counts.last());
        qInfo().noquote() << QStringLiteral("local Scanner: %1 ms, agent+rebuild: %2 ms, files: %3, bytes equal: %4")
                                 .arg(localMs).arg(remoteMs).arg(remote.root->subtreeFileCount)
                                 .arg(local.root->size == remote.root->size);
    }

    void sshCommand_quotesAndValidates()
    {
        TreemapSettings s = TreemapSettings::defaults();
        s.limitToSameFilesystem = true;
        s.excludedPaths = {QStringLiteral("/a b"), QStringLiteral("/it's")};
        QString err;
        const auto cmd = RemoteScanner::sshCommand("me@host", "/srv/my data", "diskvu-agent", s, &err);
        QCOMPARE(cmd.program, QString("ssh"));
        QCOMPARE(cmd.arguments.at(cmd.arguments.size() - 2), QString("me@host"));
        const QString remote = cmd.arguments.last();
        QVERIFY(remote.startsWith("diskvu-agent --one-file-system"));
        QVERIFY(remote.contains("--exclude '/a b'"));
        QVERIFY(remote.contains("--exclude '/it'\\''s'"));
        QVERIFY(remote.endsWith("-- '/srv/my data'"));
#ifdef DISKVU_HAVE_ZSTD
        QVERIFY(remote.contains(" --compress"));
        QVERIFY(!RemoteScanner::sshCommand("h", "/", "diskvu-agent", s, nullptr, false)
                     .arguments.last().contains("--compress"));
#endif
        QVERIFY(cmd.arguments.contains("--")); // ends ssh option parsing

        QVERIFY(RemoteScanner::sshCommand("-oProxyCommand=evil", "/", "diskvu-agent", s, &err).program.isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY(RemoteScanner::sshCommand("", "/", "diskvu-agent", s).program.isEmpty());
    }
};

QTEST_MAIN(TestRemoteScanner)
#include "test_remotescanner.moc"

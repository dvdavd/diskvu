// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "scanner.h"
#include "treemapsettings.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <functional>

// Collect all non-virtual nodes into a flat list.
static void collectNodes(FileNode* node, QList<FileNode*>& out)
{
    if (!node || node->isVirtual) return;
    out.append(node);
    for (FileNode* child : node->children)
        collectNodes(child, out);
}

static FileNode* findByName(FileNode* root, const QString& name)
{
    if (!root) return nullptr;
    if (root->name == name) return root;
    for (FileNode* child : root->children)
        if (FileNode* found = findByName(child, name))
            return found;
    return nullptr;
}

static bool writeFile(const QString& path, qint64 size)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    if (size > 0)
        f.write(QByteArray(static_cast<int>(qMin(size, qint64(1024 * 1024))), 'x'));
    return true;
}

class TestScanner : public QObject {
    Q_OBJECT

private slots:
    // ── basic results ─────────────────────────────────────────────────────────

    void scan_emptyDirectory_rootIsNonNull()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.root != nullptr);
    }

    void scan_emptyDirectory_rootSizeIsZero()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const ScanResult result = Scanner::scan(dir.path());
        QCOMPARE(result.root->size, qint64(0));
    }

    void scan_singleFile_sizeMatchesDisk()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const qint64 expectedSize = 1024;
        QVERIFY(writeFile(dir.filePath("test.txt"), expectedSize));

        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.root != nullptr);

        FileNode* fileNode = nullptr;
        for (FileNode* child : result.root->children) {
            if (!child->isDirectory && !child->isVirtual) {
                fileNode = child;
                break;
            }
        }
        QVERIFY(fileNode != nullptr);
        QCOMPARE(fileNode->name, QStringLiteral("test.txt"));
        QCOMPARE(fileNode->size, expectedSize);
    }

    void scan_rootSizeEqualsLeafSum()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir(dir.path()).mkdir("sub");
        QVERIFY(writeFile(dir.filePath("a.bin"),     512));
        QVERIFY(writeFile(dir.filePath("sub/b.bin"), 256));

        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.root != nullptr);

        qint64 leafSum = 0;
        std::function<void(FileNode*)> sum = [&](FileNode* node) {
            if (!node->isDirectory && !node->isVirtual)
                leafSum += node->size;
            for (FileNode* child : node->children)
                sum(child);
        };
        sum(result.root);

        QCOMPARE(result.root->size, leafSum);
    }

    void scan_rootAbsolutePathIsScannedDir()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.root != nullptr);
        QVERIFY(!result.root->absolutePath.isEmpty());
    }

    // ── tree structure ────────────────────────────────────────────────────────

    void scan_parentPointersAreConsistent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir(dir.path()).mkdir("child");
        QVERIFY(writeFile(dir.filePath("child/file.txt"), 100));

        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.root != nullptr);
        QVERIFY(result.root->parent == nullptr);

        std::function<void(FileNode*)> check = [&](FileNode* node) {
            for (FileNode* child : node->children) {
                QCOMPARE(child->parent, node);
                check(child);
            }
        };
        check(result.root);
    }

    void scan_computePath_childMatchesDiskPath()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.filePath("hello.txt"), 10));

        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.root != nullptr);

        for (FileNode* child : result.root->children) {
            if (child->name == "hello.txt") {
                const QString expected = QDir::cleanPath(dir.filePath("hello.txt"));
                QCOMPARE(child->computePath(), expected);
                return;
            }
        }
        QFAIL("hello.txt not found in scan result");
    }

    void scan_directoriesAreMarkedAsDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir(dir.path()).mkdir("subdir");
        QVERIFY(writeFile(dir.filePath("subdir/f.txt"), 50));

        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.root != nullptr);
        QVERIFY(result.root->isDirectory);

        for (FileNode* child : result.root->children) {
            if (child->name == "subdir") {
                QVERIFY(child->isDirectory);
                return;
            }
        }
        QFAIL("subdir not found in scan result");
    }

    // ── exclusions ────────────────────────────────────────────────────────────

    void scan_excludedPath_notInResult()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir(dir.path()).mkdir("keep");
        QDir(dir.path()).mkdir("skip");
        QVERIFY(writeFile(dir.filePath("keep/a.txt"), 100));
        QVERIFY(writeFile(dir.filePath("skip/b.txt"), 100));

        TreemapSettings settings;
        settings.excludedPaths = {dir.filePath("skip")};
        settings.sanitize();

        const ScanResult result = Scanner::scan(dir.path(), settings);
        QVERIFY(result.root != nullptr);

        std::function<bool(FileNode*, const QString&)> hasName
            = [&](FileNode* node, const QString& name) -> bool {
            if (node->name == name) return true;
            for (FileNode* child : node->children)
                if (hasName(child, name)) return true;
            return false;
        };

        QVERIFY( hasName(result.root, "keep"));
        QVERIFY(!hasName(result.root, "skip"));
    }

    // ── file/dir properties ───────────────────────────────────────────────────

    void scan_fileIsNotMarkedAsDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.filePath("plain.txt"), 100));

        const ScanResult result = Scanner::scan(dir.path());
        FileNode* file = findByName(result.root, "plain.txt");
        QVERIFY(file != nullptr);
        QVERIFY(!file->isDirectory);
    }

    void scan_zeroByteFile_includedWithSizeZero()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.filePath("empty.bin"), 0));

        const ScanResult result = Scanner::scan(dir.path());
        FileNode* file = findByName(result.root, "empty.bin");
        QVERIFY(file != nullptr);
        QCOMPARE(file->size, qint64(0));
    }

    void scan_emptySubdirectory_includedWithSizeZero()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("emptydir"));

        const ScanResult result = Scanner::scan(dir.path());
        FileNode* subdir = findByName(result.root, "emptydir");
        QVERIFY(subdir != nullptr);
        QVERIFY(subdir->isDirectory);
        QCOMPARE(subdir->size, qint64(0));
    }

    void scan_allSiblingsPresent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QStringList names = {"alpha.txt", "beta.txt", "gamma.txt"};
        for (const QString& name : names)
            QVERIFY(writeFile(dir.filePath(name), 10));

        const ScanResult result = Scanner::scan(dir.path());
        for (const QString& name : names)
            QVERIFY2(findByName(result.root, name) != nullptr, qPrintable(name));
    }

    void scan_deepTree_sizePropagatesCorrectly()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // root/a/b/c/file.txt with 512 bytes
        QVERIFY(QDir(dir.path()).mkpath("a/b/c"));
        QVERIFY(writeFile(dir.filePath("a/b/c/file.txt"), 512));

        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.root != nullptr);

        FileNode* a = findByName(result.root, "a");
        FileNode* b = findByName(result.root, "b");
        FileNode* c = findByName(result.root, "c");
        FileNode* file = findByName(result.root, "file.txt");

        QVERIFY(a && b && c && file);
        QCOMPARE(file->size, qint64(512));
        QCOMPARE(c->size, qint64(512));
        QCOMPARE(b->size, qint64(512));
        QCOMPARE(a->size, qint64(512));
        QCOMPARE(result.root->size, qint64(512));
    }

    void scan_hiddenDotFile_includedInScan()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.filePath(".hidden"), 64));

        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(findByName(result.root, ".hidden") != nullptr);
    }

    void scan_colorsAssigned_nodesHaveNonZeroColor()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("subdir"));
        QVERIFY(writeFile(dir.filePath("subdir/file.txt"), 100));

        const ScanResult result = Scanner::scan(dir.path());
        QList<FileNode*> nodes;
        collectNodes(result.root, nodes);
        for (FileNode* node : nodes)
            QVERIFY2(node->color != 0, qPrintable(node->name));
    }

    // ── multiple exclusions ───────────────────────────────────────────────────

    void scan_multipleExcludedPaths_allExcluded()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("keep"));
        QVERIFY(QDir(dir.path()).mkdir("skip1"));
        QVERIFY(QDir(dir.path()).mkdir("skip2"));
        QVERIFY(writeFile(dir.filePath("keep/a.txt"),  100));
        QVERIFY(writeFile(dir.filePath("skip1/b.txt"), 100));
        QVERIFY(writeFile(dir.filePath("skip2/c.txt"), 100));

        TreemapSettings settings;
        settings.excludedPaths = {dir.filePath("skip1"), dir.filePath("skip2")};
        settings.sanitize();

        const ScanResult result = Scanner::scan(dir.path(), settings);
        QVERIFY( findByName(result.root, "keep")  != nullptr);
        QVERIFY( findByName(result.root, "a.txt") != nullptr);
        QVERIFY( findByName(result.root, "skip1") == nullptr);
        QVERIFY( findByName(result.root, "skip2") == nullptr);
    }

    // ── symlinks ──────────────────────────────────────────────────────────────

    void scan_symlinkIsNotIncluded()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.filePath("real.txt"), 200));
        // Create a symlink next to the real file
        const QString linkPath = dir.filePath("link.txt");
        QVERIFY(QFile::link(dir.filePath("real.txt"), linkPath));

        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY( findByName(result.root, "real.txt") != nullptr);
        QVERIFY( findByName(result.root, "link.txt") == nullptr);
    }

    // ── progress callback ─────────────────────────────────────────────────────

    void scan_progressCallback_invokedAtLeastOnce()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 0; i < 5; ++i) {
            QVERIFY(QDir(dir.path()).mkdir(QString("d%1").arg(i)));
            QVERIFY(writeFile(dir.filePath(QString("d%1/f.txt").arg(i)), 100));
        }

        int callCount = 0;
        Scanner::scan(dir.path(), TreemapSettings::defaults(),
            [&callCount](ScanResult) { ++callCount; },
            []() { return true; });

        QVERIFY(callCount > 0);
    }

    // ── cancel token ──────────────────────────────────────────────────────────

    void scan_preCancelledToken_doesNotCrash()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 0; i < 10; ++i) {
            QDir(dir.path()).mkdir(QString("d%1").arg(i));
            writeFile(dir.filePath(QString("d%1/f.txt").arg(i)), 50);
        }

        std::atomic_bool cancel{true};
        // Must not crash and must return within the test timeout
        const ScanResult result = Scanner::scan(
            dir.path(), TreemapSettings::defaults(), {}, {}, {}, {}, &cancel);
        QVERIFY(true); // reaching here without crash is the pass condition
    }

    // ── arena ownership ───────────────────────────────────────────────────────

    void scan_arenaIsNonNull()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.arena != nullptr);
    }

    void scan_arenaHoldsAtLeastRootNode()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const ScanResult result = Scanner::scan(dir.path());
        QVERIFY(result.arena->totalAllocated() >= 1);
    }

    void scan_limitToSameFilesystem_stillScansCurrentFilesystem()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QDir(dir.path()).mkdir("sub");
        QVERIFY(writeFile(dir.filePath("sub/f.txt"), 100));

        TreemapSettings settings;
        settings.limitToSameFilesystem = true;
        settings.sanitize();

        const ScanResult result = Scanner::scan(dir.path(), settings);
        QVERIFY(result.root != nullptr);
        QVERIFY(findByName(result.root, "sub") != nullptr);
        QVERIFY(findByName(result.root, "f.txt") != nullptr);
    }
};

QTEST_MAIN(TestScanner)
#include "test_scanner.moc"

// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "colorutils.h"
#include "filenode.h"
#include "mainwindow_utils.h"
#include "treemapsettings.h"

#include <QApplication>
#include <QtTest/QtTest>

class TestFileNode : public QObject {
    Q_OBJECT

private slots:
    // ── NodeArena ────────────────────────────────────────────────────────────

    void nodeArena_allocReturnsNonNull()
    {
        NodeArena arena;
        QVERIFY(arena.alloc() != nullptr);
    }

    void nodeArena_defaultConstructedNodeHasZeroFields()
    {
        NodeArena arena;
        FileNode* node = arena.alloc();
        QCOMPARE(node->size, qint64(0));
        QVERIFY(node->name.isEmpty());
        QVERIFY(node->parent == nullptr);
        QVERIFY(node->children.empty());
        QVERIFY(!node->isDirectory);
        QVERIFY(!node->isVirtual);
    }

    void nodeArena_totalAllocated_tracksCount()
    {
        NodeArena arena;
        QCOMPARE(arena.totalAllocated(), size_t(0));
        arena.alloc();
        QCOMPARE(arena.totalAllocated(), size_t(1));
        for (int i = 0; i < 9; ++i)
            arena.alloc();
        QCOMPARE(arena.totalAllocated(), size_t(10));
    }

    void nodeArena_chunkBoundary_survivesTransition()
    {
        // 512 nodes per chunk — allocate more than one chunk's worth
        NodeArena arena;
        const int count = 600;
        std::vector<FileNode*> ptrs;
        ptrs.reserve(count);
        for (int i = 0; i < count; ++i) {
            FileNode* n = arena.alloc();
            QVERIFY(n != nullptr);
            n->size = qint64(i);
            ptrs.push_back(n);
        }
        // Verify sizes survived across chunk boundaries
        for (int i = 0; i < count; ++i)
            QCOMPARE(ptrs[i]->size, qint64(i));
    }

    void nodeArena_merge_combinesTotalAllocated()
    {
        auto arenaA = std::make_shared<NodeArena>();
        NodeArena arenaB;
        for (int i = 0; i < 5; ++i) arenaA->alloc();
        for (int i = 0; i < 3; ++i) arenaB.alloc();

        const size_t before = arenaA->totalAllocated();
        arenaA->merge(std::move(arenaB));

        QVERIFY(arenaA->totalAllocated() >= before + 3);
    }

    void nodeArena_merge_allocsAfterMergeWork()
    {
        NodeArena arenaA;
        NodeArena arenaB;
        for (int i = 0; i < 3; ++i) arenaB.alloc();
        arenaA.merge(std::move(arenaB));

        // Should be able to alloc after merge without crash
        FileNode* n = arenaA.alloc();
        QVERIFY(n != nullptr);
        n->size = 42;
        QCOMPARE(n->size, qint64(42));
    }

    // ── FileNode::computePath ─────────────────────────────────────────────────

    void computePath_rootWithAbsolutePath()
    {
        FileNode root;
        root.absolutePath = "/home/user";
        QCOMPARE(root.computePath(), "/home/user");
    }

    void computePath_rootWithOnlyName()
    {
        FileNode root;
        root.name = "mydir";
        // No parent, no absolutePath: returns name as-is
        QCOMPARE(root.computePath(), "mydir");
    }

    void computePath_singleChild()
    {
        FileNode root;
        root.absolutePath = "/home/user";
        FileNode child;
        child.name   = "Documents";
        child.parent = &root;
        QCOMPARE(child.computePath(), "/home/user/Documents");
    }

    void computePath_deepChain()
    {
        FileNode root;
        root.absolutePath = "/home";
        FileNode a;
        a.name   = "user";
        a.parent = &root;
        FileNode b;
        b.name   = "docs";
        b.parent = &a;
        FileNode c;
        c.name   = "report.txt";
        c.parent = &b;
        QCOMPARE(c.computePath(), "/home/user/docs/report.txt");
    }

    void computePath_cleansRedundantSlashes()
    {
        FileNode root;
        root.absolutePath = "/home/user/"; // trailing slash
        FileNode child;
        child.name   = "file.txt";
        child.parent = &root;
        // QDir::cleanPath should normalise the double slash
        QVERIFY(!child.computePath().contains("//"));
    }

    void computePath_noParentNoAbsolutePath()
    {
        FileNode node;
        node.name = "orphan.txt";
        // Falls through to returning name
        QCOMPARE(node.computePath(), "orphan.txt");
    }

    void normalizedFilesystemPath_trimsAndCleansSeparators()
    {
        QCOMPARE(normalizedFilesystemPath(" /tmp//demo/../demo2 "), QStringLiteral("/tmp/demo2"));
    }

#ifdef Q_OS_WIN
    void normalizedFilesystemPath_expandsBareDriveRoot()
    {
        QCOMPARE(normalizedFilesystemPath(QStringLiteral("C:")), QStringLiteral("C:/"));
        QCOMPARE(normalizedFilesystemPath(QStringLiteral("C:\\")), QStringLiteral("C:/"));
    }

    void breadcrumbPathSegments_preserveDriveRootTarget()
    {
        const QList<BreadcrumbPathSegment> segments =
            breadcrumbPathSegments(QStringLiteral("C:/Users/Alice"));

        QCOMPARE(segments.size(), 3);
        QCOMPARE(segments.at(0).label, QDir::toNativeSeparators(QStringLiteral("C:/")));
        QCOMPARE(segments.at(0).path, QStringLiteral("C:/"));
        QCOMPARE(segments.at(1).label, QStringLiteral("Users"));
        QCOMPARE(segments.at(1).path, QStringLiteral("C:/Users"));
        QCOMPARE(segments.at(2).label, QStringLiteral("Alice"));
        QCOMPARE(segments.at(2).path, QStringLiteral("C:/Users/Alice"));
    }

    void pathIsWithinRoot_matchesDriveRoots()
    {
        QVERIFY(pathIsWithinRoot(QStringLiteral("C:/Users/Alice"), QStringLiteral("C:")));
        QVERIFY(pathIsWithinRoot(QStringLiteral("C:/"), QStringLiteral("C:/")));
        QVERIFY(!pathIsWithinRoot(QStringLiteral("D:/Users/Alice"), QStringLiteral("C:/")));
    }
#endif

    // ── ColorUtils ──────────────────────────────────────────────────────────

    void colorUtils_normalizedHue_inRangeForPositiveInput()
    {
        for (const float h : {0.0f, 0.5f, 0.99f, 1.0f, 1.5f, 2.7f}) {
            const float result = ColorUtils::normalizedHue(h);
            QVERIFY(result >= 0.0f);
            QVERIFY(result < 1.0f);
        }
    }

    void colorUtils_normalizedHue_inRangeForNegativeInput()
    {
        for (const float h : {-0.1f, -1.0f, -2.3f}) {
            const float result = ColorUtils::normalizedHue(h);
            QVERIFY(result >= 0.0f);
            QVERIFY(result < 1.0f);
        }
    }

    void colorUtils_sampleGradient_atZeroReturnsFirstStop()
    {
        const QList<QColor> stops = {QColor(255, 0, 0), QColor(0, 0, 255)};
        const QColor result = ColorUtils::sampleGradient(stops, 0.0f);
        QCOMPARE(result.red(),   255);
        QCOMPARE(result.blue(),    0);
    }

    void colorUtils_sampleGradient_atOneReturnsLastStop()
    {
        const QList<QColor> stops = {QColor(255, 0, 0), QColor(0, 0, 255)};
        const QColor result = ColorUtils::sampleGradient(stops, 1.0f);
        QCOMPARE(result.red(),    0);
        QCOMPARE(result.blue(), 255);
    }

    void colorUtils_sampleGradient_midpointInterpolates()
    {
        const QList<QColor> stops = {QColor(0, 0, 0), QColor(200, 200, 200)};
        const QColor result = ColorUtils::sampleGradient(stops, 0.5f);
        QVERIFY(result.red() > 50 && result.red() < 150);
    }

    void colorUtils_fileTypeLabelForName_returnsNonNullForKnownExtensions()
    {
        // Known file types should get a label
        QVERIFY(!ColorUtils::fileTypeLabelForName("photo.jpg").isNull());
        QVERIFY(!ColorUtils::fileTypeLabelForName("doc.pdf").isNull());
        QVERIFY(!ColorUtils::fileTypeLabelForName("archive.zip").isNull());
        QVERIFY(!ColorUtils::fileTypeLabelForName("code.cpp").isNull());
    }

    void colorUtils_fileTypeLabelForName_doesNotCrashForUnknown()
    {
        // Unknown extension: must not crash (may return empty)
        const QString label = ColorUtils::fileTypeLabelForName("file.xyzqrs123unknown");
        QVERIFY(!label.isNull());
    }

    void colorUtils_folderColor_isValid()
    {
        const TreemapSettings s;
        const QColor c = ColorUtils::folderColor(0, 0.5f, s);
        QVERIFY(c.isValid());
    }

    void colorUtils_fileColorForName_isValid()
    {
        const TreemapSettings s;
        QVERIFY(ColorUtils::fileColorForName("document.pdf", s).isValid());
        QVERIFY(ColorUtils::fileColorForName("image.png",    s).isValid());
        QVERIFY(ColorUtils::fileColorForName("noext",        s).isValid());
    }

    void colorUtils_fileColorForName_usesFileTypeGroupHueWithThemeSaturationAndBrightness()
    {
        TreemapSettings s;
        s.fileTypeGroups = {
            { QStringLiteral("Images"), QColor(QStringLiteral("#33AAEE")), {QStringLiteral("png")} }
        };
        s.fileColorSaturation = 0.42;
        s.fileColorBrightness = 0.61;

        const QColor actual = ColorUtils::fileColorForName("photo.png", s);
        const QColor expected = QColor::fromHslF(
            ColorUtils::hueFromColor(QColor(QStringLiteral("#33AAEE")), 0.0f),
            static_cast<float>(s.fileColorSaturation),
            static_cast<float>(s.fileColorBrightness));

        QCOMPARE(actual, expected);
    }

    void colorUtils_fileColorForName_matchesFileTypeGroupsCaseInsensitively()
    {
        TreemapSettings s;
        s.fileTypeGroups = {
            { QStringLiteral("Code"), QColor(QStringLiteral("#CC5500")), {QStringLiteral("cpp")} }
        };
        s.fileColorSaturation = 0.73;
        s.fileColorBrightness = 0.52;

        const QColor actual = ColorUtils::fileColorForName("MAIN.CPP", s);
        const QColor expected = QColor::fromHslF(
            ColorUtils::hueFromColor(QColor(QStringLiteral("#CC5500")), 0.0f),
            static_cast<float>(s.fileColorSaturation),
            static_cast<float>(s.fileColorBrightness));

        QCOMPARE(actual, expected);
    }

    void colorUtils_fileColorForName_usesUnknownFileTypeColorWhenRandomDisabled()
    {
        TreemapSettings s;
        s.randomColorForUnknownFiles = false;
        s.unknownFileTypeColor = QColor(QStringLiteral("#123456"));
        s.unknownFileTypeOpacity = 0.25;
        s.fileTypeGroups.clear();

        QColor expected = s.unknownFileTypeColor;
        expected.setAlphaF(s.unknownFileTypeOpacity);
        QCOMPARE(ColorUtils::fileColorForName("mystery.oddext", s), expected);
        QCOMPARE(ColorUtils::fileColorForName("noext", s), expected);
    }

    void colorUtils_fileColorForName_prefersMatchingGroupOverUnknownFallback()
    {
        TreemapSettings s;
        s.randomColorForUnknownFiles = false;
        s.unknownFileTypeColor = QColor(QStringLiteral("#101010"));
        s.fileTypeGroups = {
            { QStringLiteral("Images"), QColor(QStringLiteral("#00CC66")), {QStringLiteral("png")} }
        };
        s.fileColorSaturation = 0.35;
        s.fileColorBrightness = 0.44;

        const QColor actual = ColorUtils::fileColorForName("icon.png", s);
        const QColor expected = QColor::fromHslF(
            ColorUtils::hueFromColor(QColor(QStringLiteral("#00CC66")), 0.0f),
            static_cast<float>(s.fileColorSaturation),
            static_cast<float>(s.fileColorBrightness));

        QCOMPARE(actual, expected);
        QVERIFY(actual != s.unknownFileTypeColor);
    }

    void colorUtils_assignColors_doesNotCrash()
    {
        NodeArena arena;
        FileNode* root = arena.alloc();
        root->isDirectory   = true;
        root->absolutePath  = "/test";
        root->size          = 1000;

        FileNode* child = arena.alloc();
        child->name      = "file.txt";
        child->size      = 1000;
        child->parent    = root;
        root->children.push_back(child);

        const TreemapSettings s;
        ColorUtils::assignColors(root, s);

        QVERIFY(root->color != 0 || true); // just must not crash
    }

    void collectAndSortFileSummaries_limitsResultsToCurrentRoot()
    {
        FileNode root;
        root.absolutePath = QStringLiteral("/scan");
        root.isDirectory = true;

        FileNode childDir;
        childDir.name = QStringLiteral("subdir");
        childDir.isDirectory = true;
        childDir.parent = &root;

        FileNode rootPdf;
        rootPdf.name = QStringLiteral("root.pdf");
        rootPdf.parent = &root;
        rootPdf.size = 100;
        rootPdf.extKey = ColorUtils::packFileExt(rootPdf.name);
        rootPdf.id = 0;

        FileNode childPdf;
        childPdf.name = QStringLiteral("child.pdf");
        childPdf.parent = &childDir;
        childPdf.size = 40;
        childPdf.extKey = ColorUtils::packFileExt(childPdf.name);
        childPdf.id = 1;

        FileNode childTxt;
        childTxt.name = QStringLiteral("child.txt");
        childTxt.parent = &childDir;
        childTxt.size = 25;
        childTxt.extKey = ColorUtils::packFileExt(childTxt.name);
        childTxt.id = 2;

        root.children = {&rootPdf, &childDir};
        childDir.children = {&childPdf, &childTxt};

        const QList<FileTypeSummary> summaries = collectAndSortFileSummaries(&childDir);
        QCOMPARE(summaries.size(), 2);
        QCOMPARE(summaries.at(0).totalSize, qint64(40));
        QCOMPARE(summaries.at(0).count, 1);
        QCOMPARE(summaries.at(1).totalSize, qint64(25));
        QCOMPARE(summaries.at(1).count, 1);
    }

    void collectAndSortFileSummaries_respectsSearchReachWithinSubtree()
    {
        FileNode root;
        root.absolutePath = QStringLiteral("/scan");
        root.isDirectory = true;

        FileNode childDir;
        childDir.name = QStringLiteral("subdir");
        childDir.isDirectory = true;
        childDir.parent = &root;

        FileNode childPdf;
        childPdf.name = QStringLiteral("child.pdf");
        childPdf.parent = &childDir;
        childPdf.size = 40;
        childPdf.extKey = ColorUtils::packFileExt(childPdf.name);
        childPdf.id = 0;

        FileNode childTxt;
        childTxt.name = QStringLiteral("child.txt");
        childTxt.parent = &childDir;
        childTxt.size = 25;
        childTxt.extKey = ColorUtils::packFileExt(childTxt.name);
        childTxt.id = 1;

        root.children = {&childDir};
        childDir.children = {&childPdf, &childTxt};

        const std::vector<bool> searchReach = {false, true};
        const QList<FileTypeSummary> summaries = collectAndSortFileSummaries(&childDir, searchReach);
        QCOMPARE(summaries.size(), 1);
        QCOMPARE(summaries.at(0).totalSize, qint64(25));
        QCOMPARE(summaries.at(0).count, 1);
    }

};

QTEST_MAIN(TestFileNode)
#include "test_filenode.moc"

// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "filenode.h"
#include "treemapwidget.h"

#include <QApplication>
#include <QtTest/QtTest>
#include <string_view>

// Helper: returns true if node's name appears at the correct offset in flatNames.
static bool nameInIndex(const SearchIndex& index, const FileNode* node, const QString& expected)
{
    if (node->id >= index.nameOffsets.size())
        return false;
    const uint32_t off = index.nameOffsets[node->id];
    const uint16_t len = index.nameLens[node->id];
    if (off + len > index.flatNames.size())
        return false;
    const std::string_view stored(index.flatNames.data() + off, len);
    return stored == expected.toUtf8().toStdString();
}

// Helper: simple substring search using the index, case-folded.
static std::vector<FileNode*> searchNodes(const SearchIndex& index, const QString& pattern)
{
    const std::string utf8 = pattern.toCaseFolded().toUtf8().toStdString();
    std::vector<FileNode*> matches;
    for (FileNode* node : index.nodes) {
        if (node->id >= index.nameOffsets.size())
            continue;
        const std::string_view name(
            index.flatNames.data() + index.nameOffsets[node->id],
            index.nameLens[node->id]);
        if (name.find(utf8) != std::string_view::npos)
            matches.push_back(node);
    }
    return matches;
}

class TestSearch : public QObject {
    Q_OBJECT

private slots:
    // ── buildSearchIndex ─────────────────────────────────────────────────────

    void buildSearchIndex_nullRootReturnsNull()
    {
        QVERIFY(buildSearchIndex(nullptr) == nullptr);
    }

    void buildSearchIndex_singleNode_idIsZero()
    {
        NodeArena arena;
        FileNode* root = arena.alloc();
        root->name = "root";
        root->absolutePath = "/root";

        auto index = buildSearchIndex(root);
        QVERIFY(index != nullptr);
        QCOMPARE(root->id, uint32_t(0));
    }

    void buildSearchIndex_nodeCountMatchesNonVirtual()
    {
        NodeArena arena;
        FileNode* root = arena.alloc();
        root->isDirectory = true;
        root->absolutePath = "/r";

        FileNode* a = arena.alloc(); a->name = "a"; a->parent = root;
        FileNode* b = arena.alloc(); b->name = "b"; b->parent = root;
        FileNode* v = arena.alloc(); v->name = "free"; v->isVirtual = true; v->parent = root;
        root->children = {a, b, v};

        auto index = buildSearchIndex(root);
        // root + a + b = 3; virtual node excluded
        QCOMPARE(index->nodeCount, uint32_t(3));
        QCOMPARE(static_cast<size_t>(index->nodeCount), index->nodes.size());
    }

    void buildSearchIndex_virtualNodesExcluded()
    {
        NodeArena arena;
        FileNode* root = arena.alloc();
        root->isDirectory = true;
        root->absolutePath = "/r";

        FileNode* real = arena.alloc(); real->name = "real"; real->parent = root;
        FileNode* virt = arena.alloc(); virt->name = "virt"; virt->isVirtual = true; virt->parent = root;
        root->children = {real, virt};

        auto index = buildSearchIndex(root);

        QCOMPARE(virt->id, uint32_t(UINT32_MAX)); // untouched
        for (FileNode* n : index->nodes)
            QVERIFY(!n->isVirtual);
    }

    void buildSearchIndex_idsAreSequential()
    {
        NodeArena arena;
        FileNode* root = arena.alloc(); root->isDirectory = true; root->absolutePath = "/r";
        FileNode* a = arena.alloc(); a->name = "a"; a->parent = root;
        FileNode* b = arena.alloc(); b->name = "b"; b->parent = root;
        FileNode* c = arena.alloc(); c->name = "c"; c->parent = root;
        root->children = {a, b, c};

        buildSearchIndex(root);

        // All four IDs must be distinct values in [0, 3]
        const std::set<uint32_t> ids = {root->id, a->id, b->id, c->id};
        QCOMPARE(ids.size(), size_t(4));
        QVERIFY(*ids.begin() == 0);
        QVERIFY(*ids.rbegin() == 3);
    }

    void buildSearchIndex_flatNamesAreCaseFolded()
    {
        NodeArena arena;
        FileNode* root = arena.alloc();
        root->name = "README.MD";
        root->absolutePath = "/r";

        auto index = buildSearchIndex(root);
        QVERIFY(nameInIndex(*index, root, "readme.md"));
    }

    void buildSearchIndex_offsetsAndLensCorrect()
    {
        NodeArena arena;
        FileNode* root = arena.alloc(); root->isDirectory = true; root->absolutePath = "/r";
        FileNode* child = arena.alloc(); child->name = "hello.txt"; child->parent = root;
        root->children = {child};

        auto index = buildSearchIndex(root);

        // child's entry in flatNames should be "hello.txt"
        QVERIFY(nameInIndex(*index, child, "hello.txt"));
    }

    // ── Substring matching via index ─────────────────────────────────────────

    void search_emptyPatternMatchesAll()
    {
        NodeArena arena;
        FileNode* root = arena.alloc(); root->isDirectory = true; root->absolutePath = "/r";
        FileNode* a = arena.alloc(); a->name = "alpha.txt"; a->parent = root;
        FileNode* b = arena.alloc(); b->name = "beta.cpp"; b->parent = root;
        root->children = {a, b};

        auto index = buildSearchIndex(root);
        // Empty pattern: find("") always matches, so all indexed nodes are returned
        const auto matches = searchNodes(*index, "");
        QCOMPARE(matches.size(), index->nodes.size()); // root + a + b
    }

    void search_literalMatchFindsNode()
    {
        NodeArena arena;
        FileNode* root = arena.alloc(); root->isDirectory = true; root->absolutePath = "/r";
        FileNode* a = arena.alloc(); a->name = "report.pdf"; a->parent = root;
        FileNode* b = arena.alloc(); b->name = "image.png"; b->parent = root;
        root->children = {a, b};

        auto index = buildSearchIndex(root);
        const auto matches = searchNodes(*index, "report");
        QCOMPARE(matches.size(), size_t(1));
        QCOMPARE(matches[0], a);
    }

    void search_matchIsCaseInsensitive()
    {
        NodeArena arena;
        FileNode* root = arena.alloc(); root->isDirectory = true; root->absolutePath = "/r";
        FileNode* child = arena.alloc(); child->name = "MyDocument.DOCX"; child->parent = root;
        root->children = {child};

        auto index = buildSearchIndex(root);

        // Search with uppercase should still find it
        const auto matches = searchNodes(*index, "MYDOCUMENT");
        QCOMPARE(matches.size(), size_t(1));
        QCOMPARE(matches[0], child);
    }

    void search_noMatchReturnsEmpty()
    {
        NodeArena arena;
        FileNode* root = arena.alloc(); root->isDirectory = true; root->absolutePath = "/r";
        FileNode* child = arena.alloc(); child->name = "notes.txt"; child->parent = root;
        root->children = {child};

        auto index = buildSearchIndex(root);
        const auto matches = searchNodes(*index, "zzznomatch");
        QVERIFY(matches.empty());
    }

    void search_substringMatchWorks()
    {
        NodeArena arena;
        FileNode* root = arena.alloc(); root->isDirectory = true; root->absolutePath = "/r";
        FileNode* a = arena.alloc(); a->name = "screenshot_2026.png"; a->parent = root;
        FileNode* b = arena.alloc(); b->name = "readme.txt"; b->parent = root;
        root->children = {a, b};

        auto index = buildSearchIndex(root);

        // "2026" only in a
        const auto m1 = searchNodes(*index, "2026");
        QCOMPARE(m1.size(), size_t(1));
        QCOMPARE(m1[0], a);

        // ".txt" only in b
        const auto m2 = searchNodes(*index, ".txt");
        QCOMPARE(m2.size(), size_t(1));
        QCOMPARE(m2[0], b);
    }
};

QTEST_MAIN(TestSearch)
#include "test_search.moc"

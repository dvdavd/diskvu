// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "filenode.h"
#include "mainwindow_utils.h"
#include "scanner.h"

#include <QApplication>
#include <QtTest/QtTest>

// Builds a minimal two-level ScanResult:
//   root (/home/user, 1000)
//   ├── sub  (dir, 400)
//   └── file.txt (file, 600)
static ScanResult buildMainTree()
{
    auto arena = std::make_shared<NodeArena>();
    ScanResult r;
    r.arena = arena;

    FileNode* root = arena->alloc();
    root->absolutePath = "/home/user";
    root->isDirectory = true;
    root->size = 1000;

    FileNode* sub = arena->alloc();
    sub->name = "sub";
    sub->isDirectory = true;
    sub->size = 400;
    sub->parent = root;

    FileNode* file = arena->alloc();
    file->name = "file.txt";
    file->size = 600;
    file->parent = root;

    root->children = {sub, file};
    r.root = root;
    return r;
}

class TestRefresh : public QObject {
    Q_OBJECT

private slots:
    void splice_replacesChildInParent()
    {
        ScanResult main = buildMainTree();
        FileNode* oldSub = main.root->children[0]; // the "sub" node

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        FileNode* newSub = refreshArena->alloc();
        newSub->absolutePath = "/home/user/sub";
        newSub->isDirectory = true;
        newSub->size = 800;
        refreshed.root = newSub;

        QVERIFY(spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed)));

        QCOMPARE(main.root->children[0], newSub);
        QVERIFY(main.root->children[0] != oldSub);
    }

    void splice_setsParentPointer()
    {
        ScanResult main = buildMainTree();

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        FileNode* newSub = refreshArena->alloc();
        newSub->absolutePath = "/home/user/sub";
        newSub->isDirectory = true;
        newSub->size = 400;
        refreshed.root = newSub;

        spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed));

        QCOMPARE(newSub->parent, main.root);
    }

    void splice_propagatesSizeDeltaUpward()
    {
        ScanResult main = buildMainTree();
        // sub was 400, new is 800 → delta = +400 → root should go from 1000 to 1400

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        FileNode* newSub = refreshArena->alloc();
        newSub->absolutePath = "/home/user/sub";
        newSub->isDirectory = true;
        newSub->size = 800;
        refreshed.root = newSub;

        spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed));

        QCOMPARE(main.root->size, qint64(1400));
    }

    void splice_negativeDeltaPropagatesCorrectly()
    {
        ScanResult main = buildMainTree();
        // sub was 400, new is 100 → delta = -300 → root should go from 1000 to 700

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        FileNode* newSub = refreshArena->alloc();
        newSub->absolutePath = "/home/user/sub";
        newSub->isDirectory = true;
        newSub->size = 100;
        refreshed.root = newSub;

        spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed));

        QCOMPARE(main.root->size, qint64(700));
    }

    void splice_mergesArenas()
    {
        ScanResult main = buildMainTree();
        const size_t before = main.arena->totalAllocated();

        auto refreshArena = std::make_shared<NodeArena>();
        // Allocate a few extra nodes in the refresh arena
        for (int i = 0; i < 5; ++i) refreshArena->alloc();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        FileNode* newSub = refreshed.arena->alloc();
        newSub->absolutePath = "/home/user/sub";
        newSub->isDirectory = true;
        newSub->size = 400;
        refreshed.root = newSub;

        spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed));

        QVERIFY(main.arena->totalAllocated() > before);
    }

    void splice_returnsFalseForUnknownPath()
    {
        ScanResult main = buildMainTree();

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        FileNode* newNode = refreshArena->alloc();
        newNode->absolutePath = "/home/user/nonexistent";
        newNode->isDirectory = true;
        newNode->size = 100;
        refreshed.root = newNode;

        QVERIFY(!spliceRefreshedSubtree(main, "/home/user/nonexistent", std::move(refreshed)));
    }

    void splice_returnsFalseForRootNode()
    {
        ScanResult main = buildMainTree();
        // The root itself has no parent — splice should refuse it

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        FileNode* newRoot = refreshArena->alloc();
        newRoot->absolutePath = "/home/user";
        newRoot->isDirectory = true;
        newRoot->size = 2000;
        refreshed.root = newRoot;

        QVERIFY(!spliceRefreshedSubtree(main, "/home/user", std::move(refreshed)));
        // Original tree unchanged
        QCOMPARE(main.root->size, qint64(1000));
    }
};

QTEST_MAIN(TestRefresh)
#include "test_refresh.moc"

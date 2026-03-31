// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QDir>
#include <QString>
#include <QRgb>
#include <QRectF>
#include <QtGlobal>
#include <cstdint>
#include <memory>
#include <vector>

struct NodeRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    NodeRect() = default;
    NodeRect(const QRectF& rect)
        : x(static_cast<float>(rect.x()))
        , y(static_cast<float>(rect.y()))
        , w(static_cast<float>(rect.width()))
        , h(static_cast<float>(rect.height()))
    {
    }

    NodeRect& operator=(const QRectF& rect)
    {
        x = static_cast<float>(rect.x());
        y = static_cast<float>(rect.y());
        w = static_cast<float>(rect.width());
        h = static_cast<float>(rect.height());
        return *this;
    }

    operator QRectF() const
    {
        return QRectF(x, y, w, h);
    }

    bool isEmpty() const
    {
        return w <= 0.0f || h <= 0.0f;
    }

    qreal width() const { return w; }
    qreal height() const { return h; }
    qreal left() const { return x; }
    qreal top() const { return y; }
};

struct FileNode {
    QString name;
    // Set only on the scan root; descendants reconstruct their path from the parent chain.
    QString absolutePath;
    std::vector<FileNode*> children;  // raw ptrs; owned by the NodeArena in ScanResult
    FileNode* parent = nullptr;
    qint64 size = 0;
    QRgb color = 0;           // ARGB; alpha==0 means "not yet assigned"
    uint64_t extKey = 0;      // packed lowercase ASCII extension for files; 0 = none/unsupported
    uint32_t id = UINT32_MAX; // assigned by rebuildSearchMetadata(); UINT32_MAX = unnumbered/virtual
    bool isDirectory = false;
    bool isVirtual = false;   // for "Free Space" synthetic node

    // Returns the full absolute path. The root stores it directly; descendants
    // reconstruct it from the parent chain to avoid per-directory path storage.
    QString computePath() const
    {
        if (!absolutePath.isEmpty())
            return absolutePath;
        if (parent) {
            const QString parentPath = parent->computePath();
            if (parentPath.isEmpty()) {
                return name;
            }
            return QDir::cleanPath(QDir(parentPath).filePath(name));
        }
        return name;
    }
};

// Bump/arena allocator for FileNode.
// Allocates nodes in large contiguous chunks, eliminating per-node malloc
// overhead (~16 bytes/node on glibc) and improving cache locality.
// All nodes are freed together when the arena is destroyed.
class NodeArena {
    static constexpr size_t kChunkNodes = 512;

    std::vector<std::unique_ptr<FileNode[]>> m_chunks;
    size_t m_usedInLast = 0;

public:
    NodeArena()
    {
        m_chunks.push_back(std::make_unique<FileNode[]>(kChunkNodes));
    }

    NodeArena(const NodeArena&) = delete;
    NodeArena& operator=(const NodeArena&) = delete;

    FileNode* alloc()
    {
        if (m_usedInLast == kChunkNodes) {
            m_chunks.push_back(std::make_unique<FileNode[]>(kChunkNodes));
            m_usedInLast = 0;
        }
        return &m_chunks.back()[m_usedInLast++];
    }

    // Consume all chunks from other, appending them to this arena.
    // Seals the current last chunk so future allocs use a fresh one,
    // avoiding any confusion about the used-count of the pre-merge tail.
    void merge(NodeArena&& other)
    {
        if (!m_chunks.empty())
            m_usedInLast = kChunkNodes; // seal current tail
        for (auto& chunk : other.m_chunks)
            m_chunks.push_back(std::move(chunk));
        if (!other.m_chunks.empty())
            m_usedInLast = other.m_usedInLast;
        other.m_chunks.clear();
        other.m_usedInLast = 0;
    }

    // Returns total number of allocated nodes (including unused slots in the last chunk).
    size_t totalAllocated() const
    {
        if (m_chunks.empty()) return 0;
        return (m_chunks.size() - 1) * kChunkNodes + m_usedInLast;
    }

    // Destructor is implicit: unique_ptr<FileNode[]> calls ~FileNode() for
    // all slots (including unused ones, which are in default-constructed state).
};

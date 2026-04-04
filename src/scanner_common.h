// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

// Shared constants and helpers used by both scanner_posix.cpp and scanner_std.cpp.
// Placed in an anonymous namespace so each translation unit gets its own copy
// (required for the thread_local statics inside emitProgress).

#include "scanner.h"

#include <QDir>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <algorithm>
#include <atomic>
#include <vector>

static constexpr int kMaxDepth = 64;
static constexpr qint64 kProgressIntervalMs = 180;
static constexpr qint64 kActivityIntervalMs = 40;
static constexpr int kPreviewDepth = 5;
static constexpr size_t kPreviewChildLimits[] = {
    16,
    24,
    64,
    160,
    384,
};
static constexpr qint64 kProvisionalDirectoryPreviewSize = 1;

namespace {

QStringList defaultExcludedPathsForScanRoot(const QString& scanRootPath)
{
    if (QDir::cleanPath(scanRootPath) != QLatin1String("/")) {
        return {};
    }

#ifdef Q_OS_LINUX
    return {
        QStringLiteral("/proc"),
        QStringLiteral("/sys"),
        QStringLiteral("/dev"),
        QStringLiteral("/run"),
    };
#elif defined(Q_OS_MACOS)
    return { QStringLiteral("/dev") };
#else
    return {};
#endif
}

QString childPathForParent(const QString& parentPath, const QString& childName)
{
    if (parentPath.endsWith(QLatin1Char('/'))) {
        return parentPath + childName;
    }
    return parentPath + QLatin1Char('/') + childName;
}

QString childPathPrefixForParent(const QString& parentPath)
{
    if (parentPath.endsWith(QLatin1Char('/'))) {
        return parentPath;
    }
    QString prefix = parentPath;
    prefix += QLatin1Char('/');
    return prefix;
}

bool shouldSkipPath(const QString& candidatePath,
                    const std::vector<QString>& excludedPathPrefixes)
{
    for (const QString& excludedPath : excludedPathPrefixes) {
        if (candidatePath.size() < excludedPath.size()) {
            continue;
        }
        if (candidatePath == excludedPath) {
            return true;
        }
        if (!candidatePath.startsWith(excludedPath)) {
            continue;
        }
        if (excludedPath.endsWith(QLatin1Char('/'))) {
            return true;
        }
        if (candidatePath.at(excludedPath.size()) == QLatin1Char('/')) {
            return true;
        }
    }

    return false;
}

bool isCancelled(const std::atomic_bool* cancelFlag)
{
    return cancelFlag && cancelFlag->load(std::memory_order_relaxed);
}

struct LiveWorkerPaths {
    QMutex mutex;
    std::vector<QString> paths;
};

void updateLiveWorkerPath(LiveWorkerPaths* livePaths, size_t index, const QString& path)
{
    if (!livePaths || index >= livePaths->paths.size()) {
        return;
    }

    QMutexLocker locker(&livePaths->mutex);
    livePaths->paths[index] = path;
}

QString currentLiveWorkerPath(LiveWorkerPaths* livePaths)
{
    if (!livePaths) {
        return {};
    }

    QMutexLocker locker(&livePaths->mutex);
    for (auto it = livePaths->paths.rbegin(); it != livePaths->paths.rend(); ++it) {
        if (!it->isEmpty()) {
            return *it;
        }
    }

    return {};
}

size_t previewChildLimitForDepth(int remainingDepth)
{
    if (remainingDepth <= 0) {
        return 0;
    }

    const size_t index = static_cast<size_t>(std::min(
        remainingDepth, static_cast<int>(std::size(kPreviewChildLimits)))) - 1u;
    return kPreviewChildLimits[index];
}

bool pathIsWithinNode(const FileNode* node, const QString& currentPath)
{
    if (!node || currentPath.isEmpty()) {
        return false;
    }

    const QString nodePath = node->computePath();
    if (nodePath.isEmpty()) {
        return false;
    }

    if (currentPath.size() <= nodePath.size()) {
        return currentPath == nodePath;
    }

    if (!currentPath.startsWith(nodePath)) {
        return false;
    }

    if (nodePath.endsWith(QLatin1Char('/'))) {
        return true;
    }

    return currentPath.at(nodePath.size()) == QLatin1Char('/');
}

bool pathIsWithinCandidate(const QString& currentPath, const QString& candidatePath)
{
    if (currentPath.size() <= candidatePath.size()) {
        return currentPath == candidatePath;
    }

    if (!currentPath.startsWith(candidatePath)) {
        return false;
    }

    if (candidatePath.endsWith(QLatin1Char('/'))) {
        return true;
    }

    return currentPath.at(candidatePath.size()) == QLatin1Char('/');
}

void addSizeUpwards(FileNode* node, qint64 delta)
{
    while (node) {
        node->size += delta;
        node = node->parent;
    }
}

FileNode* cloneNodeLimited(const FileNode* node, int remainingDepth,
                           const QString& currentPath, const QString& nodePath,
                           NodeArena& arena, FileNode* parent = nullptr)
{
    if (!node)
        return nullptr;

    FileNode* copy = arena.alloc();
    copy->name = node->name;
    copy->absolutePath = node->absolutePath;
    copy->size = node->size;
    copy->isDirectory = node->isDirectory;
    copy->isVirtual = node->isVirtual;
    copy->color = node->color;
    copy->extKey = node->extKey;
    copy->parent = parent;

    if (remainingDepth > 0) {
        std::vector<const FileNode*> selectedChildren;
        const FileNode* activeChild = nullptr;
        selectedChildren.reserve(node->children.size());
        for (const FileNode* child : node->children) {
            if (!child) {
                continue;
            }
            if (child->size <= 0 && !child->isDirectory) {
                continue;
            }
            if (!activeChild && child->isDirectory) {
                const QString childPath = childPathForParent(nodePath, child->name);
                if (pathIsWithinCandidate(currentPath, childPath)) {
                    activeChild = child;
                    continue;
                }
            }
            selectedChildren.push_back(child);
        }

        const size_t childLimit = previewChildLimitForDepth(remainingDepth);
        const size_t reservedSlots = activeChild ? 1u : 0u;
        const size_t childCount = (childLimit > reservedSlots)
            ? std::min(selectedChildren.size(), childLimit - reservedSlots)
            : 0u;
        if (selectedChildren.size() > childCount) {
            std::nth_element(selectedChildren.begin(),
                             selectedChildren.begin() + static_cast<std::ptrdiff_t>(childCount),
                             selectedChildren.end(),
                             [](const FileNode* a, const FileNode* b) {
                                 return a->size > b->size;
                             });
            selectedChildren.resize(childCount);
        }

        std::sort(selectedChildren.begin(), selectedChildren.end(),
                  [](const FileNode* a, const FileNode* b) {
                      return a->size > b->size;
                  });

        copy->children.reserve(selectedChildren.size() + reservedSlots);
        if (activeChild) {
            const QString activeChildPath = childPathForParent(nodePath, activeChild->name);
            FileNode* childCopy = cloneNodeLimited(activeChild, remainingDepth - 1, currentPath,
                                                   activeChildPath, arena, copy);
            if (childCopy) {
                copy->children.push_back(childCopy);
            }
        }
        for (const FileNode* child : selectedChildren) {
            const QString childPath = childPathForParent(nodePath, child->name);
            FileNode* childCopy = cloneNodeLimited(child, remainingDepth - 1, currentPath,
                                                   childPath, arena, copy);
            if (childCopy)
                copy->children.push_back(childCopy);
        }
    }

    return copy;
}

void emitProgress(const ScanResult& scanState, const QString& currentPath,
                  const Scanner::ProgressReadyCallback& progressReadyCallback,
                  const Scanner::ProgressCallback& progressCallback, bool force = false)
{
    if (!progressCallback || !scanState.root)
        return;

    if (scanState.profile) {
        ++scanState.profile->progressCalls;
    }

    static thread_local QElapsedTimer timer;
    static thread_local bool timerStarted = false;

    if (!timerStarted) {
        timer.start();
        timerStarted = true;
    }

    if (!force && timer.elapsed() < kProgressIntervalMs) {
        if (scanState.profile) {
            ++scanState.profile->progressThrottleSkips;
        }
        return;
    }

    if (!force && progressReadyCallback && !progressReadyCallback()) {
        if (scanState.profile) {
            ++scanState.profile->progressThrottleSkips;
        }
        return;
    }

    timer.restart();

    ScanResult snapshot;
    snapshot.arena = std::make_shared<NodeArena>();
    snapshot.freeBytes = scanState.freeBytes;
    snapshot.totalBytes = scanState.totalBytes;
    snapshot.currentScanPath = currentPath;
    QElapsedTimer cloneTimer;
    cloneTimer.start();
    snapshot.root = cloneNodeLimited(scanState.root, kPreviewDepth, currentPath,
                                     scanState.root->absolutePath, *snapshot.arena);
    if (scanState.profile) {
        ++scanState.profile->progressSnapshots;
        scanState.profile->progressCloneNs += cloneTimer.nsecsElapsed();
    }
    QElapsedTimer callbackTimer;
    callbackTimer.start();
    progressCallback(std::move(snapshot));
    if (scanState.profile) {
        scanState.profile->progressCallbackNs += callbackTimer.nsecsElapsed();
    }
}

} // namespace

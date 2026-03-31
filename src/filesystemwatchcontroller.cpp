// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "filesystemwatchcontroller.h"

#include "filenode.h"
#include "mainwindow_utils.h"

#include <QDir>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>

namespace {
constexpr qint64 kWatcherRefreshThresholdBytes = 10LL * 1024LL * 1024LL;
constexpr int kMaxWatches = 500;
}

FilesystemWatchController::FilesystemWatchController(QObject* parent)
    : QObject(parent)
{
    m_filesystemWatcher = new QFileSystemWatcher(this);
    connect(m_filesystemWatcher, &QFileSystemWatcher::directoryChanged,
            this, &FilesystemWatchController::onDirectoryChanged);

    m_thresholdWatcher = new QFutureWatcher<WatchRefreshDecision>(this);
    connect(m_thresholdWatcher, &QFutureWatcher<WatchRefreshDecision>::finished,
            this, &FilesystemWatchController::onThresholdFinished);

    m_debounceTimer = new QTimer(this);
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(150);
    connect(m_debounceTimer, &QTimer::timeout,
            this, &FilesystemWatchController::processPendingChanges);
}

void FilesystemWatchController::setTreeContext(FileNode* root, FileNode* currentNode)
{
    m_root = root;
    m_currentNode = currentNode;
    rebuildWatchList();
    schedulePendingWork();
}

void FilesystemWatchController::clear()
{
    m_root = nullptr;
    m_currentNode = nullptr;
    m_pendingPaths.clear();
    m_pendingDeltas.clear();
    m_pendingDeltaTotal = 0;
    rebuildWatchList();
    if (m_debounceTimer) {
        m_debounceTimer->stop();
    }
}

void FilesystemWatchController::setPaused(bool paused)
{
    m_paused = paused;
    if (m_paused) {
        if (m_debounceTimer) {
            m_debounceTimer->stop();
        }
        return;
    }

    schedulePendingWork();
}

void FilesystemWatchController::stop()
{
    setPaused(true);
    clear();
}

void FilesystemWatchController::onDirectoryChanged(const QString& path)
{
    if (path.isEmpty() || !m_root) {
        return;
    }

    m_pendingPaths.insert(path);
    schedulePendingWork();
}

void FilesystemWatchController::processPendingChanges()
{
    if (m_paused || m_thresholdInProgress || m_pendingPaths.isEmpty() || !m_root) {
        return;
    }

    const QString changedPath = *m_pendingPaths.constBegin();
    m_pendingPaths.remove(changedPath);

    const QString refreshPath = nearestExistingDirectoryOnDisk(changedPath);
    if (refreshPath.isEmpty()) {
        rebuildWatchList();
        schedulePendingWork();
        return;
    }

    FileNode* existingNode = findNodeByPath(m_root, refreshPath);
    const qint64 knownSize = existingNode ? existingNode->size : 0;

    m_thresholdInProgress = true;
    QFuture<WatchRefreshDecision> future = QtConcurrent::run([refreshPath, knownSize]() {
        WatchRefreshDecision decision;
        decision.refreshPath = refreshPath;
        const qint64 currentSize = directorySizeOnDisk(refreshPath);
        decision.sizeDelta = std::abs(currentSize - knownSize);
        decision.shouldRefresh = decision.sizeDelta >= kWatcherRefreshThresholdBytes;
        return decision;
    });
    m_thresholdWatcher->setFuture(future);
}

void FilesystemWatchController::onThresholdFinished()
{
    const WatchRefreshDecision decision = m_thresholdWatcher->future().takeResult();
    m_thresholdInProgress = false;

    if (!m_root) {
        return;
    }

    const qint64 previousDelta = m_pendingDeltas.value(decision.refreshPath, 0);
    m_pendingDeltaTotal = std::max<qint64>(0, m_pendingDeltaTotal - previousDelta);
    if (decision.sizeDelta > 0) {
        m_pendingDeltas.insert(decision.refreshPath, decision.sizeDelta);
        m_pendingDeltaTotal += decision.sizeDelta;
    } else {
        m_pendingDeltas.remove(decision.refreshPath);
    }

    if (!m_paused
        && (decision.shouldRefresh || m_pendingDeltaTotal >= kWatcherRefreshThresholdBytes)) {
        m_pendingDeltas.clear();
        m_pendingDeltaTotal = 0;
        emit refreshRequested(decision.refreshPath);
        return;
    }

    schedulePendingWork();
}

void FilesystemWatchController::rebuildWatchList()
{
    if (!m_filesystemWatcher) {
        return;
    }

    QStringList newPathsList;
    collectWatchDirectoryPaths(m_root, m_currentNode, newPathsList);
    if (newPathsList.size() > kMaxWatches) {
        newPathsList.resize(kMaxWatches);
    }

    const QSet<QString> desired(newPathsList.begin(), newPathsList.end());
    const QStringList currentDirsList = m_filesystemWatcher->directories();
    const QSet<QString> current(currentDirsList.begin(), currentDirsList.end());

    QStringList toRemove;
    for (const QString& path : current) {
        if (!desired.contains(path)) {
            toRemove.append(path);
        }
    }

    QStringList toAdd;
    for (const QString& path : desired) {
        if (!current.contains(path)) {
            toAdd.append(path);
        }
    }

    if (!toRemove.isEmpty()) {
        m_filesystemWatcher->removePaths(toRemove);
    }
    if (!toAdd.isEmpty()) {
        m_filesystemWatcher->addPaths(toAdd);
    }
}

void FilesystemWatchController::schedulePendingWork()
{
    if (m_paused || m_thresholdInProgress || m_pendingPaths.isEmpty() || !m_root || !m_debounceTimer) {
        return;
    }

    m_debounceTimer->start();
}

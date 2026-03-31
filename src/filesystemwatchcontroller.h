// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QTimer>

class FileNode;

struct WatchRefreshDecision {
    QString refreshPath;
    qint64 sizeDelta = 0;
    bool shouldRefresh = false;
};

class FilesystemWatchController : public QObject {
    Q_OBJECT

public:
    explicit FilesystemWatchController(QObject* parent = nullptr);

    void setTreeContext(FileNode* root, FileNode* currentNode);
    void clear();
    void setPaused(bool paused);
    void stop();

signals:
    void refreshRequested(const QString& path);

private slots:
    void onDirectoryChanged(const QString& path);
    void processPendingChanges();
    void onThresholdFinished();

private:
    void rebuildWatchList();
    void schedulePendingWork();

    QFileSystemWatcher* m_filesystemWatcher = nullptr;
    QFutureWatcher<WatchRefreshDecision>* m_thresholdWatcher = nullptr;
    QTimer* m_debounceTimer = nullptr;
    FileNode* m_root = nullptr;
    FileNode* m_currentNode = nullptr;
    QSet<QString> m_pendingPaths;
    QHash<QString, qint64> m_pendingDeltas;
    qint64 m_pendingDeltaTotal = 0;
    bool m_paused = false;
    bool m_thresholdInProgress = false;
};

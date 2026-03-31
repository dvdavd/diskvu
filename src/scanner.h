// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"
#include "treemapsettings.h"
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

struct ScanProfile {
    qint64 progressCalls = 0;
    qint64 progressSnapshots = 0;
    qint64 progressThrottleSkips = 0;
    qint64 progressCloneNs = 0;
    qint64 progressCallbackNs = 0;
    qint64 mergeNs = 0;
};

struct ScanWarning {
    QString path;
    QString message;
};

struct FsInfo {
    QString canonicalMountRoot;   // e.g. "/" or "/home"
    QString displayMountRoot;     // as reported by QStorageInfo::rootPath()
    qint64 freeBytes = 0;
    qint64 totalBytes = 0;
    bool isLocal = true;          // false for NFS, CIFS, and other network filesystems
};

struct ScanResult {
    std::shared_ptr<NodeArena> arena;
    FileNode* root = nullptr;   // owned by arena
    qint64 freeBytes = 0;
    qint64 totalBytes = 0;
    QString currentScanPath;
    std::shared_ptr<ScanProfile> profile;
    QVector<FsInfo> filesystems;  // per-filesystem data (empty on progress snapshots)
};

class Scanner {
public:
    using ProgressCallback = std::function<void(ScanResult)>;
    using ProgressReadyCallback = std::function<bool()>;
    using ActivityCallback = std::function<void(const QString&, qint64)>;
    using ErrorCallback = std::function<void(const ScanWarning&)>;

    static ScanResult scan(const QString& path, const TreemapSettings& settings = TreemapSettings::defaults(),
                           ProgressCallback progressCallback = {},
                           ProgressReadyCallback progressReadyCallback = {},
                           ActivityCallback activityCallback = {},
                           ErrorCallback errorCallback = {},
                           const std::atomic_bool* cancelFlag = nullptr);

private:
    static qint64 scanNode(FileNode* node, const QString& path, const ScanResult& scanState,
                           const TreemapSettings& settings,
                           const std::vector<QString>& allExcludedPaths,
                           const ProgressReadyCallback& progressReadyCallback,
                           const ProgressCallback& progressCallback, NodeArena& arena,
                           const ActivityCallback& activityCallback,
                           const ErrorCallback& errorCallback,
                           float branchHue, unsigned long long rootDev, const std::atomic_bool* cancelFlag = nullptr, int depth = 0);
};

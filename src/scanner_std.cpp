// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "scanner_common.h"
#include "colorutils.h"

#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <chrono>
#include <filesystem>
#include <future>
#include <limits>
#include <thread>

namespace {

namespace fs = std::filesystem;

fs::path fileSystemPathFromQString(const QString& path)
{
    const QByteArray nativePath = QDir::toNativeSeparators(path).toUtf8();
    return fs::u8path(nativePath.constData());
}

QString qStringFromFileName(const fs::path& path)
{
    const std::string name = path.filename().u8string();
    return QString::fromUtf8(name.data(), static_cast<int>(name.size()));
}

struct DirectoryEntryInfo {
    QString name;
    bool isDirectory = false;
    qint64 fileSize = 0;
};

bool readDirectoryEntryInfo(const fs::directory_entry& entry, DirectoryEntryInfo& info)
{
    std::error_code ec;
    const fs::file_status symlinkStatus = entry.symlink_status(ec);
    if (ec || symlinkStatus.type() == fs::file_type::symlink) {
        return false;
    }

    const fs::file_status status = entry.status(ec);
    if (ec) {
        return false;
    }

    info.name = qStringFromFileName(entry.path());
    info.isDirectory = status.type() == fs::file_type::directory;
    if (!info.isDirectory && status.type() == fs::file_type::regular) {
        const uintmax_t rawSize = entry.file_size(ec);
        if (!ec) {
            info.fileSize = static_cast<qint64>(std::min<uintmax_t>(
                rawSize, static_cast<uintmax_t>(std::numeric_limits<qint64>::max())));
        }
    }

    return !info.name.isEmpty();
}

void reportScanWarning(const Scanner::ErrorCallback& errorCallback,
                       const QString& path, const std::error_code& ec)
{
    if (!errorCallback || !ec) {
        return;
    }

    errorCallback({path, QString::fromStdString(ec.message())});
}

} // namespace

qint64 Scanner::scanNode(FileNode* node, const QString& path, const ScanResult& scanState,
                         const TreemapSettings& settings,
                         const std::vector<QString>& allExcludedPaths,
                         const ProgressReadyCallback& progressReadyCallback,
                         const ProgressCallback& progressCallback, NodeArena& arena,
                         const ActivityCallback& activityCallback,
                         const Scanner::ErrorCallback& errorCallback,
                         float branchHue, unsigned long long rootDev, const std::atomic_bool* cancelFlag, int depth)
{
    (void)rootDev;
    if (depth > kMaxDepth || isCancelled(cancelFlag)) {
        return 0;
    }

    if (activityCallback) {
        activityCallback(path, scanState.root ? scanState.root->size : 0);
    }
    emitProgress(scanState, path, progressReadyCallback, progressCallback);
    qint64 totalSize = 0;

    const QString childPathPrefix = childPathPrefixForParent(path);
    std::error_code ec;
    fs::directory_iterator it(fileSystemPathFromQString(path),
                              fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        reportScanWarning(errorCallback, path, ec);
        return 0;
    }

    const fs::directory_iterator end;
    while (it != end) {
        if (isCancelled(cancelFlag)) {
            break;
        }
        DirectoryEntryInfo entryInfo;
        if (!readDirectoryEntryInfo(*it, entryInfo)) {
            if (const fs::path entryPath = it->path(); !entryPath.empty()) {
                std::error_code entryEc;
                (void)it->status(entryEc);
                reportScanWarning(errorCallback,
                                  QDir::cleanPath(QString::fromUtf8(entryPath.u8string().c_str())),
                                  entryEc);
            }
            it.increment(ec);
            if (ec) {
                reportScanWarning(errorCallback, path, ec);
                break;
            }
            continue;
        }

        FileNode* child = arena.alloc();
        child->name = entryInfo.name;
        child->isDirectory = entryInfo.isDirectory;
        child->parent = node;

        if (entryInfo.isDirectory) {
            const QString childPath = childPathPrefix + child->name;
            if (!shouldSkipPath(childPath, allExcludedPaths)) {
                if (activityCallback) {
                    activityCallback(childPath, scanState.root ? scanState.root->size : 0);
                }
                child->color = ColorUtils::folderColorForBranch(depth + 1, branchHue, settings).rgba();
                node->children.push_back(child);
                child->size = scanNode(child, childPath, scanState, settings, allExcludedPaths,
                                       progressReadyCallback, progressCallback, arena, activityCallback,
                                       errorCallback, branchHue, rootDev, cancelFlag, depth + 1);
                totalSize += child->size;
                if (depth <= 1 && !isCancelled(cancelFlag))
                    emitProgress(scanState, childPath, progressReadyCallback, progressCallback);
            }
        } else {
            const QString childPath = activityCallback || (depth <= 1 && progressCallback)
                ? childPathPrefix + child->name
                : QString();
            if (activityCallback && !childPath.isEmpty()) {
                activityCallback(childPath, entryInfo.fileSize);
            }
            child->size = entryInfo.fileSize;
            child->extKey = ColorUtils::packFileExt(child->name);
            child->color = ColorUtils::fileColorForName(child->name, settings).rgba();
            node->children.push_back(child);
            totalSize += entryInfo.fileSize;
            if (depth <= 1 && !isCancelled(cancelFlag))
                emitProgress(scanState, childPath.isEmpty() ? path : childPath, progressReadyCallback, progressCallback);
        }
        it.increment(ec);
        if (ec) {
            reportScanWarning(errorCallback, path, ec);
            break;
        }
    }

    node->size = totalSize;
    return totalSize;
}

ScanResult Scanner::scan(const QString& path, const TreemapSettings& settings,
                         ProgressCallback progressCallback,
                         ProgressReadyCallback progressReadyCallback,
                         ActivityCallback activityCallback,
                         ErrorCallback errorCallback,
                         const std::atomic_bool* cancelFlag)
{
    ScanResult result;
    result.arena = std::make_shared<NodeArena>();
    result.profile = std::make_shared<ScanProfile>();

    const QString normalizedPath = QDir::cleanPath(path);
    QFileInfo rootInfo(normalizedPath);
    FileNode* root = result.arena->alloc();
    root->name = rootInfo.fileName().isEmpty() ? normalizedPath : rootInfo.fileName();
    root->absolutePath = normalizedPath;
    root->isDirectory = true;
    root->parent = nullptr;
    root->color = ColorUtils::folderColorForBranch(
        0, ColorUtils::initialFolderBranchHue(root, settings), settings).rgba();
    result.root = root;
    const bool trackWorkerPaths = settings.enableScanActivityTracking && static_cast<bool>(progressCallback);
    const bool trackByteActivity = settings.enableScanActivityTracking && static_cast<bool>(activityCallback);
    auto liveBytesSeen = trackByteActivity
        ? std::make_shared<std::atomic<qint64>>(0)
        : std::shared_ptr<std::atomic<qint64>>();

    if (!rootInfo.isReadable()) {
        QStorageInfo storageInfo(normalizedPath);
        result.freeBytes = storageInfo.bytesFree();
        result.totalBytes = storageInfo.bytesTotal();
        return result;
    }

    emitProgress(result, normalizedPath, progressReadyCallback, progressCallback, true);
    if (isCancelled(cancelFlag)) {
        result.root = nullptr;
        return result;
    }

    std::vector<QString> allExcludedPaths;
    const QStringList defaultExcluded = defaultExcludedPathsForScanRoot(normalizedPath);
    allExcludedPaths.reserve(defaultExcluded.size() + settings.excludedPaths.size());
    for (const QString& excludedPath : defaultExcluded) {
        allExcludedPaths.push_back(QDir::cleanPath(excludedPath));
    }
    for (const QString& excludedPath : settings.excludedPaths) {
        const QString cleanedPath = QDir::cleanPath(excludedPath);
        if (std::find(allExcludedPaths.begin(), allExcludedPaths.end(), cleanedPath) == allExcludedPaths.end()) {
            allExcludedPaths.push_back(cleanedPath);
        }
    }

    struct DirTask {
        FileNode* placeholder = nullptr;
        QString childPath;
        float branchHue = 0.0f;
        unsigned long long rootDev = 0;
        int depth = 0;
    };

    struct PartitionTask {
        FileNode* parent = nullptr;
        QString path;
        float branchHue = 0.0f;
        unsigned long long rootDev = 0;
        int depth = 0;
    };

    std::vector<DirTask> dirTasks;
    std::vector<PartitionTask> partitionQueue;
    partitionQueue.push_back({root, normalizedPath, 0.0f, 0, 0});

    for (size_t partitionIndex = 0; partitionIndex < partitionQueue.size(); ++partitionIndex) {
        if (isCancelled(cancelFlag)) {
            break;
        }

        const PartitionTask partition = partitionQueue[partitionIndex];
        if (partition.depth > 0) {
            addSizeUpwards(partition.parent, -kProvisionalDirectoryPreviewSize);
        }
        const QString childPathPrefix = childPathPrefixForParent(partition.path);
        std::error_code ec;
        fs::directory_iterator it(fileSystemPathFromQString(partition.path),
                                  fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            reportScanWarning(errorCallback, partition.path, ec);
            continue;
        }


        const fs::directory_iterator end;
        while (it != end) {
            if (isCancelled(cancelFlag)) {
                break;
            }
            DirectoryEntryInfo entryInfo;
            if (!readDirectoryEntryInfo(*it, entryInfo)) {
                if (const fs::path entryPath = it->path(); !entryPath.empty()) {
                    std::error_code entryEc;
                    (void)it->status(entryEc);
                    reportScanWarning(errorCallback,
                                      QDir::cleanPath(QString::fromUtf8(entryPath.u8string().c_str())),
                                      entryEc);
                }
                it.increment(ec);
                if (ec) {
                    reportScanWarning(errorCallback, partition.path, ec);
                    break;
                }
                continue;
            }
            FileNode* child = result.arena->alloc();
            child->name = entryInfo.name;
            child->isDirectory = entryInfo.isDirectory;
            child->parent = partition.parent;

            if (entryInfo.isDirectory) {
                const QString childPath = childPathPrefix + child->name;
                if (!shouldSkipPath(childPath, allExcludedPaths)) {
                    const float branchHue = partition.depth == 0
                        ? ColorUtils::topLevelFolderBranchHue(child->name, settings)
                        : partition.branchHue;
                    child->color = ColorUtils::folderColorForBranch(
                        partition.depth + 1, branchHue, settings).rgba();
                    child->size = kProvisionalDirectoryPreviewSize;
                    partition.parent->children.push_back(child);
                    addSizeUpwards(partition.parent, child->size);

                    if (partition.depth + 1 < settings.parallelPartitionDepth) {
                        partitionQueue.push_back({child, childPath, branchHue, partition.rootDev, partition.depth + 1});
                    } else {
                        dirTasks.push_back({child, childPath, branchHue, partition.rootDev, partition.depth + 1});
                    }

                }
            } else {
                child->size = entryInfo.fileSize;
                child->extKey = ColorUtils::packFileExt(child->name);
                child->color = ColorUtils::fileColorForName(child->name, settings).rgba();
                partition.parent->children.push_back(child);
                addSizeUpwards(partition.parent, entryInfo.fileSize);
                if (trackByteActivity) {
                    liveBytesSeen->fetch_add(entryInfo.fileSize, std::memory_order_relaxed);
                    activityCallback(childPathPrefix + child->name,
                                     liveBytesSeen->load(std::memory_order_relaxed));
                }
            }
            it.increment(ec);
            if (ec) {
                reportScanWarning(errorCallback, partition.path, ec);
                break;
            }
        }
    }

    if (isCancelled(cancelFlag)) {
        result.root = nullptr;
        return result;
    }

    emitProgress(result, normalizedPath, progressReadyCallback, progressCallback, true);

    struct WorkerResult {
        std::shared_ptr<NodeArena> arena;
        FileNode* workerRoot = nullptr;
        FileNode* placeholder = nullptr;
    };

    struct WorkerBatchResult {
        std::vector<WorkerResult> results;
    };

    const unsigned int concurrencyHint = std::max(1u, std::thread::hardware_concurrency());
    const size_t workerCount = std::min(dirTasks.size(), static_cast<size_t>(concurrencyHint));
    std::atomic_size_t nextTaskIndex {0};
    LiveWorkerPaths liveWorkerPaths;
    if (trackWorkerPaths) {
        liveWorkerPaths.paths.resize(workerCount);
    }
    std::vector<std::future<WorkerBatchResult>> futures;
    futures.reserve(workerCount);

    for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        futures.push_back(std::async(std::launch::async,
                                     [&, workerIndex, cancelFlag]() -> WorkerBatchResult {
            WorkerBatchResult batch;
            while (true) {
                const size_t taskIndex = nextTaskIndex.fetch_add(1, std::memory_order_relaxed);
                if (taskIndex >= dirTasks.size() || isCancelled(cancelFlag)) {
                    if (trackWorkerPaths) {
                        updateLiveWorkerPath(&liveWorkerPaths, workerIndex, QString());
                    }
                    break;
                }

                const DirTask& task = dirTasks[taskIndex];
                WorkerResult r;
                r.placeholder = task.placeholder;
                r.arena = std::make_shared<NodeArena>();
                r.workerRoot = r.arena->alloc();
                r.workerRoot->name = task.placeholder->name;
                r.workerRoot->absolutePath = task.childPath;
                r.workerRoot->isDirectory = true;
                r.workerRoot->parent = nullptr;
                r.workerRoot->color = task.placeholder->color;
                if (isCancelled(cancelFlag)) {
                    r.workerRoot = nullptr;
                    batch.results.push_back(std::move(r));
                    if (trackWorkerPaths) {
                        updateLiveWorkerPath(&liveWorkerPaths, workerIndex, QString());
                    }
                    break;
                }

                if (trackWorkerPaths) {
                    updateLiveWorkerPath(&liveWorkerPaths, workerIndex, task.childPath);
                }

                std::function<void(const QString&, qint64)> workerActivityCallback;
                if (trackWorkerPaths || trackByteActivity) {
                    workerActivityCallback = [&](const QString& currentPath, qint64 itemBytes) {
                        if (trackWorkerPaths) {
                            updateLiveWorkerPath(&liveWorkerPaths, workerIndex, currentPath);
                        }

                        if (!trackByteActivity) {
                            return;
                        }

                        qint64 totalBytesSeen = liveBytesSeen->load(std::memory_order_relaxed);
                        if (itemBytes > 0) {
                            totalBytesSeen = liveBytesSeen->fetch_add(itemBytes, std::memory_order_relaxed) + itemBytes;
                        }

                        static thread_local QElapsedTimer activityTimer;
                        static thread_local bool activityTimerStarted = false;
                        if (!activityTimerStarted) {
                            activityTimer.start();
                            activityTimerStarted = true;
                        }
                        if (activityTimer.elapsed() < kActivityIntervalMs) {
                            return;
                        }
                        activityTimer.restart();
                        activityCallback(currentPath, totalBytesSeen);
                    };
                }
                ScanResult dummy;
                Scanner::scanNode(r.workerRoot, task.childPath, dummy, settings, allExcludedPaths, {},
                                  nullptr,
                                  *r.arena, workerActivityCallback,
                                  {},
                                  task.branchHue, task.rootDev, cancelFlag, task.depth);
                if (isCancelled(cancelFlag)) {
                    r.workerRoot = nullptr;
                }
                if (trackWorkerPaths) {
                    updateLiveWorkerPath(&liveWorkerPaths, workerIndex, QString());
                }
                batch.results.push_back(std::move(r));
            }
            return batch;
        }));
    }

    std::vector<bool> collected(futures.size(), false);
    size_t remaining = futures.size();

    while (remaining > 0) {
        if (isCancelled(cancelFlag)) {
            break;
        }

        bool anyNew = false;
        for (size_t i = 0; i < futures.size(); ++i) {
            if (collected[i])
                continue;
            if (futures[i].wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;

            WorkerBatchResult batch = futures[i].get();
            for (WorkerResult& r : batch.results) {
                if (!r.workerRoot) {
                    continue;
                }
                const qint64 provisionalSize = r.placeholder->size;
                r.placeholder->size = r.workerRoot->size;
                r.placeholder->children = std::move(r.workerRoot->children);
                for (FileNode* child : r.placeholder->children) {
                    child->parent = r.placeholder;
                }
                addSizeUpwards(r.placeholder->parent, r.placeholder->size - provisionalSize);
                QElapsedTimer mergeTimer;
                mergeTimer.start();
                result.arena->merge(std::move(*r.arena));
                if (result.profile) {
                    result.profile->mergeNs += mergeTimer.nsecsElapsed();
                }
            }
            collected[i] = true;
            --remaining;
            anyNew = true;
            if (trackWorkerPaths && !isCancelled(cancelFlag)) {
                const QString livePath = currentLiveWorkerPath(&liveWorkerPaths);
                emitProgress(result, livePath.isEmpty() ? path : livePath, progressReadyCallback, progressCallback);
            }
        }
        if (!anyNew && remaining > 0) {
            if (trackWorkerPaths) {
                const QString livePath = currentLiveWorkerPath(&liveWorkerPaths);
                if (!livePath.isEmpty()) {
                    emitProgress(result, livePath, progressReadyCallback, progressCallback);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }

    for (size_t i = 0; i < futures.size(); ++i) {
        if (!collected[i]) {
            futures[i].wait();
        }
    }

    if (isCancelled(cancelFlag)) {
        result.root = nullptr;
        return result;
    }

    QStorageInfo storageInfo(path);
    result.freeBytes = storageInfo.bytesFree();
    result.totalBytes = storageInfo.bytesTotal();

    const QString canonicalScanRoot = QFileInfo(normalizedPath).canonicalFilePath();
    const QString primaryFsRoot = QFileInfo(storageInfo.rootPath()).canonicalFilePath();

    // Primary filesystem entry
    QSet<QString> seenDevices;
    seenDevices.insert(storageInfo.device());
    const auto isLocalDevice = [](const QByteArray& dev) {
        return dev.startsWith('/') && !dev.startsWith("//");
    };
    result.filesystems.push_back({primaryFsRoot, storageInfo.rootPath(),
                                   storageInfo.bytesFree(), storageInfo.bytesTotal(),
                                   isLocalDevice(storageInfo.device())});

    for (const QStorageInfo& vol : QStorageInfo::mountedVolumes()) {
        if (!vol.isValid() || !vol.isReady())
            continue;
        const QString volRoot = QFileInfo(vol.rootPath()).canonicalFilePath();
        if (volRoot == primaryFsRoot)
            continue;
        const bool withinScan = pathIsWithinCandidate(volRoot, canonicalScanRoot);
        if (!withinScan)
            continue;
        const QString cleanVolRoot = QDir::cleanPath(vol.rootPath());
        bool excluded = false;
        for (const QString& excl : allExcludedPaths) {
            if (pathIsWithinCandidate(cleanVolRoot, excl)) {
                excluded = true;
                break;
            }
        }
        if (excluded)
            continue;
        // Skip volumes sharing the same underlying device (e.g. btrfs subvolumes)
        if (seenDevices.contains(vol.device()))
            continue;
        seenDevices.insert(vol.device());
        result.freeBytes += vol.bytesFree();
        result.totalBytes += vol.bytesTotal();
        result.filesystems.push_back({volRoot, vol.rootPath(),
                                       vol.bytesFree(), vol.bytesTotal(),
                                       isLocalDevice(vol.device())});
    }

    emitProgress(result, path, progressReadyCallback, progressCallback, true);

    return result;
}

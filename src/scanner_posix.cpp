// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "scanner_common.h"
#include "colorutils.h"

#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <chrono>
#include <future>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <cstring>
#include <sys/stat.h>

// (shared scanner helpers are in scanner_common.h)

namespace {

DIR* openDirectoryWithRevalidate(const QByteArray& pathBytes)
{
    DIR* dirp = opendir(pathBytes.constData());
    if (dirp) {
        return dirp;
    }

    const int firstErrno = errno;
    struct stat st;
    if (stat(pathBytes.constData(), &st) == 0 && S_ISDIR(st.st_mode)) {
        dirp = opendir(pathBytes.constData());
        if (dirp) {
            return dirp;
        }
    }

    errno = firstErrno;
    return nullptr;
}

void reportScanWarning(const Scanner::ErrorCallback& errorCallback,
                       const QString& path, int err)
{
    if (!errorCallback || err == 0) {
        return;
    }

    errorCallback({path, QString::fromLocal8Bit(std::strerror(err))});
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

    if (depth > kMaxDepth || isCancelled(cancelFlag)) {
        return 0;
    }

    if (activityCallback) {
        activityCallback(path, scanState.root ? scanState.root->size : 0);
    }
    emitProgress(scanState, path, progressReadyCallback, progressCallback);
    qint64 totalSize = 0;

    const QByteArray pathBytes = QFile::encodeName(path);
    const QString childPathPrefix = childPathPrefixForParent(path);
    DIR* dirp = openDirectoryWithRevalidate(pathBytes);
    if (!dirp) {
        reportScanWarning(errorCallback, path, errno);
        return 0;
    }

    const int dfd = dirfd(dirp);
    struct dirent* entry;
    while ((entry = readdir(dirp)) != nullptr) {
        if (isCancelled(cancelFlag)) {
            break;
        }

        const char* dname = entry->d_name;
        if (dname[0] == '.' && (dname[1] == '\0' || (dname[1] == '.' && dname[2] == '\0')))
            continue;

        unsigned char dtype = entry->d_type;
        if (dtype == DT_LNK)
            continue;

        bool isDir = (dtype == DT_DIR);
        qint64 fileSize = 0;

        if (dtype == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
                continue;
            }
            if (S_ISLNK(st.st_mode))
                continue;
            isDir = S_ISDIR(st.st_mode);
            if (isDir) {
                if (settings.limitToSameFilesystem && st.st_dev != rootDev)
                    continue;
            } else {
                fileSize = st.st_size;
            }
        } else if (isDir) {
            if (settings.limitToSameFilesystem) {
                struct stat st;
                if (fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                    reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
                    continue;
                }
                if (st.st_dev != rootDev)
                    continue;
            }
        } else if (!isDir) {
            struct stat st;
            if (fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW) == 0)
                fileSize = st.st_size;
        }

        const QString childName = QString::fromLocal8Bit(dname);

        if (isDir) {
            const QString childPath = childPathPrefix + childName;
            if (shouldSkipPath(childPath, allExcludedPaths))
                continue;

            if (activityCallback) {
                activityCallback(childPath, scanState.root ? scanState.root->size : 0);
            }
            FileNode* child = arena.alloc();
            child->name = childName;
            child->isDirectory = true;
            child->parent = node;
            child->color = ColorUtils::folderColor(depth + 1, branchHue, settings).rgba();
            node->children.push_back(child);
            child->size = scanNode(child, childPath, scanState, settings, allExcludedPaths,
                                   progressReadyCallback, progressCallback, arena, activityCallback,
                                   errorCallback, branchHue, rootDev, cancelFlag, depth + 1);
            totalSize += child->size;
            if (depth <= 1 && !isCancelled(cancelFlag))
                emitProgress(scanState, childPath, progressReadyCallback, progressCallback);
        } else {
            const QString childPath = activityCallback || (depth <= 1 && progressCallback)
                ? childPathPrefix + childName
                : QString();
            if (activityCallback && !childPath.isEmpty()) {
                activityCallback(childPath, fileSize);
            }
            FileNode* child = arena.alloc();
            child->name = childName;
            child->isDirectory = false;
            child->parent = node;
            child->size = fileSize;
            child->extKey = ColorUtils::packFileExt(child->name);
            child->color = ColorUtils::fileColorForName(child->name, settings).rgba();
            node->children.push_back(child);
            totalSize += fileSize;
            if (depth <= 1 && !isCancelled(cancelFlag))
                emitProgress(scanState, childPath.isEmpty() ? path : childPath, progressReadyCallback, progressCallback);
        }
    }

    closedir(dirp);
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
    root->color = ColorUtils::folderColor(
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
        unsigned long long rootDev;
        int depth = 0;
    };

    struct PartitionTask {
        FileNode* parent = nullptr;
        QString path;
        float branchHue = 0.0f;
        unsigned long long rootDev;
        int depth = 0;
    };

    struct stat startSt;
    dev_t initialRootDev = 0;
    if (stat(normalizedPath.toLocal8Bit().constData(), &startSt) == 0) {
        initialRootDev = startSt.st_dev;
    }

    std::vector<DirTask> dirTasks;
    std::vector<PartitionTask> partitionQueue;
    partitionQueue.push_back({root, normalizedPath, 0.0f, static_cast<unsigned long long>(initialRootDev), 0});

    for (size_t partitionIndex = 0; partitionIndex < partitionQueue.size(); ++partitionIndex) {
        if (isCancelled(cancelFlag)) {
            break;
        }

        const PartitionTask partition = partitionQueue[partitionIndex];
        if (partition.depth > 0) {
            addSizeUpwards(partition.parent, -kProvisionalDirectoryPreviewSize);
        }
        const QByteArray pathBytes = QFile::encodeName(partition.path);
        const QString childPathPrefix = childPathPrefixForParent(partition.path);
        DIR* dirp = openDirectoryWithRevalidate(pathBytes);
        if (!dirp) {
            reportScanWarning(errorCallback, partition.path, errno);
            continue;
        }


        const int dfd = dirfd(dirp);
        struct dirent* entry;
        while ((entry = readdir(dirp)) != nullptr) {
            if (isCancelled(cancelFlag)) {
                break;
            }

            const char* dname = entry->d_name;
            if (dname[0] == '.' && (dname[1] == '\0' || (dname[1] == '.' && dname[2] == '\0'))) {
                continue;
            }

            const unsigned char dtype = entry->d_type;
            if (dtype == DT_LNK) {
                continue;
            }

            bool isDir = (dtype == DT_DIR);
            qint64 fileSize = 0;
            if (dtype == DT_UNKNOWN) {
                struct stat st;
                if (fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                    reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
                    continue;
                }
                if (S_ISLNK(st.st_mode)) {
                    continue;
                }
                isDir = S_ISDIR(st.st_mode);
                if (isDir) {
                    if (settings.limitToSameFilesystem && st.st_dev != partition.rootDev)
                        continue;
                } else {
                    fileSize = st.st_size;
                }
            } else if (isDir) {
                if (settings.limitToSameFilesystem) {
                    struct stat st;
                    if (fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW) != 0) {
                        reportScanWarning(errorCallback,
                                          childPathPrefix + QString::fromLocal8Bit(dname), errno);
                        continue;
                    }
                    if (st.st_dev != partition.rootDev)
                        continue;
                }
            } else if (!isDir) {
                struct stat st;
                if (fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                    fileSize = st.st_size;
                }
            }

            const QString childName = QString::fromLocal8Bit(dname);

            if (isDir) {
                const QString childPath = childPathPrefix + childName;
                if (shouldSkipPath(childPath, allExcludedPaths)) {
                    continue;
                }

                const float branchHue = partition.depth == 0
                    ? ColorUtils::topLevelFolderBranchHue(childName, settings)
                    : partition.branchHue;
                FileNode* child = result.arena->alloc();
                child->name = childName;
                child->isDirectory = true;
                child->parent = partition.parent;
                child->color = ColorUtils::folderColor(
                    partition.depth + 1, branchHue, settings).rgba();
                child->size = kProvisionalDirectoryPreviewSize;
                partition.parent->children.push_back(child);
                addSizeUpwards(partition.parent, child->size);

                if (partition.depth + 1 < settings.parallelPartitionDepth) {
                    partitionQueue.push_back({child, childPath, branchHue, partition.rootDev, partition.depth + 1});
                } else {
                    dirTasks.push_back({child, childPath, branchHue, partition.rootDev, partition.depth + 1});
                }

            } else {
                FileNode* child = result.arena->alloc();
                child->name = childName;
                child->isDirectory = false;
                child->parent = partition.parent;
                child->size = fileSize;
                child->extKey = ColorUtils::packFileExt(child->name);
                child->color = ColorUtils::fileColorForName(child->name, settings).rgba();
                partition.parent->children.push_back(child);
                addSizeUpwards(partition.parent, fileSize);
                if (trackByteActivity) {
                    liveBytesSeen->fetch_add(fileSize, std::memory_order_relaxed);
                    activityCallback(childPathPrefix + child->name,
                                     liveBytesSeen->load(std::memory_order_relaxed));
                }
            }
        }

        closedir(dirp);
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
                                  errorCallback,
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
        // When limited to a single filesystem, don't report other filesystems
        if (settings.limitToSameFilesystem)
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

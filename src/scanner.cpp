// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "scanner.h"
#include "scanner_common.h"
#include "colorutils.h"
#include "filesystemutils.h"

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <chrono>
#include <future>
#include <thread>
#include <mutex>

ScanResult Scanner::scan(const QString& path, const TreemapSettings& settings,
                         ProgressCallback progressCallback,
                         ProgressReadyCallback progressReadyCallback,
                         ActivityCallback activityCallback,
                         ErrorCallback errorCallback,
                         std::shared_ptr<const std::atomic_bool> cancelFlag)
{
    ScanResult result;
    result.arena = std::make_shared<NodeArena>();

    const QString normalizedPath = QDir::cleanPath(path);
    const QStorageInfo scanStorageInfo(normalizedPath);
    const bool scanIsLocal = isLocalFilesystemPath(normalizedPath);
    if (scanIsLocal) {
        result.hardLinkTracker = std::make_shared<HardLinkTracker>();
    }

    QFileInfo rootInfo(normalizedPath);
    FileNode* root = result.arena->alloc();
    root->name = normalizedPath;
    result.rootPath = normalizedPath;
    root->setIsDirectory(true);
    root->parent = nullptr;
    const float initialHue = ColorUtils::initialFolderBranchHue(root, settings);
    bool rootInMarkedBranch = false;
    if (!settings.folderColorMarks.isEmpty()) {
        auto it = settings.folderColorMarks.constFind(normalizedPath);
        if (it != settings.folderColorMarks.constEnd()) {
            root->setColorMark(static_cast<uint8_t>(it.value()));
            root->color = ColorUtils::folderColorForMark(0, ColorUtils::markHue(static_cast<FolderMark>(root->colorMark())), settings).rgba();
            rootInMarkedBranch = true;
        }
    }
    if (!rootInMarkedBranch) {
        root->color = ColorUtils::folderColor(0, initialHue, settings).rgba();
    }
    if (!settings.folderIconMarks.isEmpty()) {
        auto it = settings.folderIconMarks.constFind(normalizedPath);
        if (it != settings.folderIconMarks.constEnd()) {
            root->setIconMark(static_cast<uint8_t>(it.value()));
        }
    }

    result.root = root;
    const bool trackWorkerPaths = settings.enableScanActivityTracking && static_cast<bool>(progressCallback);
    const bool trackByteActivity = settings.enableScanActivityTracking && static_cast<bool>(activityCallback);
    auto liveBytesSeen = trackByteActivity
        ? std::make_shared<std::atomic<qint64>>(0)
        : std::shared_ptr<std::atomic<qint64>>();
    const int effectiveParallelPartitionDepth = scanIsLocal
        ? settings.parallelPartitionDepth
        : std::min(settings.parallelPartitionDepth, 2);

    const unsigned int concurrencyHint = std::max(1u, std::thread::hardware_concurrency());
    const size_t effectiveWorkerCount = scanIsLocal ? concurrencyHint : std::min(concurrencyHint, 2u);
    const size_t targetTaskCount = effectiveWorkerCount * 4;

    if (!rootInfo.isReadable()) {
        result.freeBytes = scanStorageInfo.bytesFree();
        result.totalBytes = scanStorageInfo.bytesTotal();
        rebuildScanResultSnapshot(result);
        return result;
    }

    emitProgress(result, normalizedPath, progressReadyCallback, progressCallback, true);
    if (isCancelled(cancelFlag)) {
        result.root = nullptr;
        rebuildScanResultSnapshot(result);
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

    const unsigned long long initialRootDev = initialRootDevice(normalizedPath);

    auto throttler = std::make_shared<ScanThrottler>();

    std::vector<Scanner::DirTask> dirTasks;
    std::vector<Scanner::PartitionTask> partitionQueue;
    partitionQueue.push_back({root, normalizedPath, rootInMarkedBranch ? ColorUtils::markHue(static_cast<FolderMark>(root->colorMark())) : initialHue, initialRootDev, 0, rootInMarkedBranch});

    for (size_t partitionIndex = 0; partitionIndex < partitionQueue.size(); ++partitionIndex) {
        if (isCancelled(cancelFlag)) {
            break;
        }

        partitionNode(partitionQueue[partitionIndex], settings, allExcludedPaths, errorCallback,
                      cancelFlag, partitionQueue, dirTasks, *result.arena, activityCallback,
                      liveBytesSeen, *throttler, (int)targetTaskCount, effectiveParallelPartitionDepth,
                      result.hardLinkTracker);
    }

    if (isCancelled(cancelFlag)) {
        result.root = nullptr;
        rebuildScanResultSnapshot(result);
        return result;
    }

    emitProgress(result, normalizedPath, progressReadyCallback, progressCallback, true);

    struct WorkerResult {
        FileNode* workerRoot = nullptr;
        FileNode* placeholder = nullptr;
    };

    struct CompletedTaskResult {
        std::shared_ptr<NodeArena> arena;
        WorkerResult result;
    };

    // Always launch a full complement of workers.
    const size_t workerCount = dirTasks.empty() ? 0 : effectiveWorkerCount;

    // Heap-allocate shared worker state so threads can safely outlive this function on cancel.
    struct SharedWorkerState {
        std::vector<Scanner::DirTask> tasks;
        std::atomic_size_t nextTaskIndex{0};
        LiveWorkerPaths liveWorkerPaths;

        std::mutex completedMutex;
        std::vector<CompletedTaskResult> completedTasks;
    };
    auto shared = std::make_shared<SharedWorkerState>();
    shared->tasks = std::move(dirTasks);
    if (trackWorkerPaths) {
        shared->liveWorkerPaths.paths.resize(workerCount);
    }

    std::vector<std::future<void>> futures;
    futures.reserve(workerCount);

    for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        futures.push_back(std::async(std::launch::async,
                                     [shared, workerIndex, cancelFlag,
                                      trackWorkerPaths, trackByteActivity,
                                      liveBytesSeen, activityCallback, errorCallback,
                                      hardLinkTracker = result.hardLinkTracker,
                                      settings, allExcludedPaths,
                                      arena = result.arena, throttler]() {
            while (true) {
                if (isCancelled(cancelFlag))
                    break;

                const size_t taskIndex = shared->nextTaskIndex.fetch_add(1, std::memory_order_relaxed);
                if (taskIndex >= shared->tasks.size()) {
                    break;
                }

                const Scanner::DirTask& task = shared->tasks[taskIndex];

                WorkerResult r;
                r.placeholder = task.placeholder;
                auto taskArena = std::make_shared<NodeArena>();
                r.workerRoot = taskArena->alloc();
                r.workerRoot->name = task.placeholder->name;
                r.workerRoot->setIsDirectory(true);
                r.workerRoot->parent = nullptr;
                r.workerRoot->color = task.placeholder->color;

                if (trackWorkerPaths)
                    updateLiveWorkerPath(&shared->liveWorkerPaths, workerIndex, task.childPath);

                std::function<void(const QString&, qint64)> workerActivityCallback;
                if (trackWorkerPaths || trackByteActivity) {
                    workerActivityCallback = [&](const QString& currentPath, qint64 itemBytes) {
                        if (trackWorkerPaths)
                            updateLiveWorkerPath(&shared->liveWorkerPaths, workerIndex, currentPath);
                        if (!trackByteActivity)
                            return;
                        qint64 totalBytesSeen = liveBytesSeen->load(std::memory_order_relaxed);
                        if (itemBytes > 0)
                            totalBytesSeen = liveBytesSeen->fetch_add(itemBytes, std::memory_order_relaxed) + itemBytes;
                        static thread_local QElapsedTimer activityTimer;
                        static thread_local bool activityTimerStarted = false;
                        if (!activityTimerStarted) { activityTimer.start(); activityTimerStarted = true; }
                        if (activityTimer.elapsed() < kActivityIntervalMs) return;
                        activityTimer.restart();
                        activityCallback(currentPath, totalBytesSeen);
                    };
                }

                ScanResult dummy;
                dummy.root = r.workerRoot;
                dummy.rootPath = task.childPath;
                dummy.hardLinkTracker = hardLinkTracker;

                bool taskThrottled = !isLocalFilesystemPath(task.childPath);

                Scanner::scanNode(r.workerRoot, task.childPath, dummy, settings, allExcludedPaths, {},
                                  nullptr, *taskArena, workerActivityCallback, errorCallback,
                                  task.branchHue, task.rootDev, cancelFlag, task.depth,
                                  task.inMarkedBranch, throttler.get(), taskThrottled);

                if (isCancelled(cancelFlag))
                    r.workerRoot = nullptr;
                if (trackWorkerPaths)
                    updateLiveWorkerPath(&shared->liveWorkerPaths, workerIndex, QString());
                {
                    std::lock_guard<std::mutex> lock(shared->completedMutex);
                    shared->completedTasks.push_back({std::move(taskArena), std::move(r)});
                }
            }

            if (trackWorkerPaths)
                updateLiveWorkerPath(&shared->liveWorkerPaths, workerIndex, QString());
        }));
    }

    std::vector<bool> collected(futures.size(), false);
    size_t remaining = futures.size();

    while (remaining > 0) {
        if (isCancelled(cancelFlag)) {
            break;
        }

        bool anyNew = false;
        std::vector<CompletedTaskResult> completed;
        {
            std::lock_guard<std::mutex> lock(shared->completedMutex);
            completed.swap(shared->completedTasks);
        }
        for (CompletedTaskResult& completedTask : completed) {
            WorkerResult& r = completedTask.result;
            if (!r.workerRoot || !r.placeholder) {
                continue;
            }

            const qint64 provisionalSize = r.placeholder->size;
            const int provisionalFileCount = r.placeholder->subtreeFileCount;
            r.placeholder->size = r.workerRoot->size;
            r.placeholder->subtreeFileCount = r.workerRoot->subtreeFileCount;

            r.placeholder->firstChild = r.workerRoot->firstChild;
            for (FileNode* child = r.placeholder->firstChild; child; child = child->nextSibling) {
                child->parent = r.placeholder;
            }

            addStatsUpwards(r.placeholder->parent, r.placeholder->size - provisionalSize,
                            r.placeholder->subtreeFileCount - provisionalFileCount);

            result.arena->merge(std::move(*completedTask.arena));

            anyNew = true;
            result.fileCount = result.root->subtreeFileCount;
            const QString livePath = currentLiveWorkerPath(&shared->liveWorkerPaths);
            emitProgress(result, livePath.isEmpty() ? path : livePath, progressReadyCallback, progressCallback);
        }

        for (size_t i = 0; i < futures.size(); ++i) {
            if (collected[i])
                continue;
            if (futures[i].wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;

            futures[i].get();
            collected[i] = true;
            --remaining;
            anyNew = true;
        }
        if (!anyNew && remaining > 0) {
            if (trackWorkerPaths) {
                result.fileCount = result.root->subtreeFileCount;
                const QString livePath = currentLiveWorkerPath(&shared->liveWorkerPaths);
                if (!livePath.isEmpty()) {
                    emitProgress(result, livePath, progressReadyCallback, progressCallback);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }

    if (isCancelled(cancelFlag)) {
        auto abandoned = std::make_shared<std::vector<std::future<void>>>();
        for (size_t i = 0; i < futures.size(); ++i) {
            if (!collected[i]) {
                abandoned->push_back(std::move(futures[i]));
            }
        }
        if (!abandoned->empty()) {
            std::thread([abandoned]() {
                for (auto& f : *abandoned) {
                    f.wait();
                }
            }).detach();
        }
    } else {
        for (size_t i = 0; i < futures.size(); ++i) {
            if (!collected[i]) {
                futures[i].wait();
            }
        }
    }

    if (isCancelled(cancelFlag)) {
        result.root = nullptr;
        rebuildScanResultSnapshot(result);
        return result;
    }

    recomputeApparentSizes(result.root);
    result.fileCount = result.root->subtreeFileCount;

    QStorageInfo storageInfo(path);
    result.freeBytes = storageInfo.bytesFree();
    result.totalBytes = storageInfo.bytesTotal();

    const QString canonicalScanRoot = QFileInfo(normalizedPath).canonicalFilePath();
    const QString primaryFsRoot = QFileInfo(storageInfo.rootPath()).canonicalFilePath();

    QSet<QString> seenDevices;
    seenDevices.insert(spaceSharingDeviceId(storageInfo));
    result.filesystems.push_back({primaryFsRoot, storageInfo.rootPath(),
                                   storageInfo.bytesFree(), storageInfo.bytesTotal(),
                                   isLocalFilesystem(storageInfo)});

    for (const QStorageInfo& vol : QStorageInfo::mountedVolumes()) {
        if (!vol.isValid() || !vol.isReady())
            continue;
        const QString volRoot = QFileInfo(vol.rootPath()).canonicalFilePath();
        if (volRoot == primaryFsRoot)
            continue;
        const bool withinScan = pathIsWithinCandidate(volRoot, canonicalScanRoot);
        if (!withinScan)
            continue;
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
        const QString devId = spaceSharingDeviceId(vol);
        if (seenDevices.contains(devId))
            continue;
        seenDevices.insert(devId);
        result.freeBytes += vol.bytesFree();
        result.totalBytes += vol.bytesTotal();
        result.filesystems.push_back({volRoot, vol.rootPath(),
                                       vol.bytesFree(), vol.bytesTotal(),
                                       isLocalFilesystem(vol)});
    }

    emitProgress(result, path, progressReadyCallback, progressCallback, true);
    rebuildScanResultSnapshot(result);
    return result;
}

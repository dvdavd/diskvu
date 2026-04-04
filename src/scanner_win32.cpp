// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "scanner_common.h"
#include "colorutils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <chrono>
#include <future>
#include <limits>
#include <thread>

namespace {

// Returns a Win32 extended-length path (\\?\) for long-path support.
// Requires a fully-qualified, QDir::cleanPath()-normalised input (no . or .. components).
std::wstring toWin32Path(const QString& qtPath)
{
    const QString native = QDir::toNativeSeparators(qtPath);
    if (native.startsWith(QLatin1String("\\\\?\\"))) {
        return native.toStdWString();
    }
    // UNC paths: \\server\share -> \\?\UNC\server\share
    if (native.startsWith(QLatin1String("\\\\"))) {
        return (QStringLiteral("\\\\?\\UNC\\") + native.mid(2)).toStdWString();
    }
    return (QStringLiteral("\\\\?\\") + native).toStdWString();
}

// Appends \* to a Win32 directory path for use with FindFirstFileExW.
std::wstring toSearchPattern(const std::wstring& win32Dir)
{
    std::wstring pat = win32Dir;
    if (!pat.empty() && pat.back() != L'\\') {
        pat += L'\\';
    }
    pat += L'*';
    return pat;
}

// Extracts file size from WIN32_FIND_DATAW without a separate stat call.
qint64 getFileSizeFromFindData(const WIN32_FIND_DATAW& fd)
{
    const ULONGLONG raw = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    return static_cast<qint64>(
        std::min<ULONGLONG>(raw, static_cast<ULONGLONG>(std::numeric_limits<qint64>::max())));
}

// Converts a Win32 error code to a human-readable message via FormatMessageW.
QString win32ErrorString(DWORD errorCode)
{
    if (errorCode == 0) {
        return {};
    }
    wchar_t* msgBuf = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&msgBuf), 0, nullptr);
    if (len == 0 || !msgBuf) {
        return QStringLiteral("Windows error %1").arg(errorCode);
    }
    const QString msg = QString::fromWCharArray(msgBuf, static_cast<int>(len)).trimmed();
    LocalFree(msgBuf);
    return msg;
}

void reportScanWarning(const Scanner::ErrorCallback& errorCallback,
                       const QString& path, DWORD errorCode)
{
    if (!errorCallback || errorCode == 0) {
        return;
    }
    errorCallback({path, win32ErrorString(errorCode)});
}

// Returns the volume serial number for the volume containing win32Path.
// Used as a filesystem boundary ID (Win32 equivalent of dev_t).
DWORD getVolumeSerial(const std::wstring& win32Path)
{
    wchar_t volRoot[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(win32Path.c_str(), volRoot, static_cast<DWORD>(std::size(volRoot)))) {
        return 0;
    }
    DWORD serial = 0;
    GetVolumeInformationW(volRoot, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0);
    return serial;
}

// Returns true for locally-attached volumes (not network shares).
bool isLocalWin32Volume(const QStorageInfo& vol)
{
    const QString fsType = vol.fileSystemType().toUpper();
    if (fsType == QLatin1String("CIFS") || fsType == QLatin1String("NFS") ||
        fsType == QLatin1String("SMBFS") || fsType == QLatin1String("NETFS")) {
        return false;
    }
    // UNC device paths indicate network volumes
    return !vol.device().startsWith("\\\\");
}

} // namespace

qint64 Scanner::scanNode(FileNode* node, const QString& path, const ScanResult& scanState,
                         const TreemapSettings& settings,
                         const std::vector<QString>& allExcludedPaths,
                         const ProgressReadyCallback& progressReadyCallback,
                         const ProgressCallback& progressCallback, NodeArena& arena,
                         const ActivityCallback& activityCallback,
                         const Scanner::ErrorCallback& errorCallback,
                         float branchHue, unsigned long long rootDev,
                         const std::atomic_bool* cancelFlag, int depth)
{
    if (depth > kMaxDepth || isCancelled(cancelFlag)) {
        return 0;
    }

    if (activityCallback) {
        activityCallback(path, scanState.root ? scanState.root->size : 0);
    }
    emitProgress(scanState, path, progressReadyCallback, progressCallback);
    qint64 totalSize = 0;

    const QString childPathPrefix = childPathPrefixForParent(path);
    const std::wstring win32Dir = toWin32Path(path);
    const std::wstring searchPat = toSearchPattern(win32Dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileExW(searchPat.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err != ERROR_ACCESS_DENIED && err != ERROR_FILE_NOT_FOUND) {
            reportScanWarning(errorCallback, path, err);
        }
        return 0;
    }

    do {
        if (isCancelled(cancelFlag)) {
            break;
        }

        // Skip . and ..
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
            continue;
        }

        // Skip symlinks and NTFS junctions / volume mount points.
        // On Windows, cross-volume mounts are always reparse points, so this
        // also covers the limitToSameFilesystem case for the common scenario.
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }

        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const QString childName = QString::fromWCharArray(fd.cFileName);

        FileNode* child = arena.alloc();
        child->name = childName;
        child->isDirectory = isDir;
        child->parent = node;

        if (isDir) {
            const QString childPath = childPathPrefix + childName;
            if (!shouldSkipPath(childPath, allExcludedPaths)) {
                if (activityCallback) {
                    activityCallback(childPath, scanState.root ? scanState.root->size : 0);
                }
                child->color = ColorUtils::folderColorForBranch(depth + 1, branchHue, settings).rgba();
                node->children.push_back(child);
                child->size = scanNode(child, childPath, scanState, settings, allExcludedPaths,
                                       progressReadyCallback, progressCallback, arena,
                                       activityCallback, errorCallback,
                                       branchHue, rootDev, cancelFlag, depth + 1);
                totalSize += child->size;
                if (depth <= 1 && !isCancelled(cancelFlag)) {
                    emitProgress(scanState, childPath, progressReadyCallback, progressCallback);
                }
            }
        } else {
            const qint64 fileSize = getFileSizeFromFindData(fd);
            const QString childPath = activityCallback || (depth <= 1 && progressCallback)
                ? childPathPrefix + childName
                : QString();
            if (activityCallback && !childPath.isEmpty()) {
                activityCallback(childPath, fileSize);
            }
            child->size = fileSize;
            child->extKey = ColorUtils::packFileExt(childName);
            child->color = ColorUtils::fileColorForName(childName, settings).rgba();
            node->children.push_back(child);
            totalSize += fileSize;
            if (depth <= 1 && !isCancelled(cancelFlag)) {
                emitProgress(scanState, childPath.isEmpty() ? path : childPath,
                             progressReadyCallback, progressCallback);
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
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

    // Initialise rootDev as the volume serial number of the scan root.
    // This is the Win32 equivalent of dev_t and is used for limitToSameFilesystem.
    // Note: reparse point skipping already handles the common cross-volume case.
    const unsigned long long initialRootDev = static_cast<unsigned long long>(
        getVolumeSerial(toWin32Path(normalizedPath)));

    std::vector<DirTask> dirTasks;
    std::vector<PartitionTask> partitionQueue;
    partitionQueue.push_back({root, normalizedPath, 0.0f, initialRootDev, 0});

    for (size_t partitionIndex = 0; partitionIndex < partitionQueue.size(); ++partitionIndex) {
        if (isCancelled(cancelFlag)) {
            break;
        }

        const PartitionTask partition = partitionQueue[partitionIndex];
        if (partition.depth > 0) {
            addSizeUpwards(partition.parent, -kProvisionalDirectoryPreviewSize);
        }
        const QString childPathPrefix = childPathPrefixForParent(partition.path);
        const std::wstring win32Dir = toWin32Path(partition.path);
        const std::wstring searchPat = toSearchPattern(win32Dir);

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileExW(searchPat.c_str(), FindExInfoBasic, &fd,
                                        FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (hFind == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err != ERROR_ACCESS_DENIED && err != ERROR_FILE_NOT_FOUND) {
                reportScanWarning(errorCallback, partition.path, err);
            }
            continue;
        }

        do {
            if (isCancelled(cancelFlag)) {
                break;
            }

            // Skip . and ..
            if (fd.cFileName[0] == L'.' &&
                (fd.cFileName[1] == L'\0' ||
                 (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
                continue;
            }

            // Skip symlinks and NTFS junctions / volume mount points.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue;
            }

            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const QString childName = QString::fromWCharArray(fd.cFileName);

            FileNode* child = result.arena->alloc();
            child->name = childName;
            child->isDirectory = isDir;
            child->parent = partition.parent;

            if (isDir) {
                const QString childPath = childPathPrefix + childName;
                if (!shouldSkipPath(childPath, allExcludedPaths)) {
                    const float branchHue = partition.depth == 0
                        ? ColorUtils::topLevelFolderBranchHue(childName, settings)
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
                const qint64 fileSize = getFileSizeFromFindData(fd);
                child->size = fileSize;
                child->extKey = ColorUtils::packFileExt(childName);
                child->color = ColorUtils::fileColorForName(childName, settings).rgba();
                partition.parent->children.push_back(child);
                addSizeUpwards(partition.parent, fileSize);
                if (trackByteActivity) {
                    liveBytesSeen->fetch_add(fileSize, std::memory_order_relaxed);
                    activityCallback(childPathPrefix + childName,
                                     liveBytesSeen->load(std::memory_order_relaxed));
                }
            }
        } while (FindNextFileW(hFind, &fd));

        FindClose(hFind);
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
                                  nullptr, *r.arena, workerActivityCallback, {},
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
    const auto isLocalDevice = [](const QStorageInfo& vol) {
        return isLocalWin32Volume(vol);
    };
    result.filesystems.push_back({primaryFsRoot, storageInfo.rootPath(),
                                   storageInfo.bytesFree(), storageInfo.bytesTotal(),
                                   isLocalDevice(storageInfo)});

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
        // Skip volumes sharing the same underlying device (e.g. ReFS subvolumes)
        if (seenDevices.contains(vol.device()))
            continue;
        seenDevices.insert(vol.device());
        result.freeBytes += vol.bytesFree();
        result.totalBytes += vol.bytesTotal();
        result.filesystems.push_back({volRoot, vol.rootPath(),
                                       vol.bytesFree(), vol.bytesTotal(),
                                       isLocalDevice(vol)});
    }

    emitProgress(result, path, progressReadyCallback, progressCallback, true);

    return result;
}

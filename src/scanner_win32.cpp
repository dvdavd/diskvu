// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "scanner_common.h"
#include "colorutils.h"
#include "filesystemutils.h"

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

// Converts a FILETIME (100-ns intervals since 1601-01-01) to Unix epoch seconds.
int64_t filetimeToUnixSeconds(const FILETIME& ft)
{
    const ULONGLONG intervals = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(intervals / 10000000ULL) - 11644473600LL;
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

} // namespace

unsigned long long Scanner::initialRootDevice(const QString& path)
{
    return static_cast<unsigned long long>(getVolumeSerial(toWin32Path(path)));
}

void Scanner::partitionNode(Scanner::PartitionTask partition,
                          const TreemapSettings& settings,
                          const std::vector<QString>& allExcludedPaths,
                          const ErrorCallback& errorCallback,
                          const std::shared_ptr<const std::atomic_bool>& cancelFlag,
                          std::vector<Scanner::PartitionTask>& partitionQueue,
                          std::vector<Scanner::DirTask>& dirTasks,

                          NodeArena& arena,
                          const ActivityCallback& activityCallback,
                          const std::shared_ptr<std::atomic<qint64>>& liveBytesSeen,
                          ScanThrottler& throttler,
                          int targetTaskCount,
                          int effectiveParallelPartitionDepth,
                          const std::shared_ptr<HardLinkTracker>& hardLinkTracker)
{
    (void)hardLinkTracker; // Hard link tracking not yet implemented on Win32 partition phase
    const QString childPathPrefix = childPathPrefixForParent(partition.path);
    const std::wstring win32Dir = toWin32Path(partition.path);
    const std::wstring searchPat = toSearchPattern(win32Dir);

    bool partitionThrottled = !isLocalFilesystemPath(partition.path);
    ThrottleGuard partitionThrottleGuard(&throttler.networkSemaphore, partitionThrottled);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileExW(searchPat.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err != ERROR_ACCESS_DENIED && err != ERROR_FILE_NOT_FOUND) {
            reportScanWarning(errorCallback, partition.path, err);
        }
        return;
    }

    FileNode* lastPartitionChild = nullptr;
    if (partition.parent->firstChild) {
        lastPartitionChild = partition.parent->firstChild;
        while (lastPartitionChild->nextSibling) lastPartitionChild = lastPartitionChild->nextSibling;
    }

    qint64 dirFilesSize = 0;
    int dirFilesCount = 0;

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

        if (isDir) {
            const QString childPath = childPathPrefix + childName;
            if (shouldSkipPath(childPath, allExcludedPaths)) {
                continue;
            }

            float childBranchHue = partition.branchHue;
            bool childInMarkedBranch = partition.inMarkedBranch;
            if (partition.depth == 0) {
                childBranchHue = ColorUtils::topLevelFolderBranchHue(childName, settings);
            }

            FileNode* child = arena.alloc();
            child->name = childName;
            child->setIsDirectory(true);
            child->mtime = filetimeToUnixSeconds(fd.ftLastWriteTime);
            child->parent = partition.parent;

            if (!settings.folderColorMarks.isEmpty()) {
                auto it = settings.folderColorMarks.constFind(childPath);
                if (it != settings.folderColorMarks.constEnd()) {
                    child->setColorMark(static_cast<uint8_t>(it.value()));
                    childBranchHue = ColorUtils::markHue(static_cast<FolderMark>(child->colorMark()));
                    childInMarkedBranch = true;
                }
            }
            if (!settings.folderIconMarks.isEmpty()) {
                auto it = settings.folderIconMarks.constFind(childPath);
                if (it != settings.folderIconMarks.constEnd()) {
                    child->setIconMark(static_cast<uint8_t>(it.value()));
                }
            }

            if (childInMarkedBranch) {
                child->color = ColorUtils::folderColorForMark(partition.depth + 1, childBranchHue, settings).rgba();
            } else {
                child->color = ColorUtils::folderColor(partition.depth + 1, childBranchHue, settings).rgba();
            }

            appendChild(partition.parent, child, &lastPartitionChild);

            const size_t pendingTasks = dirTasks.size() + partitionQueue.size(); // Approximation is fine
            if (partition.depth + 1 < effectiveParallelPartitionDepth || (pendingTasks < (size_t)targetTaskCount && partition.depth + 1 < kMaxDepth)) {
                partitionQueue.push_back({child, childPath, childBranchHue, partition.rootDev, partition.depth + 1, childInMarkedBranch});
            } else {
                dirTasks.push_back({child, childPath, childBranchHue, partition.rootDev, partition.depth + 1, childInMarkedBranch});
            }
            dirFilesCount += 1;
        } else {
            const qint64 fileSize = getFileSizeFromFindData(fd);
            FileNode* child = arena.alloc();
            child->name = childName;
            child->setIsDirectory(false);
            child->parent = partition.parent;
            child->size = fileSize;
            child->subtreeFileCount = 1;
            child->color = ColorUtils::fileColorForName(childName, settings).rgba();
            child->mtime = filetimeToUnixSeconds(fd.ftLastWriteTime);

            appendChild(partition.parent, child, &lastPartitionChild);

            dirFilesSize += fileSize;
            dirFilesCount += 1;

            if (liveBytesSeen) {
                liveBytesSeen->fetch_add(fileSize, std::memory_order_relaxed);
                static thread_local QElapsedTimer activityTimer;
                static thread_local bool activityTimerStarted = false;
                if (!activityTimerStarted) { activityTimer.start(); activityTimerStarted = true; }
                if (activityTimer.elapsed() >= kActivityIntervalMs) {
                    activityCallback(childPathPrefix + childName, liveBytesSeen->load(std::memory_order_relaxed));
                    activityTimer.restart();
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    if (dirFilesCount > 0) {
        addStatsUpwards(partition.parent, dirFilesSize, dirFilesCount);
    }
}

qint64 Scanner::scanNode(FileNode* node, const QString& path, const ScanResult& scanState,
                         const TreemapSettings& settings,
                         const std::vector<QString>& allExcludedPaths,
                         const ProgressReadyCallback& progressReadyCallback,
                         const ProgressCallback& progressCallback, NodeArena& arena,
                         const ActivityCallback& activityCallback,
                         const Scanner::ErrorCallback& errorCallback,
                         float branchHue, unsigned long long rootDev,
                         std::shared_ptr<const std::atomic_bool> cancelFlag, int depth,
                         bool parentInMarkedBranch,
                         ScanThrottler* throttler,
                         bool alreadyThrottled)
{
    if (depth > kMaxDepth || isCancelled(cancelFlag)) {
        return 0;
    }

    // Locality is classified at the scan root / task boundary.
    // Recursive descent inherits that throttling state instead of
    // re-checking every directory.
    ThrottleGuard throttleGuard(throttler ? &throttler->networkSemaphore : nullptr, false);
    const bool nextAlreadyThrottled = alreadyThrottled;

    if (activityCallback) {
        activityCallback(path, scanState.root ? scanState.root->size : 0);
    }
    emitProgress(scanState, path, progressReadyCallback, progressCallback);
    qint64 totalSize = 0;
    int totalFileCount = 0;

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

    FileNode* lastChild = nullptr;
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
        child->setIsDirectory(isDir);
        child->mtime = filetimeToUnixSeconds(fd.ftLastWriteTime);
        child->parent = node;

        if (isDir) {
            const QString childPath = childPathPrefix + childName;
            if (!shouldSkipPath(childPath, allExcludedPaths)) {
                if (activityCallback) {
                    static thread_local QElapsedTimer dirActivityTimer;
                    static thread_local bool dirActivityTimerStarted = false;
                    if (!dirActivityTimerStarted) {
                        dirActivityTimer.start();
                        dirActivityTimerStarted = true;
                    }
                    if (dirActivityTimer.elapsed() >= kActivityIntervalMs) {
                        activityCallback(childPath, scanState.root ? scanState.root->size : 0);
                        dirActivityTimer.restart();
                    }
                }

                float childBranchHue = branchHue;
                bool childInMarkedBranch = parentInMarkedBranch;
                if (depth == 0) {
                    childBranchHue = ColorUtils::topLevelFolderBranchHue(childName, settings);
                }

                if (!settings.folderColorMarks.isEmpty()) {
                    auto it = settings.folderColorMarks.constFind(childPath);
                    if (it != settings.folderColorMarks.constEnd()) {
                        child->setColorMark(static_cast<uint8_t>(it.value()));
                        childBranchHue = ColorUtils::markHue(static_cast<FolderMark>(child->colorMark()));
                        childInMarkedBranch = true;
                    }
                }
                if (!settings.folderIconMarks.isEmpty()) {
                    auto it = settings.folderIconMarks.constFind(childPath);
                    if (it != settings.folderIconMarks.constEnd()) {
                        child->setIconMark(static_cast<uint8_t>(it.value()));
                    }
                }

                if (childInMarkedBranch) {
                    child->color = ColorUtils::folderColorForMark(depth + 1, childBranchHue, settings).rgba();
                } else {
                    child->color = ColorUtils::folderColor(depth + 1, childBranchHue, settings).rgba();
                }

                if (!node->firstChild) {
                    node->firstChild = child;
                } else {
                    lastChild->nextSibling = child;
                }
                lastChild = child;

                child->size = scanNode(child, childPath, scanState, settings, allExcludedPaths,
                                       progressReadyCallback, progressCallback, arena,
                                       activityCallback, errorCallback,
                                       childBranchHue, rootDev, cancelFlag, depth + 1,
                                       childInMarkedBranch, throttler, nextAlreadyThrottled);

                totalSize += child->size;
                totalFileCount += child->subtreeFileCount;
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
            child->subtreeFileCount = 1;
            child->color = ColorUtils::fileColorForName(childName, settings).rgba();

            if (!node->firstChild) {
                node->firstChild = child;
            } else {
                lastChild->nextSibling = child;
            }
            lastChild = child;

            totalSize += fileSize;
            totalFileCount += 1;
            if (depth <= 1 && !isCancelled(cancelFlag)) {
                emitProgress(scanState, childPath.isEmpty() ? path : childPath,
                             progressReadyCallback, progressCallback);
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    node->size = totalSize;
    node->subtreeFileCount = totalFileCount;
    return totalSize;
}

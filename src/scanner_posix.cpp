// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "scanner_common.h"
#include "colorutils.h"
#include "filesystemutils.h"

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
#ifdef Q_OS_LINUX
#include <sys/sysmacros.h>
#endif
#include <mutex>
#include <unordered_set>

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

unsigned long long Scanner::initialRootDevice(const QString& path)
{
    struct stat startSt;
    if (stat(path.toLocal8Bit().constData(), &startSt) == 0) {
        return static_cast<unsigned long long>(startSt.st_dev);
    }
    return 0;
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
    const QByteArray pathBytes = QFile::encodeName(partition.path);
    const QString childPathPrefix = childPathPrefixForParent(partition.path);

    bool partitionThrottled = !throttler.isLocal(partition.rootDev, partition.path);
    ThrottleGuard partitionThrottleGuard(&throttler.networkSemaphore, partitionThrottled);

    DIR* dirp = openDirectoryWithRevalidate(pathBytes);
    if (!dirp) {
        reportScanWarning(errorCallback, partition.path, errno);
        return;
    }

    FileNode* lastPartitionChild = nullptr;
    if (partition.parent->firstChild) {
        lastPartitionChild = partition.parent->firstChild;
        while (lastPartitionChild->nextSibling) lastPartitionChild = lastPartitionChild->nextSibling;
    }

    const int dfd = dirfd(dirp);
    struct dirent* entry;
    qint64 dirFilesSize = 0;
    int dirFilesCount = 0;

    while (true) {
        entry = readdir(dirp);
        if (!entry) {
            break;
        }

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
        qint64 apparentFileSize = 0;
        int64_t fileMtime = 0;
        uint32_t nlink = 1;

        if (dtype == DT_UNKNOWN || isDir || !isDir) {
#ifdef Q_OS_LINUX
            struct statx stx;
            int statResult = statx(dfd, dname, AT_SYMLINK_NOFOLLOW, STATX_TYPE | STATX_SIZE | STATX_MTIME | STATX_INO | STATX_NLINK, &stx);
            if (statResult == 0) {
                if (S_ISLNK(stx.stx_mode)) {
                    continue;
                }
                isDir = S_ISDIR(stx.stx_mode);
                if (isDir) {
                    if (settings.limitToSameFilesystem && makedev(stx.stx_dev_major, stx.stx_dev_minor) != partition.rootDev)
                        continue;
                } else {
                    apparentFileSize = stx.stx_size;
                    fileSize = apparentFileSize;
                    if (stx.stx_nlink > 1
                            && !shouldCountHardLink(hardLinkTracker, makedev(stx.stx_dev_major, stx.stx_dev_minor), stx.stx_ino)) {
                        fileSize = 0;
                    }
                }
                fileMtime = static_cast<int64_t>(stx.stx_mtime.tv_sec);
                nlink = stx.stx_nlink;
            } else {
                reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
                continue;
            }
#else
            struct stat st;
            int statResult = fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW);

            if (statResult == 0) {
                if (S_ISLNK(st.st_mode)) {
                    continue;
                }
                isDir = S_ISDIR(st.st_mode);
                if (isDir) {
                    if (settings.limitToSameFilesystem && st.st_dev != partition.rootDev)
                        continue;
                } else {
                    apparentFileSize = st.st_size;
                    fileSize = apparentFileSize;
                    if (st.st_nlink > 1
                            && !shouldCountHardLink(hardLinkTracker, st.st_dev, st.st_ino)) {
                        fileSize = 0;
                    }
                }
                fileMtime = static_cast<int64_t>(st.st_mtime);
                nlink = st.st_nlink;
            } else {
                reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
                continue;
            }
#endif
        }

        const QString childName = QString::fromLocal8Bit(dname);

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
            child->mtime = fileMtime;
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
            // Directories are not counted here: subtreeFileCount counts files only,
            // and the child accounts for its own contents once it has been scanned.
        } else {
            FileNode* child = arena.alloc();
            child->name = childName;
            child->setIsDirectory(false);
            child->parent = partition.parent;
            child->size = fileSize;
            child->displaySize = apparentFileSize;
            child->setHasHardLinks(nlink > 1);
            child->subtreeFileCount = 1;
            child->mtime = fileMtime;
            child->color = ColorUtils::fileColorForName(childName, settings).rgba();

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
    }

    closedir(dirp);

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
                           const ErrorCallback& errorCallback,
                           float branchHue, unsigned long long rootDev, std::shared_ptr<const std::atomic_bool> cancelFlag, int depth,
                           bool inMarkedBranch,
                           ScanThrottler* throttler,
                           bool alreadyThrottled)
{
    if (depth > kMaxDepth || isCancelled(cancelFlag)) {
        return 0;
    }

    // Determine current device ID for throttling check.
    // If node is root, we might not have it yet.
    unsigned long long currentDev = rootDev;

    bool needsThrottle = false;
    if (throttler && !alreadyThrottled) {
        // Find current device ID if we are at the top level or crossed a boundary.
        struct stat nodeSt;
        if (stat(path.toLocal8Bit().constData(), &nodeSt) == 0) {
            currentDev = nodeSt.st_dev;
            if (!throttler->isLocal(currentDev, path)) {
                needsThrottle = true;
            }
        }
    }

    ThrottleGuard throttleGuard(throttler ? &throttler->networkSemaphore : nullptr, needsThrottle);
    bool nextAlreadyThrottled = alreadyThrottled || needsThrottle;

    FileNode* lastChild = nullptr;
    if (node->firstChild) {
        lastChild = node->firstChild;
        while (lastChild->nextSibling) lastChild = lastChild->nextSibling;
    }
    if (activityCallback) {
        static thread_local QElapsedTimer activityTimer;
        static thread_local bool activityTimerStarted = false;
        if (!activityTimerStarted) {
            activityTimer.start();
            activityTimerStarted = true;
        }
        if (activityTimer.elapsed() >= kActivityIntervalMs) {
            activityCallback(path, scanState.root ? scanState.root->size : 0);
            activityTimer.restart();
        }
    }

    qint64 totalSize = 0;
    int totalFileCount = 0;

    const QByteArray pathBytes = QFile::encodeName(path);
    const QString childPathPrefix = childPathPrefixForParent(path);
    DIR* dirp = openDirectoryWithRevalidate(pathBytes);
    if (!dirp) {
        reportScanWarning(errorCallback, path, errno);
        return 0;
    }

    const int dfd = dirfd(dirp);
    struct dirent* entry;
    while (true) {
        entry = readdir(dirp);
        if (!entry) {
            break;
        }

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
        qint64 apparentFileSize = 0;
        int64_t fileMtime = 0;
        uint32_t nlink = 1;

#ifdef Q_OS_LINUX
        struct statx stx;
        int statResult = statx(dfd, dname, AT_SYMLINK_NOFOLLOW, STATX_TYPE | STATX_SIZE | STATX_MTIME | STATX_INO | STATX_NLINK, &stx);

        if (statResult == 0) {
            if (S_ISLNK(stx.stx_mode))
                continue;
            isDir = S_ISDIR(stx.stx_mode);
            if (isDir) {
                if (settings.limitToSameFilesystem && makedev(stx.stx_dev_major, stx.stx_dev_minor) != rootDev)
                    continue;
            } else {
                apparentFileSize = stx.stx_size;
                fileSize = apparentFileSize;
                if (stx.stx_nlink > 1
                        && !shouldCountHardLink(scanState.hardLinkTracker, makedev(stx.stx_dev_major, stx.stx_dev_minor), stx.stx_ino)) {
                    fileSize = 0;
                }
            }
            fileMtime = static_cast<int64_t>(stx.stx_mtime.tv_sec);
            nlink = stx.stx_nlink;
        } else {
            reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
            continue;
        }
#else
        struct stat st;
        int statResult = fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW);

        if (statResult == 0) {
            if (S_ISLNK(st.st_mode))
                continue;
            isDir = S_ISDIR(st.st_mode);
            if (isDir) {
                if (settings.limitToSameFilesystem && st.st_dev != rootDev)
                    continue;
            } else {
                apparentFileSize = st.st_size;
                fileSize = apparentFileSize;
                if (st.st_nlink > 1
                        && !shouldCountHardLink(scanState.hardLinkTracker, st.st_dev, st.st_ino)) {
                    fileSize = 0;
                }
            }
            fileMtime = static_cast<int64_t>(st.st_mtime);
            nlink = st.st_nlink;
        } else {
            reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
            continue;
        }
#endif

        const QString childName = QString::fromLocal8Bit(dname);

        if (isDir) {
            const QString childPath = childPathPrefix + childName;
            if (shouldSkipPath(childPath, allExcludedPaths))
                continue;

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
            FileNode* child = arena.alloc();
            child->name = childName;
            child->setIsDirectory(true);
            child->mtime = fileMtime;
            child->parent = node;

            float childBranchHue = branchHue;
            bool childInMarkedBranch = inMarkedBranch;
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

            appendChild(node, child, &lastChild);

            child->size = scanNode(child, childPath, scanState, settings, allExcludedPaths,
                                   progressReadyCallback, progressCallback, arena, activityCallback,
                                   errorCallback, childBranchHue, rootDev, cancelFlag, depth + 1,
                                   childInMarkedBranch, throttler, nextAlreadyThrottled);
            totalSize += child->size;
            totalFileCount += child->subtreeFileCount;
            if (depth <= 1 && !isCancelled(cancelFlag)) {
                emitProgress(scanState, childPath, progressReadyCallback, progressCallback);
            }
        } else {
            FileNode* child = arena.alloc();
            child->name = childName;
            child->setIsDirectory(false);
            child->parent = node;
            child->size = fileSize;
            child->displaySize = apparentFileSize;
            child->setHasHardLinks(nlink > 1);
            child->mtime = fileMtime;
            child->color = ColorUtils::fileColorForName(childName, settings).rgba();
            child->subtreeFileCount = 1;

            appendChild(node, child, &lastChild);

            totalSize += fileSize;
            totalFileCount += 1;

            if (activityCallback) {
                static thread_local QElapsedTimer activityTimer;
                static thread_local bool activityTimerStarted = false;
                if (!activityTimerStarted) {
                    activityTimer.start();
                    activityTimerStarted = true;
                }
                if (activityTimer.elapsed() >= kActivityIntervalMs) {
                    const QString activityPath = (activityCallback || (depth <= 1 && progressCallback)) ? childPathPrefix + childName : QString();
                    activityCallback(activityPath, fileSize);
                    activityTimer.restart();
                }
            }
            if (depth <= 1 && !isCancelled(cancelFlag)) {
                emitProgress(scanState, childPathPrefix + childName, progressReadyCallback, progressCallback);
            }
        }
    }

    closedir(dirp);
    node->size = totalSize;
    node->subtreeFileCount = totalFileCount;
    return totalSize;
}


// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "remotescanner.h"
#include "scanner_common.h"
#include "colorutils.h"
#include "version.h"

#include "../agent/protocol.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>

#ifdef DISKVU_HAVE_ZSTD
#include <zstd.h>
#endif

#include <functional>

#include <cstring>
#include <unordered_map>

namespace {

constexpr int kMaxMessageBytes = 256 * 1024 * 1024;
constexpr qsizetype kMaxInnerBytes = 512LL * 1024 * 1024;

// Quotes `s` for a POSIX shell (ssh runs the remote command through the
// user's login shell).
QString shellQuote(const QString& s)
{
    QString out = QStringLiteral("'");
    out += QString(s).replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    out += QLatin1Char('\'');
    return out;
}

QString fromBytes(std::string_view sv)
{
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

struct NodeMeta {
    FileNode* node = nullptr;
    FileNode* last = nullptr; // tail of node's child list
    int depth = 0;
    float hue = 0.0f;
};

struct StackEntry {
    NodeMeta meta;
    qint64 pendingSize = 0;
    int pendingCount = 0;
};

// Rebuilds the tree from protocol messages.
class TreeBuilder {
public:
    TreeBuilder(ScanResult& result, const TreemapSettings& settings)
        : m_result(result)
        , m_settings(settings)
    {
    }

    bool handleHello(dva::Reader& r)
    {
        uint64_t version;
        std::string_view rootPath;
        if (!r.getVarint(version) || !r.getString(rootPath))
            return fail(QStringLiteral("Malformed hello message"));
        if (version > dva::kProtocolVersion)
            return fail(QStringLiteral("The diskvu-agent on the remote machine is newer than this "
                                       "diskvu (agent protocol %1, this diskvu understands up to %2). "
                                       "Update diskvu.")
                            .arg(version).arg(dva::kProtocolVersion));
        if (version < dva::kMinProtocolVersion)
            return fail(QStringLiteral("The diskvu-agent on the remote machine is too old "
                                       "(agent protocol %1, need at least %2). "
                                       "Rebuild and reinstall it.")
                            .arg(version).arg(dva::kMinProtocolVersion));
        m_agentProtocol = static_cast<uint32_t>(version);
        // v2+ hello carries the agent's build version and capability bits.
        if (!r.atEnd()) {
            std::string_view agentVersion;
            uint64_t caps;
            if (!r.getString(agentVersion) || !r.getVarint(caps))
                return fail(QStringLiteral("Malformed hello message"));
            m_agentVersion = fromBytes(agentVersion);
            m_agentCaps = caps;
        }

        FileNode* root = m_result.arena->alloc();
        root->name = fromBytes(rootPath);
        root->setIsDirectory(true);
        m_result.root = root;
        m_result.rootPath = root->name;

        NodeMeta meta;
        meta.node = root;
        meta.hue = ColorUtils::initialFolderBranchHue(root, m_settings);
        root->color = ColorUtils::folderColor(0, meta.hue, m_settings).rgba();
        m_nodes.push_back(meta);
        return true;
    }

    bool handleNodes(dva::Reader& r)
    {
        if (m_nodes.empty())
            return fail(QStringLiteral("Node data before hello"));
        while (!r.atEnd()) {
            uint64_t parentIndex, size, apparent;
            uint8_t flags;
            std::string_view name;
            int64_t mtime;
            if (!r.getVarint(parentIndex) || !r.getU8(flags) || !r.getString(name)
                || !r.getVarint(size) || !r.getVarint(apparent) || !r.getZigzag(mtime))
                return fail(QStringLiteral("Malformed node entry"));
            if (parentIndex >= m_nodes.size() || !m_nodes[parentIndex].node->isDirectory())
                return fail(QStringLiteral("Invalid parent index in node entry"));

            NodeMeta& parent = m_nodes[parentIndex];
            NodeMeta child = makeChild(parent, fromBytes(name), flags & dva::kNodeDir,
                                       flags, static_cast<qint64>(size),
                                       static_cast<qint64>(apparent), mtime);
            if (!(flags & dva::kNodeDir))
                addStatsUpwards(parent.node, static_cast<qint64>(size), 1);
            m_nodes.push_back(child); // may reallocate: `parent` is dead after this
        }
        return true;
    }

    bool handleSubtree(dva::Reader& r)
    {
        uint64_t taskIndex;
        uint8_t isFinal;
        if (!r.getVarint(taskIndex) || !r.getU8(isFinal))
            return fail(QStringLiteral("Malformed subtree header"));
        if (taskIndex >= m_nodes.size() || !m_nodes[taskIndex].node->isDirectory())
            return fail(QStringLiteral("Invalid task index in subtree message"));

        std::vector<StackEntry>& stack = m_stacks[taskIndex];
        if (stack.empty())
            stack.push_back({m_nodes[taskIndex], 0, 0});
        m_lastTask = m_nodes[taskIndex].node;

        while (!r.atEnd()) {
            uint8_t tag;
            if (!r.getU8(tag))
                return fail(QStringLiteral("Malformed subtree record"));

            if (tag == dva::kRecDir) {
                std::string_view name;
                int64_t mtime;
                if (!r.getString(name) || !r.getZigzag(mtime))
                    return fail(QStringLiteral("Malformed directory record"));
                NodeMeta child = makeChild(stack.back().meta, fromBytes(name), true, 0, 0, 0, mtime);
                stack.push_back({child, 0, 0});
            } else if (tag == dva::kRecFile) {
                uint8_t flags;
                std::string_view name;
                uint64_t size, apparent;
                int64_t mtime;
                if (!r.getU8(flags) || !r.getString(name) || !r.getVarint(size)
                    || !r.getVarint(apparent) || !r.getZigzag(mtime))
                    return fail(QStringLiteral("Malformed file record"));
                NodeMeta child = makeChild(stack.back().meta, fromBytes(name), false, flags,
                                           static_cast<qint64>(size), static_cast<qint64>(apparent), mtime);
                stack.back().pendingSize += static_cast<qint64>(size);
                stack.back().pendingCount += 1;
            } else if (tag == dva::kRecUp) {
                if (stack.size() < 2)
                    return fail(QStringLiteral("Unbalanced directory records"));
                StackEntry done = stack.back();
                stack.pop_back();
                done.meta.node->size += done.pendingSize;
                done.meta.node->subtreeFileCount += done.pendingCount;
                stack.back().pendingSize += done.pendingSize;
                stack.back().pendingCount += done.pendingCount;
            } else {
                return fail(QStringLiteral("Unknown subtree record tag %1").arg(tag));
            }
        }

        flushStack(stack);
        if (isFinal) {
            // Anything left open means the agent aborted mid-directory; the
            // partial sizes were already folded in by flushStack.
            m_stacks.erase(taskIndex);
        }
        return true;
    }

    bool handleWarning(dva::Reader& r, const Scanner::ErrorCallback& cb)
    {
        std::string_view path, message;
        if (!r.getString(path) || !r.getString(message))
            return fail(QStringLiteral("Malformed warning message"));
        if (cb)
            cb({fromBytes(path), fromBytes(message)});
        return true;
    }

    bool handleProgress(dva::Reader& r)
    {
        uint64_t files, bytes;
        std::string_view path;
        if (!r.getVarint(files) || !r.getVarint(bytes) || !r.getString(path))
            return fail(QStringLiteral("Malformed progress message"));
        m_progressBytes = static_cast<qint64>(bytes);
        if (path.size() > 0)
            m_progressPath = fromBytes(path);
        m_hasProgress = true;
        return true;
    }

    bool hasProgress() const { return m_hasProgress; }
    qint64 progressBytes() const { return m_progressBytes; }

    bool handleDone(dva::Reader& r)
    {
        uint64_t freeBytes, totalBytes;
        uint8_t reserved;
        if (!r.getVarint(freeBytes) || !r.getVarint(totalBytes) || !r.getU8(reserved))
            return fail(QStringLiteral("Malformed done message"));
        m_result.freeBytes = static_cast<qint64>(freeBytes);
        m_result.totalBytes = static_cast<qint64>(totalBytes);
        m_done = true;
        return true;
    }

    uint32_t agentProtocol() const { return m_agentProtocol; }
    const QString& agentVersion() const { return m_agentVersion; }
    uint64_t agentCaps() const { return m_agentCaps; }

    bool done() const { return m_done; }
    bool hasContent() const { return m_nodes.size() > 1; }
    const QString& error() const { return m_error; }
    bool fail(const QString& message)
    {
        if (m_error.isEmpty())
            m_error = message;
        return false;
    }
    void setError(const QString& message) { fail(message); }

    // Path of the directory the latest subtree message belonged to.
    QString currentPath() const
    {
        if (!m_progressPath.isEmpty())
            return m_progressPath;
        return m_lastTask ? m_lastTask->computePath() : m_result.rootPath;
    }

private:
    NodeMeta makeChild(NodeMeta& parent, const QString& name, bool isDir, uint8_t flags,
                       qint64 size, qint64 apparent, int64_t mtime)
    {
        FileNode* node = m_result.arena->alloc();
        node->name = name;
        node->mtime = mtime;
        node->setIsDirectory(isDir);

        NodeMeta meta;
        meta.node = node;
        meta.depth = parent.depth + 1;
        meta.hue = parent.hue;
        if (isDir) {
            if (parent.depth == 0)
                meta.hue = ColorUtils::topLevelFolderBranchHue(name, m_settings);
            node->color = ColorUtils::folderColor(meta.depth, meta.hue, m_settings).rgba();
        } else {
            node->size = size;
            node->displaySize = apparent;
            node->subtreeFileCount = 1;
            node->setHasHardLinks((flags & dva::kNodeHardLinks) != 0);
            node->color = ColorUtils::fileColorForName(name, m_settings).rgba();
        }

        appendChild(parent.node, node, &parent.last);
        return meta;
    }

    // Folds every open directory's unflushed totals into the tree so partial
    // progress is visible in previews.
    void flushStack(std::vector<StackEntry>& stack)
    {
        qint64 carrySize = 0;
        int carryCount = 0;
        for (size_t i = stack.size(); i-- > 0;) {
            StackEntry& e = stack[i];
            carrySize += e.pendingSize;
            carryCount += e.pendingCount;
            e.pendingSize = 0;
            e.pendingCount = 0;
            e.meta.node->size += carrySize;
            e.meta.node->subtreeFileCount += carryCount;
        }
        // Ancestors of the task root.
        addStatsUpwards(stack.front().meta.node->parent, carrySize, carryCount);
    }

    ScanResult& m_result;
    const TreemapSettings& m_settings;
    std::vector<NodeMeta> m_nodes;
    std::unordered_map<uint64_t, std::vector<StackEntry>> m_stacks;
    uint32_t m_agentProtocol = 0;
    QString m_agentVersion;
    uint64_t m_agentCaps = 0;
    FileNode* m_lastTask = nullptr;
    QString m_progressPath;
    qint64 m_progressBytes = 0;
    bool m_hasProgress = false;
    QString m_error;
    bool m_done = false;
};

} // namespace

RemoteScanner::Command RemoteScanner::sshInvocation(const QString& host,
                                                    const QString& remoteCommand, QString* error)
{
    const QString trimmedHost = host.trimmed();
    if (trimmedHost.isEmpty() || trimmedHost.startsWith(QLatin1Char('-'))
        || trimmedHost.contains(QLatin1Char(' '))) {
        if (error)
            *error = tr("Invalid remote host: %1").arg(host);
        return {};
    }

    Command cmd;
    cmd.program = QStringLiteral("ssh");
    // -T: no tty. BatchMode: never block on a password prompt the GUI cannot
    // show (use keys / ssh-agent). stdin must stay open: the agent treats EOF
    // as "client went away", so never pass -n.
    cmd.arguments = {QStringLiteral("-T"),
                     QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                     QStringLiteral("-o"), QStringLiteral("ConnectTimeout=15"),
                     QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=15"),
                     QStringLiteral("--"), trimmedHost, remoteCommand};
    return cmd;
}

RemoteScanner::Command RemoteScanner::sshCommand(const QString& host, const QString& remotePath,
                                                 const QString& agentCommand,
                                                 const TreemapSettings& settings, QString* error,
                                                 bool compress)
{
    QString remote = agentCommand.trimmed().isEmpty() ? QStringLiteral("diskvu-agent")
                                                      : agentCommand.trimmed();
    if (settings.limitToSameFilesystem)
        remote += QStringLiteral(" --one-file-system");
#ifdef DISKVU_HAVE_ZSTD
    if (compress)
        remote += QStringLiteral(" --compress");
#else
    Q_UNUSED(compress);
#endif
    for (const QString& excluded : settings.excludedPaths)
        remote += QStringLiteral(" --exclude ") + shellQuote(excluded);
    remote += QStringLiteral(" -- ") + shellQuote(remotePath.isEmpty() ? QStringLiteral("/") : remotePath);

    return sshInvocation(host, remote, error);
}

ScanResult RemoteScanner::scan(const Command& command, const TreemapSettings& settings,
                               Scanner::ProgressCallback progressCallback,
                               Scanner::ProgressReadyCallback progressReadyCallback,
                               Scanner::ActivityCallback activityCallback,
                               Scanner::ErrorCallback errorCallback,
                               std::shared_ptr<const std::atomic_bool> cancelFlag,
                               QString* errorMessage)
{
    ScanResult result;
    result.arena = std::make_shared<NodeArena>();
    TreeBuilder builder(result, settings);

    auto finishFailed = [&](const QString& message) {
        if (errorMessage)
            *errorMessage = message;
        result.root = nullptr;
        rebuildScanResultSnapshot(result);
        return result;
    };

    QProcess proc;
    proc.setProgram(command.program);
    proc.setArguments(command.arguments);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start();
    if (!proc.waitForStarted(10000))
        return finishFailed(tr("Could not start %1: %2").arg(command.program, proc.errorString()));

    QByteArray buffer;
    QByteArray stderrText;
    bool magicSeen = false;
    QElapsedTimer activityTimer;
    activityTimer.start();
    // The GUI keeps its preview "slot" closed until it has processed a snapshot
    // that was *forced* through progressReadyCallback (the local scanner forces
    // one after its skeleton pass). Without this, no live preview ever opens.
    bool forcedFirstSnapshot = false;

    // Non-fatal compatibility notices go to the scan warnings list.
    const bool compressionRequested =
        command.arguments.join(QLatin1Char(' ')).contains(QLatin1String("--compress"));
    auto reportAgentCompatibility = [&]() {
        if (!errorCallback)
            return;
        const QString who = QStringLiteral("diskvu-agent");
        if (builder.agentProtocol() < dva::kProtocolVersion) {
            errorCallback({who, tr("The diskvu-agent on the remote machine is out of date "
                                   "(no version information; this is diskvu %1). It still "
                                   "works, but newer features may be missing. Rebuild and "
                                   "reinstall it (scripts/build-agent-static.sh).")
                                    .arg(QStringLiteral(DISKVU_VERSION))});
            return;
        }
        if (compressionRequested && !(builder.agentCaps() & dva::kCapCompress)) {
            errorCallback({who, tr("diskvu-agent %1 was built without zstd support, so the "
                                   "transfer is not compressed.").arg(builder.agentVersion())});
        }
    };

    // Decompressed message bytes (from 'Z' segments) awaiting parsing.
    QByteArray inner;
#ifdef DISKVU_HAVE_ZSTD
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    const auto freeDctx = qScopeGuard([dctx]() { ZSTD_freeDCtx(dctx); });
#endif

    // Parses the complete messages in `buf` starting at `offset` and removes
    // the consumed bytes. `top` is true for the raw wire buffer, which may
    // carry 'Z' segments; a 'Z' is decoded and its messages are handled right
    // away so ordering relative to neighbouring plain messages is preserved.
    std::function<bool(QByteArray&, qsizetype, bool)> parseMessages =
        [&](QByteArray& buf, qsizetype offset, bool top) -> bool {
        while (offset < buf.size()) {
            dva::Reader header(buf.constData() + offset, static_cast<size_t>(buf.size() - offset));
            uint8_t type;
            uint64_t length;
            if (!header.getU8(type) || !header.getVarint(length))
                break; // header incomplete
            if (length > kMaxMessageBytes) {
                builder.setError(tr("Oversized message from remote agent"));
                return false;
            }
            const qsizetype headerSize = (buf.size() - offset) - static_cast<qsizetype>(header.remaining());
            if (static_cast<uint64_t>(header.remaining()) < length)
                break; // payload incomplete

            const char* payloadData = buf.constData() + offset + headerSize;
            dva::Reader payload(payloadData, static_cast<size_t>(length));
            bool ok = true;
            switch (static_cast<char>(type)) {
            case dva::kMsgHello:
                ok = builder.handleHello(payload);
                if (ok)
                    reportAgentCompatibility();
                break;
            case dva::kMsgNodes:   ok = builder.handleNodes(payload); break;
            case dva::kMsgSubtree: ok = builder.handleSubtree(payload); break;
            case dva::kMsgWarning: ok = builder.handleWarning(payload, errorCallback); break;
            case dva::kMsgProgress: ok = builder.handleProgress(payload); break;
            case dva::kMsgDone:    ok = builder.handleDone(payload); break;
            case dva::kMsgError: {
                std::string_view msg;
                payload.getString(msg);
                builder.setError(tr("Remote agent: %1").arg(fromBytes(msg)));
                ok = false;
                break;
            }
            case dva::kMsgCompressed: {
                if (!top)
                    break; // nested 'Z' is not part of the protocol; skip
#ifdef DISKVU_HAVE_ZSTD
                ZSTD_inBuffer in{payloadData, static_cast<size_t>(length), 0};
                char out[64 * 1024];
                ZSTD_outBuffer ob{out, sizeof(out), 0};
                do {
                    ob.pos = 0;
                    const size_t rc = ZSTD_decompressStream(dctx, &ob, &in);
                    if (ZSTD_isError(rc)) {
                        builder.setError(tr("Corrupt compressed data from remote agent: %1")
                                             .arg(QString::fromUtf8(ZSTD_getErrorName(rc))));
                        return false;
                    }
                    inner.append(out, static_cast<qsizetype>(ob.pos));
                    if (inner.size() > kMaxInnerBytes) {
                        builder.setError(tr("Oversized compressed data from remote agent"));
                        return false;
                    }
                } while (in.pos < in.size || ob.pos == sizeof(out));
                ok = parseMessages(inner, 0, false);
#else
                builder.setError(tr("The remote agent sent compressed data, but this build of "
                                    "diskvu has no zstd support."));
                ok = false;
#endif
                break;
            }
            default:
                break; // unknown message types are skipped for forward compatibility
            }
            if (!ok) {
                if (builder.error().isEmpty())
                    builder.setError(tr("Malformed data from remote agent"));
                return false;
            }
            offset += headerSize + static_cast<qsizetype>(length);
        }
        buf.remove(0, offset);
        return true;
    };

    // Parses as many complete messages as the wire buffer holds.
    auto drain = [&]() -> bool {
        qsizetype offset = 0;
        if (!magicSeen) {
            if (buffer.size() < 4)
                return true;
            if (std::memcmp(buffer.constData(), dva::kMagic, 4) != 0) {
                builder.setError(tr("Remote host did not speak the diskvu agent protocol. "
                                    "Is diskvu-agent installed and on the PATH there?"));
                return false;
            }
            magicSeen = true;
            offset = 4;
        }
        return parseMessages(buffer, offset, true);
    };

    bool failed = false;
    while (true) {
        if (isCancelled(cancelFlag)) {
            proc.kill();
            proc.waitForFinished(2000);
            return finishFailed(QString());
        }

        const bool gotData = proc.waitForReadyRead(100);
        buffer += proc.readAllStandardOutput();
        stderrText += proc.readAllStandardError();
        if (stderrText.size() > 16384)
            stderrText = stderrText.right(16384);

        if (!drain()) {
            failed = true;
            break;
        }

        if (result.root) {
            result.fileCount = result.root->subtreeFileCount;
            if (activityCallback && activityTimer.elapsed() >= kActivityIntervalMs) {
                activityTimer.restart();
                activityCallback(builder.currentPath(),
                                 builder.hasProgress() ? builder.progressBytes() : result.root->size);
            }
            const bool forceNow = !forcedFirstSnapshot && builder.hasContent();
            emitProgress(result, builder.currentPath(), progressReadyCallback, progressCallback, forceNow);
            if (forceNow)
                forcedFirstSnapshot = true;
        }

        if (builder.done())
            break;
        if (!gotData && proc.state() == QProcess::NotRunning) {
            buffer += proc.readAllStandardOutput();
            stderrText += proc.readAllStandardError();
            if (!drain()) {
                failed = true;
            } else if (!builder.done()) {
                QString detail = QString::fromLocal8Bit(stderrText).trimmed();
                if (detail.contains(QLatin1String("usage: diskvu-agent")))
                    detail = tr("The diskvu-agent on the remote machine does not understand this "
                                "version of diskvu (it is probably older). Rebuild and reinstall it.\n\n%1")
                                 .arg(detail);
                builder.setError(detail.isEmpty()
                                     ? tr("Remote scan ended unexpectedly (exit code %1)").arg(proc.exitCode())
                                     : detail);
                failed = true;
            }
            break;
        }
    }

    if (proc.state() != QProcess::NotRunning) {
        // Done: let the agent exit on its own; closing stdin makes it stop
        // if it is somehow still running.
        proc.closeWriteChannel();
        if (!proc.waitForFinished(3000)) {
            proc.kill();
            proc.waitForFinished(2000);
        }
    }

    if (failed || !builder.done() || !result.root) {
        QString message = builder.error();
        if (message.isEmpty())
            message = QString::fromLocal8Bit(stderrText).trimmed();
        return finishFailed(message);
    }

    recomputeNodeStats(result.root);
    recomputeApparentSizes(result.root);
    result.fileCount = result.root->subtreeFileCount;
    result.filesystems.push_back({result.rootPath, result.rootPath, result.freeBytes,
                                  result.totalBytes, /*isLocal=*/false});

    emitProgress(result, result.rootPath, progressReadyCallback, progressCallback, true);
    rebuildScanResultSnapshot(result);
    return result;
}

// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "scanner.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>

// Scans a directory tree on another machine by running `diskvu-agent` there
// (normally over ssh) and rebuilding the tree from its protocol stream
// (see agent/protocol.h). The filesystem walk happens next to the disk, so no
// per-file network round-trips are involved.
class RemoteScanner {
    Q_DECLARE_TR_FUNCTIONS(RemoteScanner)
public:
    struct Command {
        QString program;
        QStringList arguments;
    };

    // ssh invocation running `remoteCommand` (a shell command line, quoted by
    // the caller) on `host`, with the options every remote operation shares
    // (no tty, BatchMode, timeouts). Returns an empty program and sets *error
    // when `host` is not acceptable.
    static Command sshInvocation(const QString& host, const QString& remoteCommand,
                                 QString* error = nullptr);

    // Builds the ssh invocation that runs the agent on `host` for `remotePath`.
    // `host` is anything ssh accepts as a destination (user@host, ssh config
    // alias, ...). Returns an empty program and sets *error when `host` is
    // not acceptable. With `compress` the agent is asked to zstd-compress its
    // stream (ignored if diskvu was built without zstd, and by agents built
    // without it).
    static Command sshCommand(const QString& host, const QString& remotePath,
                              const QString& agentCommand, const TreemapSettings& settings,
                              QString* error = nullptr, bool compress = true);

    // Runs `command` and builds a ScanResult from its output. Mirrors
    // Scanner::scan: returns root == nullptr on cancel or failure, and on
    // failure stores a human-readable reason in *errorMessage.
    static ScanResult scan(const Command& command,
                           const TreemapSettings& settings = TreemapSettings::defaults(),
                           Scanner::ProgressCallback progressCallback = {},
                           Scanner::ProgressReadyCallback progressReadyCallback = {},
                           Scanner::ActivityCallback activityCallback = {},
                           Scanner::ErrorCallback errorCallback = {},
                           std::shared_ptr<const std::atomic_bool> cancelFlag = nullptr,
                           QString* errorMessage = nullptr);
};

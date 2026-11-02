// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>

// Installs diskvu-agent on a remote host over ssh: detects the platform with
// `uname -sm`, picks the matching prebuilt static binary shipped with diskvu
// (<prefix>/share/diskvu/agents/diskvu-agent-<os>-<arch>), streams it through
// the ssh stdin pipe into ~/.local/bin (so no scp/sftp or root is needed) and
// verifies it with `diskvu-agent --version`.
//
// install()/detectPlatform() block (they wait on ssh): run them off the GUI
// thread. Like scans they use BatchMode, so only key / ssh-agent logins work.
class RemoteAgentInstaller {
    Q_DECLARE_TR_FUNCTIONS(RemoteAgentInstaller)
public:
    struct Platform {
        QString os;   // lower-case kernel name, e.g. "linux"
        QString arch; // normalised: "x86_64", "aarch64", ...
        bool isValid() const { return !os.isEmpty() && !arch.isEmpty(); }
        QString display() const { return os + QLatin1Char('/') + arch; }
    };

    struct Result {
        bool ok = false;
        QString error;          // set when !ok
        Platform platform;
        QString agentCommand;   // value for RemoteProfile::agentCommand when ok
        QString agentVersion;   // as reported by `--version`
    };

    // Where the agent is installed on the remote host, relative to $HOME.
    static QString remoteAgentCommand();

    // Parses `uname -sm` output ("Linux x86_64\n"). Invalid platform on failure.
    static Platform parseUname(const QString& output);

    // File name of the bundled binary for `platform` (diskvu-agent-linux-x86_64),
    // or an empty string when diskvu does not ship one for it.
    static QString binaryName(const Platform& platform);

    // Directories searched for bundled agent binaries, most specific first.
    static QStringList searchDirs();

    // Full path of `name` in the first directory of `dirs` that has it, or "".
    static QString findBinary(const QString& name, const QStringList& dirs = searchDirs());

    // Parses `diskvu-agent --version` output ("diskvu-agent 0.9.28 (protocol 2)").
    static bool parseVersion(const QString& output, QString* version, int* protocol);

    // Asks the host for its platform. On failure returns an invalid platform
    // and sets *error.
    static Platform detectPlatform(const QString& host, QString* error);

    // Detects the platform, uploads the matching binary and verifies it.
    // `searchDirectories` is for tests; the default looks next to the app.
    static Result install(const QString& host,
                          const QStringList& searchDirectories = searchDirs());
};

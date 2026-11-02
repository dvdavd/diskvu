// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "remoteagentinstaller.h"
#include "remotescanner.h"

#include "../agent/protocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace {

constexpr int kDetectTimeoutMs = 30000;
constexpr int kUploadTimeoutMs = 180000;

struct ProcessOutput {
    bool ok = false;
    QString out;
    QString err; // stderr, or why the process could not run
};

// Runs `cmd`, optionally feeding `input` to its stdin, and waits for it.
ProcessOutput runSsh(const RemoteScanner::Command& cmd, const QByteArray& input, int timeoutMs)
{
    ProcessOutput result;
    QProcess proc;
    proc.setProgram(cmd.program);
    proc.setArguments(cmd.arguments);
    proc.setProcessChannelMode(QProcess::SeparateChannels);
    proc.start();
    if (!proc.waitForStarted(10000)) {
        result.err = proc.errorString();
        return result;
    }
    if (!input.isEmpty()) {
        proc.write(input);
        // Block until the pipe has drained so a slow link does not look hung
        // on the closeWriteChannel below.
        while (proc.bytesToWrite() > 0) {
            if (!proc.waitForBytesWritten(timeoutMs)) {
                proc.kill();
                proc.waitForFinished(2000);
                result.err = proc.errorString();
                return result;
            }
        }
    }
    proc.closeWriteChannel();
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(2000);
        result.err = QCoreApplication::translate("RemoteAgentInstaller", "ssh timed out");
        return result;
    }
    result.out = QString::fromUtf8(proc.readAllStandardOutput());
    result.err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
    result.ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    if (!result.ok && result.err.isEmpty())
        result.err = QCoreApplication::translate("RemoteAgentInstaller", "ssh exited with code %1")
                         .arg(proc.exitCode());
    return result;
}

} // namespace

QString RemoteAgentInstaller::remoteAgentCommand()
{
    // Expanded by the remote shell, so it keeps working if $HOME moves.
    return QStringLiteral("~/.local/bin/diskvu-agent");
}

RemoteAgentInstaller::Platform RemoteAgentInstaller::parseUname(const QString& output)
{
    const QStringList words = output.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() != 2)
        return {};
    Platform p;
    p.os = words.at(0).toLower();
    const QString arch = words.at(1).toLower();
    if (arch == QLatin1String("amd64") || arch == QLatin1String("x86_64"))
        p.arch = QStringLiteral("x86_64");
    else if (arch == QLatin1String("arm64") || arch == QLatin1String("aarch64"))
        p.arch = QStringLiteral("aarch64");
    else
        p.arch = arch;
    return p;
}

QString RemoteAgentInstaller::binaryName(const Platform& platform)
{
    if (!platform.isValid())
        return {};
    // Only platforms we build static agents for (scripts/build-agent-static.sh).
    if (platform.os != QLatin1String("linux"))
        return {};
    if (platform.arch != QLatin1String("x86_64") && platform.arch != QLatin1String("aarch64"))
        return {};
    return QStringLiteral("diskvu-agent-%1-%2").arg(platform.os, platform.arch);
}

QStringList RemoteAgentInstaller::searchDirs()
{
    const QString app = QCoreApplication::applicationDirPath();
    return {
        QDir::cleanPath(app + QStringLiteral("/agents")),                    // build tree / portable
        QDir::cleanPath(app + QStringLiteral("/../share/diskvu/agents")),    // installed prefix
    };
}

QString RemoteAgentInstaller::findBinary(const QString& name, const QStringList& dirs)
{
    if (name.isEmpty())
        return {};
    for (const QString& dir : dirs) {
        const QFileInfo info(QDir(dir).filePath(name));
        if (info.isFile() && info.isReadable())
            return info.absoluteFilePath();
    }
    return {};
}

bool RemoteAgentInstaller::parseVersion(const QString& output, QString* version, int* protocol)
{
    static const QRegularExpression re(
        QStringLiteral(R"(^diskvu-agent\s+(\S+)\s+\(protocol\s+(\d+)\))"),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = re.match(output);
    if (!m.hasMatch())
        return false;
    if (version)
        *version = m.captured(1);
    if (protocol)
        *protocol = m.captured(2).toInt();
    return true;
}

RemoteAgentInstaller::Platform RemoteAgentInstaller::detectPlatform(const QString& host,
                                                                    QString* error)
{
    QString cmdError;
    const auto cmd = RemoteScanner::sshInvocation(host, QStringLiteral("uname -sm"), &cmdError);
    if (cmd.program.isEmpty()) {
        if (error)
            *error = cmdError;
        return {};
    }
    const ProcessOutput run = runSsh(cmd, {}, kDetectTimeoutMs);
    if (!run.ok) {
        if (error)
            *error = tr("Could not reach %1: %2").arg(host, run.err);
        return {};
    }
    const Platform platform = parseUname(run.out);
    if (!platform.isValid() && error)
        *error = tr("Unexpected reply from %1: %2").arg(host, run.out.trimmed());
    return platform;
}

RemoteAgentInstaller::Result RemoteAgentInstaller::install(const QString& host,
                                                           const QStringList& searchDirectories)
{
    Result result;

    result.platform = detectPlatform(host, &result.error);
    if (!result.platform.isValid())
        return result;

    const QString name = binaryName(result.platform);
    if (name.isEmpty()) {
        result.error = tr("No diskvu-agent is available for %1 (supported: linux/x86_64, "
                          "linux/aarch64).").arg(result.platform.display());
        return result;
    }
    const QString binaryPath = findBinary(name, searchDirectories);
    if (binaryPath.isEmpty()) {
        result.error = tr("The agent binary %1 was not found. Looked in: %2. Build it with "
                          "scripts/build-agent-static.sh.")
                           .arg(name, searchDirectories.join(QStringLiteral(", ")));
        return result;
    }
    QFile file(binaryPath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = tr("Could not read %1: %2").arg(binaryPath, file.errorString());
        return result;
    }
    const QByteArray payload = file.readAll();
    if (payload.isEmpty()) {
        result.error = tr("%1 is empty.").arg(binaryPath);
        return result;
    }

    // Write beside the final name, then rename: replaces an agent that is
    // currently running (a scan in progress) without "text file busy".
    const QString uploadScript = QStringLiteral(
        "set -e; d=\"$HOME/.local/bin\"; mkdir -p \"$d\"; "
        "cat > \"$d/diskvu-agent.new\"; chmod 755 \"$d/diskvu-agent.new\"; "
        "mv -f \"$d/diskvu-agent.new\" \"$d/diskvu-agent\"");
    const auto upload = RemoteScanner::sshInvocation(host, uploadScript, &result.error);
    if (upload.program.isEmpty())
        return result;
    const ProcessOutput up = runSsh(upload, payload, kUploadTimeoutMs);
    if (!up.ok) {
        result.error = tr("Upload to %1 failed: %2").arg(host, up.err);
        return result;
    }

    const auto verify = RemoteScanner::sshInvocation(
        host, remoteAgentCommand() + QStringLiteral(" --version"), &result.error);
    const ProcessOutput ver = runSsh(verify, {}, kDetectTimeoutMs);
    int protocol = 0;
    if (!ver.ok || !parseVersion(ver.out, &result.agentVersion, &protocol)) {
        result.error = tr("The agent was copied to %1 but does not run there: %2")
                           .arg(host, ver.ok ? ver.out.trimmed() : ver.err);
        return result;
    }
    if (protocol < static_cast<int>(dva::kMinProtocolVersion)
        || protocol > static_cast<int>(dva::kProtocolVersion)) {
        result.error = tr("Installed diskvu-agent %1 speaks protocol %2, which this diskvu "
                          "does not support.").arg(result.agentVersion).arg(protocol);
        return result;
    }

    result.agentCommand = remoteAgentCommand();
    result.ok = true;
    return result;
}

// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "remotehostdialog.h"
#include "remoteagentinstaller.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QtConcurrent>
#include <QVBoxLayout>

RemoteHostDialog::RemoteHostDialog(const RemoteProfile& initial, const TreemapSettings& globalSettings,
                                   Mode mode, QWidget* parent)
    : QDialog(parent)
    , m_global(globalSettings)
    , m_customExcludes(initial.excludedPaths)
{
    setWindowTitle(mode == Mode::Edit ? tr("Edit Remote Host")
                   : mode == Mode::Add ? tr("Add Remote Host")
                                       : tr("Scan Remote Host"));
    setMinimumWidth(480);

    m_name = new QLineEdit(initial.name, this);
    m_name->setPlaceholderText(tr("Optional; defaults to the host"));
    m_host = new QLineEdit(initial.host, this);
    m_host->setPlaceholderText(tr("user@host or an ssh config alias"));
    m_path = new QLineEdit(initial.path, this);
    m_agent = new QLineEdit(initial.agentCommand, this);

    m_compress = new QCheckBox(tr("Compress transfer (zstd)"), this);
    m_compress->setChecked(initial.compress);
#ifndef DISKVU_HAVE_ZSTD
    m_compress->setChecked(false);
    m_compress->setEnabled(false);
    m_compress->setToolTip(tr("This build of diskvu has no zstd support"));
#endif

    auto* form = new QFormLayout;
    form->addRow(tr("Name:"), m_name);
    form->addRow(tr("Host:"), m_host);
    form->addRow(tr("Directory:"), m_path);
    m_installAgent = new QPushButton(tr("Install\u2026"), this);
    m_installAgent->setToolTip(tr("Copy the diskvu-agent bundled with diskvu to ~/.local/bin on "
                                  "this host over ssh"));
    connect(m_installAgent, &QPushButton::clicked, this, &RemoteHostDialog::installAgent);
    auto* agentRow = new QHBoxLayout;
    agentRow->addWidget(m_agent, 1);
    agentRow->addWidget(m_installAgent);
    form->addRow(tr("Agent command:"), agentRow);
    form->addRow(QString(), m_compress);

    // --- Per-host scan options ------------------------------------------
    auto* optionsBox = new QGroupBox(tr("Scan options"), this);
    auto* optionsLayout = new QVBoxLayout(optionsBox);

    m_useGlobalExcludes = new QCheckBox(tr("Use global excluded paths"), optionsBox);
    m_useGlobalExcludes->setChecked(initial.useGlobalExcludes);
    m_useGlobalExcludes->setToolTip(
        tr("Untick to use a separate list of excluded paths for this host. "
           "Paths are on the remote machine."));
    optionsLayout->addWidget(m_useGlobalExcludes);

    m_excludeList = new QListWidget(optionsBox);
    m_excludeList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_excludeList->setMinimumHeight(90);
    optionsLayout->addWidget(m_excludeList, 1);

    auto* addRow = new QHBoxLayout;
    m_excludeEdit = new QLineEdit(optionsBox);
    m_excludeEdit->setPlaceholderText(tr("Absolute path on the remote host, e.g. /proc"));
    m_excludeAdd = new QPushButton(tr("Add"), optionsBox);
    m_excludeRemove = new QPushButton(tr("Remove selected"), optionsBox);
    addRow->addWidget(m_excludeEdit, 1);
    addRow->addWidget(m_excludeAdd);
    addRow->addWidget(m_excludeRemove);
    optionsLayout->addLayout(addRow);

    auto* fsRow = new QHBoxLayout;
    fsRow->addWidget(new QLabel(tr("Stay on one filesystem:"), optionsBox));
    m_sameFs = new QComboBox(optionsBox);
    m_sameFs->addItem(tr("Use global setting (%1)")
                          .arg(m_global.limitToSameFilesystem ? tr("on") : tr("off")), 0);
    m_sameFs->addItem(tr("Always"), 1);
    m_sameFs->addItem(tr("Never"), 2);
    m_sameFs->setCurrentIndex(initial.useGlobalSameFilesystem
                                  ? 0 : (initial.limitToSameFilesystem ? 1 : 2));
    fsRow->addWidget(m_sameFs, 1);
    optionsLayout->addLayout(fsRow);

    connect(m_useGlobalExcludes, &QCheckBox::toggled, this, [this](bool useGlobal) {
        // Swap the list contents so the custom list survives toggling.
        if (useGlobal)
            m_customExcludes = listPaths();
        syncExcludeControls();
    });
    connect(m_excludeAdd, &QPushButton::clicked, this, &RemoteHostDialog::addExcludedPath);
    connect(m_excludeEdit, &QLineEdit::returnPressed, this, &RemoteHostDialog::addExcludedPath);
    connect(m_excludeRemove, &QPushButton::clicked, this, &RemoteHostDialog::removeSelectedExcludedPaths);
    syncExcludeControls();

    auto* hint = new QLabel(
        tr("Connects with ssh (key or ssh-agent login; password prompts are not supported) "
           "and runs diskvu-agent on the remote machine. Use Install\u2026 to copy the agent "
           "there, or give the path of an existing one above."), this);
    hint->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton* primary = nullptr;
    if (mode == Mode::ScanOnly) {
        primary = buttons->addButton(tr("Scan"), QDialogButtonBox::AcceptRole);
        connect(primary, &QPushButton::clicked, this, [this] { finish(false, true); });
    } else {
        primary = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
        connect(primary, &QPushButton::clicked, this, [this] { finish(true, false); });
    }
    primary->setDefault(mode == Mode::Edit);
    if (mode != Mode::Edit) {
        auto* saveScanButton = buttons->addButton(tr("Save && Scan"), QDialogButtonBox::AcceptRole);
        saveScanButton->setDefault(true);
        connect(saveScanButton, &QPushButton::clicked, this, [this] { finish(true, true); });
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(optionsBox, 1);
    layout->addWidget(hint);
    layout->addWidget(buttons);
}

QStringList RemoteHostDialog::listPaths() const
{
    // Only meaningful while the custom list is displayed.
    if (m_useGlobalExcludes->isChecked())
        return m_customExcludes;
    QStringList out;
    for (int i = 0; i < m_excludeList->count(); ++i)
        out.push_back(m_excludeList->item(i)->text());
    return out;
}

void RemoteHostDialog::syncExcludeControls()
{
    const bool useGlobal = m_useGlobalExcludes->isChecked();
    m_excludeList->clear();
    m_excludeList->addItems(useGlobal ? m_global.excludedPaths : m_customExcludes);
    m_excludeList->setEnabled(!useGlobal);
    m_excludeEdit->setEnabled(!useGlobal);
    m_excludeAdd->setEnabled(!useGlobal);
    m_excludeRemove->setEnabled(!useGlobal);
}

void RemoteHostDialog::addExcludedPath()
{
    const QString path = QDir::cleanPath(m_excludeEdit->text().trimmed());
    if (path.isEmpty() || path == QLatin1String(".") || !path.startsWith(QLatin1Char('/')))
        return;
    if (m_excludeList->findItems(path, Qt::MatchExactly).isEmpty())
        m_excludeList->addItem(path);
    m_excludeEdit->clear();
}

void RemoteHostDialog::removeSelectedExcludedPaths()
{
    qDeleteAll(m_excludeList->selectedItems());
}

RemoteProfile RemoteHostDialog::profile() const
{
    RemoteProfile p;
    p.name = m_name->text().trimmed();
    p.host = m_host->text().trimmed();
    p.path = m_path->text().trimmed();
    if (p.path.isEmpty())
        p.path = QStringLiteral("/");
    p.agentCommand = m_agent->text().trimmed();
    if (p.agentCommand.isEmpty())
        p.agentCommand = QStringLiteral("diskvu-agent");
    p.compress = m_compress->isChecked();
    p.useGlobalExcludes = m_useGlobalExcludes->isChecked();
    p.excludedPaths = listPaths();
    const int fsMode = m_sameFs->currentData().toInt();
    p.useGlobalSameFilesystem = fsMode == 0;
    p.limitToSameFilesystem = fsMode == 1;
    return p;
}

bool RemoteHostDialog::validate()
{
    const RemoteProfile p = profile();
    if (p.host.isEmpty() || p.host.startsWith(QLatin1Char('-')) || p.host.contains(QLatin1Char(' '))) {
        m_host->setFocus();
        return false;
    }
    if (!p.path.startsWith(QLatin1Char('/'))) {
        m_path->setFocus();
        return false;
    }
    return true;
}

void RemoteHostDialog::finish(bool save, bool scan)
{
    // Keep the dialog open when a required field is invalid.
    if (!validate())
        return;
    m_saveRequested = save;
    m_scanRequested = scan;

    // Remember the agent command / compression as defaults for the next new host.
    QSettings settings;
    settings.setValue(QStringLiteral("remote/agentCommand"), m_agent->text().trimmed());
#ifdef DISKVU_HAVE_ZSTD
    settings.setValue(QStringLiteral("remote/compress"), m_compress->isChecked());
#endif
    accept();
}

void RemoteHostDialog::installAgent()
{
    const QString host = m_host->text().trimmed();
    if (host.isEmpty() || host.startsWith(QLatin1Char('-')) || host.contains(QLatin1Char(' '))) {
        m_host->setFocus();
        return;
    }

    m_installAgent->setEnabled(false);
    m_installAgent->setText(tr("Checking\u2026"));
    auto restore = [this] {
        m_installAgent->setEnabled(true);
        m_installAgent->setText(tr("Install\u2026"));
    };

    // Stage 1: ask the host what it is (blocks on ssh, so off the GUI thread).
    auto* detect = new QFutureWatcher<QPair<RemoteAgentInstaller::Platform, QString>>(this);
    connect(detect, &QFutureWatcherBase::finished, this, [=] {
        const auto [platform, error] = detect->result();
        detect->deleteLater();
        if (!platform.isValid()) {
            restore();
            QMessageBox::warning(this, tr("Install Agent"), error);
            return;
        }
        const QString name = RemoteAgentInstaller::binaryName(platform);
        const QString localPath = RemoteAgentInstaller::findBinary(name);
        if (localPath.isEmpty()) {
            restore();
            QMessageBox::warning(
                this, tr("Install Agent"),
                name.isEmpty()
                    ? tr("No diskvu-agent is available for %1.").arg(platform.display())
                    : tr("This diskvu does not include %1. Build it with "
                         "scripts/build-agent-static.sh and place it in:\n%2")
                          .arg(name, RemoteAgentInstaller::searchDirs().first()));
            return;
        }
        const auto answer = QMessageBox::question(
            this, tr("Install Agent"),
            tr("Install diskvu-agent on %1 (%2)?\n\nIt is copied to %3 on that machine, "
               "replacing any agent already there.")
                .arg(host, platform.display(), RemoteAgentInstaller::remoteAgentCommand()));
        if (answer != QMessageBox::Yes) {
            restore();
            return;
        }

        // Stage 2: upload and verify.
        m_installAgent->setText(tr("Installing\u2026"));
        auto* install = new QFutureWatcher<RemoteAgentInstaller::Result>(this);
        connect(install, &QFutureWatcherBase::finished, this, [=] {
            const RemoteAgentInstaller::Result result = install->result();
            install->deleteLater();
            restore();
            if (!result.ok) {
                QMessageBox::warning(this, tr("Install Agent"), result.error);
                return;
            }
            m_agent->setText(result.agentCommand);
            QMessageBox::information(
                this, tr("Install Agent"),
                tr("Installed diskvu-agent %1 on %2.").arg(result.agentVersion, host));
        });
        install->setFuture(QtConcurrent::run([host] { return RemoteAgentInstaller::install(host); }));
    });
    detect->setFuture(QtConcurrent::run([host] {
        QString error;
        const auto platform = RemoteAgentInstaller::detectPlatform(host, &error);
        return qMakePair(platform, error);
    }));
}

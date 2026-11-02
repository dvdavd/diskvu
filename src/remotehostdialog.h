// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "remoteprofile.h"
#include "treemapsettings.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// Add / edit a remote host: ssh destination, directory, agent command and
// per-host scan options (excluded paths and same-filesystem limit can either
// follow the global settings or be overridden). The buttons depend on the mode:
//   ScanOnly (Scan Remote Host menu): Scan | Save & Scan | Cancel
//   Add (landing page):               Save | Save & Scan | Cancel
//   Edit (saved profile):             Save | Cancel
class RemoteHostDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { ScanOnly, Add, Edit };

    RemoteHostDialog(const RemoteProfile& initial, const TreemapSettings& globalSettings,
                     Mode mode, QWidget* parent = nullptr);

    RemoteProfile profile() const;
    bool scanRequested() const { return m_scanRequested; }
    bool saveRequested() const { return m_saveRequested; }

private:
    bool validate();
    void installAgent();
    void finish(bool save, bool scan);
    void syncExcludeControls();
    void addExcludedPath();
    void removeSelectedExcludedPaths();
    QStringList listPaths() const;

    TreemapSettings m_global;
    QStringList m_customExcludes; // override list; kept while "use global" is ticked

    QLineEdit* m_name = nullptr;
    QLineEdit* m_host = nullptr;
    QLineEdit* m_path = nullptr;
    QLineEdit* m_agent = nullptr;
    QPushButton* m_installAgent = nullptr;
    QCheckBox* m_compress = nullptr;
    QCheckBox* m_useGlobalExcludes = nullptr;
    QListWidget* m_excludeList = nullptr;
    QLineEdit* m_excludeEdit = nullptr;
    QPushButton* m_excludeAdd = nullptr;
    QPushButton* m_excludeRemove = nullptr;
    QComboBox* m_sameFs = nullptr;
    bool m_scanRequested = false;
    bool m_saveRequested = false;
};

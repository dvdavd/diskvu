// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"
#include "treemapsettings.h"

#include <QDialog>
#include <memory>

class NodePropertiesDialog : public QDialog {
    Q_OBJECT

public:
    explicit NodePropertiesDialog(const FileNode* node, const FileNode* scanRoot,
                                  std::shared_ptr<NodeArena> arena,
                                  const TreemapSettings& settings,
                                  QWidget* parent = nullptr);
};

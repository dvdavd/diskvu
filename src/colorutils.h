// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"
#include "treemapsettings.h"
#include <QColor>
#include <QList>
#include <QString>
#include <QStringView>

namespace ColorUtils {

struct DepthGradientPreset {
    QString name;
    QList<QColor> stops;
};

QList<DepthGradientPreset> depthGradientPresets();
QColor sampleGradient(const QList<QColor>& stops, float t);

// Shared color heuristics used both while scanning and when recoloring a tree
// after settings/theme changes. Keeping them in one place avoids subtle drift.
float normalizedHue(float hue);
float hueFromColor(const QColor& color, float fallbackHue);
float topLevelFolderBranchHue(const QString& name, const TreemapSettings& settings);
float initialFolderBranchHue(const FileNode* root, const TreemapSettings& settings);
QColor folderColorForBranch(int depth, float branchHue, const TreemapSettings& settings);
QColor folderColor(int depth, float branchHue, const TreemapSettings& settings);
QColor fileColorForName(const QString& name, const TreemapSettings& settings);
void assignColors(FileNode* root, const TreemapSettings& settings);
void assignColors(FileNode* root, FileNode* liveRoot, const TreemapSettings& settings);
void assignColorsForSubtree(FileNode* node, int depth, float branchHue, const TreemapSettings& settings);
uint64_t packFileExt(const QString& name);
QString fileTypeLabelForName(const QString& name);
QString fileTypeLabelForNode(const FileNode* node);

} // namespace ColorUtils

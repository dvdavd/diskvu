// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"
#include "mainwindow_types.h"
#include "treemapwidget.h"

#include <QApplication>
#include <QColor>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QList>
#include <QPointF>
#include <QSettings>
#include <QStringList>
#include <QStyle>
#include <QTreeWidget>
#include <functional>
#include <vector>

struct FileTypeSummary {
    QString label;
    QColor color;
    qint64 totalSize = 0;
    int count = 0;
};

struct BreadcrumbPathSegment {
    QString label;
    QString path;
};

QFont generalUiFont();
void applyMenuFontPolicy(QApplication& app);
// Keeps the QApplication palette aligned with the active platform style after
// startup, while allowing a one-time dark fallback if Qt starts light.
bool syncApplicationPaletteToColorScheme(QApplication& app, bool darkMode);
// Reports whether the currently rendered widget chrome is dark, regardless of
// whether that came from the platform theme or the startup fallback palette.
bool widgetChromeUsesDarkColorScheme();
QColor landingLocationBorderColor();
QIcon menuActionIcon(std::initializer_list<const char*> names,
                     const QString& lightResource,
                     const QString& darkResource,
                     QStyle::StandardPixmap fallback);
inline constexpr int kLandingTileSpacing = 10;
int landingTileWidth();
QString landingLocationStyleSheet();
QString normalizedFilesystemPath(const QString& path);
QList<BreadcrumbPathSegment> breadcrumbPathSegments(const QString& path);
bool systemUsesDarkColorScheme();
void dumpThemeState(const char* location, const QApplication& app);
QSettings appSettings();
void saveSettingsAsync(std::function<void(QSettings&)> fn);
QIcon makeColorSwatchIcon(const QColor& color);
QIcon makeTintedFolderIcon(const QColor& color);
QIcon makeRecoloredSvgIcon(const QString& svgPath, const QColor& color);
QIcon toolbarIcon(std::initializer_list<const char*> names, const QString& resource);
void clearIconCaches();
QList<FileTypeSummary> collectAndSortFileSummaries(FileNode* root,
                                                   const std::vector<bool>& searchReach = {},
                                                   std::shared_ptr<SearchIndex> searchIndex = {});
void populateTypeLegendItems(QTreeWidget* tree, QLabel* summaryLabel,
                             TreemapWidget* treemapWidget,
                             const QList<FileTypeSummary>& ordered);
bool sameViewState(const TreemapWidget::ViewState& a, const TreemapWidget::ViewState& b);
bool isOverviewState(const TreemapWidget::ViewState& state);
FileNode* findNodeByPath(FileNode* node, const QString& targetPath);
FileNode* findVirtualFreeSpaceNode(FileNode* root);
bool pathIsWithinRoot(const QString& path, const QString& rootPath);
void collectWatchDirectoryPaths(FileNode* root, FileNode* current, QStringList& paths);
QString nearestExistingNodePath(FileNode* root, QString path);
ViewStatePaths captureViewStatePaths(const TreemapWidget::ViewState& state);
TreemapWidget::ViewState remapViewStatePaths(const ViewStatePaths& original, FileNode* root);
QString nearestExistingDirectoryOnDisk(QString path);
FileNode* topLevelRefreshNode(FileNode* scanRoot, FileNode* currentNode);
QString searchPatternPlaceholderText();
QString formatPinnedDataSize(qint64 bytes);
QStringList mountedDevicePaths();
void sortChildrenBySizeRecursive(FileNode* node);
void applyFreeSpaceNodeColor(FileNode* root, const TreemapSettings& settings);
int countFilesRecursive(const FileNode* node);
int nodeDepth(const FileNode* node);
std::vector<FileNode*> prepareRootResultForDisplay(ScanResult& scanResult, const QString& currentPath,
                                                   bool showFreeSpaceInOverview,
                                                   const TreemapSettings& settings,
                                                   bool assumeSorted = false);
std::vector<FileNode*> reinjectFreeSpaceNodes(ScanResult& scanResult, const QString& currentPath,
                                              bool showFreeSpaceInOverview,
                                              const TreemapSettings& settings);
std::vector<TreemapWidget::ViewState> remapHistoryPaths(const std::vector<ViewStatePaths>& historyPaths,
                                                        FileNode* root);
bool sameSubtree(const FileNode* a, const FileNode* b);
TreemapWidget::ViewState remapViewStateByPath(const TreemapWidget::ViewState& original, FileNode* root);
void sanitizeHistoryForRoot(std::vector<TreemapWidget::ViewState>& history, FileNode* root);
qint64 pruneDeletedChildren(FileNode* node);
qint64 directorySizeOnDisk(const QString& path);
// Splices a refreshed subtree into an existing ScanResult in-place.
// Finds targetPath inside main.root, replaces it with refreshed.root,
// propagates the size delta upward, and merges the arenas.
// Returns false if targetPath is not found or has no parent (i.e. is the root).
bool spliceRefreshedSubtree(ScanResult& main, const QString& targetPath, ScanResult refreshed);

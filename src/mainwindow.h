// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "mainwindow_types.h"
#include "treemapwidget.h"
#include "treemapsettings.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QMutex>
#include <QPointF>
#include <QSet>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

class BreadcrumbPathBar;
class FilesystemWatchController;
class QComboBox;
class QAction;
class QCloseEvent;
class QLineEdit;
class QLabel;
class QMenu;
class QResizeEvent;
class QSplitter;
class QToolButton;
class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    void openInitialPath(const QString& path);

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    QMenu* createPopupMenu() override;
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void openDirectory();
    void refreshCurrentScan();
    void onRefreshActionTriggered();
    void cancelRefreshOperation();
    void navigateBack();
    void navigateUp();
    void zoomInCentered();
    void zoomOutCentered();
    void resetZoom();
    void returnToLanding();
    void onScanFinished();
    void processQueuedScanProgress();
    void processQueuedScanActivity();
    void processQueuedPermissionErrors();
    void onNodeActivated(FileNode* node);
    void onNodeHovered(FileNode* node);
    void onZoomInRequested(FileNode* node, QPointF anchorPos);
    void onZoomOutRequested(QPointF anchorPos);
    void onNodeContextMenuRequested(FileNode* node, QPoint globalPos);
    void openSettings();
    void openPathFromToolbar();
    void cancelScan();
    void onLimitToSameFilesystemToggled(bool checked);
    void onIncrementalRefreshFinished();
    void onPostProcessFinished();

private:
    void syncFilesystemWatchControllerState();
    void finalizeIncrementalRefresh(IncrementalRefreshResult refreshed);
    void onScanProgress(ScanResult scanResult);
    void updateScanStatusMessage();
    void refreshTypeLegendAsync(FileNode* root);
    void scheduleTreeMaintenance();
    void restoreHistoryFromPaths(const std::vector<ViewStatePaths>& historyPaths, FileNode* root);
    void activatePath(const QString& path, bool forceScan);
    void startScan(const QString& dir, bool forceRescan = false, bool backgroundRefresh = false);
    void navigateTo(FileNode* node, bool pushHistory,
                    const QPointF& anchorPos = QPointF(), bool useAnchor = false);
    void updateWindowTitle();
    void toggleFreeSpaceView(bool enabled);
    void updateNavigationActions();
    void updateCurrentViewUi();
    void setFreeSpaceVisible(bool visible);
    void placeFreeSpaceNodes(FileNode* currentNode);
    void loadRecentPaths(QSettings& store);
    void loadFavouritePaths(QSettings& store);
    void updateRecentPaths(const QString& path);
    void promoteRecentPath(const QString& path);
    void removeRecentPath(const QString& path);
    void clearRecentPaths();
    void setPathFavourite(const QString& path, bool favourite);
    void moveFavouritePath(const QString& path, const QString& targetPath, bool insertAfterTarget);
    void clearFavouritePaths();
    void collectLandingLocationPaths(QStringList& favouritePaths,
                                     QStringList& recentPaths,
                                     QStringList& devicePaths) const;
    void rebuildLandingLocationSections();
    void relayoutLandingLocationSections();
    void populateOpenLocationMenu(QMenu* menu);
    void syncPathCombo(const QString& path);
    void rebuildFilesystemWatchers();
    void setRefreshBusy(bool busy);
    void setSearchBusy(bool busy);
    void applySearchFromToolbar();
    void updatePathBarChrome();
    void updateLandingPageChrome();
    void updateToolbarIcons();
    void updateToolbarChrome();
    void updateToolbarResponsiveLayout();
    void updateTypeLegendPanel();
    void revealPathInFileManager(const QString& path, bool isDirectory);
    void showPathProperties(const FileNode* node, const FileNode* scanRoot,
                            std::shared_ptr<NodeArena> arena);
    bool confirmDeletion(const QFileInfo& info, bool permanentDelete);
    bool deletePath(const QFileInfo& info, bool permanentDelete);
    void notifyTrashChanged() const;
    void refreshPathAfterMutation(const QFileInfo& info);
    void launchIncrementalRefresh(const QString& refreshPath);
    void applyTreemapSettings(const TreemapSettings& settings, bool persist);
    void recolorCurrentTree();
    void syncColorThemeWithSystem(bool persist);
    void updateDirectoryTreePanel();
    void populateDirectoryTreeChildren(QTreeWidgetItem* item);
    void prepareDirectoryTreeItem(QTreeWidgetItem* item);
    void syncDirectoryTreeSelection();
    void captureTreemapPanelWidths();
    void rebuildTreemapSplitterLayout();
    QWidget* createLandingPage();
    void setLandingVisible(bool visible);
    void clearCurrentTreemap();
    void setupToolbar(QSettings& store);
    void setupCentralWidget(QSettings& store);
    void setupBackend();

    QStackedWidget* m_centralStack = nullptr;
    QWidget* m_landingPage = nullptr;
    QWidget* m_treemapPage = nullptr;
    TreemapWidget* m_treemapWidget = nullptr;
    BreadcrumbPathBar* m_pathBar = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QLabel* m_searchStatusLabel = nullptr;
    QLabel* m_scanSeenStatusLabel = nullptr;
    QLabel* m_scanPathStatusLabel = nullptr;
    QLabel* m_completedFilesStatusLabel = nullptr;
    QLabel* m_completedTotalStatusLabel = nullptr;
    QLabel* m_completedFreeStatusLabel = nullptr;
    QAction* m_scanCustomAction = nullptr;
    QAction* m_refreshAction = nullptr;
    QAction* m_backAction = nullptr;
    QAction* m_upAction = nullptr;
    QAction* m_zoomInAction = nullptr;
    QAction* m_zoomOutAction = nullptr;
    QAction* m_resetZoomAction = nullptr;
    QAction* m_homeAction = nullptr;
    QAction* m_toggleFreeSpaceAction = nullptr;
    QAction* m_toggleDirectoryTreeAction = nullptr;
    QAction* m_toggleTypeLegendAction = nullptr;
    QAction* m_settingsAction = nullptr;
    QAction* m_aboutAppAction = nullptr;
    QAction* m_aboutQtAction = nullptr;
    QAction* m_limitToSameFilesystemAction = nullptr;
    QAction* m_warningsMenuAction = nullptr;
    QToolBar* m_toolbar = nullptr;
    QMenu* m_openLocationMenu = nullptr;
    QToolButton* m_menuButton = nullptr;
    FilesystemWatchController* m_watchController = nullptr;
    QSplitter* m_treemapSplitter = nullptr;
    QWidget* m_treemapPane = nullptr;
    QWidget* m_directoryPanel = nullptr;
    QTreeWidget* m_directoryTree = nullptr;
    QWidget* m_typeLegendPanel = nullptr;
    QTreeWidget* m_typeLegendTree = nullptr;
    QAction*     m_permissionWarningAction   = nullptr;
    QWidget*     m_permissionErrorPanel      = nullptr;
    QListWidget* m_permissionErrorList       = nullptr;
    QIcon        m_permissionWarningIcon;

    QFutureWatcher<ScanResult>* m_watcher = nullptr;
    QFutureWatcher<ScanResult>* m_refreshWatcher = nullptr;
    QFutureWatcher<IncrementalRefreshResult>* m_postProcessWatcher = nullptr;

    // We need to keep the scan result alive
    ScanResult m_scanResult;
    ScanResult m_liveScanResult;
    std::optional<ScanResult> m_pendingScanProgress;
    QMutex m_scanProgressMutex;
    bool m_scanProgressQueued = false;
    std::shared_ptr<std::atomic_bool> m_scanPreviewSlotOpen;
    std::optional<ScanActivityUpdate> m_pendingScanActivity;
    QMutex m_scanActivityMutex;
    bool m_scanActivityQueued = false;
    QString m_latestScanActivityPath;
    qint64 m_latestScannedBytes = 0;
    QString m_currentPath;
    QStringList m_recentPaths;
    QStringList m_favouritePaths;
    std::vector<TreemapWidget::ViewState> m_history;
    bool m_scanInProgress = false;
    bool m_backgroundRefreshInProgress = false;
    bool m_incrementalRefreshInProgress = false;
    bool m_postProcessInProgress = false;
    bool m_postProcessStale = false;
    bool m_scanCancelled = false;
    bool m_incrementalRefreshCancelled = false;
    bool m_closeRequested = false;
    bool m_hasPendingScanRequest = false;
    bool m_pendingScanForceRescan = false;
    bool m_pendingScanBackgroundRefresh = false;
    QString m_pendingScanPath;
    QSet<QString> m_dirtyPaths;
    QTimer* m_searchDebounceTimer = nullptr;
    QComboBox* m_sizeFilterCombo = nullptr;
    QString m_activeRefreshPath;
    ViewStatePaths m_preRefreshViewPaths;
    std::vector<ViewStatePaths> m_preRefreshHistoryPaths;
    TreemapSettings m_settings;
    std::shared_ptr<std::atomic_bool> m_scanCancelToken;
    std::shared_ptr<std::atomic_bool> m_refreshCancelToken;
    std::vector<FileNode*> m_freeSpaceNodes;
    FileNode* m_mountPointFreeNode = nullptr; // free-space node injected at current sub-FS mount point
    bool m_showFreeSpaceInOverview = true;
    bool m_showDirectoryTree = false;
    bool m_showTypeLegend = false;
    bool m_showPermissionPanel       = false;
    int m_directoryPanelWidth = 300;
    int m_typeLegendPanelWidth = 320;
    int          m_permissionErrorPanelWidth = 260;
    QStringList  m_permissionErrors;
    QMutex       m_permissionErrorMutex;
    QList<ScanWarning> m_pendingPermissionErrors;
    bool         m_permissionErrorQueued     = false;
};

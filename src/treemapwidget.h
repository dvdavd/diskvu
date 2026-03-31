// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"
#include "treemapsettings.h"
#include <QAbstractScrollArea>
#include <QContextMenuEvent>
#include <QCursor>
#include <QIcon>
#include <QFont>
#include <QFontMetrics>
#include <QFutureWatcher>
#include <QHash>
#include <QLocale>
#include <QNativeGestureEvent>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QPropertyAnimation>
#include <QRectF>
#include <QRegularExpression>
#include <QString>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class QLabel;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QWidget;
class TooltipPanelWidget;
class UsageBarWidget;
class QHBoxLayout;
class QVBoxLayout;
class QScrollBar;

// Immutable search index: built once per tree, shared with background search threads.
// Held via shared_ptr so background tasks keep it alive after a tree change.
struct SearchIndex {
    std::vector<FileNode*> nodes;      // all searchable nodes (files + dirs)
    std::vector<uint32_t> nameOffsets; // indexed by node->id → byte offset in flatNames
    std::vector<uint16_t> nameLens;    // indexed by node->id → byte length in flatNames
    QHash<uint64_t, std::vector<FileNode*>> filesByExt; // packed extension key → matching files
    std::string flatNames;             // flat UTF-8 buffer of all case-folded names
    uint32_t nodeCount = 0;
};

struct SearchMatchResult {
    std::vector<FileNode*> directMatches;
    std::vector<uint8_t> matchCache;
    std::vector<bool> searchReachCache;
    bool cancelled = false;
};

struct FileTypeMatchResult {
    std::vector<uint8_t> matchCache;
    bool cancelled = false;
};

// Builds a SearchIndex from a tree. Exposed as a free function for testing.
std::shared_ptr<SearchIndex> buildSearchIndex(FileNode* root);

struct TooltipIconResult {
    QString path;
    QIcon icon;
};

class TreemapWidget : public QAbstractScrollArea {
    Q_OBJECT

public:
    struct ViewState {
        FileNode* node = nullptr;
        qreal cameraScale = 1.0;
        QPointF cameraOrigin;
        int semanticDepth = TreemapSettings::defaults().baseVisibleDepth;
        FileNode* semanticFocus = nullptr;
        FileNode* semanticLiveRoot = nullptr;
        qreal currentRootLayoutAspectRatio = 0.0;
    };

    explicit TreemapWidget(QWidget* parent = nullptr);

    void setRoot(FileNode* root, std::shared_ptr<NodeArena> rootArena = {},
                 bool prepareModel = true, bool animateLayout = true);
    void notifyTreeChanged();
    void setCurrentNode(FileNode* node, const QPointF& anchorPos = QPointF(), bool useAnchor = false);
    void restoreViewState(const ViewState& state,
                          const QPointF& anchorPos = QPointF(), bool useAnchor = false);
    void restoreViewStateImmediate(const ViewState& state);
    void applySettings(const TreemapSettings& settings);
    void setScanInProgress(bool inProgress);
    void setScanPath(const QString& path);
    void setSearchPattern(const QString& pattern);
    void setSizeFilter(qint64 minBytes, qint64 maxBytes);
    void setHighlightedFileType(const QString& typeLabel);
    // Returns a snapshot of which node IDs are "search-reachable": directly matched
    // by the current search/size filter, or inside a search-matched directory.
    // Empty vector when no search is active. Safe to capture and read off the main thread.
    std::vector<bool> captureSearchReachSnapshot() const;
    std::shared_ptr<SearchIndex> captureSearchIndexSnapshot() const;
    void setPreviewHoveredNode(FileNode* node);
    void zoomCenteredIn();
    void zoomCenteredOut();
    void resetZoomToActualSize();
    QString highlightedFileType() const { return m_highlightedFileType; }
    void setWheelZoomEnabled(bool enabled) { m_wheelZoomEnabled = enabled; }
    FileNode* currentNode() const { return m_current; }
    const TreemapSettings& settings() const { return m_settings; }
    qreal cameraScale() const { return m_cameraScale; }
    ViewState currentViewState() const;
    ViewState overviewViewState(FileNode* node = nullptr) const;

signals:
    void nodeActivated(FileNode* node);
    void nodeHovered(FileNode* node);
    void backRequested();
    void zoomInRequested(FileNode* node, QPointF anchorPos);
    void zoomOutRequested(QPointF anchorPos);
    void nodeContextMenuRequested(FileNode* node, QPoint globalPos);
    void fileTypeHighlightBusyChanged(bool busy);
    void searchResultsChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    enum class StableMetricChannel : quint8 {
        TileReveal = 1,
        HeaderLabel = 2,
        FileLabel = 3,
        FolderDetail = 4,
    };

    struct DirectoryRenderState {
        QRectF revealArea;
        QRectF childLayoutRect;
        QRectF childPaintRect;
        QRectF childContentClip;
        QRectF tileRect;
        QRectF framedHeaderRect;
        QRectF tileFillClipRect;
        QRectF contentFillClipRect;
        qreal chromeOpacity = 0.0;
        bool showsChildren = false;
        bool showChrome = false;
    };

    int desiredSemanticDepthForScale(qreal scale) const;
    QRectF currentRootViewRect() const;
    DirectoryRenderState computeDirectoryRenderState(const FileNode* node, const QRectF& viewRect,
                                                     const QRectF& effectiveClip, int depth) const;
    QRectF childLayoutRectForNode(const FileNode* node, const QRectF& viewRect) const;
    QRectF childPaintRectForNode(const FileNode* node, const QRectF& viewRect) const;
    FileNode* semanticFocusCandidateAt(const QPointF& pos, const QRectF& viewRect, int depth = -1) const;
    QString cachedSizeLabel(const FileNode* node) const;
    QString cachedElidedLabel(const FileNode* node, const QString& text, int width,
                              const QFontMetrics& metrics, quint64 fontKey = 0,
                              Qt::LayoutDirection direction = Qt::LeftToRight) const;
    QString cachedElidedLabelWithBucket(const FileNode* node, const QString& text, int width,
                                        const QFontMetrics& metrics, quint64 fontKey,
                                        int bucket,
                                        Qt::LayoutDirection direction = Qt::LeftToRight) const;
    void layoutVisibleChildren(FileNode* node, const QRectF& tileViewRect,
                               const QRectF& viewContent,
                               const QRectF& visibleClip,
                               std::vector<std::pair<FileNode*, QRectF>>& out) const;
    QSizeF stabilizedNodeSize(const FileNode* node, StableMetricChannel channel,
                              const QSizeF& size, qreal widthBucket,
                              qreal heightBucket, qreal widthHysteresis,
                              qreal heightHysteresis) const;
    bool canPaintChildrenForDisplay(const FileNode* node, const QRectF& viewBounds, int depth) const;
    qreal tinyChildRevealOpacityForLayout(const FileNode* node, const QRectF& layoutArea) const;
    qreal tileRevealOpacityForNode(const FileNode* node, const QRectF& layoutArea) const;
    qreal childRevealOpacityForLayout(const FileNode* node, const QRectF& layoutArea,
                                      int childDepth) const;
    qreal folderDetailOpacityForNode(const FileNode* node, const QRectF& bounds) const;
    qreal pixelScale() const;
    QRectF sceneToViewRectF(const QRectF& rect) const;
    QPointF maxCameraOriginForScale(qreal scale) const;
    QPointF clampCameraOrigin(const QPointF& origin, qreal scale) const;
    void animateCameraTo(qreal scale, const QPointF& origin,
                         const QPointF& focusScenePos = QPointF(),
                         const QPointF& focusScreenPos = QPointF(),
                         bool useFocusAnchor = false);
    void setCameraImmediate(qreal scale, const QPointF& origin);
    void resetCamera();
    void drawScene(QPainter& painter, FileNode* root, const QRectF& visibleClip = QRectF());
    void drawSceneScaled(QPainter& painter, FileNode* root, const QRectF& targetRect, qreal opacity = 1.0);
    void drawMatchOverlay(QPainter& painter, FileNode* root,
                          const QRectF& visibleClip = QRectF());
    QPixmap renderSceneToPixmap(FileNode* root);
    QRectF zoomRectForAnchor(const QRectF& preferredRect, const QPointF& anchorPos) const;
    void startZoomAnimation(const QPixmap& previousFrame, const QRectF& sourceRect,
                            bool zoomIn, bool crossfadeOnly = false);
    bool findVisibleViewRect(FileNode* root, const QRectF& rootViewRect,
                             FileNode* target, QRectF* outRect, int depth = -1) const;
    void relayout();
    void rebuildSearchMatches();
    void rebuildSearchMetadata();
    void cancelPendingSearch();
    void onSearchTaskFinished();
    void rebuildSearchMetadataAsync();
    void onMetadataTaskFinished();
    void rebuildFileTypeMatchesAsync();
    void cancelPendingFileTypeHighlight();
    void onFileTypeMatchTaskFinished();
    void rebuildCombinedMatchCache();
    template <typename Predicate>
    quint8 rebuildMatchTree(FileNode* node, std::vector<uint8_t>& matchCache,
                            Predicate&& isDirectMatch, quint8 selfMatchFlag, quint8 subtreeMatchFlag);
    bool isDescendantOf(const FileNode* node, const FileNode* ancestor) const;
    static void sortChildrenBySize(FileNode* node);
    void paintNode(QPainter& painter, FileNode* node, int depth,
                   const QRectF& visibleClip, const QRectF& viewRect,
                   qreal subtreeHoverBlend = 0.0, qreal subtreePrevHoverBlend = 0.0,
                   bool applyOwnReveal = true);
    void paintMatchOverlayNode(QPainter& painter, FileNode* node, int depth,
                               const QRectF& visibleClip, const QRectF& viewRect) const;
    void paintMatchBordersNode(QPainter& painter, FileNode* node, int depth,
                               const QRectF& visibleClip, const QRectF& viewRect,
                               const std::function<quint8(const FileNode*)>& matchLookup,
                               quint8 selfMatchFlag) const;
    FileNode* hitTestChildren(FileNode* node, const QPointF& pos, int depth,
                              const QRectF& tileViewRect, const QRectF& contentArea,
                              const QRectF& visibleClip,
                              QRectF* outRect = nullptr) const;
    FileNode* hitTest(FileNode* node, const QPointF& pos, int depth,
                      const QRectF& viewRect, QRectF* outRect = nullptr) const;
    void syncSemanticDepthToScale();
    void syncScrollBars();
    void applyScrollBarCameraPosition();
    void updateScrollBarOverlayStyle();
    void updateOverlayScrollBarGeometry();
    void setOverlayScrollBarExpanded(QScrollBar* bar, bool expanded);
    void setOverlayScrollBarVisible(QScrollBar* bar, bool visible);
    qreal computeCurrentRootLayoutAspectRatio() const;
    void clearHoverState(bool notify = true);
    void updateOwnedTooltipStyle();
    void updateOwnedTooltipLayoutDirection();
    QIcon fallbackTooltipIcon(bool isDirectory) const;
    void requestTooltipIcon(const QString& path, bool isDirectory);
    void positionOwnedTooltip(const QPoint& globalPos);
    void showOwnedTooltip(const QPoint& globalPos, FileNode* node, const QString& text,
                          double parentPercent, double rootPercent);
    void hideOwnedTooltip();
    void stopAnimatedNavigation();
    void panCameraImmediate(const QPointF& sceneDelta);
    void zoomCameraImmediate(qreal targetScale, const QPointF& anchorScenePos,
                             const QPointF& anchorScreenPos);
    FileNode* interactiveNodeAt(const QPointF& pos) const;
    void updateHoverAt(const QPointF& pos, const QPoint& globalPos, bool notify = true);
    void refreshHoverUnderPointer();
    void beginPressHold(FileNode* node, const QPointF& pos, bool fromTouch);
    void updatePressHold(const QPointF& pos);
    void cancelPressHold();
    void triggerPressHoldContextMenu();
    void beginTouchPan(const QPointF& pos);
    void beginTouchPinch(const QList<QEventPoint>& points);
    void updateTouchGesture(const QList<QEventPoint>& points);
    void endTouchGesture();
    void activateTouchTap(const QPointF& pos);

    bool continuousZoomGeometryActive() const;

    FileNode* m_root = nullptr;
    std::shared_ptr<NodeArena> m_rootArena;
    FileNode* m_current = nullptr;
    FileNode* m_hovered = nullptr;
    FileNode* m_previousHovered = nullptr;
    QRectF m_hoveredRect;
    QRectF m_previousHoveredRect;
    QString m_hoveredTooltip;
    TooltipPanelWidget* m_hoverTooltipWidget = nullptr;
    QHBoxLayout* m_hoverTooltipTopRowLayout = nullptr;
    QVBoxLayout* m_hoverTooltipTopTextColumnLayout = nullptr;
    QLabel* m_hoverTooltipIconLabel = nullptr;
    QLabel* m_hoverTooltipTextLabel = nullptr;
    QWidget* m_hoverTooltipSeparator = nullptr;
    QWidget* m_hoverTooltipDetailsWidget = nullptr;
    QLabel* m_hoverTooltipSizeLabel = nullptr;
    UsageBarWidget* m_hoverTooltipBarWidget = nullptr;
    QLabel* m_hoverTooltipStatsLabel = nullptr;
    QGraphicsOpacityEffect* m_hoverTooltipOpacity = nullptr;
    QPropertyAnimation* m_hoverTooltipFade = nullptr;
    QTimer m_resizeTimer;
    QVariantAnimation m_hoverAnimation;
    QVariantAnimation m_zoomAnimation;
    QVariantAnimation m_layoutAnimation;
    QVariantAnimation m_cameraAnimation;
    QPixmap m_previousFrame;
    QPixmap m_nextFrame;
    QPixmap m_layoutPreviousFrame;
    QPixmap m_layoutNextFrame;
    QPixmap m_liveFrame;
    QPixmap m_scrollBuffer;
    QSize   m_liveFrameDeviceSize;
    FileNode* m_lastLiveRoot = nullptr;
    QPointF   m_lastLiveOrigin;
    qreal     m_lastLiveScale = 0.0;
    int       m_lastLiveDepth = -1;
    QRectF m_zoomSourceRect;
    bool m_zoomingIn = false;
    bool m_zoomCrossfadeOnly = false;

    // Per-frame constants snapshotted in drawScene — identical for every node
    qreal m_framePixelScale  = 1.0;
    qreal m_frameOutlineWidth = 0.0;

    // Cached drawing resources
    QFont m_font;
    QFontMetrics m_fm{m_font};
    QFont m_headerFont;
    QFontMetrics m_headerFm{m_headerFont};
    QFont m_fileFont;
    QFontMetrics m_fileFm{m_fileFont};

    // Cached fixed pens (color does not vary per node)
    QPen m_penBorder{QColor(0, 0, 0, 235), 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin};
    QPen m_penFileBorder{QColor(0, 0, 0, 80), 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin};
    QPen m_penContent{QColor(255, 255, 255, 45), 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin};
    QPen m_penContentHov{QColor(255, 255, 255, 90), 1.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin};

    // Per-frame animation cache (set at the top of paintEvent)
    qreal m_animT = 0.0;
    bool m_layoutAnimating = false;
    qreal m_cameraScale = 1.0;
    qreal m_cameraStartScale = 1.0;
    qreal m_cameraTargetScale = 1.0;
    QPointF m_cameraOrigin;
    QPointF m_cameraStartOrigin;
    QPointF m_cameraTargetOrigin;
    QPointF m_cameraFocusScenePos;
    QPointF m_cameraFocusScreenPos;
    bool m_cameraUseFocusAnchor = false;
    bool m_pendingRestoredViewState = false;
    int m_pendingRestoredSemanticDepth = TreemapSettings::defaults().baseVisibleDepth;
    FileNode* m_pendingRestoredSemanticFocus = nullptr;
    FileNode* m_pendingRestoredSemanticLiveRoot = nullptr;
    int m_continuousZoomSettleFramesRemaining = 0;
    TreemapSettings m_settings;
    int m_activeSemanticDepth = TreemapSettings::defaults().baseVisibleDepth;
    FileNode* m_semanticFocus = nullptr;
    FileNode* m_semanticLiveRoot = nullptr;
    bool m_wheelZoomEnabled = true;
    bool m_scanInProgress = false;
    QString m_scanPath;
    QString m_searchPattern;
    QString m_searchCaseFoldedPattern;
    bool m_searchActive = false;
    bool m_searchUsesWildcards = false;
    QString m_previousSearchCaseFoldedPattern;
    bool m_previousSearchUsesWildcards = false;
    qint64 m_minSizeFilter = 0;
    qint64 m_maxSizeFilter = 0;
    bool m_sizeFilterActive = false;
    qint64 m_previousMinSizeFilter = 0;
    qint64 m_previousMaxSizeFilter = 0;
    QString m_highlightedFileType;
    QString m_previousHighlightedFileType;
    QPointF m_lastActivationPos;
    bool m_contextMenuActive = false;
    bool m_middlePanning = false;
    QPointF m_middlePanStartPos;
    QPointF m_middlePanStartOrigin;
    QTimer m_pressHoldTimer;
    bool m_pressHoldActive = false;
    bool m_pressHoldFromTouch = false;
    bool m_pressHoldTriggered = false;
    FileNode* m_pressHoldNode = nullptr;
    QPointF m_pressHoldStartPos;
    QPointF m_pressHoldCurrentPos;
    bool m_touchGestureActive = false;
    bool m_touchPanning = false;
    bool m_touchPinching = false;
    bool m_touchTapEligible = false;
    bool m_touchSequenceTapBlocked = false;
    bool m_touchPendingBackOnRelease = false;
    bool m_touchSuppressTap = false;
    QPointF m_touchPanLastPos;
    QPointF m_touchTapStartPos;
    qreal m_touchPinchInitialDistance = 0.0;
    qreal m_touchPinchInitialScale = 1.0;
    qreal m_touchPinchCandidateDistance = 0.0; // inter-finger distance at first 2-finger contact
    QPointF m_touchPinchAnchorScenePos;
    QTimer m_touchTapSuppressTimer;
    bool m_nativeGestureActive = false;
    bool m_nativeGesturePinching = false;
    bool m_nativeGesturePendingBackOnEnd = false;
    QPointF m_nativeGestureAnchorScenePos;
    qreal m_hoverBlend = 1.0;
    bool m_ownsTooltip = false;
    QLocale m_systemLocale = QLocale::system();
    QPalette m_framePalette;                 // cached per drawScene call, avoids repeated palette() vtable calls
    mutable QHash<const FileNode*, QString> m_sizeLabelCache;
    mutable QHash<quint64, QString> m_elidedTextCache;
    mutable QHash<quint64, int> m_elidedDisplayWidthCache;
    mutable QHash<quint64, QSizeF> m_stableMetricCache;
    mutable QHash<const FileNode*, bool> m_headerLabelVisibleCache;
    mutable QHash<const FileNode*, bool> m_fileLabelVisibleCache;
    mutable QHash<const FileNode*, bool> m_fileSizeLabelVisibleCache;
    std::vector<uint8_t> m_searchMatchCache;   // indexed by FileNode::id, written on main thread
    uint32_t m_nodeCount = 0;
    std::shared_ptr<SearchIndex> m_searchIndex;         // current (immutable once published)
    std::shared_ptr<SearchIndex> m_pendingSearchIndex;  // which index the in-flight search used
    std::shared_ptr<std::atomic<bool>> m_searchCancelToken;
    QFutureWatcher<SearchMatchResult>* m_searchWatcher = nullptr;
    QFutureWatcher<std::shared_ptr<SearchIndex>>* m_metadataWatcher = nullptr;
    std::shared_ptr<std::atomic<bool>> m_fileTypeCancelToken;
    QFutureWatcher<FileTypeMatchResult>* m_fileTypeWatcher = nullptr;
    std::vector<uint8_t> m_fileTypeMatchCache;
    std::vector<uint8_t> m_combinedMatchCache;
    std::vector<bool>    m_searchReachCache;   // forward-DFS reach, built by the async search worker
    std::vector<uint8_t> m_searchMatchScratch;
    std::vector<bool>    m_searchReachScratch;
    QString m_pendingHighlightedFileType;
    QHash<QString, QIcon> m_tooltipIconCache;
    QString m_activeTooltipIconPath;
    QString m_pendingTooltipIconPath;
    bool m_pendingTooltipIconIsDirectory = false;
    bool m_tooltipIconLoadQueued = false;

    uint8_t searchMatchFlags(const FileNode* n) const {
        if (m_searchMatchCache.empty()) return 0;
        return (n && n->id < m_searchMatchCache.size()) ? m_searchMatchCache[n->id] : 0;
    }
    uint8_t fileTypeMatchFlags(const FileNode* n) const {
        if (m_fileTypeMatchCache.empty()) return 0;
        return (n && n->id < m_fileTypeMatchCache.size()) ? m_fileTypeMatchCache[n->id] : 0;
    }
    uint8_t combinedMatchFlags(const FileNode* n) const {
        if (m_combinedMatchCache.empty()) return 0;
        return (n && n->id < m_combinedMatchCache.size()) ? m_combinedMatchCache[n->id] : 0;
    }
    // Returns the flags to use for overlay painting. When both search and file type
    // are active simultaneously, uses the combined (AND) cache; otherwise OR-combines
    // whichever individual caches are active.
    // Falls back to OR while either side is still in its first in-flight task
    // (no prior results yet) to avoid a blank transition during the async gap.
    uint8_t effectiveMatchFlags(const FileNode* n) const {
        if (m_searchActive && !m_highlightedFileType.isEmpty()) {
            const bool searchPending = m_searchWatcher->isRunning()
                && m_previousDirectSearchMatches.empty();
            const bool typePending = m_fileTypeWatcher->isRunning()
                && m_previousHighlightedFileType.isEmpty();
            if (!searchPending && !typePending)
                return combinedMatchFlags(n);
        }
        return searchMatchFlags(n) | fileTypeMatchFlags(n);
    }
    std::vector<FileNode*> m_previousDirectSearchMatches;
    struct SplitCacheEntry {
        qreal aspectRatio = 1.0;   // viewContent.width() / viewContent.height() at cache time
        std::vector<std::pair<FileNode*, QRectF>> rects;  // in [0, aspectRatio] x [0, 1] space
    };
    mutable QHash<FileNode*, SplitCacheEntry> m_liveSplitCache;
    qreal m_currentRootLayoutAspectRatio = 1.0;
    bool m_syncingScrollBars = false;
    struct ScrollBarState {
        Qt::ScrollBarPolicy policy = Qt::ScrollBarAlwaysOff;
        int range = -1, pageStep = -1, singleStep = -1, value = -1;
    };
    ScrollBarState m_lastHScrollBar, m_lastVScrollBar;
    QScrollBar* m_overlayHScrollBar = nullptr;
    QScrollBar* m_overlayVScrollBar = nullptr;
    QGraphicsOpacityEffect* m_overlayHOpacityEffect = nullptr;
    QGraphicsOpacityEffect* m_overlayVOpacityEffect = nullptr;
    QPropertyAnimation m_overlayHOpacityAnimation;
    QPropertyAnimation m_overlayVOpacityAnimation;
    QVariantAnimation m_overlayHStateAnimation;
    QVariantAnimation m_overlayVStateAnimation;
    qreal m_overlayHExpandProgress = 0.0;
    qreal m_overlayVExpandProgress = 0.0;
    bool m_overlayHShouldBeVisible = false;
    bool m_overlayVShouldBeVisible = false;

};

template <typename Predicate>
quint8 TreemapWidget::rebuildMatchTree(FileNode* node, std::vector<uint8_t>& matchCache,
                                       Predicate&& isDirectMatch, quint8 selfMatchFlag,
                                       quint8 subtreeMatchFlag)
{
    if (!node) {
        return 0;
    }

    quint8 flags = isDirectMatch(node) ? selfMatchFlag : 0;
    for (FileNode* child : node->children) {
        if (rebuildMatchTree(child, matchCache, isDirectMatch,
                             selfMatchFlag, subtreeMatchFlag) != 0) {
            flags |= subtreeMatchFlag;
        }
    }

    if (node->id < matchCache.size()) {
        matchCache[node->id] = flags;
    }
    return flags;
}

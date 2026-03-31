// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"
#include "treemapsettings.h"
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPixmap>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QRegularExpression>
#include <QString>
#include <utility>
#include <vector>

struct TileChromeGeometry {
    QRectF tileRect;
    QRectF outerBorderRect;
    QRectF innerFrameRect;
    QRectF headerRect;
    QRectF framedHeaderRect;
    QRectF contentPaintRect;
    QRectF contentLayoutRect;
};

struct RevealThresholds {
    qreal childStartWidth = 0.0;
    qreal childStartHeight = 0.0;
    qreal childFullWidth = 0.0;
    qreal childFullHeight = 0.0;
    qreal detailWidth = 0.0;
    qreal detailHeight = 0.0;
};

qreal normalizedPixelScale(qreal pixelScale);
qreal snapLengthToPixels(qreal length, qreal pixelScale);
QRectF snapRectToPixels(const QRectF& rect, qreal pixelScale = 1.0);
qreal applyHysteresis(qreal current, qreal previous, qreal hysteresis);
QSizeF applyAxisHysteresis(const QSizeF& current, const QSizeF& previous, qreal hysteresis);
QSizeF applyAxisHysteresis(const QSizeF& current, const QSizeF& previous, qreal widthHysteresis, qreal heightHysteresis);
qreal smoothstep(qreal t);
qreal revealOpacityForSize(const QSizeF& size, qreal startWidth, qreal startHeight,
                           qreal fullWidth, qreal fullHeight);
RevealThresholds revealThresholds(const TreemapSettings& settings);
Qt::TextElideMode elideModeForDirection(Qt::LayoutDirection direction);
qreal leadingTextX(const QRectF& rect, const QString& text, const QFontMetrics& metrics,
                   Qt::LayoutDirection direction);
bool canPaintChildren(const QRectF& bounds, int depth, int maxVisibleDepth,
                      const TreemapSettings& settings);
void squarifiedLayout(const std::vector<FileNode*>& children,
                      const QRectF& rect,
                      qint64 totalSize,
                      std::vector<std::pair<FileNode*, QRectF>>& result);
QPointF snapCameraOriginToPixelGrid(const QPointF& origin, qreal scale, qreal pixelScale = 1.0);
QColor contrastingTextColor(const QColor& background);
QColor cachedShade(const QColor& color, bool lighten, int factor);
QColor blendColors(const QColor& from, const QColor& to, qreal t);
QColor cachedPanelBase(const QColor& folderBase, const QColor& paletteBase);
QColor depthAdjustedColor(const QColor& color, int depth, int step = 4, int limit = 8);
QRegularExpression buildSearchRegex(const QString& pattern);
QRect expandedDirtyRect(const QRect& rect, const QRect& bounds, int padding = 4);
QRectF strokeRectInside(const QRectF& rect, qreal inset = 0.5);
QRectF snappedStrokeRectInside(const QRectF& rect, qreal inset, qreal pixelScale);
QRectF logicalRectToPixmapSourceRect(const QPixmap& pixmap, const QRectF& logicalRect);
QRectF confineRectToBounds(const QRectF& rect, const QRectF& bounds);
void fillInnerBorder(QPainter& painter, const QRectF& rect, const QColor& color, qreal width);
void fillOuterRectMinusInner(QPainter& painter, const QRectF& outer, const QRectF& inner,
                             const QColor& color);
qreal folderDetailOpacity(const QRectF& bounds, const TreemapSettings& settings);
TileChromeGeometry makeTileChromeGeometry(const QRectF& bounds, const TreemapSettings& settings,
                                          bool snapTile, bool detailedChrome, qreal pixelScale);
void drawHeaderLabel(QPainter& painter, const QRectF& rect, const QString& text,
                     const QFont& font, const QFontMetrics& metrics, const QColor& color,
                     qreal pixelScale, Qt::LayoutDirection direction);
void drawFileLabelLine(QPainter& painter, const QRectF& rect, const QString& text, int lineIndex,
                       const QFont& font, const QFontMetrics& metrics, const QColor& color,
                       qreal pixelScale, Qt::LayoutDirection direction);
void drawFileLabel(QPainter& painter, const QRectF& rect, const QString& text,
                   const QFont& font, const QFontMetrics& metrics, const QColor& color,
                   qreal pixelScale, Qt::LayoutDirection direction);
void paintTinyNodeFill(QPainter& painter, const QRectF& rect, const QColor& fillColor,
                       qreal pixelScale, qreal opacity);

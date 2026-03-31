// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "treemap_drawing.h"
#include <QHash>
#include <QPointF>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace {

double computeSquarifiedWorst(double rmax, double rmin, double s, double C)
{
    // Worst aspect ratio for a row: max(rmax*C/s², s²/(rmin*C))
    const double sC = s * s / C;
    return std::max(rmax / sC, sC / rmin);
}

qreal clampRevealStart(qreal full, qreal fadeDistance, qreal minVisible)
{
    const qreal clampedFull = std::max<qreal>(1.0, full);
    const qreal floor = std::clamp(minVisible, 0.0, clampedFull);
    return std::clamp(clampedFull - fadeDistance, floor, clampedFull);
}

qreal clampDetailThreshold(qreal value, qreal minValue, qreal maxValue)
{
    if (maxValue <= minValue) {
        return maxValue;
    }
    return std::clamp(value, minValue, maxValue);
}

} // namespace

qreal normalizedPixelScale(qreal pixelScale)
{
    return pixelScale > 0.0 ? pixelScale : 1.0;
}

qreal snapLengthToPixels(qreal length, qreal pixelScale)
{
    if (length <= 0.0) {
        return 0.0;
    }

    const qreal scale = normalizedPixelScale(pixelScale);
    return std::max(1.0 / scale, std::round(length * scale) / scale);
}

QRectF snapRectToPixels(const QRectF& rect, qreal pixelScale)
{
    const qreal scale = normalizedPixelScale(pixelScale);
    const qreal left = std::round(rect.left() * scale) / scale;
    const qreal top = std::round(rect.top() * scale) / scale;
    const qreal right = std::round((rect.left() + rect.width()) * scale) / scale;
    const qreal bottom = std::round((rect.top() + rect.height()) * scale) / scale;
    return QRectF(left, top, std::max<qreal>(0.0, right - left), std::max<qreal>(0.0, bottom - top));
}

qreal applyHysteresis(qreal current, qreal previous, qreal hysteresis)
{
    if (std::abs(current - previous) <= hysteresis) {
        return previous;
    }
    return current;
}

QSizeF applyAxisHysteresis(const QSizeF& current, const QSizeF& previous, qreal hysteresis)
{
    return QSizeF(applyHysteresis(current.width(), previous.width(), hysteresis),
                  applyHysteresis(current.height(), previous.height(), hysteresis));
}

QSizeF applyAxisHysteresis(const QSizeF& current, const QSizeF& previous, qreal widthHysteresis, qreal heightHysteresis)
{
    return QSizeF(applyHysteresis(current.width(), previous.width(), widthHysteresis),
                  applyHysteresis(current.height(), previous.height(), heightHysteresis));
}

qreal smoothstep(qreal t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

qreal revealOpacityForSize(const QSizeF& size, qreal startWidth, qreal startHeight,
                           qreal fullWidth, qreal fullHeight)
{
    const qreal fadeSpanW = std::max<qreal>(1.0, fullWidth - startWidth);
    const qreal fadeSpanH = std::max<qreal>(1.0, fullHeight - startHeight);
    const qreal fadeW = (size.width()  - startWidth) / fadeSpanW;
    const qreal fadeH = (size.height() - startHeight) / fadeSpanH;
    return std::min(
        smoothstep(std::clamp(fadeW, 0.0, 1.0)),
        smoothstep(std::clamp(fadeH, 0.0, 1.0)));
}

RevealThresholds revealThresholds(const TreemapSettings& settings)
{
    RevealThresholds thresholds;
    thresholds.childFullWidth = std::max<qreal>(1.0, settings.minRevealWidth);
    thresholds.childFullHeight = std::max<qreal>(1.0, settings.minRevealHeight);
    thresholds.childStartWidth = clampRevealStart(
        thresholds.childFullWidth, settings.revealFadeWidth, settings.minTileSize);
    thresholds.childStartHeight = clampRevealStart(
        thresholds.childFullHeight, settings.revealFadeHeight, settings.minTileSize);

    const qreal panelInset = settings.border + settings.folderPadding;
    const qreal minChromeWidth = thresholds.childStartWidth
        - std::max<qreal>(6.0, settings.revealFadeWidth * 0.35);
    const qreal minChromeHeight = std::max<qreal>(
        thresholds.childStartHeight + std::max<qreal>(2.0, settings.revealFadeHeight * 0.05),
        settings.headerHeight + (2.0 * panelInset));
    thresholds.detailWidth = clampDetailThreshold(
        minChromeWidth, settings.minTileSize, thresholds.childFullWidth);
    thresholds.detailHeight = clampDetailThreshold(
        minChromeHeight, settings.minTileSize, thresholds.childFullHeight);
    return thresholds;
}

Qt::TextElideMode elideModeForDirection(Qt::LayoutDirection direction)
{
    return direction == Qt::RightToLeft ? Qt::ElideLeft : Qt::ElideRight;
}

qreal leadingTextX(const QRectF& rect, const QString& text, const QFontMetrics& metrics,
                   Qt::LayoutDirection direction)
{
    const qreal textWidth = metrics.horizontalAdvance(text);
    return direction == Qt::RightToLeft ? rect.right() - textWidth : rect.left();
}

bool canPaintChildren(const QRectF& bounds, int depth, int maxVisibleDepth,
                      const TreemapSettings& settings)
{
    if (depth >= maxVisibleDepth) {
        return false;
    }

    const RevealThresholds thresholds = revealThresholds(settings);
    const qreal panelInset = settings.border + settings.folderPadding;
    const qreal detailFadeW = smoothstep(std::clamp(
        (bounds.width()  - (thresholds.detailWidth  - 8.0)) / 8.0, 0.0, 1.0));
    const qreal detailFadeH = smoothstep(std::clamp(
        (bounds.height() - (thresholds.detailHeight - 8.0)) / 8.0, 0.0, 1.0));
    const bool detailedChrome = std::min(detailFadeW, detailFadeH) > 0.0;
    const QRectF contentArea = detailedChrome
        ? bounds.adjusted(panelInset, settings.headerHeight + panelInset, -panelInset, -panelInset)
        : bounds.adjusted(panelInset, panelInset, -panelInset, -panelInset);
    if (contentArea.width() < thresholds.childStartWidth
            || contentArea.height() < thresholds.childStartHeight) {
        return false;
    }

    return true;
}

void squarifiedLayout(const std::vector<FileNode*>& children,
                      const QRectF& rect,
                      qint64 totalSize,
                      std::vector<std::pair<FileNode*, QRectF>>& result)
{
    if (children.empty() || totalSize <= 0 || rect.width() <= 0.0 || rect.height() <= 0.0)
        return;

    QRectF rem = rect;
    double S_rem = static_cast<double>(totalSize);
    const int n = static_cast<int>(children.size());
    int i = 0;

    while (i < n && rem.width() > 0.0 && rem.height() > 0.0) {
        const double W = rem.width();
        const double H = rem.height();
        const double L = std::max(W, H);
        const double w = std::min(W, H);
        const double C = L * S_rem / w;  // shape constant for this row

        // Greedily extend the row while aspect ratio improves
        const int rowStart = i;
        double rowSum = static_cast<double>(children[i]->size);
        double rmax = rowSum, rmin = rowSum;
        double worst = computeSquarifiedWorst(rmax, rmin, rowSum, C);
        ++i;

        while (i < n) {
            const double next = static_cast<double>(children[i]->size);
            const double newSum = rowSum + next;
            const double nextWorst = computeSquarifiedWorst(rmax, next, newSum, C);
            if (nextWorst > worst)
                break;
            rowSum = newSum;
            rmin = next;
            worst = nextWorst;
            ++i;
        }

        // Place row as a strip
        if (W >= H) {
            const bool lastStrip = (i >= n) || std::abs(S_rem - rowSum) < 1e-9;
            const double stripH = lastStrip ? H : (H * rowSum / S_rem);
            double x = rem.x();
            for (int j = rowStart; j < i; ++j) {
                const bool lastTile = (j + 1 == i);
                const double tileW = lastTile ? (rem.x() + W - x) : (W * children[j]->size / rowSum);
                result.emplace_back(children[j], QRectF(x, rem.y(), tileW, stripH));
                x += tileW;
            }
            rem = QRectF(rem.x(), rem.y() + stripH, W, H - stripH);
        } else {
            const bool lastStrip = (i >= n) || std::abs(S_rem - rowSum) < 1e-9;
            const double stripW = lastStrip ? W : (W * rowSum / S_rem);
            double y = rem.y();
            for (int j = rowStart; j < i; ++j) {
                const bool lastTile = (j + 1 == i);
                const double tileH = lastTile ? (rem.y() + H - y) : (H * children[j]->size / rowSum);
                result.emplace_back(children[j], QRectF(rem.x(), y, stripW, tileH));
                y += tileH;
            }
            rem = QRectF(rem.x() + stripW, rem.y(), W - stripW, H);
        }
        S_rem -= rowSum;
    }
}

QPointF snapCameraOriginToPixelGrid(const QPointF& origin, qreal scale, qreal pixelScale)
{
    const qreal snapScale = scale * normalizedPixelScale(pixelScale);
    if (snapScale <= 0.0) {
        return origin;
    }

    return QPointF(
        std::floor(origin.x() * snapScale) / snapScale,
        std::floor(origin.y() * snapScale) / snapScale);
}

QColor contrastingTextColor(const QColor& background)
{
    // Cache QRgb → QRgb: there are only ~50–200 unique derived colors per scan
    // (hue-wheel × a handful of darker()/lighter() variants) so this hash stays
    // tiny while eliminating the pow() math on every visible tile every frame.
    static QHash<QRgb, QRgb> cache;
    const QRgb key = background.rgba();
    auto it = cache.constFind(key);
    if (it != cache.cend())
        return QColor::fromRgba(*it);

    auto linearize = [](qreal channel) {
        return (channel <= 0.04045) ? (channel / 12.92)
                                    : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const qreal r = linearize(background.redF());
    const qreal g = linearize(background.greenF());
    const qreal b = linearize(background.blueF());
    const qreal luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;

    const QColor lightText(245, 245, 245);
    const QColor darkText(24, 24, 28);
    const qreal contrastWithLight = (1.0 + 0.05) / (luminance + 0.05);
    const qreal contrastWithDark = (luminance + 0.05) / (0.009 + 0.05);
    const QColor result = (contrastWithDark >= contrastWithLight) ? darkText : lightText;
    cache.insert(key, result.rgba());
    return result;
}

QColor cachedShade(const QColor& color, bool lighten, int factor)
{
    static QHash<quint64, QRgb> cache;
    const quint64 key = (static_cast<quint64>(color.rgba()) << 32)
        | (static_cast<quint64>(lighten ? 1u : 0u) << 31)
        | static_cast<quint64>(factor & 0x7fffffff);
    auto it = cache.constFind(key);
    if (it != cache.cend()) {
        return QColor::fromRgba(*it);
    }

    const QColor result = lighten ? color.lighter(factor) : color.darker(factor);
    cache.insert(key, result.rgba());
    return result;
}

QColor blendColors(const QColor& from, const QColor& to, qreal t)
{
    const qreal clamped = std::clamp(t, 0.0, 1.0);
    const qreal inv = 1.0 - clamped;

    // Use squared interpolation to approximate gamma-correct blending (γ ≈ 2.0).
    // This prevents the desaturation and "dip" in luminosity that occurs with 
    // linear RGB blending, keeping transitions vibrant.
    auto blend = [clamped, inv](qreal a, qreal b) {
        return std::sqrt((inv * a * a) + (clamped * b * b));
    };

    return QColor::fromRgbF(
        blend(from.redF(), to.redF()),
        blend(from.greenF(), to.greenF()),
        blend(from.blueF(), to.blueF()),
        from.alphaF() + ((to.alphaF() - from.alphaF()) * clamped));
}

// The panel background for a folder tile is always the same 12% blend of the
// folder's lightened base colour towards the application palette base colour.
// Key includes both so palette theme-switches produce correct new values without
// requiring an explicit cache clear.
QColor cachedPanelBase(const QColor& folderBase, const QColor& /*paletteBase*/)
{
    static QHash<quint64, QRgb> cache;
    const quint64 key = static_cast<quint64>(folderBase.rgba());
    auto it = cache.constFind(key);
    if (it != cache.cend())
        return QColor::fromRgba(*it);

    // Keep the panel tied to the folder hue, but give darker folders a bit more
    // separation so the inner panel reads clearly against the outer body.
    const qreal luminance = 0.2126 * folderBase.redF()
        + 0.7152 * folderBase.greenF()
        + 0.0722 * folderBase.blueF();
    const int lightenFactor = luminance < 0.28 ? 114
        : (luminance < 0.40 ? 110 : 106);
    const QColor result = cachedShade(folderBase, true, lightenFactor);
    cache.insert(key, result.rgba());
    return result;
}

QColor depthAdjustedColor(const QColor& color, int depth, int step, int limit)
{
    const int clampedDepth = std::clamp(depth, 0, limit);
    const int factor = 100 + (clampedDepth * step);
    const qreal luminance = 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
    const bool lightenWithDepth = luminance < 0.32;
    return cachedShade(color, lightenWithDepth, factor);
}

QRegularExpression buildSearchRegex(const QString& pattern)
{
    QString regexPattern;
    regexPattern.reserve(pattern.size() * 2);

    for (const QChar ch : pattern) {
        switch (ch.unicode()) {
        case '*':
            regexPattern += QStringLiteral(".*");
            break;
        case '?':
            regexPattern += QLatin1Char('.');
            break;
        default:
            regexPattern += QRegularExpression::escape(QString(ch));
            break;
        }
    }

    return QRegularExpression(regexPattern, QRegularExpression::CaseInsensitiveOption);
}

QRect expandedDirtyRect(const QRect& rect, const QRect& bounds, int padding)
{
    if (rect.isNull()) {
        return rect;
    }
    return rect.adjusted(-padding, -padding, padding, padding).intersected(bounds);
}

QRectF strokeRectInside(const QRectF& rect, qreal inset)
{
    return QRectF(
        rect.x() + inset,
        rect.y() + inset,
        std::max<qreal>(0.0, rect.width() - 2.0 * inset),
        std::max<qreal>(0.0, rect.height() - 2.0 * inset));
}

QRectF snappedStrokeRectInside(const QRectF& rect, qreal inset, qreal pixelScale)
{
    return snapRectToPixels(strokeRectInside(rect, inset), pixelScale);
}

QRectF logicalRectToPixmapSourceRect(const QPixmap& pixmap, const QRectF& logicalRect)
{
    // QPainter::drawPixmap(target, pixmap, source) interprets sourceRect in the
    // pixmap's own coordinate space, not in logical widget coordinates.
    const qreal dpr = pixmap.devicePixelRatio();
    return QRectF(logicalRect.x() * dpr,
                  logicalRect.y() * dpr,
                  logicalRect.width() * dpr,
                  logicalRect.height() * dpr);
}

QRectF confineRectToBounds(const QRectF& rect, const QRectF& bounds)
{
    // Preserve the source rect size and only shift it back onscreen. Intersecting
    // with the bounds shrinks edge rects and makes the zoom transition over-zoom.
    if (!rect.isValid() || rect.isEmpty() || !bounds.isValid() || bounds.isEmpty()) {
        return rect;
    }

    QRectF confined = rect;
    if (confined.width() > bounds.width()) {
        confined.setWidth(bounds.width());
    }
    if (confined.height() > bounds.height()) {
        confined.setHeight(bounds.height());
    }

    if (confined.left() < bounds.left()) {
        confined.moveLeft(bounds.left());
    }
    if (confined.right() > bounds.right()) {
        confined.moveRight(bounds.right());
    }
    if (confined.top() < bounds.top()) {
        confined.moveTop(bounds.top());
    }
    if (confined.bottom() > bounds.bottom()) {
        confined.moveBottom(bounds.bottom());
    }
    return confined;
}

void fillInnerBorder(QPainter& painter, const QRectF& rect, const QColor& color, qreal width)
{
    if (rect.isEmpty() || width <= 0.0) {
        return;
    }

    const qreal edgeWidth = std::min<qreal>(width, std::min(rect.width(), rect.height()) * 0.5);
    if (edgeWidth <= 0.0) {
        return;
    }

    painter.fillRect(QRectF(rect.left(), rect.top(), rect.width(), edgeWidth), color);
    painter.fillRect(QRectF(rect.left(), rect.bottom() - edgeWidth, rect.width(), edgeWidth), color);
    if (rect.height() > edgeWidth * 2.0) {
        painter.fillRect(QRectF(rect.left(), rect.top() + edgeWidth, edgeWidth,
                                rect.height() - (edgeWidth * 2.0)), color);
        painter.fillRect(QRectF(rect.right() - edgeWidth, rect.top() + edgeWidth, edgeWidth,
                                rect.height() - (edgeWidth * 2.0)), color);
    }
}

void fillOuterRectMinusInner(QPainter& painter, const QRectF& outer, const QRectF& inner,
                             const QColor& color)
{
    if (outer.isEmpty()) {
        return;
    }

    const QRectF clippedInner = outer.intersected(inner);
    if (clippedInner.isEmpty()) {
        painter.fillRect(outer, color);
        return;
    }

    const qreal topHeight = clippedInner.top() - outer.top();
    if (topHeight > 0.0) {
        painter.fillRect(QRectF(outer.left(), outer.top(), outer.width(), topHeight), color);
    }

    const qreal bottomHeight = outer.bottom() - clippedInner.bottom();
    if (bottomHeight > 0.0) {
        painter.fillRect(QRectF(outer.left(), clippedInner.bottom(), outer.width(), bottomHeight), color);
    }

    const qreal middleTop = std::max(outer.top(), clippedInner.top());
    const qreal middleBottom = std::min(outer.bottom(), clippedInner.bottom());
    const qreal middleHeight = middleBottom - middleTop;
    if (middleHeight <= 0.0) {
        return;
    }

    const qreal leftWidth = clippedInner.left() - outer.left();
    if (leftWidth > 0.0) {
        painter.fillRect(QRectF(outer.left(), middleTop, leftWidth, middleHeight), color);
    }

    const qreal rightWidth = outer.right() - clippedInner.right();
    if (rightWidth > 0.0) {
        painter.fillRect(QRectF(clippedInner.right(), middleTop, rightWidth, middleHeight), color);
    }
}

qreal folderDetailOpacity(const QRectF& bounds, const TreemapSettings& settings)
{
    const RevealThresholds thresholds = revealThresholds(settings);
    const qreal detailFadeW = smoothstep(std::clamp(
        (bounds.width()  - (thresholds.detailWidth  - 8.0)) / 8.0, 0.0, 1.0));
    const qreal detailFadeH = smoothstep(std::clamp(
        (bounds.height() - (thresholds.detailHeight - 8.0)) / 8.0, 0.0, 1.0));
    return std::min(detailFadeW, detailFadeH);
}

TileChromeGeometry makeTileChromeGeometry(const QRectF& bounds, const TreemapSettings& settings,
                                          bool snapTile, bool detailedChrome, qreal pixelScale)
{
    TileChromeGeometry g;
    g.tileRect = snapTile ? snapRectToPixels(bounds, pixelScale) : bounds;

    const qreal outlineWidth = snapLengthToPixels(settings.border, pixelScale);
    const qreal outlineInset = outlineWidth * 0.5;
    const qreal innerBorderInset = outlineWidth;
    const qreal folderPadding = snapLengthToPixels(settings.folderPadding, pixelScale);
    const qreal panelInset = innerBorderInset + folderPadding;
    const qreal headerHeight = snapLengthToPixels(settings.headerHeight, pixelScale);

    g.outerBorderRect = snapTile
        ? snappedStrokeRectInside(g.tileRect, outlineInset, pixelScale)
        : strokeRectInside(g.tileRect, outlineInset);
    g.innerFrameRect = snapTile
        ? snappedStrokeRectInside(g.tileRect, innerBorderInset, pixelScale)
        : strokeRectInside(g.tileRect, innerBorderInset);
    if (detailedChrome) {
        const QRectF rawHeaderRect(
            g.innerFrameRect.x(),
            g.innerFrameRect.y(),
            g.innerFrameRect.width(),
            std::min<qreal>(headerHeight, g.innerFrameRect.height()));
        g.headerRect = snapTile ? snapRectToPixels(rawHeaderRect, pixelScale) : rawHeaderRect;
        g.framedHeaderRect = g.headerRect;
    }
    const QRectF rawContentPaintRect = g.tileRect.adjusted(
            panelInset,
            detailedChrome ? (headerHeight + panelInset) : panelInset,
            -panelInset,
            -panelInset);
    g.contentPaintRect = snapTile ? snapRectToPixels(rawContentPaintRect, pixelScale)
                                  : rawContentPaintRect;
    g.contentLayoutRect = detailedChrome
        ? bounds.adjusted(panelInset, headerHeight + panelInset, -panelInset, -panelInset)
        : bounds.adjusted(panelInset, panelInset, -panelInset, -panelInset);
    return g;
}

void drawHeaderLabel(QPainter& painter, const QRectF& rect, const QString& text,
                     const QFont& font, const QFontMetrics& metrics, const QColor& color,
                     qreal pixelScale, Qt::LayoutDirection direction)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0 || text.isEmpty()) {
        return;
    }

    const QRectF snappedRect = snapRectToPixels(rect, pixelScale);
    if (snappedRect.width() <= 0.0 || snappedRect.height() <= 0.0) {
        return;
    }

    painter.setPen(color);
    painter.setFont(font);

    // snappedRect edges are already multiples of 1/DPR. The baseline is further
    // snapped to the device-pixel grid (std::round * pixelScale / pixelScale) — not to
    // logical pixels — so the text rasterizer sees a stable integer device-pixel position
    // during panning rather than a cycling sub-pixel offset from integer ascent metrics.
    const qreal baselineY = std::round(
        (snappedRect.center().y() + ((metrics.ascent() - metrics.descent()) * 0.5)) * pixelScale)
        / pixelScale;
    painter.drawText(QPointF(leadingTextX(snappedRect, text, metrics, direction), baselineY), text);
}

void drawFileLabel(QPainter& painter, const QRectF& rect, const QString& text,
                   const QFont& font, const QFontMetrics& metrics, const QColor& color,
                   qreal pixelScale, Qt::LayoutDirection direction)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0 || text.isEmpty()) {
        return;
    }

    const QRectF snappedRect = snapRectToPixels(rect, pixelScale);
    if (snappedRect.width() <= 0.0 || snappedRect.height() <= 0.0) {
        return;
    }

    painter.setPen(color);
    painter.setFont(font);

    const QStringList lines = text.split(u'\n');
    if (lines.isEmpty()) {
        return;
    }

    const auto snapBaseline = [pixelScale](qreal y) {
        return std::round(y * pixelScale) / pixelScale;
    };
    qreal baselineY = snapBaseline(snappedRect.top() + metrics.ascent());
    const qreal maxBaselineY = snapBaseline(snappedRect.bottom() - metrics.descent());
    for (const QString& line : lines) {
        if (line.isEmpty() || baselineY > maxBaselineY) {
            break;
        }
        painter.drawText(QPointF(leadingTextX(snappedRect, line, metrics, direction), baselineY), line);
        baselineY = snapBaseline(baselineY + metrics.lineSpacing());
    }
}

void drawFileLabelLine(QPainter& painter, const QRectF& rect, const QString& text, int lineIndex,
                       const QFont& font, const QFontMetrics& metrics, const QColor& color,
                       qreal pixelScale, Qt::LayoutDirection direction)
{
    if (rect.width() <= 0.0 || rect.height() <= 0.0 || text.isEmpty() || lineIndex < 0) {
        return;
    }

    const QRectF snappedRect = snapRectToPixels(rect, pixelScale);
    if (snappedRect.width() <= 0.0 || snappedRect.height() <= 0.0) {
        return;
    }

    painter.setPen(color);
    painter.setFont(font);

    const auto snapBaseline = [pixelScale](qreal y) {
        return std::round(y * pixelScale) / pixelScale;
    };
    const qreal baselineY = snapBaseline(
        snappedRect.top() + metrics.ascent() + (lineIndex * metrics.lineSpacing()));
    const qreal maxBaselineY = snapBaseline(snappedRect.bottom() - metrics.descent());
    const qreal overflowTolerance = (lineIndex == 0) ? (2.0 / std::max<qreal>(1.0, pixelScale)) : 0.0;
    if (baselineY > maxBaselineY + overflowTolerance) {
        return;
    }

    painter.drawText(QPointF(leadingTextX(snappedRect, text, metrics, direction), baselineY), text);
}

void paintTinyNodeFill(QPainter& painter, const QRectF& rect, const QColor& fillColor,
                       qreal pixelScale, qreal opacity)
{
    if (rect.isEmpty()) {
        return;
    }

    const QRectF coveredRect = snapRectToPixels(rect, pixelScale);
    if (!coveredRect.isEmpty()) {
        painter.save();
        painter.setOpacity(painter.opacity() * std::clamp(opacity, 0.0, 1.0));
        painter.fillRect(coveredRect, fillColor);
        painter.restore();
    }
}

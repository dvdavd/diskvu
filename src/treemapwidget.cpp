// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "treemapwidget.h"
#include "treemap_drawing.h"
#include "colorutils.h"
#include <QtConcurrent/QtConcurrent>
#include <QApplication>
#include <QCursor>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QHBoxLayout>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QPainter>
#include <QPropertyAnimation>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QStyle>
#include <QTouchEvent>
#include <QWheelEvent>
#include <QDir>
#include <QLocale>
#include <QEasingCurve>
#include <QLineF>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QEvent>
#include <algorithm>
#include <cmath>
#include <functional>

class UsageBarWidget final : public QWidget {
public:
    explicit UsageBarWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedHeight(7);
    }

    void setPercents(double parentPercent, double rootPercent)
    {
        const double clampedParent = std::clamp(parentPercent, 0.0, 100.0);
        const double clampedRoot = std::clamp(rootPercent, 0.0, clampedParent);
        if (qFuzzyCompare(m_parentPercent, clampedParent) && qFuzzyCompare(m_rootPercent, clampedRoot)) {
            return;
        }
        m_parentPercent = clampedParent;
        m_rootPercent = clampedRoot;
        update();
    }

    QSize sizeHint() const override
    {
        return {176, 8};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);

        const QRectF bounds = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        if (bounds.width() <= 1.0 || bounds.height() <= 1.0) {
            return;
        }

        const QPalette pal = palette();
        QColor track = pal.color(QPalette::Mid);
        track.setAlphaF(0.22);
        QColor root = QColor(QStringLiteral("#f08a24"));
        QColor parent = QColor(QStringLiteral("#4f8cff"));

        painter.setPen(Qt::NoPen);
        painter.setBrush(track);
        painter.drawRect(bounds);

        const qreal rootWidth = bounds.width() * (m_rootPercent / 100.0);
        const qreal parentWidth = bounds.width() * (m_parentPercent / 100.0);
        const bool rtl = layoutDirection() == Qt::RightToLeft;
        const auto segmentRect = [&](qreal startOffset, qreal width) {
            return rtl
                ? QRectF(bounds.right() - startOffset - width, bounds.top(), width, bounds.height())
                : QRectF(bounds.left() + startOffset, bounds.top(), width, bounds.height());
        };
        if (parentWidth > 0.0) {
            painter.save();
            painter.setClipRect(bounds);
            if (rootWidth > 0.0) {
                painter.setBrush(root);
                painter.drawRect(segmentRect(0.0, rootWidth));
            }
            if (parentWidth > rootWidth) {
                painter.setBrush(parent);
                painter.drawRect(segmentRect(rootWidth, parentWidth - rootWidth));
            }
            painter.restore();
        }
    }

private:
    double m_parentPercent = 0.0;
    double m_rootPercent = 0.0;
};

namespace {

size_t nextUtf8CodePointLength(const std::string_view s, size_t pos)
{
    if (pos >= s.size()) {
        return 0;
    }

    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if ((c & 0x80u) == 0u) return 1;
    if ((c & 0xE0u) == 0xC0u) return (pos + 1 < s.size()) ? 2 : 1;
    if ((c & 0xF0u) == 0xE0u) return (pos + 2 < s.size()) ? 3 : 1;
    if ((c & 0xF8u) == 0xF0u) return (pos + 3 < s.size()) ? 4 : 1;
    return 1;
}

bool wildcardUtf8Match(std::string_view pattern, std::string_view text)
{
    size_t patternPos = 0;
    size_t textPos = 0;
    size_t starPatternPos = std::string_view::npos;
    size_t starTextPos = std::string_view::npos;

    while (textPos < text.size()) {
        if (patternPos < pattern.size()) {
            const char token = pattern[patternPos];
            if (token == '*') {
                starPatternPos = patternPos++;
                starTextPos = textPos;
                continue;
            }
            if (token == '?') {
                const size_t codePointLen = nextUtf8CodePointLength(text, textPos);
                if (codePointLen == 0) {
                    return false;
                }
                ++patternPos;
                textPos += codePointLen;
                continue;
            }
            if (text[textPos] == token) {
                ++patternPos;
                ++textPos;
                continue;
            }
        }

        if (starPatternPos != std::string_view::npos) {
            patternPos = starPatternPos + 1;
            const size_t codePointLen = nextUtf8CodePointLength(text, starTextPos);
            if (codePointLen == 0) {
                return false;
            }
            starTextPos += codePointLen;
            textPos = starTextPos;
            continue;
        }

        return false;
    }

    while (patternPos < pattern.size() && pattern[patternPos] == '*') {
        ++patternPos;
    }
    return patternPos == pattern.size();
}

} // namespace

class TooltipPanelWidget final : public QWidget {
public:
    explicit TooltipPanelWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAutoFillBackground(false);
    }

    void setPanelColors(const QColor& background, const QColor& border)
    {
        m_background = background;
        m_border = border;
        update();
    }

    void setCompactMode(bool compact)
    {
        if (m_compact == compact) {
            return;
        }
        m_compact = compact;
        updateGeometry();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF panelRect = rect().adjusted(1.0, 1.0, -1.0, -1.0);
        painter.setBrush(m_background);
        painter.setPen(QPen(m_border, 1.0));
        painter.drawRoundedRect(panelRect, 3.0, 3.0);
    }

private:
    QColor m_background = Qt::white;
    QColor m_border = Qt::black;
    bool m_compact = false;
};

namespace {

constexpr quint8 kSearchSelfMatch = 0x1;
constexpr quint8 kSearchSubtreeMatch = 0x2;
constexpr quint8 kTypeSelfMatch = 0x1;
constexpr quint8 kTypeSubtreeMatch = 0x2;
constexpr qreal kCameraMinScale = 1.0;
constexpr qreal kZoomedInThreshold = kCameraMinScale + 0.001;
constexpr int kMaxCacheEntries = 60000;
constexpr int kResizeDebounceMs      = 50;
constexpr int kHoverAnimationMs      = 240;
constexpr int kPressHoldMs           = 450;
constexpr qreal kPressHoldMoveThreshold = 12.0;
constexpr int kTouchTapSuppressMs    = 180;
constexpr int kWheelZoomCoalesceMs    = 8;
constexpr int kTrackpadBackReadyMs    = 400;
constexpr int kOverlayScrollBarThickness = 12;
constexpr int kOverlayScrollBarInset = 4;
constexpr int kOverlayScrollBarEndMargin = 10;
constexpr int kOverlayScrollBarStateAnimationMs = 140;
constexpr int kOverlayScrollBarFadeAnimationMs = 170;
constexpr qreal kRevealWidthBucketPx = 3.0;
constexpr qreal kRevealHeightBucketPx = 2.0;
constexpr qreal kRevealMetricHysteresisPx = 3.0;
constexpr QChar kLeftToRightIsolate(0x2066);
constexpr QChar kPopDirectionalIsolate(0x2069);

QString tooltipDisplayPath(const QString& path)
{
    const QString nativePath = QDir::toNativeSeparators(path);
    return QString(kLeftToRightIsolate) + nativePath + QString(kPopDirectionalIsolate);
}

QColor opaqueCompositeColor(const QColor& background, const QColor& foreground)
{
    const qreal alpha = std::clamp<qreal>(foreground.alphaF(), 0.0, 1.0);
    if (alpha <= 0.0) {
        return background;
    }
    if (alpha >= 1.0) {
        return foreground.toRgb();
    }

    QColor opaqueForeground = foreground.toRgb();
    opaqueForeground.setAlphaF(1.0);
    QColor composite = blendColors(background, opaqueForeground, alpha);
    composite.setAlphaF(1.0);
    return composite;
}

qreal stableLabelMetric(qreal value, qreal bucket)
{
    if (bucket <= 0.0) {
        return value;
    }
    return std::round(value / bucket) * bucket;
}

bool shouldUseLightBorder(const QColor& baseColor, int borderStyle)
{
    switch (borderStyle) {
    case TreemapSettings::DarkenBorder:
        return false;
    case TreemapSettings::LightenBorder:
        return true;
    default:
        break;
    }

    const qreal luminance = 0.2126 * baseColor.redF()
        + 0.7152 * baseColor.greenF()
        + 0.0722 * baseColor.blueF();
    return luminance < 0.45;
}

QColor lerpColor(const QColor& from, const QColor& to, qreal t)
{
    const qreal clamped = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(
        from.redF() + ((to.redF() - from.redF()) * clamped),
        from.greenF() + ((to.greenF() - from.greenF()) * clamped),
        from.blueF() + ((to.blueF() - from.blueF()) * clamped),
        from.alphaF() + ((to.alphaF() - from.alphaF()) * clamped));
}

QString treemapScrollBarStyleSheet(const QPalette& palette, qreal expandProgress)
{
    const bool darkChrome = palette.color(QPalette::Window).lightnessF() < 0.5;
    const qreal clamped = std::clamp(expandProgress, 0.0, 1.0);
    QColor trackBase = palette.color(QPalette::WindowText);
    trackBase.setAlpha(darkChrome ? 24 : 16);
    QColor track = trackBase;
    track.setAlphaF(track.alphaF() * clamped);
    QColor handleIdle = palette.color(QPalette::WindowText);
    handleIdle.setAlpha(darkChrome ? 90 : 72);
    QColor handleHover = palette.color(QPalette::Highlight);
    handleHover.setAlpha(darkChrome ? 180 : 150);
    QColor handle = lerpColor(handleIdle, handleHover, clamped);
    QColor handlePressed = handleHover.lighter(darkChrome ? 124 : 116);
    const auto rgbaCss = [](const QColor& color) {
        return QStringLiteral("rgba(%1, %2, %3, %4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(QString::number(color.alphaF(), 'f', 3));
    };
    const int collapsedInset = 4;
    const int expandedInset = 1;
    const int inset = std::max(0, static_cast<int>(std::round(
        collapsedInset + ((expandedInset - collapsedInset) * clamped))));
    const int radius = std::max(1, (kOverlayScrollBarThickness / 2) - inset);

    return QStringLiteral(
        "QScrollBar:vertical {"
        "  background: transparent;"
        "  width: %5px;"
        "  margin: 0px;"
        "}"
        "QScrollBar:horizontal {"
        "  background: transparent;"
        "  height: %5px;"
        "  margin: 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  min-height: 36px;"
        "  border: none;"
        "  border-radius: %6px;"
        "  margin: %7px;"
        "  background: %1;"
        "}"
        "QScrollBar::handle:horizontal {"
        "  min-width: 36px;"
        "  border: none;"
        "  border-radius: %6px;"
        "  margin: %7px;"
        "  background: %1;"
        "}"
        "QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover {"
        "  background: %2;"
        "}"
        "QScrollBar::handle:vertical:pressed, QScrollBar::handle:horizontal:pressed {"
        "  background: %3;"
        "}"
        "QScrollBar::add-line, QScrollBar::sub-line {"
        "  width: 0px;"
        "  height: 0px;"
        "  border: none;"
        "  background: transparent;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical,"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
        "  background: %4;"
        "  border: none;"
        "  border-radius: %6px;"
        "}")
        .arg(rgbaCss(handle),
             rgbaCss(handle),
             rgbaCss(handlePressed),
             rgbaCss(track),
             QString::number(kOverlayScrollBarThickness),
             QString::number(radius),
             QString::number(inset));
}

}

TreemapWidget::TreemapWidget(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_currentRootLayoutAspectRatio = computeCurrentRootLayoutAspectRatio();
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    viewport()->setAttribute(Qt::WA_AcceptTouchEvents);
    m_overlayHScrollBar = new QScrollBar(Qt::Horizontal, viewport());
    m_overlayVScrollBar = new QScrollBar(Qt::Vertical, viewport());
    m_overlayHScrollBar->hide();
    m_overlayVScrollBar->hide();
    m_overlayHScrollBar->setFocusPolicy(Qt::NoFocus);
    m_overlayVScrollBar->setFocusPolicy(Qt::NoFocus);
    m_overlayHScrollBar->setAttribute(Qt::WA_Hover, true);
    m_overlayVScrollBar->setAttribute(Qt::WA_Hover, true);
    m_overlayHScrollBar->installEventFilter(this);
    m_overlayVScrollBar->installEventFilter(this);
    m_overlayHOpacityEffect = new QGraphicsOpacityEffect(m_overlayHScrollBar);
    m_overlayVOpacityEffect = new QGraphicsOpacityEffect(m_overlayVScrollBar);
    m_overlayHOpacityEffect->setOpacity(0.0);
    m_overlayVOpacityEffect->setOpacity(0.0);
    m_overlayHScrollBar->setGraphicsEffect(m_overlayHOpacityEffect);
    m_overlayVScrollBar->setGraphicsEffect(m_overlayVOpacityEffect);
    m_overlayHOpacityAnimation.setTargetObject(m_overlayHOpacityEffect);
    m_overlayHOpacityAnimation.setPropertyName("opacity");
    m_overlayHOpacityAnimation.setDuration(kOverlayScrollBarFadeAnimationMs);
    m_overlayHOpacityAnimation.setEasingCurve(QEasingCurve::OutCubic);
    m_overlayVOpacityAnimation.setTargetObject(m_overlayVOpacityEffect);
    m_overlayVOpacityAnimation.setPropertyName("opacity");
    m_overlayVOpacityAnimation.setDuration(kOverlayScrollBarFadeAnimationMs);
    m_overlayVOpacityAnimation.setEasingCurve(QEasingCurve::OutCubic);
    m_overlayHStateAnimation.setDuration(kOverlayScrollBarStateAnimationMs);
    m_overlayHStateAnimation.setEasingCurve(QEasingCurve::OutCubic);
    m_overlayVStateAnimation.setDuration(kOverlayScrollBarStateAnimationMs);
    m_overlayVStateAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_overlayHStateAnimation, &QVariantAnimation::valueChanged, this, [this]() {
        m_overlayHExpandProgress = m_overlayHStateAnimation.currentValue().toReal();
        updateScrollBarOverlayStyle();
    });
    connect(&m_overlayVStateAnimation, &QVariantAnimation::valueChanged, this, [this]() {
        m_overlayVExpandProgress = m_overlayVStateAnimation.currentValue().toReal();
        updateScrollBarOverlayStyle();
    });
    connect(&m_overlayHOpacityAnimation, &QPropertyAnimation::finished, this, [this]() {
        if (!m_overlayHShouldBeVisible && m_overlayHOpacityEffect->opacity() <= 0.01) {
            m_overlayHScrollBar->hide();
        }
    });
    connect(&m_overlayVOpacityAnimation, &QPropertyAnimation::finished, this, [this]() {
        if (!m_overlayVShouldBeVisible && m_overlayVOpacityEffect->opacity() <= 0.01) {
            m_overlayVScrollBar->hide();
        }
    });
    updateScrollBarOverlayStyle();
    connect(m_overlayHScrollBar, &QScrollBar::valueChanged, this, [this]() {
        applyScrollBarCameraPosition();
    });
    connect(m_overlayVScrollBar, &QScrollBar::valueChanged, this, [this]() {
        applyScrollBarCameraPosition();
    });
    connect(m_overlayHScrollBar, &QScrollBar::sliderPressed, this, [this]() {
        setOverlayScrollBarExpanded(m_overlayHScrollBar, true);
    });
    connect(m_overlayHScrollBar, &QScrollBar::sliderReleased, this, [this]() {
        setOverlayScrollBarExpanded(m_overlayHScrollBar, m_overlayHScrollBar->underMouse());
    });
    connect(m_overlayVScrollBar, &QScrollBar::sliderPressed, this, [this]() {
        setOverlayScrollBarExpanded(m_overlayVScrollBar, true);
    });
    connect(m_overlayVScrollBar, &QScrollBar::sliderReleased, this, [this]() {
        setOverlayScrollBarExpanded(m_overlayVScrollBar, m_overlayVScrollBar->underMouse());
    });

    m_hoverTooltipWidget = new TooltipPanelWidget(viewport());
    m_hoverTooltipWidget->hide();
    static constexpr int kTooltipPadLeft = 10;
    static constexpr int kTooltipPadTop = 8;
    static constexpr int kTooltipPadRight = 12;
    static constexpr int kTooltipPadBottom = 8;
    static constexpr int kTooltipIconSize = 32;
    static constexpr int kTooltipColumnGap = 8;
    static constexpr int kTooltipTextRightInset = 4;
    static constexpr int kTooltipTextLeft = kTooltipPadLeft + kTooltipIconSize + kTooltipColumnGap;

    auto* outerLayout = new QVBoxLayout(m_hoverTooltipWidget);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(6);
    m_hoverTooltipTopRowLayout = new QHBoxLayout();
    m_hoverTooltipTopRowLayout->setContentsMargins(kTooltipPadLeft, kTooltipPadTop, kTooltipPadRight, 0);
    m_hoverTooltipTopRowLayout->setSpacing(kTooltipColumnGap);
    m_hoverTooltipIconLabel = new QLabel(m_hoverTooltipWidget);
    m_hoverTooltipIconLabel->setFixedSize(kTooltipIconSize, kTooltipIconSize);
    m_hoverTooltipIconLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_hoverTooltipTopTextColumnLayout = new QVBoxLayout();
    m_hoverTooltipTopTextColumnLayout->setContentsMargins(0, 0, kTooltipTextRightInset, 0);
    m_hoverTooltipTopTextColumnLayout->setSpacing(0);
    m_hoverTooltipTextLabel = new QLabel(m_hoverTooltipWidget);
    m_hoverTooltipTextLabel->setTextFormat(Qt::RichText);
    m_hoverTooltipTextLabel->setWordWrap(false);
    m_hoverTooltipTextLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_hoverTooltipTextLabel->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    m_hoverTooltipSeparator = new QWidget(m_hoverTooltipWidget);
    m_hoverTooltipSeparator->setFixedHeight(1);
    m_hoverTooltipDetailsWidget = new QWidget(m_hoverTooltipWidget);
    auto* bottomColumn = new QVBoxLayout(m_hoverTooltipDetailsWidget);
    bottomColumn->setContentsMargins(kTooltipTextLeft, 0, kTooltipPadRight + kTooltipTextRightInset, kTooltipPadBottom);
    bottomColumn->setSpacing(4);
    m_hoverTooltipSizeLabel = new QLabel(m_hoverTooltipDetailsWidget);
    m_hoverTooltipSizeLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_hoverTooltipSizeLabel->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    m_hoverTooltipBarWidget = new UsageBarWidget(m_hoverTooltipDetailsWidget);
    m_hoverTooltipStatsLabel = new QLabel(m_hoverTooltipDetailsWidget);
    m_hoverTooltipStatsLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    m_hoverTooltipStatsLabel->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    m_hoverTooltipTopRowLayout->addWidget(m_hoverTooltipIconLabel, 0, Qt::AlignTop);
    m_hoverTooltipTopTextColumnLayout->addWidget(m_hoverTooltipTextLabel);
    m_hoverTooltipTopRowLayout->addLayout(m_hoverTooltipTopTextColumnLayout, 1);
    bottomColumn->addWidget(m_hoverTooltipSizeLabel);
    bottomColumn->addWidget(m_hoverTooltipBarWidget);
    bottomColumn->addWidget(m_hoverTooltipStatsLabel);
    outerLayout->addLayout(m_hoverTooltipTopRowLayout);
    outerLayout->addWidget(m_hoverTooltipSeparator);
    outerLayout->addWidget(m_hoverTooltipDetailsWidget);
    updateOwnedTooltipLayoutDirection();
    updateOwnedTooltipStyle();
    m_hoverTooltipOpacity = new QGraphicsOpacityEffect(m_hoverTooltipWidget);
    m_hoverTooltipOpacity->setOpacity(0.0);
    m_hoverTooltipWidget->setGraphicsEffect(m_hoverTooltipOpacity);
    m_hoverTooltipFade = new QPropertyAnimation(m_hoverTooltipOpacity, "opacity", this);
    m_hoverTooltipFade->setDuration(160);
    m_hoverTooltipFade->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_hoverTooltipFade, &QPropertyAnimation::finished, this, [this]() {
        if (m_hoverTooltipOpacity && m_hoverTooltipOpacity->opacity() <= 0.01 && m_hoverTooltipWidget) {
            m_hoverTooltipWidget->hide();
        }
    });

    m_penBorder.setCosmetic(true);
    m_penFileBorder.setCosmetic(true);
    m_penContent.setCosmetic(true);
    m_penContentHov.setCosmetic(true);

    m_font.setPointSize(8);
    m_fm = QFontMetrics(m_font);
    m_headerFont = m_font;
    m_headerFm = QFontMetrics(m_headerFont);
    m_fileFont = m_font;
    m_fileFm = QFontMetrics(m_fileFont);

    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(kResizeDebounceMs);
    connect(&m_resizeTimer, &QTimer::timeout, this, &TreemapWidget::relayout);

    m_hoverAnimation.setDuration(kHoverAnimationMs);
    m_hoverAnimation.setStartValue(0.0);
    m_hoverAnimation.setEndValue(1.0);
    m_hoverAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_hoverAnimation, &QVariantAnimation::valueChanged, this, [this]() {
        m_hoverBlend = m_hoverAnimation.currentValue().toReal();
        viewport()->update();
    });
    connect(&m_hoverAnimation, &QVariantAnimation::finished, this, [this]() {
        m_hoverBlend = 1.0;
        m_previousHovered = nullptr;
        m_previousHoveredRect = QRectF();
        viewport()->update();
    });

    m_pressHoldTimer.setSingleShot(true);
    m_pressHoldTimer.setInterval(kPressHoldMs);
    connect(&m_pressHoldTimer, &QTimer::timeout, this, &TreemapWidget::triggerPressHoldContextMenu);

    m_touchTapSuppressTimer.setSingleShot(true);
    m_touchTapSuppressTimer.setInterval(kTouchTapSuppressMs);
    connect(&m_touchTapSuppressTimer, &QTimer::timeout, this, [this]() {
        m_touchSuppressTap = false;
    });

    m_wheelZoomCoalesceTimer.setSingleShot(true);
    m_wheelZoomCoalesceTimer.setInterval(kWheelZoomCoalesceMs);
    connect(&m_wheelZoomCoalesceTimer, &QTimer::timeout,
            this, &TreemapWidget::applyPendingWheelZoom);

    m_trackpadBackReadyTimer.setSingleShot(true);
    m_trackpadBackReadyTimer.setInterval(kTrackpadBackReadyMs);
    connect(&m_trackpadBackReadyTimer, &QTimer::timeout,
            this, [this] { m_trackpadBackReady = true; });

    m_zoomAnimation.setDuration(m_settings.zoomDurationMs);
    m_zoomAnimation.setStartValue(0.0);
    m_zoomAnimation.setEndValue(1.0);
    m_zoomAnimation.setEasingCurve(QEasingCurve::InOutCubic);
    connect(&m_zoomAnimation, &QVariantAnimation::valueChanged, this, [this]() {
        refreshHoverUnderPointer();
        viewport()->update();
    });
    connect(&m_zoomAnimation, &QVariantAnimation::finished, this, [this]() {
        m_previousFrame = QPixmap();
        m_nextFrame = QPixmap();
        m_zoomSourceRect = QRectF();
        m_zoomCrossfadeOnly = false;
        refreshHoverUnderPointer();
        viewport()->update();
    });

    m_layoutAnimation.setDuration(m_settings.layoutDurationMs);
    m_layoutAnimation.setStartValue(0.0);
    m_layoutAnimation.setEndValue(1.0);
    m_layoutAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_layoutAnimation, &QVariantAnimation::valueChanged, this, [this]() {
        viewport()->update();
    });
    connect(&m_layoutAnimation, &QVariantAnimation::finished, this, [this]() {
        m_layoutPreviousFrame = QPixmap();
        m_layoutNextFrame = QPixmap();
        viewport()->update();
    });

    m_cameraAnimation.setDuration(m_settings.cameraDurationMs);
    m_cameraAnimation.setStartValue(0.0);
    m_cameraAnimation.setEndValue(1.0);
    m_cameraAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_cameraAnimation, &QVariantAnimation::valueChanged, this, [this]() {
        const qreal t = m_cameraAnimation.currentValue().toReal();
        m_cameraScale = m_cameraStartScale + ((m_cameraTargetScale - m_cameraStartScale) * t);
        if (m_cameraUseFocusAnchor) {
            const QPointF anchoredOrigin(
                m_cameraFocusScenePos.x() - (m_cameraFocusScreenPos.x() / m_cameraScale),
                m_cameraFocusScenePos.y() - (m_cameraFocusScreenPos.y() / m_cameraScale));
            m_cameraOrigin = clampCameraOrigin(anchoredOrigin, m_cameraScale);
        } else {
            const QPointF interpolatedOrigin(
                m_cameraStartOrigin.x() + ((m_cameraTargetOrigin.x() - m_cameraStartOrigin.x()) * t),
                m_cameraStartOrigin.y() + ((m_cameraTargetOrigin.y() - m_cameraStartOrigin.y()) * t));
            m_cameraOrigin = clampCameraOrigin(interpolatedOrigin, m_cameraScale);
        }
        syncSemanticDepthToScale();
        syncScrollBars();
        refreshHoverUnderPointer();
        viewport()->update();
    });
    connect(&m_cameraAnimation, &QVariantAnimation::finished, this, [this]() {
        m_cameraPreviousFrame = QPixmap();
        m_cameraNextFrame = QPixmap();
        m_continuousZoomSettleFramesRemaining = 2;
        m_cameraScale = m_cameraTargetScale;
        if (m_cameraUseFocusAnchor) {
            const QPointF anchoredOrigin(
                m_cameraFocusScenePos.x() - (m_cameraFocusScreenPos.x() / m_cameraScale),
                m_cameraFocusScenePos.y() - (m_cameraFocusScreenPos.y() / m_cameraScale));
            m_cameraOrigin = snapCameraOriginToPixelGrid(
                clampCameraOrigin(anchoredOrigin, m_cameraScale), m_cameraScale, pixelScale());
        } else {
            m_cameraOrigin = snapCameraOriginToPixelGrid(
                m_cameraTargetOrigin, m_cameraScale, pixelScale());
        }
        const int desired = m_pendingRestoredViewState
            ? m_pendingRestoredSemanticDepth
            : desiredSemanticDepthForScale(m_cameraScale);
        if (desired != m_activeSemanticDepth) {
            if (desired > m_activeSemanticDepth && m_current && !viewport()->size().isEmpty()
                    && m_layoutAnimation.state() != QAbstractAnimation::Running) {
                m_layoutPreviousFrame = renderSceneToPixmap(m_current);
                m_activeSemanticDepth = desired;
                m_layoutNextFrame = renderSceneToPixmap(m_current);
                m_layoutAnimation.stop();
                m_layoutAnimation.start();
            } else {
                m_activeSemanticDepth = desired;
            }
        }
        if (m_pendingRestoredViewState) {
            m_semanticFocus = m_cameraScale > kZoomedInThreshold ? m_pendingRestoredSemanticFocus : nullptr;
            m_semanticLiveRoot = m_cameraScale > kZoomedInThreshold ? m_pendingRestoredSemanticLiveRoot : nullptr;
            m_pendingRestoredViewState = false;
            m_pendingRestoredSemanticDepth = m_settings.baseVisibleDepth;
            m_pendingRestoredSemanticFocus = nullptr;
            m_pendingRestoredSemanticLiveRoot = nullptr;
        }
        syncScrollBars();
        refreshHoverUnderPointer();
        viewport()->update();
    });

    m_searchCancelToken = std::make_shared<std::atomic<bool>>(false);
    m_searchWatcher = new QFutureWatcher<SearchMatchResult>(this);
    connect(m_searchWatcher, &QFutureWatcher<SearchMatchResult>::finished,
            this, &TreemapWidget::onSearchTaskFinished);
    m_metadataWatcher = new QFutureWatcher<std::shared_ptr<SearchIndex>>(this);
    connect(m_metadataWatcher, &QFutureWatcher<std::shared_ptr<SearchIndex>>::finished,
            this, &TreemapWidget::onMetadataTaskFinished);
    m_fileTypeCancelToken = std::make_shared<std::atomic<bool>>(false);
    m_fileTypeWatcher = new QFutureWatcher<FileTypeMatchResult>(this);
    connect(m_fileTypeWatcher, &QFutureWatcher<FileTypeMatchResult>::finished,
            this, &TreemapWidget::onFileTypeMatchTaskFinished);
}

bool TreemapWidget::viewportEvent(QEvent* event)
{
    switch (event->type()) {
    case QEvent::Resize: {
        auto* resizeEvent = static_cast<QResizeEvent*>(event);
        if (resizeEvent->oldSize() != resizeEvent->size()) {
            const bool animationInFlight = m_cameraAnimation.state() == QAbstractAnimation::Running
                                        || m_zoomAnimation.state() == QAbstractAnimation::Running;
            if (!animationInFlight) {
                m_zoomAnimation.stop();
                m_cameraAnimation.stop();
                m_previousFrame = QPixmap();
                m_nextFrame = QPixmap();
                m_zoomSourceRect = QRectF();
            }
            m_cameraOrigin = snapCameraOriginToPixelGrid(
                clampCameraOrigin(m_cameraOrigin, m_cameraScale), m_cameraScale, pixelScale());
            if (!animationInFlight) {
                m_cameraStartOrigin = m_cameraOrigin;
                m_cameraTargetOrigin = m_cameraOrigin;
            } else if (m_cameraAnimation.state() == QAbstractAnimation::Running) {
                m_cameraTargetOrigin = clampCameraOrigin(m_cameraTargetOrigin, m_cameraTargetScale);
            } else {
                m_cameraStartOrigin = m_cameraOrigin;
                m_cameraTargetOrigin = m_cameraOrigin;
            }
            m_liveSplitCache.clear();
            syncScrollBars();
            viewport()->update();
        }
        break;
    }
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
#ifdef Q_OS_MACOS
        endTouchGesture();
        event->accept();
        return true;
#else
        auto* touchEvent = static_cast<QTouchEvent*>(event);
        if (m_scanInProgress || !m_current) {
            endTouchGesture();
            event->accept();
            return true;
        }

        if (event->type() == QEvent::TouchCancel) {
            endTouchGesture();
            event->accept();
            return true;
        }

        updateTouchGesture(touchEvent->points());
        if (event->type() == QEvent::TouchEnd && touchEvent->points().isEmpty()) {
            endTouchGesture();
        }
        event->accept();
        return true;
#endif
    }
    case QEvent::NativeGesture: {
        auto* gestureEvent = static_cast<QNativeGestureEvent*>(event);
        if (!m_wheelZoomEnabled || m_scanInProgress || !m_current) {
            m_nativeGestureActive = false;
            m_nativeGesturePinching = false;
            m_nativeGesturePendingBackOnEnd = false;
            event->ignore();
            return true;
        }

        switch (gestureEvent->gestureType()) {
        case Qt::BeginNativeGesture:
            m_nativeGestureActive = true;
            m_nativeGesturePinching = false;
            m_nativeGesturePendingBackOnEnd = false;
            cancelPressHold();
            event->accept();
            return true;
        case Qt::EndNativeGesture:
            if (m_nativeGesturePendingBackOnEnd && m_current && m_current->parent) {
                m_nativeGesturePendingBackOnEnd = false;
                m_nativeGestureActive = false;
                m_nativeGesturePinching = false;
                emit backRequested();
            } else {
                m_nativeGestureActive = false;
                m_nativeGesturePinching = false;
                m_nativeGesturePendingBackOnEnd = false;
            }
            event->accept();
            return true;
        case Qt::PanNativeGesture: {
            cancelPressHold();
            clearHoverState();
            stopAnimatedNavigation();

            const QPointF delta = gestureEvent->delta();
            if (!delta.isNull()) {
                panCameraImmediate(QPointF(-delta.x() / m_cameraScale, -delta.y() / m_cameraScale));
            }
            m_nativeGestureActive = true;
            event->accept();
            return true;
        }
        case Qt::ZoomNativeGesture: {
            cancelPressHold();
            clearHoverState();
            stopAnimatedNavigation();

            const QPointF anchorScreenPos = gestureEvent->position();
            if (!m_nativeGesturePinching) {
                m_nativeGestureAnchorScenePos = QPointF(
                    m_cameraOrigin.x() + (anchorScreenPos.x() / m_cameraScale),
                    m_cameraOrigin.y() + (anchorScreenPos.y() / m_cameraScale));
                m_nativeGesturePinchInitialScale = m_cameraScale;
                m_nativeGesturePinching = true;
                m_nativeGestureActive = true;
                if (m_cameraScale > kZoomedInThreshold) {
                    m_semanticFocus = semanticFocusCandidateAt(anchorScreenPos, currentRootViewRect());
                    m_semanticLiveRoot = m_current;
                }
            }

            const qreal zoomFactor = std::max<qreal>(0.01, 1.0 + gestureEvent->value());
            const qreal rawTargetScale = m_cameraScale * zoomFactor;
            const qreal targetScale = std::clamp(rawTargetScale, kCameraMinScale, m_settings.cameraMaxScale);

            if (rawTargetScale < kCameraMinScale
                    && m_nativeGesturePinchInitialScale <= (kCameraMinScale + 0.0001)
                    && m_current && m_current->parent) {
                m_nativeGesturePendingBackOnEnd = true;
            } else if (targetScale > kCameraMinScale + 0.0001) {
                m_nativeGesturePendingBackOnEnd = false;
            }

            zoomCameraImmediate(targetScale, m_nativeGestureAnchorScenePos, anchorScreenPos);
            event->accept();
            return true;
        }
        default:
            event->ignore();
            return true;
        }
    }
    default:
        break;
    }

    return QAbstractScrollArea::viewportEvent(event);
}

void TreemapWidget::scrollContentsBy(int, int)
{
    applyScrollBarCameraPosition();
}

void TreemapWidget::applySettings(const TreemapSettings& settings)
{
    m_settings = settings;
    m_settings.sanitize();
    updateOwnedTooltipStyle();
    m_headerFont = font();
    if (!m_settings.headerFontFamily.isEmpty()) {
        m_headerFont.setFamily(m_settings.headerFontFamily);
    }
    m_headerFont.setPointSize(m_settings.headerFontSize);
    m_headerFont.setBold(m_settings.headerFontBold);
    m_headerFont.setItalic(m_settings.headerFontItalic);
    m_headerFm = QFontMetrics(m_headerFont);
    m_fileFont = font();
    if (!m_settings.fileFontFamily.isEmpty()) {
        m_fileFont.setFamily(m_settings.fileFontFamily);
    }
    m_fileFont.setPointSize(m_settings.fileFontSize);
    m_fileFont.setBold(m_settings.fileFontBold);
    m_fileFont.setItalic(m_settings.fileFontItalic);
    m_fileFm = QFontMetrics(m_fileFont);
    m_zoomAnimation.setDuration(m_settings.zoomDurationMs);
    m_layoutAnimation.setDuration(m_settings.layoutDurationMs);
    m_cameraAnimation.setDuration(m_settings.cameraDurationMs);
    m_liveSplitCache.clear();
    m_stableMetricCache.clear();
    m_headerLabelVisibleCache.clear();
    m_fileLabelVisibleCache.clear();
    m_fileSizeLabelVisibleCache.clear();
    m_activeSemanticDepth = std::clamp(m_activeSemanticDepth,
                                       m_settings.baseVisibleDepth,
                                       m_settings.maxSemanticDepth);
    m_cameraScale = std::clamp(m_cameraScale, kCameraMinScale, m_settings.cameraMaxScale);
    m_cameraOrigin = snapCameraOriginToPixelGrid(
        clampCameraOrigin(m_cameraOrigin, m_cameraScale), m_cameraScale, pixelScale());
    m_cameraStartScale = m_cameraScale;
    m_cameraTargetScale = m_cameraScale;
    m_cameraStartOrigin = m_cameraOrigin;
    m_cameraTargetOrigin = m_cameraOrigin;
    m_currentRootLayoutAspectRatio = computeCurrentRootLayoutAspectRatio();
    relayout();
}

void TreemapWidget::setScanInProgress(bool inProgress)
{
    if (m_scanInProgress == inProgress) {
        return;
    }

    m_scanInProgress = inProgress;
    if (!m_scanInProgress) {
        m_scanPath.clear();
    }
    viewport()->update();
}

void TreemapWidget::setScanPath(const QString& path)
{
    m_scanPath = path;
    if (m_scanInProgress) {
        viewport()->update();
    }
}

void TreemapWidget::setSearchPattern(const QString& pattern)
{
    const QString normalized = pattern.trimmed();
    if (m_searchPattern == normalized) {
        return;
    }

    clearHoverState(false);
    m_searchPattern = normalized;
    m_searchCaseFoldedPattern = m_searchPattern.toCaseFolded();
    m_searchActive = !m_searchPattern.isEmpty() || m_sizeFilterActive;
    m_searchUsesWildcards = !m_searchPattern.isEmpty()
        && (m_searchPattern.contains(QLatin1Char('*')) || m_searchPattern.contains(QLatin1Char('?')));
    rebuildSearchMatches();
    viewport()->update();
}

void TreemapWidget::setHighlightedFileType(const QString& typeLabel)
{
    const QString normalized = typeLabel.trimmed();
    if (m_highlightedFileType == normalized) {
        return;
    }

    clearHoverState(false);
    m_highlightedFileType = normalized;
    rebuildFileTypeMatchesAsync();
    viewport()->update();
}

void TreemapWidget::setPreviewHoveredNode(FileNode* node)
{
    if (node == m_current) {
        node = nullptr;
    }
    if (m_hovered == node && m_previousHovered == nullptr) {
        return;
    }

    m_hoverAnimation.stop();
    m_previousHovered = nullptr;
    m_previousHoveredRect = QRectF();
    m_hovered = node;
    m_hoveredRect = QRectF();
    m_hoveredTooltip.clear();
    m_hoverBlend = node ? 1.0 : 0.0;
    hideOwnedTooltip();
    viewport()->update();
}

void TreemapWidget::zoomCenteredIn()
{
    if (!m_wheelZoomEnabled || m_scanInProgress || !m_current
            || m_zoomAnimation.state() == QAbstractAnimation::Running) {
        return;
    }

    const bool cameraAnimating = m_cameraAnimation.state() == QAbstractAnimation::Running;
    const qreal baseScale = cameraAnimating ? m_cameraTargetScale : m_cameraScale;
    const qreal zoomStepFactor = 1.0 + (m_settings.wheelZoomStepPercent / 100.0);
    const qreal targetScale = std::clamp(baseScale * zoomStepFactor,
                                         kCameraMinScale, m_settings.cameraMaxScale);
    if (qFuzzyCompare(targetScale, baseScale)) {
        return;
    }

    const QPointF center(viewport()->rect().center());
    const QPointF anchorScenePos(
        m_cameraOrigin.x() + (center.x() / m_cameraScale),
        m_cameraOrigin.y() + (center.y() / m_cameraScale));
    const QPointF targetOrigin(
        anchorScenePos.x() - (center.x() / targetScale),
        anchorScenePos.y() - (center.y() / targetScale));

    if (targetScale > kZoomedInThreshold) {
        m_semanticFocus = semanticFocusCandidateAt(center, currentRootViewRect());
        m_semanticLiveRoot = m_current;
    } else {
        m_semanticFocus = nullptr;
        m_semanticLiveRoot = nullptr;
    }

    const QPointF clampedOrigin = snapCameraOriginToPixelGrid(
        clampCameraOrigin(targetOrigin, targetScale), targetScale, pixelScale());
    animateCameraTo(targetScale, clampedOrigin, anchorScenePos, center, true);
}

void TreemapWidget::zoomCenteredOut()
{
    if (!m_wheelZoomEnabled || m_scanInProgress || !m_current
            || m_zoomAnimation.state() == QAbstractAnimation::Running) {
        return;
    }

    const bool cameraAnimating = m_cameraAnimation.state() == QAbstractAnimation::Running;
    const qreal baseScale = cameraAnimating ? m_cameraTargetScale : m_cameraScale;
    const qreal zoomStepFactor = 1.0 + (m_settings.wheelZoomStepPercent / 100.0);
    const qreal targetScale = std::clamp(baseScale / zoomStepFactor,
                                         kCameraMinScale, m_settings.cameraMaxScale);
    if (qFuzzyCompare(targetScale, baseScale)) {
        return;
    }

    const QPointF center(viewport()->rect().center());
    const QPointF anchorScenePos(
        m_cameraOrigin.x() + (center.x() / m_cameraScale),
        m_cameraOrigin.y() + (center.y() / m_cameraScale));
    const QPointF targetOrigin(
        anchorScenePos.x() - (center.x() / targetScale),
        anchorScenePos.y() - (center.y() / targetScale));

    if (targetScale > kZoomedInThreshold) {
        m_semanticLiveRoot = m_current;
    } else {
        m_semanticFocus = nullptr;
        m_semanticLiveRoot = nullptr;
    }

    const QPointF clampedOrigin = snapCameraOriginToPixelGrid(
        clampCameraOrigin(targetOrigin, targetScale), targetScale, pixelScale());
    animateCameraTo(targetScale, clampedOrigin, anchorScenePos, center, true);
}

void TreemapWidget::resetZoomToActualSize()
{
    if (!m_current) {
        return;
    }

    restoreViewState(overviewViewState(m_current));
}


void TreemapWidget::setRoot(FileNode* root, std::shared_ptr<NodeArena> rootArena,
                            bool prepareModel, bool animateLayout)
{
    m_zoomAnimation.stop();
    m_layoutAnimation.stop();
    m_cameraAnimation.stop();
    m_previousFrame = QPixmap();
    m_nextFrame = QPixmap();
    m_zoomSourceRect = QRectF();
    m_layoutPreviousFrame = QPixmap();
    m_layoutNextFrame = QPixmap();
    cancelPendingSearch();
    cancelPendingFileTypeHighlight();

    // Move large per-tree caches out of the widget and let them destruct on a
    // background thread. Destroying them on the UI thread causes a noticeable
    // hitch when a large refreshed tree replaces the previous one.
    auto oldLiveSplitCache = std::move(m_liveSplitCache);
    m_liveSplitCache = {};
    auto oldSizeLabelCache = std::move(m_sizeLabelCache);
    m_sizeLabelCache = {};
    auto oldElidedTextCache = std::move(m_elidedTextCache);
    m_elidedTextCache = {};
    auto oldElidedDisplayWidthCache = std::move(m_elidedDisplayWidthCache);
    m_elidedDisplayWidthCache = {};
    m_stableMetricCache.clear();
    m_headerLabelVisibleCache.clear();
    m_fileLabelVisibleCache.clear();
    m_fileSizeLabelVisibleCache.clear();
    auto oldSearchMatchCache = std::move(m_searchMatchCache);
    m_searchMatchCache = {};
    auto oldSearchIndex = std::move(m_searchIndex);
    m_searchIndex.reset();
    auto oldPendingSearchIndex = std::move(m_pendingSearchIndex);
    m_pendingSearchIndex.reset();
    auto oldFileTypeMatchCache = std::move(m_fileTypeMatchCache);
    m_fileTypeMatchCache = {};
    m_combinedMatchCache = {};
    m_searchReachCache = {};
    auto oldPreviousDirectSearchMatches = std::move(m_previousDirectSearchMatches);
    m_previousDirectSearchMatches = {};
    auto oldRootArena = std::move(m_rootArena);
    (void)QtConcurrent::run(
        [oldLiveSplitCache = std::move(oldLiveSplitCache),
         oldSizeLabelCache = std::move(oldSizeLabelCache),
         oldElidedTextCache = std::move(oldElidedTextCache),
         oldElidedDisplayWidthCache = std::move(oldElidedDisplayWidthCache),
         oldSearchMatchCache = std::move(oldSearchMatchCache),
         oldSearchIndex = std::move(oldSearchIndex),
         oldPendingSearchIndex = std::move(oldPendingSearchIndex),
         oldFileTypeMatchCache = std::move(oldFileTypeMatchCache),
         oldPreviousDirectSearchMatches = std::move(oldPreviousDirectSearchMatches),
         oldRootArena = std::move(oldRootArena)]() mutable {
        });

    m_nodeCount = 0;
    m_previousSearchCaseFoldedPattern.clear();
    m_previousSearchUsesWildcards = false;
    m_previousMinSizeFilter = 0;
    m_previousMaxSizeFilter = 0;
    resetCamera();
    m_activeSemanticDepth = m_settings.baseVisibleDepth;
    m_semanticFocus = nullptr;
    m_semanticLiveRoot = nullptr;

    const bool shouldAnimateLayout = animateLayout && root && m_current
        && root->computePath() == m_current->computePath()
        && !viewport()->size().isEmpty();
    if (shouldAnimateLayout) {
        m_layoutPreviousFrame = renderSceneToPixmap(m_current);
    }

    m_rootArena = std::move(rootArena);
    m_root = root;
    m_current = root;
    m_currentRootLayoutAspectRatio = computeCurrentRootLayoutAspectRatio();
    clearHoverState(false);
    if (prepareModel) {
        sortChildrenBySize(root);
    }
    relayout();
    rebuildSearchMetadataAsync();

    // Capture the new state as a pixmap so paintEvent can do a clean
    // pixmap-to-pixmap crossfade. Without this, the rescan path overlays a
    // live draw of the new layout on top of the old pixmap from T=0, which
    // makes tiles at different positions ghost through each other.
    if (shouldAnimateLayout && !m_layoutPreviousFrame.isNull()) {
        m_layoutNextFrame = renderSceneToPixmap(m_current);
        m_layoutAnimation.stop();
        m_layoutAnimation.start();
    }
}

void TreemapWidget::setCurrentNode(FileNode* node, const QPointF& anchorPos, bool useAnchor)
{
    if (!node)
        return;

    restoreViewState(overviewViewState(node), anchorPos, useAnchor);
}

TreemapWidget::ViewState TreemapWidget::currentViewState() const
{
    ViewState state;
    state.node = m_current;
    state.cameraScale = m_cameraScale;
    state.cameraOrigin = m_cameraOrigin;
    state.semanticDepth = m_activeSemanticDepth;
    state.semanticFocus = m_semanticFocus;
    state.semanticLiveRoot = m_semanticLiveRoot;
    state.currentRootLayoutAspectRatio = m_currentRootLayoutAspectRatio;
    return state;
}

TreemapWidget::ViewState TreemapWidget::overviewViewState(FileNode* node) const
{
    ViewState state;
    state.node = node ? node : m_current;
    state.cameraScale = 1.0;
    state.cameraOrigin = QPointF();
    state.semanticDepth = m_settings.baseVisibleDepth;
    state.semanticFocus = nullptr;
    state.semanticLiveRoot = nullptr;
    state.currentRootLayoutAspectRatio = computeCurrentRootLayoutAspectRatio();
    return state;
}

void TreemapWidget::restoreViewState(const ViewState& state,
                                     const QPointF& anchorPos, bool useAnchor)
{
    if (!state.node) {
        return;
    }

    const qreal targetScale = std::clamp(state.cameraScale,
                                         kCameraMinScale, m_settings.cameraMaxScale);
    const QPointF targetOrigin = clampCameraOrigin(state.cameraOrigin, targetScale);
    const int targetSemanticDepth = std::clamp(state.semanticDepth,
                                               m_settings.baseVisibleDepth,
                                               m_settings.maxSemanticDepth);
    FileNode* targetNode = state.node;

    const bool sameNode = (targetNode == m_current);
    const bool sameScale = std::abs(targetScale - m_cameraScale) < 0.0001;
    const bool sameOrigin = QLineF(targetOrigin, m_cameraOrigin).length() < 0.01;
    const bool sameDepth = (targetSemanticDepth == m_activeSemanticDepth);
    const bool sameFocus = (state.semanticFocus == m_semanticFocus);
    const bool sameLiveRoot = (state.semanticLiveRoot == m_semanticLiveRoot);
    if (sameNode && sameScale && sameOrigin && sameDepth && sameFocus && sameLiveRoot) {
        return;
    }

    FileNode* previousNode = m_current;
    const bool canAnimate = !viewport()->size().isEmpty() && m_current != nullptr;
    QPixmap previousFrame;
    QRectF sourceRect;
    bool zoomIn = false;
    bool hasMappedTransitionRect = false;

    m_layoutAnimation.stop();
    m_cameraAnimation.stop();
    if (canAnimate) {
        previousFrame = renderSceneToPixmap(m_current);
        const QRectF previousViewRect = currentRootViewRect();
        if (findVisibleViewRect(m_current, previousViewRect, targetNode, &sourceRect)) {
            zoomIn = true;
            hasMappedTransitionRect = true;
        }
    }

    m_layoutPreviousFrame = QPixmap();
    m_layoutNextFrame = QPixmap();
    m_pendingRestoredViewState = false;
    m_pendingRestoredSemanticDepth = m_settings.baseVisibleDepth;
    m_pendingRestoredSemanticFocus = nullptr;
    m_pendingRestoredSemanticLiveRoot = nullptr;
    if (!sameNode) {
        m_liveSplitCache.clear();
        resetCamera();
    } else {
        m_zoomAnimation.stop();
        m_previousFrame = QPixmap();
        m_nextFrame = QPixmap();
        m_zoomSourceRect = QRectF();
    }
    if (!sameNode) {
        m_activeSemanticDepth = targetSemanticDepth;
        m_semanticFocus = targetScale > kZoomedInThreshold ? state.semanticFocus : nullptr;
        m_semanticLiveRoot = targetScale > kZoomedInThreshold ? state.semanticLiveRoot : nullptr;
    }

    m_current = state.node;
    if (!sameNode) {
        m_currentRootLayoutAspectRatio = state.currentRootLayoutAspectRatio > 0.0
            ? state.currentRootLayoutAspectRatio
            : computeCurrentRootLayoutAspectRatio();
    }
    clearHoverState(false);
    if (!sameNode) {
        setCameraImmediate(targetScale, targetOrigin);
        relayout();
    } else {
        m_cameraStartScale = m_cameraScale;
        m_cameraStartOrigin = m_cameraOrigin;
        m_cameraTargetScale = targetScale;
        m_cameraTargetOrigin = targetOrigin;
        m_cameraFocusScenePos = QPointF();
        m_cameraFocusScreenPos = QPointF();
        m_cameraUseFocusAnchor = false;
        m_pendingRestoredViewState = true;
        m_pendingRestoredSemanticDepth = targetSemanticDepth;
        m_pendingRestoredSemanticFocus = state.semanticFocus;
        m_pendingRestoredSemanticLiveRoot = state.semanticLiveRoot;
        animateCameraTo(targetScale, targetOrigin);
        return;
    }

    if (canAnimate && previousNode && !sameNode) {
        const QRectF currentViewRect = currentRootViewRect();
        if (findVisibleViewRect(m_current, currentViewRect, previousNode, &sourceRect)) {
            zoomIn = false;
            hasMappedTransitionRect = true;
        }
    }

    if (canAnimate && !sameNode) {
        const bool useCrossfade = !hasMappedTransitionRect;
        const QPointF fallbackAnchor = useAnchor ? anchorPos : m_lastActivationPos;
        if (!useCrossfade && (useAnchor || !sourceRect.isValid()
                || sourceRect.width() < 8.0 || sourceRect.height() < 8.0)) {
            sourceRect = zoomRectForAnchor(sourceRect, fallbackAnchor);
        }
        startZoomAnimation(previousFrame, sourceRect, zoomIn, useCrossfade);
    } else {
        viewport()->update();
    }
}

void TreemapWidget::restoreViewStateImmediate(const ViewState& state)
{
    if (!state.node) {
        return;
    }

    const qreal targetScale = std::clamp(state.cameraScale,
                                         kCameraMinScale, m_settings.cameraMaxScale);
    const QPointF targetOrigin = clampCameraOrigin(state.cameraOrigin, targetScale);
    const int targetSemanticDepth = std::clamp(state.semanticDepth,
                                               m_settings.baseVisibleDepth,
                                               m_settings.maxSemanticDepth);

    m_zoomAnimation.stop();
    m_layoutAnimation.stop();
    m_cameraAnimation.stop();
    m_previousFrame = QPixmap();
    m_nextFrame = QPixmap();
    m_zoomSourceRect = QRectF();
    m_layoutPreviousFrame = QPixmap();
    m_layoutNextFrame = QPixmap();
    m_pendingRestoredViewState = false;
    m_pendingRestoredSemanticDepth = m_settings.baseVisibleDepth;
    m_pendingRestoredSemanticFocus = nullptr;
    m_pendingRestoredSemanticLiveRoot = nullptr;
    m_liveSplitCache.clear();

    m_current = state.node;
    m_currentRootLayoutAspectRatio = state.currentRootLayoutAspectRatio > 0.0
        ? state.currentRootLayoutAspectRatio
        : computeCurrentRootLayoutAspectRatio();
    clearHoverState(false);
    setCameraImmediate(targetScale, targetOrigin);
    m_activeSemanticDepth = targetSemanticDepth;
    m_semanticFocus = targetScale > kZoomedInThreshold ? state.semanticFocus : nullptr;
    m_semanticLiveRoot = targetScale > kZoomedInThreshold ? state.semanticLiveRoot : nullptr;

    relayout();
}

int TreemapWidget::desiredSemanticDepthForScale(qreal scale) const
{
    const qreal clampedScale = std::max<qreal>(1.0, scale);
    const int extraDepth = std::max(
        0,
        static_cast<int>(std::ceil(
            std::log2(clampedScale) * m_settings.depthRevealPerZoomDoubling)));
    return std::min(m_settings.baseVisibleDepth + extraDepth, m_settings.maxSemanticDepth);
}

void TreemapWidget::syncSemanticDepthToScale()
{
    if (m_pendingRestoredViewState) {
        return;
    }

    m_activeSemanticDepth = desiredSemanticDepthForScale(m_cameraScale);
}

void TreemapWidget::clearHoverState(bool notify)
{
    if (!m_hovered && !m_previousHovered && m_hoveredRect.isEmpty() && m_previousHoveredRect.isEmpty()) {
        hideOwnedTooltip();
        return;
    }

    const QRect dirty = expandedDirtyRect(
        m_hoveredRect.united(m_previousHoveredRect).toAlignedRect(), viewport()->rect());
    m_hovered = nullptr;
    m_previousHovered = nullptr;
    m_tooltipTarget = nullptr;
    m_hoveredRect = QRectF();
    m_previousHoveredRect = QRectF();
    m_hoveredTooltip.clear();
    m_activeTooltipIconPath.clear();
    m_hoverAnimation.stop();
    m_hoverBlend = 1.0;
    hideOwnedTooltip();
    if (!dirty.isNull()) {
        viewport()->update(dirty);
    }
    if (notify) {
        emit nodeHovered(nullptr);
    }
}

void TreemapWidget::updateOwnedTooltipStyle()
{
    if (!m_hoverTooltipWidget || !m_hoverTooltipTextLabel || !m_hoverTooltipIconLabel
            || !m_hoverTooltipSeparator || !m_hoverTooltipDetailsWidget || !m_hoverTooltipSizeLabel
            || !m_hoverTooltipBarWidget || !m_hoverTooltipStatsLabel) {
        return;
    }

    const QPalette tooltipTheme = QApplication::palette();
    QColor tooltipBase = tooltipTheme.color(QPalette::ToolTipBase).toRgb();
    tooltipBase.setAlphaF(1.0);
    QColor tooltipText = tooltipTheme.color(QPalette::ToolTipText).toRgb();
    tooltipText.setAlphaF(1.0);

    // On some platforms (notably macOS), QPalette::ToolTipBase may not track
    // the active colour scheme, leaving a light tooltip in dark mode. Detect
    // the mismatch by comparing tooltip-base lightness against the window
    // chrome lightness and fall back to Window/WindowText when they disagree.
    const bool chromeDark = tooltipTheme.color(QPalette::Window).lightnessF() < 0.5;
    if (chromeDark != (tooltipBase.lightnessF() < 0.5)) {
        tooltipBase = tooltipTheme.color(QPalette::Window).toRgb();
        tooltipBase.setAlphaF(1.0);
        tooltipText = tooltipTheme.color(QPalette::WindowText).toRgb();
        tooltipText.setAlphaF(1.0);
    }
    QColor tooltipBorder(
        (tooltipBase.red() * 3 + tooltipText.red()) / 4,
        (tooltipBase.green() * 3 + tooltipText.green()) / 4,
        (tooltipBase.blue() * 3 + tooltipText.blue()) / 4);
    tooltipBorder.setAlphaF(tooltipBase.lightnessF() < 0.5 ? 0.85 : 0.55);

    const auto cssRgba = [](const QColor& color) {
        return QStringLiteral("rgba(%1, %2, %3, %4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(color.alpha());
    };

    m_hoverTooltipWidget->setPanelColors(tooltipBase, tooltipBorder);
    m_hoverTooltipWidget->setStyleSheet(QStringLiteral(
        "QLabel {"
        " background: transparent;"
        " color: %1;"
        " border: none;"
        " }")
        .arg(cssRgba(tooltipText)));
    m_hoverTooltipSeparator->setStyleSheet(QStringLiteral(
        "background:%1; border:none;").arg(cssRgba(tooltipBorder)));
    QPalette labelPalette = m_hoverTooltipStatsLabel->palette();
    labelPalette.setColor(QPalette::WindowText, tooltipText);
    m_hoverTooltipSizeLabel->setPalette(labelPalette);
    m_hoverTooltipStatsLabel->setPalette(labelPalette);
    m_hoverTooltipBarWidget->setPalette(tooltipTheme);
    m_hoverTooltipWidget->update();
}

void TreemapWidget::updateOwnedTooltipLayoutDirection()
{
    if (!m_hoverTooltipWidget || !m_hoverTooltipTopRowLayout || !m_hoverTooltipTopTextColumnLayout
            || !m_hoverTooltipDetailsWidget || !m_hoverTooltipTextLabel
            || !m_hoverTooltipSizeLabel || !m_hoverTooltipStatsLabel) {
        return;
    }

    static constexpr int kTooltipPadLeft = 10;
    static constexpr int kTooltipPadTop = 8;
    static constexpr int kTooltipPadRight = 12;
    static constexpr int kTooltipPadBottom = 8;
    static constexpr int kTooltipIconSize = 32;
    static constexpr int kTooltipColumnGap = 8;
    static constexpr int kTooltipTextRightInset = 4;
    static constexpr int kTooltipTextLeft = kTooltipPadLeft + kTooltipIconSize + kTooltipColumnGap;
    static constexpr int kTooltipTextRight = kTooltipPadRight + kTooltipIconSize + kTooltipColumnGap;

    const Qt::LayoutDirection direction = layoutDirection();
    const bool rtl = direction == Qt::RightToLeft;

    m_hoverTooltipWidget->setLayoutDirection(direction);
    m_hoverTooltipDetailsWidget->setLayoutDirection(direction);
    m_hoverTooltipTextLabel->setLayoutDirection(direction);
    m_hoverTooltipSizeLabel->setLayoutDirection(direction);
    m_hoverTooltipStatsLabel->setLayoutDirection(direction);
    m_hoverTooltipBarWidget->setLayoutDirection(direction);
    m_hoverTooltipTopRowLayout->setContentsMargins(kTooltipPadLeft, kTooltipPadTop, kTooltipPadRight, 0);
    m_hoverTooltipTopTextColumnLayout->setContentsMargins(
        rtl ? kTooltipTextRightInset : 0,
        0,
        rtl ? 0 : kTooltipTextRightInset,
        0);

    if (auto* bottomColumn = qobject_cast<QVBoxLayout*>(m_hoverTooltipDetailsWidget->layout())) {
        bottomColumn->setContentsMargins(
            rtl ? kTooltipPadLeft + kTooltipTextRightInset : kTooltipTextLeft,
            0,
            rtl ? kTooltipTextRight : kTooltipPadRight + kTooltipTextRightInset,
            kTooltipPadBottom);
    }

    const Qt::Alignment textAlignment = (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignTop;
    m_hoverTooltipTextLabel->setAlignment(textAlignment);
    m_hoverTooltipSizeLabel->setAlignment(textAlignment);
    m_hoverTooltipStatsLabel->setAlignment(textAlignment);
}

QIcon TreemapWidget::fallbackTooltipIcon(bool isDirectory) const
{
    return style()->standardIcon(isDirectory ? QStyle::SP_DirIcon : QStyle::SP_FileIcon);
}

void TreemapWidget::requestTooltipIcon(const QString& path, bool isDirectory)
{
    if (path.isEmpty()) {
        return;
    }

    if (m_tooltipIconCache.contains(path)) {
        return;
    }

    m_pendingTooltipIconPath = path;
    m_pendingTooltipIconIsDirectory = isDirectory;
    if (m_tooltipIconLoadQueued) {
        return;
    }

    m_tooltipIconLoadQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_tooltipIconLoadQueued = false;
        const QString queuedPath = m_pendingTooltipIconPath;
        const bool queuedIsDirectory = m_pendingTooltipIconIsDirectory;
        m_pendingTooltipIconPath.clear();
        m_pendingTooltipIconIsDirectory = false;
        if (queuedPath.isEmpty() || m_tooltipIconCache.contains(queuedPath)) {
            return;
        }

        QFileIconProvider iconProvider;
        QIcon icon = iconProvider.icon(QFileInfo(queuedPath));
        if (icon.isNull()) {
            icon = fallbackTooltipIcon(queuedIsDirectory);
        }
        if (!icon.isNull()) {
            m_tooltipIconCache.insert(queuedPath, icon);
        }
        if (!m_settings.simpleTooltips
                && m_ownsTooltip
                && m_hoverTooltipIconLabel
                && queuedPath == m_activeTooltipIconPath
                && !icon.isNull()) {
            m_hoverTooltipIconLabel->setPixmap(icon.pixmap(32, 32));
        }

        if (!m_pendingTooltipIconPath.isEmpty()) {
            requestTooltipIcon(m_pendingTooltipIconPath, m_pendingTooltipIconIsDirectory);
        }
    });
}

bool TreemapWidget::eventFilter(QObject* watched, QEvent* event)
{
    const auto syncExpandedState = [this](QScrollBar* bar, QEvent* currentEvent) {
        bool expanded = false;
        switch (currentEvent->type()) {
        case QEvent::Enter:
        case QEvent::HoverEnter:
            expanded = true;
            break;
        case QEvent::Leave:
        case QEvent::HoverLeave:
            expanded = bar->isSliderDown();
            break;
        default:
            return;
        }
        setOverlayScrollBarExpanded(bar, expanded);
    };

    if (watched == m_overlayHScrollBar) {
        syncExpandedState(m_overlayHScrollBar, event);
    } else if (watched == m_overlayVScrollBar) {
        syncExpandedState(m_overlayVScrollBar, event);
    }

    return QAbstractScrollArea::eventFilter(watched, event);
}

void TreemapWidget::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange
            || event->type() == QEvent::ApplicationPaletteChange
            || event->type() == QEvent::StyleChange) {
        updateScrollBarOverlayStyle();
        updateOwnedTooltipStyle();
    } else if (event->type() == QEvent::LayoutDirectionChange) {
        updateOwnedTooltipLayoutDirection();
    }
    QAbstractScrollArea::changeEvent(event);
}

void TreemapWidget::hideOwnedTooltip()
{
    if (!m_ownsTooltip) {
        return;
    }

    m_ownsTooltip = false;
    m_activeTooltipIconPath.clear();
    if (m_hoverTooltipFade && m_hoverTooltipOpacity && m_hoverTooltipWidget) {
        m_hoverTooltipFade->stop();
        m_hoverTooltipFade->setStartValue(m_hoverTooltipOpacity->opacity());
        m_hoverTooltipFade->setEndValue(0.0);
        m_hoverTooltipFade->start();
        return;
    }
    if (m_hoverTooltipWidget) {
        m_hoverTooltipWidget->hide();
    }
}

void TreemapWidget::positionOwnedTooltip(const QPoint& globalPos)
{
    if (!m_hoverTooltipWidget) {
        return;
    }

    constexpr int kTooltipOffsetX = 14;
    constexpr int kTooltipOffsetY = 20;
    constexpr int kPointerClearanceY = 10;

    const QPoint pointerPos = viewport()->mapFromGlobal(globalPos);
    const int maxX = std::max(0, viewport()->width() - m_hoverTooltipWidget->width());
    const int maxY = std::max(0, viewport()->height() - m_hoverTooltipWidget->height());
    const bool rtl = layoutDirection() == Qt::RightToLeft;

    int preferredX = rtl
        ? pointerPos.x() - m_hoverTooltipWidget->width() - kTooltipOffsetX
        : pointerPos.x() + kTooltipOffsetX;
    if (rtl) {
        if (preferredX < 0) {
            preferredX = pointerPos.x() + kTooltipOffsetX;
        }
    } else if (preferredX > maxX) {
        preferredX = pointerPos.x() - m_hoverTooltipWidget->width() - kTooltipOffsetX;
    }

    QPoint tooltipPos(preferredX, pointerPos.y() + kTooltipOffsetY);

    if (tooltipPos.y() > maxY) {
        tooltipPos.setY(pointerPos.y() - m_hoverTooltipWidget->height() - kPointerClearanceY);
    }

    tooltipPos.setX(std::clamp(tooltipPos.x(), 0, maxX));
    tooltipPos.setY(std::clamp(tooltipPos.y(), 0, maxY));
    m_hoverTooltipWidget->move(tooltipPos);
}

void TreemapWidget::showOwnedTooltip(const QPoint& globalPos, FileNode* node, const QString& text,
                                     double parentPercent, double rootPercent)
{
    if (!m_hoverTooltipWidget || !m_hoverTooltipTextLabel || !m_hoverTooltipIconLabel
            || !m_hoverTooltipSeparator || !m_hoverTooltipDetailsWidget || !m_hoverTooltipSizeLabel
            || !m_hoverTooltipBarWidget || !m_hoverTooltipStatsLabel) {
        return;
    }

    const bool firstShow = !m_ownsTooltip || !m_hoverTooltipWidget->isVisible();
    const bool simpleTooltips = m_settings.simpleTooltips;
    const QString nodePath = (!node || node->isVirtual) ? QString() : node->computePath();
    const bool isDirectory = node && node->isDirectory;
    QIcon icon;
    bool applyImmediateIcon = false;
    if (!simpleTooltips) {
        m_activeTooltipIconPath = nodePath;
        if (!nodePath.isEmpty()) {
            const auto cached = m_tooltipIconCache.constFind(nodePath);
            if (cached != m_tooltipIconCache.cend()) {
                icon = cached.value();
                applyImmediateIcon = true;
            } else {
                requestTooltipIcon(nodePath, isDirectory);
                icon = fallbackTooltipIcon(isDirectory);
                applyImmediateIcon = true;
            }
        } else {
            icon = fallbackTooltipIcon(isDirectory);
            applyImmediateIcon = true;
        }
    } else {
        m_activeTooltipIconPath.clear();
    }
    m_hoverTooltipWidget->setCompactMode(simpleTooltips);
    m_hoverTooltipIconLabel->setVisible(!simpleTooltips);
    m_hoverTooltipSeparator->setVisible(!simpleTooltips);
    m_hoverTooltipDetailsWidget->setVisible(!simpleTooltips);
    if (QLayout* outerLayout = m_hoverTooltipWidget->layout()) {
        if (simpleTooltips) {
            outerLayout->setContentsMargins(0, 0, 0, 0);
            outerLayout->setSpacing(0);
        } else {
            outerLayout->setContentsMargins(0, 0, 0, 0);
            outerLayout->setSpacing(6);
        }
    }
    if (m_hoverTooltipTopRowLayout) {
        if (simpleTooltips) {
            m_hoverTooltipTopRowLayout->setContentsMargins(6, 2, 4, 2);
            m_hoverTooltipTopRowLayout->setSpacing(0);
        } else {
            m_hoverTooltipTopRowLayout->setContentsMargins(10, 8, 12, 0);
            m_hoverTooltipTopRowLayout->setSpacing(8);
        }
    }
    if (m_hoverTooltipTopTextColumnLayout) {
        if (simpleTooltips) {
            m_hoverTooltipTopTextColumnLayout->setContentsMargins(0, 0, 0, 0);
        } else {
            m_hoverTooltipTopTextColumnLayout->setContentsMargins(0, 0, 4, 0);
        }
    }
    if (!simpleTooltips) {
        updateOwnedTooltipLayoutDirection();
    }
    if (!simpleTooltips && applyImmediateIcon) {
        m_hoverTooltipIconLabel->setPixmap(icon.pixmap(32, 32));
    }
    m_hoverTooltipTextLabel->setTextFormat(simpleTooltips ? Qt::PlainText : Qt::RichText);
    m_hoverTooltipTextLabel->setText(text);
    m_hoverTooltipSizeLabel->setText(
        tr("Size: %1").arg(QLocale::system().formattedDataSize(node ? node->size : 0)));
    m_hoverTooltipBarWidget->setPercents(parentPercent, rootPercent);
    m_hoverTooltipStatsLabel->setText(
        tr("Parent %1 · Root %2")
            .arg(QString::number(parentPercent, 'f', 1) + QLatin1Char('%'),
                 QString::number(rootPercent, 'f', 1) + QLatin1Char('%')));
    if (QLayout* layout = m_hoverTooltipWidget->layout()) {
        layout->invalidate();
        layout->activate();
    }
    m_hoverTooltipWidget->adjustSize();
    positionOwnedTooltip(globalPos);
    m_hoverTooltipWidget->show();
    m_hoverTooltipWidget->raise();
    if (m_hoverTooltipFade && m_hoverTooltipOpacity) {
        if (firstShow) {
            m_hoverTooltipFade->stop();
            m_hoverTooltipOpacity->setOpacity(0.0);
            m_hoverTooltipFade->setStartValue(0.0);
            m_hoverTooltipFade->setEndValue(1.0);
            m_hoverTooltipFade->start();
        } else {
            m_hoverTooltipFade->stop();
            m_hoverTooltipOpacity->setOpacity(1.0);
        }
    }
    m_ownsTooltip = true;
}

void TreemapWidget::stopAnimatedNavigation()
{
    m_wheelZoomCoalesceTimer.stop();
    m_trackpadBackReadyTimer.stop();
    m_trackpadBackReady = false;
    m_pendingWheelSteps = 0.0;
    m_zoomAnimation.stop();
    m_cameraAnimation.stop();
    m_previousFrame = QPixmap();
    m_nextFrame = QPixmap();
    m_zoomSourceRect = QRectF();
}

void TreemapWidget::panCameraImmediate(const QPointF& sceneDelta)
{
    const QPointF pannedOrigin = snapCameraOriginToPixelGrid(
        clampCameraOrigin(m_cameraOrigin + sceneDelta, m_cameraScale),
        m_cameraScale, pixelScale());
    const QPointF delta = pannedOrigin - m_cameraOrigin;
    m_cameraOrigin = pannedOrigin;
    m_cameraStartOrigin = pannedOrigin;
    m_cameraTargetOrigin += delta;
    if (m_cameraUseFocusAnchor) {
        m_cameraFocusScenePos += delta;
    }
    syncScrollBars();
    viewport()->update();
}

void TreemapWidget::zoomCameraImmediate(qreal targetScale, const QPointF& anchorScenePos,
                                        const QPointF& anchorScreenPos)
{
    const QPointF targetOrigin(
        anchorScenePos.x() - (anchorScreenPos.x() / targetScale),
        anchorScenePos.y() - (anchorScreenPos.y() / targetScale));
    m_cameraScale = targetScale;
    m_cameraOrigin = snapCameraOriginToPixelGrid(
        clampCameraOrigin(targetOrigin, targetScale), targetScale, pixelScale());
    m_cameraStartScale = m_cameraScale;
    m_cameraTargetScale = m_cameraScale;
    m_cameraStartOrigin = m_cameraOrigin;
    m_cameraTargetOrigin = m_cameraOrigin;
    syncSemanticDepthToScale();
    if (m_cameraScale <= kZoomedInThreshold) {
        m_semanticFocus = nullptr;
        m_semanticLiveRoot = nullptr;
    }
    syncScrollBars();
    viewport()->update();
}

void TreemapWidget::applyPendingWheelZoom()
{
    if (!m_current || qFuzzyIsNull(m_pendingWheelSteps)) {
        m_pendingWheelSteps = 0.0;
        return;
    }

    const qreal baseScale = m_cameraAnimation.state() == QAbstractAnimation::Running
        ? m_cameraTargetScale
        : m_cameraScale;
    const qreal zoomStepFactor = 1.0 + (m_settings.wheelZoomStepPercent / 100.0);
    const qreal targetScale = std::clamp(baseScale * std::pow(zoomStepFactor, m_pendingWheelSteps),
                                         kCameraMinScale, m_settings.cameraMaxScale);
    m_pendingWheelSteps = 0.0;
    if (qFuzzyCompare(targetScale, baseScale)) {
        return;
    }

    const QPointF targetOrigin(
        m_pendingWheelAnchorScenePos.x() - (m_pendingWheelCursorPos.x() / targetScale),
        m_pendingWheelAnchorScenePos.y() - (m_pendingWheelCursorPos.y() / targetScale));

    if (targetScale > kZoomedInThreshold) {
        m_semanticFocus = semanticFocusCandidateAt(m_pendingWheelCursorPos, currentRootViewRect());
        m_semanticLiveRoot = m_current;
    } else {
        m_semanticFocus = nullptr;
        m_semanticLiveRoot = nullptr;
    }

    const QPointF clampedOrigin = snapCameraOriginToPixelGrid(
        clampCameraOrigin(targetOrigin, targetScale), targetScale, pixelScale());
    animateCameraTo(targetScale, clampedOrigin,
                    m_pendingWheelAnchorScenePos, m_pendingWheelCursorPos, true);
}

FileNode* TreemapWidget::interactiveNodeAt(const QPointF& pos) const
{
    if (!m_current) {
        return nullptr;
    }

    FileNode* hit = hitTest(m_current, pos, -1, currentRootViewRect());
    if (!hit || hit == m_current || hit->isVirtual) {
        return nullptr;
    }
    return hit;
}

void TreemapWidget::updateHoverAt(const QPointF& pos, const QPoint& globalPos, bool notify)
{
    if (!m_current) {
        return;
    }

    QRectF hitRect;
    FileNode* hit = hitTest(m_current, pos, -1, currentRootViewRect(), &hitRect);
    if (hit == m_current) {
        hit = nullptr;
        hitRect = QRectF();
    }

    FileNode* hoverHit = hit;
    QRectF hoverRect = hitRect;
    if (suppressHoverForTinyTranslucentLeaf(hoverHit, hoverRect)) {
        hoverHit = nullptr;
        hoverRect = QRectF();
    }

    const bool hoverChanged = (hoverHit != m_hovered);
    const bool hoverRectChanged = (!hoverChanged && hoverRect != m_hoveredRect);
    if (hoverChanged || hoverRectChanged) {
        QRect dirty = hoverDirtyRectForNode(m_hovered, m_hoveredRect);
        dirty = dirty.united(hoverDirtyRectForNode(hoverHit, hoverRect));
        m_previousHovered = nullptr;
        m_previousHoveredRect = QRectF();
        m_hovered = hoverHit;
        m_hoveredRect = hoverRect;
        if (hoverChanged) {
            m_hoveredTooltip.clear();
            m_hoverBlend = hoverHit ? 1.0 : 0.0;
            m_hoverAnimation.stop();
        }
        viewport()->update(dirty);
    }

    const bool tooltipChanged = (hit != m_tooltipTarget);
    if (hit) {
        if (m_hoveredTooltip.isEmpty() || tooltipChanged) {
            const QString nodePath = hit->isVirtual ? QString() : hit->computePath();
            const QFileInfo info(nodePath);
            const QString displayPath = (nodePath.isEmpty() ? hit->name : nodePath);
            constexpr int kMaxSimpleTooltipPathChars = 80;
            auto middleElideChars = [](const QString& text, int maxChars) {
                if (maxChars <= 3 || text.size() <= maxChars) {
                    return text;
                }
                constexpr auto kEllipsis = "...";
                const int availableChars = maxChars - 3;
                const int prefixChars = (availableChars + 1) / 2;
                const int suffixChars = availableChars / 2;
                return text.left(prefixChars)
                    + QString::fromLatin1(kEllipsis)
                    + text.right(suffixChars);
            };
            const int simplePathWidth = std::max(120, viewport()->width() - 44);
            const int richPathWidth = std::max(120, viewport()->width() - 92);
            const QString cappedSimplePath = middleElideChars(displayPath, kMaxSimpleTooltipPathChars);
            const QString elidedSimplePath = m_hoverTooltipTextLabel->fontMetrics().elidedText(
                cappedSimplePath, Qt::ElideMiddle, simplePathWidth);
            const QString sizeStr = QLocale::system().formattedDataSize(hit->size);
            if (m_settings.simpleTooltips) {
                m_hoveredTooltip = QStringLiteral("%1\n%2")
                    .arg(elidedSimplePath, sizeStr);
            } else {
                const bool rtl = layoutDirection() == Qt::RightToLeft;
                const QString dirAttr = rtl ? QStringLiteral("rtl") : QStringLiteral("ltr");
                const QString alignAttr = rtl ? QStringLiteral("right") : QStringLiteral("left");
                const QString displayName = hit->isVirtual
                    ? hit->name
                    : (!info.fileName().isEmpty() ? info.fileName() : nodePath);
                const QString richPath = hit->isVirtual
                    ? QString()
                    : tooltipDisplayPath(info.absolutePath());
                constexpr int kMaxTooltipPathChars = 64;
                const QString cappedRichPath = middleElideChars(richPath, kMaxTooltipPathChars);
                const QString elidedRichPath = m_hoverTooltipTextLabel->fontMetrics().elidedText(
                    cappedRichPath, Qt::ElideMiddle, richPathWidth);
                const QString kindText = hit->isVirtual
                    ? QCoreApplication::translate("ColorUtils", "Free Space")
                    : (hit->isDirectory ? tr("Folder") : tr("File"));
                const QString escapedName = displayName.toHtmlEscaped();
                const QString escapedPath = elidedRichPath.toHtmlEscaped();
                m_hoveredTooltip = QStringLiteral("<div dir=\"%1\"><div align=\"%2\"><b>%3</b></div>")
                    .arg(dirAttr, alignAttr, escapedName);
                if (!escapedPath.isEmpty()) {
                    m_hoveredTooltip += QStringLiteral(
                        "<div dir=\"ltr\" align=\"%1\" style=\"opacity:0.78;\">%2</div>")
                        .arg(alignAttr, escapedPath);
                }
                if (!kindText.isEmpty() && kindText != displayName) {
                    m_hoveredTooltip += QStringLiteral("<div align=\"%1\" style=\"opacity:0.78;\">%2</div>")
                        .arg(alignAttr, kindText.toHtmlEscaped());
                }
                m_hoveredTooltip += QStringLiteral("</div>");
            }
        }
        if (hoverChanged || hoverRectChanged || tooltipChanged || !m_ownsTooltip
                || m_hoverTooltipTextLabel->text() != m_hoveredTooltip) {
            const qint64 parentSize = (hit->parent && hit->parent->size > 0) ? hit->parent->size : hit->size;
            const double parentPercent = (parentSize > 0)
                ? (100.0 * static_cast<double>(hit->size) / static_cast<double>(parentSize))
                : 0.0;
            const double rootPercent = (m_root && m_root->size > 0)
                ? (100.0 * static_cast<double>(hit->size) / static_cast<double>(m_root->size))
                : 0.0;
            showOwnedTooltip(globalPos, hit, m_hoveredTooltip, parentPercent, rootPercent);
        } else {
            positionOwnedTooltip(globalPos);
        }
        m_tooltipTarget = hit;
    } else {
        m_tooltipTarget = nullptr;
        m_hoveredTooltip.clear();
        hideOwnedTooltip();
    }

    if (notify) {
        emit nodeHovered(hit);
    }
}

void TreemapWidget::refreshHoverUnderPointer()
{
    if (!m_current) {
        return;
    }

    if (m_scanInProgress) {
        clearHoverState();
        return;
    }

    if (m_contextMenuActive || m_middlePanning || m_touchGestureActive || m_nativeGestureActive) {
        return;
    }

    const QPoint globalPos = QCursor::pos();
    const QPoint localPos = viewport()->mapFromGlobal(globalPos);
    if (!viewport()->rect().contains(localPos)) {
        if (m_hovered || m_previousHovered || !m_hoveredRect.isEmpty() || !m_previousHoveredRect.isEmpty()) {
            clearHoverState();
        }
        return;
    }

    updateHoverAt(QPointF(localPos), globalPos);
}

void TreemapWidget::beginPressHold(FileNode* node, const QPointF& pos, bool fromTouch)
{
    if (!node) {
        cancelPressHold();
        return;
    }

    m_pressHoldActive = true;
    m_pressHoldFromTouch = fromTouch;
    m_pressHoldTriggered = false;
    m_pressHoldNode = node;
    m_pressHoldStartPos = pos;
    m_pressHoldCurrentPos = pos;
    m_pressHoldTimer.start();
}

void TreemapWidget::updatePressHold(const QPointF& pos)
{
    if (!m_pressHoldActive) {
        return;
    }

    m_pressHoldCurrentPos = pos;
    if (QLineF(pos, m_pressHoldStartPos).length() > kPressHoldMoveThreshold) {
        cancelPressHold();
    }
}

void TreemapWidget::cancelPressHold()
{
    m_pressHoldTimer.stop();
    m_pressHoldActive = false;
    m_pressHoldFromTouch = false;
    m_pressHoldTriggered = false;
    m_pressHoldNode = nullptr;
    m_pressHoldStartPos = QPointF();
    m_pressHoldCurrentPos = QPointF();
}

void TreemapWidget::triggerPressHoldContextMenu()
{
    if (!m_pressHoldActive || !m_pressHoldNode || m_scanInProgress) {
        cancelPressHold();
        return;
    }

    m_pressHoldTriggered = true;
    hideOwnedTooltip();
    m_contextMenuActive = true;
    emit nodeContextMenuRequested(
        m_pressHoldNode,
        viewport()->mapToGlobal(m_pressHoldCurrentPos.toPoint()));
    m_contextMenuActive = false;
    clearHoverState(false);

    if (m_pressHoldFromTouch) {
        m_touchTapEligible = false;
        m_touchSequenceTapBlocked = true;
        m_touchSuppressTap = true;
        m_touchTapSuppressTimer.start();
    }

    cancelPressHold();

    if (!underMouse() && m_hovered) {
        leaveEvent(nullptr);
    }
}

void TreemapWidget::beginTouchPan(const QPointF& pos)
{
    const bool continuingGesture = m_touchGestureActive;

    m_touchGestureActive = true;
    m_touchPanning = true;
    m_touchPinching = false;
    m_touchTapEligible = !m_touchSequenceTapBlocked;
    m_touchPanLastPos = pos;
    m_touchTapStartPos = pos;
    if (!continuingGesture) {
        m_touchSequenceTapBlocked = false;
        m_touchPendingBackOnRelease = false;
        m_touchSuppressTap = false;
        m_touchTapSuppressTimer.stop();
    }
    beginPressHold(interactiveNodeAt(pos), pos, true);
    m_semanticFocus = m_cameraScale > kZoomedInThreshold ? semanticFocusCandidateAt(pos, currentRootViewRect()) : nullptr;
    m_semanticLiveRoot = m_cameraScale > kZoomedInThreshold ? m_current : nullptr;
}

void TreemapWidget::beginTouchPinch(const QList<QEventPoint>& points)
{
    if (points.size() < 2) {
        return;
    }

    const QPointF p1 = points.at(0).position();
    const QPointF p2 = points.at(1).position();
    const QPointF center = (p1 + p2) * 0.5;
    const qreal distance = QLineF(p1, p2).length();

    m_touchGestureActive = true;
    m_touchPanning = false;
    m_touchPinching = true;
    m_touchTapEligible = false;
    m_touchSequenceTapBlocked = true;
    m_touchPanLastPos = center;
    m_touchPinchInitialDistance = std::max<qreal>(distance, 1.0);
    m_touchPinchInitialScale = m_cameraScale;
    m_touchSuppressTap = true;
    m_touchTapSuppressTimer.start();
    cancelPressHold();
    m_touchPinchAnchorScenePos = QPointF(
        m_cameraOrigin.x() + (center.x() / m_cameraScale),
        m_cameraOrigin.y() + (center.y() / m_cameraScale));
    m_semanticFocus = semanticFocusCandidateAt(center, currentRootViewRect());
    m_semanticLiveRoot = m_current;
}

void TreemapWidget::updateTouchGesture(const QList<QEventPoint>& points)
{
    if (m_nativeGestureActive) {
        return;
    }

    QList<QEventPoint> activePoints;
    activePoints.reserve(points.size());
    for (const QEventPoint& point : points) {
        if (point.state() != QEventPoint::State::Released) {
            activePoints.push_back(point);
        }
    }

    if (activePoints.isEmpty()) {
        endTouchGesture();
        return;
    }

    if (activePoints.size() >= 2) {
        const QPointF p1 = activePoints.at(0).position();
        const QPointF p2 = activePoints.at(1).position();
        const QPointF center = (p1 + p2) * 0.5;
        const qreal distance = std::max<qreal>(QLineF(p1, p2).length(), 1.0);

        if (!m_touchPinching) {
            // Don't activate pinch immediately on two-finger contact — on macOS a plain
            // two-finger scroll also sends touch events and would block wheel scroll events
            // and cancel in-flight zoom animations. Wait until inter-finger distance changes
            // by >4% to distinguish a real pinch from a parallel scroll gesture.
            if (m_touchPinchCandidateDistance < 1.0) {
                m_touchPinchCandidateDistance = distance;
                return;
            }
            if (std::abs(distance / m_touchPinchCandidateDistance - 1.0) < 0.04) {
                return; // still looks like a scroll — let wheel events handle it
            }
            // Actual pinch confirmed: stop any in-flight animations before taking over.
            stopAnimatedNavigation();
            clearHoverState();
            if (m_middlePanning) {
                m_middlePanning = false;
                unsetCursor();
            }
            beginTouchPinch(activePoints);
        }

        const qreal scaleFactor = distance / std::max<qreal>(m_touchPinchInitialDistance, 1.0);
        const qreal rawTargetScale = m_touchPinchInitialScale * scaleFactor;
        const qreal targetScale = std::clamp(
            rawTargetScale,
            kCameraMinScale,
            m_settings.cameraMaxScale);

        const bool zoomingOutPastMin = rawTargetScale < kCameraMinScale
            && m_touchPinchInitialScale <= (kCameraMinScale + 0.0001)
            && m_current
            && m_current->parent
            && !m_touchPendingBackOnRelease;
        if (zoomingOutPastMin) {
            m_touchPendingBackOnRelease = true;
            m_touchSuppressTap = true;
            m_touchTapSuppressTimer.start();
        }

        QPointF targetOrigin(
            m_touchPinchAnchorScenePos.x() - (center.x() / targetScale),
            m_touchPinchAnchorScenePos.y() - (center.y() / targetScale));
        targetOrigin = snapCameraOriginToPixelGrid(
            clampCameraOrigin(targetOrigin, targetScale), targetScale, pixelScale());

        m_cameraScale = targetScale;
        m_cameraOrigin = targetOrigin;
        m_cameraStartScale = m_cameraScale;
        m_cameraTargetScale = m_cameraScale;
        m_cameraStartOrigin = m_cameraOrigin;
        m_cameraTargetOrigin = m_cameraOrigin;
        syncSemanticDepthToScale();
        if (m_cameraScale <= kZoomedInThreshold) {
            m_semanticFocus = nullptr;
            m_semanticLiveRoot = nullptr;
        }
        syncScrollBars();
        viewport()->update();
        return;
    }

    // Single-finger pan path.
    stopAnimatedNavigation();
    clearHoverState();

    if (m_middlePanning) {
        m_middlePanning = false;
        unsetCursor();
    }

    const QPointF pos = activePoints.at(0).position();
    if (!m_touchPanning) {
        beginTouchPan(pos);
    }

    const QPointF delta = pos - m_touchPanLastPos;
    updatePressHold(pos);
    if (QLineF(pos, m_touchTapStartPos).length() > 12.0) {
        m_touchTapEligible = false;
        m_touchSequenceTapBlocked = true;
        m_touchSuppressTap = true;
        m_touchTapSuppressTimer.start();
        cancelPressHold();
    }
    const QPointF targetOrigin(
        m_cameraOrigin.x() - (delta.x() / m_cameraScale),
        m_cameraOrigin.y() - (delta.y() / m_cameraScale));
    m_cameraOrigin = snapCameraOriginToPixelGrid(
        clampCameraOrigin(targetOrigin, m_cameraScale), m_cameraScale, pixelScale());
    m_cameraStartOrigin = m_cameraOrigin;
    m_cameraTargetOrigin = m_cameraOrigin;
    m_touchPanLastPos = pos;
    if (m_cameraScale <= kZoomedInThreshold) {
        m_semanticFocus = nullptr;
        m_semanticLiveRoot = nullptr;
    }
    syncScrollBars();
    viewport()->update();
}

void TreemapWidget::endTouchGesture()
{
    cancelPressHold();
    const bool shouldRequestBack = m_touchPendingBackOnRelease;
    const bool shouldActivateTap = m_touchGestureActive
        && m_touchPanning
        && !m_touchPinching
        && m_touchTapEligible
        && !m_touchSuppressTap
        && !shouldRequestBack;
    const QPointF tapPos = m_touchPanLastPos;
    const bool shouldDebounceTap = m_touchSuppressTap || !m_touchTapEligible || m_touchPinching || shouldRequestBack;

    m_touchGestureActive = false;
    m_touchPanning = false;
    m_touchPinching = false;
    m_touchTapEligible = false;
    m_touchSequenceTapBlocked = false;
    m_touchPendingBackOnRelease = false;
    m_touchPinchInitialDistance = 0.0;
    m_touchPinchInitialScale = m_cameraScale;
    m_touchPinchCandidateDistance = 0.0;
    m_touchPanLastPos = QPointF();
    m_touchTapStartPos = QPointF();
    if (shouldDebounceTap) {
        m_touchSuppressTap = true;
        m_touchTapSuppressTimer.start();
    } else {
        m_touchSuppressTap = false;
        m_touchTapSuppressTimer.stop();
    }
    if (m_cameraScale <= kZoomedInThreshold) {
        m_semanticFocus = nullptr;
        m_semanticLiveRoot = nullptr;
    }
    if (shouldRequestBack) {
        emit backRequested();
        return;
    }
    if (shouldActivateTap) {
        activateTouchTap(tapPos);
    }
}

void TreemapWidget::activateTouchTap(const QPointF& pos)
{
    if (!m_current || m_scanInProgress) {
        return;
    }

    FileNode* hit = hitTest(m_current, pos, -1, currentRootViewRect());
    if (hit && hit != m_current && !hit->isVirtual) {
        m_lastActivationPos = pos;
        emit nodeActivated(hit);
    }
}

QRectF TreemapWidget::currentRootViewRect() const
{
    return sceneToViewRectF(QRectF(0.0, 0.0, viewport()->width(), viewport()->height()));
}

TreemapWidget::DirectoryRenderState TreemapWidget::computeDirectoryRenderState(
    const FileNode* node, const QRectF& viewRect, const QRectF& effectiveClip, int depth) const
{
    DirectoryRenderState state;
    const bool liveCameraAnimating = continuousZoomGeometryActive();
    const qreal devicePixelScale = pixelScale();
    const QRectF snappedViewRect = liveCameraAnimating
        ? viewRect
        : snapRectToPixels(viewRect, devicePixelScale);

    state.revealArea = viewRect;
    state.showsChildren = canPaintChildrenForDisplay(node, viewRect, depth);

    if (node == m_current) {
        state.childPaintRect = childPaintRectForNode(node, viewRect);
        state.childLayoutRect = state.childPaintRect;
        state.childContentClip = snappedViewRect.adjusted(-1.0, -1.0, 1.0, 1.0).intersected(effectiveClip);
        state.tileRect = snappedViewRect;
        state.tileFillClipRect = snappedViewRect;
        state.contentFillClipRect = state.childContentClip;
        return state;
    }

    state.chromeOpacity = folderDetailOpacityForNode(node, viewRect);
    state.showChrome = state.chromeOpacity > 0.0;
    const TileChromeGeometry chrome = makeTileChromeGeometry(
        viewRect, m_settings, !liveCameraAnimating, state.showChrome, devicePixelScale);
    // When showChrome is true, contentLayoutRect from this chrome call is identical
    // to what childLayoutRectForNode would return (both use detailedChrome=true,
    // and contentLayoutRect only depends on bounds, not snapTile).  Reuse it to
    // avoid a second makeTileChromeGeometry call.
    state.childLayoutRect = state.showChrome
        ? chrome.contentLayoutRect
        : childLayoutRectForNode(node, viewRect);
    state.childPaintRect = chrome.contentPaintRect;
    state.childContentClip = state.childPaintRect.adjusted(-1.0, -1.0, 1.0, 1.0).intersected(effectiveClip);
    state.tileRect = chrome.tileRect;
    state.framedHeaderRect = chrome.framedHeaderRect;
    state.tileFillClipRect = liveCameraAnimating
        ? chrome.tileRect.intersected(effectiveClip)
        : snapRectToPixels(chrome.tileRect.intersected(effectiveClip), devicePixelScale);
    state.contentFillClipRect = liveCameraAnimating
        ? state.childPaintRect.intersected(effectiveClip)
        : snapRectToPixels(state.childPaintRect.intersected(effectiveClip), devicePixelScale);
    return state;
}

QRectF TreemapWidget::childLayoutRectForNode(const FileNode* node, const QRectF& viewRect) const
{
    if (node == m_current) {
        return viewRect;
    }
    return makeTileChromeGeometry(viewRect, m_settings, false, true, pixelScale()).contentLayoutRect;
}

QRectF TreemapWidget::childPaintRectForNode(const FileNode* node, const QRectF& viewRect) const
{
    const bool liveCameraAnimating = continuousZoomGeometryActive();
    if (node == m_current) {
        if (liveCameraAnimating) {
            return viewRect;
        }
        // Snapped origin + continuous camera-scale dimensions: each tile boundary
        // then advances independently when its own normalised position crosses a
        // device-pixel threshold, instead of all shifting at once when
        // round(W * scale * DPR) crosses an integer.  This same rect is used by
        // paintNode, paintMatchOverlayNode, paintMatchBordersNode, and hit-testing
        // so all code paths see identical child positions.
        const QRectF ri = snapRectToPixels(viewRect, pixelScale());
        return QRectF(ri.topLeft(),
                      QSizeF(viewport()->width() * m_cameraScale,
                             viewport()->height() * m_cameraScale));
    }
    return makeTileChromeGeometry(viewRect, m_settings, !liveCameraAnimating,
                                  true, pixelScale()).contentPaintRect;
}

FileNode* TreemapWidget::semanticFocusCandidateAt(const QPointF& pos, const QRectF& viewRect, int depth) const
{
    if (!m_current || !viewRect.contains(pos)) {
        return nullptr;
    }

    std::function<FileNode*(FileNode*, const QRectF&, int)> recurse =
        [&](FileNode* node, const QRectF& nodeViewRect, int nodeDepth) -> FileNode* {
            if (!node || !node->isDirectory || !nodeViewRect.contains(pos)) {
                return nullptr;
            }

            FileNode* best = node;
            if (nodeDepth >= (m_settings.baseVisibleDepth - 1)) {
                return best;
            }

            const QRectF contentArea = childPaintRectForNode(node, nodeViewRect);
            const QRectF contentClip = contentArea.adjusted(-1.0, -1.0, 1.0, 1.0);
            std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
            layoutVisibleChildren(node, nodeViewRect, contentArea, contentClip, visibleChildren);
            for (const auto& [child, childViewRect] : visibleChildren) {
                if (!child->isDirectory || !childViewRect.contains(pos)) {
                    continue;
                }
                if (FileNode* deeper = recurse(child, childViewRect, nodeDepth + 1)) {
                    best = deeper;
                }
                break;
            }
            return best;
        };

    FileNode* candidate = recurse(m_current, viewRect, depth);
    return (candidate && candidate != m_current) ? candidate : nullptr;
}

void TreemapWidget::relayout()
{
    syncScrollBars();
    if (!m_current) {
        viewport()->update();
        return;
    }

    viewport()->update();
}

void TreemapWidget::notifyTreeChanged()
{
    m_liveSplitCache.clear();
    m_sizeLabelCache.clear();
    m_elidedTextCache.clear();
    m_elidedDisplayWidthCache.clear();
    m_stableMetricCache.clear();
    m_headerLabelVisibleCache.clear();
    m_fileLabelVisibleCache.clear();
    m_fileSizeLabelVisibleCache.clear();
    cancelPendingSearch();
    m_searchMatchCache = {};
    m_fileTypeMatchCache = {};
    m_combinedMatchCache = {};
    m_searchReachCache = {};
    m_searchMatchScratch = {};
    m_searchReachScratch = {};
    m_nodeCount = 0;
    m_searchIndex.reset();
    cancelPendingFileTypeHighlight();

    m_previousDirectSearchMatches.clear();
    m_previousSearchCaseFoldedPattern.clear();
    m_previousSearchUsesWildcards = false;
    m_previousMinSizeFilter = 0;
    m_previousMaxSizeFilter = 0;
    clearHoverState(false);
    viewport()->update();
    rebuildSearchMetadataAsync();
}


void TreemapWidget::cancelPendingSearch()
{
    if (m_searchCancelToken) {
        m_searchCancelToken->store(true, std::memory_order_relaxed);
    }

    // Do not block the UI waiting for old metadata/search work to finish.
    // The async tasks hold their own shared owners for any tree data they
    // touch, and replacing the watched future is enough to ignore stale work.
    m_searchCancelToken = std::make_shared<std::atomic<bool>>(false);
}

void TreemapWidget::cancelPendingFileTypeHighlight()
{
    if (m_fileTypeCancelToken) {
        m_fileTypeCancelToken->store(true, std::memory_order_relaxed);
    }
    m_fileTypeCancelToken = std::make_shared<std::atomic<bool>>(false);
}

void TreemapWidget::rebuildSearchMatches()
{
    if (!m_searchActive || !m_root || !m_searchIndex) {
        // Search deactivated: clear highlights immediately and repaint.
        std::fill(m_searchMatchCache.begin(), m_searchMatchCache.end(), 0);
        m_searchReachCache = {};
        m_searchMatchScratch = {};
        m_searchReachScratch = {};
        m_previousDirectSearchMatches.clear();
        m_previousSearchCaseFoldedPattern = m_searchCaseFoldedPattern;
        m_previousSearchUsesWildcards = m_searchUsesWildcards;
        m_previousMinSizeFilter = m_minSizeFilter;
        m_previousMaxSizeFilter = m_maxSizeFilter;
        rebuildCombinedMatchCache();
        emit searchResultsChanged();
        viewport()->update();
        return;
    }
    // Keep old highlights visible until new results arrive — no premature clear.

    // Cancel any in-flight search and allocate a fresh cancel token.
    cancelPendingSearch();

    const bool sizeFilterUnchanged = (m_minSizeFilter == m_previousMinSizeFilter)
        && (m_maxSizeFilter == m_previousMaxSizeFilter);
    // A narrowed size filter (higher min or lower max) makes previous matches a valid
    // superset, so we can safely search incrementally within them.
    const bool sizeFilterNarrowed = !sizeFilterUnchanged
        && (m_minSizeFilter >= m_previousMinSizeFilter)
        && (m_maxSizeFilter == 0 || m_previousMaxSizeFilter == 0
            || m_maxSizeFilter <= m_previousMaxSizeFilter);
    const bool canIncremental = (sizeFilterUnchanged || sizeFilterNarrowed)
        && !m_searchUsesWildcards
        && !m_previousSearchUsesWildcards
        && !m_previousSearchCaseFoldedPattern.isEmpty()
        && m_searchCaseFoldedPattern.startsWith(m_previousSearchCaseFoldedPattern);

    // Snapshot all inputs for the background task.
    m_pendingSearchIndex = m_searchIndex;
    const bool incrementalInput = canIncremental;
    const std::vector<FileNode*> incrementalNodes = incrementalInput
        ? m_previousDirectSearchMatches
        : std::vector<FileNode*>();
    const QString pattern      = m_searchCaseFoldedPattern;
    const std::string patternUtf8 = pattern.toUtf8().toStdString();
    const bool usesWildcards   = m_searchUsesWildcards;
    const qint64 sizeMin       = m_minSizeFilter;
    const qint64 sizeMax       = m_maxSizeFilter;
    const bool sizeActive      = m_sizeFilterActive;
    std::vector<uint8_t> matchScratch = std::move(m_searchMatchScratch);
    std::vector<bool> reachScratch = std::move(m_searchReachScratch);
    auto cancelToken           = m_searchCancelToken; // shared ownership

    m_searchWatcher->setFuture(QtConcurrent::run([
            index = m_pendingSearchIndex,
            incrementalInput,
            inputNodes = std::move(incrementalNodes),
            pattern, patternUtf8, usesWildcards,
            sizeMin, sizeMax, sizeActive,
            matchScratch = std::move(matchScratch),
            reachScratch = std::move(reachScratch),
            cancelToken]() -> SearchMatchResult
    {
        SearchMatchResult result;
        if (!index) {
            return result;
        }

        const std::vector<FileNode*>& searchNodes = incrementalInput ? inputNodes : index->nodes;
        result.directMatches.reserve(std::min<size_t>(searchNodes.size(), 4096));
        result.matchCache = std::move(matchScratch);
        if (result.matchCache.size() != index->nodeCount) {
            result.matchCache.assign(index->nodeCount, 0);
        } else {
            std::fill(result.matchCache.begin(), result.matchCache.end(), 0);
        }
        int i = 0;
        for (FileNode* node : searchNodes) {
            if ((++i & 0xFFF) == 0 && cancelToken->load(std::memory_order_relaxed)) {
                result.cancelled = true;
                return result;
            }

            // Pattern match
            bool patternOk = pattern.isEmpty();
            if (!patternOk) {
                if (usesWildcards) {
                    if (node->id < index->nameOffsets.size()
                        && index->nameLens[node->id] > 0) {
                        const std::string_view name(
                            index->flatNames.data() + index->nameOffsets[node->id],
                            index->nameLens[node->id]);
                        patternOk = wildcardUtf8Match(patternUtf8, name);
                    } else {
                        patternOk = wildcardUtf8Match(
                            patternUtf8, node->name.toCaseFolded().toUtf8().toStdString());
                    }
                } else if (node->id < index->nameOffsets.size()
                           && index->nameLens[node->id] > 0) {
                    const std::string_view name(
                        index->flatNames.data() + index->nameOffsets[node->id],
                        index->nameLens[node->id]);
                    patternOk = name.find(patternUtf8) != std::string_view::npos;
                } else {
                    patternOk = node->name.toCaseFolded().contains(pattern);
                }
            }

            // Size match
            const bool sizeOk = !sizeActive
                || ((sizeMin == 0 || node->size >= sizeMin)
                    && (sizeMax == 0 || node->size <= sizeMax));

            if (patternOk && sizeOk) {
                result.directMatches.push_back(node);
                if (node->id < result.matchCache.size()) {
                    result.matchCache[node->id] |= kSearchSelfMatch;
                }
            }
        }

        result.searchReachCache = std::move(reachScratch);
        if (result.searchReachCache.size() != index->nodeCount) {
            result.searchReachCache.assign(index->nodeCount, false);
        } else {
            std::fill(result.searchReachCache.begin(), result.searchReachCache.end(), false);
        }

        for (FileNode* node : result.directMatches) {
            for (FileNode* parent = node ? node->parent : nullptr; parent; parent = parent->parent) {
                if (parent->id >= result.matchCache.size()) {
                    continue;
                }
                if (result.matchCache[parent->id] & kSearchSubtreeMatch) {
                    break;
                }
                result.matchCache[parent->id] |= kSearchSubtreeMatch;
            }

            if (!node || node->id >= result.searchReachCache.size()) {
                continue;
            }

            if (!node->isDirectory) {
                result.searchReachCache[node->id] = true;
                continue;
            }

            if (result.searchReachCache[node->id]) {
                continue;
            }

            std::vector<FileNode*> stack;
            stack.push_back(node);
            while (!stack.empty()) {
                if ((++i & 0xFFF) == 0 && cancelToken->load(std::memory_order_relaxed)) {
                    result.cancelled = true;
                    return result;
                }
                FileNode* current = stack.back();
                stack.pop_back();
                if (!current || current->id >= result.searchReachCache.size()
                    || result.searchReachCache[current->id]) {
                    continue;
                }
                result.searchReachCache[current->id] = true;
                for (auto it = current->children.rbegin(); it != current->children.rend(); ++it) {
                    stack.push_back(*it);
                }
            }
        }

        return result;
    }));
}

void TreemapWidget::onSearchTaskFinished()
{
    // Discard results if the index was replaced since this task was launched
    // (i.e. a tree change invalidated everything while the task ran).
    if (m_pendingSearchIndex != m_searchIndex) {
        return;
    }

    SearchMatchResult result = m_searchWatcher->result();
    if (result.cancelled) {
        return; // task was cancelled — a newer search is already in flight
    }

    m_searchMatchCache = result.matchCache;
    m_searchReachCache = result.searchReachCache;
    m_searchMatchScratch = std::move(result.matchCache);
    m_searchReachScratch = std::move(result.searchReachCache);
    m_previousDirectSearchMatches = std::move(result.directMatches);
    m_previousSearchCaseFoldedPattern = m_searchCaseFoldedPattern;
    m_previousSearchUsesWildcards = m_searchUsesWildcards;
    m_previousMinSizeFilter = m_minSizeFilter;
    m_previousMaxSizeFilter = m_maxSizeFilter;

    rebuildCombinedMatchCache();
    emit searchResultsChanged();
    viewport()->update();
}

std::vector<bool> TreemapWidget::captureSearchReachSnapshot() const
{
    // m_searchReachCache is built in onSearchTaskFinished; return a copy for off-thread use.
    return m_searchReachCache;
}

std::shared_ptr<SearchIndex> TreemapWidget::captureSearchIndexSnapshot() const
{
    return m_searchIndex;
}

std::shared_ptr<SearchIndex> buildSearchIndex(FileNode* root)
{
    if (!root) return {};

    auto index = std::make_shared<SearchIndex>();
    std::vector<FileNode*> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        FileNode* node = stack.back();
        stack.pop_back();
        if (!node || node->isVirtual) continue;

        node->id = index->nodeCount++;
        index->nodes.push_back(node);
        index->nameOffsets.push_back(static_cast<uint32_t>(index->flatNames.size()));
        const QByteArray folded = node->name.toCaseFolded().toUtf8();
        index->nameLens.push_back(static_cast<uint16_t>(std::min<int>(folded.size(), 65535)));
        index->flatNames.append(folded.constData(), folded.size());
        if (!node->isDirectory) {
            index->filesByExt[node->extKey].push_back(node);
        }
        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
            stack.push_back(*it);
        }
    }

    return index;
}

void TreemapWidget::rebuildSearchMetadata()
{
    if (!m_root) {
        m_nodeCount = 0;
        m_searchMatchCache = {};
        m_searchIndex.reset();
        return;
    }
    m_searchIndex = buildSearchIndex(m_root);
    m_nodeCount = m_searchIndex->nodeCount;
    m_searchMatchCache.assign(m_nodeCount, 0);
}

void TreemapWidget::rebuildSearchMetadataAsync()
{
    if (!m_root) {
        m_nodeCount = 0;
        m_searchMatchCache = {};
        m_searchIndex.reset();
        return;
    }

    FileNode* const root = m_root;
    const std::shared_ptr<NodeArena> rootArena = m_rootArena;
    auto cancelToken = m_searchCancelToken;

    m_metadataWatcher->setFuture(QtConcurrent::run(
        [root, rootArena, cancelToken]() -> std::shared_ptr<SearchIndex>
    {
        if (!rootArena) {
            return {};
        }
        auto index = std::make_shared<SearchIndex>();
        std::vector<FileNode*> stack;
        stack.push_back(root);
        int i = 0;
        while (!stack.empty()) {
            if ((++i & 0xFFF) == 0 && cancelToken->load(std::memory_order_relaxed)) {
                return {};
            }
            FileNode* node = stack.back();
            stack.pop_back();
            if (!node || node->isVirtual) continue;

            node->id = index->nodeCount++;
            index->nodes.push_back(node);
            index->nameOffsets.push_back(static_cast<uint32_t>(index->flatNames.size()));
            const QByteArray folded = node->name.toCaseFolded().toUtf8();
            index->nameLens.push_back(static_cast<uint16_t>(std::min<int>(folded.size(), 65535)));
            index->flatNames.append(folded.constData(), folded.size());
            if (!node->isDirectory) {
                index->filesByExt[node->extKey].push_back(node);
            }
            for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
                stack.push_back(*it);
            }
        }

        index->arenaOwner = rootArena;
        return index;
    }));
}

void TreemapWidget::onMetadataTaskFinished()
{
    const std::shared_ptr<SearchIndex> index = m_metadataWatcher->result();
    if (!index) return; // cancelled

    m_nodeCount = index->nodeCount;
    m_searchMatchCache.assign(m_nodeCount, 0);
    m_fileTypeMatchCache.assign(m_nodeCount, 0);
    m_searchIndex = index;
    rebuildSearchMatches();
    rebuildFileTypeMatchesAsync();
    viewport()->update();
}

void TreemapWidget::rebuildFileTypeMatchesAsync()
{
    if (m_nodeCount == 0 || !m_root) {
        m_fileTypeMatchCache = {};
        emit fileTypeHighlightBusyChanged(false);
        return;
    }

    cancelPendingFileTypeHighlight();
    if (m_highlightedFileType.isEmpty()) {
        m_fileTypeMatchCache.assign(m_nodeCount, 0);
        m_previousHighlightedFileType = m_highlightedFileType;
        emit fileTypeHighlightBusyChanged(false);
        rebuildCombinedMatchCache();
        viewport()->update();
        return;
    }

    const QString highlightedType = m_highlightedFileType;
    auto index = m_searchIndex;
    auto cancelToken = m_fileTypeCancelToken;
    m_pendingHighlightedFileType = highlightedType;
    emit fileTypeHighlightBusyChanged(true);

    // Precompute match key on main thread to avoid per-node string ops in the worker.
    const bool matchVirtual = (highlightedType == QCoreApplication::translate("ColorUtils", "Free Space"));
    const bool matchNoExt   = (highlightedType == QCoreApplication::translate("ColorUtils", "No extension"));
    const uint64_t typeKey  = (!matchVirtual && !matchNoExt)
        ? ColorUtils::packFileExt(QString(u'x') + highlightedType)
        : 0;

    m_fileTypeWatcher->setFuture(QtConcurrent::run(
        [index, cancelToken, matchVirtual, matchNoExt, typeKey]() -> FileTypeMatchResult
    {
        FileTypeMatchResult result;
        if (!index) {
            return result;
        }

        result.matchCache.assign(index->nodeCount, 0);

        if (matchVirtual) {
            int i = 0;
            for (FileNode* node : index->nodes) {
                if ((++i & 0xFFF) == 0 && cancelToken->load(std::memory_order_relaxed)) {
                    result.cancelled = true;
                    return result;
                }
                if (node->isVirtual || (node->parent && node->parent->isVirtual)) {
                    if (node->id < result.matchCache.size()) {
                        result.matchCache[node->id] |= kTypeSelfMatch;
                    }
                }
            }
        } else {
            const auto it = index->filesByExt.constFind(matchNoExt ? uint64_t{0} : typeKey);
            if (it != index->filesByExt.constEnd()) {
                for (FileNode* node : it.value()) {
                    if (cancelToken->load(std::memory_order_relaxed)) {
                        result.cancelled = true;
                        return result;
                    }
                    if (node->id < result.matchCache.size()) {
                        result.matchCache[node->id] |= kTypeSelfMatch;
                    }
                }
            }
        }

        for (auto it = index->nodes.rbegin(); it != index->nodes.rend(); ++it) {
            FileNode* node = *it;
            if (node->id < result.matchCache.size() && result.matchCache[node->id] != 0 && node->parent) {
                if (node->parent->id < result.matchCache.size()) {
                    result.matchCache[node->parent->id] |= kTypeSubtreeMatch;
                }
            }
        }

        return result;
    }));
}

void TreemapWidget::onFileTypeMatchTaskFinished()
{
    emit fileTypeHighlightBusyChanged(false);
    if (m_pendingHighlightedFileType != m_highlightedFileType) {
        return;
    }

    const FileTypeMatchResult result = m_fileTypeWatcher->result();
    if (result.cancelled) {
        return; // task was cancelled — a newer highlight is already in flight
    }

    m_fileTypeMatchCache = result.matchCache;

    m_previousHighlightedFileType = m_highlightedFileType;
    rebuildCombinedMatchCache();
    viewport()->update();
}

void TreemapWidget::rebuildCombinedMatchCache()
{
    // Combined cache applies AND semantics when both name/size search and file type
    // filter are active at the same time. When only one is active, painting uses the
    // individual caches directly via effectiveMatchFlags(), so we just clear here.
    const bool bothActive = m_searchActive && !m_highlightedFileType.isEmpty();
    if (!bothActive || !m_searchIndex || m_nodeCount == 0) {
        m_combinedMatchCache.assign(m_nodeCount, 0);
        return;
    }

    m_combinedMatchCache.assign(m_nodeCount, 0);

    // Pass 1 (search reachability): use the pre-built m_searchReachCache computed in
    // onSearchTaskFinished — no allocation or traversal needed here.
    const std::vector<bool>& searchReach = m_searchReachCache;

    // Pass 2: mark combined self-matches. Only files can be direct combined matches
    // (directories are never typed), so directories are handled entirely by pass 3.
    for (FileNode* node : m_searchIndex->nodes) {
        if (node->isDirectory || node->id >= m_nodeCount) continue;
        if ((m_fileTypeMatchCache[node->id] & kTypeSelfMatch)
            && node->id < searchReach.size() && searchReach[node->id])
            m_combinedMatchCache[node->id] = kTypeSelfMatch;
    }

    // Pass 3 (reverse / bottom-up DFS): propagate subtree match upward.
    for (auto it = m_searchIndex->nodes.rbegin(); it != m_searchIndex->nodes.rend(); ++it) {
        FileNode* node = *it;
        if (node->id >= m_nodeCount) continue;
        if (m_combinedMatchCache[node->id] != 0 && node->parent && node->parent->id < m_nodeCount)
            m_combinedMatchCache[node->parent->id] |= kTypeSubtreeMatch;
    }
}

void TreemapWidget::setSizeFilter(qint64 minBytes, qint64 maxBytes)
{
    const bool wasActive = m_sizeFilterActive;
    const bool newActive = (minBytes > 0 || maxBytes > 0);

    if (newActive == wasActive && m_minSizeFilter == minBytes && m_maxSizeFilter == maxBytes) {
        return;
    }

    clearHoverState(false);
    m_minSizeFilter = minBytes;
    m_maxSizeFilter = maxBytes;
    m_sizeFilterActive = newActive;
    m_searchActive = !m_searchPattern.isEmpty() || m_sizeFilterActive;

    rebuildSearchMatches();
    viewport()->update();
}


bool TreemapWidget::isDescendantOf(const FileNode* node, const FileNode* ancestor) const
{
    if (!node || !ancestor) {
        return false;
    }

    for (const FileNode* current = node; current; current = current->parent) {
        if (current == ancestor) {
            return true;
        }
    }

    return false;
}

void TreemapWidget::sortChildrenBySize(FileNode* node)
{
    if (!node) return;
    std::sort(node->children.begin(), node->children.end(),
              [](const FileNode* a, const FileNode* b) {
                  return a->size > b->size;
              });
    for (FileNode* child : node->children)
        sortChildrenBySize(child);
}

QString TreemapWidget::cachedSizeLabel(const FileNode* node) const
{
    if (!node) {
        return {};
    }

    auto it = m_sizeLabelCache.constFind(node);
    if (it != m_sizeLabelCache.cend()) {
        return *it;
    }

    const QString label = m_systemLocale.formattedDataSize(node->size);
    if (m_sizeLabelCache.size() > kMaxCacheEntries)
        m_sizeLabelCache.clear();
    m_sizeLabelCache.insert(node, label);
    return label;
}

QString TreemapWidget::cachedElidedLabel(const FileNode* node, const QString& text, int width,
                                         const QFontMetrics& metrics, quint64 fontKey,
                                         Qt::LayoutDirection direction) const
{
    return cachedElidedLabelWithBucket(node, text, width, metrics, fontKey,
                                       static_cast<int>(kRevealWidthBucketPx), direction);
}

QString TreemapWidget::cachedElidedLabelWithBucket(const FileNode* node, const QString& text,
                                                   int width, const QFontMetrics& metrics,
                                                   quint64 fontKey, int bucket,
                                                   Qt::LayoutDirection direction) const
{
    if (!node || text.isEmpty() || width <= 0) {
        return {};
    }

    const int effectiveBucket = std::max(1, bucket);
    const quint64 nodeKey = static_cast<quint64>(reinterpret_cast<quintptr>(node));
    const quint64 textKey = static_cast<quint64>(qHash(text));
    const quint64 displayWidthKey = (nodeKey << 32)
        ^ (textKey << 1)
        ^ (fontKey << 24)
        ^ static_cast<quint64>(effectiveBucket);
    const int quantizedWidth = std::max(
        effectiveBucket, ((width + (effectiveBucket / 2)) / effectiveBucket) * effectiveBucket);
    width = quantizedWidth;
    if (auto it = m_elidedDisplayWidthCache.constFind(displayWidthKey);
            it != m_elidedDisplayWidthCache.cend()) {
        width = static_cast<int>(applyHysteresis(width, *it, std::max(2, effectiveBucket)));
    }
    if (m_elidedDisplayWidthCache.size() > kMaxCacheEntries) {
        m_elidedDisplayWidthCache.clear();
    }
    m_elidedDisplayWidthCache.insert(displayWidthKey, width);

    // Avoid caching labels that already fit: keeping the original text in the
    // cache adds memory pressure without reducing elision work.
    if (metrics.horizontalAdvance(text) <= width) {
        return text;
    }

    const quint64 key = (nodeKey << 21)
        ^ (textKey << 1)
        ^ static_cast<quint64>(width & 0x1fffff)
        ^ (fontKey << 9)
        ^ static_cast<quint64>(effectiveBucket);
    auto it = m_elidedTextCache.constFind(key);
    if (it != m_elidedTextCache.cend()) {
        return *it;
    }

    const QString elided = metrics.elidedText(text, elideModeForDirection(direction), width);
    if (m_elidedTextCache.size() > kMaxCacheEntries)
        m_elidedTextCache.clear();
    m_elidedTextCache.insert(key, elided);
    return elided;
}

void TreemapWidget::layoutVisibleChildren(FileNode* node, const QRectF& tileViewRect,
                                          const QRectF& viewContent,
                                          const QRectF& visibleClip,
                                          std::vector<std::pair<FileNode*, QRectF>>& out) const
{
    out.clear();
    if (!node || !node->isDirectory || node->children.empty()
            || viewContent.width() <= 0.0 || viewContent.height() <= 0.0) {
        return;
    }

    // Check the split cache before building the children vector — on cache hits
    // (every frame after first render) this avoids an unnecessary heap allocation
    // and a full pass over node->children.
    auto cacheIt = m_liveSplitCache.find(node);
    if (cacheIt == m_liveSplitCache.end()) {
        // Cache miss: build children vector and compute the normalised layout.
        // Cache the layout in normalized [0,1]×[0,1] space so it can be reused
        // on every subsequent call, only scaling to the current viewContent
        // dimensions.  This locks in split directions on first render so they
        // can never flip during a zoom animation.
        std::vector<FileNode*> children;
        children.reserve(node->children.size());
        qint64 total = 0;
        for (const auto& child : node->children) {
            if (child->size > 0) {
                children.push_back(child);
                total += child->size;
            }
        }
        if (children.empty() || total <= 0) {
            return;
        }

        std::sort(children.begin(), children.end(),
                  [](const FileNode* a, const FileNode* b) {
                      if (a->size != b->size) {
                          return a->size > b->size;
                      }
                      return a->name < b->name;
                  });

        // Compute the aspect ratio of the tile in view space.  The AR is used
        // as the width of the normalised layout rect so the squarified algorithm
        // targets square tiles in the actual viewport.
        //
        // For the current root node we use the scroll-bar-neutral widget
        // dimensions so the layout AR does not change when scrollbars appear or
        // disappear.  Using the raw viewport() dimensions would cause the AR
        // to shift by the scrollbar width/height whenever scrollbars toggle,
        // which flips split directions when the cache is cleared (e.g. on
        // navigate-back).  Tiles are still rendered into the actual (possibly
        // smaller) viewport; the negligible AR discrepancy is invisible.
        //
        // For non-current nodes, we compute the true, zoom-invariant physical aspect ratio
        // by looking up the child's raw logical proportions in the parent's normalized cache. 
        // This ensures the layout is strictly determined by the topological tree structure 
        // and currentRootLayoutAspectRatio, completely ignoring the dynamically shifting 
        // proportions of tileViewRect as headers physically appear or disappear with zoom.
        qreal tileAr;
        if (node == m_current) {
            tileAr = m_currentRootLayoutAspectRatio;
        } else {
            bool foundInParent = false;
            if (node->parent) {
                auto pIt = m_liveSplitCache.find(node->parent);
                if (pIt != m_liveSplitCache.end()) {
                    for (const auto& pair : pIt->rects) {
                        if (pair.first == node) {
                            const qreal n_ar = (pair.second.height() > 0.0) 
                                ? (pair.second.width() / pair.second.height()) 
                                : 1.0;
                            tileAr = n_ar / std::pow(3.0, m_settings.tileAspectBias);
                            foundInParent = true;
                            break;
                        }
                    }
                }
            }
            if (!foundInParent) {
                tileAr = (tileViewRect.height() > 0.0)
                    ? tileViewRect.width() / tileViewRect.height()
                    : 1.0;
            }
        }
        const qreal ar = tileAr * std::pow(3.0, m_settings.tileAspectBias);
        std::vector<std::pair<FileNode*, QRectF>> normalized;
        squarifiedLayout(children, QRectF(0, 0, ar, 1.0), total, normalized);
        cacheIt = m_liveSplitCache.insert(node, SplitCacheEntry{ar, std::move(normalized)});
    }

    const qreal storedAr = cacheIt->aspectRatio;
    // Propagate continuous child view rects through layout and let each paint path
    // snap only the geometry it actually rasterizes. Pre-snapping here makes text
    // and reveal thresholds inherit tile-edge step changes during panning.
    for (const auto& [child, n] : cacheIt->rects) {
        const qreal left   = viewContent.x() + (n.x() / storedAr) * viewContent.width();
        const qreal top    = viewContent.y() + n.y() * viewContent.height();
        const qreal right  = viewContent.x() + ((n.x() + n.width()) / storedAr) * viewContent.width();
        const qreal bottom = viewContent.y() + (n.y() + n.height()) * viewContent.height();
        const QRectF childViewRect(left, top, right - left, bottom - top);
        if (childViewRect.intersects(visibleClip)) {
            out.emplace_back(child, childViewRect);
        }
    }
}

qreal TreemapWidget::computeCurrentRootLayoutAspectRatio() const
{
    qreal vpW = viewport()->width();
    qreal vpH = viewport()->height();
    return (vpH > 0.0) ? (vpW / vpH) : 1.0;
}

void TreemapWidget::updateScrollBarOverlayStyle()
{
    if (m_overlayHScrollBar) {
        const QString style = treemapScrollBarStyleSheet(palette(), m_overlayHExpandProgress);
        if (m_overlayHScrollBar->styleSheet() != style) {
            m_overlayHScrollBar->setStyleSheet(style);
        }
    }
    if (m_overlayVScrollBar) {
        const QString style = treemapScrollBarStyleSheet(palette(), m_overlayVExpandProgress);
        if (m_overlayVScrollBar->styleSheet() != style) {
            m_overlayVScrollBar->setStyleSheet(style);
        }
    }
}

void TreemapWidget::setOverlayScrollBarExpanded(QScrollBar* bar, bool expanded)
{
    QVariantAnimation* animation = nullptr;
    qreal current = 0.0;
    if (bar == m_overlayHScrollBar) {
        animation = &m_overlayHStateAnimation;
        current = m_overlayHExpandProgress;
    } else if (bar == m_overlayVScrollBar) {
        animation = &m_overlayVStateAnimation;
        current = m_overlayVExpandProgress;
    } else {
        return;
    }

    const qreal target = expanded ? 1.0 : 0.0;
    if (std::abs(current - target) < 0.001 && animation->state() != QAbstractAnimation::Running) {
        return;
    }
    animation->stop();
    animation->setStartValue(current);
    animation->setEndValue(target);
    animation->start();
}

void TreemapWidget::setOverlayScrollBarVisible(QScrollBar* bar, bool visible)
{
    QGraphicsOpacityEffect* effect = nullptr;
    QPropertyAnimation* animation = nullptr;
    bool* visibleFlag = nullptr;
    if (bar == m_overlayHScrollBar) {
        effect = m_overlayHOpacityEffect;
        animation = &m_overlayHOpacityAnimation;
        visibleFlag = &m_overlayHShouldBeVisible;
    } else if (bar == m_overlayVScrollBar) {
        effect = m_overlayVOpacityEffect;
        animation = &m_overlayVOpacityAnimation;
        visibleFlag = &m_overlayVShouldBeVisible;
    } else {
        return;
    }

    if (*visibleFlag == visible && animation->state() != QAbstractAnimation::Running) {
        if (!visible && effect->opacity() <= 0.01) {
            bar->hide();
        }
        return;
    }
    *visibleFlag = visible;
    animation->stop();
    if (visible) {
        bar->show();
        bar->raise();
    }
    animation->setStartValue(effect->opacity());
    animation->setEndValue(visible ? 1.0 : 0.0);
    animation->start();
}

void TreemapWidget::updateOverlayScrollBarGeometry()
{
    if (!m_overlayHScrollBar || !m_overlayVScrollBar || !viewport()) {
        return;
    }

    const QRect vp = viewport()->rect();
    const bool showH = m_overlayHScrollBar->isVisible();
    const bool showV = m_overlayVScrollBar->isVisible();
    const int horizontalWidth = std::max(0, vp.width() - (kOverlayScrollBarEndMargin * 2)
                                            - (showV ? (kOverlayScrollBarThickness + kOverlayScrollBarInset) : 0));
    const int verticalHeight = std::max(0, vp.height() - (kOverlayScrollBarEndMargin * 2)
                                           - (showH ? (kOverlayScrollBarThickness + kOverlayScrollBarInset) : 0));

    m_overlayHScrollBar->setGeometry(kOverlayScrollBarEndMargin,
                                     vp.height() - kOverlayScrollBarThickness - kOverlayScrollBarInset,
                                     horizontalWidth,
                                     kOverlayScrollBarThickness);
    m_overlayVScrollBar->setGeometry(vp.width() - kOverlayScrollBarThickness - kOverlayScrollBarInset,
                                     kOverlayScrollBarEndMargin,
                                     kOverlayScrollBarThickness,
                                     verticalHeight);
    m_overlayHScrollBar->raise();
    m_overlayVScrollBar->raise();
}

QSizeF TreemapWidget::stabilizedNodeSize(const FileNode* node, StableMetricChannel channel,
                                         const QSizeF& size, qreal widthBucket,
                                         qreal heightBucket, qreal widthHysteresis,
                                         qreal heightHysteresis) const
{
    const qreal quantizedWidth = stableLabelMetric(size.width(), widthBucket);
    const qreal quantizedHeight = stableLabelMetric(size.height(), heightBucket);
    if (!node) {
        return QSizeF(quantizedWidth, quantizedHeight);
    }

    const quint64 nodeKey = static_cast<quint64>(reinterpret_cast<quintptr>(node));
    const quint64 channelKey = static_cast<quint64>(channel) << 56;
    const quint64 cacheKey = nodeKey ^ channelKey;
    if (auto it = m_stableMetricCache.constFind(cacheKey); it != m_stableMetricCache.cend()) {
        const QSizeF stabilized = applyAxisHysteresis(QSizeF(quantizedWidth, quantizedHeight), *it,
                                                      widthHysteresis, heightHysteresis);
        m_stableMetricCache.insert(cacheKey, stabilized);
        return stabilized;
    }

    const QSizeF stabilized(quantizedWidth, quantizedHeight);
    m_stableMetricCache.insert(cacheKey, stabilized);
    return stabilized;
}

bool TreemapWidget::canPaintChildrenForDisplay(const FileNode* node, const QRectF& viewBounds, int depth) const
{
    if (depth >= m_activeSemanticDepth) {
        return false;
    }

    const RevealThresholds thresholds = revealThresholds(m_settings);
    const QSizeF size = viewBounds.size();
    const qreal dpBias = 1.0 / pixelScale();
    return size.width()  >= thresholds.childStartWidth  - dpBias
        && size.height() >= thresholds.childStartHeight - dpBias;
}

qreal TreemapWidget::tileRevealOpacityForNode(const FileNode* node, const QRectF& layoutArea) const
{
    Q_UNUSED(node);
    const RevealThresholds thresholds = revealThresholds(m_settings);
    return revealOpacityForSize(layoutArea.size(),
                                thresholds.childStartWidth, thresholds.childStartHeight,
                                thresholds.childFullWidth, thresholds.childFullHeight);
}

qreal TreemapWidget::tinyChildRevealOpacityForLayout(const FileNode* node, const QRectF& layoutArea) const
{
    Q_UNUSED(node);
    const RevealThresholds thresholds = revealThresholds(m_settings);
    const qreal tinyStart = std::max<qreal>(1.0, m_settings.minTileSize);
    const qreal tinyFullWidth = std::max<qreal>(tinyStart, thresholds.childStartWidth);
    const qreal tinyFullHeight = std::max<qreal>(tinyStart, thresholds.childStartHeight);
    if (tinyFullWidth <= tinyStart && tinyFullHeight <= tinyStart) {
        return 0.0;
    }

    return revealOpacityForSize(layoutArea.size(),
                                tinyStart, tinyStart,
                                tinyFullWidth, tinyFullHeight);
}

qreal TreemapWidget::childRevealOpacityForLayout(const FileNode* node, const QRectF& layoutArea,
                                                 int childDepth) const
{
    const qreal sizeFade = tileRevealOpacityForNode(node, layoutArea);

    qreal semanticFade = 1.0;
    if (childDepth > m_settings.baseVisibleDepth) {
        const qreal revealDepth = m_settings.baseVisibleDepth
            + std::max<qreal>(0.0,
                              std::log2(std::max<qreal>(1.0, m_cameraScale))
                                  * m_settings.depthRevealPerZoomDoubling);
        semanticFade = smoothstep(std::clamp(revealDepth - childDepth, 0.0, 1.0));
    }

    return std::min(sizeFade, semanticFade);
}

qreal TreemapWidget::folderDetailOpacityForNode(const FileNode* node, const QRectF& bounds) const
{
    const RevealThresholds thresholds = revealThresholds(m_settings);
    const QSizeF stabilizedSize = stabilizedNodeSize(
        node, StableMetricChannel::FolderDetail, bounds.size(),
        kRevealWidthBucketPx, kRevealHeightBucketPx,
        kRevealWidthBucketPx, kRevealHeightBucketPx);

    const qreal detailFadeW = smoothstep(std::clamp(
        (stabilizedSize.width()  - (thresholds.detailWidth  - 8.0)) / 8.0, 0.0, 1.0));
    const qreal detailFadeH = smoothstep(std::clamp(
        (stabilizedSize.height() - (thresholds.detailHeight - 8.0)) / 8.0, 0.0, 1.0));
    return std::min(detailFadeW, detailFadeH);
}

bool TreemapWidget::suppressHoverForTinyTranslucentLeaf(const FileNode* node, const QRectF& rect) const
{
    return node
        && !node->isDirectory
        && QColor::fromRgba(node->color).alphaF() < 1.0
        && tileRevealOpacityForNode(node, rect) <= 0.0;
}

QRect TreemapWidget::hoverDirtyRectForNode(const FileNode* node, const QRectF& rect) const
{
    if (rect.isEmpty()) {
        return QRect();
    }
    const int padding = (node && !node->isDirectory) ? 0 : 4;
    return expandedDirtyRect(rect.toAlignedRect(), viewport()->rect(), padding);
}

QPointF TreemapWidget::maxCameraOriginForScale(qreal scale) const
{
    const qreal clampedScale = std::clamp(scale, kCameraMinScale, m_settings.cameraMaxScale);
    const qreal visibleWidth = viewport()->width() / clampedScale;
    const qreal visibleHeight = viewport()->height() / clampedScale;
    return QPointF(
        std::max<qreal>(0.0, viewport()->width() - visibleWidth),
        std::max<qreal>(0.0, viewport()->height() - visibleHeight));
}

QPointF TreemapWidget::clampCameraOrigin(const QPointF& origin, qreal scale) const
{
    const QPointF maxOrigin = maxCameraOriginForScale(scale);
    return QPointF(
        std::clamp(origin.x(), 0.0, maxOrigin.x()),
        std::clamp(origin.y(), 0.0, maxOrigin.y()));
}

qreal TreemapWidget::pixelScale() const
{
    return normalizedPixelScale(viewport() ? viewport()->devicePixelRatioF() : devicePixelRatioF());
}

QRectF TreemapWidget::sceneToViewRectF(const QRectF& rect) const
{
    return QRectF(
        (rect.x() - m_cameraOrigin.x()) * m_cameraScale,
        (rect.y() - m_cameraOrigin.y()) * m_cameraScale,
        rect.width() * m_cameraScale,
        rect.height() * m_cameraScale);
}

void TreemapWidget::animateCameraTo(qreal scale, const QPointF& origin,
                                    const QPointF& focusScenePos,
                                    const QPointF& focusScreenPos,
                                    bool useFocusAnchor)
{
    const qreal clampedScale = std::clamp(scale, kCameraMinScale, m_settings.cameraMaxScale);
    const QPointF clampedOrigin = clampCameraOrigin(origin, clampedScale);

    m_cameraAnimation.stop();
    m_cameraAnimation.setDuration(m_settings.cameraDurationMs);
    m_cameraAnimation.setEasingCurve(QEasingCurve::OutCubic);
    m_cameraStartScale = m_cameraScale;
    m_cameraStartOrigin = m_cameraOrigin;
    m_cameraTargetScale = clampedScale;
    m_cameraTargetOrigin = clampedOrigin;
    m_cameraFocusScenePos = focusScenePos;
    m_cameraFocusScreenPos = focusScreenPos;
    m_cameraUseFocusAnchor = useFocusAnchor;

    if (qFuzzyCompare(m_cameraStartScale, m_cameraTargetScale)
            && m_cameraStartOrigin == m_cameraTargetOrigin) {
        return;
    }

    if (m_settings.fastWheelZoom && m_current && !viewport()->size().isEmpty()) {
        m_cameraPreviousFrame = renderSceneToPixmap(m_current).copy();
        
        const qreal savedScale = m_cameraScale;
        const QPointF savedOrigin = m_cameraOrigin;
        const int savedDepth = m_activeSemanticDepth;
        
        m_cameraScale = m_cameraTargetScale;
        m_cameraOrigin = m_cameraTargetOrigin;
        m_activeSemanticDepth = desiredSemanticDepthForScale(m_cameraTargetScale);
        
        m_cameraNextFrame = renderSceneToPixmap(m_current).copy();
        
        m_cameraScale = savedScale;
        m_cameraOrigin = savedOrigin;
        m_activeSemanticDepth = savedDepth;
    }

    m_cameraAnimation.start();
}

void TreemapWidget::setCameraImmediate(qreal scale, const QPointF& origin)
{
    m_cameraScale = scale;
    m_cameraOrigin = snapCameraOriginToPixelGrid(origin, scale, pixelScale());
    m_cameraStartScale = scale;
    m_cameraTargetScale = scale;
    m_cameraStartOrigin = m_cameraOrigin;
    m_cameraTargetOrigin = m_cameraOrigin;
    m_cameraFocusScenePos = QPointF();
    m_cameraFocusScreenPos = QPointF();
    m_cameraUseFocusAnchor = false;
}

bool TreemapWidget::continuousZoomGeometryActive() const
{
    return m_cameraAnimation.state() == QAbstractAnimation::Running
        || m_continuousZoomSettleFramesRemaining > 0;
}

void TreemapWidget::resetCamera()
{
    m_cameraScale = 1.0;
    m_cameraStartScale = 1.0;
    m_cameraTargetScale = 1.0;
    m_cameraOrigin = QPointF();
    m_cameraStartOrigin = QPointF();
    m_cameraTargetOrigin = QPointF();
    m_cameraFocusScenePos = QPointF();
    m_cameraFocusScreenPos = QPointF();
    m_cameraUseFocusAnchor = false;
    m_semanticLiveRoot = nullptr;
}

void TreemapWidget::syncScrollBars()
{
    const QSize vpSize = viewport()->size();
    const bool zoomed = m_cameraScale > (kCameraMinScale + 0.0001);
    const int maxH = zoomed ? std::max(0, static_cast<int>(std::round(vpSize.width()  * m_cameraScale - vpSize.width()))) : 0;
    const int maxV = zoomed ? std::max(0, static_cast<int>(std::round(vpSize.height() * m_cameraScale - vpSize.height()))) : 0;
    const int hVal = std::clamp(static_cast<int>(std::round(m_cameraOrigin.x() * m_cameraScale)), 0, maxH);
    const int vVal = std::clamp(static_cast<int>(std::round(m_cameraOrigin.y() * m_cameraScale)), 0, maxV);
    const Qt::ScrollBarPolicy newHorizontalPolicy = Qt::ScrollBarAlwaysOff;
    const Qt::ScrollBarPolicy newVerticalPolicy = Qt::ScrollBarAlwaysOff;
    const int newHorizontalPageStep = vpSize.width();
    const int newVerticalPageStep = vpSize.height();
    const int newHorizontalSingleStep = std::max(16, vpSize.width() / 12);
    const int newVerticalSingleStep = std::max(16, vpSize.height() / 12);

    m_syncingScrollBars = true;
    m_lastHScrollBar.policy = newHorizontalPolicy;
    m_lastVScrollBar.policy = newVerticalPolicy;
    {
        QSignalBlocker b(m_overlayHScrollBar);
        if (m_lastHScrollBar.range != maxH) {
            m_overlayHScrollBar->setRange(0, maxH);
            m_lastHScrollBar.range = maxH;
        }
        if (m_lastHScrollBar.pageStep != newHorizontalPageStep) {
            m_overlayHScrollBar->setPageStep(newHorizontalPageStep);
            m_lastHScrollBar.pageStep = newHorizontalPageStep;
        }
        if (m_lastHScrollBar.singleStep != newHorizontalSingleStep) {
            m_overlayHScrollBar->setSingleStep(newHorizontalSingleStep);
            m_lastHScrollBar.singleStep = newHorizontalSingleStep;
        }
        if (m_lastHScrollBar.value != hVal) {
            m_overlayHScrollBar->setValue(hVal);
            m_lastHScrollBar.value = hVal;
        }
    }
    {
        QSignalBlocker b(m_overlayVScrollBar);
        if (m_lastVScrollBar.range != maxV) {
            m_overlayVScrollBar->setRange(0, maxV);
            m_lastVScrollBar.range = maxV;
        }
        if (m_lastVScrollBar.pageStep != newVerticalPageStep) {
            m_overlayVScrollBar->setPageStep(newVerticalPageStep);
            m_lastVScrollBar.pageStep = newVerticalPageStep;
        }
        if (m_lastVScrollBar.singleStep != newVerticalSingleStep) {
            m_overlayVScrollBar->setSingleStep(newVerticalSingleStep);
            m_lastVScrollBar.singleStep = newVerticalSingleStep;
        }
        if (m_lastVScrollBar.value != vVal) {
            m_overlayVScrollBar->setValue(vVal);
            m_lastVScrollBar.value = vVal;
        }
    }
    setOverlayScrollBarVisible(m_overlayHScrollBar, maxH > 0);
    setOverlayScrollBarVisible(m_overlayVScrollBar, maxV > 0);
    updateOverlayScrollBarGeometry();
    m_syncingScrollBars = false;
}

void TreemapWidget::applyScrollBarCameraPosition()
{
    if (m_syncingScrollBars || m_zoomAnimation.state() == QAbstractAnimation::Running) {
        return;
    }
    const qreal scale = std::max<qreal>(m_cameraScale, kCameraMinScale);
    const QPointF scrollOrigin(m_overlayHScrollBar->value() / scale,
                               m_overlayVScrollBar->value() / scale);
    const QPointF clamped = snapCameraOriginToPixelGrid(
        clampCameraOrigin(scrollOrigin, scale), scale, pixelScale());
    if (QLineF(clamped, m_cameraOrigin).length() < 0.01) {
        return;
    }
    m_cameraAnimation.stop();
    m_cameraOrigin = clamped;
    m_cameraStartOrigin = clamped;
    m_cameraTargetOrigin = clamped;
    viewport()->update();
}

void TreemapWidget::drawScene(QPainter& painter, FileNode* root, const QRectF& visibleClip)
{
    const QRect full = viewport()->rect();
    if (!root) {
        painter.fillRect(full, palette().color(QPalette::Window));
        return;
    }

    m_framePalette = palette();
    m_framePixelScale   = pixelScale();
    m_frameOutlineWidth = snapLengthToPixels(m_settings.border, m_framePixelScale);

    // The current folder owns the full viewport. Descendant geometry is derived
    // recursively from that root-bleed rect, clipped to the requested visible area.
    const QRectF clip = visibleClip.isValid() ? visibleClip : QRectF(full);
    painter.setClipRect(full, Qt::IntersectClip);
    paintNode(painter, root, -1, clip, currentRootViewRect());
}

void TreemapWidget::drawSceneScaled(QPainter& painter, FileNode* root, const QRectF& targetRect, qreal opacity)
{
    if (!root || targetRect.width() <= 0.0 || targetRect.height() <= 0.0
            || viewport()->width() <= 0 || viewport()->height() <= 0) {
        return;
    }

    painter.save();
    painter.setOpacity(painter.opacity() * opacity);
    painter.translate(targetRect.x(), targetRect.y());
    painter.scale(targetRect.width() / viewport()->width(), targetRect.height() / viewport()->height());
    const QRectF fullClip(0, 0, viewport()->width(), viewport()->height());
    drawScene(painter, root, fullClip);
    drawMatchOverlay(painter, root, fullClip);
    painter.restore();
}

void TreemapWidget::drawMatchOverlay(QPainter& painter, FileNode* root, const QRectF& visibleClip)
{
    const bool anyFilterActive = m_searchActive || !m_highlightedFileType.isEmpty();
    if (!root || !anyFilterActive) {
        return;
    }

    const QRectF clip = visibleClip.isValid() ? visibleClip : QRectF(viewport()->rect());

    painter.save();
    painter.setPen(Qt::NoPen);
    paintMatchOverlayNode(painter, root, -1, clip, currentRootViewRect());
    // Use searchMatchFlags for borders as they represent the primary focus.
    paintMatchBordersNode(painter, root, -1, clip, currentRootViewRect(),
                          [this](const FileNode* n) { return searchMatchFlags(n); },
                          kSearchSelfMatch);
    painter.restore();
}


QPixmap TreemapWidget::renderSceneToPixmap(FileNode* root)
{
    if (viewport()->width() <= 0 || viewport()->height() <= 0) {
        return QPixmap();
    }

    const qreal dpr = pixelScale();
    const QSize deviceSize(
        std::max(1, static_cast<int>(std::ceil(viewport()->width() * dpr))),
        std::max(1, static_cast<int>(std::ceil(viewport()->height() * dpr))));

    if (root == m_current) {
        m_lastLiveRoot = root;
        m_lastLiveOrigin = m_cameraOrigin;
        m_lastLiveScale = m_cameraScale;
        m_lastLiveDepth = m_activeSemanticDepth;
    } else {
        // Rendering a transition frame for another root: this invalidates
        // the live cache so the next paintEvent knows it must redraw.
        m_lastLiveRoot = nullptr;
    }

    if (m_liveFrame.isNull() || m_liveFrameDeviceSize != deviceSize) {
        m_liveFrame = QPixmap(deviceSize);
        m_liveFrame.setDevicePixelRatio(dpr);
        m_liveFrameDeviceSize = deviceSize;
    }

    QPainter painter(&m_liveFrame);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRectF fullClip(0, 0, viewport()->width(), viewport()->height());
    drawScene(painter, root, fullClip);
    drawMatchOverlay(painter, root, fullClip);
    return m_liveFrame;
}
QRectF TreemapWidget::zoomRectForAnchor(const QRectF& preferredRect, const QPointF& anchorPos) const
{
    const QRectF fullRect(QPointF(0, 0), QSizeF(viewport()->width(), viewport()->height()));
    if (!fullRect.isValid() || fullRect.isEmpty()) {
        return preferredRect;
    }

    qreal scale = 0.5;
    if (preferredRect.width() >= 8.0 && preferredRect.height() >= 8.0) {
        scale = std::max(preferredRect.width() / fullRect.width(),
                         preferredRect.height() / fullRect.height());
    }
    scale = std::clamp(scale, m_settings.wheelZoomMinScale, m_settings.wheelZoomMaxScale);

    QRectF anchoredRect(0.0, 0.0, fullRect.width() * scale, fullRect.height() * scale);

    const qreal minX = fullRect.left();
    const qreal maxX = fullRect.right();
    const qreal minY = fullRect.top();
    const qreal maxY = fullRect.bottom();
    const QPointF clampedAnchor(
        std::clamp(anchorPos.x(), minX, maxX),
        std::clamp(anchorPos.y(), minY, maxY));

    anchoredRect.moveCenter(clampedAnchor);
    if (anchoredRect.left() < fullRect.left()) {
        anchoredRect.moveLeft(fullRect.left());
    }
    if (anchoredRect.right() > fullRect.right()) {
        anchoredRect.moveRight(fullRect.right());
    }
    if (anchoredRect.top() < fullRect.top()) {
        anchoredRect.moveTop(fullRect.top());
    }
    if (anchoredRect.bottom() > fullRect.bottom()) {
        anchoredRect.moveBottom(fullRect.bottom());
    }

    return anchoredRect.intersected(fullRect);
}

void TreemapWidget::startZoomAnimation(const QPixmap& previousFrame,
                                       const QRectF& sourceRect, bool zoomIn,
                                       bool crossfadeOnly)
{
    m_zoomAnimation.stop();
    m_previousFrame = previousFrame;
    m_nextFrame = renderSceneToPixmap(m_current);
    m_zoomingIn = zoomIn;
    m_zoomCrossfadeOnly = crossfadeOnly;

    const QRectF fullRect(QPointF(0, 0), QSizeF(viewport()->width(), viewport()->height()));
    QRectF confinedSource = fullRect;
    if (!crossfadeOnly) {
        confinedSource = confineRectToBounds(sourceRect, fullRect);
        if (confinedSource.width() < 8.0 || confinedSource.height() < 8.0) {
            confinedSource = fullRect;
        }
    }
    // Snap the source rect to the device pixel grid so drawPixmap samples exactly
    // the region that the pixmap rendered the tile into — avoids a fractional-pixel
    // offset that makes the zoom transition appear to slide before it begins.
    m_zoomSourceRect = snapRectToPixels(confinedSource, pixelScale());
    m_zoomAnimation.start();
}

bool TreemapWidget::findVisibleViewRect(FileNode* root, const QRectF& rootViewRect,
                                        FileNode* target, QRectF* outRect, int depth) const
{
    if (!root || !target || !outRect || !rootViewRect.isValid() || rootViewRect.isEmpty()) {
        return false;
    }

    if (!canPaintChildrenForDisplay(root, rootViewRect, depth)) {
        return false;
    }

    const QRectF contentArea = childPaintRectForNode(root, rootViewRect);
    const QRectF contentClip = contentArea.adjusted(-1.0, -1.0, 1.0, 1.0);

    std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
    layoutVisibleChildren(root, rootViewRect, contentArea, contentClip, visibleChildren);
    for (const auto& [child, childViewRect] : visibleChildren) {
        if (childViewRect.width() < m_settings.minPaint || childViewRect.height() < m_settings.minPaint) {
            continue;
        }
        if (child == target) {
            *outRect = snapRectToPixels(childViewRect, pixelScale());
            return true;
        }
        if (!child->isDirectory) {
            continue;
        }
        if (findVisibleViewRect(child, childViewRect, target, outRect, depth + 1)) {
            return true;
        }
    }

    return false;
}

// ── Recursive paint ──────────────────────────────────────────────────────────

void TreemapWidget::paintNode(QPainter& p, FileNode* node, int depth,
                              const QRectF& visibleClip, const QRectF& viewRect,
                              qreal subtreeHoverBlend, qreal subtreePrevHoverBlend,
                              bool applyOwnReveal)
{
    const QRectF r = viewRect;
    const QRectF effectiveClip = r.intersected(visibleClip);
    if (r.width() < m_settings.minPaint || r.height() < m_settings.minPaint || effectiveClip.isEmpty())
        return;

    const bool liveCameraAnimating = continuousZoomGeometryActive();
    const QRectF ri = liveCameraAnimating ? r : snapRectToPixels(r, m_framePixelScale);
    const QRectF fillClipRect = liveCameraAnimating ? effectiveClip
                                                    : snapRectToPixels(effectiveClip, m_framePixelScale);
    const qreal outlineWidth = m_frameOutlineWidth;
    const qreal nodeHoverStrength = std::max(
        (node == m_hovered)         ? m_hoverBlend         : 0.0,
        (node == m_previousHovered) ? (1.0 - m_hoverBlend) : 0.0);
    // subtreeHoverStrength is passed down from parent instead of re-walking the
    // parent chain via isDescendantOf() on every node every frame.
    const qreal subtreeHoverStrength = std::min(subtreeHoverBlend + subtreePrevHoverBlend, 1.0);
    const qreal highlightOpacity = std::clamp(m_settings.highlightOpacity, 0.0, 1.0);
    const qreal subtreeHighlightStrength = subtreeHoverStrength * highlightOpacity;
    const qreal baseOpacity = p.opacity();

    // Compute child subtree hover blends once — passed down to every child call.
    // If this node IS the hovered directory, its children receive the hover blend;
    // otherwise the parent's inherited blend passes through unchanged.
    const qreal childSubtreeHoverBlend = (node == m_hovered && m_hovered && m_hovered->isDirectory)
        ? m_hoverBlend : subtreeHoverBlend;
    const qreal childSubtreePrevHoverBlend = (node == m_previousHovered && m_previousHovered && m_previousHovered->isDirectory)
        ? (1.0 - m_hoverBlend) : subtreePrevHoverBlend;

    if (node->isDirectory) {
        const qreal tileRevealOpacity = (node == m_current || !applyOwnReveal)
            ? 1.0
            : tileRevealOpacityForNode(node, r);
        if (tileRevealOpacity <= 0.0) {
            return;
        }

        p.save();
        const QColor folderBase = QColor::fromRgba(node->color);
        const qreal borderIntensity = std::clamp(m_settings.borderIntensity, 0.0, 1.0);
        const bool useLightBorder = shouldUseLightBorder(folderBase, m_settings.borderStyle);
        QColor vibrantTarget;
        {
            float h, s, l, a;
            folderBase.getHslF(&h, &s, &l, &a);
            const float boostedS = std::clamp(s * 1.25f, 0.0f, 1.0f);
            vibrantTarget.setHslF(h, boostedS, useLightBorder ? 0.92f : 0.10f, a);
        }
        const QColor outerBorderBase = blendColors(folderBase, vibrantTarget, borderIntensity);
        
        const QColor panelBase = cachedPanelBase(folderBase, m_framePalette.color(QPalette::Base));
        const QColor hoverBase = m_settings.highlightColor;
        const qreal nodeHighlightStrength = nodeHoverStrength * highlightOpacity;
        const qreal bgHighlightStrength = std::max(nodeHighlightStrength, subtreeHighlightStrength);
        const QColor colorBg = bgHighlightStrength > 0.0
            ? blendColors(folderBase, hoverBase, bgHighlightStrength) : folderBase;
        const QColor colorPanel = bgHighlightStrength > 0.0
            ? blendColors(panelBase, hoverBase, bgHighlightStrength) : panelBase;
        const QColor effectiveHeaderColor = nodeHoverStrength > 0.0
            ? blendColors(colorBg, hoverBase, nodeHoverStrength) : colorBg;
        const DirectoryRenderState directoryState = computeDirectoryRenderState(node, r, effectiveClip, depth);
        const QColor contentAreaColor = (node == m_current)
            ? colorPanel
            : (directoryState.showChrome
                ? blendColors(colorBg, colorPanel, directoryState.chromeOpacity)
                : colorBg);
        const QColor childBackgroundColor = contentAreaColor;
        const auto paintVisibleChildren = [&](const DirectoryRenderState& state) {
            p.save();
            p.setClipRect(state.childPaintRect, Qt::IntersectClip);

            std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
            layoutVisibleChildren(node, r, state.childLayoutRect, state.childContentClip, visibleChildren);
            for (const auto& [child, childRect] : visibleChildren) {
                if (!childRect.intersects(state.childContentClip)) {
                    continue;
                }

                const qreal childRevealOpacity = tileRevealOpacityForNode(child, childRect);
                const qreal childTinyOpacity = tinyChildRevealOpacityForLayout(child, childRect);

                // Use the maximum reveal opacity for the background fill to ensure a single,
                // continuous, non-popping base color for both tiny and detailed versions.
                const qreal childFillOpacity = std::max(childRevealOpacity, childTinyOpacity);
                if (childFillOpacity <= 0.0) {
                    continue;
                }

                // Calculate highlights for the background fill. We use the child subtree
                // blends calculated at the top of paintNode to ensure children correctly
                // inherit the highlight from a hovered parent.
                const qreal childSubtreeHighlightStrength = std::min(childSubtreeHoverBlend + childSubtreePrevHoverBlend, 1.0) * highlightOpacity;
                const bool suppressTinyLeafHover =
                    suppressHoverForTinyTranslucentLeaf(child, childRect);
                const qreal childNodeHoverStrength = std::max(
                    (child == m_hovered && !suppressTinyLeafHover)         ? m_hoverBlend         : 0.0,
                    (child == m_previousHovered && !suppressTinyLeafHover) ? (1.0 - m_hoverBlend) : 0.0);
                const qreal childBgHighlightStrength = std::max(
                    childNodeHoverStrength * highlightOpacity, childSubtreeHighlightStrength);

                const QColor fc = QColor::fromRgba(child->color);

                // Use the folder's panel color if it is a directory and chrome is revealing.
                // We blend from the saturated color to the panel color manually so we
                // can use our gamma-correct blendColors() instead of the muddy sRGB
                // alpha-blending provided by QPainter.
                QColor baseFill = fc;
                if (child->isDirectory) {
                    const qreal childNodeHighlightStrength = childNodeHoverStrength * highlightOpacity;
                    baseFill = childNodeHighlightStrength > 0.0
                        ? blendColors(fc, hoverBase, childNodeHighlightStrength)
                        : fc;
                }

                const QColor fillColor = childBgHighlightStrength > 0.0
                    ? blendColors(baseFill, hoverBase, childBgHighlightStrength) : baseFill;
                const QColor effectiveFillColor = (!child->isDirectory && fillColor.alphaF() < 1.0)
                    ? opaqueCompositeColor(childBackgroundColor, fillColor)
                    : fillColor;

                // Draw background at the calculated total opacity for this child
                paintTinyNodeFill(p, childRect, effectiveFillColor, m_framePixelScale,
                                 tileRevealOpacity * childFillOpacity);

                // If detailed features (chrome, labels, children) should show, recurse.
                if (childRevealOpacity > 0.0
                        && childRect.width() >= m_settings.minPaint
                        && childRect.height() >= m_settings.minPaint) {
                    p.save();
                    p.setOpacity(baseOpacity * tileRevealOpacity * childRevealOpacity);
                    paintNode(p, child, depth + 1, state.childContentClip, childRect,
                              childSubtreeHoverBlend, childSubtreePrevHoverBlend, false);
                    p.restore();
                }
            }
            p.restore();
        };

        if (node == m_current) {
            p.save();
            p.setClipRect(ri, Qt::IntersectClip);
            p.fillRect(ri, contentAreaColor);
            paintVisibleChildren(directoryState);
            p.restore();
            p.restore(); // for the initial p.save() at start of directory block
            return;
        }

        // Fast path for ancestor tiles whose chrome is entirely outside the visible
        // clip.  This is the common case when the camera is zoomed far in: the tile
        // is much larger than the viewport, so its header, border, and the fill
        // between the tile edge and content area are all off-screen.  Skip those and
        // only draw the content background and recurse into children.
        //
        // Detection: if contentFillClipRect (= childPaintRect ∩ effectiveClip) covers
        // essentially all of effectiveClip, then effectiveClip ⊆ childPaintRect, which
        // means the chrome ring outside childPaintRect is invisible.
        const bool chromeOutsideClip =
            directoryState.contentFillClipRect.width()  >= effectiveClip.width()  - 1.5
            && directoryState.contentFillClipRect.height() >= effectiveClip.height() - 1.5;

        if (chromeOutsideClip) {
            // Tile is so large chrome is entirely off-screen — fill body and recurse.
            if (directoryState.childPaintRect.width() > 1.0
                    && directoryState.childPaintRect.height() > 1.0) {
                p.save();
                p.setClipRect(directoryState.contentFillClipRect, Qt::IntersectClip);
                p.setOpacity(baseOpacity * tileRevealOpacity);
                p.fillRect(directoryState.childPaintRect, contentAreaColor);
                p.restore();
            }
            paintVisibleChildren(directoryState);
            p.restore(); // for the initial p.save() at start of directory block
            return;
        }

        const qreal highlightBorderStrength = std::max(nodeHoverStrength, subtreeHighlightStrength);
        const QRectF tileFillRect = ri;

        if (applyOwnReveal) {
            p.save();
            p.setClipRect(directoryState.tileFillClipRect, Qt::IntersectClip);
            p.setOpacity(baseOpacity * tileRevealOpacity);
            // Fill entire tile with the folder's base colour. The content area gets a
            // panel-colour overlay at chromeOpacity below, so chrome and body appear together.
            p.fillRect(tileFillRect, colorBg);
            p.restore();
        }

        if (directoryState.showChrome
                && directoryState.childPaintRect.width() > 1.0
                && directoryState.childPaintRect.height() > 1.0) {
            if (applyOwnReveal) {
                p.save();
                p.setClipRect(directoryState.contentFillClipRect, Qt::IntersectClip);
                p.setOpacity(baseOpacity * tileRevealOpacity);
                p.fillRect(directoryState.childPaintRect, contentAreaColor);
                p.restore();
            }
        }

        if (directoryState.showChrome) {
            p.save();
            p.setClipRect(directoryState.tileFillClipRect, Qt::IntersectClip);
            p.setOpacity(baseOpacity * tileRevealOpacity * directoryState.chromeOpacity);
            // Ring area (between tile and childPaintRect) is already colorBg from the base fill;
            // only the header needs its own fill to support the per-node hover highlight.
            p.fillRect(directoryState.framedHeaderRect, effectiveHeaderColor);

            if (depth >= 0) {
                const QColor baseContrast = contrastingTextColor(effectiveHeaderColor);
                const QColor hoverContrast = contrastingTextColor(hoverBase);
                const QColor headerTextColor = nodeHoverStrength > 0.0
                    ? blendColors(baseContrast, hoverContrast, nodeHoverStrength)
                    : baseContrast;
                const QString sizeText = cachedSizeLabel(node);
                const QString headerText = QString("%1  %2").arg(node->name, sizeText);
                constexpr qreal kHeaderTextInset = 3.0;
                const qreal availableHeaderSlack = std::max<qreal>(
                    0.0, (directoryState.framedHeaderRect.height() - m_headerFm.height()) * 0.5 - 1.0);
                const qreal headerVerticalInset = std::min<qreal>(
                    std::max<qreal>(0.0, m_settings.border),
                    availableHeaderSlack);
                const QRectF headerTextRect = strokeRectInside(
                        directoryState.framedHeaderRect.adjusted(
                        kHeaderTextInset,
                        headerVerticalInset,
                        -kHeaderTextInset,
                        -headerVerticalInset),
                    0.0);
                constexpr int kHeaderElideSafetyPx = 2;
                constexpr int kHeaderElideBucketPx = 3;
                const QSizeF stableHeaderSize = stabilizedNodeSize(
                    node, StableMetricChannel::HeaderLabel, headerTextRect.size(),
                    kRevealWidthBucketPx, kRevealHeightBucketPx,
                    kRevealWidthBucketPx, kRevealHeightBucketPx);
                const qreal stableHeaderWidth = stableHeaderSize.width();
                const qreal stableHeaderHeight = stableHeaderSize.height();
                const int headerWidth = std::max(
                    0, static_cast<int>(std::floor(
                        stableHeaderWidth)) - kHeaderElideSafetyPx);
                const qreal showHeaderWidth = 22.0;
                const qreal hideHeaderWidth = 18.0;
                const qreal showHeaderHeight = m_headerFm.height();
                const qreal hideHeaderHeight = std::max<qreal>(6.0, showHeaderHeight - 2.0);
                const bool headerWasVisible = m_headerLabelVisibleCache.value(node, false);
                const bool keepHeaderVisible = stableHeaderWidth >= hideHeaderWidth
                    && stableHeaderHeight >= hideHeaderHeight;
                const bool becomeHeaderVisible = stableHeaderWidth >= showHeaderWidth
                    && stableHeaderHeight >= showHeaderHeight;
                const bool headerVisible = headerWasVisible ? keepHeaderVisible : becomeHeaderVisible;
                m_headerLabelVisibleCache.insert(node, headerVisible);
                const qreal headerWidthSpan = std::max<qreal>(1.0, 44.0 - hideHeaderWidth);
                const qreal headerHeightSpan = std::max<qreal>(1.0, (showHeaderHeight + 2.0) - hideHeaderHeight);
                const qreal headerWidthFade = smoothstep(std::clamp(
                    (stableHeaderWidth - hideHeaderWidth) / headerWidthSpan, 0.0, 1.0));
                const qreal headerHeightFade = smoothstep(std::clamp(
                    (stableHeaderHeight - hideHeaderHeight) / headerHeightSpan, 0.0, 1.0));
                const qreal headerLabelFade = std::min(headerWidthFade, headerHeightFade);
                if (headerVisible && headerLabelFade > 0.0 && headerWidth > 0) {
                    p.setOpacity(baseOpacity * tileRevealOpacity * directoryState.chromeOpacity * headerLabelFade);
                    drawHeaderLabel(p, headerTextRect,
                                    cachedElidedLabelWithBucket(
                                        node, headerText, headerWidth, m_headerFm, 1, kHeaderElideBucketPx,
                                        layoutDirection()),
                                    m_headerFont, m_headerFm, headerTextColor, m_framePixelScale,
                                    layoutDirection());
                }
            }
            p.restore();
        }

        paintVisibleChildren(directoryState);

        if (outlineWidth > 0.0) {
            p.save();
            p.setOpacity(baseOpacity * tileRevealOpacity);
            // Highlight border matches header/hoverBase exactly
            const QColor highlightBorderBase = hoverBase;
            const QColor effectiveBorderColor = highlightBorderStrength > 0.0
                ? blendColors(outerBorderBase, highlightBorderBase, highlightBorderStrength)
                : outerBorderBase;
            p.setClipRect(directoryState.tileRect, Qt::IntersectClip);
            fillInnerBorder(p, directoryState.tileRect, effectiveBorderColor, outlineWidth);
            p.restore();
        }

        p.restore();
    } else {
        // ── File leaf — does not modify painter state the caller depends on
        const qreal tileRevealOpacity = applyOwnReveal ? tileRevealOpacityForNode(node, r) : 1.0;
        if (tileRevealOpacity <= 0.0) {
            return;
        }

        p.save();
        p.setClipRect(fillClipRect, Qt::IntersectClip);
        p.setOpacity(baseOpacity * tileRevealOpacity);
        const QColor fc = QColor::fromRgba(node->color);
        const QColor hoverBase = m_settings.highlightColor;
        const qreal nodeHighlightStrength = nodeHoverStrength * highlightOpacity;
        const qreal bgHighlightStrength = std::max(nodeHighlightStrength, subtreeHighlightStrength);
        QColor fillColor = bgHighlightStrength > 0.0
            ? blendColors(fc, hoverBase, bgHighlightStrength) : fc;
        if (fillColor.alphaF() < 1.0 && node->parent && node->parent->isDirectory) {
            const QColor parentBase = QColor::fromRgba(node->parent->color);
            const QColor parentPanelBase = cachedPanelBase(parentBase, m_framePalette.color(QPalette::Base));
            const QColor parentFillColor = subtreeHighlightStrength > 0.0
                ? blendColors(parentPanelBase, hoverBase, subtreeHighlightStrength)
                : parentPanelBase;
            fillColor = opaqueCompositeColor(parentFillColor, fillColor);
        }

        if (applyOwnReveal) {
            p.fillRect(ri, fillColor);
        }

        if (outlineWidth > 0.0) {
            const qreal clampedBorderIntensity = std::clamp(m_settings.borderIntensity, 0.0, 1.0);
            const bool useLightFileBorder = shouldUseLightBorder(fc, m_settings.borderStyle);
            QColor vibrantFileTarget;
            {
                float h, s, l, a;
                fc.getHslF(&h, &s, &l, &a);
                const float boostedS = std::clamp(s * 1.25f, 0.0f, 1.0f);
                vibrantFileTarget.setHslF(h, boostedS, useLightFileBorder ? 0.92f : 0.10f, a);
            }
            const QColor fileBorderBase = blendColors(fc, vibrantFileTarget, clampedBorderIntensity);
            
            const QColor highlightBorderBase = hoverBase;
            const QColor fileBorderColor = bgHighlightStrength > 0.0
                ? blendColors(fileBorderBase, highlightBorderBase, bgHighlightStrength)
                : fileBorderBase;

            QColor effectiveFileBorderColor = fileBorderColor;
            if (!m_settings.randomColorForUnknownFiles && fc.alphaF() < 1.0 && node->parent && node->parent->isDirectory) {
                const QColor parentBase = QColor::fromRgba(node->parent->color);
                const QColor parentPanelBase = cachedPanelBase(parentBase, m_framePalette.color(QPalette::Base));
                const QColor parentPanelColor = subtreeHighlightStrength > 0.0
                    ? blendColors(parentPanelBase, hoverBase, subtreeHighlightStrength)
                    : parentPanelBase;
                const qreal borderBlendOpacity = std::max<qreal>(0.20, fc.alphaF());
                effectiveFileBorderColor = blendColors(parentPanelColor, fileBorderColor, borderBlendOpacity);
            }
            fillInnerBorder(p, ri, effectiveFileBorderColor, outlineWidth);
        }

        // Use continuous r dimensions for the fade so pixel-snapping of ri
        // does not cause a hard 1-frame pop as tiles cross integer boundaries.
        const qreal fileTextInset = outlineWidth + 2.0;
        const QRectF fileTextRect = strokeRectInside(
            r.adjusted(fileTextInset, fileTextInset, -fileTextInset, -fileTextInset),
            0.0);
        const QSizeF stableFileLabelSize = stabilizedNodeSize(
            node, StableMetricChannel::FileLabel, fileTextRect.size(),
            kRevealWidthBucketPx, kRevealHeightBucketPx,
            kRevealWidthBucketPx, kRevealHeightBucketPx);
        const qreal stableWidth = stableFileLabelSize.width();
        const qreal stableHeight = stableFileLabelSize.height();
        const qreal showWidth = 13.0;
        const qreal hideWidth = 10.0;
        const qreal showHeight = m_fileFm.height() + 0.5;
        const qreal hideHeight = std::max<qreal>(4.0, showHeight - 3.0);
        const bool wasVisible = m_fileLabelVisibleCache.value(node, false);
        const bool keepVisible = stableWidth >= hideWidth && stableHeight >= hideHeight;
        const bool becomeVisible = stableWidth >= showWidth && stableHeight >= showHeight;
        const bool textVisible = wasVisible ? keepVisible : becomeVisible;
        m_fileLabelVisibleCache.insert(node, textVisible);
        const qreal widthSpan = std::max<qreal>(1.0, 40.0 - hideWidth);
        const qreal oneLineHeightSpan = std::max<qreal>(1.0, (showHeight + 6.0) - hideHeight);
        const qreal widthFade = smoothstep(std::clamp(
            (stableWidth - hideWidth) / widthSpan, 0.0, 1.0));
        const qreal oneLineHeightFade = smoothstep(std::clamp(
            (stableHeight - hideHeight) / oneLineHeightSpan, 0.0, 1.0));
        const qreal textFade = std::min(widthFade, oneLineHeightFade);
        if (textFade > 0.0) {
            // Replace nested save/restore with direct opacity arithmetic — the outer
            // file p.save()/p.restore() already guards all state for the caller.
            const QColor baseContrast = contrastingTextColor(fc);
            const QColor hoverContrast = contrastingTextColor(hoverBase);
            const QColor fileTextColor = bgHighlightStrength > 0.0
                ? blendColors(baseContrast, hoverContrast, bgHighlightStrength)
                : baseContrast;

            p.setOpacity(baseOpacity * tileRevealOpacity * textFade);
            p.setPen(fileTextColor);
            p.setFont(m_fileFont);

            // Keep the text box on the continuous layout rect. The draw helper snaps
            // the baseline internally so labels stay stable without inheriting tile-edge
            // snap steps from ri.
            constexpr int kFileElideSafetyPx = 3;
            constexpr int kFileElideBucketPx = 3;
            const int textWidth = std::max(
                1, static_cast<int>(std::floor(stableWidth)) - kFileElideSafetyPx);
            const QString nameText = cachedElidedLabelWithBucket(
                node, node->name, textWidth, m_fileFm, 2, kFileElideBucketPx,
                layoutDirection());

            const qreal twoLineMinHeight = m_fileFm.height() + m_fileFm.lineSpacing() + 1.0;
            const qreal stableTwoLineWidth = stableFileLabelSize.width();
            const qreal stableTwoLineHeight = stableFileLabelSize.height();
            const qreal sizeShowWidth = 36.0;
            const qreal sizeHideWidth = 32.0;
            const qreal sizeShowHeight = twoLineMinHeight;
            const qreal sizeHideHeight = std::max<qreal>(m_fileFm.height() + 2.0, sizeShowHeight - 3.0);
            const qreal sizeWidthSpan = std::max<qreal>(1.0, 64.0 - sizeHideWidth);
            const qreal sizeHeightSpan = std::max<qreal>(1.0, (sizeShowHeight + 6.0) - sizeHideHeight);
            const qreal sizeWidthFade = smoothstep(std::clamp(
                (stableTwoLineWidth - sizeHideWidth) / sizeWidthSpan, 0.0, 1.0));
            const qreal sizeHeightFade = smoothstep(std::clamp(
                (stableTwoLineHeight - sizeHideHeight) / sizeHeightSpan, 0.0, 1.0));
            const qreal sizeFade = std::min(sizeWidthFade, sizeHeightFade);
            if (sizeFade > 0.0) {
                const QString sizeText = cachedElidedLabelWithBucket(
                    node, cachedSizeLabel(node), textWidth, m_fileFm, 3, kFileElideBucketPx,
                    layoutDirection());
                p.setOpacity(baseOpacity * tileRevealOpacity * textFade);
                drawFileLabelLine(p, fileTextRect, nameText, 0,
                                  m_fileFont, m_fileFm, fileTextColor, m_framePixelScale,
                                  layoutDirection());
                p.setOpacity(baseOpacity * tileRevealOpacity * textFade * sizeFade);
                drawFileLabelLine(p, fileTextRect, sizeText, 1,
                                  m_fileFont, m_fileFm, fileTextColor, m_framePixelScale,
                                  layoutDirection());
            } else if (!nameText.isEmpty()) {
                drawFileLabelLine(p, fileTextRect, nameText, 0,
                                  m_fileFont, m_fileFm, fileTextColor, m_framePixelScale,
                                  layoutDirection());
            }
        }
        p.restore();
    }
}

void TreemapWidget::paintMatchOverlayNode(QPainter& painter, FileNode* node, int depth,
                                          const QRectF& visibleClip, const QRectF& viewRect) const
{
    if (!node) {
        return;
    }

    const QRectF r = viewRect;
    const qreal devicePixelScale = pixelScale();
    const QRectF effectiveClip = r.intersected(visibleClip);
    if (r.width() < m_settings.minPaint || r.height() < m_settings.minPaint || effectiveClip.isEmpty()) {
        return;
    }

    const quint8 eFlags = effectiveMatchFlags(node);
    const bool matched = (eFlags != 0);
    const bool isViewRoot = (node == m_current);

    // If we're starting a new search/filter and haven't found anything yet, don't dim
    // the root until the worker returns. This prevents the "brief dim" flicker.
    if (isViewRoot && !matched) {
        if (m_searchActive && m_searchWatcher->isRunning() && m_previousDirectSearchMatches.empty()) {
            return;
        }
        if (!m_highlightedFileType.isEmpty() && m_fileTypeWatcher->isRunning() && m_previousHighlightedFileType.isEmpty()) {
            return;
        }
    }

    // Direct match: node itself matched; subtree match: a descendant matched.
    const bool directMatch = (eFlags & kTypeSelfMatch);
    const bool subtreeMatch = (eFlags & kTypeSubtreeMatch);

    const QColor baseColor = palette().color(QPalette::Base);
    const QColor midColor = palette().color(QPalette::Mid);
    const QColor fadeBaseColor = blendColors(baseColor, midColor, 0.35);
    const QColor panelFadeColor(fadeBaseColor.red(), fadeBaseColor.green(), fadeBaseColor.blue(), 110);
    const QColor subtreeFadeColor(fadeBaseColor.red(), fadeBaseColor.green(), fadeBaseColor.blue(), 140);

    if (!matched) {
        // No match in this node's entire subtree. Dim it and return.
        painter.fillRect(snapRectToPixels(effectiveClip, devicePixelScale), subtreeFadeColor);
        return;
    }

    if (!node->isDirectory) {
        // Direct match on a file: it stays clear.
        return;
    }

    const bool showsChildren = canPaintChildrenForDisplay(node, r, depth);
    QRectF contentArea;
    QRectF contentClip;

    if (isViewRoot) {
        contentArea = r;
        contentClip = effectiveClip;
    } else {
        const bool detailedChrome = folderDetailOpacityForNode(node, r) > 0.0;
        const TileChromeGeometry chrome = makeTileChromeGeometry(
            r, m_settings, true, detailedChrome, devicePixelScale);
        contentArea = chrome.contentPaintRect;
        contentClip = contentArea.adjusted(-1.0, -1.0, 1.0, 1.0).intersected(effectiveClip);
        // Chrome (header) stays clear to help identify the path.
    }

    if (showsChildren) {
        std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
        layoutVisibleChildren(node, r, contentArea, contentClip, visibleChildren);

        // If it's the view root or a directory that is not a direct pattern match,
        // we want to dim everything except the matching children.
        // If it IS a direct match (e.g. folder name matches search), we preserve 
        // its contents as clear unless preserveDirectMatchSubtree is false.
        const bool preserveSubtree = !m_searchPattern.isEmpty() && !isViewRoot;
        if (isViewRoot || !directMatch || !preserveSubtree) {
            for (const auto& [child, childRect] : visibleChildren) {
                const QRectF childClip = childRect.intersected(contentClip);
                if (childClip.isEmpty()) continue;

                if (effectiveMatchFlags(child) == 0) {
                    painter.fillRect(snapRectToPixels(childClip, devicePixelScale), panelFadeColor);
                } else {
                    // Recurse into matching children.
                    paintMatchOverlayNode(painter, child, depth + 1, childClip, childRect);
                }
            }
        }
    } else if (!isViewRoot && !directMatch) {
        // Ancestor match on a folder too small to show children: it's part of a matching path.
    }
}

void TreemapWidget::paintMatchBordersNode(QPainter& painter, FileNode* node, int depth,
                                          const QRectF& visibleClip, const QRectF& viewRect,
                                          const std::function<quint8(const FileNode*)>& matchLookup,
                                          quint8 selfMatchFlag) const
{
    if (!node) {
        return;
    }

    const QRectF r = viewRect;
    const qreal devicePixelScale = pixelScale();
    const QRectF effectiveClip = r.intersected(visibleClip);
    if (r.width() < m_settings.minPaint || r.height() < m_settings.minPaint || effectiveClip.isEmpty()) {
        return;
    }

    const quint8 matchFlags = matchLookup(node);
    const bool directMatch = (matchFlags & selfMatchFlag) != 0;

    // Draw the border for this node if it's a direct match
    if (directMatch && node != m_current) {
        painter.save();
        const QRectF borderRect = snapRectToPixels(r, devicePixelScale);
        const qreal borderWidth = snapLengthToPixels(
            std::max<qreal>(m_settings.border * 1.5, 2.0 / devicePixelScale),
            devicePixelScale);
        if (borderWidth > 0.0 && !borderRect.isEmpty()) {
            painter.setPen(Qt::NoPen);
            fillInnerBorder(painter, borderRect, m_settings.highlightColor, borderWidth);
        }
        painter.restore();
    }

    // Recurse into children to find and outline nested matches
    if (node->isDirectory && canPaintChildrenForDisplay(node, r, depth)) {
        const QRectF contentArea = childPaintRectForNode(node, r);
        const QRectF contentClip = contentArea.adjusted(-1.0, -1.0, 1.0, 1.0).intersected(effectiveClip);
        std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
        layoutVisibleChildren(node, r, contentArea, contentClip, visibleChildren);
        for (const auto& [child, childRect] : visibleChildren) {
            if (matchLookup(child) != 0
                    && tileRevealOpacityForNode(child, childRect) > 0.0
                    && childRect.intersects(contentClip)) {
                paintMatchBordersNode(painter, child, depth + 1, contentClip, childRect,
                                      matchLookup, selfMatchFlag);
            }
        }
    }
}

void TreemapWidget::paintEvent(QPaintEvent* event)
{
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, false);
    // Cache animation state once per frame — avoids per-node QVariant unboxing
    m_layoutAnimating = (m_layoutAnimation.state() == QAbstractAnimation::Running);
    m_animT = m_layoutAnimating ? m_layoutAnimation.currentValue().toReal() : 0.0;

    if (!m_current) {
        painter.fillRect(viewport()->rect(), palette().color(QPalette::Window));
        painter.setPen(palette().color(QPalette::WindowText));
        const QString message = m_scanInProgress
            ? tr("Scanning %1").arg(QDir::toNativeSeparators(m_scanPath))
            : tr("Open a directory to begin");
        painter.drawText(rect(), Qt::AlignCenter, message);
        return;
    }

    if (m_zoomAnimation.state() == QAbstractAnimation::Running
            && !m_previousFrame.isNull() && !m_nextFrame.isNull()) {
        const qreal progress = m_zoomAnimation.currentValue().toReal();
        if (m_zoomCrossfadeOnly) {
            painter.drawPixmap(0, 0, m_previousFrame);
            painter.setOpacity(progress);
            painter.drawPixmap(0, 0, m_nextFrame);
            painter.setOpacity(1.0);
            return;
        }
        const QRectF fullRect(QPointF(0, 0), QSizeF(viewport()->width(), viewport()->height()));
        const QRectF shrinkingRect(
            fullRect.x() + (m_zoomSourceRect.x() - fullRect.x()) * progress,
            fullRect.y() + (m_zoomSourceRect.y() - fullRect.y()) * progress,
            fullRect.width() + (m_zoomSourceRect.width() - fullRect.width()) * progress,
            fullRect.height() + (m_zoomSourceRect.height() - fullRect.height()) * progress);
        const QRectF expandingRect(
            m_zoomSourceRect.x() + (fullRect.x() - m_zoomSourceRect.x()) * progress,
            m_zoomSourceRect.y() + (fullRect.y() - m_zoomSourceRect.y()) * progress,
            m_zoomSourceRect.width() + (fullRect.width() - m_zoomSourceRect.width()) * progress,
            m_zoomSourceRect.height() + (fullRect.height() - m_zoomSourceRect.height()) * progress);
        const QRectF previousFullSourceRect = logicalRectToPixmapSourceRect(m_previousFrame, fullRect);
        const QRectF previousShrinkingSourceRect = logicalRectToPixmapSourceRect(m_previousFrame, shrinkingRect);
        const QRectF nextFullSourceRect = logicalRectToPixmapSourceRect(m_nextFrame, fullRect);
        const QRectF nextExpandingSourceRect = logicalRectToPixmapSourceRect(m_nextFrame, expandingRect);
        // Bilinear sampling prevents 1-device-pixel jitter as the continuous
        // float source/dest rects shift between frames during the zoom.
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (m_zoomingIn) {
            // Keep the full-viewport source frame opaque so the window background
            // never flashes through while the destination frame expands in.
            painter.setOpacity(1.0);
            painter.drawPixmap(fullRect, m_previousFrame, previousShrinkingSourceRect);

            painter.setOpacity(progress);
            painter.drawPixmap(expandingRect, m_nextFrame, nextFullSourceRect);
        } else {
            // Zoom the parent scene back out from the destination tile so
            // zoom-out reads like the inverse of zoom-in instead of a static
            // background with only the departing folder moving.
            painter.setOpacity(1.0);
            painter.drawPixmap(fullRect, m_nextFrame, nextExpandingSourceRect);

            // Shrink the outgoing scene into the tile while fading it away.
            painter.setOpacity(std::max<qreal>(0.0, 1.0 - (1.1 * progress)));
            painter.drawPixmap(shrinkingRect, m_previousFrame, previousFullSourceRect);
        }
        painter.setOpacity(1.0);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        return;
    }

    if (m_cameraAnimation.state() == QAbstractAnimation::Running
            && m_settings.fastWheelZoom
            && !m_cameraPreviousFrame.isNull() && !m_cameraNextFrame.isNull()) {
        const qreal t = m_cameraAnimation.currentValue().toReal();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const qreal curScale = m_cameraScale;
        const QPointF curOrigin = m_cameraOrigin;
        const QRectF vpRect(0, 0, viewport()->width(), viewport()->height());

        const qreal prevRatio = curScale / m_cameraStartScale;
        const QPointF prevOffset = (m_cameraStartOrigin - curOrigin) * curScale;
        const QRectF prevRect(prevOffset.x(), prevOffset.y(),
                              vpRect.width() * prevRatio, vpRect.height() * prevRatio);

        const qreal nextRatio = curScale / m_cameraTargetScale;
        const QPointF nextOffset = (m_cameraTargetOrigin - curOrigin) * curScale;
        const QRectF nextRect(nextOffset.x(), nextOffset.y(),
                              vpRect.width() * nextRatio, vpRect.height() * nextRatio);

        const bool previousCoversViewport = prevRect.width() >= vpRect.width()
            && prevRect.height() >= vpRect.height();
        if (previousCoversViewport) {
            painter.setOpacity(1.0);
            painter.drawPixmap(prevRect, m_cameraPreviousFrame, QRectF(m_cameraPreviousFrame.rect()));
            painter.setOpacity(t);
            painter.drawPixmap(nextRect, m_cameraNextFrame, QRectF(m_cameraNextFrame.rect()));
        } else {
            painter.setOpacity(1.0);
            painter.drawPixmap(nextRect, m_cameraNextFrame, QRectF(m_cameraNextFrame.rect()));
            painter.setOpacity(1.0 - t);
            painter.drawPixmap(prevRect, m_cameraPreviousFrame, QRectF(m_cameraPreviousFrame.rect()));
        }

        painter.setOpacity(1.0);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        return;
    }

    if (m_layoutAnimating && !m_layoutPreviousFrame.isNull()) {
        if (!m_layoutNextFrame.isNull()) {
            // Depth-reveal crossfade: blend two pre-captured screen-space pixmaps.
            // No live rendering, no source-rect mismatch, no tile grid jump.
            painter.drawPixmap(0, 0, m_layoutPreviousFrame);
            painter.setOpacity(m_animT);
            painter.drawPixmap(0, 0, m_layoutNextFrame);
            painter.setOpacity(1.0);
        } else {
            // Rescan crossfade: blend old and new root-live renders.
            painter.fillRect(viewport()->rect(), palette().color(QPalette::Window));
            painter.setOpacity(1.0 - (0.75 * m_animT));
            painter.drawPixmap(0, 0, m_layoutPreviousFrame);
            painter.setOpacity(0.35 + (0.65 * m_animT));
            drawScene(painter, m_current, event->rect());
            painter.setOpacity(1.0);
        }
        return;
    }

    const QRect dirty = event->rect();
    const qreal dpr = pixelScale();
    const QSize deviceSize(
        std::max(1, static_cast<int>(std::ceil(viewport()->width() * dpr))),
        std::max(1, static_cast<int>(std::ceil(viewport()->height() * dpr))));

    // Pure-pan fast path: when only the camera origin changed (scale, depth, root are
    // the same), all tiles shift by a fixed pixel offset.  Scroll the cached m_liveFrame
    // by that offset and repaint only the newly exposed edge strips instead of doing a
    // full scene redraw every frame.  Only valid when not animating (pixel-snapping must
    // be active so the shift is an exact number of device pixels).
    const bool purePan = !m_liveFrame.isNull()
        && m_liveFrameDeviceSize == deviceSize
        && m_current        == m_lastLiveRoot
        && m_cameraScale    == m_lastLiveScale
        && m_activeSemanticDepth == m_lastLiveDepth
        && m_cameraOrigin   != m_lastLiveOrigin
        && !continuousZoomGeometryActive();

    if (purePan) {
        const QPointF originDelta = m_cameraOrigin - m_lastLiveOrigin;
        // Tiles shift by -originDelta * scale in view (logical) pixels.
        const qreal dx = -originDelta.x() * m_cameraScale;
        const qreal dy = -originDelta.y() * m_cameraScale;
        const qreal vpW = viewport()->width();
        const qreal vpH = viewport()->height();

        if (std::abs(dx) < vpW && std::abs(dy) < vpH) {
            // Prepare scroll buffer (reuse across frames; resize only on viewport change).
            if (m_scrollBuffer.isNull() || m_scrollBuffer.size() != m_liveFrame.size()
                    || m_scrollBuffer.devicePixelRatio() != dpr) {
                m_scrollBuffer = QPixmap(deviceSize);
                m_scrollBuffer.setDevicePixelRatio(dpr);
            }

            QPainter lp(&m_scrollBuffer);
            lp.setRenderHint(QPainter::Antialiasing, false);
            // Background fill covers the edges left bare by the shift.
            lp.fillRect(viewport()->rect(), palette().color(QPalette::Window));
            // Blit the existing frame shifted by the pan delta.
            lp.drawPixmap(QPointF(dx, dy), m_liveFrame);

            // Compute the two newly-exposed axis-aligned strips.
            QRectF xStrip, yStrip;
            if (dx < 0)      xStrip = QRectF(vpW + dx, 0,    -dx, vpH);
            else if (dx > 0) xStrip = QRectF(0,        0,     dx, vpH);
            if (dy < 0)      yStrip = QRectF(0, vpH + dy, vpW, -dy);
            else if (dy > 0) yStrip = QRectF(0,        0, vpW,  dy);

            // Trim the y-strip to avoid double-drawing the corner shared with the x-strip.
            if (!xStrip.isEmpty() && !yStrip.isEmpty()) {
                if (dx < 0) yStrip.setRight(vpW + dx);
                else        yStrip.setLeft(dx);
            }

            lp.setClipping(true);
            const auto drawStrip = [&](const QRectF& strip) {
                if (strip.width() < 0.5 || strip.height() < 0.5) return;
                lp.setClipRect(strip);
                drawScene(lp, m_current, strip);
                drawMatchOverlay(lp, m_current, strip);
            };
            drawStrip(xStrip);
            drawStrip(yStrip);
            lp.end();

            std::swap(m_liveFrame, m_scrollBuffer);

            m_lastLiveRoot   = m_current;
            m_lastLiveOrigin = m_cameraOrigin;
            m_lastLiveScale  = m_cameraScale;
            m_lastLiveDepth  = m_activeSemanticDepth;

            painter.drawPixmap(0, 0, m_liveFrame);
            return;
        }
        // Fall through to full redraw for large pan steps (e.g. scroll-bar jump).
    }

    const bool stateChanged = (m_current != m_lastLiveRoot ||
                               m_cameraOrigin != m_lastLiveOrigin ||
                               m_cameraScale != m_lastLiveScale ||
                               m_activeSemanticDepth != m_lastLiveDepth ||
                               m_liveFrame.isNull() ||
                               m_liveFrameDeviceSize != deviceSize);

    if (stateChanged) {
        m_lastLiveRoot = m_current;
        m_lastLiveOrigin = m_cameraOrigin;
        m_lastLiveScale = m_cameraScale;
        m_lastLiveDepth = m_activeSemanticDepth;
        if (m_liveFrame.isNull() || m_liveFrameDeviceSize != deviceSize) {
            m_liveFrame = QPixmap(deviceSize);
            m_liveFrame.setDevicePixelRatio(dpr);
            m_liveFrameDeviceSize = deviceSize;
        }
        QPainter lp(&m_liveFrame);
        lp.setRenderHint(QPainter::Antialiasing, false);
        drawScene(lp, m_current, QRectF(viewport()->rect()));
        drawMatchOverlay(lp, m_current, QRectF(viewport()->rect()));
    } else if (!dirty.isEmpty()) {
        // Partial update for hover etc
        QPainter lp(&m_liveFrame);
        lp.setRenderHint(QPainter::Antialiasing, false);
        lp.setClipRect(dirty);
        drawScene(lp, m_current, QRectF(dirty));
        drawMatchOverlay(lp, m_current, QRectF(dirty));
    }

    painter.drawPixmap(0, 0, m_liveFrame);

    if (m_continuousZoomSettleFramesRemaining > 0
            && m_cameraAnimation.state() != QAbstractAnimation::Running) {
        --m_continuousZoomSettleFramesRemaining;
        if (m_continuousZoomSettleFramesRemaining > 0) {
            viewport()->update();
        }
    }
}

// ── Hit testing ──────────────────────────────────────────────────────────────

FileNode* TreemapWidget::hitTestChildren(FileNode* node, const QPointF& pos, int depth,
                                         const QRectF& tileViewRect, const QRectF& contentArea,
                                         const QRectF& visibleClip,
                                         QRectF* outRect) const
{
    std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
    layoutVisibleChildren(node, tileViewRect, contentArea, visibleClip, visibleChildren);
    for (const auto& [child, childViewRect] : visibleChildren) {
        if (!childViewRect.contains(pos)) {
            continue;
        }
        FileNode* hit = hitTest(child, pos, depth + 1, childViewRect, outRect);
        if (hit) {
            return hit;
        }
    }
    return nullptr;
}

FileNode* TreemapWidget::hitTest(FileNode* node, const QPointF& pos, int depth,
                                 const QRectF& viewRect, QRectF* outRect) const
{
    if (!node || !viewRect.contains(pos))
        return nullptr;

    if (!canPaintChildrenForDisplay(node, viewRect, depth)) {
        if (outRect) {
            *outRect = viewRect;
        }
        return node;
    }

    const QRectF contentArea = childPaintRectForNode(node, viewRect);
    const QRectF contentClip = contentArea.adjusted(-0.75, -0.75, 0.75, 0.75);
    if (FileNode* hit = hitTestChildren(node, pos, depth, viewRect, contentArea, contentClip, outRect)) {
        return hit;
    }
    if (outRect) {
        *outRect = viewRect;
    }
    return node;
}

// ── Mouse events ─────────────────────────────────────────────────────────────

void TreemapWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_current) return;
    if (m_touchGestureActive) {
        event->ignore();
        return;
    }

    if (m_scanInProgress) {
        clearHoverState();
        return;
    }

    if (m_middlePanning) {
        cancelPressHold();
        const QPointF delta = event->position() - m_middlePanStartPos;
        const QPointF targetOrigin(
            m_middlePanStartOrigin.x() - (delta.x() / m_cameraScale),
            m_middlePanStartOrigin.y() - (delta.y() / m_cameraScale));
        m_cameraOrigin = snapCameraOriginToPixelGrid(
            clampCameraOrigin(targetOrigin, m_cameraScale), m_cameraScale, pixelScale());
        m_cameraStartOrigin = m_cameraOrigin;
        m_cameraTargetOrigin = m_cameraOrigin;
        syncScrollBars();
        viewport()->update();
        return;
    }

    updatePressHold(event->position());
    updateHoverAt(event->position(), event->globalPosition().toPoint());
}

void TreemapWidget::mousePressEvent(QMouseEvent* event)
{
    if (m_touchGestureActive) {
        event->ignore();
        return;
    }

    if (m_scanInProgress) {
        event->ignore();
        return;
    }

    if (event->button() == Qt::BackButton) {
        cancelPressHold();
        emit backRequested();
        return;
    }

    if (event->button() == Qt::MiddleButton && m_current) {
        cancelPressHold();
        stopAnimatedNavigation();
        clearHoverState(false);
        m_middlePanning = true;
        m_middlePanStartPos = event->position();
        m_middlePanStartOrigin = m_cameraOrigin;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton || !m_current) return;
    beginPressHold(interactiveNodeAt(event->position()), event->position(), false);
    event->accept();
}

void TreemapWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_touchGestureActive) {
        event->ignore();
        return;
    }

    if (event->button() == Qt::MiddleButton && m_middlePanning) {
        m_middlePanning = false;
        unsetCursor();
        cancelPressHold();
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton && m_current) {
        const FileNode* pendingNode = m_pressHoldNode;
        const bool pressHoldTriggered = m_pressHoldTriggered;
        const QPointF pressPos = event->position();
        FileNode* hit = interactiveNodeAt(pressPos);
        cancelPressHold();
        if (!pressHoldTriggered && pendingNode && hit == pendingNode) {
            m_lastActivationPos = pressPos;
            emit nodeActivated(hit);
        }
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void TreemapWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (m_touchGestureActive) {
        event->ignore();
        return;
    }

    if (m_scanInProgress) {
        event->ignore();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        event->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}

void TreemapWidget::wheelEvent(QWheelEvent* event)
{
    if (m_nativeGestureActive) {
        event->ignore();
        return;
    }

    if (!m_wheelZoomEnabled || m_scanInProgress || !m_current) {
        event->ignore();
        return;
    }

    const QPoint pixelDelta = event->pixelDelta();
    const QPoint angleDelta = event->angleDelta();

    const QPointF cursorPos = event->position();
    const QPointF anchorScenePos(
        m_cameraOrigin.x() + (cursorPos.x() / m_cameraScale),
        m_cameraOrigin.y() + (cursorPos.y() / m_cameraScale));

#ifdef Q_OS_MACOS
    const bool looksLikeTrackpadScroll = !pixelDelta.isNull() || event->phase() != Qt::NoScrollPhase;
    if (looksLikeTrackpadScroll) {
        QPointF sceneDelta;
        if (!pixelDelta.isNull()) {
            sceneDelta = QPointF(-pixelDelta.x() / m_cameraScale,
                                 -pixelDelta.y() / m_cameraScale);
        } else if (!angleDelta.isNull()) {
            constexpr qreal kAngleStepToPixels = 16.0;
            sceneDelta = QPointF(-(angleDelta.x() / 120.0) * kAngleStepToPixels / m_cameraScale,
                                 -(angleDelta.y() / 120.0) * kAngleStepToPixels / m_cameraScale);
        } else {
            event->ignore();
            return;
        }

        stopAnimatedNavigation();
        clearHoverState();
        panCameraImmediate(sceneDelta);
        event->accept();
        return;
    }
#endif

    const bool hasCtrl = event->modifiers() & Qt::ControlModifier;

    // Scale pan distance by the system scroll-lines preference so the user's desktop
    // speed setting is respected. 11 px/line gives ~33 px/notch at the default of 3 lines.
    const qreal kScrollPanStep = 11.0 * QApplication::wheelScrollLines();

    // Horizontal scroll always pans (works for both trackpad and H-scroll mice).
    if (!hasCtrl && angleDelta.x() != 0 && angleDelta.y() == 0) {
        stopAnimatedNavigation();
        clearHoverState();
        panCameraImmediate(QPointF(-(angleDelta.x() / 120.0) * kScrollPanStep / m_cameraScale, 0.0));
        event->accept();
        return;
    }

    // In trackpad-scroll-pans mode: plain scroll pans, Ctrl+scroll zooms immediately
    // (no coalesce timer — pinch events are live and should track the gesture frame-exact).
    // Otherwise (mouse mode): all scroll goes through the animated coalesce path.
    if (m_settings.trackpadScrollPans) {
        // Let the back-navigation zoom animation play out uninterrupted.
        if (m_zoomAnimation.state() == QAbstractAnimation::Running) {
            event->accept();
            return;
        }
        const bool savedBackReady = m_trackpadBackReady;
        stopAnimatedNavigation();
        m_trackpadBackReady = savedBackReady;
        clearHoverState();
        if (!hasCtrl) {
            panCameraImmediate(QPointF(-(angleDelta.x() / 120.0) * kScrollPanStep / m_cameraScale,
                                       -(angleDelta.y() / 120.0) * kScrollPanStep / m_cameraScale));
        } else {
            const qreal steps = angleDelta.y() != 0 ? angleDelta.y() / 120.0 : pixelDelta.y() / 120.0;
            if (!qFuzzyIsNull(steps)) {
                if (steps < 0 && m_cameraScale <= (kCameraMinScale + 0.0001)
                        && m_current && m_current->parent) {
                    if (m_trackpadBackReady) {
                        // Deliberate new pinch after a pause at minimum — go back.
                        m_trackpadBackReady = false;
                        event->accept();
                        emit backRequested();
                        return;
                    }
                    // Still in the original gesture: keep pushing the ready timer back.
                    m_trackpadBackReadyTimer.start();
                    event->accept();
                    return;
                }
                // Zooming in — cancel any pending ready state.
                m_trackpadBackReadyTimer.stop();
                m_trackpadBackReady = false;
                const qreal zoomStepFactor = 1.0 + (m_settings.wheelZoomStepPercent / 100.0);
                const qreal targetScale = std::clamp(m_cameraScale * std::pow(zoomStepFactor, steps),
                                                     kCameraMinScale, m_settings.cameraMaxScale);
                if (targetScale > kZoomedInThreshold) {
                    m_semanticFocus = semanticFocusCandidateAt(cursorPos, currentRootViewRect());
                    m_semanticLiveRoot = m_current;
                } else {
                    m_semanticFocus = nullptr;
                    m_semanticLiveRoot = nullptr;
                }
                zoomCameraImmediate(targetScale, anchorScenePos, cursorPos);
            }
        }
        event->accept();
        return;
    }

    const qreal wheelSteps = angleDelta.y() != 0
        ? angleDelta.y() / 120.0
        : pixelDelta.y() / 120.0;
    if (qFuzzyIsNull(wheelSteps)) {
        event->ignore();
        return;
    }

    m_pendingWheelSteps += wheelSteps;
    m_pendingWheelAnchorScenePos = anchorScenePos;
    m_pendingWheelCursorPos = cursorPos;
    if (!m_wheelZoomCoalesceTimer.isActive()) {
        m_wheelZoomCoalesceTimer.start();
    }
    event->accept();
}

void TreemapWidget::resizeEvent(QResizeEvent* event)
{
    const bool outerSizeChanged = event->oldSize() != event->size();
    const auto snapOriginForResize = [&](const QPointF& origin, qreal scale) {
        const qreal snapScale = scale * pixelScale();
        if (snapScale <= 0.0) {
            return origin;
        }
        return QPointF(
            std::round(origin.x() * snapScale) / snapScale,
            std::round(origin.y() * snapScale) / snapScale);
    };
    const auto maxOriginForViewport = [](const QSize& viewportSize, qreal scale) {
        const qreal clampedScale = std::max<qreal>(scale, kCameraMinScale);
        const qreal visibleWidth = viewportSize.width() / clampedScale;
        const qreal visibleHeight = viewportSize.height() / clampedScale;
        return QPointF(
            std::max<qreal>(0.0, viewportSize.width() - visibleWidth),
            std::max<qreal>(0.0, viewportSize.height() - visibleHeight));
    };
    const auto remapOriginToViewport = [&](const QPointF& origin, qreal scale) {
        if (!outerSizeChanged) {
            return clampCameraOrigin(origin, scale);
        }

        const QPointF oldMax = maxOriginForViewport(event->oldSize(), scale);
        const QPointF newMax = maxOriginForViewport(event->size(), scale);
        const qreal ratioX = oldMax.x() > 0.0 ? std::clamp(origin.x() / oldMax.x(), 0.0, 1.0) : 0.0;
        const qreal ratioY = oldMax.y() > 0.0 ? std::clamp(origin.y() / oldMax.y(), 0.0, 1.0) : 0.0;
        return QPointF(newMax.x() * ratioX, newMax.y() * ratioY);
    };
    // If a camera animation is in progress, the resize is most likely caused by
    // scrollbars appearing as the scale crosses the threshold. Don't kill
    // in-flight animations in that case — they will land on the correct geometry.
    const bool animationInFlight = m_cameraAnimation.state() == QAbstractAnimation::Running
                                || m_zoomAnimation.state() == QAbstractAnimation::Running;
    if (outerSizeChanged && !animationInFlight) {
        m_zoomAnimation.stop();
        m_cameraAnimation.stop();
        m_previousFrame = QPixmap();
        m_nextFrame = QPixmap();
        m_zoomSourceRect = QRectF();
    }
    m_cameraOrigin = snapOriginForResize(
        remapOriginToViewport(m_cameraOrigin, m_cameraScale), m_cameraScale);
    if (outerSizeChanged && !animationInFlight) {
        m_cameraStartOrigin = m_cameraOrigin;
        m_cameraTargetOrigin = m_cameraOrigin;
    } else if (m_cameraAnimation.state() == QAbstractAnimation::Running) {
        m_cameraTargetOrigin = snapOriginForResize(
            remapOriginToViewport(m_cameraTargetOrigin, m_cameraTargetScale),
            m_cameraTargetScale);
    } else {
        m_cameraStartOrigin = m_cameraOrigin;
        m_cameraTargetOrigin = m_cameraOrigin;
    }
    if (outerSizeChanged) {
        m_currentRootLayoutAspectRatio = computeCurrentRootLayoutAspectRatio();
    }
    m_liveSplitCache.clear();
    syncScrollBars();
    m_resizeTimer.start();
}

void TreemapWidget::leaveEvent(QEvent*)
{
    if (m_contextMenuActive) {
        return;
    }

    if (m_middlePanning) {
        return;
    }

    if (m_touchGestureActive) {
        return;
    }

    if (m_hovered) {
        clearHoverState(true);
    }
}

void TreemapWidget::contextMenuEvent(QContextMenuEvent* event)
{
    cancelPressHold();
    if (m_scanInProgress || !m_current) {
        event->ignore();
        return;
    }

    const QPointF pos = event->pos();
    FileNode* hit = hitTest(m_current, pos, -1,
                            currentRootViewRect());
    if (hit && hit != m_current && !hit->isVirtual) {
        hideOwnedTooltip();
        m_contextMenuActive = true;
        emit nodeContextMenuRequested(hit, event->globalPos());
        m_contextMenuActive = false;
        clearHoverState(false);
        // Reset any gesture state that may have been active when the menu opened
        // (e.g. native gesture or touch sequence on macOS), otherwise the next
        // pan/click will be swallowed by stuck m_nativeGestureActive / m_touchGestureActive.
        m_nativeGestureActive = false;
        m_nativeGesturePinching = false;
        m_nativeGesturePendingBackOnEnd = false;
        m_touchGestureActive = false;
        m_touchPanning = false;
        m_touchPinching = false;
        // If the mouse left while the menu was open, clear hover now
        if (!underMouse() && m_hovered) {
            leaveEvent(nullptr);
        }
        event->accept();
        return;
    }

    QWidget::contextMenuEvent(event);
}

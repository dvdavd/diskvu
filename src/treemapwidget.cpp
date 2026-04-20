// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "treemapwidget.h"
#include "treemap_drawing.h"
#include "colorutils.h"
#include "svgutils.h"
#include "thumbnailprovider.h"
#include "filesystemutils.h"
#include "mainwindow_utils.h"
#include <QtConcurrent/QtConcurrent>
#include <QApplication>
#include <QThreadPool>
#include <QCryptographicHash>
#include <QDateTime>
#include <QImageReader>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QTimer>
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
#include <unordered_set>

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

        const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
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

class ThumbnailTask : public QRunnable {
public:
    ThumbnailTask(const QString& path, TreemapWidget* widget,
                  int resolution, int maxFileSizeMB, bool skipNetworkPaths, bool isVideo)
        : m_path(path), m_widget(widget),
          m_resolution(resolution),
          m_maxFileSizeMB(maxFileSizeMB),
          m_skipNetworkPaths(skipNetworkPaths),
          m_isVideo(isVideo)
    {
        setAutoDelete(true);
    }

    void run() override {
        // Network path check
        if (m_skipNetworkPaths) {
            if (!isLocalFilesystem(QStorageInfo(m_path))) {
                QMetaObject::invokeMethod(m_widget, [widget = m_widget, path = m_path]() {
                    widget->m_pendingThumbnails.remove(path);
                }, Qt::QueuedConnection);
                return;
            }
        }

        // File size check
        if (m_maxFileSizeMB > 0) {
            const qint64 limit = (qint64)m_maxFileSizeMB * 1024 * 1024;
            if (QFileInfo(m_path).size() > limit) {
                QMetaObject::invokeMethod(m_widget, [widget = m_widget, path = m_path]() {
                    widget->m_pendingThumbnails.remove(path);
                }, Qt::QueuedConnection);
                return;
            }
        }

        QImage image = readThumbnailCache(m_path, m_resolution);
        bool fromCache = !image.isNull();

        if (image.isNull()) {
#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
            // On Windows and macOS, the system provider is excellent for both images and videos.
            image = getSystemThumbnail(m_path, m_resolution);
#else
            // On Linux, use system provider for videos only.
            if (m_isVideo) {
                image = getSystemThumbnail(m_path, m_resolution);
            }
#endif
        }

        if (image.isNull() && !m_isVideo) {
            // Fallback to QImageReader for images if system provider failed or is unavailable.
            QImageReader reader(m_path);
            if (reader.canRead()) {
                const QSize size = reader.size();
                if (size.isValid()) {
                    const QSize targetSize(m_resolution, m_resolution);
                    if (size.width() > targetSize.width() || size.height() > targetSize.height()) {
                        reader.setScaledSize(size.scaled(targetSize, Qt::KeepAspectRatio));
                    }
                }
                image = reader.read();
            }
        }

        if (!image.isNull()) {
            if (!fromCache) {
                writeThumbnailCache(image, m_path, m_resolution);
            }

            QMetaObject::invokeMethod(m_widget, [widget = m_widget, path = m_path, img = std::move(image)]() mutable {
                QPixmap p = QPixmap::fromImage(img);
                const qsizetype bytes = (qsizetype)p.width() * p.height() * p.depth() / 8;
                widget->m_thumbnailStore.insert(path, p);
                widget->m_thumbnailBytes.insert(path, bytes);
                widget->m_thumbnailTotalBytes += bytes;
                widget->m_thumbnailLastAccess.insert(path, ++widget->m_thumbnailAccessSeq);
                widget->m_pendingThumbnails.remove(path);
                widget->m_thumbnailReadyTimes.insert(path, QDateTime::currentMSecsSinceEpoch());
                widget->viewport()->update();
            }, Qt::QueuedConnection);
        } else {
            QMetaObject::invokeMethod(m_widget, [widget = m_widget, path = m_path]() {
                widget->m_pendingThumbnails.remove(path);
                widget->m_thumbnailFailedPaths.insert(path);
            }, Qt::QueuedConnection);
        }
    }

private:
    QString m_path;
    TreemapWidget* m_widget;
    int m_resolution;
    int m_maxFileSizeMB;
    bool m_skipNetworkPaths;
    bool m_isVideo;
};

QSet<uint64_t> supportedImageExtKeys()
{
    static QSet<uint64_t> imageExtKeys;
    static bool initialized = false;
    if (!initialized) {
        for (const QByteArray& fmt : QImageReader::supportedImageFormats()) {
            imageExtKeys.insert(ColorUtils::packFileExt(QLatin1String("f.") + QString::fromLatin1(fmt).toLower()));
        }
        static const char* common[] = {"jpg", "jpeg", "png", "gif", "webp", "tiff", "bmp", "ico", "svg"};
        for (const char* ext : common) {
            imageExtKeys.insert(ColorUtils::packFileExt(QLatin1String("f.") + QLatin1String(ext)));
        }
        initialized = true;
    }
    return imageExtKeys;
}

QSet<uint64_t> supportedVideoExtKeys()
{
    static QSet<uint64_t> keys;
    static bool initialized = false;
    if (!initialized) {
        static const char* exts[] = {
            "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v",
            "mpg", "mpeg", "m2ts", "3gp", "ogv", "vob", nullptr
        };
        for (int i = 0; exts[i]; ++i)
            keys.insert(ColorUtils::packFileExt(QLatin1String("f.") + QLatin1String(exts[i])));
        initialized = true;
    }
    return keys;
}

class FullImageTask : public QRunnable {
public:
    FullImageTask(const QString& path, TreemapWidget* widget)
        : m_path(path), m_widget(widget)
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QImageReader reader(m_path);
        reader.setAutoTransform(true);
        const QImage image = reader.read();
        QMetaObject::invokeMethod(m_widget, [widget = m_widget, path = m_path, image]() {
            widget->applyLoadedImagePreview(path, image);
        }, Qt::QueuedConnection);
    }

private:
    QString m_path;
    TreemapWidget* m_widget;
};

constexpr quint8 kSearchSelfMatch = 0x1;
constexpr quint8 kSearchSubtreeMatch = 0x2;
constexpr quint8 kSearchMarkMatch = 0x4;
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

    m_imagePreviewAnimation.setDuration(220);
    m_imagePreviewAnimation.setStartValue(0.0);
    m_imagePreviewAnimation.setEndValue(1.0);
    m_imagePreviewAnimation.setEasingCurve(QEasingCurve::InOutCubic);
    connect(&m_imagePreviewAnimation, &QVariantAnimation::valueChanged, this, [this]() {
        m_imagePreviewProgress = std::clamp(m_imagePreviewAnimation.currentValue().toReal(), 0.0, 1.0);
        viewport()->update();
    });
    connect(&m_imagePreviewAnimation, &QVariantAnimation::finished, this, [this]() {
        if (!m_imagePreviewOpening && m_imagePreviewProgress <= 0.001) {
            clearImagePreview();
        }
        viewport()->update();
    });

    m_searchCancelToken = std::make_shared<std::atomic<bool>>(false);
    m_searchWatcher = new QFutureWatcher<SearchMatchResult>(this);
    connect(m_searchWatcher, &QFutureWatcher<SearchMatchResult>::finished,
            this, &TreemapWidget::onSearchTaskFinished);
    m_fileTypeCancelToken = std::make_shared<std::atomic<bool>>(false);
    m_fileTypeWatcher = new QFutureWatcher<FileTypeMatchResult>(this);
    connect(m_fileTypeWatcher, &QFutureWatcher<FileTypeMatchResult>::finished,
            this, &TreemapWidget::onFileTypeMatchTaskFinished);

    m_launchProgressAnimation.setDuration(350);
    m_launchProgressAnimation.setStartValue(0.0);
    m_launchProgressAnimation.setEndValue(1.0);
    m_launchProgressAnimation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_launchProgressAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        qreal progress = value.toReal();
        for (auto& anim : m_launchAnimations) {
            anim.progress = progress;
        }
        viewport()->update();
    });
    connect(&m_launchProgressAnimation, &QVariantAnimation::finished, this, [this]() {
        m_launchAnimations.clear();
        viewport()->update();
    });
}

TreemapWidget::~TreemapWidget()
{
    shutdownAsyncWorkers(true);
}

bool TreemapWidget::viewportEvent(QEvent* event)
{
    if (hasOpenImagePreview()) {
        switch (event->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
        case QEvent::NativeGesture:
            event->accept();
            return true;
        default:
            break;
        }
    }

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
    m_thumbnailStableSizeCache.clear();
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

    // Invalidate pixmap caches to ensure visual changes (marks, colors, fonts) are redrawn.
    m_liveStaticFrame = {};
    m_liveDynamicFrame = {};

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

FileNodeStats TreemapWidget::filteredStats(const FileNode* node) const
{
    if (!node) {
        return {};
    }

    if (!m_searchActive && m_highlightedFileType.isEmpty()) {
        FileNodeStats stats;
        stats.fileCount = node->subtreeFileCount;
        stats.totalSize = node->isVirtual() ? fileNodeStats(node).totalSize : node->displaySize;
        return stats;
    }

    FileNodeStats stats;
    std::function<void(const FileNode*)> recurse = [&](const FileNode* n) {
        if (!n) return;

        const uint8_t flags = effectiveMatchFlags(n);
        // kSearchSelfMatch=0x1, kTypeSelfMatch=0x1, kSearchMarkMatch=0x4
        if (flags & 0x01 || flags & 0x04) {
            const FileNodeStats s = fileNodeStats(n);
            stats.fileCount += s.fileCount;
            stats.totalSize += !n->isVirtual() ? n->displaySize : s.totalSize;
            return;
        }

        if (flags & 0x02) { // Subtree match (kSearchSubtreeMatch=0x2, kTypeSubtreeMatch=0x2)
            for (const FileNode* child = n->firstChild; child; child = child->nextSibling) {
                recurse(child);
            }
        }
    };

    recurse(node);
    return stats;
}

qint64 TreemapWidget::effectiveNodeSize(const FileNode* node) const
{
    if (!node) return 0;
    qint64 sum = 0;
    for (const FileNode* c = node->firstChild; c; c = c->nextSibling) {
        const qint64 childSize = nodeLayoutSize(c);
        if (childSize > 0) sum += childSize;
    }
    return std::max(nodeLayoutSize(node), sum);
}

qint64 TreemapWidget::nodeLayoutSize(const FileNode* node) const
{
    if (!node) {
        return 0;
    }
    // Always lay out using apparent (displaySize) so no tile is ever invisible.
    // Virtual nodes (free space) always have size == displaySize, so this is safe.
    return node->displaySize;
}

qint64 TreemapWidget::nodeDisplaySize(const FileNode* node) const
{
    if (!node) {
        return 0;
    }
    return node->displaySize;
}

void TreemapWidget::setFilterParams(const FilterParams& params)
{
    if (params == m_filterParams) return;

    // During live tree swaps we immediately recompute hover under the cursor.
    // Keep the tooltip visible in the interim to avoid flicker/reset each snapshot.
    clearHoverState(false, false);
    const bool hideChanged = params.hideNonMatching != m_filterParams.hideNonMatching;
    m_filterParams = params;

    // Sync derived members used by incremental search logic
    const QString normalized = params.namePattern.trimmed();
    m_searchPattern = normalized;
    m_searchCaseFoldedPattern = m_searchPattern.toCaseFolded();
    m_searchUsesWildcards = !m_searchPattern.isEmpty()
        && (m_searchPattern.contains(QLatin1Char('*')) || m_searchPattern.contains(QLatin1Char('?')));
    m_minSizeFilter = params.sizeMin;
    m_maxSizeFilter = params.sizeMax;
    m_sizeFilterActive = (params.sizeMin > 0 || params.sizeMax > 0);
    m_searchActive = params.isActive();

    if (hideChanged || m_filterParams.hideNonMatching) {
        m_liveSplitCache.clear();
    }
    rebuildSearchMatches();
    viewport()->update();
}

void TreemapWidget::setSearchPattern(const QString& pattern)
{
    FilterParams p = m_filterParams;
    p.namePattern = pattern.trimmed();
    setFilterParams(p);
}

void TreemapWidget::refreshSearchIndex()
{
    rebuildSearchMetadataAsync();
}

void TreemapWidget::clearAllNodeMarks()
{
    if (!m_root) return;

    if (m_searchIndex) {
        for (FileNode* node : m_searchIndex->nodes) {
            node->setColorMark(0);
            node->setIconMark(0);
        }
        std::fill(m_searchIndex->markCache.begin(), m_searchIndex->markCache.end(), 0u);
        std::fill(m_searchIndex->directMarkCache.begin(), m_searchIndex->directMarkCache.end(), 0u);
    } else {
        // Fallback: recursive clear if index is not yet built.
        auto recurse = [](auto& self, FileNode* node) -> void {
            if (!node) return;
            node->setColorMark(0);
            node->setIconMark(0);
            for (FileNode* child = node->firstChild; child; child = child->nextSibling) {
                self(self, child);
            }
        };
        recurse(recurse, m_root);
    }
}

QString TreemapWidget::nodePath(const FileNode* node) const
{
    if (!node) return {};
    return node->computePath();
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

void TreemapWidget::syncSnapshotCache()
{
    m_root = m_snapshot ? m_snapshot->root : nullptr;
}

FileNode* TreemapWidget::currentImagePreviewNode() const
{
    return (m_snapshot && !m_imagePreviewPath.isEmpty())
        ? m_snapshot->findNode(m_imagePreviewPath)
        : nullptr;
}

void TreemapWidget::resetLiveRenderCache()
{
    m_liveFrame = QPixmap();
    m_liveStaticFrame = QPixmap();
    m_liveDynamicFrame = QPixmap();
    m_scrollBuffer = QPixmap();
    m_liveFrameDeviceSize = QSize();
    m_lastLiveRoot = nullptr;
    m_lastLiveOrigin = QPointF();
    m_lastLiveScale = 0.0;
    m_lastLiveDepth = -1;
}

void TreemapWidget::updateSnapshot(std::shared_ptr<TreemapSnapshot> snapshot)
{
    auto oldSnapshot = std::move(m_snapshot);
    m_snapshot = std::move(snapshot);
    syncSnapshotCache();
    resetLiveRenderCache();
    // Old snapshots may still own a large arena and other per-tree data, so let
    // destruction happen off the UI thread to avoid a visible hitch.
    if (oldSnapshot) {
        std::thread([oldSnapshot = std::move(oldSnapshot)]() {}).detach();
    }
}

void TreemapWidget::setRoot(std::shared_ptr<TreemapSnapshot> snapshot,
                            bool prepareModel, bool animateLayout)
{
    clearImagePreview();
    cancelPressHold();
    m_zoomAnimation.stop();
    m_layoutAnimation.stop();
    m_cameraAnimation.stop();
    m_previousFrame = QPixmap();
    m_nextFrame = QPixmap();
    m_zoomSourceRect = QRectF();
    m_layoutPreviousFrame = QPixmap();
    m_layoutNextFrame = QPixmap();
    cancelPendingSearch();
    cancelPendingMetadata();
    cancelPendingFileTypeHighlight();
    m_metadataRestartPending = false;
    m_pendingMetadataRoot = nullptr;
    m_pendingMetadataArena.reset();
    m_pendingFileTypeIndex.reset();

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
    m_thumbnailStableSizeCache.clear();
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
    resetLiveRenderCache();
    // Capture the layout animation snapshot while the current snapshot (and
    // therefore m_current) is still alive. The old snapshot is moved into the
    // background lambda below; any access to m_current after that point would
    // be a use-after-free if the background thread frees the arena first.
    const QString currentRootPath = (m_snapshot && m_current)
        ? m_snapshot->keyFor(m_current).normalizedPath
        : QString();
    const QString nextRootPath = (snapshot && snapshot->root)
        ? snapshot->keyFor(snapshot->root).normalizedPath
        : QString();
    const bool shouldAnimateLayout = animateLayout
        && !currentRootPath.isEmpty()
        && currentRootPath == nextRootPath
        && !viewport()->size().isEmpty();
    if (shouldAnimateLayout) {
        m_layoutPreviousFrame = renderSceneToPixmap(m_current);
    }

    auto oldSnapshot = std::move(m_snapshot);
    std::thread(
        [oldLiveSplitCache = std::move(oldLiveSplitCache),
         oldSizeLabelCache = std::move(oldSizeLabelCache),
         oldElidedTextCache = std::move(oldElidedTextCache),
         oldElidedDisplayWidthCache = std::move(oldElidedDisplayWidthCache),
         oldSearchMatchCache = std::move(oldSearchMatchCache),
         oldSearchIndex = std::move(oldSearchIndex),
         oldPendingSearchIndex = std::move(oldPendingSearchIndex),
         oldFileTypeMatchCache = std::move(oldFileTypeMatchCache),
         oldPreviousDirectSearchMatches = std::move(oldPreviousDirectSearchMatches),
         oldSnapshot = std::move(oldSnapshot)]() mutable {
        })
        .detach();

    m_nodeCount = 0;
    m_previousSearchCaseFoldedPattern.clear();
    m_previousSearchUsesWildcards = false;
    m_previousMinSizeFilter = 0;
    m_previousMaxSizeFilter = 0;
    m_previousFilterParams = {};
    resetCamera();
    m_activeSemanticDepth = m_settings.baseVisibleDepth;
    m_semanticFocus = nullptr;
    m_semanticLiveRoot = nullptr;

    m_snapshot = std::move(snapshot);
    syncSnapshotCache();
    m_current = m_root;
    m_currentRootLayoutAspectRatio = computeCurrentRootLayoutAspectRatio();
    clearHoverState(false);
    if (prepareModel && m_root) {
        sortChildrenBySize(m_root);
    }
    relayout();
    refreshHoverUnderPointer();
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
    state.nodeKey = m_snapshot ? m_snapshot->keyFor(m_current) : NodeKey{};
    state.cameraScale = m_cameraScale;
    state.cameraOrigin = m_cameraOrigin;
    state.semanticDepth = m_activeSemanticDepth;
    state.semanticFocusKey = m_snapshot ? m_snapshot->keyFor(m_semanticFocus) : NodeKey{};
    state.semanticLiveRootKey = m_snapshot ? m_snapshot->keyFor(m_semanticLiveRoot) : NodeKey{};
    state.currentRootLayoutAspectRatio = m_currentRootLayoutAspectRatio;
    return state;
}

TreemapWidget::ViewState TreemapWidget::overviewViewState(FileNode* node) const
{
    ViewState state;
    FileNode* targetNode = node ? node : m_current;
    state.nodeKey = m_snapshot ? m_snapshot->keyFor(targetNode) : NodeKey{};
    state.cameraScale = 1.0;
    state.cameraOrigin = QPointF();
    state.semanticDepth = m_settings.baseVisibleDepth;
    state.semanticFocusKey = {};
    state.semanticLiveRootKey = {};
    state.currentRootLayoutAspectRatio = computeCurrentRootLayoutAspectRatio();
    return state;
}

void TreemapWidget::restoreViewState(const ViewState& state,
                                     const QPointF& anchorPos, bool useAnchor)
{
    FileNode* targetNode = m_snapshot ? m_snapshot->findNode(state.nodeKey) : nullptr;
    if (!targetNode && m_snapshot) {
        targetNode = m_snapshot->findNode(nearestExistingNodeKey(m_snapshot, state.nodeKey));
    }
    if (!targetNode) {
        targetNode = m_root;
    }
    if (!targetNode) {
        return;
    }

    closeImagePreview(false);

    const qreal targetScale = std::clamp(state.cameraScale,
                                         kCameraMinScale, m_settings.cameraMaxScale);
    const QPointF targetOrigin = clampCameraOrigin(state.cameraOrigin, targetScale);
    const int targetSemanticDepth = std::clamp(state.semanticDepth,
                                               m_settings.baseVisibleDepth,
                                               m_settings.maxSemanticDepth);
    FileNode* targetSemanticFocus = m_snapshot ? m_snapshot->findNode(state.semanticFocusKey) : nullptr;
    FileNode* targetSemanticLiveRoot = m_snapshot ? m_snapshot->findNode(state.semanticLiveRootKey) : nullptr;

    const bool sameNode = (targetNode == m_current);
    const bool sameScale = std::abs(targetScale - m_cameraScale) < 0.0001;
    const bool sameOrigin = QLineF(targetOrigin, m_cameraOrigin).length() < 0.01;
    const bool sameDepth = (targetSemanticDepth == m_activeSemanticDepth);
    const bool sameFocus = (targetSemanticFocus == m_semanticFocus);
    const bool sameLiveRoot = (targetSemanticLiveRoot == m_semanticLiveRoot);
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
        const bool initialMatches = isDescendantOfDirectMatch(m_current);
        if (findVisibleViewRect(m_current, previousViewRect, targetNode, &sourceRect, 0, initialMatches)) {
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
        m_semanticFocus = targetScale > kZoomedInThreshold ? targetSemanticFocus : nullptr;
        m_semanticLiveRoot = targetScale > kZoomedInThreshold ? targetSemanticLiveRoot : nullptr;
    }

    m_current = targetNode;
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
        m_pendingRestoredSemanticFocus = targetSemanticFocus;
        m_pendingRestoredSemanticLiveRoot = targetSemanticLiveRoot;
        animateCameraTo(targetScale, targetOrigin);
        return;
    }

    if (canAnimate && previousNode && !sameNode) {
        const QRectF currentViewRect = currentRootViewRect();
        const bool initialMatches = isDescendantOfDirectMatch(m_current);
        if (findVisibleViewRect(m_current, currentViewRect, previousNode, &sourceRect, 0, initialMatches)) {
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
    FileNode* targetNode = m_snapshot ? m_snapshot->findNode(state.nodeKey) : nullptr;
    if (!targetNode && m_snapshot) {
        targetNode = m_snapshot->findNode(nearestExistingNodeKey(m_snapshot, state.nodeKey));
    }
    if (!targetNode) {
        targetNode = m_root;
    }
    if (!targetNode) {
        return;
    }

    closeImagePreview(false);

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

    m_current = targetNode;
    m_currentRootLayoutAspectRatio = state.currentRootLayoutAspectRatio > 0.0
        ? state.currentRootLayoutAspectRatio
        : computeCurrentRootLayoutAspectRatio();
    // Live scan view remaps call this frequently; avoid forcing tooltip fade-out
    // and immediately re-evaluate hover under the current cursor.
    clearHoverState(false, false);
    setCameraImmediate(targetScale, targetOrigin);
    m_activeSemanticDepth = targetSemanticDepth;
    m_semanticFocus = targetScale > kZoomedInThreshold
        ? (m_snapshot ? m_snapshot->findNode(state.semanticFocusKey) : nullptr)
        : nullptr;
    m_semanticLiveRoot = targetScale > kZoomedInThreshold
        ? (m_snapshot ? m_snapshot->findNode(state.semanticLiveRootKey) : nullptr)
        : nullptr;

    relayout();
    refreshHoverUnderPointer();
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

void TreemapWidget::clearHoverState(bool notify, bool hideTooltip)
{
    if (!m_hovered && !m_previousHovered && m_hoveredRect.isEmpty() && m_previousHoveredRect.isEmpty()) {
        if (hideTooltip) {
            hideOwnedTooltip();
        }
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
    if (hideTooltip) {
        hideOwnedTooltip();
    }
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

void TreemapWidget::pruneThumbnailCache()
{
    const qsizetype kBudget = (qsizetype)m_settings.thumbnailMemoryLimitMB * 1024 * 1024;
    if (m_thumbnailTotalBytes <= kBudget)
        return;

    // Build an eviction list of unpinned entries, oldest access first.
    QList<std::pair<quint64, QString>> candidates;
    candidates.reserve(m_thumbnailStore.size());
    for (auto it = m_thumbnailStore.keyBegin(); it != m_thumbnailStore.keyEnd(); ++it) {
        if (!m_thumbnailFramePinSet.contains(*it))
            candidates.append({m_thumbnailLastAccess.value(*it, 0), *it});
    }
    std::sort(candidates.begin(), candidates.end());

    for (auto& [seq, path] : candidates) {
        if (m_thumbnailTotalBytes <= kBudget)
            break;
        m_thumbnailTotalBytes -= m_thumbnailBytes.value(path, 0);
        m_thumbnailStore.remove(path);
        m_thumbnailBytes.remove(path);
        m_thumbnailLastAccess.remove(path);
        m_thumbnailReadyTimes.remove(path);
    }
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
    const QString nodePath = (!node || node->isVirtual()) ? QString() : node->computePath();
    const bool isDirectory = node && node->isDirectory();
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
    const qint64 shownSize = nodeDisplaySize(node);
    const QString shownSizeText = QLocale::system().formattedDataSize(shownSize);
    m_hoverTooltipSizeLabel->setText(tr("Size: %1").arg(shownSizeText));

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

bool TreemapWidget::nodeSupportsImagePreview(const FileNode* node) const
{
    if (!node || node->isDirectory() || node->isVirtual()) {
        return false;
    }

    if (!m_settings.showThumbnails) {
        return false;
    }

    const uint64_t extKey = ColorUtils::packFileExt(node->name);
    const bool isImage = supportedImageExtKeys().contains(extKey);

    // Don't allow clicking to expand previews of videos as they are often low resolution
    // and not very helpful in the full-screen preview.
    if (!isImage) {
        return false;
    }

    // Don't show previews for images where thumbnails are disabled (e.g. on network locations
    // if skipNetworkPaths is enabled) to avoid long waits or black screens.
    if (m_settings.thumbnailSkipNetworkPaths) {
        const QString path = nodePath(node);
        // If it's already in the cache, we allow it.
        if (m_thumbnailStore.contains(path)) {
            return true;
        }
        if (!isLocalFilesystem(QStorageInfo(path))) {
            return false;
        }
    }

    return true;
}

QRectF TreemapWidget::imagePreviewSourceRectForNode(const FileNode* node) const
{
    if (!node || !m_current) {
        return {};
    }

    QRectF sourceRect;
    const bool initialMatches = isDescendantOfDirectMatch(m_current);
    if (!findVisibleViewRect(m_current, currentRootViewRect(), const_cast<FileNode*>(node), &sourceRect, 0, initialMatches)) {
        return {};
    }
    return sourceRect;
}

QSize TreemapWidget::imagePreviewBaseImageSize() const
{
    if (!m_imagePreviewImage.isNull()) {
        return m_imagePreviewImage.size();
    }
    if (!m_imagePreviewPath.isEmpty()) {
        auto it = m_thumbnailStore.constFind(m_imagePreviewPath);
        if (it != m_thumbnailStore.cend()) {
            return it.value().size();
        }
    }
    return {};
}

qreal TreemapWidget::imagePreviewBaseOpacity() const
{
    FileNode* const previewNode = currentImagePreviewNode();
    if (!m_settings.showThumbnails || !previewNode || m_imagePreviewPath.isEmpty() || !nodeSupportsImagePreview(previewNode)) {
        return 0.0;
    }

    const qreal tileRevealOpacity = tileRevealOpacityForNode(
        previewNode,
        m_imagePreviewSourceRect.isValid() ? m_imagePreviewSourceRect : imagePreviewTargetRect());
    if (tileRevealOpacity <= 0.0) {
        return 0.0;
    }

    const qreal thumbFullSize = std::max<qreal>(1.0, m_settings.thumbnailMinTileSize);
    const qreal thumbRevealDistance = std::clamp(thumbFullSize * 0.3, 16.0, 32.0);
    const qreal thumbWidthHysteresis = 8.0;
    const qreal thumbHeightHysteresis = 8.0;
    const qreal thumbStartSize = std::max<qreal>(1.0, thumbFullSize - thumbRevealDistance);
    const QSizeF stableThumbnailSize = stabilizedNodeSize(
        previewNode, StableMetricChannel::ThumbnailReveal,
        m_imagePreviewSourceRect.isValid() ? m_imagePreviewSourceRect.size() : imagePreviewTargetRect().size(),
        kRevealWidthBucketPx, kRevealHeightBucketPx,
        thumbWidthHysteresis, thumbHeightHysteresis);
    const qreal thumbnailRevealOpacity = revealOpacityForSize(
        stableThumbnailSize,
        thumbStartSize,
        thumbStartSize,
        thumbFullSize,
        thumbFullSize);

    const qint64 readyTime = m_thumbnailReadyTimes.value(m_imagePreviewPath, 0);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qreal loadAlpha = std::clamp((now - readyTime) / 300.0, 0.0, 1.0);
    return tileRevealOpacity * thumbnailRevealOpacity * loadAlpha;
}

QRectF TreemapWidget::imagePreviewTargetRect() const
{
    const QSize imageSize = imagePreviewBaseImageSize();
    const QRectF fullRect(QPointF(0, 0), QSizeF(viewport()->size()));
    if (!imageSize.isValid() || fullRect.isEmpty()) {
        return fullRect;
    }

    QSizeF scaledSize = QSizeF(imageSize).scaled(fullRect.size(), Qt::KeepAspectRatio);
    QRectF targetRect(QPointF(0, 0), scaledSize);
    targetRect.moveCenter(fullRect.center());
    return targetRect;
}

void TreemapWidget::requestImagePreview(FileNode* node, const QRectF& sourceRect)
{
    if (!nodeSupportsImagePreview(node)) {
        return;
    }

    stopAnimatedNavigation();
    cancelPressHold();
    clearHoverState(false);
    hideOwnedTooltip();

    const QString path = nodePath(node);
    const bool samePath = (path == m_imagePreviewPath);
    m_imagePreviewPath = path;
    m_imagePreviewSourceRect = sourceRect.isValid() ? sourceRect : imagePreviewSourceRectForNode(node);
    if (!m_imagePreviewSourceRect.isValid()) {
        m_imagePreviewSourceRect = QRectF(QPointF(0, 0), QSizeF(viewport()->size()));
    }

    m_imagePreviewOpening = true;
    if (!samePath) {
        m_imagePreviewImage = QImage();
    }
    if (!m_imagePreviewPath.isEmpty() && (!samePath || m_imagePreviewImage.isNull())) {
        m_imagePreviewLoading = true;
        QThreadPool::globalInstance()->start(new FullImageTask(m_imagePreviewPath, this));
    } else {
        m_imagePreviewLoading = false;
    }

    m_imagePreviewAnimation.stop();
    m_imagePreviewAnimation.setStartValue(m_imagePreviewProgress);
    m_imagePreviewAnimation.setEndValue(1.0);
    m_imagePreviewAnimation.start();
    syncScrollBars();
}

void TreemapWidget::closeImagePreview(bool animated)
{
    if (m_imagePreviewPath.isEmpty() && m_imagePreviewProgress <= 0.0) {
        return;
    }

    m_imagePreviewOpening = false;
    if (!animated) {
        m_imagePreviewAnimation.stop();
        m_imagePreviewProgress = 0.0;
        clearImagePreview();
        syncScrollBars();
        viewport()->update();
        return;
    }

    m_imagePreviewAnimation.stop();
    m_imagePreviewAnimation.setStartValue(m_imagePreviewProgress);
    m_imagePreviewAnimation.setEndValue(0.0);
    m_imagePreviewAnimation.start();
}

void TreemapWidget::clearImagePreview()
{
    m_imagePreviewAnimation.stop();
    m_imagePreviewPath.clear();
    m_imagePreviewSourceRect = QRectF();
    m_imagePreviewImage = QImage();
    m_imagePreviewLoading = false;
    m_imagePreviewOpening = false;
    m_imagePreviewProgress = 0.0;
    syncScrollBars();
}

void TreemapWidget::closeImagePreviewFromNavigation()
{
    closeImagePreview(true);
}

void TreemapWidget::applyLoadedImagePreview(const QString& path, const QImage& image)
{
    if (m_imagePreviewPath != path) {
        return;
    }
    m_imagePreviewLoading = false;
    m_imagePreviewImage = image;
    viewport()->update();
}

void TreemapWidget::paintImagePreviewOverlay(QPainter& painter)
{
    if (m_imagePreviewPath.isEmpty() || m_imagePreviewProgress <= 0.0) {
        return;
    }

    const QRectF fullRect(QPointF(0, 0), QSizeF(viewport()->size()));
    const QRectF sourceRect = m_imagePreviewSourceRect.isValid() ? m_imagePreviewSourceRect : fullRect;
    const QRectF targetRect = imagePreviewTargetRect();
    const qreal t = smoothstep(std::clamp(m_imagePreviewProgress, 0.0, 1.0));
    const qreal imageBaseOpacity = imagePreviewBaseOpacity();
    const qreal edgeOpacityBlend = smoothstep(std::clamp(t / 0.18, 0.0, 1.0));
    const qreal imageOpacityScale = imageBaseOpacity + ((1.0 - imageBaseOpacity) * edgeOpacityBlend);
    const QRectF animRect(
        sourceRect.x() + ((targetRect.x() - sourceRect.x()) * t),
        sourceRect.y() + ((targetRect.y() - sourceRect.y()) * t),
        sourceRect.width() + ((targetRect.width() - sourceRect.width()) * t),
        sourceRect.height() + ((targetRect.height() - sourceRect.height()) * t));
    const qreal sourceAlpha = std::clamp(1.0 - (t * 1.15), 0.0, 1.0);
    const qreal targetAlpha = std::clamp((t - 0.08) / 0.92, 0.0, 1.0);

    painter.save();
    QColor backdrop = Qt::black;
    backdrop.setAlphaF(std::clamp(0.95 * t, 0.0, 0.95));
    painter.fillRect(fullRect, backdrop);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const auto drawTileImage = [&](const QRectF& rect, qreal opacity) {
        if (opacity <= 0.0 || rect.isEmpty()) {
            return;
        }

        if (!m_imagePreviewPath.isEmpty()) {
            auto it = m_thumbnailStore.constFind(m_imagePreviewPath);
            if (it != m_thumbnailStore.cend()) {
                const QPixmap& pixmap = it.value();
                const QSize imgSize = pixmap.size();
                painter.save();
                painter.setOpacity(opacity * imageOpacityScale);
                const int fitMode = m_settings.thumbnailFitMode;
                if (fitMode == TreemapSettings::ThumbnailFill) {
                    QSizeF scaledSize = QSizeF(imgSize).scaled(rect.size(), Qt::KeepAspectRatioByExpanding);
                    QRectF imgRect(QPointF(0, 0), scaledSize);
                    imgRect.moveCenter(rect.center());
                    painter.setClipRect(rect);
                    painter.drawPixmap(imgRect, pixmap, pixmap.rect());
                } else if (fitMode == TreemapSettings::ThumbnailFit) {
                    QSizeF scaledSize = QSizeF(imgSize).scaled(rect.size(), Qt::KeepAspectRatio);
                    QRectF imgRect(QPointF(0, 0), scaledSize);
                    imgRect.moveCenter(rect.center());
                    painter.drawPixmap(imgRect, pixmap, pixmap.rect());
                } else {
                    painter.drawPixmap(rect, pixmap, pixmap.rect());
                }
                painter.restore();
                return;
            }
        }

        if (!m_imagePreviewImage.isNull()) {
            painter.save();
            painter.setOpacity(opacity * imageOpacityScale);
            painter.drawImage(rect, m_imagePreviewImage);
            painter.restore();
        }
    };

    const auto drawFullscreenImage = [&](const QRectF& rect, qreal opacity) {
        if (opacity <= 0.0 || rect.isEmpty()) {
            return;
        }

        if (!m_imagePreviewImage.isNull()) {
            const QSizeF scaledSize = QSizeF(m_imagePreviewImage.size()).scaled(rect.size(), Qt::KeepAspectRatio);
            QRectF imgRect(QPointF(0, 0), scaledSize);
            imgRect.moveCenter(rect.center());
            painter.save();
            painter.setOpacity(opacity * imageOpacityScale);
            painter.drawImage(imgRect, m_imagePreviewImage);
            painter.restore();
            return;
        }

        if (!m_imagePreviewPath.isEmpty()) {
            auto it = m_thumbnailStore.constFind(m_imagePreviewPath);
            if (it != m_thumbnailStore.cend()) {
                const QPixmap& pixmap = it.value();
                const QSizeF scaledSize = QSizeF(pixmap.size()).scaled(rect.size(), Qt::KeepAspectRatio);
                QRectF imgRect(QPointF(0, 0), scaledSize);
                imgRect.moveCenter(rect.center());
                painter.save();
                painter.setOpacity(opacity * imageOpacityScale);
                painter.drawPixmap(imgRect, pixmap, pixmap.rect());
                painter.restore();
            }
        }
    };

    drawTileImage(animRect, sourceAlpha);
    drawFullscreenImage(animRect, targetAlpha);
    painter.restore();
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

    const bool initialMatches = isDescendantOfDirectMatch(m_current);
    FileNode* hit = hitTest(m_current, pos, -1, currentRootViewRect(), initialMatches);
    if (!hit || hit == m_current || hit->isVirtual()) {
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
    const bool initialMatches = isDescendantOfDirectMatch(m_current);
    FileNode* hit = hitTest(m_current, pos, -1, currentRootViewRect(), initialMatches, &hitRect);
    if (hit == m_current) {
        hit = nullptr;
        hitRect = QRectF();
    }

    const bool hoverChanged = (hit != m_hovered);
    const bool hoverRectChanged = (!hoverChanged && hitRect != m_hoveredRect);
    if (hoverChanged || hoverRectChanged) {
        QRect dirty = expandedDirtyRect(m_hoveredRect.toAlignedRect(), viewport()->rect());
        if (!hitRect.isEmpty()) {
            dirty = dirty.united(expandedDirtyRect(hitRect.toAlignedRect(), viewport()->rect()));
        }
        m_previousHovered = nullptr;
        m_previousHoveredRect = QRectF();
        m_hovered = hit;
        m_hoveredRect = hitRect;
        if (hoverChanged) {
            m_hoveredTooltip.clear();
            m_hoverBlend = hit ? 1.0 : 0.0;
            m_hoverAnimation.stop();
        }
        viewport()->update(dirty);
    }

    const bool tooltipChanged = (hit != m_tooltipTarget);
    if (hit) {
        if (m_hoveredTooltip.isEmpty() || tooltipChanged) {
            const QString nodePath = hit->isVirtual() ? QString() : hit->computePath();
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
            const QString sizeStr = QLocale::system().formattedDataSize(
                hit->isVirtual() ? hit->size : hit->displaySize);
            QString modifiedStr;
            if (hit->mtime > 0) {
                const QDateTime dt = QDateTime::fromSecsSinceEpoch(hit->mtime);
                modifiedStr = tr("Modified: %1").arg(QLocale::system().toString(dt, QLocale::ShortFormat));
            }

            if (m_settings.simpleTooltips) {
                m_hoveredTooltip = QStringLiteral("%1\n%2")
                    .arg(elidedSimplePath, sizeStr);
                if (!modifiedStr.isEmpty()) {
                    m_hoveredTooltip += QStringLiteral("\n%1").arg(modifiedStr);
                }
            } else {
                const bool rtl = layoutDirection() == Qt::RightToLeft;
                const QString dirAttr = rtl ? QStringLiteral("rtl") : QStringLiteral("ltr");
                const QString alignAttr = rtl ? QStringLiteral("right") : QStringLiteral("left");
                const bool isConsolidatedChild = hit->isVirtual() && hit->parent && hit->parent->isVirtual() && hit->parent->isDirectory();
                const QString displayName = hit->isVirtual()
                    ? (isConsolidatedChild ? tr("Free Space") : hit->name)
                    : (!info.fileName().isEmpty() ? info.fileName() : nodePath);
                const QString richPath = hit->isVirtual()
                    ? (isConsolidatedChild ? hit->name : QString())
                    : tooltipDisplayPath(info.absolutePath());
                constexpr int kMaxTooltipPathChars = 64;
                const QString cappedRichPath = middleElideChars(richPath, kMaxTooltipPathChars);
                const QString elidedRichPath = m_hoverTooltipTextLabel->fontMetrics().elidedText(
                    cappedRichPath, Qt::ElideMiddle, richPathWidth);
                const QString kindText = hit->isVirtual()
                    ? QCoreApplication::translate("ColorUtils", "Free Space")
                    : (hit->isDirectory() ? tr("Folder") : tr("File"));
                const QString escapedName = displayName.toHtmlEscaped();
                const QString escapedPath = elidedRichPath.toHtmlEscaped();
                m_hoveredTooltip = QStringLiteral("<div dir=\"%1\"><div align=\"%2\"><b>%3</b></div>")
                    .arg(dirAttr, alignAttr, escapedName);
                if (!escapedPath.isEmpty()) {
                    m_hoveredTooltip += QStringLiteral(
                        "<div dir=\"ltr\" align=\"%1\" style=\"opacity:0.78;\">%2</div>")
                        .arg(alignAttr, escapedPath);
                }
                if (!modifiedStr.isEmpty()) {
                    m_hoveredTooltip += QStringLiteral("<div align=\"%1\" style=\"opacity:0.78;\">%2</div>")
                        .arg(alignAttr, modifiedStr.toHtmlEscaped());
                }
                if (!kindText.isEmpty() && kindText != displayName) {
                    m_hoveredTooltip += QStringLiteral("<div align=\"%1\" style=\"opacity:0.78;\">%2</div>")
                        .arg(alignAttr, kindText.toHtmlEscaped());
                }
                if (!hit->isDirectory() && hit->hasHardLinks()) {
                    const QString hlNote = hit->size == 0
                        ? tr("Hard link - duplicate, counted via another link")
                        : tr("Hard link - counted once on disk");
                    m_hoveredTooltip += QStringLiteral(
                        "<div align=\"%1\" style=\"opacity:0.78;\">%2</div>")
                        .arg(alignAttr, hlNote.toHtmlEscaped());
                }
                m_hoveredTooltip += QStringLiteral("</div>");
            }
        }
        if (hoverChanged || hoverRectChanged || tooltipChanged || !m_ownsTooltip
                || m_hoverTooltipTextLabel->text() != m_hoveredTooltip) {
            const qint64 parentBaseline = hit->parent ? effectiveNodeSize(hit->parent) : hit->displaySize;
            const double parentPercent = (parentBaseline > 0)
                ? (100.0 * static_cast<double>(hit->displaySize) / static_cast<double>(parentBaseline))
                : 0.0;

            const qint64 totalDataSize = m_root ? m_root->displaySize : 0;
            const qint64 totalCapacity = effectiveNodeSize(m_root);
            const qint64 rootBaseline = hit->isVirtual() ? totalCapacity : totalDataSize;

            const double rootPercent = (rootBaseline > 0)
                ? (100.0 * static_cast<double>(hit->displaySize) / static_cast<double>(rootBaseline))
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

    if (hasOpenImagePreview()) {
        clearHoverState();
        return;
    }

    if (m_scanInProgress) {
        clearHoverState();
        return;
    }

    if (m_contextMenuActive || m_middlePanning || m_touchGestureActive || m_nativeGestureActive) {
        return;
    }

    if (QApplication::activeModalWidget()) {
        if (m_hovered || m_previousHovered || !m_hoveredRect.isEmpty() || !m_previousHoveredRect.isEmpty()) {
            clearHoverState();
        }
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
    m_pressHoldPath = (node && !node->isVirtual()) ? nodePath(node) : QString();
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
    m_pressHoldPath.clear();
    m_pressHoldStartPos = QPointF();
    m_pressHoldCurrentPos = QPointF();
}

void TreemapWidget::triggerPressHoldContextMenu()
{
    FileNode* const targetNode = (!m_pressHoldPath.isEmpty() && m_snapshot)
        ? m_snapshot->findNode(m_pressHoldPath)
        : nullptr;
    if (!m_pressHoldActive || !targetNode || m_scanInProgress) {
        cancelPressHold();
        return;
    }

    m_pressHoldTriggered = true;
    hideOwnedTooltip();
    m_contextMenuActive = true;
    emit nodeContextMenuRequested(
        targetNode,
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
    m_cameraUseFocusAnchor = false;
    m_cameraPreviousFrame = QPixmap();
    m_cameraNextFrame = QPixmap();
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

    QRectF hitRect;
    const bool initialMatches = isDescendantOfDirectMatch(m_current);
    FileNode* hit = hitTest(m_current, pos, -1, currentRootViewRect(), initialMatches, &hitRect);
    if (hit && hit != m_current && !hit->isVirtual()) {
        if (nodeSupportsImagePreview(hit)) {
            m_lastActivationPos = pos;
            requestImagePreview(hit, hitRect);
            return;
        }
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

    const bool initialMatches = isDescendantOfDirectMatch(m_current);

    std::function<FileNode*(FileNode*, const QRectF&, int, bool)> recurse =
        [&](FileNode* node, const QRectF& nodeViewRect, int nodeDepth, bool parentMatches) -> FileNode* {
            if (!node || !node->isDirectory() || !nodeViewRect.contains(pos)) {
                return nullptr;
            }

            FileNode* best = node;
            if (!canPaintChildrenForDisplay(node, nodeViewRect, nodeDepth)) {
                return best;
            }

            const bool currentMatches = parentMatches || (effectiveMatchFlags(node) & 0x01);

            const QRectF contentArea = childPaintRectForNode(node, nodeViewRect);
            const QRectF contentClip = contentArea.adjusted(-1.0, -1.0, 1.0, 1.0);
            std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
            layoutVisibleChildren(node, nodeViewRect, contentArea, contentClip, visibleChildren, currentMatches);
            for (const auto& [child, childViewRect] : visibleChildren) {
                if (!child->isDirectory() || !childViewRect.contains(pos)) {
                    continue;
                }
                if (FileNode* deeper = recurse(child, childViewRect, nodeDepth + 1, currentMatches)) {
                    best = deeper;
                }
                break;
            }
            return best;
        };

    FileNode* candidate = recurse(m_current, viewRect, (depth == -1) ? 0 : depth, initialMatches);
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
    closeImagePreview(false);
    m_liveSplitCache.clear();
    m_sizeLabelCache.clear();
    m_elidedTextCache.clear();
    m_elidedDisplayWidthCache.clear();
    m_stableMetricCache.clear();
    m_headerLabelVisibleCache.clear();
    m_fileLabelVisibleCache.clear();
    m_thumbnailStableSizeCache.clear();
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
    resetLiveRenderCache();

    clearIncrementalSearchState();
    clearHoverState(false);
    viewport()->update();
    rebuildSearchMetadataAsync();
}


void TreemapWidget::cancelPendingSearch()
{
    if (m_searchCancelToken) {
        m_searchCancelToken->store(true, std::memory_order_relaxed);
    }
    m_searchCancelToken = std::make_shared<std::atomic<bool>>(false);
}

void TreemapWidget::cancelPendingMetadata()
{
    if (m_metadataCancelToken) {
        m_metadataCancelToken->store(true, std::memory_order_relaxed);
    }
    m_metadataCancelToken = std::make_shared<std::atomic<bool>>(false);
}

void TreemapWidget::shutdownAsyncWorkers(bool waitForFinished)
{
    m_asyncShutdown = true;
    m_metadataRestartPending = false;
    m_pendingMetadataRoot = nullptr;
    m_pendingMetadataArena.reset();
    m_pendingFileTypeIndex.reset();
    cancelPendingSearch();
    cancelPendingMetadata();
    cancelPendingFileTypeHighlight();

    if (m_searchWatcher) {
        disconnect(m_searchWatcher, nullptr, this, nullptr);
        if (waitForFinished && m_searchWatcher->isRunning()) {
            m_searchWatcher->waitForFinished();
        }
    }
    if (m_metadataThread.joinable()) {
        if (waitForFinished) {
            m_metadataThread.join();
        } else {
            m_metadataThread.detach();
        }
    }
    if (m_fileTypeWatcher) {
        disconnect(m_fileTypeWatcher, nullptr, this, nullptr);
        if (waitForFinished && m_fileTypeWatcher->isRunning()) {
            m_fileTypeWatcher->waitForFinished();
        }
    }

    m_metadataTaskRunning = false;
    m_activeMetadataTaskId = 0;
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
        m_previousFilterParams = m_filterParams;
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
    // Only use incremental search when none of the new filter dimensions changed.
    const bool newFiltersUnchanged = m_filterParams.dateMin == m_previousFilterParams.dateMin
        && m_filterParams.dateMax == m_previousFilterParams.dateMax
        && m_filterParams.fileTypeGroups == m_previousFilterParams.fileTypeGroups
        && m_filterParams.filesOnly == m_previousFilterParams.filesOnly
        && m_filterParams.foldersOnly == m_previousFilterParams.foldersOnly
        && m_filterParams.markFilter == m_previousFilterParams.markFilter;
    const bool canIncremental = newFiltersUnchanged
        && (sizeFilterUnchanged || sizeFilterNarrowed)
        && !m_searchUsesWildcards
        && !m_previousSearchUsesWildcards
        && !m_previousSearchCaseFoldedPattern.isEmpty()
        && m_searchCaseFoldedPattern.startsWith(m_previousSearchCaseFoldedPattern);

    // Build file type ext key set on main thread (avoids settings access in background task).
    std::unordered_set<uint64_t> fileTypeExtKeys;
    if (!m_filterParams.fileTypeGroups.isEmpty()) {
        for (const FileTypeGroup& group : m_settings.fileTypeGroups) {
            if (m_filterParams.fileTypeGroups.contains(group.name)) {
                for (const QString& ext : group.extensions) {
                    const uint64_t key = ColorUtils::packFileExt(QStringLiteral("x.") + ext);
                    if (key != 0) fileTypeExtKeys.insert(key);
                }
            }
        }
    }

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
    const int64_t dateMin      = m_filterParams.dateMin;
    const int64_t dateMax      = m_filterParams.dateMax;
    const bool filesOnly       = m_filterParams.filesOnly;
    const bool foldersOnly     = m_filterParams.foldersOnly;
    const QSet<int> markFilter = m_filterParams.markFilter;
    std::vector<uint8_t> matchScratch = std::move(m_searchMatchScratch);
    std::vector<bool> reachScratch = std::move(m_searchReachScratch);
    auto cancelToken           = m_searchCancelToken; // shared ownership

    m_searchWatcher->setFuture(QtConcurrent::run([
            index = m_pendingSearchIndex,
            incrementalInput,
            inputNodes = std::move(incrementalNodes),
            pattern, patternUtf8, usesWildcards,
            sizeMin, sizeMax, sizeActive,
            dateMin, dateMax,
            fileTypeExtKeys = std::move(fileTypeExtKeys),
            filesOnly, foldersOnly,
            markFilter,
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
        uint32_t markFilterMask = 0;
        for (int m : markFilter) {
            if (m > 0) markFilterMask |= (1u << m);
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

            // Date match (files with unknown mtime=0 are excluded when date filter is active)
            const bool dateOk = (dateMin == 0 && dateMax == 0)
                || (node->mtime != 0
                    && (dateMin == 0 || node->mtime >= dateMin)
                    && (dateMax == 0 || node->mtime <= dateMax));

            // File type match (applies to files only; dirs always pass)
            const bool typeOk = fileTypeExtKeys.empty()
                || (!node->isDirectory() && fileTypeExtKeys.count(ColorUtils::packFileExt(node->name)) > 0);

            // Files/folders-only mode
            const bool modeOk = (!filesOnly || !node->isDirectory())
                && (!foldersOnly || node->isDirectory());

            // Mark filter: matches if node (or any ancestor) has a mark in the filter set.
            bool markOk = markFilter.isEmpty() || markFilterMask == 0;
            bool markDirect = markOk;
            if (!markOk && node->id < index->markCache.size()) {
                if ((index->markCache[node->id] & markFilterMask) != 0) {
                    markOk = true;
                    // It's a direct mark match if this node itself has a matching mark
                    // (not just inherited from a parent).
                    markDirect = node->id < index->directMarkCache.size() && (index->directMarkCache[node->id] & markFilterMask) != 0;
                }
            }

            if (patternOk && sizeOk && dateOk && typeOk && modeOk && markOk) {
                result.directMatches.push_back(node);
                if (node->id < result.matchCache.size()) {
                    if (markDirect) {
                        result.matchCache[node->id] |= kSearchSelfMatch;
                    } else {
                        result.matchCache[node->id] |= kSearchMarkMatch;
                    }
                }
            }
        }

        // Pass 1 (Reverse): Propagate subtree matches from children to parents.
        // Since index->nodes is DFS pre-order, the reverse is post-order.
        for (auto it = index->nodes.rbegin(); it != index->nodes.rend(); ++it) {
            FileNode* node = *it;
            if (!node->parent || node->parent->id >= result.matchCache.size()) continue;
            
            // If the current node is a match OR its children are matches, the parent is a subtree match.
            if (result.matchCache[node->id] != 0) {
                result.matchCache[node->parent->id] |= kSearchSubtreeMatch;
            }
        }

        // Pass 2 (Forward): Propagate reachability (descendants of direct matches).
        result.searchReachCache = std::move(reachScratch);
        if (result.searchReachCache.size() != index->nodeCount) {
            result.searchReachCache.assign(index->nodeCount, false);
        } else {
            std::fill(result.searchReachCache.begin(), result.searchReachCache.end(), false);
        }

        for (FileNode* node : index->nodes) {
            if (node->id >= result.matchCache.size()) continue;

            // Reach if: this node is a match, or its parent is reachable.
            bool reachable = (result.matchCache[node->id] & (kSearchSelfMatch | kSearchMarkMatch));
            if (!reachable && node->parent && node->parent->id < result.searchReachCache.size()) {
                reachable = result.searchReachCache[node->parent->id];
            }
            result.searchReachCache[node->id] = reachable;
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
    m_previousFilterParams = m_filterParams;

    if (m_filterParams.hideNonMatching) {
        m_liveSplitCache.clear();
    }

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
    struct StackEntry {
        FileNode* node;
        QString path;
    };
    std::vector<StackEntry> stack;
    stack.push_back({root, root->computePath()});
    while (!stack.empty()) {
        StackEntry entry = stack.back();
        stack.pop_back();
        FileNode* node = entry.node;
        if (!node || node->isVirtual()) continue;

        node->id = index->nodeCount++;
        index->nodes.push_back(node);
        index->nameOffsets.push_back(static_cast<uint32_t>(index->flatNames.size()));
        const QByteArray folded = node->name.toCaseFolded().toUtf8();
        index->nameLens.push_back(static_cast<uint16_t>(std::min<int>(folded.size(), 65535)));
        index->flatNames.append(folded.constData(), folded.size());
        if (!node->isDirectory()) {
            index->filesByExt[ColorUtils::packFileExt(node->name)].push_back(node);
        }
        const QString pathPrefix = entry.path.endsWith(QLatin1Char('/')) ? entry.path : entry.path + QLatin1Char('/');
        std::vector<FileNode*> children;
        for (FileNode* child = node->firstChild; child; child = child->nextSibling) {
            children.push_back(child);
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back({*it, pathPrefix + (*it)->name});
        }
    }

    return index;
}

void TreemapWidget::clearIncrementalSearchState()
{
    m_previousDirectSearchMatches.clear();
    m_previousSearchCaseFoldedPattern.clear();
    m_previousSearchUsesWildcards = false;
    m_previousMinSizeFilter = 0;
    m_previousMaxSizeFilter = 0;
    m_previousFilterParams = {};
}

void TreemapWidget::rebuildSearchMetadataAsync()
{
    if (m_asyncShutdown) {
        return;
    }

    if (!m_root) {
        cancelPendingMetadata();
        cancelPendingSearch();
        cancelPendingFileTypeHighlight();
        m_metadataRestartPending = false;
        m_pendingMetadataRoot = nullptr;
        m_pendingMetadataArena.reset();
        m_pendingFileTypeIndex.reset();
        m_nodeCount = 0;
        m_searchMatchCache = {};
        m_fileTypeMatchCache = {};
        m_combinedMatchCache = {};
        m_searchReachCache = {};
        m_searchIndex.reset();
        clearIncrementalSearchState();
        return;
    }

    // Cancel any in-flight metadata work. We also cancel the search because it depends
    // on the metadata we're about to replace.
    cancelPendingMetadata();
    cancelPendingSearch();

    // Clear previous search results so we don't use them as a "stable" reference
    // while the new index is building — the IDs in the old results are about to
    // be invalidated.
    clearIncrementalSearchState();

    // Qt 6 race: QFutureInterface<T>::~QFutureInterface calls ResultStoreBase::clear<T>
    // without holding the result-store mutex (M0), but the background task holds M0
    // while writing into the result store via reportAndMoveResult. Calling setFuture
    // while the previous task is in the narrow window between reportAndMoveResult
    // completing and ~StoredFunctionCall decrementing the refcount triggers the race.
    //
    // Fix: never call setFuture while a task is in-flight. Instead, set a restart flag
    // and let onMetadataTaskFinished restart us after the task (and its destructor) are
    // fully done. The finished signal is queued, so by the time we handle it the
    // thread-pool thread has already deleted the StoredFunctionCall.
    if (m_metadataTaskRunning) {
        m_metadataRestartPending = true;
        return;
    }

    FileNode* const root = m_root;
    const std::shared_ptr<NodeArena> rootArena = m_snapshot ? m_snapshot->arena : nullptr;
    const QString rootPath = m_snapshot ? m_snapshot->rootPath : QString();
    auto cancelToken = m_metadataCancelToken;
    // Snapshot marks on main thread; looked up by path inside the background task.
    const QHash<QString, FolderMark> colorMarkSnapshot = m_settings.folderColorMarks;
    const QHash<QString, FolderMark> iconMarkSnapshot  = m_settings.folderIconMarks;

    m_metadataTaskRunning = true;
    m_pendingMetadataRoot = root;
    m_pendingMetadataArena = rootArena;
    const quint64 taskId = ++m_nextMetadataTaskId;
    m_activeMetadataTaskId = taskId;
    if (m_metadataThread.joinable()) {
        m_metadataThread.join();
    }
    const QPointer<TreemapWidget> that(this);
    m_metadataThread = std::thread([that, root, rootPath, rootArena, cancelToken,
                                    colorMarkSnapshot, iconMarkSnapshot, taskId]() mutable {
        auto index = [root, rootPath, rootArena, cancelToken, colorMarkSnapshot, iconMarkSnapshot]()
            -> std::shared_ptr<SearchIndex>
        {
            if (!rootArena) {
                return {};
            }
            auto index = std::make_shared<SearchIndex>();
            index->nodes.reserve(rootArena->totalAllocated());
            index->nameOffsets.reserve(index->nodes.capacity());
            index->nameLens.reserve(index->nodes.capacity());
            index->markCache.reserve(index->nodes.capacity());
            index->directMarkCache.reserve(index->nodes.capacity());
            index->pendingColorMarks.reserve(index->nodes.capacity());
            index->pendingIconMarks.reserve(index->nodes.capacity());

            struct StackEntry {
                FileNode* node;
                QString path;
                uint32_t inheritedMarks;
            };
            std::vector<StackEntry> stack;
            stack.push_back({root, rootPath, 0u});

            int i = 0;
            while (!stack.empty()) {
                if ((++i & 0xFFF) == 0 && cancelToken->load(std::memory_order_relaxed)) {
                    return {};
                }
                StackEntry entry = stack.back();
                stack.pop_back();
                FileNode* node = entry.node;
                if (!node || node->isVirtual()) continue;

                index->nodeCount++;
                index->nodes.push_back(node);

                // Compute marks but do NOT write to FileNode fields here — paintNode
                // reads colorMark/iconMark/id on the main thread concurrently.
                // Store them in the index and apply in onMetadataTaskFinished instead.
                index->pendingColorMarks.push_back(0);
                index->pendingIconMarks.push_back(0);
                uint32_t directMarks = 0;
                if (node->isDirectory()) {
                    auto itColor = colorMarkSnapshot.constFind(entry.path);
                    if (itColor != colorMarkSnapshot.constEnd()) {
                        const FolderMark m = itColor.value();
                        index->pendingColorMarks.back() = static_cast<uint8_t>(m);
                        directMarks |= (1u << static_cast<int>(m));
                    }
                    auto itIcon = iconMarkSnapshot.constFind(entry.path);
                    if (itIcon != iconMarkSnapshot.constEnd()) {
                        const FolderMark m = itIcon.value();
                        index->pendingIconMarks.back() = static_cast<uint8_t>(m);
                        directMarks |= (1u << static_cast<int>(m));
                    }
                }

                const uint32_t currentMarks = entry.inheritedMarks | directMarks;
                index->markCache.push_back(currentMarks);
                index->directMarkCache.push_back(directMarks);

                index->nameOffsets.push_back(static_cast<uint32_t>(index->flatNames.size()));
                const QByteArray folded = node->name.toCaseFolded().toUtf8();
                index->nameLens.push_back(static_cast<uint16_t>(std::min<int>(folded.size(), 65535)));
                index->flatNames.append(folded.constData(), folded.size());
                if (!node->isDirectory()) {
                    index->filesByExt[ColorUtils::packFileExt(node->name)].push_back(node);
                }

                const QString pathPrefix = entry.path.endsWith(QLatin1Char('/')) ? entry.path : entry.path + QLatin1Char('/');
                std::vector<FileNode*> children;
                for (FileNode* child = node->firstChild; child; child = child->nextSibling) {
                    children.push_back(child);
                }
                for (auto it = children.rbegin(); it != children.rend(); ++it) {
                    FileNode* child = *it;
                    if (!child) continue;
                    QString childPath = pathPrefix + child->name;
                    stack.push_back({child, childPath, currentMarks});
                }
            }

            index->arenaOwner = rootArena;
            return index;
        }();
        if (!qApp) {
            return;
        }
        QMetaObject::invokeMethod(
            qApp,
            [that, index = std::move(index), root, rootArena, taskId]() mutable {
                if (that) {
                    that->onMetadataTaskFinished(std::move(index), root, std::move(rootArena), taskId);
                }
            },
            Qt::QueuedConnection);
    });
}

void TreemapWidget::onMetadataTaskFinished(std::shared_ptr<SearchIndex> index,
                                           FileNode* expectedRoot,
                                           std::shared_ptr<NodeArena> expectedArena,
                                           quint64 taskId)
{
    if (m_asyncShutdown) {
        return;
    }

    if (m_metadataThread.joinable()) {
        m_metadataThread.join();
    }
    if (taskId != m_activeMetadataTaskId) {
        return;
    }

    m_metadataTaskRunning = false;

    // A newer root arrived while this task was running (see rebuildSearchMetadataAsync).
    // Restart with current state; skip the stale result from this task.
    if (m_metadataRestartPending) {
        m_metadataRestartPending = false;
        rebuildSearchMetadataAsync();
        return;
    }

    if (!index) return; // cancelled
    if (m_root != expectedRoot || !m_snapshot || m_snapshot->arena != expectedArena) {
        return;
    }

    // Apply FileNode field writes on the main thread so they don't race with
    // paintNode reading id/colorMark/iconMark. The background task staged these
    // in pendingColorMarks/pendingIconMarks rather than writing directly.
    for (uint32_t i = 0; i < index->nodeCount; ++i) {
        FileNode* node = index->nodes[i];
        node->id = i;
        node->setColorMark(index->pendingColorMarks[i]);
        node->setIconMark(index->pendingIconMarks[i]);
    }

    m_nodeCount = index->nodeCount;
    m_searchMatchCache.assign(m_nodeCount, 0);
    m_fileTypeMatchCache.assign(m_nodeCount, 0);
    m_searchIndex = index;
    // The new index has fresh node IDs; any incremental-search state built against
    // the old index is now invalid. Force a full search pass.
    clearIncrementalSearchState();
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
    m_pendingFileTypeIndex = index;
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
                if (node->isVirtual() || (node->parent && node->parent->isVirtual())) {
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
    if (m_asyncShutdown) {
        return;
    }

    emit fileTypeHighlightBusyChanged(false);
    if (m_pendingHighlightedFileType != m_highlightedFileType) {
        return;
    }
    if (m_pendingFileTypeIndex != m_searchIndex) {
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
        if (node->isDirectory() || node->id >= m_nodeCount) continue;
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
    FilterParams p = m_filterParams;
    p.sizeMin = minBytes;
    p.sizeMax = maxBytes;
    setFilterParams(p);
}


bool TreemapWidget::isDescendantOfDirectMatch(const FileNode* node) const
{
    // Check if the node or any ancestor is a direct match for the current active filters.
    // kSearchSelfMatch and kTypeSelfMatch both use 0x1.
    for (const FileNode* p = node; p; p = p->parent) {
        if (effectiveMatchFlags(p) & 0x01) {
            return true;
        }
    }
    return false;
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
    if (!node || !node->firstChild) return;

    std::vector<FileNode*> children;
    for (FileNode* child = node->firstChild; child; child = child->nextSibling) {
        children.push_back(child);
    }

    std::sort(children.begin(), children.end(),
              [](const FileNode* a, const FileNode* b) {
                  if (a->displaySize != b->displaySize) {
                      return a->displaySize > b->displaySize;
                  }
                  return a->name < b->name;
              });

    node->firstChild = children.front();
    for (size_t i = 0; i < children.size() - 1; ++i) {
        children[i]->nextSibling = children[i+1];
        sortChildrenBySize(children[i]);
    }
    children.back()->nextSibling = nullptr;
    sortChildrenBySize(children.back());
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

    const QString label = m_systemLocale.formattedDataSize(nodeDisplaySize(node));
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

TreemapWidget::FilteredChildren TreemapWidget::computeFilteredChildren(FileNode* node, bool parentMatches) const
{
    FilteredChildren result;
    if (!node) return result;

    for (FileNode* child = node->firstChild; child; child = child->nextSibling) {
        const qint64 childSize = nodeLayoutSize(child);
        if (childSize > 0) {
            if (!node->isVirtual() && child->isVirtual()) {
                result.freeChildren.push_back(child);
                result.freeTotal += childSize;
            } else {
                result.children.push_back(child);
            }
            result.total += childSize;
        }
    }

    // When "hide non-matching" is active, exclude children with no match from layout.
    if (m_filterParams.hideNonMatching && m_filterParams.isActive()) {
        // Include children if they match, OR if this node (or any ancestor) is a direct match.
        if (!parentMatches) {
            result.children.erase(std::remove_if(result.children.begin(), result.children.end(),
                [this](const FileNode* c) { return effectiveMatchFlags(c) == 0; }),
                result.children.end());
            result.total = 0;
            for (const auto* c : result.children) result.total += nodeLayoutSize(c);
            for (const auto* c : result.freeChildren) result.total += nodeLayoutSize(c);
        }
    }
    return result;
}

void TreemapWidget::layoutVisibleChildren(FileNode* node, const QRectF& tileViewRect,
                                          const QRectF& viewContent,
                                          const QRectF& visibleClip,
                                          std::vector<std::pair<FileNode*, QRectF>>& out,
                                          bool parentMatches) const
{
    out.clear();
    if (!node || !node->isDirectory() || !node->firstChild
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
        FilteredChildren filtered = computeFilteredChildren(node, parentMatches);
        std::vector<FileNode*>& children = filtered.children;
        std::vector<FileNode*>& freeChildren = filtered.freeChildren;
        qint64 total = filtered.total;
        qint64 freeTotal = filtered.freeTotal;

        if ((children.empty() && freeChildren.empty()) || total <= 0) {
            return;
        }

        std::sort(children.begin(), children.end(),
                  [](const FileNode* a, const FileNode* b) {
                      if (a->displaySize != b->displaySize) {
                          return a->displaySize > b->displaySize;
                      }
                      return a->name < b->name;
                  });
        std::sort(freeChildren.begin(), freeChildren.end(),
                  [](const FileNode* a, const FileNode* b) {
                      if (a->displaySize != b->displaySize) {
                          return a->displaySize > b->displaySize;
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

        QRectF layoutRect(0, 0, ar, 1.0);
        
        if (!freeChildren.empty() && total > 0) {
            const double fraction = static_cast<double>(freeTotal) / total;
            QRectF freeRect;

            if (layoutRect.width() >= layoutRect.height()) {
                const double w = layoutRect.width() * fraction;
                freeRect = QRectF(layoutRect.right() - w, layoutRect.top(), w, layoutRect.height());
                layoutRect.setWidth(layoutRect.width() - w);
            } else {
                const double h = layoutRect.height() * fraction;
                freeRect = QRectF(layoutRect.left(), layoutRect.bottom() - h, layoutRect.width(), h);
                layoutRect.setHeight(layoutRect.height() - h);
            }

            squarifiedLayout(freeChildren, freeRect, freeTotal, normalized, true);
        }

        squarifiedLayout(children, layoutRect, total - freeTotal, normalized, true);
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
    const int visibleDepthLimit = std::max(0, m_activeSemanticDepth - 1);
    if (depth >= visibleDepthLimit) {
        // If "hide non-matching" is active, we allow recursing much deeper because 
        // the number of items is restricted by the filter.
        if (!(m_filterParams.hideNonMatching && m_searchActive)) {
            return false;
        }
        const int maxDepthLimit = std::max(0, m_settings.maxSemanticDepth - 1);
        if (depth >= maxDepthLimit) {
            return false;
        }
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
    const qreal baseRevealDepth = std::max(0, m_settings.baseVisibleDepth - 1);
    if (childDepth > baseRevealDepth) {
        const qreal revealDepth = baseRevealDepth
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
        // Render previous frame first and copy it out, so the second render into
        // m_liveFrame doesn't clobber it. The next frame is left sharing m_liveFrame
        // (safe: the animation paint branch never calls renderSceneToPixmap, and the
        // finished handler clears both frames before the next paint).
        // Both frames are rendered at the current semantic depth so no extra layout
        // work is needed; the depth transition is handled by the finished handler.
        m_cameraPreviousFrame = renderSceneToPixmap(m_current).copy();

        const qreal savedScale = m_cameraScale;
        const QPointF savedOrigin = m_cameraOrigin;
        m_cameraScale = m_cameraTargetScale;
        m_cameraOrigin = m_cameraTargetOrigin;

        m_cameraNextFrame = renderSceneToPixmap(m_current);

        m_cameraScale = savedScale;
        m_cameraOrigin = savedOrigin;
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

bool TreemapWidget::navigationAnimationInProgress() const
{
    return m_zoomAnimation.state() == QAbstractAnimation::Running
        || m_cameraAnimation.state() == QAbstractAnimation::Running;
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
    const bool hideOverlayScrollBars = hasOpenImagePreview();
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
    setOverlayScrollBarVisible(m_overlayHScrollBar, !hideOverlayScrollBars && maxH > 0);
    setOverlayScrollBarVisible(m_overlayVScrollBar, !hideOverlayScrollBars && maxV > 0);
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

void TreemapWidget::drawScene(QPainter& painter, FileNode* root, const QRectF& visibleClip,
                              SceneRenderLayer layer)
{
    const QRect full = viewport()->rect();
    if (!root) {
        if (layer != SceneRenderLayer::DynamicOnly) {
            painter.fillRect(full, palette().color(QPalette::Window));
        }
        return;
    }

    m_framePalette = palette();
    m_framePixelScale   = pixelScale();
    m_frameOutlineWidth = snapLengthToPixels(m_settings.border, m_framePixelScale);
    m_frameImagePreviewNode = currentImagePreviewNode();

    // The current folder owns the full viewport. Descendant geometry is derived
    // recursively from that root-bleed rect, clipped to the requested visible area.
    const QRectF clip = visibleClip.isValid() ? visibleClip : QRectF(full);
    painter.setClipRect(full, Qt::IntersectClip);
    const bool initialMatches = isDescendantOfDirectMatch(root);
    paintNode(painter, root, -1, clip, currentRootViewRect(), 0.0, 0.0, true, initialMatches, layer);
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

    const bool drawBorders = !(m_filterParams.hideNonMatching && m_searchActive);
    const bool initialMatches = isDescendantOfDirectMatch(root);
    painter.save();
    painter.setPen(Qt::NoPen);
    paintMatchOverlayNode(painter, root, -1, clip, currentRootViewRect(), drawBorders, initialMatches);
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

    // Rendering to animation frames: use a local pixmap so the live static/dynamic
    // cache (m_liveStaticFrame / m_liveDynamicFrame) is not disturbed. The next
    // paintEvent will detect stateChanged and redraw the live cache as needed.
    if (root != m_current) {
        m_lastLiveRoot = nullptr;
    }

    QPixmap frame(deviceSize);
    frame.setDevicePixelRatio(dpr);
    frame.fill(palette().color(QPalette::Window));

    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const QRectF fullClip(0, 0, viewport()->width(), viewport()->height());
    drawScene(painter, root, fullClip, SceneRenderLayer::StaticOnly);
    drawScene(painter, root, fullClip, SceneRenderLayer::DynamicOnly);
    drawMatchOverlay(painter, root, fullClip);
    return frame;
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
                                        FileNode* target, QRectF* outRect, int depth, bool parentMatches) const
{
    if (!root || !target || !outRect || !rootViewRect.isValid() || rootViewRect.isEmpty()) {
        return false;
    }

    if (!canPaintChildrenForDisplay(root, rootViewRect, depth)) {
        return false;
    }

    const bool currentMatches = parentMatches || (effectiveMatchFlags(root) & 0x01);

    const QRectF contentArea = childPaintRectForNode(root, rootViewRect);
    const QRectF contentClip = contentArea.adjusted(-1.0, -1.0, 1.0, 1.0);

    std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
    layoutVisibleChildren(root, rootViewRect, contentArea, contentClip, visibleChildren, currentMatches);
    for (const auto& [child, childViewRect] : visibleChildren) {
        if (childViewRect.width() < m_settings.minPaint || childViewRect.height() < m_settings.minPaint) {
            continue;
        }
        if (child == target) {
            *outRect = snapRectToPixels(childViewRect, pixelScale());
            return true;
        }
        if (!child->isDirectory()) {
            continue;
        }
        if (findVisibleViewRect(child, childViewRect, target, outRect, depth + 1, currentMatches)) {
            return true;
        }
    }

    return false;
}

// ── Recursive paint ──────────────────────────────────────────────────────────

void TreemapWidget::paintNode(QPainter& p, FileNode* node, int depth,
                              const QRectF& visibleClip, const QRectF& viewRect,
                              qreal subtreeHoverBlend, qreal subtreePrevHoverBlend,
                              bool applyOwnReveal, bool parentMatches, SceneRenderLayer layer)
{
    const QRectF r = viewRect;
    const QRectF effectiveClip = r.intersected(visibleClip);
    if (r.width() < m_settings.minPaint || r.height() < m_settings.minPaint || effectiveClip.isEmpty())
        return;

    const bool currentMatches = parentMatches || (effectiveMatchFlags(node) & 0x01);

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
    const qreal childSubtreeHoverBlend = (node == m_hovered && m_hovered && m_hovered->isDirectory())
        ? m_hoverBlend : subtreeHoverBlend;
    const qreal childSubtreePrevHoverBlend = (node == m_previousHovered && m_previousHovered && m_previousHovered->isDirectory())
        ? (1.0 - m_hoverBlend) : subtreePrevHoverBlend;

    if (node->isDirectory()) {
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
            layoutVisibleChildren(node, r, state.childLayoutRect, state.childContentClip, visibleChildren, currentMatches);
            for (const auto& [child, childRect] : visibleChildren) {
                if (!childRect.intersects(state.childContentClip)) {
                    continue;
                }

                const int childDepth = depth + 1;
                qreal childSemanticOpacity = 1.0;
                const qreal baseRevealDepth = std::max(0, m_settings.baseVisibleDepth - 1);
                if (childDepth > baseRevealDepth) {
                    const qreal revealDepth = baseRevealDepth
                        + std::max<qreal>(0.0,
                                          std::log2(std::max<qreal>(1.0, m_cameraScale))
                                              * m_settings.depthRevealPerZoomDoubling);
                    childSemanticOpacity = smoothstep(
                        std::clamp(revealDepth - childDepth, 0.0, 1.0));
                }
                if (childSemanticOpacity <= 0.0) {
                    continue;
                }

                const qreal childRevealOpacity = std::min(
                    tileRevealOpacityForNode(child, childRect), childSemanticOpacity);
                const qreal childTinyOpacity = std::min(
                    tinyChildRevealOpacityForLayout(child, childRect), childSemanticOpacity);

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
                const qreal childNodeHoverStrength = std::max(
                    (child == m_hovered)         ? m_hoverBlend         : 0.0,
                    (child == m_previousHovered) ? (1.0 - m_hoverBlend) : 0.0);
                const qreal childBgHighlightStrength = std::max(
                    childNodeHoverStrength * highlightOpacity, childSubtreeHighlightStrength);

                const QColor fc = QColor::fromRgba(child->color);

                // Use the folder's panel color if it is a directory and chrome is revealing.
                // We blend from the saturated color to the panel color manually so we
                // can use our gamma-correct blendColors() instead of the muddy sRGB
                // alpha-blending provided by QPainter.
                QColor baseFill = fc;
                if (child->isDirectory()) {
                    const qreal childNodeHighlightStrength = childNodeHoverStrength * highlightOpacity;
                    baseFill = childNodeHighlightStrength > 0.0
                        ? blendColors(fc, hoverBase, childNodeHighlightStrength)
                        : fc;
                }

                const QColor fillColor = childBgHighlightStrength > 0.0
                    ? blendColors(baseFill, hoverBase, childBgHighlightStrength) : baseFill;
                // For file tiles, always pre-composite the fill colour against the
                // panel background using gamma-correct blending.  opaqueCompositeColor
                // is a no-op for fully-opaque colours, so this handles both cases.
                const QColor effectiveFillColor = child->isDirectory()
                    ? fillColor
                    : opaqueCompositeColor(childBackgroundColor, fillColor);

                // Draw background at the calculated total opacity for this child
                if (layer != SceneRenderLayer::DynamicOnly) {
                    paintTinyNodeFill(p, childRect, effectiveFillColor, m_framePixelScale,
                                     tileRevealOpacity * childFillOpacity);
                }

                // If detailed features (chrome, labels, children) should show, recurse.
                if (childRevealOpacity > 0.0
                        && childRect.width() >= m_settings.minPaint
                        && childRect.height() >= m_settings.minPaint) {
                    p.save();
                    p.setOpacity(baseOpacity * tileRevealOpacity * childRevealOpacity);
                    paintNode(p, child, depth + 1, state.childContentClip, childRect,
                              childSubtreeHoverBlend, childSubtreePrevHoverBlend, false, currentMatches, layer);
                    p.restore();
                }
            }
            p.restore();
        };

        if (node == m_current) {
            p.save();
            p.setClipRect(ri, Qt::IntersectClip);
            if (layer != SceneRenderLayer::DynamicOnly) {
                p.fillRect(ri, contentAreaColor);
            }
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
            if (layer != SceneRenderLayer::DynamicOnly
                    && directoryState.childPaintRect.width() > 1.0
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

        if (applyOwnReveal && layer != SceneRenderLayer::DynamicOnly) {
            p.save();
            p.setClipRect(directoryState.tileFillClipRect, Qt::IntersectClip);
            p.setOpacity(baseOpacity * tileRevealOpacity);
            // Fill entire tile with the folder's base colour. The content area gets a
            // panel-colour overlay at chromeOpacity below, so chrome and body appear together.
            p.fillRect(tileFillRect, colorBg);
            p.restore();
        }

        if (layer != SceneRenderLayer::DynamicOnly
                && directoryState.showChrome
                && directoryState.childPaintRect.width() > 1.0
                && directoryState.childPaintRect.height() > 1.0) {
            // Always fill the content area with the panel colour, even when
            // applyOwnReveal is false.  childBackgroundColor is contentAreaColor,
            // so opaqueCompositeColor() pre-composites file tiles against the
            // panel colour; the background must actually be that colour or
            // semi-transparent files will look wrong (too dark, or brighter on
            // hover when the partial-repaint chromeOutsideClip path corrects it).
            p.save();
            p.setClipRect(directoryState.contentFillClipRect, Qt::IntersectClip);
            p.setOpacity(baseOpacity * tileRevealOpacity);
            p.fillRect(directoryState.childPaintRect, contentAreaColor);
            p.restore();
        }

        if (directoryState.showChrome) {
            p.save();
            p.setClipRect(directoryState.tileFillClipRect, Qt::IntersectClip);
            if (layer != SceneRenderLayer::DynamicOnly) {
                p.setOpacity(baseOpacity * tileRevealOpacity * directoryState.chromeOpacity);
                // Ring area (between tile and childPaintRect) is already colorBg from the base fill;
                // only the header needs its own fill to support the per-node hover highlight.
                p.fillRect(directoryState.framedHeaderRect, effectiveHeaderColor);
            }

            if (depth >= 0 && layer != SceneRenderLayer::StaticOnly) {
                const QColor headerTextColor = contrastingTextColor(effectiveHeaderColor);
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
                const bool headerWasVisible = (node->id < m_headerLabelVisibleCache.size()) ? m_headerLabelVisibleCache[node->id] : false;
                const bool keepHeaderVisible = stableHeaderWidth >= hideHeaderWidth
                    && stableHeaderHeight >= hideHeaderHeight;
                const bool becomeHeaderVisible = stableHeaderWidth >= showHeaderWidth
                    && stableHeaderHeight >= showHeaderHeight;
                const bool headerVisible = headerWasVisible ? keepHeaderVisible : becomeHeaderVisible;
                if (node->id < m_headerLabelVisibleCache.size()) m_headerLabelVisibleCache[node->id] = headerVisible;
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
                                    layoutDirection(),
                                    static_cast<FolderMark>(node->iconMark()));
                }
            }
            p.restore();
        }

        paintVisibleChildren(directoryState);

        if (outlineWidth > 0.0 && layer != SceneRenderLayer::DynamicOnly) {
            p.save();
            p.setOpacity(baseOpacity * tileRevealOpacity);
            // Highlight border matches header/hoverBase exactly
            const QColor highlightBorderBase = hoverBase;
            const qreal clampedBorderIntensity = std::clamp(m_settings.borderIntensity, 0.0, 1.0);
            const QColor contrastBorder = contrastingBorderColor(colorBg);
            const QColor effectiveBorderColor = highlightBorderStrength > 0.0
                ? opaqueCompositeColor(blendColors(outerBorderBase, highlightBorderBase, highlightBorderStrength),
                                       blendColors(Qt::transparent, contrastBorder, clampedBorderIntensity))
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
        if (fillColor.alphaF() < 1.0 && node->parent && node->parent->isDirectory()) {
            const QColor parentBase = QColor::fromRgba(node->parent->color);
            const QColor parentPanelBase = cachedPanelBase(parentBase, m_framePalette.color(QPalette::Base));
            const QColor parentFillColor = subtreeHighlightStrength > 0.0
                ? blendColors(parentPanelBase, hoverBase, subtreeHighlightStrength)
                : parentPanelBase;
            fillColor = opaqueCompositeColor(parentFillColor, fillColor);
        }

        if (applyOwnReveal && layer != SceneRenderLayer::DynamicOnly) {
            p.fillRect(ri, fillColor);
        }

        qreal finalThumbnailAlpha = 0.0;
        const qreal thumbFullSize = std::max<qreal>(1.0, m_settings.thumbnailMinTileSize);
        const qreal thumbRevealDistance = std::clamp(thumbFullSize * 0.3, 16.0, 32.0);
        const qreal thumbWidthHysteresis = 8.0;
        const qreal thumbHeightHysteresis = 8.0;
        const qreal thumbStartSize = std::max<qreal>(1.0, thumbFullSize - thumbRevealDistance);
        const QSizeF stableThumbnailSize = stabilizedNodeSize(
            node, StableMetricChannel::ThumbnailReveal, ri.size(),
            kRevealWidthBucketPx, kRevealHeightBucketPx,
            thumbWidthHysteresis, thumbHeightHysteresis);
        const qreal thumbnailRevealOpacity = revealOpacityForSize(
            stableThumbnailSize,
            thumbStartSize,
            thumbStartSize,
            thumbFullSize,
            thumbFullSize);
        const bool isVideoNode = supportedVideoExtKeys().contains(ColorUtils::packFileExt(node->name));
        if (m_settings.showThumbnails && thumbnailRevealOpacity > 0.0
                && (supportedImageExtKeys().contains(ColorUtils::packFileExt(node->name))
                    || (isVideoNode && m_settings.showVideoThumbnails))) {
            const QString path = nodePath(node);
            if (!path.isEmpty()) {
                auto storeIt = m_thumbnailStore.find(path);
                if (storeIt != m_thumbnailStore.end()) {
                    m_thumbnailLastAccess[path] = ++m_thumbnailAccessSeq;
                    m_thumbnailFramePinSet.insert(path);
                    const QPixmap& pixmap = storeIt.value();
                    const qint64 readyTime = m_thumbnailReadyTimes.value(path, 0);
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    const qreal loadAlpha = std::clamp((now - readyTime) / 300.0, 0.0, 1.0);
                    finalThumbnailAlpha = loadAlpha * thumbnailRevealOpacity;

                    if (layer != SceneRenderLayer::StaticOnly && finalThumbnailAlpha > 0.0) {
                        p.save();
                        p.setOpacity(baseOpacity * tileRevealOpacity * finalThumbnailAlpha);
                        const QSize imgSize = pixmap.size();
                        const bool isAnimatingFade = (loadAlpha < 1.0);
                        p.setRenderHint(QPainter::SmoothPixmapTransform, !isAnimatingFade);
                        const QRectF thumbnailRect = ri.adjusted(
                            outlineWidth, outlineWidth, -outlineWidth, -outlineWidth);
                        if (thumbnailRect.width() > 0.0 && thumbnailRect.height() > 0.0) {
                            const int fitMode = m_settings.thumbnailFitMode;
                            if (fitMode == TreemapSettings::ThumbnailFill) {
                                QSizeF scaledSize = QSizeF(imgSize).scaled(thumbnailRect.size(), Qt::KeepAspectRatioByExpanding);
                                QRectF imgRect(0, 0, scaledSize.width(), scaledSize.height());
                                imgRect.moveCenter(thumbnailRect.center());
                                p.setClipRect(thumbnailRect);
                                p.drawPixmap(imgRect, pixmap, pixmap.rect());
                            } else if (fitMode == TreemapSettings::ThumbnailFit) {
                                QSizeF scaledSize = QSizeF(imgSize).scaled(thumbnailRect.size(), Qt::KeepAspectRatio);
                                QRectF imgRect(0, 0, scaledSize.width(), scaledSize.height());
                                imgRect.moveCenter(thumbnailRect.center());
                                p.drawPixmap(imgRect, pixmap, pixmap.rect());
                            } else {
                                p.drawPixmap(thumbnailRect, pixmap, pixmap.rect());
                            }
                        }
                        p.restore();
                    }
                    if (loadAlpha < 1.0) {
                        QTimer::singleShot(16, this, [this]() { viewport()->update(); });
                    }
                } else if (!m_pendingThumbnails.contains(path) && !m_thumbnailFailedPaths.contains(path)) {
                    m_pendingThumbnails.insert(path);
                    QThreadPool::globalInstance()->start(new ThumbnailTask(
                        path, this,
                        m_settings.thumbnailResolution,
                        isVideoNode ? 0 : m_settings.thumbnailMaxFileSizeMB,
                        m_settings.thumbnailSkipNetworkPaths,
                        isVideoNode));
                }
            }
        }

        if (outlineWidth > 0.0 && layer != SceneRenderLayer::DynamicOnly) {
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
            const QColor contrastBorder = contrastingBorderColor(fillColor);
            const QColor fileBorderColor = bgHighlightStrength > 0.0
                ? opaqueCompositeColor(blendColors(fileBorderBase, highlightBorderBase, bgHighlightStrength),
                                       blendColors(Qt::transparent, contrastBorder, clampedBorderIntensity))
                : fileBorderBase;

            QColor effectiveFileBorderColor = fileBorderColor;
            if (!m_settings.randomColorForUnknownFiles && fc.alphaF() < 1.0 && node->parent && node->parent->isDirectory()) {
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
        const bool wasVisible = (node->id < m_fileLabelVisibleCache.size()) ? m_fileLabelVisibleCache[node->id] : false;
        const bool keepVisible = stableWidth >= hideWidth && stableHeight >= hideHeight;
        const bool becomeVisible = stableWidth >= showWidth && stableHeight >= showHeight;
        const bool textVisible = wasVisible ? keepVisible : becomeVisible;
        if (node->id < m_fileLabelVisibleCache.size()) m_fileLabelVisibleCache[node->id] = textVisible;
        const qreal widthSpan = std::max<qreal>(1.0, 40.0 - hideWidth);
        const qreal oneLineHeightSpan = std::max<qreal>(1.0, (showHeight + 6.0) - hideHeight);
        const qreal widthFade = smoothstep(std::clamp(
            (stableWidth - hideWidth) / widthSpan, 0.0, 1.0));
        const qreal oneLineHeightFade = smoothstep(std::clamp(
            (stableHeight - hideHeight) / oneLineHeightSpan, 0.0, 1.0));
        const qreal previewLabelFade = (node == m_frameImagePreviewNode)
            ? std::clamp(1.0 - smoothstep(std::clamp(m_imagePreviewProgress, 0.0, 1.0)), 0.0, 1.0)
            : 1.0;
        const qreal textFade = std::min(widthFade, oneLineHeightFade) * previewLabelFade;
        if (textFade > 0.0 && layer != SceneRenderLayer::StaticOnly) {
            // Replace nested save/restore with direct opacity arithmetic — the outer
            // file p.save()/p.restore() already guards all state for the caller.
            const QColor rawFileTextColor = contrastingTextColor(fillColor);
            const QColor fileTextColor = finalThumbnailAlpha > 0.4 ? Qt::white : rawFileTextColor;

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

            if (finalThumbnailAlpha > 0.1) {
                const qreal bgAlpha = std::clamp(finalThumbnailAlpha, 0.0, 1.0) * 0.7;
                p.save();
                p.setOpacity(baseOpacity * tileRevealOpacity * textFade * bgAlpha);

                const qreal totalTextHeight = (sizeFade > 0.0) 
                    ? (m_fileFm.height() + m_fileFm.lineSpacing())
                    : m_fileFm.height();

                const int nameW = m_fileFm.horizontalAdvance(nameText);
                int totalW = nameW;
                if (sizeFade > 0.0) {
                    const QString sizeText = cachedElidedLabelWithBucket(
                        node, cachedSizeLabel(node), textWidth, m_fileFm, 3, kFileElideBucketPx,
                        layoutDirection());
                    totalW = std::max(totalW, m_fileFm.horizontalAdvance(sizeText));
                }

                // Flush with the inner border edge so the fill doesn't overlap the tile border.
                const QRectF bgRect(
                    ri.left() + outlineWidth,
                    ri.top() + outlineWidth,
                    std::min<qreal>(ri.width() - outlineWidth, totalW + 8.0),
                    std::min<qreal>(ri.height() - outlineWidth, totalTextHeight + 4.0));

                p.fillRect(bgRect, QColor(0, 0, 0, 180));
                p.restore();
            }
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

        // Hard link badge: small chain icon in top-right corner for files with nlink > 1.
        if (m_settings.showFileFlags && node->hasHardLinks() && ri.width() >= 16.0 && ri.height() >= 16.0
                && layer != SceneRenderLayer::StaticOnly) {
            constexpr qreal kBadgeSize = 13.0;
            const QColor textColor = contrastingTextColor(fillColor);
            const QIcon badge = makeRecoloredSvgIcon(
                QStringLiteral(":/assets/tabler-icons/link.svg"), textColor);
            const QRectF badgeRect(
                ri.right() - outlineWidth - kBadgeSize - 1.0,
                ri.top() + outlineWidth + 1.0,
                kBadgeSize, kBadgeSize);
            p.setOpacity(baseOpacity * tileRevealOpacity * 0.65);
            badge.paint(&p, badgeRect.toRect(), Qt::AlignCenter);
        }

        p.restore();
    }
}

void TreemapWidget::paintMatchOverlayNode(QPainter& painter, FileNode* node, int depth,
                                          const QRectF& visibleClip, const QRectF& viewRect,
                                          bool drawBorders, bool parentMatches) const
{
    if (!node) return;

    const QRectF r = viewRect;
    const qreal devicePixelScale = pixelScale();
    const QRectF effectiveClip = r.intersected(visibleClip);
    if (r.width() < m_settings.minPaint || r.height() < m_settings.minPaint || effectiveClip.isEmpty())
        return;

    const quint8 eFlags = effectiveMatchFlags(node);
    const bool matched = parentMatches || (eFlags != 0);
    const bool isViewRoot = (node == m_current);

    // Don't dim the root while the first async result is still in flight — prevents brief flicker.
    if (isViewRoot && !matched) {
        if (m_searchActive && m_searchWatcher->isRunning() && m_previousDirectSearchMatches.empty())
            return;
        if (!m_highlightedFileType.isEmpty() && m_fileTypeWatcher->isRunning() && m_previousHighlightedFileType.isEmpty())
            return;
    }

    // Draw highlight border for direct search matches (uses searchMatchFlags, not effectiveMatchFlags,
    // so file-type highlight alone doesn't trigger borders).
    if (drawBorders && matched && !isViewRoot) {
        if (searchMatchFlags(node) & kSearchSelfMatch) {
            const QRectF borderRect = snapRectToPixels(r, devicePixelScale);
            const qreal borderWidth = snapLengthToPixels(
                std::max<qreal>(m_settings.border * 1.5, 2.0 / devicePixelScale),
                devicePixelScale);
            if (borderWidth > 0.0 && !borderRect.isEmpty())
                fillInnerBorder(painter, borderRect, m_settings.highlightColor, borderWidth);
        }
    }

    const QColor baseColor = palette().color(QPalette::Base);
    const QColor midColor = palette().color(QPalette::Mid);
    const QColor fadeBaseColor = blendColors(baseColor, midColor, 0.35);
    const QColor panelFadeColor(fadeBaseColor.red(), fadeBaseColor.green(), fadeBaseColor.blue(), 110);
    const QColor subtreeFadeColor(fadeBaseColor.red(), fadeBaseColor.green(), fadeBaseColor.blue(), 140);

    if (!matched) {
        painter.fillRect(snapRectToPixels(effectiveClip, devicePixelScale), subtreeFadeColor);
        return;
    }

    if (!node->isDirectory()) return;  // File match: stays clear.

    if (!canPaintChildrenForDisplay(node, r, depth)) return;

    const bool directMatch = (eFlags & kTypeSelfMatch);
    const bool currentMatches = parentMatches || directMatch;

    QRectF contentLayout;
    QRectF contentClip;
    if (isViewRoot) {
        contentLayout = childLayoutRectForNode(node, r);
        contentClip = childPaintRectForNode(node, r).adjusted(-1.0, -1.0, 1.0, 1.0).intersected(effectiveClip);
    } else {
        const bool detailedChrome = folderDetailOpacityForNode(node, r) > 0.0;
        const TileChromeGeometry chrome = makeTileChromeGeometry(
            r, m_settings, true, detailedChrome, devicePixelScale);
        contentLayout = detailedChrome ? chrome.contentLayoutRect : childLayoutRectForNode(node, r);
        contentClip = chrome.contentPaintRect.adjusted(-1.0, -1.0, 1.0, 1.0).intersected(effectiveClip);
        // Chrome (header) stays clear to help identify the path.
    }

    std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
    layoutVisibleChildren(node, r, contentLayout, contentClip, visibleChildren, currentMatches);

    // Dim children that have no match; recurse into those that do.
    // Skip dimming when this folder is itself a direct pattern match (its contents stay clear).
    const bool preserveSubtree = !m_searchPattern.isEmpty() && !isViewRoot;
    const bool shouldDimChildren = isViewRoot || !directMatch || !preserveSubtree;
    for (const auto& [child, childRect] : visibleChildren) {
        const QRectF childClip = childRect.intersected(contentClip);
        if (childClip.isEmpty()) continue;

        if (!currentMatches && effectiveMatchFlags(child) == 0) {
            if (shouldDimChildren)
                painter.fillRect(snapRectToPixels(childClip, devicePixelScale), panelFadeColor);
        } else {
            paintMatchOverlayNode(painter, child, depth + 1, childClip, childRect, drawBorders, currentMatches);
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
            paintImagePreviewOverlay(painter);
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
        paintImagePreviewOverlay(painter);
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
        paintImagePreviewOverlay(painter);
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
    // the same), all tiles shift by a fixed pixel offset.  To avoid stale text/thumbnail
    // artifacts, we maintain two separate cached frames: m_liveStaticFrame (opaque tile
    // fills, chrome borders) and m_liveDynamicFrame (transparent; text labels, thumbnails).
    // Each is scrolled and redrawn independently with the correct composition mode so
    // semi-transparent dynamic elements compose correctly against the static background.
    const bool purePan = !m_liveStaticFrame.isNull()
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

            // Prepare scroll buffer (reuse across frames; resize only on viewport change).
            if (m_scrollBuffer.isNull() || m_scrollBuffer.size() != m_liveStaticFrame.size()
                    || m_scrollBuffer.devicePixelRatio() != dpr) {
                m_scrollBuffer = QPixmap(deviceSize);
                m_scrollBuffer.setDevicePixelRatio(dpr);
            }

            // --- Static layer: shift opaque frame, repaint exposed strips ---
            {
                QPainter sp(&m_scrollBuffer);
                sp.setRenderHint(QPainter::Antialiasing, false);
                sp.fillRect(viewport()->rect(), palette().color(QPalette::Window));
                sp.drawPixmap(QPointF(dx, dy), m_liveStaticFrame);
                sp.setClipping(true);
                const auto drawStaticStrip = [&](const QRectF& strip) {
                    if (strip.width() < 0.5 || strip.height() < 0.5) return;
                    sp.setClipRect(strip);
                    drawScene(sp, m_current, strip, SceneRenderLayer::StaticOnly);
                };
                drawStaticStrip(xStrip);
                drawStaticStrip(yStrip);
            }
            std::swap(m_liveStaticFrame, m_scrollBuffer);

            // --- Dynamic layer: always full redraw ---
            // Text labels and thumbnails are cheap to render and may have live
            // animations (thumbnail fade-in) that must update every frame.  Doing a
            // full DynamicOnly pass here keeps them correct without re-drawing the
            // expensive static geometry (tile fills, chrome, borders).
            if (m_liveDynamicFrame.isNull() || m_liveDynamicFrame.size() != deviceSize
                    || m_liveDynamicFrame.devicePixelRatio() != dpr) {
                m_liveDynamicFrame = QPixmap(deviceSize);
                m_liveDynamicFrame.setDevicePixelRatio(dpr);
            }
            m_liveDynamicFrame.fill(Qt::transparent);
            {
                QPainter dp(&m_liveDynamicFrame);
                dp.setRenderHint(QPainter::Antialiasing, false);
                drawScene(dp, m_current, QRectF(viewport()->rect()), SceneRenderLayer::DynamicOnly);
                drawMatchOverlay(dp, m_current, QRectF(viewport()->rect()));
            }
            pruneThumbnailCache();

            m_lastLiveRoot   = m_current;
            m_lastLiveOrigin = m_cameraOrigin;
            m_lastLiveScale  = m_cameraScale;
            m_lastLiveDepth  = m_activeSemanticDepth;

            painter.drawPixmap(0, 0, m_liveStaticFrame);
            painter.drawPixmap(0, 0, m_liveDynamicFrame);
            paintImagePreviewOverlay(painter);
            return;
        }
        // Fall through to full redraw for large pan steps (e.g. scroll-bar jump).
    }

    const bool stateChanged = (m_current != m_lastLiveRoot ||
                               m_cameraOrigin != m_lastLiveOrigin ||
                               m_cameraScale != m_lastLiveScale ||
                               m_activeSemanticDepth != m_lastLiveDepth ||
                               m_liveStaticFrame.isNull() ||
                               m_liveDynamicFrame.isNull() ||
                               m_liveFrameDeviceSize != deviceSize);

    if (stateChanged) {
        m_lastLiveRoot = m_current;
        m_lastLiveOrigin = m_cameraOrigin;
        m_lastLiveScale = m_cameraScale;
        m_lastLiveDepth = m_activeSemanticDepth;
        if (m_liveStaticFrame.isNull() || m_liveFrameDeviceSize != deviceSize) {
            m_liveStaticFrame = QPixmap(deviceSize);
            m_liveStaticFrame.setDevicePixelRatio(dpr);
            m_liveDynamicFrame = QPixmap(deviceSize);
            m_liveDynamicFrame.setDevicePixelRatio(dpr);
            m_liveFrameDeviceSize = deviceSize;
        }
        m_thumbnailFramePinSet.clear();
        {
            QPainter sp(&m_liveStaticFrame);
            sp.setRenderHint(QPainter::Antialiasing, false);
            sp.fillRect(viewport()->rect(), palette().color(QPalette::Window));
            drawScene(sp, m_current, QRectF(viewport()->rect()), SceneRenderLayer::StaticOnly);
        }
        m_liveDynamicFrame.fill(Qt::transparent);
        {
            QPainter dp(&m_liveDynamicFrame);
            dp.setRenderHint(QPainter::Antialiasing, false);
            drawScene(dp, m_current, QRectF(viewport()->rect()), SceneRenderLayer::DynamicOnly);
            drawMatchOverlay(dp, m_current, QRectF(viewport()->rect()));
        }
        pruneThumbnailCache();
    } else if (!dirty.isEmpty()) {
        // Partial update for hover etc
        {
            QPainter sp(&m_liveStaticFrame);
            sp.setRenderHint(QPainter::Antialiasing, false);
            sp.setClipRect(dirty);
            // Clear the dirty region before repainting so cached pixels don't show through.
            sp.fillRect(dirty, palette().color(QPalette::Window));
            drawScene(sp, m_current, QRectF(dirty), SceneRenderLayer::StaticOnly);
        }
        {
            QPainter dp(&m_liveDynamicFrame);
            dp.setRenderHint(QPainter::Antialiasing, false);
            dp.setCompositionMode(QPainter::CompositionMode_Source);
            dp.fillRect(dirty, Qt::transparent);
            dp.setCompositionMode(QPainter::CompositionMode_SourceOver);
            dp.setClipRect(dirty);
            drawScene(dp, m_current, QRectF(dirty), SceneRenderLayer::DynamicOnly);
            drawMatchOverlay(dp, m_current, QRectF(dirty));
        }
    }

    painter.drawPixmap(0, 0, m_liveStaticFrame);
    painter.drawPixmap(0, 0, m_liveDynamicFrame);

    if (m_filterParams.hideNonMatching && m_filterParams.isActive()
            && m_current && effectiveMatchFlags(m_current) == 0
            && !m_searchWatcher->isRunning()) {
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(viewport()->rect(), Qt::AlignCenter, tr("No matching items"));
    }

    paintImagePreviewOverlay(painter);
    paintLaunchAnimations(painter);

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
                                         const QRectF& visibleClip, bool parentMatches,
                                         QRectF* outRect) const
{
    std::vector<std::pair<FileNode*, QRectF>> visibleChildren;
    layoutVisibleChildren(node, tileViewRect, contentArea, visibleClip, visibleChildren, parentMatches);
    for (const auto& [child, childViewRect] : visibleChildren) {
        if (!childViewRect.contains(pos)) {
            continue;
        }
        FileNode* hit = hitTest(child, pos, depth + 1, childViewRect, parentMatches, outRect);
        if (hit) {
            return hit;
        }
    }
    return nullptr;
}

FileNode* TreemapWidget::hitTest(FileNode* node, const QPointF& pos, int depth,
                                 const QRectF& viewRect, bool parentMatches, QRectF* outRect) const
{
    if (!node || !viewRect.contains(pos))
        return nullptr;

    const bool currentMatches = parentMatches || (effectiveMatchFlags(node) & 0x01);

    if (!canPaintChildrenForDisplay(node, viewRect, depth)) {
        if (outRect) {
            *outRect = viewRect;
        }
        return node;
    }

    const QRectF contentArea = childPaintRectForNode(node, viewRect);
    const QRectF contentClip = contentArea.adjusted(-0.75, -0.75, 0.75, 0.75);
    if (FileNode* hit = hitTestChildren(node, pos, depth, viewRect, contentArea, contentClip, currentMatches, outRect)) {
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
    if (hasOpenImagePreview()) {
        event->accept();
        return;
    }
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
        m_cameraUseFocusAnchor = false;
        m_cameraPreviousFrame = QPixmap();
        m_cameraNextFrame = QPixmap();
        syncScrollBars();
        viewport()->update();
        return;
    }

    updatePressHold(event->position());
    updateHoverAt(event->position(), event->globalPosition().toPoint());
}

void TreemapWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::BackButton) {
        cancelPressHold();
        if (hasOpenImagePreview()) {
            closeImagePreviewFromNavigation();
        } else {
            emit backRequested();
        }
        return;
    }

    if (hasOpenImagePreview()) {
        if (event->button() == Qt::LeftButton) {
            closeImagePreview(true);
            event->accept();
            return;
        }
        event->ignore();
        return;
    }

    if (m_touchGestureActive) {
        event->ignore();
        return;
    }

    if (m_scanInProgress) {
        event->ignore();
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
    if (hasOpenImagePreview()) {
        event->accept();
        return;
    }

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
        const QString pendingPath = m_pressHoldPath;
        const bool pressHoldTriggered = m_pressHoldTriggered;
        const QPointF pressPos = event->position();
        QRectF hitRect;
        const bool initialMatches = isDescendantOfDirectMatch(m_current);
        FileNode* hit = hitTest(m_current, pressPos, -1, currentRootViewRect(), initialMatches, &hitRect);
        if (!hit || hit == m_current || hit->isVirtual()) {
            hit = nullptr;
            hitRect = QRectF();
        }
        cancelPressHold();
        const bool samePath = hit
            && !pendingPath.isEmpty()
            && !hit->isVirtual()
            && (hit->computePath() == pendingPath);
        if (!pressHoldTriggered && samePath) {
            m_lastActivationPos = pressPos;
            if (hit->isDirectory()) {
                emit nodeActivated(hit);
            } else if (nodeSupportsImagePreview(hit)) {
                requestImagePreview(hit, hitRect);
            } else if (!m_settings.doubleClickToOpen) {
                emit nodeActivated(hit);
            }
        }
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void TreemapWidget::paintLaunchAnimations(QPainter& painter)
{
    if (m_launchAnimations.isEmpty()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (const auto& anim : m_launchAnimations) {
        if (anim.progress >= 1.0) continue;

        // White flash that expands to 1.3x and fades out.
        const qreal expansion = 1.0 + (anim.progress * 0.3);
        const qreal opacity = 1.0 - anim.progress;

        const QPointF center = anim.rect.center();
        const qreal w = anim.rect.width() * expansion;
        const qreal h = anim.rect.height() * expansion;
        const QRectF drawRect(center.x() - w/2.0, center.y() - h/2.0, w, h);

        painter.setOpacity(opacity);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 160));
        painter.drawRect(drawRect);
    }

    painter.restore();
}

void TreemapWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (hasOpenImagePreview()) {
        event->accept();
        return;
    }

    if (m_touchGestureActive) {
        event->ignore();
        return;
    }

    if (m_scanInProgress) {
        event->ignore();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        if (m_settings.doubleClickToOpen && m_current) {
            QRectF hitRect;
            const bool initialMatches = isDescendantOfDirectMatch(m_current);
            FileNode* hit = hitTest(m_current, event->position(), -1, currentRootViewRect(), initialMatches, &hitRect);
            if (hit && hit != m_current && !hit->isVirtual()) {
                m_lastActivationPos = event->position();
                if (hit->isDirectory()) {
                    emit nodeActivated(hit);
                } else {
                    // Trigger white flash launch animation for files.
                    LaunchAnimation anim;
                    anim.rect = hitRect;
                    anim.progress = 0.0;
                    m_launchAnimations.append(anim);

                    if (m_launchProgressAnimation.state() != QAbstractAnimation::Running) {
                        m_launchProgressAnimation.start();
                    } else {
                        m_launchProgressAnimation.setCurrentTime(0);
                    }
                    emit nodeOpenFile(hit);
                }
            }
        }
        event->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}

void TreemapWidget::keyPressEvent(QKeyEvent* event)
{
    if (hasOpenImagePreview() && !event->isAutoRepeat()) {
        const bool isBackKey = event->matches(QKeySequence::Back)
            || event->key() == Qt::Key_Back
            || event->key() == Qt::Key_Backspace
            || event->key() == Qt::Key_Escape
            || (event->modifiers() == Qt::AltModifier && event->key() == Qt::Key_Left);
        if (isBackKey) {
            closeImagePreviewFromNavigation();
            event->accept();
            return;
        }
    }

    QAbstractScrollArea::keyPressEvent(event);
}

void TreemapWidget::wheelEvent(QWheelEvent* event)
{
    if (hasOpenImagePreview()) {
        event->ignore();
        return;
    }

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
        if (FileNode* const previewNode = currentImagePreviewNode()) {
            const QRectF updatedSource = imagePreviewSourceRectForNode(previewNode);
            if (updatedSource.isValid()) {
                m_imagePreviewSourceRect = updatedSource;
            }
        }
    }
    m_liveSplitCache.clear();
    syncScrollBars();
    m_resizeTimer.start();
}

void TreemapWidget::leaveEvent(QEvent*)
{
    if (hasOpenImagePreview()) {
        return;
    }

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
    if (hasOpenImagePreview()) {
        event->ignore();
        return;
    }

    cancelPressHold();
    if (m_scanInProgress || !m_current) {
        event->ignore();
        return;
    }

    const QPointF pos = event->pos();
    const bool initialMatches = isDescendantOfDirectMatch(m_current);
    FileNode* hit = hitTest(m_current, pos, -1,
                            currentRootViewRect(), initialMatches);
    if (hit && hit != m_current && !hit->isVirtual()) {
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

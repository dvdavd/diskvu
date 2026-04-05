// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "mainwindow_utils.h"

#include "colorutils.h"
#include "iconutils.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDirIterator>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHeaderView>
#include <QImage>
#include <QLocale>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QScreen>
#include <QStorageInfo>
#include <QSvgRenderer>
#include <QStyleHints>
#include <QStyle>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <unordered_map>

static QHash<QString, QIcon> s_colorSwatchCache;
static QHash<QString, QIcon> s_tintedFolderCache;
static QHash<QString, QIcon> s_recoloredSvgCache;

namespace {

constexpr auto kPaletteInitializedProperty = "diskscapePaletteInitialized";
constexpr auto kPaletteOverrideProperty = "diskscapePaletteOverride";
constexpr auto kCachedLightPaletteProperty = "diskscapeCachedLightPalette";
constexpr auto kCachedDarkPaletteProperty = "diskscapeCachedDarkPalette";

bool isUsablePalette(const QPalette& palette)
{
    return palette.color(QPalette::Window).isValid()
        && palette.color(QPalette::WindowText).isValid()
        && palette.color(QPalette::Base).isValid();
}

QPalette cachedObservedPalette(const QApplication& app, bool darkMode)
{
    return app.property(darkMode ? kCachedDarkPaletteProperty
                                 : kCachedLightPaletteProperty).value<QPalette>();
}

// Returns the DPRs of all connected screens plus 1.0 as a baseline.
// Used for per-color icons (swatches, tinted folders) where count × colors matters.
QList<qreal> screenDevicePixelRatios()
{
    QList<qreal> ratios = {1.0};

    if (qApp) {
        const auto screens = qApp->screens();
        for (const QScreen* screen : screens) {
            if (!screen) {
                continue;
            }
            ratios.append(screen->devicePixelRatio());
        }
    }

    std::sort(ratios.begin(), ratios.end());
    ratios.erase(std::unique(ratios.begin(), ratios.end(), [](qreal a, qreal b) {
        return qAbs(a - b) < 0.01;
    }), ratios.end());
    return ratios;
}

// Returns a broad set of DPRs covering common fractional ratios.
// Used for toolbar/SVG icons (small count, pre-rendered so moves between
// screens look sharp without reloading).
QList<qreal> speculativeDevicePixelRatios()
{
    QList<qreal> ratios = {
        1.0, 1.25, 1.5, 1.75,
        2.0, 2.25, 2.5, 3.0, 4.0
    };

    if (qApp) {
        const auto screens = qApp->screens();
        for (const QScreen* screen : screens) {
            if (!screen) {
                continue;
            }
            ratios.append(screen->devicePixelRatio());
        }
    }

    std::sort(ratios.begin(), ratios.end());
    ratios.erase(std::unique(ratios.begin(), ratios.end(), [](qreal a, qreal b) {
        return qAbs(a - b) < 0.01;
    }), ratios.end());
    return ratios;
}

QByteArray normalizeTablerSvg(QByteArray svgData)
{
    svgData.replace("stroke-width=\"1\"", "stroke-width=\"1.5\"");
    return svgData;
}

QColor softenedReadableTextColor(const QPalette& palette, qreal alpha)
{
    QColor color = palette.color(QPalette::WindowText);
    color.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return color;
}

QColor softenedReadableBorderColor(const QPalette& palette, qreal alpha)
{
    QColor color = palette.color(QPalette::WindowText);
    color.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return color;
}

bool paletteLooksDark(const QPalette& palette)
{
    return palette.color(QPalette::Window).lightnessF() < 0.5;
}

void cacheObservedPalette(QApplication& app, bool darkMode, const QPalette& palette)
{
    if (!isUsablePalette(palette) || paletteLooksDark(palette) != darkMode) {
        return;
    }

    app.setProperty(darkMode ? kCachedDarkPaletteProperty
                             : kCachedLightPaletteProperty,
                    palette);
}

QPalette styleStandardPalette(const QApplication& app)
{
    return app.style() ? app.style()->standardPalette() : QPalette();
}

void setPaletteSyncState(QApplication& app, bool initialized, bool overrideActive)
{
    app.setProperty(kPaletteInitializedProperty, initialized);
    app.setProperty(kPaletteOverrideProperty, overrideActive);
}

QPalette fallbackDarkApplicationPalette()
{
    QPalette palette;
    const QColor windowColor(QStringLiteral("#232629"));
    const QColor baseColor(QStringLiteral("#1b1e20"));
    const QColor alternateBaseColor(QStringLiteral("#2b2f33"));
    const QColor textColor(QStringLiteral("#eff0f1"));
    const QColor disabledTextColor(QStringLiteral("#6b7278"));
    const QColor buttonColor(QStringLiteral("#2c3034"));
    const QColor highlightColor(QStringLiteral("#3daee9"));
    const QColor highlightedTextColor(QStringLiteral("#0b0d0e"));
    const QColor linkColor(QStringLiteral("#58a6ff"));
    const QColor visitedLinkColor(QStringLiteral("#c792ea"));
    const QColor shadowColor(QStringLiteral("#111315"));

    palette.setColor(QPalette::Window, windowColor);
    palette.setColor(QPalette::WindowText, textColor);
    palette.setColor(QPalette::Base, baseColor);
    palette.setColor(QPalette::AlternateBase, alternateBaseColor);
    palette.setColor(QPalette::ToolTipBase, baseColor);
    palette.setColor(QPalette::ToolTipText, textColor);
    palette.setColor(QPalette::Text, textColor);
    palette.setColor(QPalette::Button, buttonColor);
    palette.setColor(QPalette::ButtonText, textColor);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Link, linkColor);
    palette.setColor(QPalette::LinkVisited, visitedLinkColor);
    palette.setColor(QPalette::Highlight, highlightColor);
    palette.setColor(QPalette::HighlightedText, highlightedTextColor);
    palette.setColor(QPalette::Light, windowColor.lighter(145));
    palette.setColor(QPalette::Midlight, windowColor.lighter(120));
    palette.setColor(QPalette::Mid, windowColor.lighter(150));
    palette.setColor(QPalette::Dark, shadowColor);
    palette.setColor(QPalette::Shadow, shadowColor);
    palette.setColor(QPalette::PlaceholderText, QColor(textColor.red(), textColor.green(), textColor.blue(), 140));

    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledTextColor);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabledTextColor);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledTextColor);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledTextColor);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(highlightColor.red(),
                                                                     highlightColor.green(),
                                                                     highlightColor.blue(),
                                                                     90));
    return palette;
}

} // namespace

void dumpThemeState(const char* location, const QApplication& app)
{
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    const char* schemeStr = scheme == Qt::ColorScheme::Dark  ? "Dark"
                          : scheme == Qt::ColorScheme::Light ? "Light"
                                                             : "Unknown";
    const QPalette appPal   = app.palette();
    const QPalette stylePal = app.style() ? app.style()->standardPalette() : QPalette();
    // qDebug("[theme:%s] colorScheme=%s  appPalWindow=%.2f(%s)  stylePalWindow=%.2f(%s)"
    //        "  initialized=%d  overrideActive=%d",
    //        location, schemeStr,
    //        appPal.color(QPalette::Window).lightnessF(),
    //        paletteLooksDark(appPal) ? "dark" : "light",
    //        stylePal.color(QPalette::Window).lightnessF(),
    //        paletteLooksDark(stylePal) ? "dark" : "light",
    //        app.property("diskscapePaletteInitialized").toBool(),
    //        app.property("diskscapePaletteOverride").toBool());
}

QFont generalUiFont()
{
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont);
}

int landingTileWidth()
{
    return qMax(96, QFontMetrics(generalUiFont()).height() * 6);
}

void applyMenuFontPolicy(QApplication& app)
{
    app.setFont(generalUiFont(), "QMenu");
}

bool syncApplicationPaletteToColorScheme(QApplication& app, bool darkMode)
{
    dumpThemeState(darkMode ? "sync(dark)" : "sync(light)", app);
    const bool initialized = app.property(kPaletteInitializedProperty).toBool();
    const bool overrideActive = app.property(kPaletteOverrideProperty).toBool();
    const bool currentDark = paletteLooksDark(app.palette());
    const QPalette stylePalette = styleStandardPalette(app);
    const bool styleDark = paletteLooksDark(stylePalette);

    cacheObservedPalette(app, currentDark, app.palette());
    cacheObservedPalette(app, styleDark, stylePalette);

    if (initialized) {
        const QPalette cachedTargetPalette = cachedObservedPalette(app, darkMode);
        const bool cachedTargetValid = isUsablePalette(cachedTargetPalette)
            && paletteLooksDark(cachedTargetPalette) == darkMode;

        if (cachedTargetValid && currentDark != darkMode && styleDark == darkMode) {
            app.setPalette(cachedTargetPalette);
            setPaletteSyncState(app, true, false);
            return true;
        }

        if (styleDark == darkMode && (styleDark != currentDark || overrideActive)) {
            app.setPalette(stylePalette);
            setPaletteSyncState(app, true, false);
            return true;
        }

        if (darkMode && !currentDark && !styleDark) {
            app.setPalette(fallbackDarkApplicationPalette());
            setPaletteSyncState(app, true, true);
            return true;
        }

        if (styleDark == darkMode && styleDark != currentDark) {
            app.setPalette(stylePalette);
            setPaletteSyncState(app, true, false);
            return true;
        }
        return false;
    }

    // Startup path: if the live style palette already matches the desired
    // light/dark mode, trust it. Only fall back to the bundled dark palette
    // when Qt starts in light mode inside a dark session.
    if (styleDark == darkMode) {
        if (currentDark == darkMode) {
            setPaletteSyncState(app, true, false);
            return false;
        }
        app.setPalette(stylePalette);
        setPaletteSyncState(app, true, false);
        return true;
    }

    if (darkMode) {
        if (currentDark) {
            setPaletteSyncState(app, true, overrideActive);
            return false;
        }
        app.setPalette(fallbackDarkApplicationPalette());
        setPaletteSyncState(app, true, true);
        return true;
    }

    app.setPalette(stylePalette);
    setPaletteSyncState(app, true, false);
    return paletteLooksDark(stylePalette) != currentDark || overrideActive;
}

bool widgetChromeUsesDarkColorScheme()
{
    const QPalette palette = qApp ? qApp->palette() : QApplication::palette();
    return paletteLooksDark(palette);
}

QColor landingLocationBorderColor()
{
    const QPalette palette = qApp ? qApp->palette() : QPalette();
    return softenedReadableBorderColor(palette, 0.25);
}

QIcon menuActionIcon(std::initializer_list<const char*> names,
                     const QString& lightResource,
                     const QString& darkResource,
                     QStyle::StandardPixmap fallback)
{
    QIcon icon = IconUtils::themeIcon(names, lightResource, darkResource);
    if (!icon.isNull()) {
        return icon;
    }

    return qApp ? qApp->style()->standardIcon(fallback) : QIcon();
}

QString landingLocationStyleSheet()
{
    const QPalette palette = qApp ? qApp->palette() : QPalette();
    QColor pressedHighlight = qApp ? qApp->palette().color(QPalette::Highlight) : QColor(QStringLiteral("#4a90e2"));
    pressedHighlight.setAlpha(128);
    const QString pressedHighlightCss = pressedHighlight.name(QColor::HexArgb);
    const QColor panelBorder = landingLocationBorderColor();
    const QString panelBorderCss = panelBorder.name(QColor::HexArgb);
    const bool isDark = paletteLooksDark(palette);
    const QString tileHoverBorderCss = softenedReadableBorderColor(palette, isDark ? 0.28 : 0.20).name(QColor::HexArgb);
    const QString tilePressedBorderCss = palette.color(QPalette::Highlight).name(QColor::HexArgb);
    const QString panelBackgroundCss = palette.color(QPalette::Base).name(QColor::HexArgb);
    const QString tileHoverBackgroundCss = softenedReadableTextColor(palette, 0.08).name(QColor::HexArgb);
    const QString emptyLabelCss = softenedReadableTextColor(palette, 0.72).name(QColor::HexArgb);

    return QStringLiteral(
        "QWidget#landingLocationContainer {"
        "  border: 1px solid %1;"
        "  border-radius: 8px;"
        "  background: %2;"
        "}"
        "QToolButton#landingLocationTile {"
        "  color: palette(button-text);"
        "  padding: 2px 2px 4px 2px;"
        "  border: 1px solid transparent;"
        "  border-radius: 6px;"
        "  background: transparent;"
        "}"
        "QToolButton#landingLocationTile:hover,"
        "QToolButton#landingLocationTile[pinHovered=\"true\"] {"
        "  border: 1px solid %3;"
        "  background: %4;"
        "}"
        "QToolButton#landingLocationTile:pressed {"
        "  border: 1px solid %5;"
        "  background: %6;"
        "}"
        "QToolButton#landingLocationPin {"
        "  padding: 0;"
        "  margin: 6px;"
        "  border: 1px solid transparent;"
        "  border-radius: 10px;"
        "  background: transparent;"
        "}"
        "QLabel#landingLocationEmpty {"
        "  color: %7;"
        "  padding: 0;"
        "}")
        .arg(panelBorderCss,
             panelBackgroundCss,
             tileHoverBorderCss,
             tileHoverBackgroundCss,
             tilePressedBorderCss,
             pressedHighlightCss,
             emptyLabelCss);
}

QString normalizedFilesystemPath(const QString& path)
{
    QString normalized = QDir::fromNativeSeparators(QDir::cleanPath(path.trimmed()));
#ifdef Q_OS_WIN
    if (normalized.size() == 2 && normalized.at(1) == QLatin1Char(':')) {
        normalized += QLatin1Char('/');
    }
#endif
    return normalized;
}

QList<BreadcrumbPathSegment> breadcrumbPathSegments(const QString& path)
{
    const QString normalized = normalizedFilesystemPath(path);
    if (normalized.isEmpty()) {
        return {};
    }

    QList<BreadcrumbPathSegment> segments;

#ifdef Q_OS_WIN
    if (normalized.size() >= 3
            && normalized.at(1) == QLatin1Char(':')
            && normalized.at(2) == QLatin1Char('/')) {
        QString currentPath = normalized.left(3);
        segments.push_back({QDir::toNativeSeparators(currentPath), currentPath});
        const QStringList descendants = normalized.mid(3).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString& descendant : descendants) {
            if (!currentPath.endsWith(QLatin1Char('/'))) {
                currentPath += QLatin1Char('/');
            }
            currentPath += descendant;
            segments.push_back({descendant, currentPath});
        }
        return segments;
    }
#endif

    QString currentPath;
    if (normalized.startsWith(QLatin1Char('/'))) {
        currentPath = QStringLiteral("/");
        segments.push_back({QStringLiteral("/"), currentPath});
    }

    const QStringList descendants = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& descendant : descendants) {
        if (currentPath.isEmpty() || currentPath == QStringLiteral("/")) {
            currentPath += descendant;
        } else {
            currentPath += QLatin1Char('/');
            currentPath += descendant;
        }
        segments.push_back({descendant, currentPath});
    }

    return segments;
}

bool systemUsesDarkColorScheme()
{
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark) {
        return true;
    }
    if (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light) {
        return false;
    }
    return QApplication::palette().color(QPalette::Window).lightnessF() < 0.5;
}

QSettings appSettings()
{
    return QSettings(QStringLiteral("diskscape"), QStringLiteral("diskscape"));
}

void saveSettingsAsync(std::function<void(QSettings&)> fn)
{
    QThreadPool::globalInstance()->start([fn = std::move(fn)]() {
        QSettings store = appSettings();
        fn(store);
    });
}


QIcon makeColorSwatchIcon(const QColor& color)
{
    const QString key = color.name(QColor::HexArgb);
    auto it = s_colorSwatchCache.find(key);
    if (it != s_colorSwatchCache.end()) {
        return it.value();
    }

    const QList<int> logicalSizes = {12, 14, 16, 18};
    const QList<qreal> dprs = screenDevicePixelRatios();
    QIcon icon;
    for (const qreal dpr : dprs) {
        for (const int logicalSize : logicalSizes) {
            QPixmap pixmap(qMax(1, qRound(logicalSize * dpr)), qMax(1, qRound(logicalSize * dpr)));
            pixmap.setDevicePixelRatio(dpr);
            pixmap.fill(Qt::transparent);

            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            const qreal scale = logicalSize / 12.0;
            const qreal leftPadding = 2.5 * scale;
            const qreal swatchSize = logicalSize - leftPadding - (2.5 * scale);
            const qreal topInset = (logicalSize - swatchSize) / 2.0;
            painter.setPen(QPen(color.darker(140), scale));
            painter.setBrush(color);
            painter.drawRoundedRect(QRectF(leftPadding, topInset, swatchSize, swatchSize),
                                    2.0 * scale,
                                    2.0 * scale);
            painter.end();
            icon.addPixmap(pixmap);
        }
    }

    s_colorSwatchCache.insert(key, icon);
    return icon;
}

QIcon makeTintedFolderIcon(const QColor& color)
{
    const QColor outlineColor = qApp
        ? qApp->palette().color(QPalette::WindowText)
        : QColor(QStringLiteral("#444444"));
    const QString key = QStringLiteral("%1:%2")
        .arg(outlineColor.name(QColor::HexArgb),
             color.name(QColor::HexArgb));
    auto it = s_tintedFolderCache.find(key);
    if (it != s_tintedFolderCache.end()) {
        return it.value();
    }

    const QString templatePath = QStringLiteral(":/assets/tabler-icons/folder.svg");
    QFile templateFile(templatePath);
    if (!templateFile.open(QIODevice::ReadOnly)) {
        return QIcon();
    }
    QByteArray svgData = normalizeTablerSvg(templateFile.readAll());
    // Apply the tint by replacing the root SVG element's fill="none" with the
    // target color. The folder path has no explicit fill so it inherits from
    // the root; the background clear path has its own fill="none" so it is
    // unaffected. Only the first occurrence is replaced (the root attribute).
    const QByteArray oldRootFill = "fill=\"none\"";
    const QByteArray newRootFill = QByteArray("fill=\"") + color.name(QColor::HexRgb).toUtf8() + "\"";
    const int rootFillPos = svgData.indexOf(oldRootFill);
    if (rootFillPos != -1)
        svgData.replace(rootFillPos, oldRootFill.size(), newRootFill);
    const QByteArray oldStroke = "stroke=\"currentColor\"";
    const QByteArray newStroke = QByteArray("stroke=\"") + outlineColor.name(QColor::HexRgb).toUtf8() + "\"";
    const int strokePos = svgData.indexOf(oldStroke);
    if (strokePos != -1)
        svgData.replace(strokePos, oldStroke.size(), newStroke);
    QSvgRenderer renderer(svgData);
    if (!renderer.isValid()) {
        return QIcon();
    }

    const QList<int> logicalSizes = {16, 18, 20, 22, 24, 32, 48};
    const QList<qreal> dprs = screenDevicePixelRatios();

    QIcon icon;
    for (const qreal dpr : dprs) {
        for (const int logicalSize : logicalSizes) {
            QPixmap pixmap(qMax(1, qRound(logicalSize * dpr)), qMax(1, qRound(logicalSize * dpr)));
            pixmap.setDevicePixelRatio(dpr);
            pixmap.fill(Qt::transparent);

            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            renderer.render(&painter, QRectF(0, 0, logicalSize, logicalSize));
            painter.end();

            icon.addPixmap(pixmap);
        }
    }

    s_tintedFolderCache.insert(key, icon);
    return icon;
}

void clearIconCaches()
{
    s_colorSwatchCache.clear();
    s_tintedFolderCache.clear();
    s_recoloredSvgCache.clear();
}

static QPixmap renderSvgToPixmap(QSvgRenderer& renderer, int logicalSize, qreal dpr)
{
    QPixmap pixmap(qMax(1, qRound(logicalSize * dpr)), qMax(1, qRound(logicalSize * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, QRectF(0, 0, logicalSize, logicalSize));
    return pixmap;
}

QIcon makeRecoloredSvgIcon(const QString& svgPath, const QColor& color)
{
    const QColor disabledColor = qApp
        ? qApp->palette().color(QPalette::Disabled, QPalette::ButtonText)
        : color;
    const QString key = svgPath + QLatin1Char(':')
        + color.name(QColor::HexArgb) + QLatin1Char(':')
        + disabledColor.name(QColor::HexArgb);
    auto it = s_recoloredSvgCache.find(key);
    if (it != s_recoloredSvgCache.end())
        return it.value();

    QFile file(svgPath);
    if (!file.open(QIODevice::ReadOnly))
        return QIcon();
    const QByteArray svgTemplate = normalizeTablerSvg(file.readAll());

    auto recolor = [&](const QColor& c) {
        QByteArray data = svgTemplate;
        data.replace(QByteArrayLiteral("currentColor"), c.name(QColor::HexRgb).toUtf8());
        return data;
    };

    const QByteArray normalSvg = recolor(color);
    const QByteArray disabledSvg = recolor(disabledColor);
    QSvgRenderer normalRenderer(normalSvg);
    QSvgRenderer disabledRenderer(disabledSvg);
    if (!normalRenderer.isValid() || !disabledRenderer.isValid())
        return QIcon();

    const QList<int> logicalSizes = {16, 18, 20, 22, 24, 32, 48};
    const QList<qreal> dprs = speculativeDevicePixelRatios();
    QIcon icon;
    for (const qreal dpr : dprs) {
        for (int logicalSize : logicalSizes) {
            icon.addPixmap(renderSvgToPixmap(normalRenderer, logicalSize, dpr), QIcon::Normal);
            icon.addPixmap(renderSvgToPixmap(disabledRenderer, logicalSize, dpr), QIcon::Disabled);
        }
    }

    s_recoloredSvgCache.insert(key, icon);
    return icon;
}

QIcon toolbarIcon(std::initializer_list<const char*> /*names*/, const QString& resource)
{
    const QColor color = qApp
        ? qApp->palette().color(QPalette::ButtonText)
        : QColor(QStringLiteral("#444444"));
    return makeRecoloredSvgIcon(resource, color);
}

namespace {

constexpr int kLegendSortValueRole = Qt::UserRole + 1;
constexpr int kLegendColorRole = Qt::UserRole + 2;

class LegendTreeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override
    {
        const QTreeWidget* owner = treeWidget();
        const int column = owner ? owner->sortColumn() : 0;
        const QVariant leftValue = data(column, kLegendSortValueRole);
        const QVariant rightValue = other.data(column, kLegendSortValueRole);
        if (leftValue.isValid() && rightValue.isValid()) {
            switch (leftValue.metaType().id()) {
            case QMetaType::LongLong:
                return leftValue.toLongLong() < rightValue.toLongLong();
            case QMetaType::Int:
                return leftValue.toInt() < rightValue.toInt();
            default:
                break;
            }
        }
        return QTreeWidgetItem::operator<(other);
    }
};

// kNoExtKey and kFreeSpaceKey are sentinel uint64_t values for the two special
// buckets that don't correspond to a real extension pack.
static constexpr uint64_t kNoExtKey      = UINT64_MAX;
static constexpr uint64_t kFreeSpaceKey  = UINT64_MAX - 1;

void accumulateSummaryNode(FileTypeSummary& summary, const FileNode* node)
{
    if (!node) {
        return;
    }
    summary.color = QColor::fromRgba(node->color);
    summary.totalSize += node->size;
    ++summary.count;
}

void collectVirtualFileTypeSummaries(const FileNode* root, QHash<uint64_t, FileTypeSummary>& summaries,
                                     const std::vector<bool>& searchReach)
{
    if (!root || !searchReach.empty()) {
        return;
    }

    for (const FileNode* child : root->children) {
        if (!child || !child->isVirtual) {
            continue;
        }
        FileTypeSummary& summary = summaries[kFreeSpaceKey];
        if (summary.label.isEmpty()) {
            summary.label = QCoreApplication::translate("ColorUtils", "Free Space");
        }
        accumulateSummaryNode(summary, child);
    }
}

void collectSubtreeFileTypeSummaries(const FileNode* node, QHash<uint64_t, FileTypeSummary>& summaries,
                                     const std::vector<bool>& searchReach)
{
    if (!node || node->isVirtual) {
        return;
    }

    if (!node->isDirectory) {
        const bool reachable = searchReach.empty()
            || (node->id < searchReach.size() && searchReach[node->id]);
        if (!reachable) {
            return;
        }

        const uint64_t key = node->extKey ? node->extKey : kNoExtKey;
        FileTypeSummary& summary = summaries[key];
        if (summary.label.isEmpty()) {
            summary.label = (key == kNoExtKey)
                ? QCoreApplication::translate("ColorUtils", "No extension")
                : ColorUtils::fileTypeLabelForNode(node);
        }
        accumulateSummaryNode(summary, node);
        return;
    }

    for (const FileNode* child : node->children) {
        collectSubtreeFileTypeSummaries(child, summaries, searchReach);
    }
}

std::vector<FileNode*> collectAndMaybeStripVirtualNodes(std::vector<FileNode*>& children,
                                                         bool show)
{
    std::vector<FileNode*> freeSpaceNodes;
    for (FileNode* child : children) {
        if (child && child->isVirtual)
            freeSpaceNodes.push_back(child);
    }
    if (!freeSpaceNodes.empty() && !show) {
        children.erase(std::remove_if(children.begin(), children.end(),
                                      [](FileNode* c) { return c && c->isVirtual; }),
                       children.end());
    }
    return freeSpaceNodes;
}

void appendUniquePath(QStringList& paths, const QString& path)
{
    if (path.isEmpty() || paths.contains(path)) {
        return;
    }
    paths.push_back(path);
}

void collectDescendantDirectoryPaths(FileNode* node, int remainingDepth, QStringList& paths, int maxPaths)
{
    if (!node || remainingDepth < 0 || !node->isDirectory || node->isVirtual || paths.size() >= maxPaths) {
        return;
    }

    appendUniquePath(paths, node->computePath());
    if (remainingDepth == 0) {
        return;
    }

    for (FileNode* child : node->children) {
        collectDescendantDirectoryPaths(child, remainingDepth - 1, paths, maxPaths);
    }
}

void injectFreeSpaceNodeIfNeeded(ScanResult& scanResult, const QString& currentPath,
                                 const TreemapSettings& settings)
{
    if (!scanResult.root || scanResult.filesystems.isEmpty())
        return;

    const QString canonicalCurrentPath = QFileInfo(currentPath).canonicalFilePath();

    // Only inject when the scan starts at a filesystem root
    bool atFsRoot = false;
    for (const FsInfo& fs : scanResult.filesystems) {
        if (fs.canonicalMountRoot == canonicalCurrentPath) {
            atFsRoot = true;
            break;
        }
    }
    if (!atFsRoot)
        return;

    for (const FsInfo& fs : scanResult.filesystems) {
        if (fs.freeBytes <= 0)
            continue;
        if (!fs.isLocal && settings.hideNonLocalFreeSpace)
            continue;

        const bool isPrimary = (fs.canonicalMountRoot == canonicalCurrentPath);
        const QString label = isPrimary
            ? QCoreApplication::translate("ColorUtils", "Free Space")
            : QCoreApplication::translate("ColorUtils", "Free Space (%1)").arg(fs.displayMountRoot);

        FileNode* freeNode = scanResult.arena->alloc();
        freeNode->name = label;
        freeNode->size = fs.freeBytes;
        freeNode->isVirtual = true;
        freeNode->isDirectory = false;
        freeNode->color = settings.freeSpaceColor.rgba();
        // absolutePath stores the canonical mount root so navigation can redistribute this node
        freeNode->absolutePath = fs.canonicalMountRoot;
        freeNode->parent = scanResult.root;
        scanResult.root->children.push_back(freeNode);
    }
}

} // namespace

QList<FileTypeSummary> collectAndSortFileSummaries(FileNode* root, const std::vector<bool>& searchReach,
                                                   std::shared_ptr<SearchIndex> searchIndex)
{
    Q_UNUSED(searchIndex);

    QHash<uint64_t, FileTypeSummary> summaries;
    collectVirtualFileTypeSummaries(root, summaries, searchReach);
    collectSubtreeFileTypeSummaries(root, summaries, searchReach);

    QList<FileTypeSummary> ordered = summaries.values();
    std::sort(ordered.begin(), ordered.end(),
              [](const FileTypeSummary& a, const FileTypeSummary& b) {
                  if (a.totalSize != b.totalSize) {
                      return a.totalSize > b.totalSize;
                  }
                  return a.label < b.label;
              });
    return ordered;
}

void populateTypeLegendItems(QTreeWidget* tree, QLabel* summaryLabel,
                             TreemapWidget* treemapWidget,
                             const QList<FileTypeSummary>& ordered)
{
    const bool sortingEnabled = tree->isSortingEnabled();
    const int sortColumn = tree->sortColumn();
    const Qt::SortOrder sortOrder = tree->header() ? tree->header()->sortIndicatorOrder()
                                                   : Qt::AscendingOrder;
    tree->setSortingEnabled(false);
    tree->clear();

    if (ordered.isEmpty()) {
        if (summaryLabel) {
            summaryLabel->setText(QCoreApplication::translate("MainWindow", "No files in the current treemap"));
        }
        tree->setSortingEnabled(sortingEnabled);
        return;
    }

    const QString highlightedType = treemapWidget ? treemapWidget->highlightedFileType() : QString();
    bool foundHighlightedType = highlightedType.isEmpty();
    qint64 totalSize = 0;
    int totalCount = 0;

    const QLocale locale = QLocale::system();
    QList<QTreeWidgetItem*> items;
    items.reserve(ordered.size());
    QTreeWidgetItem* selectedItem = nullptr;

    for (const FileTypeSummary& summary : ordered) {
        totalSize += summary.totalSize;
        totalCount += summary.count;

        auto* item = new LegendTreeItem();
        item->setText(0, summary.label);
        item->setData(0, Qt::UserRole, summary.label);
        item->setData(0, kLegendSortValueRole, summary.label);
        item->setData(0, kLegendColorRole, summary.color);
        item->setText(1, locale.formattedDataSize(summary.totalSize));
        item->setData(1, kLegendSortValueRole, summary.totalSize);
        item->setText(2, locale.toString(summary.count));
        item->setData(2, kLegendSortValueRole, summary.count);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        if (summary.label == highlightedType) {
            selectedItem = item;
            foundHighlightedType = true;
        }
        items.append(item);
    }

    tree->setUpdatesEnabled(false);
    tree->addTopLevelItems(items);
    if (selectedItem) {
        selectedItem->setSelected(true);
        tree->setCurrentItem(selectedItem);
    }
    tree->setSortingEnabled(sortingEnabled);
    if (sortingEnabled) {
        tree->sortItems(sortColumn, sortOrder);
    }
    tree->setUpdatesEnabled(true);

    if (!foundHighlightedType && treemapWidget) {
        treemapWidget->setHighlightedFileType(QString());
        tree->clearSelection();
        tree->setCurrentItem(nullptr);
    }

    if (summaryLabel) {
        summaryLabel->setText(
            QCoreApplication::translate("MainWindow", "%1 types  |  %2 files  |  %3")
                .arg(ordered.size())
                .arg(QLocale::system().toString(totalCount))
                .arg(QLocale::system().formattedDataSize(totalSize)));
    }
}

bool sameViewState(const TreemapWidget::ViewState& a, const TreemapWidget::ViewState& b)
{
    return a.node == b.node
        && std::abs(a.cameraScale - b.cameraScale) < 0.0001
        && std::abs(a.cameraOrigin.x() - b.cameraOrigin.x()) < 0.01
        && std::abs(a.cameraOrigin.y() - b.cameraOrigin.y()) < 0.01
        && a.semanticDepth == b.semanticDepth
        && a.semanticFocus == b.semanticFocus
        && a.semanticLiveRoot == b.semanticLiveRoot
        && std::abs(a.currentRootLayoutAspectRatio - b.currentRootLayoutAspectRatio) < 0.0001;
}

bool isOverviewState(const TreemapWidget::ViewState& state)
{
    return std::abs(state.cameraScale - 1.0) < 0.0001
        && std::abs(state.cameraOrigin.x()) < 0.01
        && std::abs(state.cameraOrigin.y()) < 0.01
        && state.semanticFocus == nullptr
        && state.semanticLiveRoot == nullptr;
}

FileNode* findNodeByPath(FileNode* node, const QString& targetPath)
{
    if (!node || targetPath.isEmpty() || node->isVirtual) {
        return nullptr;
    }

    const QString normalizedTargetPath = normalizedFilesystemPath(targetPath);
    const QString nodePath = normalizedFilesystemPath(node->computePath());
    if (nodePath == normalizedTargetPath) {
        return node;
    }

    if (!node->isDirectory) {
        return nullptr;
    }

    if (!pathIsWithinRoot(normalizedTargetPath, nodePath)) {
        return nullptr;
    }

    for (FileNode* child : node->children) {
        if (FileNode* match = findNodeByPath(child, targetPath)) {
            return match;
        }
    }

    return nullptr;
}

FileNode* findVirtualFreeSpaceNode(FileNode* root)
{
    if (!root) {
        return nullptr;
    }

    for (FileNode* child : root->children) {
        if (child && child->isVirtual) {
            return child;
        }
    }

    return nullptr;
}

bool pathIsWithinRoot(const QString& path, const QString& rootPath)
{
    const QString normalizedPath = normalizedFilesystemPath(path);
    const QString normalizedRootPath = normalizedFilesystemPath(rootPath);
    if (normalizedPath.isEmpty() || normalizedRootPath.isEmpty()) {
        return false;
    }

    if (normalizedPath == normalizedRootPath) {
        return true;
    }

    QString rootPrefix = normalizedRootPath;
    if (!rootPrefix.endsWith(QLatin1Char('/'))) {
        rootPrefix += QLatin1Char('/');
    }
    return normalizedPath.startsWith(rootPrefix);
}

void collectWatchDirectoryPaths(FileNode* root, FileNode* current, QStringList& paths)
{
    if (!root || !root->isDirectory || root->isVirtual) {
        return;
    }

    appendUniquePath(paths, root->computePath());

    for (FileNode* node = current; node; node = node->parent) {
        if (node->isDirectory && !node->isVirtual) {
            appendUniquePath(paths, node->computePath());
        }
    }

    if (!current || !current->isDirectory || current->isVirtual) {
        return;
    }

    collectDescendantDirectoryPaths(current, 2, paths, 500);
}

QString nearestExistingNodePath(FileNode* root, QString path)
{
    while (!path.isEmpty()) {
        if (findNodeByPath(root, path)) {
            return path;
        }

        const QString parentPath = QFileInfo(path).absolutePath();
        if (parentPath.isEmpty() || parentPath == path) {
            break;
        }
        path = parentPath;
    }

    return root ? root->computePath() : QString();
}

ViewStatePaths captureViewStatePaths(const TreemapWidget::ViewState& state)
{
    ViewStatePaths captured;
    captured.nodePath = (state.node && !state.node->isVirtual) ? state.node->computePath() : QString();
    captured.cameraScale = state.cameraScale;
    captured.cameraOrigin = state.cameraOrigin;
    captured.semanticDepth = state.semanticDepth;
    captured.semanticFocusPath = (state.semanticFocus && !state.semanticFocus->isVirtual)
        ? state.semanticFocus->computePath()
        : QString();
    captured.semanticLiveRootPath = (state.semanticLiveRoot && !state.semanticLiveRoot->isVirtual)
        ? state.semanticLiveRoot->computePath()
        : QString();
    captured.currentRootLayoutAspectRatio = state.currentRootLayoutAspectRatio;
    return captured;
}

TreemapWidget::ViewState remapViewStatePaths(const ViewStatePaths& original, FileNode* root)
{
    TreemapWidget::ViewState remapped;
    if (!root) {
        return remapped;
    }

    remapped.cameraScale = original.cameraScale;
    remapped.cameraOrigin = original.cameraOrigin;
    remapped.semanticDepth = original.semanticDepth;
    remapped.currentRootLayoutAspectRatio = original.currentRootLayoutAspectRatio;
    remapped.node = findNodeByPath(root, nearestExistingNodePath(root, original.nodePath));
    if (!remapped.node) {
        remapped.node = root;
    }
    remapped.semanticFocus = findNodeByPath(root, original.semanticFocusPath);
    remapped.semanticLiveRoot = findNodeByPath(root, original.semanticLiveRootPath);
    return remapped;
}

QString nearestExistingDirectoryOnDisk(QString path)
{
    while (!path.isEmpty()) {
        const QFileInfo info(path);
        if (info.exists() && info.isDir()) {
            return info.absoluteFilePath();
        }

        const QString parentPath = info.absolutePath();
        if (parentPath.isEmpty() || parentPath == path) {
            break;
        }
        path = parentPath;
    }

    return {};
}

FileNode* topLevelRefreshNode(FileNode* scanRoot, FileNode* currentNode)
{
    if (!scanRoot || !currentNode || currentNode->isVirtual) {
        return scanRoot;
    }

    FileNode* refreshNode = currentNode;
    while (refreshNode->parent && refreshNode->parent != scanRoot) {
        refreshNode = refreshNode->parent;
    }

    return refreshNode;
}

QString searchPatternPlaceholderText()
{
    return QCoreApplication::translate("MainWindow", "Search filenames (*, ?)");
}

QString formatPinnedDataSize(qint64 bytes)
{
    QString text = QLocale::system().formattedDataSize(std::max<qint64>(0, bytes));
    static constexpr int kSeenFieldWidth = 10;
    if (text.size() < kSeenFieldWidth) {
        text = text.rightJustified(kSeenFieldWidth, QLatin1Char(' '));
    }
    return text;
}

QStringList mountedDevicePaths()
{
    QStringList mountedPaths;
    const QString normalizedRootPath = normalizedFilesystemPath(QDir::rootPath());
    QStringList allowedPrefixes;

#ifdef Q_OS_LINUX
    allowedPrefixes = {
        QStringLiteral("/media"),
        QStringLiteral("/run/media"),
        QStringLiteral("/mnt"),
    };
#elif defined(Q_OS_MACOS)
    allowedPrefixes = {
        QStringLiteral("/Volumes"),
    };
#else
    return mountedPaths;
#endif

    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo& volume : volumes) {
        if (!volume.isValid() || !volume.isReady() || volume.bytesTotal() <= 0) {
            continue;
        }

        const QString normalizedPath = normalizedFilesystemPath(volume.rootPath());
        if (normalizedPath.isEmpty() || normalizedPath == normalizedRootPath) {
            continue;
        }
        const bool allowed = std::any_of(allowedPrefixes.begin(), allowedPrefixes.end(),
                                         [&normalizedPath](const QString& prefix) {
                                             return normalizedPath == prefix
                                                 || normalizedPath.startsWith(prefix + QLatin1Char('/'));
                                         });
        if (!allowed) {
            continue;
        }

        appendUniquePath(mountedPaths, normalizedPath);
    }

    return mountedPaths;
}

void sortChildrenBySizeRecursive(FileNode* node)
{
    if (!node) {
        return;
    }

    std::sort(node->children.begin(), node->children.end(),
              [](const FileNode* a, const FileNode* b) {
                  return a->size > b->size;
              });
    for (FileNode* child : node->children) {
        sortChildrenBySizeRecursive(child);
    }
}

void applyFreeSpaceNodeColor(FileNode* root, const TreemapSettings& settings)
{
    if (!root)
        return;
    for (FileNode* child : root->children) {
        if (child && child->isVirtual)
            child->color = settings.freeSpaceColor.rgba();
    }
}

int countFilesRecursive(const FileNode* node)
{
    if (!node || node->isVirtual) {
        return 0;
    }

    if (!node->isDirectory) {
        return 1;
    }

    int count = 0;
    for (const FileNode* child : node->children) {
        count += countFilesRecursive(child);
    }
    return count;
}

int nodeDepth(const FileNode* node)
{
    int depth = 0;
    for (const FileNode* current = node; current && current->parent; current = current->parent) {
        ++depth;
    }
    return depth;
}

std::vector<FileNode*> prepareRootResultForDisplay(ScanResult& scanResult, const QString& currentPath,
                                                   bool showFreeSpaceInOverview,
                                                   const TreemapSettings& settings,
                                                   bool assumeSorted)
{
    if (!scanResult.root) {
        return {};
    }

    injectFreeSpaceNodeIfNeeded(scanResult, currentPath, settings);
    if (!assumeSorted) {
        sortChildrenBySizeRecursive(scanResult.root);
    }
    applyFreeSpaceNodeColor(scanResult.root, settings);

    return collectAndMaybeStripVirtualNodes(scanResult.root->children, showFreeSpaceInOverview);
}

std::vector<FileNode*> reinjectFreeSpaceNodes(ScanResult& scanResult, const QString& currentPath,
                                              bool showFreeSpaceInOverview,
                                              const TreemapSettings& settings)
{
    if (!scanResult.root || scanResult.filesystems.isEmpty())
        return {};

    auto& children = scanResult.root->children;

    // Strip any existing virtual nodes (in children or previously stashed — we don't care which)
    children.erase(std::remove_if(children.begin(), children.end(),
                                  [](FileNode* c) { return c && c->isVirtual; }),
                   children.end());

    // Re-inject under new settings, then re-sort root's direct children only
    // (the rest of the tree is already sorted from the original scan)
    injectFreeSpaceNodeIfNeeded(scanResult, currentPath, settings);
    std::sort(children.begin(), children.end(),
              [](const FileNode* a, const FileNode* b) { return a->size > b->size; });
    applyFreeSpaceNodeColor(scanResult.root, settings);

    return collectAndMaybeStripVirtualNodes(children, showFreeSpaceInOverview);
}

std::vector<TreemapWidget::ViewState> remapHistoryPaths(const std::vector<ViewStatePaths>& historyPaths,
                                                        FileNode* root)
{
    std::vector<TreemapWidget::ViewState> remappedHistory;
    remappedHistory.reserve(historyPaths.size());
    for (const ViewStatePaths& state : historyPaths) {
        TreemapWidget::ViewState remapped = remapViewStatePaths(state, root);
        if (!remapped.node) {
            continue;
        }
        if (remappedHistory.empty() || !sameViewState(remappedHistory.back(), remapped)) {
            remappedHistory.push_back(remapped);
        }
    }
    return remappedHistory;
}

bool sameSubtree(const FileNode* a, const FileNode* b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    if (a->name != b->name
            || a->isDirectory != b->isDirectory
            || a->isVirtual != b->isVirtual
            || a->size != b->size
            || a->children.size() != b->children.size()) {
        return false;
    }

    for (size_t i = 0; i < a->children.size(); ++i) {
        if (!sameSubtree(a->children[i], b->children[i])) {
            return false;
        }
    }

    return true;
}

TreemapWidget::ViewState remapViewStateByPath(const TreemapWidget::ViewState& original, FileNode* root)
{
    return remapViewStatePaths(captureViewStatePaths(original), root);
}

void sanitizeHistoryForRoot(std::vector<TreemapWidget::ViewState>& history, FileNode* root)
{
    std::vector<TreemapWidget::ViewState> sanitized;
    sanitized.reserve(history.size());

    for (const TreemapWidget::ViewState& state : history) {
        TreemapWidget::ViewState remapped = remapViewStateByPath(state, root);
        if (!remapped.node) {
            continue;
        }
        if (sanitized.empty() || !sameViewState(sanitized.back(), remapped)) {
            sanitized.push_back(remapped);
        }
    }

    history = std::move(sanitized);
}

qint64 pruneDeletedChildren(FileNode* node)
{
    if (!node || !node->isDirectory) {
        return 0;
    }

    qint64 removedBytes = 0;
    auto& children = node->children;
    auto newEnd = std::remove_if(children.begin(), children.end(), [&](FileNode* child) {
        if (!child || child->isVirtual) {
            return false;
        }

        const QString childPath = child->computePath();
        if (!QFileInfo::exists(childPath)) {
            removedBytes += child->size;
            return true;
        }

        if (child->isDirectory) {
            const qint64 nestedRemovedBytes = pruneDeletedChildren(child);
            if (nestedRemovedBytes > 0) {
                child->size = std::max<qint64>(0, child->size - nestedRemovedBytes);
                removedBytes += nestedRemovedBytes;
            }
        }

        return false;
    });
    children.erase(newEnd, children.end());
    return removedBytes;
}

qint64 directorySizeOnDisk(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists()) {
        return 0;
    }
    if (info.isFile() || info.isSymLink()) {
        return std::max<qint64>(0, info.size());
    }
    if (!info.isDir()) {
        return 0;
    }

    qint64 totalSize = 0;
    QDirIterator it(path,
                    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo entry = it.fileInfo();
        if (entry.isSymLink()) {
            continue;
        }
        if (entry.isFile()) {
            totalSize += std::max<qint64>(0, entry.size());
        }
    }

    return totalSize;
}

bool spliceRefreshedSubtree(ScanResult& main, const QString& targetPath, ScanResult refreshed)
{
    FileNode* targetNode = findNodeByPath(main.root, targetPath);
    if (!targetNode || !targetNode->parent)
        return false;

    const qint64 oldSize = targetNode->size;
    FileNode* parent = targetNode->parent;
    FileNode* newRoot = refreshed.root;
    newRoot->parent = parent;

    for (FileNode*& child : parent->children) {
        if (child == targetNode) {
            child = newRoot;
            break;
        }
    }

    const qint64 sizeDelta = newRoot->size - oldSize;
    for (FileNode* current = parent; current; current = current->parent)
        current->size += sizeDelta;

    main.arena->merge(std::move(*refreshed.arena));
    return true;
}

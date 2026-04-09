// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "treemapsettings.h"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QPalette>
#include <QSettings>
#include <QUuid>
#include <cmath>

namespace {

qreal defaultHeaderHeightForFont(const QString& family, int pointSize, bool bold, bool italic)
{
    QFont headerFont = QApplication::instance()
        ? QFontDatabase::systemFont(QFontDatabase::GeneralFont)
        : QFont();
    if (!family.isEmpty()) {
        headerFont.setFamily(family);
    }
    headerFont.setPointSize(qMax(1, pointSize));
    headerFont.setBold(bold);
    headerFont.setItalic(italic);
    return qMax<qreal>(1.0, std::ceil(QFontMetrics(headerFont).height() + 4.0));
}

QFont defaultTreemapFont()
{
    return QApplication::instance()
        ? QFontDatabase::systemFont(QFontDatabase::GeneralFont)
        : QFont();
}

}

QColor defaultFreeSpaceColor()
{
    if (QApplication::instance()) {
        const QColor windowColor = QApplication::palette().color(QPalette::Window);
        if (windowColor.isValid()) {
            return windowColor;
        }
    }
    return QColor(80, 80, 80);
}

// ── TreemapColorTheme ────────────────────────────────────────────────────────

TreemapColorTheme TreemapColorTheme::defaultLightTheme()
{
    TreemapColorTheme theme;
    theme.id = builtInLightId();
    theme.name = QStringLiteral("Light");
    theme.folderColorMode = 1;
    theme.folderBaseColor = QColor::fromHsl(210, 200, 150);
    theme.folderColorSaturation = 0.85;
    theme.folderColorBrightness = 0.97;
    theme.folderDepthBrightnessMode = 0;
    theme.folderColorDarkenPerLevel = 0.03;
    theme.fileColorSaturation = 0.75;
    theme.fileColorBrightness = 0.70;
    theme.borderStyle = TreemapSettings::AutomaticBorder;
    theme.borderIntensity = 0.30;
    if (QApplication::instance()) {
        const QPalette palette = QApplication::palette();
        const QColor highlight = palette.color(QPalette::Highlight);
        theme.highlightColor = highlight.isValid() ? highlight : QColor(0, 120, 215);
    } else {
        theme.highlightColor = QColor(0, 120, 215);
    }
    theme.freeSpaceColor = QColor(QStringLiteral("#A6A6A7"));
    theme.unknownFileTypeColor = QColor(QStringLiteral("#E8E8E8"));
    theme.highlightOpacity = 0.30;
    theme.unknownFileTypeOpacity = 0.5;
    return theme;
}

TreemapColorTheme TreemapColorTheme::defaultDarkTheme()
{
    TreemapColorTheme theme;
    theme.id = builtInDarkId();
    theme.name = QStringLiteral("Dark");
    theme.folderColorMode = 1;
    theme.folderBaseColor = QColor::fromHsl(210, 140, 60);
    theme.folderColorSaturation = 0.70;
    theme.folderColorBrightness = 0.15;
    theme.folderDepthBrightnessMode = 1;
    theme.folderColorDarkenPerLevel = 0.02;
    theme.fileColorSaturation = 0.60;
    theme.fileColorBrightness = 0.30;
    theme.borderStyle = TreemapSettings::AutomaticBorder;
    theme.borderIntensity = 0.15;
    if (QApplication::instance()) {
        const QPalette palette = QApplication::palette();
        const QColor highlight = palette.color(QPalette::Highlight);
        theme.highlightColor = highlight.isValid() ? highlight : QColor(0, 120, 215);
    } else {
        theme.highlightColor = QColor(0, 120, 215);
    }
    theme.freeSpaceColor = QColor(QStringLiteral("#3E3E3E"));
    theme.unknownFileTypeColor = QColor(QStringLiteral("#1A1A1A"));
    theme.highlightOpacity = 0.30;
    theme.unknownFileTypeOpacity = 0.5;
    return theme;
}

QList<TreemapColorTheme> TreemapColorTheme::defaults()
{
    return {defaultLightTheme(), defaultDarkTheme()};
}

QString TreemapColorTheme::createCustomId()
{
    return QStringLiteral("theme-custom-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void TreemapColorTheme::sanitize()
{
    id = id.trimmed();
    name = name.trimmed();
    if (!folderBaseColor.isValid()) {
        folderBaseColor = QColor::fromHsl(210, 200, 150);
    }
    folderColorSaturation = qBound(0.0, folderColorSaturation, 1.0);
    folderColorBrightness = qBound(0.0, folderColorBrightness, 1.0);
    folderColorDarkenPerLevel = qBound(0.0, folderColorDarkenPerLevel, 0.30);
    depthGradientPreset = qMax(0, depthGradientPreset);
    fileColorSaturation = qBound(0.0, fileColorSaturation, 1.0);
    fileColorBrightness = qBound(0.0, fileColorBrightness, 1.0);
    borderStyle = (borderStyle == TreemapSettings::DarkenBorder
                   || borderStyle == TreemapSettings::LightenBorder)
        ? borderStyle
        : TreemapSettings::AutomaticBorder;
    borderIntensity = qBound(0.0, borderIntensity, 1.0);
    if (!freeSpaceColor.isValid()) {
        freeSpaceColor = defaultFreeSpaceColor();
    }
    if (!unknownFileTypeColor.isValid()) {
        unknownFileTypeColor = QColor(QStringLiteral("#E8E8E8"));
    }
    highlightOpacity = qBound(0.0, highlightOpacity, 1.0);
    unknownFileTypeOpacity = qBound(0.0, unknownFileTypeOpacity, 1.0);
}

// ── TreemapSettings ──────────────────────────────────────────────────────────

QColor TreemapSettings::defaultHighlightColor()
{
    if (QApplication::instance()) {
        const QPalette palette = QApplication::palette();
        const QColor highlightColor = palette.color(QPalette::Highlight);
        if (highlightColor.isValid()) {
            return highlightColor;
        }
        const QColor accentColor = palette.color(QPalette::Accent);
        if (accentColor.isValid()) {
            return accentColor;
        }
    }
    return QColor(0, 120, 215);
}

void TreemapSettings::applyDefaults(TreemapSettings& settings)
{
    settings.headerHeight = defaultHeaderHeightForFont(QString(), 9, false, false);
    settings.headerFontFamily.clear();
    settings.headerFontSize = 9;
    settings.headerFontBold = false;
    settings.headerFontItalic = false;
    if (QApplication::instance()) {
        QFont uiFont = defaultTreemapFont();
        settings.headerFontFamily = uiFont.family();
        settings.headerFontBold = uiFont.bold();
        settings.headerFontItalic = uiFont.italic();
        settings.headerHeight = defaultHeaderHeightForFont(
            settings.headerFontFamily,
            settings.headerFontSize,
            settings.headerFontBold,
            settings.headerFontItalic);
    }

    settings.fileFontFamily.clear();
    settings.fileFontSize = 8;
    settings.fileFontBold = false;
    settings.fileFontItalic = false;
    if (QApplication::instance()) {
        QFont uiFont = defaultTreemapFont();
        settings.fileFontFamily = uiFont.family();
        settings.fileFontBold = uiFont.bold();
        settings.fileFontItalic = uiFont.italic();
    }
    settings.folderColorMode = 1;
    settings.folderBaseColor = QColor::fromHsl(210, 200, 150);
    settings.folderColorSaturation = 0.85;
    settings.folderColorBrightness = 0.97;
    settings.folderColorDarkenPerLevel = 0.03;
    settings.fileColorSaturation = 0.75;
    settings.fileColorBrightness = 0.70;
    settings.border = 1.0;
    settings.borderStyle = AutomaticBorder;
    settings.borderIntensity = 0.30;
    settings.highlightColor = defaultHighlightColor();
    settings.freeSpaceColor = defaultFreeSpaceColor();
    settings.unknownFileTypeColor = QColor(QStringLiteral("#E8E8E8"));
    settings.randomColorForUnknownFiles = false;
    settings.highlightOpacity = 0.75;
    settings.unknownFileTypeOpacity = 0.5;
    settings.folderPadding = 2.0;
    settings.depthRevealPerZoomDoubling = 3.0;
    settings.minTileSize = 4.0;
    settings.minPaint = 2.0;
    settings.minRevealWidth = 24.0;
    settings.minRevealHeight = 20.0;
    settings.revealFadeWidth = 14.0;
    settings.revealFadeHeight = 10.0;
    settings.fileTypeGroups = {
        { QStringLiteral("Code"),
          QColor::fromHslF(0.58f, 1.0f, 0.50f),
          { QStringLiteral("cpp"), QStringLiteral("c"), QStringLiteral("cc"),
            QStringLiteral("cxx"), QStringLiteral("h"), QStringLiteral("hpp"),
            QStringLiteral("hh"), QStringLiteral("hxx"), QStringLiteral("inl"),
            QStringLiteral("rs"), QStringLiteral("go"), QStringLiteral("py"),
            QStringLiteral("pyi"), QStringLiteral("rb"), QStringLiteral("php"),
            QStringLiteral("js"), QStringLiteral("mjs"), QStringLiteral("cjs"),
            QStringLiteral("ts"), QStringLiteral("tsx"), QStringLiteral("jsx"),
            QStringLiteral("java"), QStringLiteral("kt"), QStringLiteral("kts"),
            QStringLiteral("swift"), QStringLiteral("cs"), QStringLiteral("fs"),
            QStringLiteral("scala"), QStringLiteral("groovy"), QStringLiteral("clj"),
            QStringLiteral("lua"), QStringLiteral("zig"), QStringLiteral("nim"),
            QStringLiteral("d"), QStringLiteral("vb"), QStringLiteral("m") } },
        { QStringLiteral("Images"),
          QColor::fromHslF(0.80f, 1.0f, 0.50f),
          { QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
            QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("svg"),
            QStringLiteral("svgz"), QStringLiteral("psd"), QStringLiteral("psb"),
            QStringLiteral("kra"), QStringLiteral("xcf"), QStringLiteral("bmp"),
            QStringLiteral("tiff"), QStringLiteral("tif"), QStringLiteral("ico"),
            QStringLiteral("icns"), QStringLiteral("heic"), QStringLiteral("heif"),
            QStringLiteral("avif"), QStringLiteral("jxl"), QStringLiteral("raw"),
            QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("nef"),
            QStringLiteral("arw"), QStringLiteral("dng"), QStringLiteral("exr"),
            QStringLiteral("hdr") } },
        { QStringLiteral("Audio"),
          QColor::fromHslF(0.34f, 1.0f, 0.50f),
          { QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("wav"),
            QStringLiteral("ogg"), QStringLiteral("oga"), QStringLiteral("opus"),
            QStringLiteral("m4a"), QStringLiteral("aac"), QStringLiteral("wma"),
            QStringLiteral("aiff"), QStringLiteral("aif"), QStringLiteral("ape"),
            QStringLiteral("wv"), QStringLiteral("mka"), QStringLiteral("mid"),
            QStringLiteral("midi"), QStringLiteral("amr") } },
        { QStringLiteral("Video"),
          QColor::fromHslF(0.08f, 1.0f, 0.50f),
          { QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"),
            QStringLiteral("avi"), QStringLiteral("webm"), QStringLiteral("flv"),
            QStringLiteral("wmv"), QStringLiteral("m4v"), QStringLiteral("mpg"),
            QStringLiteral("mpeg"), QStringLiteral("ts"), QStringLiteral("mts"),
            QStringLiteral("m2ts"), QStringLiteral("vob"), QStringLiteral("ogv"),
            QStringLiteral("3gp"), QStringLiteral("rmvb") } },
        { QStringLiteral("Documents"),
          QColor::fromHslF(0.03f, 1.0f, 0.50f),
          { QStringLiteral("pdf"), QStringLiteral("doc"), QStringLiteral("docx"),
            QStringLiteral("xls"), QStringLiteral("xlsx"), QStringLiteral("ppt"),
            QStringLiteral("pptx"), QStringLiteral("odt"), QStringLiteral("ods"),
            QStringLiteral("odp"), QStringLiteral("rtf"), QStringLiteral("txt"),
            QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("rst"),
            QStringLiteral("tex"), QStringLiteral("epub"), QStringLiteral("mobi"),
            QStringLiteral("djvu"), QStringLiteral("pages"), QStringLiteral("numbers"),
            QStringLiteral("key") } },
        { QStringLiteral("Archives"),
          QColor::fromHslF(0.12f, 1.0f, 0.50f),
          { QStringLiteral("zip"), QStringLiteral("tar"), QStringLiteral("gz"),
            QStringLiteral("tgz"), QStringLiteral("xz"), QStringLiteral("txz"),
            QStringLiteral("bz2"), QStringLiteral("tbz2"), QStringLiteral("7z"),
            QStringLiteral("rar"), QStringLiteral("zst"), QStringLiteral("lz4"),
            QStringLiteral("lzma"), QStringLiteral("cab"), QStringLiteral("iso"),
            QStringLiteral("img"), QStringLiteral("dmg"), QStringLiteral("deb"),
            QStringLiteral("rpm"), QStringLiteral("apk"), QStringLiteral("jar"),
            QStringLiteral("war"), QStringLiteral("ear"), QStringLiteral("snap") } },
        { QStringLiteral("Data"),
          QColor::fromHslF(0.50f, 1.0f, 0.50f),
          { QStringLiteral("json"), QStringLiteral("jsonl"), QStringLiteral("yaml"),
            QStringLiteral("yml"), QStringLiteral("toml"), QStringLiteral("xml"),
            QStringLiteral("csv"), QStringLiteral("tsv"), QStringLiteral("sql"),
            QStringLiteral("sqlite"), QStringLiteral("db"), QStringLiteral("parquet"),
            QStringLiteral("avro"), QStringLiteral("proto"), QStringLiteral("cbor"),
            QStringLiteral("msgpack") } },
        { QStringLiteral("Fonts"),
          QColor::fromHslF(0.72f, 1.0f, 0.50f),
          { QStringLiteral("ttf"), QStringLiteral("otf"), QStringLiteral("woff"),
            QStringLiteral("woff2"), QStringLiteral("eot"), QStringLiteral("pfb"),
            QStringLiteral("pfm") } },
        { QStringLiteral("3D / CAD"),
          QColor::fromHslF(0.44f, 1.0f, 0.50f),
          { QStringLiteral("obj"), QStringLiteral("fbx"), QStringLiteral("gltf"),
            QStringLiteral("glb"), QStringLiteral("dae"), QStringLiteral("stl"),
            QStringLiteral("ply"), QStringLiteral("blend"), QStringLiteral("3ds"),
            QStringLiteral("step"), QStringLiteral("stp"), QStringLiteral("iges"),
            QStringLiteral("igs"), QStringLiteral("usd"), QStringLiteral("usda"),
            QStringLiteral("usdc") } },
        { QStringLiteral("Executables"),
          QColor::fromHslF(0.97f, 1.0f, 0.50f),
          { QStringLiteral("exe"), QStringLiteral("dll"), QStringLiteral("so"),
            QStringLiteral("dylib"), QStringLiteral("a"), QStringLiteral("lib"),
            QStringLiteral("o"), QStringLiteral("ko"), QStringLiteral("out"),
            QStringLiteral("elf"), QStringLiteral("wasm"), QStringLiteral("sys") } },
        { QStringLiteral("Web"),
          QColor::fromHslF(0.25f, 1.0f, 0.50f),
          { QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("xhtml"),
            QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("sass"),
            QStringLiteral("less"), QStringLiteral("vue"), QStringLiteral("svelte") } },
        { QStringLiteral("Disk Images"),
          QColor::fromHslF(0.62f, 1.0f, 0.50f),
          { QStringLiteral("vmdk"), QStringLiteral("vdi"), QStringLiteral("vhd"),
            QStringLiteral("vhdx"), QStringLiteral("qcow2"), QStringLiteral("qcow") } },
    };
    settings.colorThemes = TreemapColorTheme::defaults();
    settings.activeColorThemeId = TreemapColorTheme::builtInLightId();
    settings.lightModeColorThemeId = TreemapColorTheme::builtInLightId();
    settings.darkModeColorThemeId = TreemapColorTheme::builtInDarkId();
    settings.followSystemColorTheme = true;
    settings.limitToSameFilesystem = false;
}

TreemapSettings::TreemapSettings()
{
    applyDefaults(*this);
}

TreemapColorTheme* TreemapSettings::findColorTheme(const QString& themeId)
{
    for (TreemapColorTheme& theme : colorThemes) {
        if (theme.id == themeId) {
            return &theme;
        }
    }
    return nullptr;
}

const TreemapColorTheme* TreemapSettings::findColorTheme(const QString& themeId) const
{
    for (const TreemapColorTheme& theme : colorThemes) {
        if (theme.id == themeId) {
            return &theme;
        }
    }
    return nullptr;
}

void TreemapSettings::applyColorTheme(const TreemapColorTheme& theme)
{
    folderColorMode = theme.folderColorMode;
    folderBaseColor = theme.folderBaseColor;
    folderColorSaturation = theme.folderColorSaturation;
    folderColorBrightness = theme.folderColorBrightness;
    folderDepthBrightnessMode = theme.folderDepthBrightnessMode;
    folderColorDarkenPerLevel = theme.folderColorDarkenPerLevel;
    depthGradientPreset = theme.depthGradientPreset;
    depthGradientFlipped = theme.depthGradientFlipped;
    fileColorSaturation = theme.fileColorSaturation;
    fileColorBrightness = theme.fileColorBrightness;
    borderStyle = theme.borderStyle;
    borderIntensity = theme.borderIntensity;
    highlightColor = theme.highlightColor;
    freeSpaceColor = theme.freeSpaceColor;
    unknownFileTypeColor = theme.unknownFileTypeColor;
    highlightOpacity = theme.highlightOpacity;
    unknownFileTypeOpacity = theme.unknownFileTypeOpacity;
}

TreemapColorTheme TreemapSettings::activeColorTheme() const
{
    if (const TreemapColorTheme* theme = findColorTheme(activeColorThemeId)) {
        return *theme;
    }
    return colorThemes.isEmpty() ? TreemapColorTheme::defaultLightTheme() : colorThemes.constFirst();
}

QString TreemapSettings::colorThemeIdForSystemScheme(bool darkMode) const
{
    const QString requested = darkMode ? darkModeColorThemeId : lightModeColorThemeId;
    if (findColorTheme(requested)) {
        return requested;
    }
    return darkMode ? TreemapColorTheme::builtInDarkId() : TreemapColorTheme::builtInLightId();
}

void TreemapSettings::applyActiveColorTheme()
{
    if (colorThemes.isEmpty()) {
        colorThemes = TreemapColorTheme::defaults();
    }
    if (const TreemapColorTheme* theme = findColorTheme(activeColorThemeId)) {
        applyColorTheme(*theme);
    } else {
        activeColorThemeId = colorThemes.constFirst().id;
        applyColorTheme(colorThemes.constFirst());
    }
}

void TreemapSettings::ensureColorThemes()
{
    QList<TreemapColorTheme> normalized = colorThemes;
    if (normalized.isEmpty()) {
        normalized = TreemapColorTheme::defaults();
    }

    bool hasLight = false;
    bool hasDark = false;
    QStringList seenIds;
    for (TreemapColorTheme& theme : normalized) {
        theme.sanitize();
        if (theme.id.isEmpty()) {
            theme.id = TreemapColorTheme::createCustomId();
        }
        while (seenIds.contains(theme.id)) {
            theme.id = TreemapColorTheme::createCustomId();
        }
        if (theme.name.isEmpty()) {
            theme.name = QStringLiteral("Custom theme");
        }
        seenIds.push_back(theme.id);
        hasLight = hasLight || theme.id == TreemapColorTheme::builtInLightId();
        hasDark = hasDark || theme.id == TreemapColorTheme::builtInDarkId();
    }

    if (!hasLight) {
        normalized.prepend(TreemapColorTheme::defaultLightTheme());
    }
    if (!hasDark) {
        normalized.append(TreemapColorTheme::defaultDarkTheme());
    }

    colorThemes = normalized;
    if (!findColorTheme(activeColorThemeId)) {
        activeColorThemeId = TreemapColorTheme::builtInLightId();
    }
    if (!findColorTheme(lightModeColorThemeId)) {
        lightModeColorThemeId = TreemapColorTheme::builtInLightId();
    }
    if (!findColorTheme(darkModeColorThemeId)) {
        darkModeColorThemeId = TreemapColorTheme::builtInDarkId();
    }
}

void TreemapSettings::sanitize()
{
    headerFontFamily = headerFontFamily.trimmed();
    headerFontSize = qMax(1, headerFontSize);
    headerHeight = qMax(headerHeight, defaultHeaderHeightForFont(
        headerFontFamily,
        headerFontSize,
        headerFontBold,
        headerFontItalic));
    fileFontFamily = fileFontFamily.trimmed();
    fileFontSize = qMax(1, fileFontSize);
    if (folderColorMode != SingleHue && folderColorMode != DepthGradient) {
        folderColorMode = DistinctTopLevel;
    }
    depthGradientPreset = qMax(0, depthGradientPreset);
    if (!folderBaseColor.isValid()) {
        folderBaseColor = QColor::fromHsl(210, 200, 150);
    }
    folderColorSaturation = qBound(0.0, folderColorSaturation, 1.0);
    folderColorBrightness = qBound(0.0, folderColorBrightness, 1.0);
    folderDepthBrightnessMode = (folderDepthBrightnessMode == LightenPerLevel)
        ? LightenPerLevel
        : DarkenPerLevel;
    folderColorDarkenPerLevel = qBound(0.0, folderColorDarkenPerLevel, 0.30);
    fileColorSaturation = qBound(0.0, fileColorSaturation, 1.0);
    fileColorBrightness = qBound(0.0, fileColorBrightness, 1.0);
    border = qMax(0.0, border);
    borderStyle = (borderStyle == DarkenBorder || borderStyle == LightenBorder)
        ? borderStyle
        : AutomaticBorder;
    borderIntensity = qBound(0.0, borderIntensity, 1.0);
    if (!highlightColor.isValid()) {
        highlightColor = defaultHighlightColor();
    }
    if (!freeSpaceColor.isValid()) {
        freeSpaceColor = defaultFreeSpaceColor();
    }
    if (!unknownFileTypeColor.isValid()) {
        unknownFileTypeColor = QColor(QStringLiteral("#E8E8E8"));
    }
    highlightOpacity = qBound(0.0, highlightOpacity, 1.0);
    unknownFileTypeOpacity = qBound(0.0, unknownFileTypeOpacity, 1.0);
    folderPadding = qMax(0.0, folderPadding);
    baseVisibleDepth = qMax(0, baseVisibleDepth);
    depthRevealPerZoomDoubling = qBound(0.0, depthRevealPerZoomDoubling, 8.0);
    minTileSize = qMax(1.0, minTileSize);
    minPaint = qMax(0.5, minPaint);
    minRevealWidth = qMax(1.0, minRevealWidth);
    minRevealHeight = qMax(1.0, minRevealHeight);
    revealFadeWidth = qMax(0.0, revealFadeWidth);
    revealFadeHeight = qMax(0.0, revealFadeHeight);
    zoomDurationMs = qMax(0, zoomDurationMs);
    layoutDurationMs = qMax(0, layoutDurationMs);
    wheelZoomStepPercent = qBound(0.1, wheelZoomStepPercent, 200.0);
    wheelZoomMinScale = qMax(0.01, wheelZoomMinScale);
    wheelZoomMaxScale = qMax(wheelZoomMinScale, wheelZoomMaxScale);
    cameraDurationMs = qMax(0, cameraDurationMs);
    cameraMaxScale = qBound(1.0, cameraMaxScale, 512.0);
    parallelPartitionDepth = qBound(1, parallelPartitionDepth, 8);
    maxSemanticDepth = qMax(baseVisibleDepth, maxSemanticDepth);
    edgeFocusInsetRatio = qBound(0.0, edgeFocusInsetRatio, 0.49);
    tileAspectBias = qBound(-1.0, tileAspectBias, 1.0);
    ensureColorThemes();
    applyActiveColorTheme();

    QStringList normalizedPaths;
    normalizedPaths.reserve(excludedPaths.size());
    for (const QString& path : excludedPaths) {
        const QString trimmed = path.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        const QString normalizedPath = QDir::cleanPath(trimmed);
        if (!normalizedPaths.contains(normalizedPath)) {
            normalizedPaths.push_back(normalizedPath);
        }
    }
    excludedPaths = normalizedPaths;

    for (auto& g : fileTypeGroups) {
        g.name = g.name.trimmed();
        for (auto& ext : g.extensions) {
            ext = ext.trimmed().toLower();
        }
        g.extensions.removeAll(QString{});
    }
}

TreemapSettings TreemapSettings::defaults()
{
    TreemapSettings settings;
    settings.sanitize();
    return settings;
}

TreemapSettings TreemapSettings::load(QSettings& store)
{
    TreemapSettings settings = defaults();
    settings.headerHeight = store.value("treemap/headerHeight", settings.headerHeight).toDouble();
    settings.headerFontFamily = store.value("treemap/headerFontFamily", settings.headerFontFamily).toString();
    settings.headerFontSize = store.value("treemap/headerFontSize", settings.headerFontSize).toInt();
    settings.headerFontBold = store.value("treemap/headerFontBold", settings.headerFontBold).toBool();
    settings.headerFontItalic = store.value("treemap/headerFontItalic", settings.headerFontItalic).toBool();
    settings.fileFontFamily = store.value("treemap/fileFontFamily", settings.fileFontFamily).toString();
    settings.fileFontSize = store.value("treemap/fileFontSize", settings.fileFontSize).toInt();
    settings.fileFontBold = store.value("treemap/fileFontBold", settings.fileFontBold).toBool();
    settings.fileFontItalic = store.value("treemap/fileFontItalic", settings.fileFontItalic).toBool();
    settings.folderColorMode = store.value("treemap/folderColorMode", settings.folderColorMode).toInt();
    settings.folderColorSaturation = store.value("treemap/folderColorSaturation", settings.folderColorSaturation).toDouble();
    settings.folderColorBrightness = store.value("treemap/folderColorBrightness", settings.folderColorBrightness).toDouble();
    settings.folderDepthBrightnessMode = store.value(
        "treemap/folderDepthBrightnessMode",
        settings.folderDepthBrightnessMode).toInt();
    settings.folderColorDarkenPerLevel = store.value("treemap/folderColorDarkenPerLevel", settings.folderColorDarkenPerLevel).toDouble();
    settings.fileColorSaturation = store.value("treemap/fileColorSaturation", settings.fileColorSaturation).toDouble();
    settings.fileColorBrightness = store.value("treemap/fileColorBrightness", settings.fileColorBrightness).toDouble();
    settings.border = store.value("treemap/border", settings.border).toDouble();
    settings.borderStyle = store.value("treemap/borderStyle", settings.borderStyle).toInt();
    settings.borderIntensity = store.value("treemap/borderIntensity", settings.borderIntensity).toDouble();
    settings.highlightColor = store.value("treemap/highlightColor", settings.highlightColor).value<QColor>();
    settings.freeSpaceColor = store.value("treemap/freeSpaceColor", settings.freeSpaceColor).value<QColor>();
    settings.randomColorForUnknownFiles = store.value("treemap/randomColorForUnknownFiles", settings.randomColorForUnknownFiles).toBool();
    settings.highlightOpacity = store.value("treemap/highlightOpacity", settings.highlightOpacity).toDouble();
    settings.unknownFileTypeOpacity = store.value("treemap/unknownFileTypeOpacity",
                                                  settings.unknownFileTypeOpacity).toDouble();
    settings.folderPadding = store.value("treemap/folderPadding", settings.folderPadding).toDouble();
    settings.baseVisibleDepth = store.value("treemap/baseVisibleDepth", settings.baseVisibleDepth).toInt();
    settings.depthRevealPerZoomDoubling = store.value(
        "treemap/depthRevealPerZoomDoubling",
        settings.depthRevealPerZoomDoubling).toDouble();
    settings.minTileSize = store.value("treemap/minTileSize", settings.minTileSize).toDouble();
    settings.minPaint = store.value("treemap/minPaint", settings.minPaint).toDouble();
    settings.minRevealWidth = store.contains("treemap/minRevealWidth")
        ? store.value("treemap/minRevealWidth", settings.minRevealWidth).toDouble()
        : store.value("treemap/minLiveRevealWidth", settings.minRevealWidth).toDouble();
    settings.minRevealHeight = store.contains("treemap/minRevealHeight")
        ? store.value("treemap/minRevealHeight", settings.minRevealHeight).toDouble()
        : store.value("treemap/minLiveRevealHeight", settings.minRevealHeight).toDouble();
    settings.revealFadeWidth = store.contains("treemap/revealFadeWidth")
        ? store.value("treemap/revealFadeWidth", settings.revealFadeWidth).toDouble()
        : std::max<qreal>(0.0,
                          settings.minRevealWidth
                              - store.value("treemap/minContentWidth",
                                            settings.minRevealWidth - settings.revealFadeWidth).toDouble());
    settings.revealFadeHeight = store.contains("treemap/revealFadeHeight")
        ? store.value("treemap/revealFadeHeight", settings.revealFadeHeight).toDouble()
        : std::max<qreal>(0.0,
                          settings.minRevealHeight
                              - store.value("treemap/minContentHeight",
                                            settings.minRevealHeight - settings.revealFadeHeight).toDouble());
    settings.zoomDurationMs = store.value("treemap/zoomDurationMs", settings.zoomDurationMs).toInt();
    settings.layoutDurationMs = store.value("treemap/layoutDurationMs", settings.layoutDurationMs).toInt();
    settings.wheelZoomStepPercent = store.value("treemap/wheelZoomStepPercent", settings.wheelZoomStepPercent).toDouble();
    settings.wheelZoomMinScale = store.value("treemap/wheelZoomMinScale", settings.wheelZoomMinScale).toDouble();
    settings.wheelZoomMaxScale = store.value("treemap/wheelZoomMaxScale", settings.wheelZoomMaxScale).toDouble();
    settings.fastWheelZoom = store.value("treemap/fastWheelZoom", settings.fastWheelZoom).toBool();
    settings.trackpadScrollPans = store.value("treemap/trackpadScrollPans", settings.trackpadScrollPans).toBool();
    settings.cameraDurationMs = store.value("treemap/cameraDurationMs", settings.cameraDurationMs).toInt();
    settings.cameraMaxScale = store.value("treemap/cameraMaxScale", settings.cameraMaxScale).toDouble();
    settings.parallelPartitionDepth = store.value("treemap/parallelPartitionDepth", settings.parallelPartitionDepth).toInt();
    settings.maxSemanticDepth = store.value("treemap/maxSemanticDepth", settings.maxSemanticDepth).toInt();
    settings.edgeFocusInsetRatio = store.value("treemap/edgeFocusInsetRatio", settings.edgeFocusInsetRatio).toDouble();
    settings.tileAspectBias = store.value("treemap/tileAspectBias", settings.tileAspectBias).toDouble();
    settings.liveScanPreview = store.value("treemap/liveScanPreview", settings.liveScanPreview).toBool();
    settings.simpleTooltips = store.value("treemap/simpleTooltips", settings.simpleTooltips).toBool();
    settings.hideNonLocalFreeSpace = store.value("treemap/hideNonLocalFreeSpace", settings.hideNonLocalFreeSpace).toBool();
    settings.showThumbnails = store.value("treemap/showThumbnails", settings.showThumbnails).toBool();
    settings.thumbnailResolution = store.value("treemap/thumbnailResolution", settings.thumbnailResolution).toInt();
    settings.thumbnailMinTileSize = store.value("treemap/thumbnailMinTileSize", settings.thumbnailMinTileSize).toInt();
    settings.thumbnailMemoryLimitMB = store.value("treemap/thumbnailMemoryLimitMB", settings.thumbnailMemoryLimitMB).toInt();
    settings.thumbnailMaxFileSizeMB = store.value("treemap/thumbnailMaxFileSizeMB", settings.thumbnailMaxFileSizeMB).toInt();
    settings.thumbnailSkipNetworkPaths = store.value("treemap/thumbnailSkipNetworkPaths", settings.thumbnailSkipNetworkPaths).toBool();
    settings.thumbnailFitMode = store.value("treemap/thumbnailFitMode", settings.thumbnailFitMode).toInt();
    settings.excludedPaths = store.value("treemap/excludedPaths", settings.excludedPaths).toStringList();
    settings.activeColorThemeId = store.value("treemap/activeColorThemeId", settings.activeColorThemeId).toString();
    settings.lightModeColorThemeId = store.value("treemap/lightModeColorThemeId", settings.lightModeColorThemeId).toString();
    settings.darkModeColorThemeId = store.value("treemap/darkModeColorThemeId", settings.darkModeColorThemeId).toString();
    settings.followSystemColorTheme = store.value("treemap/followSystemColorTheme", settings.followSystemColorTheme).toBool();
    settings.limitToSameFilesystem = store.value("treemap/limitToSameFilesystem",
                                                 settings.limitToSameFilesystem).toBool();

    const int themeCount = store.beginReadArray("treemap/colorThemes");
    if (themeCount > 0) {
        settings.colorThemes.clear();
        for (int i = 0; i < themeCount; ++i) {
            store.setArrayIndex(i);
            TreemapColorTheme theme;
            theme.id = store.value("id").toString();
            theme.name = store.value("name").toString();
            theme.folderColorMode = store.value("folderColorMode", theme.folderColorMode).toInt();
            {
                const QColor saved = store.value("folderBaseColor").value<QColor>();
                if (saved.isValid()) {
                    theme.folderBaseColor = saved;
                } else {
                    // Migrate from old separate hue/sat/bright fields.
                    const int oldHue = store.value("folderTopLevelHue", 210).toInt();
                    const double oldSat = store.value("folderColorSaturation", 0.85).toDouble();
                    const double oldBright = store.value("folderColorBrightness", 0.97).toDouble();
                    theme.folderBaseColor = QColor::fromHsl(
                        ((oldHue % 360) + 360) % 360,
                        qRound(qBound(0.0, oldSat, 1.0) * 255),
                        qRound(qBound(0.0, oldBright, 1.0) * 255));
                }
            }
            theme.folderColorSaturation = store.value("folderColorSaturation", theme.folderColorSaturation).toDouble();
            theme.folderColorBrightness = store.value("folderColorBrightness", theme.folderColorBrightness).toDouble();
            theme.folderDepthBrightnessMode = store.value("folderDepthBrightnessMode", theme.folderDepthBrightnessMode).toInt();
            theme.folderColorDarkenPerLevel = store.value("folderColorDarkenPerLevel", theme.folderColorDarkenPerLevel).toDouble();
            theme.depthGradientPreset = store.value("depthGradientPreset", theme.depthGradientPreset).toInt();
            theme.depthGradientFlipped = store.value("depthGradientFlipped", theme.depthGradientFlipped).toBool();
            theme.fileColorSaturation = store.value("fileColorSaturation", theme.fileColorSaturation).toDouble();
            theme.fileColorBrightness = store.value("fileColorBrightness", theme.fileColorBrightness).toDouble();
            theme.borderStyle = store.value("borderStyle", theme.borderStyle).toInt();
            theme.borderIntensity = store.value("borderIntensity", theme.borderIntensity).toDouble();
            theme.highlightColor = store.value("highlightColor").value<QColor>();
            theme.freeSpaceColor = store.value("freeSpaceColor", theme.freeSpaceColor).value<QColor>();
            theme.unknownFileTypeColor = store.value("unknownFileTypeColor", theme.unknownFileTypeColor).value<QColor>();
            theme.highlightOpacity = store.value("highlightOpacity", theme.highlightOpacity).toDouble();
            theme.unknownFileTypeOpacity = store.contains("unknownFileTypeOpacity")
                ? store.value("unknownFileTypeOpacity", theme.unknownFileTypeOpacity).toDouble()
                : theme.unknownFileTypeColor.alphaF();
            settings.colorThemes.push_back(theme);
        }
    }
    store.endArray();

    const int groupCount = store.beginReadArray("treemap/fileTypeGroups");
    if (groupCount > 0) {
        settings.fileTypeGroups.clear();
        for (int i = 0; i < groupCount; ++i) {
            store.setArrayIndex(i);
            FileTypeGroup g;
            g.name = store.value("name").toString();
            g.color = QColor(store.value("color").toString());
            const QString exts = store.value("extensions").toString();
            g.extensions = exts.split(QLatin1Char(','), Qt::SkipEmptyParts);
            settings.fileTypeGroups.append(std::move(g));
        }
    }
    store.endArray();

    const bool hasSavedThemeArray = themeCount > 0;
    const bool hasLegacyColorKeys = store.contains("treemap/folderColorMode")
        || store.contains("treemap/folderTopLevelHue")
        || store.contains("treemap/highlightColor");
    if (!hasSavedThemeArray && hasLegacyColorKeys) {
        TreemapColorTheme migrated;
        migrated.id = TreemapColorTheme::createCustomId();
        migrated.name = QStringLiteral("Migrated theme");
        migrated.folderColorMode = settings.folderColorMode;
        {
            const int oldHue = store.value("treemap/folderTopLevelHue", 210).toInt();
            migrated.folderBaseColor = QColor::fromHsl(
                ((oldHue % 360) + 360) % 360,
                qRound(qBound(0.0, settings.folderColorSaturation, 1.0) * 255),
                qRound(qBound(0.0, settings.folderColorBrightness, 1.0) * 255));
        }
        migrated.folderColorSaturation = settings.folderColorSaturation;
        migrated.folderColorBrightness = settings.folderColorBrightness;
        migrated.folderDepthBrightnessMode = settings.folderDepthBrightnessMode;
        migrated.folderColorDarkenPerLevel = settings.folderColorDarkenPerLevel;
        migrated.depthGradientPreset = settings.depthGradientPreset;
        migrated.depthGradientFlipped = settings.depthGradientFlipped;
        migrated.fileColorSaturation = settings.fileColorSaturation;
        migrated.fileColorBrightness = settings.fileColorBrightness;
        migrated.borderStyle = settings.borderStyle;
        migrated.borderIntensity = settings.borderIntensity;
        migrated.highlightColor = settings.highlightColor;
        migrated.freeSpaceColor = settings.freeSpaceColor;
        migrated.unknownFileTypeColor = settings.unknownFileTypeColor;
        migrated.highlightOpacity = settings.highlightOpacity;
        migrated.unknownFileTypeOpacity = settings.unknownFileTypeOpacity;
        settings.colorThemes.push_back(migrated);
        settings.activeColorThemeId = migrated.id;
    }

    settings.sanitize();
    return settings;
}

void TreemapSettings::save(QSettings& store) const
{
    TreemapSettings snapshot = *this;
    snapshot.sanitize();
    store.setValue("treemap/headerHeight", snapshot.headerHeight);
    store.setValue("treemap/headerFontFamily", snapshot.headerFontFamily);
    store.setValue("treemap/headerFontSize", snapshot.headerFontSize);
    store.setValue("treemap/headerFontBold", snapshot.headerFontBold);
    store.setValue("treemap/headerFontItalic", snapshot.headerFontItalic);
    store.setValue("treemap/fileFontFamily", snapshot.fileFontFamily);
    store.setValue("treemap/fileFontSize", snapshot.fileFontSize);
    store.setValue("treemap/fileFontBold", snapshot.fileFontBold);
    store.setValue("treemap/fileFontItalic", snapshot.fileFontItalic);
    store.setValue("treemap/folderColorMode", snapshot.folderColorMode);
    store.setValue("treemap/folderColorSaturation", snapshot.folderColorSaturation);
    store.setValue("treemap/folderColorBrightness", snapshot.folderColorBrightness);
    store.setValue("treemap/folderDepthBrightnessMode", snapshot.folderDepthBrightnessMode);
    store.setValue("treemap/folderColorDarkenPerLevel", snapshot.folderColorDarkenPerLevel);
    store.setValue("treemap/fileColorSaturation", snapshot.fileColorSaturation);
    store.setValue("treemap/fileColorBrightness", snapshot.fileColorBrightness);
    store.setValue("treemap/border", snapshot.border);
    store.setValue("treemap/borderStyle", snapshot.borderStyle);
    store.setValue("treemap/borderIntensity", snapshot.borderIntensity);
    store.setValue("treemap/highlightColor", snapshot.highlightColor);
    store.setValue("treemap/freeSpaceColor", snapshot.freeSpaceColor);
    store.setValue("treemap/randomColorForUnknownFiles", snapshot.randomColorForUnknownFiles);
    store.setValue("treemap/highlightOpacity", snapshot.highlightOpacity);
    store.setValue("treemap/unknownFileTypeOpacity", snapshot.unknownFileTypeOpacity);
    store.setValue("treemap/folderPadding", snapshot.folderPadding);
    store.setValue("treemap/baseVisibleDepth", snapshot.baseVisibleDepth);
    store.setValue("treemap/depthRevealPerZoomDoubling", snapshot.depthRevealPerZoomDoubling);
    store.setValue("treemap/minTileSize", snapshot.minTileSize);
    store.setValue("treemap/minPaint", snapshot.minPaint);
    store.setValue("treemap/minRevealWidth", snapshot.minRevealWidth);
    store.setValue("treemap/minRevealHeight", snapshot.minRevealHeight);
    store.setValue("treemap/revealFadeWidth", snapshot.revealFadeWidth);
    store.setValue("treemap/revealFadeHeight", snapshot.revealFadeHeight);
    store.setValue("treemap/zoomDurationMs", snapshot.zoomDurationMs);
    store.setValue("treemap/layoutDurationMs", snapshot.layoutDurationMs);
    store.setValue("treemap/wheelZoomStepPercent", snapshot.wheelZoomStepPercent);
    store.setValue("treemap/wheelZoomMinScale", snapshot.wheelZoomMinScale);
    store.setValue("treemap/wheelZoomMaxScale", snapshot.wheelZoomMaxScale);
    store.setValue("treemap/fastWheelZoom", snapshot.fastWheelZoom);
    store.setValue("treemap/trackpadScrollPans", snapshot.trackpadScrollPans);
    store.setValue("treemap/cameraDurationMs", snapshot.cameraDurationMs);
    store.setValue("treemap/cameraMaxScale", snapshot.cameraMaxScale);
    store.setValue("treemap/parallelPartitionDepth", snapshot.parallelPartitionDepth);
    store.setValue("treemap/maxSemanticDepth", snapshot.maxSemanticDepth);
    store.setValue("treemap/edgeFocusInsetRatio", snapshot.edgeFocusInsetRatio);
    store.setValue("treemap/tileAspectBias", snapshot.tileAspectBias);
    store.setValue("treemap/liveScanPreview", snapshot.liveScanPreview);
    store.setValue("treemap/simpleTooltips", snapshot.simpleTooltips);
    store.setValue("treemap/hideNonLocalFreeSpace", snapshot.hideNonLocalFreeSpace);
    store.setValue("treemap/showThumbnails", snapshot.showThumbnails);
    store.setValue("treemap/thumbnailResolution", snapshot.thumbnailResolution);
    store.setValue("treemap/thumbnailMinTileSize", snapshot.thumbnailMinTileSize);
    store.setValue("treemap/thumbnailMemoryLimitMB", snapshot.thumbnailMemoryLimitMB);
    store.setValue("treemap/thumbnailMaxFileSizeMB", snapshot.thumbnailMaxFileSizeMB);
    store.setValue("treemap/thumbnailSkipNetworkPaths", snapshot.thumbnailSkipNetworkPaths);
    store.setValue("treemap/thumbnailFitMode", snapshot.thumbnailFitMode);
    store.setValue("treemap/excludedPaths", snapshot.excludedPaths);
    store.setValue("treemap/activeColorThemeId", snapshot.activeColorThemeId);
    store.setValue("treemap/lightModeColorThemeId", snapshot.lightModeColorThemeId);
    store.setValue("treemap/darkModeColorThemeId", snapshot.darkModeColorThemeId);
    store.setValue("treemap/followSystemColorTheme", snapshot.followSystemColorTheme);
    store.setValue("treemap/limitToSameFilesystem", snapshot.limitToSameFilesystem);

    store.beginWriteArray("treemap/colorThemes");
    for (int i = 0; i < snapshot.colorThemes.size(); ++i) {
        const TreemapColorTheme& theme = snapshot.colorThemes.at(i);
        store.setArrayIndex(i);
        store.setValue("id", theme.id);
        store.setValue("name", theme.name);
        store.setValue("folderColorMode", theme.folderColorMode);
        store.setValue("folderBaseColor", theme.folderBaseColor);
        store.setValue("folderColorSaturation", theme.folderColorSaturation);
        store.setValue("folderColorBrightness", theme.folderColorBrightness);
        store.setValue("folderDepthBrightnessMode", theme.folderDepthBrightnessMode);
        store.setValue("folderColorDarkenPerLevel", theme.folderColorDarkenPerLevel);
        store.setValue("depthGradientPreset", theme.depthGradientPreset);
        store.setValue("depthGradientFlipped", theme.depthGradientFlipped);
        store.setValue("fileColorSaturation", theme.fileColorSaturation);
        store.setValue("fileColorBrightness", theme.fileColorBrightness);
        store.setValue("borderStyle", theme.borderStyle);
        store.setValue("borderIntensity", theme.borderIntensity);
        store.setValue("highlightColor", theme.highlightColor);
        store.setValue("freeSpaceColor", theme.freeSpaceColor);
        store.setValue("unknownFileTypeColor", theme.unknownFileTypeColor);
        store.setValue("highlightOpacity", theme.highlightOpacity);
        store.setValue("unknownFileTypeOpacity", theme.unknownFileTypeOpacity);
    }
    store.endArray();

    store.beginWriteArray("treemap/fileTypeGroups");
    for (int i = 0; i < snapshot.fileTypeGroups.size(); ++i) {
        store.setArrayIndex(i);
        const FileTypeGroup& g = snapshot.fileTypeGroups.at(i);
        store.setValue("name", g.name);
        store.setValue("color", g.color.name(QColor::HexRgb));
        store.setValue("extensions", g.extensions.join(QLatin1Char(',')));
    }
    store.endArray();
}

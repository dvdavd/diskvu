// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QStringList>

// Implemented in treemapsettings.cpp
QColor defaultFreeSpaceColor();
class QSettings;

struct TreemapColorTheme {
    QString id;
    QString name;
    int folderColorMode = 1;
    QColor folderBaseColor = QColor::fromHsl(210, 200, 150);
    double folderColorSaturation = 0.85;
    double folderColorBrightness = 0.97;
    int folderDepthBrightnessMode = 0;
    double folderColorDarkenPerLevel = 0.03;
    int depthGradientPreset = 0;
    bool depthGradientFlipped = false;
    double fileColorSaturation = 0.60;
    double fileColorBrightness = 0.70;
    int borderStyle = 2;
    double borderIntensity = 0.50;
    QColor highlightColor;
    QColor freeSpaceColor;
    QColor unknownFileTypeColor;
    double highlightOpacity = 0.75;
    double unknownFileTypeOpacity = 0.5;

    static QString builtInLightId() { return QStringLiteral("theme-light"); }
    static QString builtInDarkId()  { return QStringLiteral("theme-dark"); }

    static bool isBuiltInId(const QString& themeId)
    {
        return themeId == builtInLightId() || themeId == builtInDarkId();
    }

    static TreemapColorTheme defaultLightTheme();
    static TreemapColorTheme defaultDarkTheme();
    static QList<TreemapColorTheme> defaults();
    static QString createCustomId();

    void sanitize();
};

struct FileTypeGroup {
    QString name;           // e.g. "Images"
    QColor color;           // exact colour; not scaled by theme sat/brightness sliders
    QStringList extensions; // lowercase, no dot: {"png", "jpg", "gif"}
};

struct TreemapSettings {
    static QColor defaultHighlightColor();
    static void applyDefaults(TreemapSettings& settings);

    enum FolderColorMode {
        SingleHue = 0,
        DistinctTopLevel = 1,
        DepthGradient = 2,
    };

    enum FolderDepthBrightnessMode {
        DarkenPerLevel = 0,
        LightenPerLevel = 1,
    };

    enum BorderStyle {
        DarkenBorder = 0,
        LightenBorder = 1,
        AutomaticBorder = 2,
    };

    double headerHeight = 18.0;
    QString headerFontFamily;
    int headerFontSize = 8;
    bool headerFontBold = false;
    bool headerFontItalic = false;
    QString fileFontFamily;
    int fileFontSize = 8;
    bool fileFontBold = false;
    bool fileFontItalic = false;
    int folderColorMode = DistinctTopLevel;
    QColor folderBaseColor = QColor::fromHsl(210, 200, 150);
    double folderColorSaturation = 0.86;
    double folderColorBrightness = 0.56;
    int folderDepthBrightnessMode = DarkenPerLevel;
    double folderColorDarkenPerLevel = 0.05;
    int depthGradientPreset = 0;
    bool depthGradientFlipped = false;
    double fileColorSaturation = 0.68;
    double fileColorBrightness = 0.60;
    double border = 1.0;
    int borderStyle = AutomaticBorder;
    double borderIntensity = 0.55;
    QColor highlightColor;
    QColor freeSpaceColor = defaultFreeSpaceColor();
    QColor unknownFileTypeColor;
    bool randomColorForUnknownFiles = true;
    double highlightOpacity = 0.75;
    double unknownFileTypeOpacity = 0.5;
    double folderPadding = 4.0;
    int baseVisibleDepth = 7;
    double depthRevealPerZoomDoubling = 3.0;
    double minTileSize = 4.0;
    double minPaint = 2.0;
    double minRevealWidth = 24.0;
    double minRevealHeight = 20.0;
    double revealFadeWidth = 14.0;
    double revealFadeHeight = 10.0;
    int zoomDurationMs = 180;
    int layoutDurationMs = 90;
    double wheelZoomMinScale = 0.38;
    double wheelZoomMaxScale = 0.72;
    int cameraDurationMs = 140;
    double cameraMaxScale = 256.0;
    int parallelPartitionDepth = 3;
    int maxSemanticDepth = 20;
    double edgeFocusInsetRatio = 0.18;
    double tileAspectBias = 0.0;
    bool liveScanPreview = true;
    bool simpleTooltips = false;
    bool enableScanActivityTracking = true;
    bool hideNonLocalFreeSpace = false;
    bool limitToSameFilesystem = false;
    QStringList excludedPaths;
    QList<FileTypeGroup> fileTypeGroups;
    QList<TreemapColorTheme> colorThemes;
    QString activeColorThemeId;
    QString lightModeColorThemeId;
    QString darkModeColorThemeId;
    bool followSystemColorTheme = true;

    TreemapSettings();

    TreemapColorTheme* findColorTheme(const QString& themeId);
    const TreemapColorTheme* findColorTheme(const QString& themeId) const;
    void applyColorTheme(const TreemapColorTheme& theme);
    TreemapColorTheme activeColorTheme() const;
    QString colorThemeIdForSystemScheme(bool darkMode) const;
    void applyActiveColorTheme();
    void ensureColorThemes();
    void sanitize();
    static TreemapSettings defaults();
    static TreemapSettings load(QSettings& store);
    void save(QSettings& store) const;
};

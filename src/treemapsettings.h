// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>

enum class FolderMark : int {
    None = 0,
    ColorRed, ColorOrange, ColorYellow, ColorGreen, ColorBlue, ColorPurple,
    CatGames, CatDevelopment, CatBackup, CatCloud, CatPhotos, CatDownloads, CatTemporary,
    CatMusic, CatFavourites, CatEncrypted, CatVideo
};

inline bool isFolderColorMark(FolderMark m)
{
    return m >= FolderMark::ColorRed && m <= FolderMark::ColorPurple;
}

inline bool isFolderIconMark(FolderMark m)
{
    return m >= FolderMark::CatGames && m <= FolderMark::CatVideo;
}

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
    double wheelZoomStepPercent = 20.0;
    double wheelZoomMinScale = 0.38;
    double wheelZoomMaxScale = 0.72;
    int cameraDurationMs = 140;
    bool fastWheelZoom = false;
    bool trackpadScrollPans = false;
    bool doubleClickToOpen = false;
    double cameraMaxScale = 256.0;
    int parallelPartitionDepth = 4;
    int maxSemanticDepth = 20;
    double edgeFocusInsetRatio = 0.18;
    double tileAspectBias = 0.0;
    enum ScanPreviewMode {
        ScanPreviewNone = 0,
        ScanPreviewFast = 1,
    };
    int scanPreviewMode = ScanPreviewFast;
    bool simpleTooltips = false;
    bool enableScanActivityTracking = true;
    bool hideNonLocalFreeSpace = false;
    bool limitToSameFilesystem = false;
    enum ThumbnailFitMode {
        ThumbnailFill = 0,    // crop to fill tile, keep AR
        ThumbnailFit = 1,     // letterbox, keep AR
        ThumbnailStretch = 2, // stretch, ignore AR
    };

    bool showThumbnails = false;
    bool showVideoThumbnails = false;
    bool showFileFlags = true;
    int thumbnailResolution = 256;
    int thumbnailMinTileSize = 80;
    int thumbnailMemoryLimitMB = 256;
    int thumbnailMaxFileSizeMB = 50;
    bool thumbnailSkipNetworkPaths = true;
    int thumbnailFitMode = ThumbnailFill;
    QStringList excludedPaths;
    QList<FileTypeGroup> fileTypeGroups;
    QList<TreemapColorTheme> colorThemes;
    QString activeColorThemeId;
    QString lightModeColorThemeId;
    QString darkModeColorThemeId;
    bool followSystemColorTheme = true;
    QHash<QString, FolderMark> folderColorMarks;
    QHash<QString, FolderMark> folderIconMarks;
    QSet<QString> markedPathPrefixes; // All prefixes of marked paths for pruning traversals

    TreemapSettings();

    void rebuildMarkPrefixes();
    bool mightHaveMarkInSubtree(const QString& path) const;

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

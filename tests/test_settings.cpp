// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "treemapsettings.h"

#include <QApplication>
#include <QSettings>
#include <QTemporaryFile>
#include <QtTest/QtTest>

class TestSettings : public QObject {
    Q_OBJECT

private slots:
    // ── TreemapColorTheme ────────────────────────────────────────────────────

    void colorTheme_builtInIds()
    {
        QCOMPARE(TreemapColorTheme::builtInLightId(), QStringLiteral("theme-light"));
        QCOMPARE(TreemapColorTheme::builtInDarkId(),  QStringLiteral("theme-dark"));
    }

    void colorTheme_isBuiltInId()
    {
        QVERIFY( TreemapColorTheme::isBuiltInId(TreemapColorTheme::builtInLightId()));
        QVERIFY( TreemapColorTheme::isBuiltInId(TreemapColorTheme::builtInDarkId()));
        QVERIFY(!TreemapColorTheme::isBuiltInId(QStringLiteral("theme-custom-abc")));
        QVERIFY(!TreemapColorTheme::isBuiltInId(QString()));
    }

    void colorTheme_defaultLightTheme_hasValidFields()
    {
        const TreemapColorTheme t = TreemapColorTheme::defaultLightTheme();
        QCOMPARE(t.id, TreemapColorTheme::builtInLightId());
        QCOMPARE(t.fileColorSaturation, 0.75);
        QVERIFY(t.highlightColor.isValid());
        QVERIFY(t.freeSpaceColor.isValid());
        QVERIFY(t.folderColorSaturation >= 0.0 && t.folderColorSaturation <= 1.0);
        QVERIFY(t.folderColorBrightness >= 0.0 && t.folderColorBrightness <= 1.0);
        QCOMPARE(t.borderStyle, TreemapSettings::AutomaticBorder);
        QVERIFY(t.borderIntensity    >= 0.0 && t.borderIntensity    <= 1.0);
    }

    void colorTheme_defaultDarkTheme_hasValidFields()
    {
        const TreemapColorTheme t = TreemapColorTheme::defaultDarkTheme();
        QCOMPARE(t.id, TreemapColorTheme::builtInDarkId());
        QVERIFY(t.highlightColor.isValid());
        QVERIFY(t.freeSpaceColor.isValid());
        QVERIFY(t.folderColorBrightness < TreemapColorTheme::defaultLightTheme().folderColorBrightness);
    }

    void colorTheme_defaults_containsBothBuiltIns()
    {
        const QList<TreemapColorTheme> list = TreemapColorTheme::defaults();
        QCOMPARE(list.size(), 2);
        QCOMPARE(list.at(0).id, TreemapColorTheme::builtInLightId());
        QCOMPARE(list.at(1).id, TreemapColorTheme::builtInDarkId());
    }

    void colorTheme_createCustomId_isUniqueAndPrefixed()
    {
        const QString id = TreemapColorTheme::createCustomId();
        QVERIFY(id.startsWith(QStringLiteral("theme-custom-")));
        QVERIFY(!TreemapColorTheme::isBuiltInId(id));
        // Each call produces a different ID
        QVERIFY(id != TreemapColorTheme::createCustomId());
    }

    void colorTheme_sanitize_clampsSaturationAndBrightness()
    {
        TreemapColorTheme t;
        t.folderColorSaturation    = 2.0;
        t.folderColorBrightness    = -1.0;
        t.fileColorSaturation      = 5.0;
        t.fileColorBrightness      = -0.5;
        t.borderIntensity          = 3.0;
        t.folderColorDarkenPerLevel = 1.0;
        t.highlightOpacity         = -2.0;
        t.unknownFileTypeOpacity   = 2.0;
        t.sanitize();

        QCOMPARE(t.folderColorSaturation,    1.0);
        QCOMPARE(t.folderColorBrightness,    0.0);
        QCOMPARE(t.fileColorSaturation,      1.0);
        QCOMPARE(t.fileColorBrightness,      0.0);
        QCOMPARE(t.borderIntensity,          1.0);
        QCOMPARE(t.folderColorDarkenPerLevel, 0.30);
        QCOMPARE(t.highlightOpacity,         0.0);
        QCOMPARE(t.unknownFileTypeOpacity,   1.0);
    }

    void colorTheme_sanitize_fixesInvalidFreeSpaceColor()
    {
        TreemapColorTheme t;
        t.freeSpaceColor = QColor(); // invalid
        t.sanitize();
        QVERIFY(t.freeSpaceColor.isValid());
    }

    void colorTheme_sanitize_fixesInvalidUnknownFileTypeColor()
    {
        TreemapColorTheme t;
        t.unknownFileTypeColor = QColor();
        t.sanitize();
        QVERIFY(t.unknownFileTypeColor.isValid());
    }

    void colorTheme_sanitize_trimsStrings()
    {
        TreemapColorTheme t;
        t.id   = "  theme-light  ";
        t.name = "  Light  ";
        t.sanitize();
        QCOMPARE(t.id,   QStringLiteral("theme-light"));
        QCOMPARE(t.name, QStringLiteral("Light"));
    }

    // ── TreemapSettings ──────────────────────────────────────────────────────

    void settings_defaultConstructor_isValid()
    {
        const TreemapSettings s;
        QVERIFY(s.headerHeight  >= 1.0);
        QVERIFY(s.headerFontSize >= 1);
        QVERIFY(s.freeSpaceColor.isValid());
        QVERIFY(!s.colorThemes.isEmpty());
        QVERIFY(!s.activeColorThemeId.isEmpty());
    }

    void settings_sanitize_clampsHeaderHeight()
    {
        TreemapSettings s;
        s.headerHeight = -99.0;
        s.sanitize();
        QVERIFY(s.headerHeight >= 1.0);
    }

    void settings_sanitize_clampsParallelPartitionDepth()
    {
        TreemapSettings s;
        s.parallelPartitionDepth = 0;
        s.sanitize();
        QCOMPARE(s.parallelPartitionDepth, 1);

        s.parallelPartitionDepth = 999;
        s.sanitize();
        QCOMPARE(s.parallelPartitionDepth, 8);
    }

    void settings_sanitize_clampsWheelZoomScales()
    {
        TreemapSettings s;
        s.wheelZoomMinScale = -1.0;
        s.wheelZoomMaxScale = -1.0;
        s.sanitize();
        QVERIFY(s.wheelZoomMinScale > 0.0);
        QVERIFY(s.wheelZoomMaxScale >= s.wheelZoomMinScale);
    }

    void settings_sanitize_removesDuplicateExcludedPaths()
    {
        TreemapSettings s;
        s.excludedPaths = {"/foo/bar", "/foo/bar", "/baz"};
        s.sanitize();
        QCOMPARE(s.excludedPaths.size(), 2);
        QVERIFY(s.excludedPaths.contains("/foo/bar"));
        QVERIFY(s.excludedPaths.contains("/baz"));
    }

    void settings_sanitize_removesBlankExcludedPaths()
    {
        TreemapSettings s;
        s.excludedPaths = {"", "   ", "/valid/path"};
        s.sanitize();
        QCOMPARE(s.excludedPaths.size(), 1);
        QCOMPARE(s.excludedPaths.at(0), "/valid/path");
    }

    void settings_sanitize_normalizesFileTypeGroups()
    {
        TreemapSettings s;
        s.fileTypeGroups = {
            { QStringLiteral("  Images  "), QColor(QStringLiteral("#AA33DD")),
              { QStringLiteral(" PNG "), QStringLiteral("jpg"), QStringLiteral("  "), QStringLiteral("Jpeg  ") } }
        };

        s.sanitize();

        QCOMPARE(s.fileTypeGroups.size(), 1);
        QCOMPARE(s.fileTypeGroups.at(0).name, QStringLiteral("Images"));
        QCOMPARE(s.fileTypeGroups.at(0).extensions,
                 QStringList({QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg")}));
    }

    void settings_findColorTheme_knownId()
    {
        TreemapSettings s;
        const TreemapColorTheme* t = s.findColorTheme(TreemapColorTheme::builtInLightId());
        QVERIFY(t != nullptr);
        QCOMPARE(t->id, TreemapColorTheme::builtInLightId());
    }

    void settings_findColorTheme_unknownId()
    {
        TreemapSettings s;
        QVERIFY(s.findColorTheme(QStringLiteral("does-not-exist")) == nullptr);
    }

    void settings_ensureColorThemes_alwaysHasLightAndDark()
    {
        TreemapSettings s;
        s.colorThemes.clear();
        s.ensureColorThemes();

        bool hasLight = false, hasDark = false;
        for (const TreemapColorTheme& t : s.colorThemes) {
            hasLight = hasLight || (t.id == TreemapColorTheme::builtInLightId());
            hasDark  = hasDark  || (t.id == TreemapColorTheme::builtInDarkId());
        }
        QVERIFY(hasLight);
        QVERIFY(hasDark);
    }

    void settings_ensureColorThemes_deduplicatesIds()
    {
        TreemapSettings s;
        // Add two themes with the same ID
        TreemapColorTheme dup;
        dup.id   = "dupe";
        dup.name = "Dupe";
        s.colorThemes.push_back(dup);
        s.colorThemes.push_back(dup);
        s.ensureColorThemes();

        QStringList ids;
        for (const TreemapColorTheme& t : s.colorThemes)
            ids.push_back(t.id);
        QCOMPARE(ids.removeDuplicates(), 0); // removeDuplicates returns count removed; 0 = already unique
    }

    void settings_colorThemeIdForSystemScheme_defaults()
    {
        const TreemapSettings s;
        QCOMPARE(s.colorThemeIdForSystemScheme(false), TreemapColorTheme::builtInLightId());
        QCOMPARE(s.colorThemeIdForSystemScheme(true),  TreemapColorTheme::builtInDarkId());
    }

    void settings_colorThemeIdForSystemScheme_fallsBackToBuiltIn()
    {
        TreemapSettings s;
        s.lightModeColorThemeId = "nonexistent-light";
        s.darkModeColorThemeId  = "nonexistent-dark";
        QCOMPARE(s.colorThemeIdForSystemScheme(false), TreemapColorTheme::builtInLightId());
        QCOMPARE(s.colorThemeIdForSystemScheme(true),  TreemapColorTheme::builtInDarkId());
    }

    void settings_applyColorTheme_updatesUnknownFileTypeColor()
    {
        TreemapSettings s;
        TreemapColorTheme theme = TreemapColorTheme::defaultDarkTheme();
        theme.unknownFileTypeColor = QColor(QStringLiteral("#224466"));

        s.applyColorTheme(theme);

        QCOMPARE(s.unknownFileTypeColor, theme.unknownFileTypeColor);
    }

    void settings_loadSave_roundtrip()
    {
        QTemporaryFile tmpFile;
        QVERIFY(tmpFile.open());
        tmpFile.close();

        TreemapSettings original;
        original.headerHeight       = 22.0;
        original.headerFontSize     = 11;
        original.baseVisibleDepth   = 5;
        original.depthRevealPerZoomDoubling = 3.5;
        original.minRevealWidth     = 84.0;
        original.revealFadeWidth    = 18.0;
        original.liveScanPreview    = false;
        original.excludedPaths      = {"/tmp/no-scan"};
        original.activeColorThemeId = TreemapColorTheme::builtInDarkId();
        {
            QSettings store(tmpFile.fileName(), QSettings::IniFormat);
            original.save(store);
        }

        TreemapSettings loaded;
        {
            QSettings store(tmpFile.fileName(), QSettings::IniFormat);
            loaded = TreemapSettings::load(store);
        }

        QCOMPARE(loaded.headerHeight,       original.headerHeight);
        QCOMPARE(loaded.headerFontSize,     original.headerFontSize);
        QCOMPARE(loaded.baseVisibleDepth,   original.baseVisibleDepth);
        QCOMPARE(loaded.depthRevealPerZoomDoubling, original.depthRevealPerZoomDoubling);
        QCOMPARE(loaded.minRevealWidth,     original.minRevealWidth);
        QCOMPARE(loaded.revealFadeWidth,    original.revealFadeWidth);
        QCOMPARE(loaded.liveScanPreview,    original.liveScanPreview);
        QCOMPARE(loaded.excludedPaths,      original.excludedPaths);
        QCOMPARE(loaded.activeColorThemeId, original.activeColorThemeId);
    }

    void settings_load_legacyRevealThresholds_migratesToSimplifiedModel()
    {
        QTemporaryFile tmpFile;
        QVERIFY(tmpFile.open());
        tmpFile.close();

        {
            QSettings store(tmpFile.fileName(), QSettings::IniFormat);
            store.setValue("treemap/minContentWidth", 20.0);
            store.setValue("treemap/minContentHeight", 18.0);
            store.setValue("treemap/minLiveRevealWidth", 40.0);
            store.setValue("treemap/minLiveRevealHeight", 36.0);
        }

        TreemapSettings loaded;
        {
            QSettings store(tmpFile.fileName(), QSettings::IniFormat);
            loaded = TreemapSettings::load(store);
        }

        QCOMPARE(loaded.minRevealWidth, 40.0);
        QCOMPARE(loaded.minRevealHeight, 36.0);
        QCOMPARE(loaded.revealFadeWidth, 20.0);
        QCOMPARE(loaded.revealFadeHeight, 18.0);
    }

    void settings_loadSave_fileTypeOptions_roundtrip()
    {
        QTemporaryFile tmpFile;
        QVERIFY(tmpFile.open());
        tmpFile.close();

        TreemapSettings original;
        original.randomColorForUnknownFiles = false;
        original.fileTypeGroups = {
            { QStringLiteral("  Images  "), QColor(QStringLiteral("#3366CC")),
              { QStringLiteral(" PNG "), QStringLiteral("jpg")} },
            { QStringLiteral("Docs"), QColor(QStringLiteral("#AA5500")),
              { QStringLiteral("pdf")} }
        };

        {
            QSettings store(tmpFile.fileName(), QSettings::IniFormat);
            original.save(store);
        }

        TreemapSettings loaded;
        {
            QSettings store(tmpFile.fileName(), QSettings::IniFormat);
            loaded = TreemapSettings::load(store);
        }

        QCOMPARE(loaded.randomColorForUnknownFiles, original.randomColorForUnknownFiles);
        QCOMPARE(loaded.fileTypeGroups.size(), 2);
        QCOMPARE(loaded.fileTypeGroups.at(0).name, QStringLiteral("Images"));
        QCOMPARE(loaded.fileTypeGroups.at(0).color, QColor(QStringLiteral("#3366CC")));
        QCOMPARE(loaded.fileTypeGroups.at(0).extensions,
                 QStringList({QStringLiteral("png"), QStringLiteral("jpg")}));
        QCOMPARE(loaded.fileTypeGroups.at(1).name, QStringLiteral("Docs"));
        QCOMPARE(loaded.fileTypeGroups.at(1).color, QColor(QStringLiteral("#AA5500")));
        QCOMPARE(loaded.fileTypeGroups.at(1).extensions, QStringList({QStringLiteral("pdf")}));
    }

    void settings_loadSave_colorThemes_roundtrip()
    {
        QTemporaryFile tmpFile;
        QVERIFY(tmpFile.open());
        tmpFile.close();

        TreemapSettings original;
        // Add a custom theme
        TreemapColorTheme custom;
        custom.id               = TreemapColorTheme::createCustomId();
        custom.name             = "My Theme";
        custom.borderStyle      = TreemapSettings::LightenBorder;
        custom.borderIntensity  = 0.42;
        custom.highlightOpacity = 0.6;
        custom.unknownFileTypeOpacity = 0.35;
        custom.freeSpaceColor   = QColor(100, 200, 50);
        custom.unknownFileTypeColor = QColor(QStringLiteral("#445566"));
        original.colorThemes.push_back(custom);
        {
            QSettings store(tmpFile.fileName(), QSettings::IniFormat);
            original.save(store);
        }

        TreemapSettings loaded;
        {
            QSettings store(tmpFile.fileName(), QSettings::IniFormat);
            loaded = TreemapSettings::load(store);
        }

        // Find the custom theme in loaded settings
        const TreemapColorTheme* found = loaded.findColorTheme(custom.id);
        QVERIFY(found != nullptr);
        QCOMPARE(found->name, custom.name);
        QCOMPARE(found->borderStyle, custom.borderStyle);
        QCOMPARE(found->borderIntensity, custom.borderIntensity);
        QCOMPARE(found->freeSpaceColor,  custom.freeSpaceColor);
        QCOMPARE(found->unknownFileTypeColor, custom.unknownFileTypeColor);
        QCOMPARE(found->unknownFileTypeOpacity, custom.unknownFileTypeOpacity);
    }

    void settings_defaults_doesNotCrash()
    {
        const TreemapSettings s = TreemapSettings::defaults();
        QVERIFY(s.headerHeight >= 1.0);
        QVERIFY(!s.colorThemes.isEmpty());
        QCOMPARE(s.fileColorSaturation, 0.75);
        QVERIFY(!s.randomColorForUnknownFiles);
    }
};

QTEST_MAIN(TestSettings)
#include "test_settings.moc"

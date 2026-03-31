// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"
#include "treemapsettings.h"

#include <QDialog>

class QDialogButtonBox;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QLabel;
class QFormLayout;
class QGroupBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class TreemapWidget;
class QWidget;

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const TreemapSettings& settings, QWidget* parent = nullptr);

    TreemapSettings settings() const;

private:
    static QDoubleSpinBox* createDoubleSpinBox(double min, double max, double step, int decimals);
    static QSlider* createPercentageSlider();
    static QSpinBox* createSpinBox(int min, int max, int step);
    static QWidget* createFieldWithDescription(QWidget* field, const QString& description);
    static QWidget* createPairedFieldWithDescription(QWidget* firstField, const QString& firstLabel,
                                                     QWidget* secondField, const QString& secondLabel,
                                                     const QString& description);
    static QWidget* createPageIntro(const QString& title, const QString& text);
    static QGroupBox* createSectionGroup(const QString& title, const QString& text, QFormLayout* formLayout);
    static FileNode* createPreviewNode(NodeArena& arena, const QString& name, qint64 size,
                                       bool isDirectory, QRgb color = 0, FileNode* parent = nullptr);
    void addExcludedPath();
    void addColorTheme();
    void duplicateColorTheme();
    void removeSelectedColorTheme();
    void removeSelectedExcludedPaths();
    void addFileTypeGroup();
    void removeSelectedFileTypeGroup();
    void onFileTypeGroupSelectionChanged();
    void updateFileTypeGroupColorButton();
    void syncFileTypeGroupFromFields(int row);
    void syncFileTypeGroupToFields(int row);
    static QIcon fileTypeGroupSwatchIcon(const QColor& color);
    void applySettingsToFields(const TreemapSettings& settings);
    void buildPreviewTree();
    void loadSelectedColorThemeIntoFields();
    void populateColorThemeSelectors();
    void refreshPreview();
    void restoreDefaultsPreservingCustomThemes();
    void storeSelectedColorThemeInto(TreemapSettings& settings) const;
    void updateFolderBaseColorButton();
    void updateHighlightColorButton();
    void updateFreeSpaceColorButton();
    void updateUnknownFileTypeColorButton();
    void updateColorThemeEditorState();
    QWidget* createFontControls(QFontComboBox* familyCombo, QSpinBox* sizeSpin,
                                QCheckBox* boldCheck, QCheckBox* italicCheck) const;

    QDoubleSpinBox* m_headerHeight = nullptr;
    QFontComboBox* m_headerFontFamily = nullptr;
    QSpinBox* m_headerFontSize = nullptr;
    QCheckBox* m_headerFontBold = nullptr;
    QCheckBox* m_headerFontItalic = nullptr;
    QFontComboBox* m_fileFontFamily = nullptr;
    QSpinBox* m_fileFontSize = nullptr;
    QCheckBox* m_fileFontBold = nullptr;
    QCheckBox* m_fileFontItalic = nullptr;
    QComboBox* m_colorThemeSelector = nullptr;
    QLineEdit* m_colorThemeName = nullptr;
    QPushButton* m_addColorThemeButton = nullptr;
    QPushButton* m_duplicateColorThemeButton = nullptr;
    QPushButton* m_removeColorThemeButton = nullptr;
    QCheckBox* m_followSystemColorTheme = nullptr;
    QComboBox* m_lightModeColorTheme = nullptr;
    QComboBox* m_darkModeColorTheme = nullptr;
    QComboBox* m_folderColorMode = nullptr;
    QComboBox* m_depthGradientPreset = nullptr;
    QCheckBox* m_depthGradientFlipped = nullptr;
    QPushButton* m_folderBaseColorButton = nullptr;
    QSlider* m_folderColorSaturation = nullptr;
    QSlider* m_folderColorBrightness = nullptr;
    QComboBox* m_folderDepthBrightnessMode = nullptr;
    QDoubleSpinBox* m_folderColorDarkenPerLevel = nullptr;
    QSlider* m_fileColorSaturation = nullptr;
    QSlider* m_fileColorBrightness = nullptr;
    QDoubleSpinBox* m_border = nullptr;
    QComboBox* m_borderStyle = nullptr;
    QSlider* m_borderIntensity = nullptr;
    QPushButton* m_highlightColorButton = nullptr;
    QPushButton* m_freeSpaceColorButton = nullptr;
    QPushButton* m_unknownFileTypeColorButton = nullptr;
    QCheckBox* m_randomColorForUnknownFiles = nullptr;
    QSlider* m_highlightOpacity = nullptr;
    QSlider* m_unknownFileTypeOpacity = nullptr;
    QDoubleSpinBox* m_folderPadding = nullptr;
    QSlider* m_baseVisibleDepth = nullptr;
    QLabel* m_baseVisibleDepthValue = nullptr;
    QDoubleSpinBox* m_depthRevealPerZoomDoubling = nullptr;
    QSlider* m_tileAspectBias = nullptr;
    QLabel* m_tileAspectBiasValue = nullptr;
    QDoubleSpinBox* m_minTileSize = nullptr;
    QDoubleSpinBox* m_minPaint = nullptr;
    QDoubleSpinBox* m_minRevealHeight = nullptr;
    QDoubleSpinBox* m_minRevealWidth = nullptr;
    QDoubleSpinBox* m_revealFadeHeight = nullptr;
    QDoubleSpinBox* m_revealFadeWidth = nullptr;
    QSpinBox* m_zoomDurationMs = nullptr;
    QSpinBox* m_cameraDurationMs = nullptr;
    QCheckBox* m_simpleTooltips = nullptr;
    QSlider* m_cameraMaxScale = nullptr;
    QLabel* m_cameraMaxScaleValue = nullptr;
    QCheckBox* m_liveScanPreview = nullptr;
    QCheckBox* m_hideNonLocalFreeSpace = nullptr;
    QLineEdit* m_excludedPathEdit = nullptr;
    QListWidget* m_excludedPathsList = nullptr;
    QListWidget* m_fileTypeGroupsList = nullptr;
    QLineEdit* m_fileTypeGroupName = nullptr;
    QPushButton* m_fileTypeGroupColorButton = nullptr;
    QPlainTextEdit* m_fileTypeGroupExtensions = nullptr;
    QGroupBox* m_fileTypeGroupEditBox = nullptr;
    TreemapWidget* m_previewWidget = nullptr;
    NodeArena m_previewArena;
    FileNode* m_previewRoot = nullptr;
    TreemapSettings m_workingSettings;
    QString m_selectedColorThemeId;
    bool m_updatingColorThemeUi = false;
};

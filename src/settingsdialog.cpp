// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "settingsdialog.h"

#include "colorutils.h"
#include "iconutils.h"
#include "mainwindow_utils.h"
#include "treemapwidget.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QButtonGroup>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QFrame>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSlider>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QToolTip>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

static void drawGradientSwatch(QPainter* painter, const QRect& r, const QList<QColor>& stops)
{
    for (int x = r.left(); x <= r.right(); ++x) {
        float t = static_cast<float>(x - r.left()) / static_cast<float>(std::max(r.width() - 1, 1));
        painter->setPen(ColorUtils::sampleGradient(stops, t));
        painter->drawLine(x, r.top(), x, r.bottom());
    }
}

namespace {
constexpr int kGradientPresetSwatchWidth = 80;
constexpr int kGradientPresetSwatchMargin = 5;
constexpr int kGradientPresetTrailingInset = 22;
constexpr int kSettingsScrollBarGap = 6;

class SettingsPageScrollArea final : public QScrollArea {
public:
    explicit SettingsPageScrollArea(QWidget* parent = nullptr)
        : QScrollArea(parent)
    {
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
                [this](int, int) { updateViewportInset(); });
        connect(verticalScrollBar(), &QScrollBar::sliderPressed, this,
                [this]() { updateViewportInset(); });
        connect(verticalScrollBar(), &QScrollBar::sliderReleased, this,
                [this]() { updateViewportInset(); });
    }

    void refreshViewportInset()
    {
        updateViewportInset();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QScrollArea::resizeEvent(event);
        updateViewportInset();
    }

    void showEvent(QShowEvent* event) override
    {
        QScrollArea::showEvent(event);
        updateViewportInset();
    }

private:
    void updateViewportInset()
    {
        const bool needsGap = verticalScrollBar() && verticalScrollBar()->isVisible();
        setViewportMargins(0, 0, needsGap ? kSettingsScrollBarGap : 0, 0);
    }
};

QRect gradientPresetTextRect(const QRect& bounds, Qt::LayoutDirection direction, int trailingInset = kGradientPresetTrailingInset)
{
    const int reservedWidth = kGradientPresetSwatchWidth + trailingInset;
    const QRect logicalRect(bounds.left(),
                            bounds.top(),
                            std::max(0, bounds.width() - reservedWidth),
                            bounds.height());
    return QStyle::visualRect(direction, bounds, logicalRect);
}

QRect gradientPresetSwatchRect(const QRect& bounds, Qt::LayoutDirection direction, int trailingInset = kGradientPresetTrailingInset)
{
    const QRect logicalRect(bounds.left() + std::max(0, bounds.width() - kGradientPresetSwatchWidth - trailingInset),
                            bounds.top() + kGradientPresetSwatchMargin,
                            kGradientPresetSwatchWidth,
                            std::max(0, bounds.height() - (kGradientPresetSwatchMargin * 2)));
    return QStyle::visualRect(direction, bounds, logicalRect);
}
}

class GradientPresetComboBox : public QComboBox {
public:
    using QComboBox::QComboBox;

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QComboBox::paintEvent(e);
        const auto stops = currentData(Qt::UserRole).value<QList<QColor>>();
        if (stops.isEmpty())
            return;
        QPainter painter(this);
        const QRect r = gradientPresetSwatchRect(rect(), layoutDirection());
        drawGradientSwatch(&painter, r, stops);
    }
};

class GradientPresetDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem bgOpt = option;
        initStyleOption(&bgOpt, index);
        const QWidget* widget = bgOpt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &bgOpt, painter, widget);

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.rect = gradientPresetTextRect(option.rect, opt.direction);
        QStyledItemDelegate::paint(painter, opt, index);

        const auto stops = index.data(Qt::UserRole).value<QList<QColor>>();
        if (stops.isEmpty())
            return;

        int scrollBarInset = 0;
        if (const QWidget* widget = option.widget) {
            if (const auto* itemView = qobject_cast<const QAbstractItemView*>(widget)) {
                if (itemView->verticalScrollBar() && itemView->verticalScrollBar()->isVisible()) {
                    scrollBarInset = itemView->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, itemView);
                }
            }
        }

        const QRect r = gradientPresetSwatchRect(option.rect, opt.direction, kGradientPresetTrailingInset - scrollBarInset);
        drawGradientSwatch(painter, r, stops);
    }
};

// Sidebar navigation button: uses a QVBoxLayout + stretch so the icon+text block
// is always vertically centered regardless of which Qt style is active.
class SectionButton : public QAbstractButton {
public:
    SectionButton(const QString& text, const QIcon& icon, QWidget* parent = nullptr)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(132);

        auto* iconLabel = new QLabel(this);
        iconLabel->setPixmap(icon.pixmap(28, 28));
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents);

        auto* textLabel = new QLabel(text, this);
        textLabel->setAlignment(Qt::AlignCenter);
        textLabel->setAttribute(Qt::WA_TransparentForMouseEvents);

        auto* vl = new QVBoxLayout(this);
        vl->setContentsMargins(4, 0, 4, 0);
        vl->setSpacing(4);
        vl->addStretch();
        vl->addWidget(iconLabel);
        vl->addWidget(textLabel);
        vl->addStretch();
    }

    QSize sizeHint() const override { return {minimumWidth(), 72}; }
    QSize minimumSizeHint() const override { return {80, 60}; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QColor highlight = palette().color(QPalette::Highlight);
        if (isChecked()) {
            QColor bg = highlight;
            bg.setAlphaF(0.20);
            p.fillRect(rect(), bg);
            p.fillRect(0, 0, 3, height(), highlight);
        } else if (underMouse()) {
            QColor bg = highlight;
            bg.setAlphaF(0.10);
            p.fillRect(rect(), bg);
        }
    }
};

QDoubleSpinBox* SettingsDialog::createDoubleSpinBox(double min, double max, double step, int decimals)
{
    auto* spinBox = new QDoubleSpinBox();
    spinBox->setRange(min, max);
    spinBox->setSingleStep(step);
    spinBox->setDecimals(decimals);
    spinBox->setAccelerated(true);
    return spinBox;
}

QSlider* SettingsDialog::createPercentageSlider()
{
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 100);
    slider->setSingleStep(1);
    slider->setPageStep(10);
    return slider;
}

QSpinBox* SettingsDialog::createSpinBox(int min, int max, int step)
{
    auto* spinBox = new QSpinBox();
    spinBox->setRange(min, max);
    spinBox->setSingleStep(step);
    spinBox->setAccelerated(true);
    return spinBox;
}

QWidget* SettingsDialog::createFieldWithDescription(QWidget* field, const QString& description)
{
    auto* wrapper = new QWidget();
    auto* layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(field);

    if (!description.isEmpty()) {
        auto* text = new QLabel(description, wrapper);
        text->setWordWrap(true);
        layout->addWidget(text);
    }
    return wrapper;
}

QWidget* SettingsDialog::createFontControls(QFontComboBox* familyCombo, QSpinBox* sizeSpin,
                                           QCheckBox* boldCheck, QCheckBox* italicCheck) const
{
    auto* wrapper = new QWidget();
    auto* layout = new QHBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    familyCombo->setMinimumContentsLength(8);
    familyCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    familyCombo->setSizePolicy(QSizePolicy::Expanding, familyCombo->sizePolicy().verticalPolicy());
    layout->addWidget(familyCombo, 1);

    auto* sizeLabel = new QLabel(tr("Size"), wrapper);
    layout->addWidget(sizeLabel);
    layout->addWidget(sizeSpin);
    layout->addWidget(boldCheck);
    layout->addWidget(italicCheck);
    layout->addStretch();
    return wrapper;
}

QWidget* SettingsDialog::createPairedFieldWithDescription(QWidget* firstField, const QString& firstLabel,
                                                          QWidget* secondField, const QString& secondLabel,
                                                          const QString& description)
{
    auto* wrapper = new QWidget();
    auto* layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* fieldsLayout = new QHBoxLayout();
    fieldsLayout->setContentsMargins(0, 0, 0, 0);
    fieldsLayout->setSpacing(12);

    auto* firstLabelWidget = new QLabel(firstLabel, wrapper);
    auto* secondLabelWidget = new QLabel(secondLabel, wrapper);
    firstLabelWidget->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    secondLabelWidget->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    firstField->setSizePolicy(QSizePolicy::Expanding, firstField->sizePolicy().verticalPolicy());
    secondField->setSizePolicy(QSizePolicy::Expanding, secondField->sizePolicy().verticalPolicy());

    fieldsLayout->addWidget(firstLabelWidget);
    fieldsLayout->addWidget(firstField, 1);
    fieldsLayout->addSpacing(8);
    fieldsLayout->addWidget(secondLabelWidget);
    fieldsLayout->addWidget(secondField, 1);
    layout->addLayout(fieldsLayout);

    if (!description.isEmpty()) {
        auto* text = new QLabel(description, wrapper);
        text->setWordWrap(true);
        layout->addWidget(text);
    }
    return wrapper;
}

QWidget* SettingsDialog::createPageIntro(const QString& title, const QString& text)
{
    auto* wrapper = new QWidget();

    auto* layout = new QVBoxLayout(wrapper);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* titleLabel = new QLabel(title, wrapper);
    titleLabel->setWordWrap(true);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleLabel->setFont(titleFont);
    layout->addWidget(titleLabel);

    auto* bodyLabel = new QLabel(text, wrapper);
    bodyLabel->setWordWrap(true);
    layout->addWidget(bodyLabel);

    return wrapper;
}

static QScrollArea* createSettingsPageScrollArea(QWidget* page, QWidget* parent)
{
    auto* scrollArea = new SettingsPageScrollArea(parent);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(page);
    return scrollArea;
}

QGroupBox* SettingsDialog::createSectionGroup(const QString& title, const QString& text, QFormLayout* formLayout)
{
    auto* group = new QGroupBox(title);

    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(12, 16, 12, 12);
    layout->setSpacing(10);

    if (!text.isEmpty()) {
        auto* bodyLabel = new QLabel(text, group);
        bodyLabel->setWordWrap(true);
        layout->addWidget(bodyLabel);
    }

    formLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    formLayout->setFormAlignment(Qt::AlignTop);
    formLayout->setHorizontalSpacing(16);
    formLayout->setVerticalSpacing(12);
    layout->addLayout(formLayout);

    return group;
}

FileNode* SettingsDialog::createPreviewNode(NodeArena& arena, const QString& name, qint64 size,
                                            bool isDirectory, QRgb color, FileNode* parent)
{
    FileNode* node = arena.alloc();
    node->name = name;
    node->size = size;
    node->isDirectory = isDirectory;
    node->extKey = isDirectory ? 0 : ColorUtils::packFileExt(name);
    node->color = color;
    node->parent = parent;
    if (parent) {
        parent->children.push_back(node);
    }
    return node;
}

SettingsDialog::SettingsDialog(const TreemapSettings& currentSettings, QWidget* parent)
    : QDialog(parent)
    , m_workingSettings(currentSettings)
{
    setWindowTitle(tr("Settings"));
    resize(1120, 780);
    setMinimumSize(920, 640);
    setModal(true);

    m_headerHeight = createDoubleSpinBox(1.0, 200.0, 1.0, 0);
    m_headerFontFamily = new QFontComboBox(this);
    m_headerFontSize = createSpinBox(1, 48, 1);
    m_headerFontBold = new QCheckBox(tr("Bold"), this);
    m_headerFontItalic = new QCheckBox(tr("Italic"), this);
    m_fileFontFamily = new QFontComboBox(this);
    m_fileFontSize = createSpinBox(1, 48, 1);
    m_fileFontBold = new QCheckBox(tr("Bold"), this);
    m_fileFontItalic = new QCheckBox(tr("Italic"), this);
    m_colorThemeSelector = new QComboBox(this);
    m_colorThemeName = new QLineEdit(this);
    m_addColorThemeButton = new QPushButton(tr("New"), this);
    m_duplicateColorThemeButton = new QPushButton(tr("Duplicate"), this);
    m_removeColorThemeButton = new QPushButton(tr("Delete"), this);
    m_followSystemColorTheme = new QCheckBox(tr("Switch treemap theme with OS light/dark mode"), this);
    m_lightModeColorTheme = new QComboBox(this);
    m_darkModeColorTheme = new QComboBox(this);
    m_folderColorMode = new QComboBox(this);
    m_folderColorMode->addItem(tr("Single hue"), TreemapSettings::SingleHue);
    m_folderColorMode->addItem(tr("Distinct top-level folders"), TreemapSettings::DistinctTopLevel);
    m_folderColorMode->addItem(tr("Depth gradient"), TreemapSettings::DepthGradient);
    m_depthGradientPreset = new GradientPresetComboBox(this);
    m_depthGradientPreset->setItemDelegate(new GradientPresetDelegate(m_depthGradientPreset));
    for (const ColorUtils::DepthGradientPreset& preset : ColorUtils::depthGradientPresets()) {
        m_depthGradientPreset->addItem(preset.name);
        m_depthGradientPreset->setItemData(m_depthGradientPreset->count() - 1,
                                           QVariant::fromValue(preset.stops), Qt::UserRole);
    }
    m_depthGradientFlipped = new QCheckBox(tr("Reverse"), this);
    m_folderBaseColorButton = new QPushButton(this);
    m_folderColorSaturation = createPercentageSlider();
    m_folderColorBrightness = createPercentageSlider();
    m_folderDepthBrightnessMode = new QComboBox(this);
    m_folderDepthBrightnessMode->addItem(tr("Darken"), TreemapSettings::DarkenPerLevel);
    m_folderDepthBrightnessMode->addItem(tr("Lighten"), TreemapSettings::LightenPerLevel);
    m_folderColorDarkenPerLevel = createDoubleSpinBox(0.0, 30.0, 0.5, 1);
    m_fileColorSaturation = createPercentageSlider();
    m_fileColorBrightness = createPercentageSlider();
    m_border = createDoubleSpinBox(0.0, 20.0, 1.0, 0);
    m_borderStyle = new QComboBox(this);
    m_borderStyle->addItem(tr("Darken"), TreemapSettings::DarkenBorder);
    m_borderStyle->addItem(tr("Lighten"), TreemapSettings::LightenBorder);
    m_borderStyle->addItem(tr("Automatic"), TreemapSettings::AutomaticBorder);
    m_borderIntensity = createPercentageSlider();
    m_highlightColorButton = new QPushButton(this);
    m_freeSpaceColorButton = new QPushButton(this);
    m_highlightOpacity = createPercentageSlider();
    m_folderPadding = createDoubleSpinBox(0.0, 40.0, 1.0, 0);
    m_baseVisibleDepth = new QSlider(Qt::Horizontal, this);
    m_baseVisibleDepth->setRange(0, 20);
    m_baseVisibleDepth->setSingleStep(1);
    m_baseVisibleDepth->setPageStep(5);
    m_baseVisibleDepthValue = new QLabel(this);
    m_baseVisibleDepthValue->setMinimumWidth(24);
    m_baseVisibleDepthValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_depthRevealPerZoomDoubling = createDoubleSpinBox(0.0, 8.0, 0.25, 2);
    m_tileAspectBias = new QSlider(Qt::Horizontal, this);
    m_tileAspectBias->setRange(-10, 10);
    m_tileAspectBias->setSingleStep(1);
    m_tileAspectBias->setPageStep(2);
    m_tileAspectBiasValue = new QLabel(this);
    m_tileAspectBiasValue->setMinimumWidth(36);
    m_tileAspectBiasValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_minTileSize = createDoubleSpinBox(1.0, 100.0, 1.0, 0);
    m_minPaint = createDoubleSpinBox(1.0, 50.0, 1.0, 0);
    m_minRevealHeight = createDoubleSpinBox(1.0, 1000.0, 1.0, 0);
    m_minRevealWidth = createDoubleSpinBox(1.0, 1000.0, 1.0, 0);
    m_revealFadeHeight = createDoubleSpinBox(0.0, 1000.0, 1.0, 0);
    m_revealFadeWidth = createDoubleSpinBox(0.0, 1000.0, 1.0, 0);
    m_zoomDurationMs = createSpinBox(0, 5000, 10);
    m_cameraDurationMs = createSpinBox(0, 5000, 10);
    m_simpleTooltips = new QCheckBox(tr("Use smaller, simpler tooltips in the treemap"));
    m_cameraMaxScale = new QSlider(Qt::Horizontal, this);
    m_cameraMaxScale->setRange(1, 512);
    m_cameraMaxScale->setSingleStep(1);
    m_cameraMaxScale->setPageStep(16);
    m_cameraMaxScaleValue = new QLabel(this);
    m_cameraMaxScaleValue->setMinimumWidth(32);
    m_cameraMaxScaleValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_liveScanPreview = new QCheckBox(tr("Enable live updates while scanning (slightly slower)"));
    m_excludedPathEdit = new QLineEdit(this);
    m_excludedPathEdit->setPlaceholderText(tr("/var/lib/docker"));
    m_excludedPathsList = new QListWidget(this);
    m_excludedPathsList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_randomColorForUnknownFiles = new QCheckBox(tr("Use random colours for unspecified file types"), this);
    m_unknownFileTypeColorButton = new QPushButton(this);
    m_unknownFileTypeColorButton->setMinimumWidth(100);
    m_unknownFileTypeOpacity = createPercentageSlider();

    const auto installPercentageTooltip = [this](QSlider* slider) {
        const auto updateTooltip = [this, slider](int value) {
            const QString text = tr("%1%").arg(value);
            slider->setToolTip(text);
            if (slider->isSliderDown()) {
                QToolTip::showText(slider->mapToGlobal(slider->rect().center()), text, slider);
            }
        };
        connect(slider, &QSlider::valueChanged, this, updateTooltip);
        updateTooltip(slider->value());
    };
    installPercentageTooltip(m_folderColorSaturation);
    installPercentageTooltip(m_folderColorBrightness);
    installPercentageTooltip(m_fileColorSaturation);
    installPercentageTooltip(m_fileColorBrightness);
    installPercentageTooltip(m_borderIntensity);
    installPercentageTooltip(m_highlightOpacity);
    installPercentageTooltip(m_unknownFileTypeOpacity);

    m_fileTypeGroupsList = new QListWidget(this);
    m_fileTypeGroupsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fileTypeGroupName = new QLineEdit(this);
    m_fileTypeGroupColorButton = new QPushButton(this);
    m_fileTypeGroupColorButton->setMinimumWidth(100);
    m_fileTypeGroupExtensions = new QPlainTextEdit(this);
    m_fileTypeGroupExtensions->setPlaceholderText(tr("png\njpg\ngif"));
    m_fileTypeGroupExtensions->setMinimumHeight(100);
    m_fileTypeGroupExtensions->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_previewWidget = new TreemapWidget(this);
    m_previewWidget->setMinimumHeight(220);
    m_previewWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_previewWidget->setWheelZoomEnabled(false);
    m_previewWidget->setAttribute(Qt::WA_TransparentForMouseEvents);
    buildPreviewTree();

    auto* appearancePage = new QWidget(this);
    auto* appearanceLayout = new QVBoxLayout(appearancePage);
    appearanceLayout->setContentsMargins(4, 10, 0, 10);
    appearanceLayout->setSpacing(16);
    appearanceLayout->addWidget(createPageIntro(
        tr("Adjust appearance"),
        tr("Control tile shape, labels, and motion.")));

    auto* appearanceColumns = new QHBoxLayout();
    appearanceColumns->setSpacing(16);

    auto* appearanceControls = new QWidget(appearancePage);
    auto* appearanceControlsLayout = new QVBoxLayout(appearanceControls);
    appearanceControlsLayout->setContentsMargins(0, 0, 0, 0);
    appearanceControlsLayout->setSpacing(16);

    auto* tileAspectBiasRow = new QWidget(this);
    auto* tileAspectBiasLayout = new QHBoxLayout(tileAspectBiasRow);
    tileAspectBiasLayout->setContentsMargins(0, 0, 0, 0);
    tileAspectBiasLayout->setSpacing(12);
    tileAspectBiasLayout->addWidget(m_tileAspectBias, 1);
    tileAspectBiasLayout->addWidget(m_tileAspectBiasValue);

    auto* chromeForm = new QFormLayout();
    chromeForm->addRow(tr("Header height"),
                       createFieldWithDescription(m_headerHeight, QString()));
    chromeForm->addRow(tr("Header font"),
                       createFieldWithDescription(
                           createFontControls(m_headerFontFamily, m_headerFontSize, m_headerFontBold, m_headerFontItalic),
                           QString()));
    chromeForm->addRow(tr("File font"),
                       createFieldWithDescription(
                           createFontControls(m_fileFontFamily, m_fileFontSize, m_fileFontBold, m_fileFontItalic),
                           QString()));
    chromeForm->addRow(tr("Border"),
                       createFieldWithDescription(m_border, QString()));
    chromeForm->addRow(tr("Folder padding"),
                       createFieldWithDescription(m_folderPadding, QString()));
    chromeForm->addRow(tr("Tile shape"),
                       createFieldWithDescription(tileAspectBiasRow,
                           tr("Biases tiles toward horizontal (\xe2\x86\x90) or vertical (\xe2\x86\x92). "
                           "0 produces the squarest tiles for the available area.")));
    appearanceControlsLayout->addWidget(createSectionGroup(
        tr("Tile appearance"),
        tr("Set tile shape, borders, spacing, and fonts."),
        chromeForm));

    auto* motionForm = new QFormLayout();
    motionForm->addRow(tr("Zoom duration (ms)"),
                       createFieldWithDescription(m_zoomDurationMs,
                           tr("Length of the folder-to-folder zoom animation.")));
    motionForm->addRow(tr("Camera duration (ms)"),
                       createFieldWithDescription(m_cameraDurationMs,
                           tr("Length of wheel-zoom camera motion. Set to zero for instant movement.")));
    appearanceControlsLayout->addWidget(createSectionGroup(
        tr("Animation timing"),
        tr("Set zoom and camera animation speed."),
        motionForm));

    auto* tooltipForm = new QFormLayout();
    tooltipForm->addRow(m_simpleTooltips);
    appearanceControlsLayout->addWidget(createSectionGroup(
        tr("Tooltip appearance"),
        QString(),
        tooltipForm));
    appearanceControlsLayout->addStretch();

    appearanceColumns->addWidget(appearanceControls);
    appearanceLayout->addLayout(appearanceColumns);
    appearanceLayout->addStretch();

    auto* colorsPage = new QWidget(this);
    auto* colorsLayout = new QVBoxLayout(colorsPage);
    colorsLayout->setContentsMargins(4, 10, 0, 10);
    colorsLayout->setSpacing(16);
    colorsLayout->addWidget(createPageIntro(
        tr("Adjust colours"),
        tr("Manage themes and set treemap colours.")));

    auto* colorsColumns = new QHBoxLayout();
    colorsColumns->setSpacing(16);

    auto* colorsControls = new QWidget(colorsPage);
    auto* colorsControlsLayout = new QVBoxLayout(colorsControls);
    colorsControlsLayout->setContentsMargins(0, 0, 0, 0);
    colorsControlsLayout->setSpacing(16);

    auto* themePickerRow = new QWidget(colorsPage);
    auto* themePickerLayout = new QHBoxLayout(themePickerRow);
    themePickerLayout->setContentsMargins(0, 0, 0, 0);
    themePickerLayout->setSpacing(8);
    themePickerLayout->addWidget(m_colorThemeSelector, 1);
    themePickerLayout->addWidget(m_addColorThemeButton);
    themePickerLayout->addWidget(m_duplicateColorThemeButton);
    themePickerLayout->addWidget(m_removeColorThemeButton);

    auto* themeForm = new QFormLayout();
    themeForm->addRow(tr("Editing theme"),
                      createFieldWithDescription(
                          themePickerRow,
                          QString()));
    themeForm->addRow(tr("Theme name"),
                      createFieldWithDescription(
                          m_colorThemeName,
                          QString()));
    themeForm->addRow(tr("Automatic switching"),
                      createFieldWithDescription(
                          m_followSystemColorTheme,
                          QString()));
    themeForm->addRow(tr("Mode defaults"),
                      createPairedFieldWithDescription(
                          m_lightModeColorTheme, tr("Light mode"),
                          m_darkModeColorTheme, tr("Dark mode"),
                          QString()));
    colorsControlsLayout->addWidget(createSectionGroup(
        tr("Saved themes"),
        tr("Choose, rename, duplicate, and switch themes."),
        themeForm));

    auto* colorForm = new QFormLayout();
    colorForm->addRow(tr("Folder colour mode"),
                      createFieldWithDescription(
                          m_folderColorMode,
                          QString()));
    auto* presetInner = new QWidget();
    auto* presetInnerLayout = new QHBoxLayout(presetInner);
    presetInnerLayout->setContentsMargins(0, 0, 0, 0);
    presetInnerLayout->setSpacing(8);
    presetInnerLayout->addWidget(m_depthGradientPreset, 1);
    presetInnerLayout->addWidget(m_depthGradientFlipped);
    auto* depthGradientPresetWrapper = createFieldWithDescription(presetInner, QString());
    colorForm->addRow(tr("Gradient preset"), depthGradientPresetWrapper);
    colorForm->setRowVisible(depthGradientPresetWrapper, false);
    auto* folderBaseColorWrapper = createFieldWithDescription(
        m_folderBaseColorButton,
        QString());
    colorForm->addRow(tr("Folder base colour"), folderBaseColorWrapper);
    colorForm->setRowVisible(folderBaseColorWrapper, false);
    auto* satBrightWrapper = createPairedFieldWithDescription(
        m_folderColorSaturation, tr("Saturation %"),
        m_folderColorBrightness, tr("Brightness %"),
        QString());
    colorForm->addRow(tr("Folder colour"), satBrightWrapper);
    auto* depthChangeWrapper = createPairedFieldWithDescription(
        m_folderDepthBrightnessMode, tr("Mode"),
        m_folderColorDarkenPerLevel, tr("Amount %"),
        tr("Choose whether deeper folders darken or lighten, and by how much per level."));
    colorForm->addRow(tr("Folder depth change"), depthChangeWrapper);
    colorForm->addRow(tr("File colour"),
                      createPairedFieldWithDescription(
                          m_fileColorSaturation, tr("Saturation %"),
                          m_fileColorBrightness, tr("Brightness %"),
                          QString()));
    colorForm->addRow(tr("Unspecified types"),
                      createPairedFieldWithDescription(
                          m_unknownFileTypeColorButton, tr("Colour"),
                          m_unknownFileTypeOpacity, tr("Opacity %"),
                          tr("Colour used for file types not matching any group (when random is off).")));
    colorForm->addRow(tr("Border"),
                      createPairedFieldWithDescription(
                          m_borderStyle, tr("Style"),
                          m_borderIntensity, tr("Intensity %"),
                          QString()));
    colorForm->addRow(tr("Highlight"),
                      createPairedFieldWithDescription(
                          m_highlightColorButton, tr("Colour"),
                          m_highlightOpacity, tr("Opacity %"),
                          QString()));
    colorForm->addRow(tr("Free space"),
                      createFieldWithDescription(
                          m_freeSpaceColorButton,
                          QString()));
    colorsControlsLayout->addWidget(createSectionGroup(
        tr("Treemap colours"),
        tr("Set folder, file, border, highlight, and free-space colours."),
        colorForm));
    colorsControlsLayout->addStretch();

    colorsColumns->addWidget(colorsControls);
    colorsLayout->addLayout(colorsColumns);
    colorsLayout->addStretch();

    auto* performancePage = new QWidget(this);
    auto* performanceLayout = new QVBoxLayout(performancePage);
    performanceLayout->setContentsMargins(4, 10, 0, 10);
    performanceLayout->setSpacing(16);
    performanceLayout->addWidget(createPageIntro(
        tr("Adjust performance"),
        tr("Control scan updates, visibility thresholds, and zoom limits.")));

    auto* renderingForm = new QFormLayout();
    renderingForm->addRow(createFieldWithDescription(m_liveScanPreview, QString()));
    performanceLayout->addWidget(createSectionGroup(
        tr("Scan updates"),
        QString(),
        renderingForm));

    auto* densityForm = new QFormLayout();
    auto* visibleDepthRow = new QWidget(this);
    auto* visibleDepthLayout = new QHBoxLayout(visibleDepthRow);
    visibleDepthLayout->setContentsMargins(0, 0, 0, 0);
    visibleDepthLayout->setSpacing(12);
    visibleDepthLayout->addWidget(m_baseVisibleDepth, 1);
    visibleDepthLayout->addWidget(m_baseVisibleDepthValue);
    densityForm->addRow(tr("Visible depth"),
                        createFieldWithDescription(visibleDepthRow,
                            tr("How many folder levels are shown before zoom starts revealing more.")));
    densityForm->addRow(tr("Depth reveal rate"),
                        createFieldWithDescription(
                            m_depthRevealPerZoomDoubling,
                            tr("How many extra depth levels become visible each time zoom doubles.")));
    densityForm->addRow(tr("Fully reveal children"),
                        createPairedFieldWithDescription(
                            m_minRevealWidth, tr("Width"),
                            m_minRevealHeight, tr("Height"),
                            tr("Child tiles are fully visible once the folder reaches these dimensions.")));
    densityForm->addRow(tr("Reveal fade distance"),
                        createPairedFieldWithDescription(
                            m_revealFadeWidth, tr("Width"),
                            m_revealFadeHeight, tr("Height"),
                            tr("How far below the full-reveal size children start fading in. For example, 20 means reveal begins 20 pixels earlier.")));
    densityForm->addRow(tr("Tiny tile size"),
                        createFieldWithDescription(m_minTileSize,
                            tr("Small files at or above this size still render as tiny tiles instead of disappearing into the minimum paint cutoff.")));
    densityForm->addRow(tr("Minimum size"),
                        createFieldWithDescription(m_minPaint,
                            tr("Tiles smaller than this in either dimension are skipped entirely.")));
    performanceLayout->addWidget(createSectionGroup(
        tr("Child visibility"),
        tr("Set when folder details and child tiles appear."),
        densityForm));

    auto* navigationForm = new QFormLayout();
    auto* cameraMaxScaleRow = new QWidget(this);
    auto* cameraMaxScaleLayout = new QHBoxLayout(cameraMaxScaleRow);
    cameraMaxScaleLayout->setContentsMargins(0, 0, 0, 0);
    cameraMaxScaleLayout->setSpacing(12);
    cameraMaxScaleLayout->addWidget(m_cameraMaxScale, 1);
    cameraMaxScaleLayout->addWidget(m_cameraMaxScaleValue);
    navigationForm->addRow(tr("Camera max scale"),
                           createFieldWithDescription(cameraMaxScaleRow,
                               tr("Maximum allowed camera zoom level.")));
    performanceLayout->addWidget(createSectionGroup(
        tr("Navigation"),
        tr("Set the maximum camera zoom level."),
        navigationForm));

    auto* exclusionsPage = new QWidget(this);
    auto* exclusionsLayout = new QVBoxLayout(exclusionsPage);
    exclusionsLayout->setContentsMargins(4, 10, 0, 10);
    exclusionsLayout->setSpacing(16);
    exclusionsLayout->addWidget(createPageIntro(
        tr("Manage exclusions"),
        tr("Choose which paths to skip during scans.")));

    auto* excludedPathsGroup = new QGroupBox(tr("Excluded paths"), exclusionsPage);
    auto* excludedPathsLayout = new QVBoxLayout(excludedPathsGroup);
    excludedPathsLayout->setContentsMargins(12, 16, 12, 12);
    excludedPathsLayout->setSpacing(10);

    auto* bodyLabel = new QLabel(
        tr("Use absolute paths such as /var/lib/docker or /mnt/archive. Matching is recursive, so descendants are excluded too. When scanning /, virtual system paths like /proc are already skipped automatically."),
        excludedPathsGroup);
    bodyLabel->setWordWrap(true);
    excludedPathsLayout->addWidget(bodyLabel);

    auto* inputRow = new QHBoxLayout();
    inputRow->setSpacing(8);
    inputRow->addWidget(m_excludedPathEdit, 1);

    auto* addExcludedPathButton = new QPushButton(tr("Add"), excludedPathsGroup);
    inputRow->addWidget(addExcludedPathButton);
    excludedPathsLayout->addLayout(inputRow);
    excludedPathsLayout->addWidget(m_excludedPathsList, 1);

    auto* removeExcludedPathButton = new QPushButton(tr("Remove selected"), excludedPathsGroup);
    excludedPathsLayout->addWidget(removeExcludedPathButton, 0, Qt::AlignLeft);
    exclusionsLayout->addWidget(excludedPathsGroup, 1);

    auto* freeSpaceGroup = new QGroupBox(tr("Free space"), exclusionsPage);
    auto* freeSpaceGroupLayout = new QVBoxLayout(freeSpaceGroup);
    freeSpaceGroupLayout->setContentsMargins(12, 16, 12, 12);
    m_hideNonLocalFreeSpace = new QCheckBox(
        tr("Hide free space for network filesystems (NFS, SMB, etc.)"), freeSpaceGroup);
    freeSpaceGroupLayout->addWidget(m_hideNonLocalFreeSpace);
    exclusionsLayout->addWidget(freeSpaceGroup);

    exclusionsLayout->addStretch();

    connect(addExcludedPathButton, &QPushButton::clicked, this, &SettingsDialog::addExcludedPath);
    connect(removeExcludedPathButton, &QPushButton::clicked, this, &SettingsDialog::removeSelectedExcludedPaths);
    connect(m_excludedPathEdit, &QLineEdit::returnPressed, this, &SettingsDialog::addExcludedPath);
    connect(m_addColorThemeButton, &QPushButton::clicked, this, &SettingsDialog::addColorTheme);
    connect(m_duplicateColorThemeButton, &QPushButton::clicked, this, &SettingsDialog::duplicateColorTheme);
    connect(m_removeColorThemeButton, &QPushButton::clicked, this, &SettingsDialog::removeSelectedColorTheme);
    connect(m_colorThemeSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_updatingColorThemeUi) {
            return;
        }
        if (m_colorThemeSelector->count() == 0) {
            return;
        }
        storeSelectedColorThemeInto(m_workingSettings);
        m_selectedColorThemeId = m_colorThemeSelector->currentData().toString();
        loadSelectedColorThemeIntoFields();
        refreshPreview();
    });
    connect(m_colorThemeName, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_updatingColorThemeUi) {
            return;
        }
        TreemapColorTheme* theme = m_workingSettings.findColorTheme(m_colorThemeSelector->currentData().toString());
        if (!theme || TreemapColorTheme::isBuiltInId(theme->id)) {
            return;
        }
        theme->name = text.trimmed();
        populateColorThemeSelectors();
        updateColorThemeEditorState();
        refreshPreview();
    });
    connect(m_followSystemColorTheme, &QCheckBox::toggled, this, &SettingsDialog::refreshPreview);
    connect(m_lightModeColorTheme, qOverload<int>(&QComboBox::currentIndexChanged), this, &SettingsDialog::refreshPreview);
    connect(m_darkModeColorTheme, qOverload<int>(&QComboBox::currentIndexChanged), this, &SettingsDialog::refreshPreview);
    connect(m_headerHeight, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_headerFontFamily, &QFontComboBox::currentFontChanged, this, &SettingsDialog::refreshPreview);
    connect(m_headerFontSize, qOverload<int>(&QSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_headerFontBold, &QCheckBox::toggled, this, &SettingsDialog::refreshPreview);
    connect(m_headerFontItalic, &QCheckBox::toggled, this, &SettingsDialog::refreshPreview);
    connect(m_fileFontFamily, &QFontComboBox::currentFontChanged, this, &SettingsDialog::refreshPreview);
    connect(m_fileFontSize, qOverload<int>(&QSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_fileFontBold, &QCheckBox::toggled, this, &SettingsDialog::refreshPreview);
    connect(m_fileFontItalic, &QCheckBox::toggled, this, &SettingsDialog::refreshPreview);
    connect(m_folderColorMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, colorForm, satBrightWrapper, depthChangeWrapper,
             depthGradientPresetWrapper, folderBaseColorWrapper](int) {
        const int mode = m_folderColorMode->currentData().toInt();
        const bool isSingleHue = mode == TreemapSettings::SingleHue;
        const bool isDepthGradient = mode == TreemapSettings::DepthGradient;
        colorForm->setRowVisible(depthGradientPresetWrapper, !isSingleHue);
        colorForm->setRowVisible(folderBaseColorWrapper, isSingleHue);
        colorForm->setRowVisible(satBrightWrapper, !isSingleHue);
        colorForm->setRowVisible(depthChangeWrapper, !isDepthGradient);
        refreshPreview();
    });
    connect(m_depthGradientPreset, qOverload<int>(&QComboBox::currentIndexChanged), this, &SettingsDialog::refreshPreview);
    connect(m_depthGradientFlipped, &QCheckBox::toggled, this, &SettingsDialog::refreshPreview);
    connect(m_folderBaseColorButton, &QPushButton::clicked, this, [this]() {
        const QColor current = m_folderBaseColorButton->property("selectedColor").value<QColor>();
        const QColor color = QColorDialog::getColor(current.isValid() ? current : Qt::blue, this, tr("Choose folder base colour"));
        if (!color.isValid()) {
            return;
        }
        m_folderBaseColorButton->setProperty("selectedColor", color);
        updateFolderBaseColorButton();
        refreshPreview();
    });
    connect(m_folderColorSaturation, &QSlider::valueChanged, this, &SettingsDialog::refreshPreview);
    connect(m_folderColorBrightness, &QSlider::valueChanged, this, &SettingsDialog::refreshPreview);
    connect(m_folderDepthBrightnessMode, qOverload<int>(&QComboBox::currentIndexChanged), this, &SettingsDialog::refreshPreview);
    connect(m_folderColorDarkenPerLevel, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_fileColorSaturation, &QSlider::valueChanged, this, &SettingsDialog::refreshPreview);
    connect(m_fileColorBrightness, &QSlider::valueChanged, this, &SettingsDialog::refreshPreview);
    connect(m_border, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_borderStyle, qOverload<int>(&QComboBox::currentIndexChanged), this, &SettingsDialog::refreshPreview);
    connect(m_borderIntensity, &QSlider::valueChanged, this, &SettingsDialog::refreshPreview);
    connect(m_highlightColorButton, &QPushButton::clicked, this, [this]() {
        TreemapSettings currentSettings = settings();
        const QColor color = QColorDialog::getColor(currentSettings.highlightColor, this, tr("Choose highlight colour"));
        if (!color.isValid()) {
            return;
        }
        m_highlightColorButton->setProperty("selectedColor", color);
        updateHighlightColorButton();
        refreshPreview();
    });
    connect(m_freeSpaceColorButton, &QPushButton::clicked, this, [this]() {
        TreemapSettings currentSettings = settings();
        const QColor color = QColorDialog::getColor(currentSettings.freeSpaceColor, this, tr("Choose free space colour"));
        if (!color.isValid()) {
            return;
        }
        m_freeSpaceColorButton->setProperty("selectedColor", color);
        updateFreeSpaceColorButton();
        refreshPreview();
    });
    connect(m_unknownFileTypeColorButton, &QPushButton::clicked, this, [this]() {
        const QColor current = m_unknownFileTypeColorButton->property("selectedColor").value<QColor>();
        const QColor color = QColorDialog::getColor(
            current.isValid() ? current : Qt::gray, this, tr("Choose colour for unspecified types"));
        if (!color.isValid()) {
            return;
        }
        m_unknownFileTypeColorButton->setProperty("selectedColor", color);
        updateUnknownFileTypeColorButton();
        refreshPreview();
    });
    connect(m_unknownFileTypeOpacity, &QSlider::valueChanged, this, &SettingsDialog::refreshPreview);
    connect(m_highlightOpacity, &QSlider::valueChanged, this, &SettingsDialog::refreshPreview);
    connect(m_folderPadding, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_tileAspectBias, &QSlider::valueChanged, this, [this](int value) {
        m_tileAspectBiasValue->setText(QString::number(value / 10.0, 'f', 1));
        refreshPreview();
    });
    connect(m_baseVisibleDepth, &QSlider::valueChanged, this, [this](int value) {
        m_baseVisibleDepthValue->setText(QString::number(value));
        refreshPreview();
    });
    connect(m_depthRevealPerZoomDoubling, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_minTileSize, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_minPaint, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_minRevealHeight, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_minRevealWidth, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_revealFadeHeight, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_revealFadeWidth, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SettingsDialog::refreshPreview);
    connect(m_simpleTooltips, &QCheckBox::toggled, this, &SettingsDialog::refreshPreview);
    connect(m_cameraMaxScale, &QSlider::valueChanged, this, [this](int value) {
        m_cameraMaxScaleValue->setText(QString::number(value));
        refreshPreview();
    });
    auto* appearanceScroll = createSettingsPageScrollArea(appearancePage, this);
    auto* appearanceContainer = new QWidget(this);
    auto* appearanceContainerLayout = new QHBoxLayout(appearanceContainer);
    appearanceContainerLayout->setContentsMargins(0, 0, 0, 0);
    appearanceContainerLayout->setSpacing(8);
    appearanceContainerLayout->addWidget(appearanceScroll, 1);

    auto* colorsScroll = createSettingsPageScrollArea(colorsPage, this);
    auto* colorsContainer = new QWidget(this);
    auto* colorsContainerLayout = new QHBoxLayout(colorsContainer);
    colorsContainerLayout->setContentsMargins(0, 0, 0, 0);
    colorsContainerLayout->setSpacing(8);
    colorsContainerLayout->addWidget(colorsScroll, 1);

    auto* performanceScroll = createSettingsPageScrollArea(performancePage, this);
    auto* performanceContainer = new QWidget(this);
    auto* performanceContainerLayout = new QHBoxLayout(performanceContainer);
    performanceContainerLayout->setContentsMargins(0, 0, 0, 0);
    performanceContainerLayout->setSpacing(8);
    performanceContainerLayout->addWidget(performanceScroll, 1);
    auto* exclusionsScroll = createSettingsPageScrollArea(exclusionsPage, this);

    // ── File Types page ──────────────────────────────────────────────────────
    // Groups list (left of split)
    auto* ftGroupsBox = new QGroupBox(tr("File type groups"), this);
    auto* ftGroupsLayout = new QVBoxLayout(ftGroupsBox);
    ftGroupsLayout->setContentsMargins(12, 16, 12, 12);
    ftGroupsLayout->setSpacing(8);
    ftGroupsLayout->addWidget(m_fileTypeGroupsList, 1);
    auto* ftGroupButtons = new QHBoxLayout();
    ftGroupButtons->setSpacing(8);
    auto* addFileTypeGroupButton = new QPushButton(tr("Add"), ftGroupsBox);
    auto* removeFileTypeGroupButton = new QPushButton(tr("Remove selected"), ftGroupsBox);
    ftGroupButtons->addWidget(addFileTypeGroupButton);
    ftGroupButtons->addWidget(removeFileTypeGroupButton);
    ftGroupButtons->addStretch();
    ftGroupsLayout->addLayout(ftGroupButtons);

    // Edit panel (right of split)
    m_fileTypeGroupEditBox = new QGroupBox(tr("Edit group"), this);
    auto* ftEditOuter = new QVBoxLayout(m_fileTypeGroupEditBox);
    ftEditOuter->setContentsMargins(12, 16, 12, 12);
    ftEditOuter->setSpacing(8);
    auto* ftEditForm = new QFormLayout();
    ftEditForm->setSpacing(8);
    ftEditForm->addRow(tr("Name"), m_fileTypeGroupName);
    ftEditForm->addRow(tr("Colour"), m_fileTypeGroupColorButton);
    ftEditOuter->addLayout(ftEditForm);
    ftEditOuter->addWidget(new QLabel(tr("Extensions"), m_fileTypeGroupEditBox));
    ftEditOuter->addWidget(m_fileTypeGroupExtensions, 1);
    auto* extLabel = new QLabel(tr("One per line (or comma-separated)"), m_fileTypeGroupEditBox);
    extLabel->setEnabled(false);
    ftEditOuter->addWidget(extLabel);

    // Additional options (below the split, full width)
    auto* additionalOptionsBox = new QGroupBox(tr("Additional options"), this);
    auto* additionalOptionsLayout = new QVBoxLayout(additionalOptionsBox);
    additionalOptionsLayout->setContentsMargins(12, 16, 12, 12);
    additionalOptionsLayout->setSpacing(8);
    additionalOptionsLayout->addWidget(m_randomColorForUnknownFiles);
    auto* unspecifiedNote = new QLabel(
        tr("When disabled, use the <b>Unspecified types</b> colour on the Colours page."), additionalOptionsBox);
    unspecifiedNote->setWordWrap(true);
    additionalOptionsLayout->addWidget(unspecifiedNote);

    // Page: intro + horizontal split + additional options
    auto* fileTypesPage = new QWidget(this);
    auto* fileTypesPageLayout = new QVBoxLayout(fileTypesPage);
    fileTypesPageLayout->setContentsMargins(4, 10, 0, 10);
    fileTypesPageLayout->setSpacing(16);
    fileTypesPageLayout->addWidget(createPageIntro(
        tr("File type colours"),
        tr("Assign exact colours to groups of file extensions.")));
    auto* ftSplit = new QHBoxLayout();
    ftSplit->setSpacing(8);
    ftSplit->addWidget(ftGroupsBox, 1);
    ftSplit->addWidget(m_fileTypeGroupEditBox, 1);
    fileTypesPageLayout->addLayout(ftSplit, 1);
    fileTypesPageLayout->addWidget(additionalOptionsBox);

    auto* fileTypesContainer = new QWidget(this);
    auto* fileTypesContainerLayout = new QHBoxLayout(fileTypesContainer);
    fileTypesContainerLayout->setContentsMargins(0, 0, 0, 0);
    fileTypesContainerLayout->setSpacing(8);
    fileTypesContainerLayout->addWidget(fileTypesPage, 1);

    connect(addFileTypeGroupButton, &QPushButton::clicked, this, &SettingsDialog::addFileTypeGroup);
    connect(removeFileTypeGroupButton, &QPushButton::clicked, this, &SettingsDialog::removeSelectedFileTypeGroup);
    connect(m_fileTypeGroupsList, &QListWidget::currentRowChanged,
            this, &SettingsDialog::onFileTypeGroupSelectionChanged);
    connect(m_fileTypeGroupColorButton, &QPushButton::clicked, this, [this]() {
        const QColor current = m_fileTypeGroupColorButton->property("selectedColor").value<QColor>();
        const QColor color = QColorDialog::getColor(
            current.isValid() ? current : Qt::blue, this, tr("Choose colour"));
        if (!color.isValid()) {
            return;
        }
        m_fileTypeGroupColorButton->setProperty("selectedColor", color);
        updateFileTypeGroupColorButton();
        syncFileTypeGroupFromFields(m_fileTypeGroupsList->currentRow());
    });
    connect(m_fileTypeGroupName, &QLineEdit::textChanged, this, [this](const QString&) {
        syncFileTypeGroupFromFields(m_fileTypeGroupsList->currentRow());
    });
    connect(m_fileTypeGroupExtensions, &QPlainTextEdit::textChanged, this, [this]() {
        syncFileTypeGroupFromFields(m_fileTypeGroupsList->currentRow());
    });

    auto* sectionNav = new QWidget(this);
    sectionNav->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    sectionNav->setMinimumWidth(132);
    sectionNav->setMaximumWidth(132);
    auto* sectionNavLayout = new QVBoxLayout(sectionNav);
    sectionNavLayout->setContentsMargins(0, 0, 0, 0);
    sectionNavLayout->setSpacing(4);

    const auto makeSectionIcon = [](std::initializer_list<const char*> iconNames, const QString& light, const QString& dark) {
        return IconUtils::themeIcon(iconNames, light, dark);
    };

    auto* pages = new QStackedWidget(this);
    pages->addWidget(appearanceContainer);
    pages->addWidget(colorsContainer);
    pages->addWidget(fileTypesContainer);
    pages->addWidget(performanceContainer);
    pages->addWidget(exclusionsScroll);
    connect(pages, &QStackedWidget::currentChanged, this, [pages](int index) {
        QWidget* page = pages->widget(index);
        auto* scrollArea = dynamic_cast<SettingsPageScrollArea*>(page);
        if (!scrollArea && page) {
            scrollArea = dynamic_cast<SettingsPageScrollArea*>(page->findChild<QScrollArea*>());
        }
        if (scrollArea) {
            QTimer::singleShot(0, scrollArea, [scrollArea]() {
                scrollArea->refreshViewportInset();
            });
        }
    });

    auto* sectionButtons = new QButtonGroup(this);
    sectionButtons->setExclusive(true);

    const auto addSectionButton = [&](const QString& label, const QIcon& icon, int index) {
        auto* button = new SectionButton(label, icon, sectionNav);
        button->setAutoExclusive(true);
        sectionNavLayout->addWidget(button);
        sectionButtons->addButton(button, index);
        connect(button, &SectionButton::clicked, this, [pages, index]() {
            pages->setCurrentIndex(index);
        });
        return button;
    };

    auto* appearanceButton = addSectionButton(
        tr("Appearance"),
        makeSectionIcon({"preferences-desktop-theme"},
            QStringLiteral(":/assets/tabler-icons/adjustments-horizontal.svg"),
            QStringLiteral(":/assets/tabler-icons/adjustments-horizontal.svg")),
        0);
    addSectionButton(
        tr("Colours"),
        makeSectionIcon({"color-management"},
            QStringLiteral(":/assets/tabler-icons/palette.svg"),
            QStringLiteral(":/assets/tabler-icons/palette.svg")),
        1);
    addSectionButton(
        tr("File Types"),
        makeSectionIcon({"text-x-generic"},
            QStringLiteral(":/assets/tabler-icons/files.svg"),
            QStringLiteral(":/assets/tabler-icons/files.svg")),
        2);
    addSectionButton(
        tr("Performance"),
        makeSectionIcon({"preferences-system-performance"},
            QStringLiteral(":/assets/tabler-icons/brand-speedtest.svg"),
            QStringLiteral(":/assets/tabler-icons/brand-speedtest.svg")),
        3);
    addSectionButton(
        tr("Exclusions"),
        makeSectionIcon({"folder-blacklist", "folder"},
            QStringLiteral(":/assets/tabler-icons/folders-off.svg"),
            QStringLiteral(":/assets/tabler-icons/folders-off.svg")),
        4);
    appearanceButton->setChecked(true);
    sectionNavLayout->addStretch(1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        Qt::Horizontal,
        this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this]() {
        restoreDefaultsPreservingCustomThemes();
    });

    applySettingsToFields(currentSettings);
    m_previewWidget->setRoot(m_previewRoot, {}, true, false);
    if (!m_previewRoot->children.empty()) {
        m_previewWidget->setPreviewHoveredNode(m_previewRoot->children.front());
    }
    refreshPreview();

    auto* previewFrame = new QFrame(this);
    previewFrame->setFrameShape(QFrame::StyledPanel);
    previewFrame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    previewFrame->setMinimumWidth(190);
    auto* previewLayout = new QVBoxLayout(previewFrame);
    previewLayout->setContentsMargins(8, 8, 8, 8);
    previewLayout->setSpacing(6);

    auto* previewTitle = new QLabel(tr("Preview"), previewFrame);
    QFont previewTitleFont = previewTitle->font();
    previewTitleFont.setBold(true);
    previewTitle->setFont(previewTitleFont);
    previewLayout->addWidget(previewTitle);
    previewLayout->addWidget(m_previewWidget, 1);

    auto* contentLayout = new QHBoxLayout();
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(6);
    contentLayout->addWidget(sectionNav);
    contentLayout->addWidget(pages, 1);
    contentLayout->addWidget(previewFrame);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);
    layout->addLayout(contentLayout, 1);
    layout->addWidget(buttons);
}

void SettingsDialog::populateColorThemeSelectors()
{
    m_workingSettings.ensureColorThemes();
    const QString currentThemeId = !m_selectedColorThemeId.isEmpty()
        ? m_selectedColorThemeId
        : m_colorThemeSelector->currentData().toString();
    const QString selectedThemeId = !currentThemeId.isEmpty()
        ? currentThemeId
        : m_workingSettings.activeColorThemeId;

    const QSignalBlocker blocker1(m_colorThemeSelector);
    const QSignalBlocker blocker2(m_lightModeColorTheme);
    const QSignalBlocker blocker3(m_darkModeColorTheme);

    m_colorThemeSelector->clear();
    m_lightModeColorTheme->clear();
    m_darkModeColorTheme->clear();
    for (const TreemapColorTheme& theme : m_workingSettings.colorThemes) {
        m_colorThemeSelector->addItem(theme.name, theme.id);
        m_lightModeColorTheme->addItem(theme.name, theme.id);
        m_darkModeColorTheme->addItem(theme.name, theme.id);
    }

    int activeIndex = m_colorThemeSelector->findData(selectedThemeId);
    if (activeIndex < 0) {
        activeIndex = m_colorThemeSelector->findData(m_workingSettings.activeColorThemeId);
    }
    if (activeIndex < 0) {
        activeIndex = 0;
    }
    m_colorThemeSelector->setCurrentIndex(activeIndex);
    m_selectedColorThemeId = m_colorThemeSelector->currentData().toString();

    int lightIndex = m_lightModeColorTheme->findData(m_workingSettings.lightModeColorThemeId);
    if (lightIndex < 0) {
        lightIndex = 0;
    }
    m_lightModeColorTheme->setCurrentIndex(lightIndex);

    int darkIndex = m_darkModeColorTheme->findData(m_workingSettings.darkModeColorThemeId);
    if (darkIndex < 0) {
        darkIndex = 0;
    }
    m_darkModeColorTheme->setCurrentIndex(darkIndex);
}

void SettingsDialog::loadSelectedColorThemeIntoFields()
{
    m_workingSettings.ensureColorThemes();
    m_selectedColorThemeId = m_colorThemeSelector->currentData().toString();
    const TreemapColorTheme* theme = m_workingSettings.findColorTheme(m_selectedColorThemeId);
    if (!theme) {
        return;
    }

    m_updatingColorThemeUi = true;
    m_colorThemeName->setText(theme->name);

    int folderColorModeIndex = m_folderColorMode->findData(theme->folderColorMode);
    if (folderColorModeIndex < 0) {
        folderColorModeIndex = 0;
    }
    m_folderColorMode->setCurrentIndex(folderColorModeIndex);
    {
        const QSignalBlocker b(m_depthGradientPreset);
        m_depthGradientPreset->setCurrentIndex(
            std::clamp(theme->depthGradientPreset, 0, m_depthGradientPreset->count() - 1));
    }
    m_depthGradientFlipped->setChecked(theme->depthGradientFlipped);
    m_folderColorSaturation->setValue(theme->folderColorSaturation * 100.0);
    m_folderColorBrightness->setValue(theme->folderColorBrightness * 100.0);
    m_folderBaseColorButton->setProperty("selectedColor", theme->folderBaseColor);
    updateFolderBaseColorButton();

    int folderDepthModeIndex = m_folderDepthBrightnessMode->findData(theme->folderDepthBrightnessMode);
    if (folderDepthModeIndex < 0) {
        folderDepthModeIndex = 0;
    }
    m_folderDepthBrightnessMode->setCurrentIndex(folderDepthModeIndex);
    m_folderColorDarkenPerLevel->setValue(theme->folderColorDarkenPerLevel * 100.0);
    m_fileColorSaturation->setValue(theme->fileColorSaturation * 100.0);
    m_fileColorBrightness->setValue(theme->fileColorBrightness * 100.0);
    int borderStyleIndex = m_borderStyle->findData(theme->borderStyle);
    if (borderStyleIndex < 0) {
        borderStyleIndex = m_borderStyle->findData(TreemapSettings::AutomaticBorder);
    }
    m_borderStyle->setCurrentIndex(std::max(0, borderStyleIndex));
    m_borderIntensity->setValue(theme->borderIntensity * 100.0);
    m_highlightColorButton->setProperty("selectedColor", theme->highlightColor);
    updateHighlightColorButton();
    m_freeSpaceColorButton->setProperty("selectedColor", theme->freeSpaceColor);
    updateFreeSpaceColorButton();
    m_unknownFileTypeColorButton->setProperty("selectedColor", theme->unknownFileTypeColor);
    updateUnknownFileTypeColorButton();
    m_highlightOpacity->setValue(theme->highlightOpacity * 100.0);
    m_unknownFileTypeOpacity->setValue(theme->unknownFileTypeOpacity * 100.0);
    m_updatingColorThemeUi = false;
    updateColorThemeEditorState();
}

void SettingsDialog::storeSelectedColorThemeInto(TreemapSettings& settings) const
{
    const QString themeId = !m_selectedColorThemeId.isEmpty()
        ? m_selectedColorThemeId
        : m_colorThemeSelector->currentData().toString();
    TreemapColorTheme* theme = settings.findColorTheme(themeId);
    if (!theme) {
        return;
    }

    if (!TreemapColorTheme::isBuiltInId(theme->id)) {
        theme->name = m_colorThemeName->text().trimmed();
    }
    theme->folderColorMode = m_folderColorMode->currentData().toInt();
    theme->depthGradientPreset = m_depthGradientPreset->currentIndex();
    theme->depthGradientFlipped = m_depthGradientFlipped->isChecked();
    if (theme->folderColorMode == TreemapSettings::SingleHue) {
        const QColor c = m_folderBaseColorButton->property("selectedColor").value<QColor>();
        theme->folderBaseColor = c.isValid() ? c : QColor::fromHslF(0.58, 0.5, 0.45);
    } else {
        theme->folderColorSaturation = m_folderColorSaturation->value() / 100.0;
        theme->folderColorBrightness = m_folderColorBrightness->value() / 100.0;
    }
    theme->folderDepthBrightnessMode = m_folderDepthBrightnessMode->currentData().toInt();
    theme->folderColorDarkenPerLevel = m_folderColorDarkenPerLevel->value() / 100.0;
    theme->fileColorSaturation = m_fileColorSaturation->value() / 100.0;
    theme->fileColorBrightness = m_fileColorBrightness->value() / 100.0;
    theme->borderStyle = m_borderStyle->currentData().toInt();
    theme->borderIntensity = m_borderIntensity->value() / 100.0;
    theme->highlightColor = m_highlightColorButton->property("selectedColor").value<QColor>();
    theme->freeSpaceColor = m_freeSpaceColorButton->property("selectedColor").value<QColor>();
    theme->unknownFileTypeColor = m_unknownFileTypeColorButton->property("selectedColor").value<QColor>();
    theme->highlightOpacity = m_highlightOpacity->value() / 100.0;
    theme->unknownFileTypeOpacity = m_unknownFileTypeOpacity->value() / 100.0;
    theme->sanitize();
}

void SettingsDialog::addColorTheme()
{
    storeSelectedColorThemeInto(m_workingSettings);
    m_workingSettings.activeColorThemeId = m_colorThemeSelector->currentData().toString();
    m_workingSettings.followSystemColorTheme = m_followSystemColorTheme->isChecked();
    m_workingSettings.lightModeColorThemeId = m_lightModeColorTheme->currentData().toString();
    m_workingSettings.darkModeColorThemeId = m_darkModeColorTheme->currentData().toString();

    const TreemapColorTheme* source = m_workingSettings.findColorTheme(m_colorThemeSelector->currentData().toString());
    TreemapColorTheme theme = source ? *source : m_workingSettings.activeColorTheme();
    theme.id = TreemapColorTheme::createCustomId();
    theme.name = tr("Custom theme");
    m_workingSettings.colorThemes.push_back(theme);
    populateColorThemeSelectors();
    m_colorThemeSelector->setCurrentIndex(m_colorThemeSelector->findData(theme.id));
    m_selectedColorThemeId = theme.id;
    loadSelectedColorThemeIntoFields();
    refreshPreview();
}

void SettingsDialog::duplicateColorTheme()
{
    storeSelectedColorThemeInto(m_workingSettings);
    m_workingSettings.activeColorThemeId = m_colorThemeSelector->currentData().toString();
    m_workingSettings.followSystemColorTheme = m_followSystemColorTheme->isChecked();
    m_workingSettings.lightModeColorThemeId = m_lightModeColorTheme->currentData().toString();
    m_workingSettings.darkModeColorThemeId = m_darkModeColorTheme->currentData().toString();

    const TreemapColorTheme* source = m_workingSettings.findColorTheme(m_colorThemeSelector->currentData().toString());
    if (!source) {
        return;
    }

    TreemapColorTheme copy = *source;
    copy.id = TreemapColorTheme::createCustomId();
    copy.name = tr("%1 Copy").arg(source->name);
    m_workingSettings.colorThemes.push_back(copy);
    populateColorThemeSelectors();
    m_colorThemeSelector->setCurrentIndex(m_colorThemeSelector->findData(copy.id));
    m_selectedColorThemeId = copy.id;
    loadSelectedColorThemeIntoFields();
    refreshPreview();
}

void SettingsDialog::removeSelectedColorTheme()
{
    storeSelectedColorThemeInto(m_workingSettings);
    m_workingSettings.activeColorThemeId = m_colorThemeSelector->currentData().toString();
    m_workingSettings.followSystemColorTheme = m_followSystemColorTheme->isChecked();
    m_workingSettings.lightModeColorThemeId = m_lightModeColorTheme->currentData().toString();
    m_workingSettings.darkModeColorThemeId = m_darkModeColorTheme->currentData().toString();

    const QString themeId = m_colorThemeSelector->currentData().toString();
    if (TreemapColorTheme::isBuiltInId(themeId)) {
        return;
    }

    for (int i = 0; i < m_workingSettings.colorThemes.size(); ++i) {
        if (m_workingSettings.colorThemes.at(i).id == themeId) {
            m_workingSettings.colorThemes.removeAt(i);
            break;
        }
    }

    if (m_workingSettings.activeColorThemeId == themeId) {
        m_workingSettings.activeColorThemeId = m_workingSettings.lightModeColorThemeId;
    }
    if (m_workingSettings.lightModeColorThemeId == themeId) {
        m_workingSettings.lightModeColorThemeId = TreemapColorTheme::builtInLightId();
    }
    if (m_workingSettings.darkModeColorThemeId == themeId) {
        m_workingSettings.darkModeColorThemeId = TreemapColorTheme::builtInDarkId();
    }

    m_workingSettings.ensureColorThemes();
    populateColorThemeSelectors();
    loadSelectedColorThemeIntoFields();
    refreshPreview();
}

void SettingsDialog::applySettingsToFields(const TreemapSettings& settings)
{
    m_workingSettings = settings;
    m_selectedColorThemeId = settings.activeColorThemeId;
    m_headerHeight->setValue(settings.headerHeight);
    m_headerFontFamily->setCurrentFont(settings.headerFontFamily.isEmpty()
        ? font()
        : QFont(settings.headerFontFamily));
    m_headerFontSize->setValue(settings.headerFontSize);
    m_headerFontBold->setChecked(settings.headerFontBold);
    m_headerFontItalic->setChecked(settings.headerFontItalic);
    m_fileFontFamily->setCurrentFont(settings.fileFontFamily.isEmpty()
        ? font()
        : QFont(settings.fileFontFamily));
    m_fileFontSize->setValue(settings.fileFontSize);
    m_fileFontBold->setChecked(settings.fileFontBold);
    m_fileFontItalic->setChecked(settings.fileFontItalic);
    m_border->setValue(settings.border);
    m_folderPadding->setValue(settings.folderPadding);
    m_tileAspectBias->setValue(qRound(settings.tileAspectBias * 10.0));
    m_tileAspectBiasValue->setText(QString::number(settings.tileAspectBias, 'f', 1));
    m_baseVisibleDepth->setValue(settings.baseVisibleDepth);
    m_baseVisibleDepthValue->setText(QString::number(settings.baseVisibleDepth));
    m_depthRevealPerZoomDoubling->setValue(settings.depthRevealPerZoomDoubling);
    m_minTileSize->setValue(settings.minTileSize);
    m_minPaint->setValue(settings.minPaint);
    m_minRevealHeight->setValue(settings.minRevealHeight);
    m_minRevealWidth->setValue(settings.minRevealWidth);
    m_revealFadeHeight->setValue(settings.revealFadeHeight);
    m_revealFadeWidth->setValue(settings.revealFadeWidth);
    m_zoomDurationMs->setValue(settings.zoomDurationMs);
    m_cameraDurationMs->setValue(settings.cameraDurationMs);
    m_simpleTooltips->setChecked(settings.simpleTooltips);
    m_cameraMaxScale->setValue(static_cast<int>(std::round(settings.cameraMaxScale)));
    m_cameraMaxScaleValue->setText(QString::number(static_cast<int>(std::round(settings.cameraMaxScale))));
    m_liveScanPreview->setChecked(settings.liveScanPreview);
    m_followSystemColorTheme->setChecked(settings.followSystemColorTheme);
    m_hideNonLocalFreeSpace->setChecked(settings.hideNonLocalFreeSpace);
    m_randomColorForUnknownFiles->setChecked(settings.randomColorForUnknownFiles);
    m_excludedPathEdit->clear();
    m_excludedPathsList->clear();
    m_excludedPathsList->addItems(settings.excludedPaths);
    {
        QSignalBlocker blocker(m_fileTypeGroupsList);
        m_fileTypeGroupsList->clear();
        for (const FileTypeGroup& g : settings.fileTypeGroups) {
            auto* item = new QListWidgetItem(
                fileTypeGroupSwatchIcon(g.color),
                tr("%1  (%2 ext)").arg(g.name).arg(g.extensions.size()));
            item->setData(Qt::UserRole,     g.color);
            item->setData(Qt::UserRole + 1, g.extensions);
            item->setData(Qt::UserRole + 2, g.name);
            m_fileTypeGroupsList->addItem(item);
        }
        m_fileTypeGroupsList->setCurrentRow(0);
    }
    onFileTypeGroupSelectionChanged();
    populateColorThemeSelectors();
    loadSelectedColorThemeIntoFields();
}

TreemapSettings SettingsDialog::settings() const
{
    TreemapSettings currentSettings = m_workingSettings;
    storeSelectedColorThemeInto(currentSettings);
    currentSettings.headerHeight = m_headerHeight->value();
    currentSettings.headerFontFamily = m_headerFontFamily->currentFont().family();
    currentSettings.headerFontSize = m_headerFontSize->value();
    currentSettings.headerFontBold = m_headerFontBold->isChecked();
    currentSettings.headerFontItalic = m_headerFontItalic->isChecked();
    currentSettings.fileFontFamily = m_fileFontFamily->currentFont().family();
    currentSettings.fileFontSize = m_fileFontSize->value();
    currentSettings.fileFontBold = m_fileFontBold->isChecked();
    currentSettings.fileFontItalic = m_fileFontItalic->isChecked();
    currentSettings.border = m_border->value();
    currentSettings.folderPadding = m_folderPadding->value();
    currentSettings.tileAspectBias = m_tileAspectBias->value() / 10.0;
    currentSettings.baseVisibleDepth = m_baseVisibleDepth->value();
    currentSettings.depthRevealPerZoomDoubling = m_depthRevealPerZoomDoubling->value();
    currentSettings.minTileSize = m_minTileSize->value();
    currentSettings.minPaint = m_minPaint->value();
    currentSettings.minRevealHeight = m_minRevealHeight->value();
    currentSettings.minRevealWidth = m_minRevealWidth->value();
    currentSettings.revealFadeHeight = m_revealFadeHeight->value();
    currentSettings.revealFadeWidth = m_revealFadeWidth->value();
    currentSettings.zoomDurationMs = m_zoomDurationMs->value();
    currentSettings.cameraDurationMs = m_cameraDurationMs->value();
    currentSettings.simpleTooltips = m_simpleTooltips->isChecked();
    currentSettings.cameraMaxScale = m_cameraMaxScale->value();
    currentSettings.liveScanPreview = m_liveScanPreview->isChecked();
    currentSettings.randomColorForUnknownFiles = m_randomColorForUnknownFiles->isChecked();
    currentSettings.activeColorThemeId = !m_selectedColorThemeId.isEmpty()
        ? m_selectedColorThemeId
        : m_colorThemeSelector->currentData().toString();
    currentSettings.followSystemColorTheme = m_followSystemColorTheme->isChecked();
    currentSettings.lightModeColorThemeId = m_lightModeColorTheme->currentData().toString();
    currentSettings.darkModeColorThemeId = m_darkModeColorTheme->currentData().toString();
    currentSettings.hideNonLocalFreeSpace = m_hideNonLocalFreeSpace->isChecked();
    currentSettings.excludedPaths.clear();
    for (int i = 0; i < m_excludedPathsList->count(); ++i) {
        currentSettings.excludedPaths.push_back(m_excludedPathsList->item(i)->text());
    }
    currentSettings.fileTypeGroups.clear();
    for (int i = 0; i < m_fileTypeGroupsList->count(); ++i) {
        const QListWidgetItem* item = m_fileTypeGroupsList->item(i);
        FileTypeGroup g;
        g.name       = item->data(Qt::UserRole + 2).toString();
        g.color      = item->data(Qt::UserRole).value<QColor>();
        g.extensions = item->data(Qt::UserRole + 1).toStringList();
        currentSettings.fileTypeGroups.append(std::move(g));
    }
    currentSettings.sanitize();
    return currentSettings;
}

void SettingsDialog::addExcludedPath()
{
    TreemapSettings normalized;
    normalized.excludedPaths = {m_excludedPathEdit->text()};
    normalized.sanitize();
    if (normalized.excludedPaths.isEmpty()) {
        return;
    }

    const QString path = normalized.excludedPaths.constFirst();
    if (m_excludedPathsList->findItems(path, Qt::MatchExactly).isEmpty()) {
        m_excludedPathsList->addItem(path);
    }
    m_excludedPathEdit->clear();
}

void SettingsDialog::removeSelectedExcludedPaths()
{
    qDeleteAll(m_excludedPathsList->selectedItems());
}

void SettingsDialog::updateColorThemeEditorState()
{
    const QString themeId = m_colorThemeSelector->currentData().toString();
    const bool builtIn = TreemapColorTheme::isBuiltInId(themeId);
    m_colorThemeName->setEnabled(!builtIn);
    m_removeColorThemeButton->setEnabled(!builtIn);
}

void SettingsDialog::buildPreviewTree()
{
    m_previewRoot = createPreviewNode(m_previewArena, QStringLiteral("Root"), 1120, true);
    m_previewRoot->absolutePath = QDir::rootPath();

    FileNode* workspace = createPreviewNode(m_previewArena, "workspace", 460, true, 0, m_previewRoot);
    FileNode* assets = createPreviewNode(m_previewArena, "assets", 360, true, 0, m_previewRoot);
    FileNode* system = createPreviewNode(m_previewArena, "system", 300, true, 0, m_previewRoot);

    createPreviewNode(m_previewArena, "README.md", 28, false, qRgb(96, 156, 214), workspace);
    createPreviewNode(m_previewArena, "bundle.weird", 26, false, qRgb(128, 128, 128), workspace);
    createPreviewNode(m_previewArena, "apps", 150, true, 0, workspace);
    createPreviewNode(m_previewArena, "docs", 120, true, 0, workspace);
    createPreviewNode(m_previewArena, "shared", 95, true, 0, workspace);
    createPreviewNode(m_previewArena, "build.ninja", 32, false, qRgb(122, 176, 224), workspace);

    createPreviewNode(m_previewArena, "media", 145, true, 0, assets);
    createPreviewNode(m_previewArena, "icons", 90, true, 0, assets);
    createPreviewNode(m_previewArena, "textures", 70, true, 0, assets);
    createPreviewNode(m_previewArena, "hero.png", 55, false, qRgb(190, 96, 96), assets);
    createPreviewNode(m_previewArena, "sprite.odd", 24, false, qRgb(128, 128, 128), assets);

    createPreviewNode(m_previewArena, "logs", 110, true, 0, system);
    createPreviewNode(m_previewArena, "services", 90, true, 0, system);
    createPreviewNode(m_previewArena, "cache", 65, true, 0, system);
    createPreviewNode(m_previewArena, "temp.db", 35, false, qRgb(186, 170, 222), system);
    createPreviewNode(m_previewArena, "snapshot.blobx", 22, false, qRgb(128, 128, 128), system);

    FileNode* apps = workspace->children[2];
    createPreviewNode(m_previewArena, "widgets", 62, true, 0, apps);
    createPreviewNode(m_previewArena, "core", 54, true, 0, apps);
    createPreviewNode(m_previewArena, "tests", 34, true, 0, apps);

    FileNode* docs = workspace->children[3];
    createPreviewNode(m_previewArena, "preview", 52, true, 0, docs);
    createPreviewNode(m_previewArena, "guide.md", 38, false, qRgb(108, 166, 126), docs);
    createPreviewNode(m_previewArena, "api.txt", 30, false, qRgb(85, 153, 118), docs);

    FileNode* shared = workspace->children[4];
    createPreviewNode(m_previewArena, "theme", 38, true, 0, shared);
    createPreviewNode(m_previewArena, "platform", 33, true, 0, shared);
    createPreviewNode(m_previewArena, "types.h", 24, false, qRgb(118, 189, 154), shared);

    FileNode* media = assets->children[0];
    createPreviewNode(m_previewArena, "stills", 52, true, 0, media);
    createPreviewNode(m_previewArena, "video", 49, true, 0, media);
    createPreviewNode(m_previewArena, "audio", 44, true, 0, media);

    FileNode* icons = assets->children[1];
    createPreviewNode(m_previewArena, "toolbar", 36, true, 0, icons);
    createPreviewNode(m_previewArena, "status", 29, true, 0, icons);
    createPreviewNode(m_previewArena, "close.svg", 25, false, qRgb(216, 144, 62), icons);

    FileNode* textures = assets->children[2];
    createPreviewNode(m_previewArena, "paper.png", 26, false, qRgb(226, 144, 104), textures);
    createPreviewNode(m_previewArena, "noise.png", 22, false, qRgb(206, 122, 90), textures);
    createPreviewNode(m_previewArena, "gradients", 22, true, 0, textures);

    FileNode* logs = system->children[0];
    createPreviewNode(m_previewArena, "app", 42, true, 0, logs);
    createPreviewNode(m_previewArena, "worker", 38, true, 0, logs);
    createPreviewNode(m_previewArena, "events.log", 30, false, qRgb(198, 180, 226), logs);

    FileNode* services = system->children[1];
    createPreviewNode(m_previewArena, "daemon", 36, true, 0, services);
    createPreviewNode(m_previewArena, "scheduler", 31, true, 0, services);
    createPreviewNode(m_previewArena, "indexer", 23, true, 0, services);

    FileNode* cacheRoot = system->children[2];
    createPreviewNode(m_previewArena, "snapshots", 24, true, 0, cacheRoot);
    createPreviewNode(m_previewArena, "thumbs", 21, true, 0, cacheRoot);
    createPreviewNode(m_previewArena, "state.json", 20, false, qRgb(164, 136, 208), cacheRoot);

    FileNode* widgets = apps->children[0];
    createPreviewNode(m_previewArena, "controls", 24, true, 0, widgets);
    createPreviewNode(m_previewArena, "models", 18, true, 0, widgets);
    createPreviewNode(m_previewArena, "dialog.cpp", 20, false, qRgb(66, 153, 184), widgets);

    FileNode* core = apps->children[1];
    createPreviewNode(m_previewArena, "render", 18, true, 0, core);
    createPreviewNode(m_previewArena, "layout", 18, true, 0, core);
    createPreviewNode(m_previewArena, "scene.cpp", 18, false, qRgb(82, 162, 201), core);

    FileNode* tests = apps->children[2];
    createPreviewNode(m_previewArena, "ui_test", 18, false, qRgb(86, 181, 134), tests);
    createPreviewNode(m_previewArena, "perf_test", 16, false, qRgb(116, 201, 152), tests);

    FileNode* preview = docs->children[0];
    createPreviewNode(m_previewArena, "shots", 22, true, 0, preview);
    createPreviewNode(m_previewArena, "mock.json", 18, false, qRgb(92, 173, 136), preview);
    createPreviewNode(m_previewArena, "theme.ini", 12, false, qRgb(118, 189, 154), preview);

    FileNode* theme = shared->children[0];
    createPreviewNode(m_previewArena, "light.json", 15, false, qRgb(132, 198, 164), theme);
    createPreviewNode(m_previewArena, "dark.json", 13, false, qRgb(154, 210, 180), theme);
    createPreviewNode(m_previewArena, "tokens", 10, true, 0, theme);

    FileNode* platform = shared->children[1];
    createPreviewNode(m_previewArena, "linux", 14, true, 0, platform);
    createPreviewNode(m_previewArena, "mac", 11, true, 0, platform);
    createPreviewNode(m_previewArena, "win", 8, true, 0, platform);

    FileNode* stills = media->children[0];
    createPreviewNode(m_previewArena, "home.png", 19, false, qRgb(122, 198, 160), stills);
    createPreviewNode(m_previewArena, "detail.png", 17, false, qRgb(144, 210, 174), stills);
    createPreviewNode(m_previewArena, "focus.png", 16, false, qRgb(164, 224, 188), stills);

    FileNode* video = media->children[1];
    createPreviewNode(m_previewArena, "demo.mp4", 27, false, qRgb(230, 166, 72), video);
    createPreviewNode(m_previewArena, "walkthrough.mov", 22, false, qRgb(242, 186, 96), video);

    FileNode* audio = media->children[2];
    createPreviewNode(m_previewArena, "theme.ogg", 23, false, qRgb(192, 110, 110), audio);
    createPreviewNode(m_previewArena, "fx.wav", 21, false, qRgb(208, 126, 126), audio);

    FileNode* toolbar = icons->children[0];
    createPreviewNode(m_previewArena, "open.svg", 13, false, qRgb(238, 182, 86), toolbar);
    createPreviewNode(m_previewArena, "save.svg", 12, false, qRgb(244, 194, 106), toolbar);
    createPreviewNode(m_previewArena, "refresh.svg", 11, false, qRgb(248, 204, 122), toolbar);

    FileNode* status = icons->children[1];
    createPreviewNode(m_previewArena, "ok.svg", 11, false, qRgb(216, 144, 62), status);
    createPreviewNode(m_previewArena, "warn.svg", 10, false, qRgb(228, 160, 76), status);
    createPreviewNode(m_previewArena, "error.svg", 8, false, qRgb(236, 176, 92), status);

    FileNode* gradients = textures->children[2];
    createPreviewNode(m_previewArena, "warm.png", 12, false, qRgb(210, 130, 92), gradients);
    createPreviewNode(m_previewArena, "cool.png", 10, false, qRgb(230, 150, 112), gradients);

    FileNode* appLogs = logs->children[0];
    createPreviewNode(m_previewArena, "today.log", 16, false, qRgb(154, 138, 198), appLogs);
    createPreviewNode(m_previewArena, "yesterday.log", 14, false, qRgb(176, 157, 214), appLogs);
    createPreviewNode(m_previewArena, "archive", 12, true, 0, appLogs);

    FileNode* workerLogs = logs->children[1];
    createPreviewNode(m_previewArena, "queue.log", 15, false, qRgb(166, 148, 206), workerLogs);
    createPreviewNode(m_previewArena, "jobs.log", 13, false, qRgb(188, 170, 220), workerLogs);

    FileNode* daemon = services->children[0];
    createPreviewNode(m_previewArena, "state", 13, true, 0, daemon);
    createPreviewNode(m_previewArena, "daemon.cpp", 11, false, qRgb(146, 128, 192), daemon);

    FileNode* scheduler = services->children[1];
    createPreviewNode(m_previewArena, "plans", 12, true, 0, scheduler);
    createPreviewNode(m_previewArena, "scheduler.cpp", 10, false, qRgb(170, 150, 210), scheduler);

    FileNode* indexer = services->children[2];
    createPreviewNode(m_previewArena, "jobs", 11, true, 0, indexer);
    createPreviewNode(m_previewArena, "indexer.cpp", 8, false, qRgb(186, 168, 220), indexer);

    FileNode* snapshots = cacheRoot->children[0];
    createPreviewNode(m_previewArena, "frame-01", 10, false, qRgb(162, 135, 208), snapshots);
    createPreviewNode(m_previewArena, "frame-02", 8, false, qRgb(176, 151, 220), snapshots);
    createPreviewNode(m_previewArena, "frame-03", 6, false, qRgb(188, 165, 226), snapshots);

    FileNode* thumbs = cacheRoot->children[1];
    createPreviewNode(m_previewArena, "thumb-a.png", 9, false, qRgb(188, 165, 226), thumbs);
    createPreviewNode(m_previewArena, "thumb-b.png", 7, false, qRgb(198, 176, 232), thumbs);

    FileNode* controls = widgets->children[0];
    createPreviewNode(m_previewArena, "slider.cpp", 9, false, qRgb(103, 177, 210), controls);
    createPreviewNode(m_previewArena, "toggle.cpp", 8, false, qRgb(126, 190, 219), controls);
    createPreviewNode(m_previewArena, "knob.h", 7, false, qRgb(144, 202, 227), controls);
    createPreviewNode(m_previewArena, "hit_test.h", 3, false, qRgb(162, 214, 236), controls);
    createPreviewNode(m_previewArena, "focus_ring.h", 2, false, qRgb(178, 224, 242), controls);

    FileNode* models = widgets->children[1];
    createPreviewNode(m_previewArena, "pane_model.h", 10, false, qRgb(92, 162, 196), models);
    createPreviewNode(m_previewArena, "tree_model.h", 8, false, qRgb(112, 176, 208), models);

    FileNode* render = core->children[0];
    createPreviewNode(m_previewArena, "tiles.cpp", 8, false, qRgb(94, 168, 204), render);
    createPreviewNode(m_previewArena, "labels.cpp", 6, false, qRgb(118, 184, 216), render);

    FileNode* layout = core->children[1];
    createPreviewNode(m_previewArena, "split.cpp", 9, false, qRgb(104, 178, 214), layout);
    createPreviewNode(m_previewArena, "squarify.cpp", 7, false, qRgb(128, 194, 224), layout);

    FileNode* shots = preview->children[0];
    createPreviewNode(m_previewArena, "home.png", 10, false, qRgb(122, 198, 160), shots);
    createPreviewNode(m_previewArena, "detail.png", 7, false, qRgb(144, 210, 174), shots);
    createPreviewNode(m_previewArena, "search.png", 5, false, qRgb(164, 224, 188), shots);
    createPreviewNode(m_previewArena, "tooltip.png", 2, false, qRgb(182, 232, 202), shots);

    FileNode* tokens = theme->children[2];
    createPreviewNode(m_previewArena, "accent.json", 6, false, qRgb(154, 210, 180), tokens);
    createPreviewNode(m_previewArena, "spacing.json", 4, false, qRgb(174, 224, 194), tokens);
    createPreviewNode(m_previewArena, "radius.json", 2, false, qRgb(190, 234, 206), tokens);

    FileNode* linuxPlatform = platform->children[0];
    createPreviewNode(m_previewArena, "paths.cpp", 8, false, qRgb(140, 204, 170), linuxPlatform);
    createPreviewNode(m_previewArena, "fs.cpp", 6, false, qRgb(160, 218, 186), linuxPlatform);

    FileNode* archive = appLogs->children[2];
    createPreviewNode(m_previewArena, "2024.log", 7, false, qRgb(186, 168, 220), archive);
    createPreviewNode(m_previewArena, "2023.log", 5, false, qRgb(202, 184, 230), archive);
    createPreviewNode(m_previewArena, "2022.log", 2, false, qRgb(216, 198, 236), archive);

    FileNode* daemonState = daemon->children[0];
    createPreviewNode(m_previewArena, "pid", 7, false, qRgb(160, 142, 202), daemonState);
    createPreviewNode(m_previewArena, "config", 6, false, qRgb(178, 160, 214), daemonState);

    FileNode* schedulerPlans = scheduler->children[0];
    createPreviewNode(m_previewArena, "daily.json", 7, false, qRgb(178, 160, 214), schedulerPlans);
    createPreviewNode(m_previewArena, "nightly.json", 5, false, qRgb(196, 178, 224), schedulerPlans);

    FileNode* indexerJobs = indexer->children[0];
    createPreviewNode(m_previewArena, "scan.json", 6, false, qRgb(190, 172, 222), indexerJobs);
    createPreviewNode(m_previewArena, "merge.json", 5, false, qRgb(206, 188, 232), indexerJobs);
    createPreviewNode(m_previewArena, "gc.json", 2, false, qRgb(220, 202, 238), indexerJobs);

}

void SettingsDialog::refreshPreview()
{
    if (!m_previewWidget || !m_previewRoot) {
        return;
    }

    TreemapSettings previewSettings = settings();
    ColorUtils::assignColors(m_previewRoot, previewSettings);
    for (FileNode* child : m_previewRoot->children) {
        if (child && child->isVirtual) {
            child->color = previewSettings.freeSpaceColor.rgba();
            break;
        }
    }
    m_previewWidget->applySettings(previewSettings);
    m_previewWidget->notifyTreeChanged();
    if (!m_previewRoot->children.empty()) {
        m_previewWidget->setPreviewHoveredNode(m_previewRoot->children.front());
    }
}

void SettingsDialog::restoreDefaultsPreservingCustomThemes()
{
    storeSelectedColorThemeInto(m_workingSettings);

    const TreemapSettings defaults = TreemapSettings::defaults();
    QList<TreemapColorTheme> mergedThemes;
    mergedThemes.reserve(m_workingSettings.colorThemes.size());

    mergedThemes.push_back(TreemapColorTheme::defaultLightTheme());
    mergedThemes.push_back(TreemapColorTheme::defaultDarkTheme());
    for (const TreemapColorTheme& theme : m_workingSettings.colorThemes) {
        if (TreemapColorTheme::isBuiltInId(theme.id)) {
            continue;
        }
        mergedThemes.push_back(theme);
    }

    TreemapSettings resetSettings = defaults;
    resetSettings.colorThemes = mergedThemes;
    if (!resetSettings.findColorTheme(m_selectedColorThemeId)) {
        m_selectedColorThemeId = resetSettings.activeColorThemeId;
    }
    if (resetSettings.findColorTheme(m_selectedColorThemeId)) {
        resetSettings.activeColorThemeId = m_selectedColorThemeId;
    }

    applySettingsToFields(resetSettings);
    refreshPreview();
}

static void applyColorButton(QPushButton* button, const QColor& validColor)
{
    button->setText(validColor.name(QColor::HexRgb).toUpper());
    button->setIcon(makeColorSwatchIcon(validColor));
    button->setIconSize(QSize(18, 18));
    button->setStyleSheet(QString());
}

void SettingsDialog::updateFolderBaseColorButton()
{
    const QColor color = m_folderBaseColorButton->property("selectedColor").value<QColor>();
    applyColorButton(m_folderBaseColorButton,
                     color.isValid() ? color : QColor::fromHslF(0.58, 0.5, 0.45));
}

void SettingsDialog::updateHighlightColorButton()
{
    const QColor color = m_highlightColorButton->property("selectedColor").value<QColor>();
    applyColorButton(m_highlightColorButton,
                     color.isValid() ? color : TreemapSettings::defaults().highlightColor);
}

void SettingsDialog::updateFreeSpaceColorButton()
{
    const QColor color = m_freeSpaceColorButton->property("selectedColor").value<QColor>();
    applyColorButton(m_freeSpaceColorButton,
                     color.isValid() ? color : defaultFreeSpaceColor());
}

void SettingsDialog::updateUnknownFileTypeColorButton()
{
    const QColor color = m_unknownFileTypeColorButton->property("selectedColor").value<QColor>();
    applyColorButton(m_unknownFileTypeColorButton,
                     color.isValid() ? color : QColor(QStringLiteral("#E8E8E8")));
}

// ── File type group helpers ──────────────────────────────────────────────────

QIcon SettingsDialog::fileTypeGroupSwatchIcon(const QColor& color)
{
    QPixmap px(16, 16);
    px.fill(color.isValid() ? color : Qt::gray);
    return QIcon(px);
}

void SettingsDialog::updateFileTypeGroupColorButton()
{
    const QColor color = m_fileTypeGroupColorButton->property("selectedColor").value<QColor>();
    applyColorButton(m_fileTypeGroupColorButton,
                     color.isValid() ? color : QColor(Qt::gray));
}

void SettingsDialog::syncFileTypeGroupToFields(int row)
{
    if (row < 0 || row >= m_fileTypeGroupsList->count()) {
        m_fileTypeGroupEditBox->setEnabled(false);
        return;
    }
    m_fileTypeGroupEditBox->setEnabled(true);
    const QListWidgetItem* item = m_fileTypeGroupsList->item(row);
    const QString name = item->data(Qt::UserRole + 2).toString();
    const QColor color = item->data(Qt::UserRole).value<QColor>();
    const QStringList exts = item->data(Qt::UserRole + 1).toStringList();

    QSignalBlocker b1(m_fileTypeGroupName);
    QSignalBlocker b2(m_fileTypeGroupExtensions);
    m_fileTypeGroupName->setText(name);
    m_fileTypeGroupExtensions->setPlainText(exts.join(QLatin1Char('\n')));
    m_fileTypeGroupColorButton->setProperty("selectedColor", color);
    updateFileTypeGroupColorButton();
}

void SettingsDialog::syncFileTypeGroupFromFields(int row)
{
    if (row < 0 || row >= m_fileTypeGroupsList->count()) {
        return;
    }
    QListWidgetItem* item = m_fileTypeGroupsList->item(row);
    const QString name = m_fileTypeGroupName->text().trimmed();
    const QColor color = m_fileTypeGroupColorButton->property("selectedColor").value<QColor>();
    const QString extsText = m_fileTypeGroupExtensions->toPlainText();
    QStringList exts = extsText.split(QRegularExpression(QStringLiteral("[,\n]")), Qt::SkipEmptyParts);
    for (auto& e : exts) {
        e = e.trimmed().toLower();
    }
    exts.removeAll(QString{});

    item->setData(Qt::UserRole + 2, name);
    item->setData(Qt::UserRole,     color);
    item->setData(Qt::UserRole + 1, exts);
    item->setIcon(fileTypeGroupSwatchIcon(color));
    item->setText(tr("%1  (%2 ext)").arg(name).arg(exts.size()));
}

void SettingsDialog::onFileTypeGroupSelectionChanged()
{
    syncFileTypeGroupToFields(m_fileTypeGroupsList->currentRow());
}

void SettingsDialog::addFileTypeGroup()
{
    const float hue = QRandomGenerator::global()->bounded(360) / 360.0f;
    const QColor color = QColor::fromHslF(hue, 1.0f, 0.50f);
    auto* item = new QListWidgetItem(
        fileTypeGroupSwatchIcon(color),
        tr("New group  (0 ext)"));
    item->setData(Qt::UserRole,     color);
    item->setData(Qt::UserRole + 1, QStringList{});
    item->setData(Qt::UserRole + 2, tr("New group"));
    m_fileTypeGroupsList->addItem(item);
    m_fileTypeGroupsList->setCurrentItem(item);
    m_fileTypeGroupName->setFocus();
    m_fileTypeGroupName->selectAll();
}

void SettingsDialog::removeSelectedFileTypeGroup()
{
    const int row = m_fileTypeGroupsList->currentRow();
    if (row < 0) {
        return;
    }
    delete m_fileTypeGroupsList->takeItem(row);
    onFileTypeGroupSelectionChanged();
}

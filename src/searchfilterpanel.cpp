// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "searchfilterpanel.h"
#include "mainwindow_utils.h"

#include <algorithm>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateEdit>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// Colors matching the ColorRed…ColorPurple marks in treemapsettings.h
static const QColor kMarkColors[] = {
    QColor(210,  65,  65),   // ColorRed
    QColor(210, 120,  40),   // ColorOrange
    QColor(190, 170,  35),   // ColorYellow
    QColor( 55, 155,  55),   // ColorGreen
    QColor( 55, 115, 210),   // ColorBlue
    QColor(130,  65, 195),   // ColorPurple
};
static const char* const kMarkNames[] = {
    QT_TRANSLATE_NOOP("SearchFilterPanel", "Red"), QT_TRANSLATE_NOOP("SearchFilterPanel", "Orange"), QT_TRANSLATE_NOOP("SearchFilterPanel", "Yellow"),
    QT_TRANSLATE_NOOP("SearchFilterPanel", "Green"), QT_TRANSLATE_NOOP("SearchFilterPanel", "Blue"), QT_TRANSLATE_NOOP("SearchFilterPanel", "Purple")
};

// CatGames…CatVideo (FolderMark int values 7–17)
struct IconMarkDef { const char* name; const char* icon; int value; };
static const IconMarkDef kIconMarks[] = {
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Backup"),      ":/assets/tabler-icons/database-export.svg", 9 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Cloud"),       ":/assets/tabler-icons/cloud.svg",            10 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Development"), ":/assets/tabler-icons/code.svg",             8 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Downloads"),   ":/assets/tabler-icons/download.svg",         12 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Encrypted"),   ":/assets/tabler-icons/lock.svg",             16 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Favourites"),  ":/assets/tabler-icons/heart.svg",            15 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Games"),       ":/assets/tabler-icons/device-gamepad-2.svg", 7 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Music"),       ":/assets/tabler-icons/music.svg",            14 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Photos"),      ":/assets/tabler-icons/photo.svg",            11 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Temporary"),   ":/assets/tabler-icons/trash.svg",            13 },
    { QT_TRANSLATE_NOOP("SearchFilterPanel", "Video"),       ":/assets/tabler-icons/video.svg",            17 },
};

QComboBox* makeSizeUnitCombo(QWidget* parent)
{
    auto* c = new QComboBox(parent);
    c->addItem(QStringLiteral("B"));
    c->addItem(QStringLiteral("KiB"));
    c->addItem(QStringLiteral("MiB"));
    c->addItem(QStringLiteral("GiB"));
    c->addItem(QStringLiteral("TiB"));
    c->setCurrentIndex(2); // default to MiB
    c->setMaximumWidth(72);
    return c;
}

} // namespace

SearchFilterPanel::SearchFilterPanel(QWidget* parent)
    : QWidget(parent)
{
    m_nameDebounce = new QTimer(this);
    m_nameDebounce->setSingleShot(true);
    m_nameDebounce->setInterval(150);
    connect(m_nameDebounce, &QTimer::timeout, this, &SearchFilterPanel::onAnyFilterChanged);

    buildUi();
}

void SearchFilterPanel::buildUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_mainFrame = new QFrame(this);
    m_mainFrame->setObjectName(QStringLiteral("searchFilterMainFrame"));
    mainLayout->addWidget(m_mainFrame);

    auto* rootLayout = new QVBoxLayout(m_mainFrame);
#ifdef Q_OS_MACOS
    rootLayout->setContentsMargins(8, 1, 8, 1);
    rootLayout->setSpacing(1);
#else
    rootLayout->setContentsMargins(8, 5, 8, 5);
    rootLayout->setSpacing(4);
#endif

    // ── Row 1: Name | Size | Date ────────────────────────────────────────────
    auto* row1 = new QHBoxLayout;
    row1->setAlignment(Qt::AlignVCenter);
#ifdef Q_OS_MACOS
    row1->setSpacing(4);
#else
    row1->setSpacing(5);
#endif


    // Name
    auto* nameLabel = new QLabel(tr("Name:"), this);
    nameLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    row1->addWidget(nameLabel, 0, Qt::AlignVCenter);
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(searchPatternPlaceholderText());
    m_nameEdit->setClearButtonEnabled(true);
    m_nameEdit->setMinimumWidth(180);
    m_nameEdit->setMaximumWidth(320);
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this]() {
        m_nameDebounce->start();
    });
    row1->addWidget(m_nameEdit, 0, Qt::AlignVCenter);

    row1->addSpacing(8);

    // Size range
    auto* sizeLabel = new QLabel(tr("Size:"), this);
    sizeLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    row1->addWidget(sizeLabel, 0, Qt::AlignVCenter);
    m_sizeMinSpin = new QDoubleSpinBox(this);
    m_sizeMinSpin->setObjectName(QStringLiteral("m_sizeMinSpin"));
    m_sizeMinSpin->setRange(0, 9999999);
    m_sizeMinSpin->setDecimals(0);
    m_sizeMinSpin->setSpecialValueText(tr("any"));
    m_sizeMinSpin->setMaximumWidth(85);
    connect(m_sizeMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &SearchFilterPanel::onAnyFilterChanged);
    row1->addWidget(m_sizeMinSpin, 0, Qt::AlignVCenter);
    m_sizeMinUnit = makeSizeUnitCombo(this);
    m_sizeMinUnit->setObjectName(QStringLiteral("m_sizeMinUnit"));
    connect(m_sizeMinUnit, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SearchFilterPanel::onAnyFilterChanged);
    row1->addWidget(m_sizeMinUnit, 0, Qt::AlignVCenter);
    row1->addWidget(new QLabel(QStringLiteral("–"), this), 0, Qt::AlignVCenter);
    m_sizeMaxSpin = new QDoubleSpinBox(this);
    m_sizeMaxSpin->setObjectName(QStringLiteral("m_sizeMaxSpin"));
    m_sizeMaxSpin->setRange(0, 9999999);
    m_sizeMaxSpin->setDecimals(0);
    m_sizeMaxSpin->setSpecialValueText(tr("any"));
    m_sizeMaxSpin->setMaximumWidth(85);
    connect(m_sizeMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &SearchFilterPanel::onAnyFilterChanged);
    row1->addWidget(m_sizeMaxSpin, 0, Qt::AlignVCenter);
    m_sizeMaxUnit = makeSizeUnitCombo(this);
    m_sizeMaxUnit->setObjectName(QStringLiteral("m_sizeMaxUnit"));
    connect(m_sizeMaxUnit, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SearchFilterPanel::onAnyFilterChanged);
    row1->addWidget(m_sizeMaxUnit, 0, Qt::AlignVCenter);

    row1->addSpacing(8);

    // Date range
    auto* dateLabel = new QLabel(tr("Date:"), this);
    dateLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    row1->addWidget(dateLabel, 0, Qt::AlignVCenter);
    m_dateFromCheck = new QCheckBox(this);
    m_dateFromCheck->setObjectName(QStringLiteral("m_dateFromCheck"));
    m_dateFromCheck->setToolTip(tr("Enable 'modified from' date bound"));
    connect(m_dateFromCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_dateFromEdit->setEnabled(on);
        onAnyFilterChanged();
    });
    row1->addWidget(m_dateFromCheck, 0, Qt::AlignVCenter);
    m_dateFromEdit = new QDateEdit(QDate::currentDate(), this);
    m_dateFromEdit->setObjectName(QStringLiteral("m_dateFromEdit"));
    m_dateFromEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateFromEdit->setCalendarPopup(true);
    m_dateFromEdit->setEnabled(false);
    m_dateFromEdit->setMaximumWidth(150);
    connect(m_dateFromEdit, &QDateEdit::dateChanged, this, &SearchFilterPanel::onAnyFilterChanged);
    row1->addWidget(m_dateFromEdit, 0, Qt::AlignVCenter);
    row1->addWidget(new QLabel(QStringLiteral("–"), this), 0, Qt::AlignVCenter);
    m_dateToCheck = new QCheckBox(this);
    m_dateToCheck->setObjectName(QStringLiteral("m_dateToCheck"));
    m_dateToCheck->setToolTip(tr("Enable 'modified to' date bound"));
    connect(m_dateToCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_dateToEdit->setEnabled(on);
        onAnyFilterChanged();
    });
    row1->addWidget(m_dateToCheck, 0, Qt::AlignVCenter);
    m_dateToEdit = new QDateEdit(QDate::currentDate(), this);
    m_dateToEdit->setObjectName(QStringLiteral("m_dateToEdit"));
    m_dateToEdit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateToEdit->setCalendarPopup(true);
    m_dateToEdit->setEnabled(false);
    m_dateToEdit->setMaximumWidth(150);
    connect(m_dateToEdit, &QDateEdit::dateChanged, this, &SearchFilterPanel::onAnyFilterChanged);
    row1->addWidget(m_dateToEdit, 0, Qt::AlignVCenter);

    row1->addStretch(1);

    // Clear button
    m_clearButton = new QPushButton(tr("Clear"), this);
    m_clearButton->setToolTip(tr("Clear all filters"));
    m_clearButton->setFlat(true);
    connect(m_clearButton, &QPushButton::clicked, this, &SearchFilterPanel::clearAll);
    row1->addWidget(m_clearButton, 0, Qt::AlignVCenter);

    rootLayout->addLayout(row1);

    // ── Row 2: Type | Mode | Marks | Hide | Clear ────────────────────────────
    auto* row2 = new QHBoxLayout;
    row2->setAlignment(Qt::AlignVCenter);
#ifdef Q_OS_MACOS
    row2->setSpacing(4);
#else
    row2->setSpacing(5);
#endif


    // File type combo
    auto* typeLabel = new QLabel(tr("Type:"), this);
    typeLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    row2->addWidget(typeLabel, 0, Qt::AlignVCenter);
    m_typeCombo = new QComboBox(this);
    m_typeCombo->setObjectName(QStringLiteral("m_typeCombo"));
    m_typeCombo->addItem(tr("All types"), QString());
    m_typeCombo->setMinimumWidth(110);
    m_typeCombo->setMaximumWidth(180);
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SearchFilterPanel::onAnyFilterChanged);
    row2->addWidget(m_typeCombo, 0, Qt::AlignVCenter);

    row2->addSpacing(8);

    // Mode: All / Files / Folders radio buttons
    m_modeGroup = new QButtonGroup(this);
    auto* radioAll     = new QRadioButton(tr("All"),     this);
    auto* radioFiles   = new QRadioButton(tr("Files"),   this);
    auto* radioFolders = new QRadioButton(tr("Folders"), this);
    radioAll->setChecked(true);
    m_modeGroup->addButton(radioAll,     0);
    m_modeGroup->addButton(radioFiles,   1);
    m_modeGroup->addButton(radioFolders, 2);
    connect(m_modeGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, [this](int) { onAnyFilterChanged(); });
    row2->addWidget(radioAll, 0, Qt::AlignVCenter);
    row2->addWidget(radioFiles, 0, Qt::AlignVCenter);
    row2->addWidget(radioFolders, 0, Qt::AlignVCenter);

    row2->addSpacing(8);

    // Mark filter (color marks only)
    auto* markLabel = new QLabel(tr("Mark:"), this);
    markLabel->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    row2->addWidget(markLabel, 0, Qt::AlignVCenter);
    for (int i = 0; i < 6; ++i) {
        const QColor c = kMarkColors[i];
        auto* btn = new QPushButton(QStringLiteral("✔"), this);
        btn->setCheckable(true);
        btn->setFixedSize(18, 18);
        btn->setFlat(true);
        btn->setToolTip(tr("Filter: %1 mark").arg(QCoreApplication::translate("SearchFilterPanel", kMarkNames[i])));
        btn->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background-color: %1;"
            "  border: none;"
            "  border-radius: 3px;"
            "  color: transparent;"
            "  font-weight: bold;"
            "  font-size: 14px;"
            "  padding: 0;"
            "}"
            "QPushButton:checked {"
            "  color: white;"
            "}"
        ).arg(c.name()));
        connect(btn, &QPushButton::toggled, this, [this](bool) { onAnyFilterChanged(); });
        m_markButtons[i] = btn;
        row2->addWidget(btn, 0, Qt::AlignVCenter);
    }

    row2->addSpacing(4);

    for (int i = 0; i < 11; ++i) {
        auto* btn = new QPushButton(this);
        btn->setCheckable(true);
        btn->setFixedSize(18, 18);
        btn->setFlat(true);
        btn->setIconSize(QSize(16, 16));
        btn->setToolTip(tr("Filter: %1 mark").arg(QCoreApplication::translate("SearchFilterPanel", kIconMarks[i].name)));
        connect(btn, &QPushButton::toggled, this, [this](bool) { onAnyFilterChanged(); });
        m_iconMarkButtons[i] = btn;
        row2->addWidget(btn, 0, Qt::AlignVCenter);
    }

    row2->addSpacing(8);

    // Hide non-matching
    m_hideCheck = new QCheckBox(tr("Hide non-matching"), this);
    m_hideCheck->setToolTip(tr("Exclude non-matching tiles from the treemap layout"));
    connect(m_hideCheck, &QCheckBox::toggled, this, [this](bool) { onAnyFilterChanged(); });
    row2->addWidget(m_hideCheck, 0, Qt::AlignVCenter);

    row2->addStretch(1);

    rootLayout->addLayout(row2);

    refreshChromeStyles();
}

void SearchFilterPanel::focusNameField()
{
    if (m_nameEdit) {
        m_nameEdit->setFocus(Qt::OtherFocusReason);
        m_nameEdit->selectAll();
    }
}

void SearchFilterPanel::setChromeBorderColor(const QColor& color)
{
    if (m_chromeBorderColor == color) {
        return;
    }

    m_chromeBorderColor = color;
    refreshChromeStyles();
}

void SearchFilterPanel::refreshChromeStyles()
{
    if (!m_mainFrame) return;

    const QPalette p = palette();
    const QColor borderColor = m_chromeBorderColor.isValid()
        ? m_chromeBorderColor
        : p.color(QPalette::Mid);

    m_mainFrame->setStyleSheet(QStringLiteral(
        "QFrame#searchFilterMainFrame {"
        "  background: palette(window);"
        "  border: none;"
        "  border-bottom: 1px solid %1;"
        "}"
    ).arg(borderColor.name(QColor::HexArgb)));

    const bool isDark = widgetChromeUsesDarkColorScheme();
    const QString iconHoverBg = isDark ? QStringLiteral("rgba(255,255,255,30)") : QStringLiteral("rgba(0,0,0,15)");
    const QString iconCheckedBg = isDark ? QStringLiteral("rgba(255,255,255,60)") : QStringLiteral("rgba(0,0,0,30)");

    // Update category icons
    for (int i = 0; i < 11; ++i) {
        if (m_iconMarkButtons[i]) {
            m_iconMarkButtons[i]->setIcon(toolbarIcon({}, QString::fromLatin1(kIconMarks[i].icon)));
            m_iconMarkButtons[i]->setStyleSheet(QStringLiteral(
                "QPushButton {"
                "  border: none;"
                "  border-radius: 3px;"
                "  background: transparent;"
                "  padding: 0;"
                "}"
                "QPushButton:hover {"
                "  background-color: %1;"
                "}"
                "QPushButton:checked {"
                "  background-color: %2;"
                "}"
            ).arg(iconHoverBg, iconCheckedBg));
        }
    }

    // Update clear button icon
    if (m_clearButton) {
        m_clearButton->setIcon(toolbarIcon({"edit-clear-all", "edit-clear"},
            QStringLiteral(":/assets/tabler-icons/filter-off.svg")));
    }
}

void SearchFilterPanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);

    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
    case QEvent::ThemeChange:
        refreshChromeStyles();
        break;
    default:
        break;
    }
}

void SearchFilterPanel::setSettings(const TreemapSettings& settings)
{
    const QString currentType = m_typeCombo->currentData().toString();

    QSignalBlocker blocker(m_typeCombo);
    m_typeCombo->clear();
    m_typeCombo->addItem(tr("All types"), QString());

    QList<const FileTypeGroup*> sortedGroups;
    for (const auto& group : settings.fileTypeGroups) {
        sortedGroups.append(&group);
    }
    std::sort(sortedGroups.begin(), sortedGroups.end(), [](const FileTypeGroup* a, const FileTypeGroup* b) {
        return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
    });

    for (const FileTypeGroup* group : sortedGroups) {
        m_typeCombo->addItem(makeColorSwatchIcon(group->color), group->name, group->name);
    }

    // Restore previous selection if it still exists.
    const int idx = m_typeCombo->findData(currentType);
    m_typeCombo->setCurrentIndex(idx >= 0 ? idx : 0);
}

FilterParams SearchFilterPanel::currentParams() const
{
    FilterParams p;

    p.namePattern = m_nameEdit->text().trimmed();

    const qint64 sizeMin = computeSizeBytes(m_sizeMinSpin->value(), m_sizeMinUnit->currentIndex());
    const qint64 sizeMax = computeSizeBytes(m_sizeMaxSpin->value(), m_sizeMaxUnit->currentIndex());
    if (sizeMin > 0 && sizeMax > 0 && sizeMin > sizeMax) {
        p.sizeMin = sizeMax;
        p.sizeMax = sizeMin;
    } else {
        p.sizeMin = sizeMin;
        p.sizeMax = sizeMax;
    }

    if (m_dateFromCheck->isChecked()) {
        const QDateTime dt(m_dateFromEdit->date(), QTime(0, 0, 0));
        p.dateMin = dt.toSecsSinceEpoch();
    }
    if (m_dateToCheck->isChecked()) {
        const QDateTime dt(m_dateToEdit->date(), QTime(23, 59, 59));
        p.dateMax = dt.toSecsSinceEpoch();
    }
    if (p.dateMin > 0 && p.dateMax > 0 && p.dateMin > p.dateMax) {
        std::swap(p.dateMin, p.dateMax);
    }

    const QString typeGroup = m_typeCombo->currentData().toString();
    if (!typeGroup.isEmpty()) {
        p.fileTypeGroups.insert(typeGroup);
    }

    const int modeId = m_modeGroup->checkedId();
    p.filesOnly   = (modeId == 1);
    p.foldersOnly = (modeId == 2);

    for (int i = 0; i < 6; ++i) {
        if (m_markButtons[i] && m_markButtons[i]->isChecked())
            p.markFilter.insert(1 + i); // FolderMark::ColorRed = 1, ..., ColorPurple = 6
    }
    for (int i = 0; i < 11; ++i) {
        if (m_iconMarkButtons[i] && m_iconMarkButtons[i]->isChecked())
            p.markFilter.insert(kIconMarks[i].value);
    }

    p.hideNonMatching = m_hideCheck->isChecked();
    return p;
}

void SearchFilterPanel::clearAll()
{
    {
        QSignalBlocker b1(m_nameEdit);
        QSignalBlocker b2(m_sizeMinSpin);
        QSignalBlocker b3(m_sizeMaxSpin);
        QSignalBlocker b4(m_dateFromCheck);
        QSignalBlocker b5(m_dateToCheck);
        QSignalBlocker b6(m_typeCombo);
        QSignalBlocker b7(m_hideCheck);

        m_nameEdit->clear();
        m_sizeMinSpin->setValue(0);
        m_sizeMaxSpin->setValue(0);
        m_sizeMinUnit->setCurrentIndex(2); // MiB
        m_sizeMaxUnit->setCurrentIndex(2); // MiB
        m_dateFromCheck->setChecked(false);
        m_dateFromEdit->setEnabled(false);
        m_dateToCheck->setChecked(false);
        m_dateToEdit->setEnabled(false);
        m_typeCombo->setCurrentIndex(0);
        m_hideCheck->setChecked(false);
        if (m_modeGroup->button(0)) m_modeGroup->button(0)->setChecked(true);
        for (auto* btn : m_markButtons) {
            if (btn) {
                QSignalBlocker bm(btn);
                btn->setChecked(false);
            }
        }
        for (auto* btn : m_iconMarkButtons) {
            if (btn) {
                QSignalBlocker bm(btn);
                btn->setChecked(false);
            }
        }
    }
    if (m_nameDebounce) m_nameDebounce->stop();
    emit filterParamsChanged(FilterParams{});
}

void SearchFilterPanel::onNameTextChanged()
{
    onAnyFilterChanged();
}

void SearchFilterPanel::onAnyFilterChanged()
{
    emit filterParamsChanged(currentParams());
}

// static
qint64 SearchFilterPanel::computeSizeBytes(double value, int unitIndex)
{
    if (value <= 0.0) return 0;
    static const qint64 kMults[] = {
        1LL,
        1024LL,
        1024LL * 1024,
        1024LL * 1024 * 1024,
        1024LL * 1024 * 1024 * 1024,
    };
    if (unitIndex < 0 || unitIndex >= static_cast<int>(sizeof(kMults) / sizeof(*kMults)))
        return 0;
    return static_cast<qint64>(value * static_cast<double>(kMults[unitIndex]));
}

// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "mainwindow.h"
#include "breadcrumbpathbar.h"
#include "platformthemewatcher.h"
#include "colorutils.h"
#include "filesystemwatchcontroller.h"
#include "iconutils.h"
#include "mainwindow_utils.h"
#include "nodepropertiesdialog.h"
#include "scanner.h"
#include "settingsdialog.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#ifdef DISKSCAPE_HAS_QT_DBUS
#include <QDBusConnection>
#include <QDBusMessage>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QStyleHints>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>

namespace {
constexpr int kMaxRecentPaths = 12;
constexpr int kMaxFavouritePaths = 8;
constexpr int kDirectoryPathRole = Qt::UserRole;
constexpr int kDirectoryLoadedRole = Qt::UserRole + 1;
constexpr int kDirectoryDummyRole = Qt::UserRole + 2;
constexpr int kDirectoryUsagePercentRole = Qt::UserRole + 3;
constexpr int kDirectorySyntheticFilesRole = Qt::UserRole + 4;
constexpr int kDirectorySortValueRole = Qt::UserRole + 5;
constexpr int kDirectoryColorRole = Qt::UserRole + 6;
constexpr int kDirectoryNodeRole = Qt::UserRole + 7;
constexpr int kLegendColorRole = Qt::UserRole + 2;
constexpr QChar kLeftToRightIsolate(0x2066);
constexpr QChar kPopDirectionalIsolate(0x2069);

QStringList defaultFavouritePaths()
{
    QStringList defaults;
    const auto appendIfPresent = [&defaults](const QString& path) {
        const QString normalized = normalizedFilesystemPath(path);
        if (normalized.isEmpty() || defaults.contains(normalized) || !QFileInfo::exists(normalized)) {
            return;
        }
        defaults.push_back(normalized);
    };

    appendIfPresent(QDir::homePath());
    appendIfPresent(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    appendIfPresent(QDir::rootPath());
    return defaults;
}

QString statusBarDisplayPath(const QString& path)
{
    const QString nativePath = QDir::toNativeSeparators(path);
    return QString(kLeftToRightIsolate) + nativePath + QString(kPopDirectionalIsolate);
}

QString directoryDisplayName(const FileNode* node)
{
    if (!node) {
        return {};
    }
    if (!node->parent) {
        const QString path = normalizedFilesystemPath(node->computePath());
        if (path == QDir::separator()) {
            return path;
        }
        const QFileInfo info(path);
        return info.fileName().isEmpty() ? QDir::toNativeSeparators(path) : info.fileName();
    }
    return node->name;
}

QStringList sanitizedLandingPaths(const QStringList& paths, int maxCount)
{
    QStringList sanitized;
    sanitized.reserve(paths.size());
    for (const QString& rawPath : paths) {
        const QString normalized = normalizedFilesystemPath(rawPath);
        if (normalized.isEmpty() || sanitized.contains(normalized)) {
            continue;
        }
        sanitized.push_back(normalized);
        if (maxCount > 0 && sanitized.size() >= maxCount) {
            break;
        }
    }
    return sanitized;
}

QString relativeUsageText(qint64 size, qint64 totalSize)
{
    if (totalSize <= 0) {
        return QStringLiteral("0.0%");
    }
    const double percent = (static_cast<double>(size) * 100.0) / static_cast<double>(totalSize);
    return QStringLiteral("%1%").arg(QLocale::system().toString(percent, 'f', 1));
}

bool hasDirectoryChildren(const FileNode* node)
{
    if (!node) {
        return false;
    }

    return std::any_of(node->children.begin(), node->children.end(), [](const FileNode* child) {
        return child && child->isDirectory && !child->isVirtual;
    });
}

qint64 directFileBytes(const FileNode* node)
{
    if (!node) {
        return 0;
    }

    qint64 total = 0;
    for (const FileNode* child : node->children) {
        if (child && !child->isDirectory && !child->isVirtual) {
            total += child->size;
        }
    }
    return total;
}

QIcon directoryTreeFolderIcon(const QColor& color = {})
{
    if (color.isValid()) {
        return makeTintedFolderIcon(color);
    }

    return IconUtils::themeIcon({"folder"},
        QStringLiteral(":/assets/tabler-icons/folder.svg"),
        QStringLiteral(":/assets/tabler-icons/folder.svg"));
}

QIcon directoryTreeFilesIcon()
{
    return IconUtils::themeIcon({"files"},
        QStringLiteral(":/assets/tabler-icons/files.svg"),
        QStringLiteral(":/assets/tabler-icons/files.svg"));
}

void refreshDirectoryTreeIcons(QTreeWidgetItem* item)
{
    if (!item) {
        return;
    }

    if (item->data(0, kDirectorySyntheticFilesRole).toBool()) {
        item->setIcon(0, directoryTreeFilesIcon());
    } else if (!item->data(0, kDirectoryDummyRole).toBool()) {
        const auto nodePtr = item->data(0, kDirectoryNodeRole).value<quintptr>();
        if (nodePtr) {
            const QColor liveColor = QColor::fromRgba(
                reinterpret_cast<const FileNode*>(nodePtr)->color);
            item->setData(0, kDirectoryColorRole, liveColor);
            item->setIcon(0, directoryTreeFolderIcon(liveColor));
        } else {
            item->setIcon(0, directoryTreeFolderIcon(
                item->data(0, kDirectoryColorRole).value<QColor>()));
        }
    }

    for (int i = 0; i < item->childCount(); ++i) {
        refreshDirectoryTreeIcons(item->child(i));
    }
}

class DirectoryUsageBarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QVariant percentData = index.data(kDirectoryUsagePercentRole);
        const bool selected = (opt.state & QStyle::State_Selected);

        QStyledItemDelegate::paint(painter, opt, index);

        const double percent = percentData.toDouble();
        if (percentData.isValid() && percent > 0.0) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);

            const QRect contentRect = opt.rect.adjusted(6, 4, -6, -4);
            const int barHeight = std::min(contentRect.height(), 12);
            const int barTop = contentRect.top() + std::max(0, (contentRect.height() - barHeight) / 2);
            const QRect barRect(contentRect.left(), barTop, contentRect.width(), barHeight);
            if (barRect.width() > 0 && barRect.height() > 0) {
                const QWidget* widget = opt.widget;
                const bool windowActive = widget
                    ? widget->window()->isActiveWindow()
                    : (opt.state & QStyle::State_Active);
                const QPalette::ColorGroup colorGroup = windowActive
                    ? QPalette::Active
                    : QPalette::Inactive;
                const QColor trackColor = opt.palette.color(colorGroup, QPalette::AlternateBase);
                QColor fillColor = opt.palette.color(colorGroup, QPalette::Highlight);
                fillColor.setAlpha(selected ? 150 : 110);
                const qreal trackRadius = 3.0;
                const qreal fillRadius = 1.75;

                painter->setPen(Qt::NoPen);
                painter->setBrush(trackColor);
                painter->drawRoundedRect(barRect, trackRadius, trackRadius);

                QRect fillBounds = barRect.adjusted(2, 2, -2, -2);
                if (fillBounds.width() > 0 && fillBounds.height() > 0) {
                    QRect fillRect = fillBounds;
                    fillRect.setWidth(std::max(1, static_cast<int>(std::round(fillBounds.width() * std::clamp(percent, 0.0, 1.0)))));
                    painter->setBrush(fillColor);
                    painter->drawRoundedRect(fillRect, fillRadius, fillRadius);
                }
            }

            painter->restore();
        }
    }
};

class LegendTypeDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QColor swatchColor = index.data(kLegendColorRole).value<QColor>();

        opt.icon = QIcon();
        const qreal swatchSize = std::min<qreal>(option.rect.height() - 8.0, 11.0);
        const qreal leftPadding = 6.0;
        const qreal textGap = 8.0;
        const int reservedLeft = qRound(leftPadding + swatchSize + textGap);
        opt.rect.adjust(reservedLeft, 0, 0, 0);
        QStyledItemDelegate::paint(painter, opt, index);

        if (!swatchColor.isValid()) {
            return;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const qreal top = option.rect.top() + ((option.rect.height() - swatchSize) / 2.0);
        const QRectF swatchRect(option.rect.left() + leftPadding, top, swatchSize, swatchSize);
        painter->setPen(QPen(swatchColor.darker(140), 1.0));
        painter->setBrush(swatchColor);
        painter->drawRoundedRect(swatchRect, 2.0, 2.0);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        return QStyledItemDelegate::sizeHint(option, index);
    }
};

class DirectoryTreeItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override
    {
        const QTreeWidget* owner = treeWidget();
        const int column = owner ? owner->sortColumn() : 0;
        const QVariant leftValue = data(column, kDirectorySortValueRole);
        const QVariant rightValue = other.data(column, kDirectorySortValueRole);
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

class DeleteActionModifierWatcher : public QObject {
public:
    explicit DeleteActionModifierWatcher(QAction* action, QObject* parent = nullptr)
        : QObject(parent), m_action(action)
    {
        updateLabel((QApplication::keyboardModifiers() & Qt::ShiftModifier) != 0);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::KeyPress: {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            const bool shiftHeld = keyEvent->key() == Qt::Key_Shift
                ? true
                : (keyEvent->modifiers() & Qt::ShiftModifier);
            updateLabel(shiftHeld);
            break;
        }
        case QEvent::KeyRelease: {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            const bool shiftHeld = keyEvent->key() == Qt::Key_Shift
                ? false
                : (keyEvent->modifiers() & Qt::ShiftModifier);
            updateLabel(shiftHeld);
            break;
        }
        case QEvent::ShortcutOverride:
        case QEvent::Show:
        case QEvent::WindowActivate:
            updateLabel((QApplication::keyboardModifiers() & Qt::ShiftModifier) != 0);
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void updateLabel(bool shiftHeld)
    {
        if (!m_action) {
            return;
        }
        m_action->setData(shiftHeld);
        m_action->setIcon(shiftHeld
            ? menuActionIcon({"edit-delete", "delete"},
                QStringLiteral(":/assets/tabler-icons/trash-x.svg"),
                QStringLiteral(":/assets/tabler-icons/trash-x.svg"),
                QStyle::SP_TrashIcon)
            : menuActionIcon({"user-trash", "trash-empty", "edit-delete"},
                QStringLiteral(":/assets/tabler-icons/trash.svg"),
                QStringLiteral(":/assets/tabler-icons/trash.svg"),
                QStyle::SP_TrashIcon));
        m_action->setText(shiftHeld
            ? QCoreApplication::translate("MainWindow", "Delete Permanently")
            : QCoreApplication::translate("MainWindow", "Move to Wastebin"));
    }

    QAction* m_action = nullptr;
};

}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    qApp->installEventFilter(this);
    QSettings store = appSettings();
    m_settings = TreemapSettings::load(store);
    if (m_settings.followSystemColorTheme) {
        m_settings.activeColorThemeId = m_settings.colorThemeIdForSystemScheme(widgetChromeUsesDarkColorScheme());
        m_settings.sanitize();
    }
    m_showFreeSpaceInOverview = store.value("treemap/showFreeSpaceInOverview", m_showFreeSpaceInOverview).toBool();
    m_showDirectoryTree = store.value("treemap/showDirectoryTree", m_showDirectoryTree).toBool();
    m_showTypeLegend = store.value("treemap/showTypeLegend", m_showTypeLegend).toBool();
    m_directoryPanelWidth = store.value("treemap/directoryPanelWidth", m_directoryPanelWidth).toInt();
    m_typeLegendPanelWidth = store.value("treemap/typeLegendPanelWidth", m_typeLegendPanelWidth).toInt();
    m_permissionErrorPanelWidth = store.value("treemap/permissionPanelWidth", m_permissionErrorPanelWidth).toInt();
    loadRecentPaths(store);
    loadFavouritePaths(store);

    setWindowTitle(QStringLiteral("Diskscape"));
    resize(1200, 800);

    setupToolbar(store);
    setupCentralWidget(store);
    setupBackend();
}

void MainWindow::setupToolbar(QSettings& store)
{
    // Toolbar
    m_toolbar = addToolBar("Main");
    m_toolbar->setMovable(false);
    m_toolbar->setFocusPolicy(Qt::NoFocus);
    m_toolbar->setIconSize(QSize(24, 24));
#ifdef Q_OS_MACOS
    setUnifiedTitleAndToolBarOnMac(true);
#endif

    const auto setActionTooltip = [](QAction* action, const QString& text) {
        if (action) {
            action->setToolTip(text);
            action->setStatusTip(text);
        }
    };

    m_scanCustomAction = new QAction(
        toolbarIcon({"folder-open", "document-open"},
            QStringLiteral(":/assets/tabler-icons/folder-open.svg")),
        tr("Open"),
        this);
    m_scanCustomAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+O")));
    setActionTooltip(m_scanCustomAction, tr("Choose a new folder to scan"));
    addAction(m_scanCustomAction);
    connect(m_scanCustomAction, &QAction::triggered, this, &MainWindow::openDirectory);

    m_homeAction = m_toolbar->addAction(
        toolbarIcon({"user-home"},
            QStringLiteral(":/assets/tabler-icons/home.svg")),
        tr("Home"));
    m_homeAction->setShortcuts({
        QKeySequence(QStringLiteral("Ctrl+Alt+H")),
        QKeySequence(Qt::Key_Home)
    });
    setActionTooltip(m_homeAction, tr("Return to the home screen"));
    connect(m_homeAction, &QAction::triggered, this, &MainWindow::returnToLanding);

    m_openLocationMenu = new QMenu(this);
    connect(m_openLocationMenu, &QMenu::aboutToShow, this, [this]() {
        populateOpenLocationMenu(m_openLocationMenu);
    });

    auto* openButton = new QToolButton(this);
    openButton->setDefaultAction(m_scanCustomAction);
    openButton->setMenu(m_openLocationMenu);
    openButton->setPopupMode(QToolButton::MenuButtonPopup);
    openButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    openButton->setFocusPolicy(Qt::NoFocus);
    openButton->setToolTip(tr("Choose a new folder to scan"));
    m_toolbar->addWidget(openButton);

    m_toolbar->addSeparator();

    m_backAction = m_toolbar->addAction(
        toolbarIcon({"go-previous", "arrow-left"},
            QStringLiteral(":/assets/tabler-icons/chevron-left.svg")),
        tr("Back"));
    m_backAction->setShortcuts({
        QKeySequence(Qt::ALT | Qt::Key_Left),
        QKeySequence::Back,
        QKeySequence(Qt::Key_Back)
    });
    m_backAction->setEnabled(false);
    setActionTooltip(m_backAction, tr("Go back to the previous location"));
    connect(m_backAction, &QAction::triggered, this, &MainWindow::navigateBack);

    m_upAction = m_toolbar->addAction(
        toolbarIcon({"go-up", "arrow-up"},
            QStringLiteral(":/assets/tabler-icons/chevron-up.svg")),
        tr("Up"));
    m_upAction->setShortcuts({QKeySequence(Qt::ALT | Qt::Key_Up), QKeySequence(Qt::CTRL | Qt::Key_Up)});
    m_upAction->setEnabled(false);
    setActionTooltip(m_upAction, tr("Move up to the parent folder"));
    connect(m_upAction, &QAction::triggered, this, &MainWindow::navigateUp);

    m_refreshAction = m_toolbar->addAction(
        toolbarIcon({"view-refresh", "refresh"},
            QStringLiteral(":/assets/tabler-icons/refresh.svg")),
        tr("Refresh"));
    m_refreshAction->setShortcuts({
        QKeySequence(Qt::Key_F5),
        QKeySequence::Refresh,
        QKeySequence(Qt::CTRL | Qt::Key_R),
        QKeySequence(Qt::Key_Refresh)
    });
    setActionTooltip(m_refreshAction, tr("Refresh the current scan"));
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::onRefreshActionTriggered);
    if (QToolButton* refreshButton = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_refreshAction))) {
        refreshButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }

#ifndef Q_OS_WIN
    m_limitToSameFilesystemAction = new QAction(
        toolbarIcon({"freeze-row-column"},
            QStringLiteral(":/assets/tabler-icons/freeze-row-column.svg")),
        tr("Single Filesystem"),
        this);
    m_limitToSameFilesystemAction->setCheckable(true);
    m_limitToSameFilesystemAction->setChecked(m_settings.limitToSameFilesystem);
    m_limitToSameFilesystemAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+S")));
    setActionTooltip(m_limitToSameFilesystemAction,
        tr("Stay on the current filesystem while scanning (refresh required)"));
    connect(m_limitToSameFilesystemAction, &QAction::toggled, this, &MainWindow::onLimitToSameFilesystemToggled);
#endif

    m_toolbar->addSeparator();

    m_zoomInAction = m_toolbar->addAction(
        toolbarIcon({"zoom-in"},
            QStringLiteral(":/assets/tabler-icons/zoom-in.svg")),
        tr("Zoom In"));
    m_zoomInAction->setShortcuts({
        QKeySequence(Qt::CTRL | Qt::Key_Equal),
        QKeySequence(Qt::CTRL | Qt::Key_Plus),
        QKeySequence::ZoomIn,
        QKeySequence(Qt::Key_ZoomIn)
    });
    setActionTooltip(m_zoomInAction, tr("Zoom in on the treemap"));
    connect(m_zoomInAction, &QAction::triggered, this, &MainWindow::zoomInCentered);

    m_zoomOutAction = m_toolbar->addAction(
        toolbarIcon({"zoom-out"},
            QStringLiteral(":/assets/tabler-icons/zoom-out.svg")),
        tr("Zoom Out"));
    m_zoomOutAction->setShortcuts({
        QKeySequence(QStringLiteral("Ctrl+-")),
        QKeySequence(QStringLiteral("Ctrl+Shift+-")),
        QKeySequence(QStringLiteral("Ctrl+_")),
        QKeySequence(QStringLiteral("Ctrl+KP_Subtract")),
        QKeySequence(Qt::CTRL | Qt::Key_Minus),
        QKeySequence::ZoomOut,
        QKeySequence(Qt::Key_ZoomOut)
    });
    setActionTooltip(m_zoomOutAction, tr("Zoom out from the treemap"));
    connect(m_zoomOutAction, &QAction::triggered, this, &MainWindow::zoomOutCentered);

    m_resetZoomAction = m_toolbar->addAction(
        toolbarIcon({"zoom-original", "zoom-fit-best"},
            QStringLiteral(":/assets/tabler-icons/zoom-reset.svg")),
        tr("Reset Zoom"));
    m_resetZoomAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_0)});
    setActionTooltip(m_resetZoomAction, tr("Reset the treemap zoom level"));
    connect(m_resetZoomAction, &QAction::triggered, this, &MainWindow::resetZoom);

    m_toolbar->addSeparator();

#ifndef Q_OS_WIN
    if (m_limitToSameFilesystemAction) {
        m_toolbar->addAction(m_limitToSameFilesystemAction);
        if (QToolButton* singleFilesystemButton = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_limitToSameFilesystemAction))) {
            singleFilesystemButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
    }
#endif

    m_toggleFreeSpaceAction = m_toolbar->addAction(
        toolbarIcon({"chart-donut"},
            QStringLiteral(":/assets/tabler-icons/chart-donut.svg")),
        tr("Free Space"));
    m_toggleFreeSpaceAction->setCheckable(true);
    m_toggleFreeSpaceAction->setChecked(m_showFreeSpaceInOverview);
    m_toggleFreeSpaceAction->setEnabled(false);
    m_toggleFreeSpaceAction->setVisible(false);
    m_toggleFreeSpaceAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+F")));
    setActionTooltip(m_toggleFreeSpaceAction, tr("Show or hide free space in the overview"));
    connect(m_toggleFreeSpaceAction, &QAction::toggled,
            this, &MainWindow::toggleFreeSpaceView);
    if (QToolButton* freeSpaceButton = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_toggleFreeSpaceAction))) {
        freeSpaceButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }

    QIcon legendIcon = toolbarIcon({"view-list-details", "view-list"},
        QStringLiteral(":/assets/tabler-icons/list-details.svg"));
    m_toggleDirectoryTreeAction = m_toolbar->addAction(
        toolbarIcon({"folder"},
            QStringLiteral(":/assets/tabler-icons/folders.svg")),
        tr("Tree"));
    m_toggleDirectoryTreeAction->setCheckable(true);
    m_toggleDirectoryTreeAction->setChecked(m_showDirectoryTree);
    m_toggleDirectoryTreeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+T")));
    setActionTooltip(m_toggleDirectoryTreeAction, tr("Show or hide the directory tree"));
    if (QToolButton* treeButton = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_toggleDirectoryTreeAction))) {
        treeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }

    m_toggleTypeLegendAction = m_toolbar->addAction(legendIcon, tr("File Types"));
    m_toggleTypeLegendAction->setCheckable(true);
    m_toggleTypeLegendAction->setChecked(m_showTypeLegend);
    m_toggleTypeLegendAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+L")));
    setActionTooltip(m_toggleTypeLegendAction, tr("Show or hide the file types panel"));
    if (QToolButton* legendButton = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_toggleTypeLegendAction))) {
        legendButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }

    m_toolbar->addSeparator();

    QIcon settingsIcon = toolbarIcon({"settings-configure", "preferences-system"},
        QStringLiteral(":/assets/tabler-icons/settings.svg"));
    m_settingsAction = m_toolbar->addAction(settingsIcon, tr("Settings"));
    m_settingsAction->setShortcuts({QKeySequence(QStringLiteral("Ctrl+,")), QKeySequence(QStringLiteral("Ctrl+Alt+,"))});
    setActionTooltip(m_settingsAction, tr("Open application settings"));
    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::openSettings);

    m_aboutAppAction = new QAction(tr("About Diskscape"), this);
    connect(m_aboutAppAction, &QAction::triggered, this, [this]() {
        QMessageBox::about(
            this,
            QStringLiteral("About Diskscape"),
            tr("Diskscape %1\n\n"
               "Visualises disk usage as an interactive treemap.\n\n"
               "Copyright \u00a9 2026 dvdavd\n\n"
               "Bundled icons derived from Tabler Icons are Copyright \u00a9 2020-2026 "
               "Pawe\u0142 Kuna and licensed under the MIT License.\n\n"
               "Diskscape is licensed under the MIT License.")
                .arg(QCoreApplication::applicationVersion()));
    });

    m_aboutQtAction = new QAction(tr("About Qt"), this);
    connect(m_aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);

    m_pathBar = new BreadcrumbPathBar(this);
    m_pathBar->setMinimumWidth(360);
    m_pathBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_pathBar->setPlaceholderText(tr("Enter a directory path"));
    m_pathBar->setRecentPaths(m_recentPaths);
    auto* dirModel = m_pathBar->completionModel();
    connect(m_pathBar, &BreadcrumbPathBar::pathActivated, this, [this](const QString& path, bool forceScan) {
        activatePath(path, forceScan);
    });
    connect(m_pathBar->lineEdit(), &QLineEdit::textEdited, this, [dirModel](const QString& text) {
        const QString trimmed = text.trimmed();
        QStringList completions;
        QString basePath = trimmed;
        QString typedLeaf;

        if (basePath.isEmpty()) {
            basePath = QDir::homePath();
        }

        QFileInfo info(basePath);
        if (!info.isDir()) {
            typedLeaf = info.fileName();
            basePath = info.absolutePath();
        } else {
            basePath = info.absoluteFilePath();
        }

        QDir dir(basePath);
        const QFileInfoList entries = dir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name | QDir::IgnoreCase);
        completions.reserve(entries.size());
        for (const QFileInfo& entry : entries) {
            if (!typedLeaf.isEmpty() && !entry.fileName().startsWith(typedLeaf, Qt::CaseInsensitive)) {
                continue;
            }
            completions.push_back(entry.absoluteFilePath());
        }
        dirModel->setStringList(completions);
    });
    auto* toolbarStretch = new QWidget(this);
    toolbarStretch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbarStretch->setFocusPolicy(Qt::NoFocus);
    m_toolbar->addWidget(toolbarStretch);

    m_permissionWarningIcon = toolbarIcon({"dialog-warning"},
        QStringLiteral(":/assets/tabler-icons/alert-triangle.svg"));

    m_permissionWarningAction = m_toolbar->addAction(
        m_permissionWarningIcon, tr("Scan Warnings"));
    m_permissionWarningAction->setCheckable(true);
    m_permissionWarningAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+W")));
    setActionTooltip(m_permissionWarningAction,
        tr("Show directories skipped because they could not be read"));
    m_permissionWarningAction->setVisible(false);
    connect(m_permissionWarningAction, &QAction::toggled, this, [this](bool checked) {
        m_showPermissionPanel = checked;
        rebuildTreemapSplitterLayout();
    });

    auto* searchSpacer = new QWidget(this);
    searchSpacer->setFixedWidth(12);
    searchSpacer->setFocusPolicy(Qt::NoFocus);
    m_toolbar->addWidget(searchSpacer);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->installEventFilter(this);
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setPlaceholderText(searchPatternPlaceholderText());
    m_searchEdit->setFixedWidth(190);
    m_searchEdit->setToolTip(tr("Type a filename pattern and press Enter to search"));
    m_pathBar->setFixedHeight(m_searchEdit->sizeHint().height());
    updatePathBarChrome();
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MainWindow::applySearchFromToolbar);
    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!m_treemapWidget) {
            return;
        }
        if (text.trimmed().isEmpty()) {
            if (m_searchDebounceTimer) m_searchDebounceTimer->stop();
            m_treemapWidget->setSearchPattern(QString());
            setSearchBusy(false);
            return;
        }
        if (m_searchDebounceTimer) m_searchDebounceTimer->start();
    });
    m_toolbar->addWidget(m_searchEdit);

    auto* sizeFilterSpacer = new QWidget(this);
    sizeFilterSpacer->setFixedWidth(6);
    sizeFilterSpacer->setFocusPolicy(Qt::NoFocus);
    m_toolbar->addWidget(sizeFilterSpacer);

    m_sizeFilterCombo = new QComboBox(this);
    m_sizeFilterCombo->installEventFilter(this);
    m_sizeFilterCombo->setToolTip(tr("Filter by file/folder size"));
    m_sizeFilterCombo->addItem(tr("Any size"),        QVariantList{0LL, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 1 MB"),     QVariantList{1LL<<20, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 10 MB"),    QVariantList{10LL<<20, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 100 MB"),   QVariantList{100LL<<20, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 1 GB"),     QVariantList{1LL<<30, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 5 GB"),     QVariantList{5LL<<30, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 10 GB"),    QVariantList{10LL<<30, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 20 GB"),    QVariantList{20LL<<30, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 50 GB"),    QVariantList{50LL<<30, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 100 GB"),   QVariantList{100LL<<30, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 500 GB"),   QVariantList{500LL<<30, 0LL});
    m_sizeFilterCombo->addItem(tr("\u2265 1 TB"),     QVariantList{1LL<<40, 0LL});
    connect(m_sizeFilterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::applySearchFromToolbar);
    updateSizeFilterChrome();
    m_toolbar->addWidget(m_sizeFilterCombo);

    auto* menuSpacer = new QWidget(this);
    menuSpacer->setFixedWidth(10);
    menuSpacer->setFocusPolicy(Qt::NoFocus);
    m_toolbar->addWidget(menuSpacer);

    auto* toolbarMenu = new QMenu(this);
    toolbarMenu->addAction(m_homeAction);
    toolbarMenu->addAction(m_scanCustomAction);
    toolbarMenu->addSeparator();
    toolbarMenu->addAction(m_backAction);
    toolbarMenu->addAction(m_upAction);
    toolbarMenu->addAction(m_refreshAction);
    toolbarMenu->addSeparator();
    toolbarMenu->addAction(m_zoomInAction);
    toolbarMenu->addAction(m_zoomOutAction);
    toolbarMenu->addAction(m_resetZoomAction);
    toolbarMenu->addSeparator();
#ifndef Q_OS_WIN
    if (m_limitToSameFilesystemAction) {
        toolbarMenu->addAction(m_limitToSameFilesystemAction);
    }
#endif
    toolbarMenu->addAction(m_toggleFreeSpaceAction);
    toolbarMenu->addAction(m_toggleDirectoryTreeAction);
    toolbarMenu->addAction(m_toggleTypeLegendAction);
    m_warningsMenuAction = toolbarMenu->addAction(tr("Warnings"));
    m_warningsMenuAction->setCheckable(true);
    m_warningsMenuAction->setShortcut(m_permissionWarningAction->shortcut());
    m_warningsMenuAction->setVisible(false);
    m_warningsMenuAction->setIcon(m_permissionWarningIcon);
    toolbarMenu->addSeparator();
    toolbarMenu->addAction(m_settingsAction);
    toolbarMenu->addSeparator();
    toolbarMenu->addAction(m_aboutAppAction);
    toolbarMenu->addAction(m_aboutQtAction);

#ifndef Q_OS_MACOS
    m_menuButton = new QToolButton(this);
    m_menuButton->setAutoRaise(true);
    m_menuButton->setPopupMode(QToolButton::InstantPopup);
    m_menuButton->setIcon(toolbarIcon({"application-menu", "open-menu"},
        QStringLiteral(":/assets/tabler-icons/menu-2.svg")));
    m_menuButton->setToolTip(tr("Open the application menu (F10)"));
    m_menuButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_menuButton->setFocusPolicy(Qt::NoFocus);
    m_menuButton->setStyleSheet(QStringLiteral(
        "QToolButton::menu-indicator {"
        "  image: none;"
        "  width: 0px;"
        "}"
    ));
    m_menuButton->setMenu(toolbarMenu);
    m_toolbar->addWidget(m_menuButton);
#endif

    connect(m_permissionWarningAction, &QAction::toggled, this, [this](bool checked) {
        if (!m_warningsMenuAction || m_warningsMenuAction->isChecked() == checked) {
            return;
        }
        m_warningsMenuAction->setChecked(checked);
    });
    connect(m_warningsMenuAction, &QAction::toggled, this, [this](bool checked) {
        if (!m_permissionWarningAction || m_permissionWarningAction->isChecked() == checked) {
            return;
        }
        m_permissionWarningAction->setChecked(checked);
    });

    auto* openMenuAction = new QAction(this);
    openMenuAction->setShortcuts({
        QKeySequence(Qt::Key_F10),
        QKeySequence(Qt::Key_Menu)
    });
    openMenuAction->setShortcutContext(Qt::WindowShortcut);
    addAction(openMenuAction);
    connect(openMenuAction, &QAction::triggered, this, [this]() {
        if (!m_menuButton) {
            return;
        }
        m_menuButton->setFocus(Qt::ShortcutFocusReason);
        m_menuButton->showMenu();
    });

    const auto toolbarButtons = m_toolbar->findChildren<QToolButton*>();
    for (QToolButton* button : toolbarButtons) {
        if (button != m_menuButton) {
            button->setFocusPolicy(Qt::NoFocus);
        }
    }

    updateToolbarChrome();
    updateToolbarResponsiveLayout();

#ifdef Q_OS_MACOS
    // On macOS, the native menu bar replaces the hamburger button
    m_aboutAppAction->setMenuRole(QAction::AboutRole);
    m_aboutQtAction->setMenuRole(QAction::AboutQtRole);
    m_settingsAction->setMenuRole(QAction::PreferencesRole);

    auto* mb = menuBar();

    auto* macFileMenu = mb->addMenu(tr("File"));
    macFileMenu->addAction(m_scanCustomAction);
    macFileMenu->addAction(m_homeAction);
    macFileMenu->addSeparator();
    macFileMenu->addAction(m_settingsAction);

    auto* macNavigateMenu = mb->addMenu(tr("Navigate"));
    macNavigateMenu->addAction(m_backAction);
    macNavigateMenu->addAction(m_upAction);
    macNavigateMenu->addAction(m_refreshAction);

    auto* macViewMenu = mb->addMenu(tr("View"));
    macViewMenu->addAction(m_zoomInAction);
    macViewMenu->addAction(m_zoomOutAction);
    macViewMenu->addAction(m_resetZoomAction);
    macViewMenu->addSeparator();
    if (m_limitToSameFilesystemAction) {
        macViewMenu->addAction(m_limitToSameFilesystemAction);
    }
    macViewMenu->addAction(m_toggleFreeSpaceAction);
    macViewMenu->addAction(m_toggleDirectoryTreeAction);
    macViewMenu->addAction(m_toggleTypeLegendAction);
    macViewMenu->addAction(m_permissionWarningAction);

    auto* macHelpMenu = mb->addMenu(tr("Help"));
    macHelpMenu->addAction(m_aboutAppAction);
    macHelpMenu->addAction(m_aboutQtAction);
#endif
}

void MainWindow::setupCentralWidget(QSettings& store)
{
    // Central widget
    m_landingPage = createLandingPage();
    updateLandingPageChrome();
    m_treemapPage = new QWidget(this);
    auto* treemapPageLayout = new QVBoxLayout(m_treemapPage);
    treemapPageLayout->setContentsMargins(0, 0, 0, 0);
    treemapPageLayout->setSpacing(0);

    m_pathBar->setParent(m_treemapPage);
    treemapPageLayout->addWidget(m_pathBar);

    m_directoryPanel = new QWidget(m_treemapPage);
    m_directoryPanel->setObjectName(QStringLiteral("directoryPanel"));
    m_directoryPanel->setMinimumWidth(0);
    m_directoryPanel->hide();
    auto* directoryLayout = new QVBoxLayout(m_directoryPanel);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->setSpacing(0);

    m_directoryTree = new QTreeWidget(m_directoryPanel);
    m_directoryTree->setColumnCount(3);
    m_directoryTree->setHeaderLabels({tr("Directory"),
                                      tr("Relative"),
                                      tr("Size")});
    m_directoryTree->setUniformRowHeights(true);
    m_directoryTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_directoryTree->setFocusPolicy(Qt::StrongFocus);
    m_directoryTree->setFrameShape(QFrame::NoFrame);
    m_directoryTree->setRootIsDecorated(true);
    m_directoryTree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_directoryTree->header()->setStretchLastSection(false);
    m_directoryTree->header()->setSectionsClickable(true);
    m_directoryTree->header()->setSectionsMovable(false);
    m_directoryTree->header()->setSortIndicatorShown(true);
    m_directoryTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_directoryTree->setItemDelegateForColumn(1, new DirectoryUsageBarDelegate(m_directoryTree));
    m_directoryTree->setSortingEnabled(true);
    m_directoryTree->header()->setSortIndicator(1, Qt::DescendingOrder);
    m_directoryTree->viewport()->installEventFilter(this);
    const QByteArray directoryHeaderState = store.value("treemap/directoryTreeHeaderState").toByteArray();
    if (!directoryHeaderState.isEmpty()) {
        m_directoryTree->header()->restoreState(directoryHeaderState);
    } else {
        m_directoryTree->setColumnWidth(0, 180);
        m_directoryTree->setColumnWidth(1, 110);
        m_directoryTree->setColumnWidth(2, 96);
    }
    directoryLayout->addWidget(m_directoryTree, 1);

    m_treemapPane = new QWidget(m_treemapPage);
    m_treemapPane->hide();
    auto* treemapPaneLayout = new QVBoxLayout(m_treemapPane);
    treemapPaneLayout->setContentsMargins(0, 0, 0, 0);
    treemapPaneLayout->setSpacing(0);

    m_treemapWidget = new TreemapWidget(m_treemapPane);
    m_treemapWidget->applySettings(m_settings);
    m_treemapWidget->setSearchPattern(m_searchEdit ? m_searchEdit->text() : QString());
    treemapPaneLayout->addWidget(m_treemapWidget, 1);
    connect(m_treemapWidget, &TreemapWidget::searchResultsChanged,
            this, [this]() {
                refreshTypeLegendAsync(m_treemapWidget ? m_treemapWidget->currentNode()
                                                       : m_scanResult.root);
            });
    connect(m_treemapWidget, &TreemapWidget::fileTypeHighlightBusyChanged, this, [this](bool busy) {
        if (busy) {
            setCursor(Qt::BusyCursor);
        } else if (!m_scanInProgress && !m_incrementalRefreshInProgress && !m_postProcessInProgress) {
            unsetCursor();
        }
    });

    m_typeLegendPanel = new QWidget(m_treemapPage);
    m_typeLegendPanel->setObjectName(QStringLiteral("typeLegendPanel"));
    m_typeLegendPanel->setMinimumWidth(0);
    m_typeLegendPanel->hide();
    auto* legendLayout = new QVBoxLayout(m_typeLegendPanel);
    legendLayout->setContentsMargins(0, 0, 0, 0);
    legendLayout->setSpacing(0);

    m_typeLegendTree = new QTreeWidget(m_typeLegendPanel);
    m_typeLegendTree->setColumnCount(3);
    m_typeLegendTree->setHeaderLabels({tr("Type"),
                                       tr("Size"),
                                       tr("Count")});
    m_typeLegendTree->setRootIsDecorated(false);
    m_typeLegendTree->setUniformRowHeights(true);
    m_typeLegendTree->setAlternatingRowColors(false);
    m_typeLegendTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_typeLegendTree->setFocusPolicy(Qt::StrongFocus);
    m_typeLegendTree->setFrameShape(QFrame::NoFrame);
    m_typeLegendTree->setIndentation(0);
    m_typeLegendTree->header()->setStretchLastSection(false);
    m_typeLegendTree->header()->setSectionsClickable(true);
    m_typeLegendTree->header()->setSectionsMovable(false);
    m_typeLegendTree->header()->setSortIndicatorShown(true);
    m_typeLegendTree->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_typeLegendTree->setSortingEnabled(true);
    m_typeLegendTree->header()->setSortIndicator(1, Qt::DescendingOrder);
    m_typeLegendTree->setItemDelegateForColumn(0, new LegendTypeDelegate(m_typeLegendTree));
    legendLayout->addWidget(m_typeLegendTree, 1);

    m_permissionErrorPanel = new QWidget(m_treemapPage);
    m_permissionErrorPanel->setObjectName(QStringLiteral("permissionErrorPanel"));
    m_permissionErrorPanel->setMinimumWidth(0);
    m_permissionErrorPanel->hide();
    auto* permissionErrorLayout = new QVBoxLayout(m_permissionErrorPanel);
    permissionErrorLayout->setContentsMargins(0, 0, 0, 0);
    permissionErrorLayout->setSpacing(0);

    m_permissionErrorList = new QListWidget(m_permissionErrorPanel);
    m_permissionErrorList->setFrameShape(QFrame::NoFrame);
    m_permissionErrorList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_permissionErrorList->setUniformItemSizes(true);
    m_permissionErrorList->setFocusPolicy(Qt::StrongFocus);
    permissionErrorLayout->addWidget(m_permissionErrorList, 1);

    connect(m_toggleDirectoryTreeAction, &QAction::toggled, this, [this](bool checked) {
        m_showDirectoryTree = checked;
        rebuildTreemapSplitterLayout();
    });
    connect(m_toggleTypeLegendAction, &QAction::toggled, this, [this](bool checked) {
        m_showTypeLegend = checked;
        rebuildTreemapSplitterLayout();
    });
    const QByteArray headerState = store.value("treemap/typeLegendHeaderState").toByteArray();
    if (!headerState.isEmpty()) {
        m_typeLegendTree->header()->restoreState(headerState);
    } else {
        m_typeLegendTree->setColumnWidth(0, 140);
        m_typeLegendTree->setColumnWidth(1, 90);
        m_typeLegendTree->setColumnWidth(2, 70);
    }
    connect(m_typeLegendTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (m_scanInProgress && !m_backgroundRefreshInProgress) {
            return;
        }
        if (!m_treemapWidget) {
            return;
        }
        const QString typeLabel = item ? item->data(0, Qt::UserRole).toString() : QString();
        const bool clearing = !typeLabel.isEmpty() && typeLabel == m_treemapWidget->highlightedFileType();
        m_treemapWidget->setHighlightedFileType(clearing ? QString() : typeLabel);
        if (clearing && m_typeLegendTree) {
            m_typeLegendTree->clearSelection();
            m_typeLegendTree->setCurrentItem(nullptr);
        }
    });
    connect(m_directoryTree, &QTreeWidget::itemExpanded, this, &MainWindow::populateDirectoryTreeChildren);
    connect(m_directoryTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item || !m_scanResult.root || !m_treemapWidget) {
            return;
        }
        QString path = item->data(0, kDirectoryPathRole).toString();
        if (path.isEmpty() && item->data(0, kDirectorySyntheticFilesRole).toBool() && item->parent()) {
            path = item->parent()->data(0, kDirectoryPathRole).toString();
        }
        if (path.isEmpty()) {
            return;
        }
        if (FileNode* node = findNodeByPath(m_scanResult.root, path)) {
            navigateTo(node, true);
        }
    });
    connect(m_directoryTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (!m_directoryTree || !m_scanResult.root) {
            return;
        }
        QTreeWidgetItem* item = m_directoryTree->itemAt(pos);
        if (!item) {
            return;
        }
        QString path = item->data(0, kDirectoryPathRole).toString();
        if (path.isEmpty() && item->data(0, kDirectorySyntheticFilesRole).toBool() && item->parent()) {
            path = item->parent()->data(0, kDirectoryPathRole).toString();
        }
        if (path.isEmpty()) {
            return;
        }
        if (FileNode* node = findNodeByPath(m_scanResult.root, path)) {
            onNodeContextMenuRequested(node, m_directoryTree->viewport()->mapToGlobal(pos));
        }
    });
    m_centralStack = new QStackedWidget(this);
    m_centralStack->addWidget(m_landingPage);
    m_centralStack->addWidget(m_treemapPage);
    setCentralWidget(m_centralStack);
    setLandingVisible(true);
    rebuildTreemapSplitterLayout();
    updateDirectoryTreePanel();
    updateTypeLegendPanel();

    connect(m_treemapWidget, &TreemapWidget::nodeActivated,
            this, &MainWindow::onNodeActivated);
    connect(m_treemapWidget, &TreemapWidget::nodeHovered,
            this, &MainWindow::onNodeHovered);
    connect(m_treemapWidget, &TreemapWidget::backRequested,
            this, &MainWindow::navigateBack);
    connect(m_treemapWidget, &TreemapWidget::zoomInRequested,
            this, &MainWindow::onZoomInRequested);
    connect(m_treemapWidget, &TreemapWidget::zoomOutRequested,
            this, &MainWindow::onZoomOutRequested);
    connect(m_treemapWidget, &TreemapWidget::nodeContextMenuRequested,
            this, &MainWindow::onNodeContextMenuRequested);
}

void MainWindow::captureTreemapPanelWidths()
{
    if (!m_treemapSplitter) {
        return;
    }

    const QList<int> sizes = m_treemapSplitter->sizes();
    const int widgetCount = m_treemapSplitter->count();
    for (int i = 0; i < widgetCount && i < sizes.size(); ++i) {
        QWidget* const widget = m_treemapSplitter->widget(i);
        const int size = sizes.at(i);
        if (size <= 0) {
            continue;
        }
        if (widget == m_directoryPanel) {
            m_directoryPanelWidth = size;
        } else if (widget == m_typeLegendPanel) {
            m_typeLegendPanelWidth = size;
        } else if (widget == m_permissionErrorPanel) {
            m_permissionErrorPanelWidth = size;
        }
    }
}

void MainWindow::rebuildTreemapSplitterLayout()
{
    if (!m_treemapPage || !m_treemapPane) {
        return;
    }

    auto* pageLayout = qobject_cast<QVBoxLayout*>(m_treemapPage->layout());
    if (!pageLayout) {
        return;
    }

    QPointer<QWidget> previouslyFocused = QApplication::focusWidget();
    const bool restoreFocusAfterLayout = previouslyFocused
        && isAncestorOf(previouslyFocused)
        && previouslyFocused != m_treemapSplitter;

    const bool showDirectoryTree = m_showDirectoryTree && m_directoryPanel;
    const bool showTypeLegend = m_showTypeLegend && m_typeLegendPanel;
    const bool showPermissionPanel = m_showPermissionPanel && m_permissionErrorPanel;

    if (!m_treemapSplitter) {
        // First call only: create the splitter and add all panels in fixed order.
        // Subsequent calls just show/hide the side panels in-place to avoid
        // tearing down the splitter (which causes a white flash).
        m_treemapSplitter = new QSplitter(Qt::Horizontal, m_treemapPage);
        m_treemapSplitter->setChildrenCollapsible(false);

        if (m_directoryPanel) {
            m_treemapSplitter->addWidget(m_directoryPanel);
        }
        m_treemapSplitter->addWidget(m_treemapPane);
        if (m_typeLegendPanel) {
            m_treemapSplitter->addWidget(m_typeLegendPanel);
        }
        if (m_permissionErrorPanel) {
            m_treemapSplitter->addWidget(m_permissionErrorPanel);
        }

        connect(m_treemapSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
            captureTreemapPanelWidths();
        });

        pageLayout->addWidget(m_treemapSplitter, 1);
    }

    // Show/hide side panels; treemapPane is always visible.
    if (m_directoryPanel) {
        m_directoryPanel->setVisible(showDirectoryTree);
    }
    m_treemapPane->show();
    if (m_typeLegendPanel) {
        m_typeLegendPanel->setVisible(showTypeLegend);
    }
    if (m_permissionErrorPanel) {
        m_permissionErrorPanel->setVisible(showPermissionPanel);
    }

    // Stretch factor: treemapPane always expands; side panels stay at their preferred width.
    for (int i = 0; i < m_treemapSplitter->count(); ++i) {
        m_treemapSplitter->setStretchFactor(i, m_treemapSplitter->widget(i) == m_treemapPane ? 1 : 0);
    }

    const int directoryWidth = showDirectoryTree ? std::max(220, m_directoryPanelWidth) : 0;
    const int legendWidth = showTypeLegend ? std::max(220, m_typeLegendPanelWidth) : 0;
    const int permissionWidth = showPermissionPanel ? std::max(220, m_permissionErrorPanelWidth) : 0;
    const int availableWidth = std::max(1, m_treemapPage->width() - directoryWidth - legendWidth - permissionWidth);
    QList<int> sizes;
    if (m_directoryPanel) {
        sizes.push_back(showDirectoryTree ? directoryWidth : 0);
    }
    sizes.push_back(availableWidth);
    if (m_typeLegendPanel) {
        sizes.push_back(showTypeLegend ? legendWidth : 0);
    }
    if (m_permissionErrorPanel) {
        sizes.push_back(showPermissionPanel ? permissionWidth : 0);
    }
    m_treemapSplitter->setSizes(sizes);

    if (restoreFocusAfterLayout && previouslyFocused && previouslyFocused->isVisible()) {
        previouslyFocused->setFocus(Qt::OtherFocusReason);
    }
}

void MainWindow::setupBackend()
{
    // Status bar
    statusBar()->showMessage(tr("Open a directory to begin"));
    m_searchStatusLabel = new QLabel(tr("Searching..."), this);
    m_searchStatusLabel->setVisible(false);
    m_scanSeenStatusLabel = new QLabel(this);
    m_scanSeenStatusLabel->setVisible(false);
    m_scanPathStatusLabel = new QLabel(this);
    m_scanPathStatusLabel->setVisible(false);
    m_completedFilesStatusLabel = new QLabel(this);
    m_completedFilesStatusLabel->setVisible(false);
    m_completedTotalStatusLabel = new QLabel(this);
    m_completedTotalStatusLabel->setVisible(false);
    m_completedFreeStatusLabel = new QLabel(this);
    m_completedFreeStatusLabel->setVisible(false);
    m_scanPathStatusLabel->setTextFormat(Qt::PlainText);
    m_scanPathStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_scanPathStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_scanPathStatusLabel->setMinimumWidth(180);
    statusBar()->addPermanentWidget(m_scanSeenStatusLabel);
    statusBar()->addPermanentWidget(m_scanPathStatusLabel, 1);
    statusBar()->addPermanentWidget(m_completedFilesStatusLabel);
    statusBar()->addPermanentWidget(m_completedTotalStatusLabel);
    statusBar()->addPermanentWidget(m_completedFreeStatusLabel);
    statusBar()->addPermanentWidget(m_searchStatusLabel);

    // Future watcher
    m_watcher = new QFutureWatcher<ScanResult>(this);
    connect(m_watcher, &QFutureWatcher<ScanResult>::finished,
            this, &MainWindow::onScanFinished);

    m_refreshWatcher = new QFutureWatcher<ScanResult>(this);
    connect(m_refreshWatcher, &QFutureWatcher<ScanResult>::finished,
            this, &MainWindow::onIncrementalRefreshFinished);

    m_postProcessWatcher = new QFutureWatcher<IncrementalRefreshResult>(this);
    connect(m_postProcessWatcher, &QFutureWatcher<IncrementalRefreshResult>::finished,
            this, &MainWindow::onPostProcessFinished);

    m_searchDebounceTimer = new QTimer(this);
    m_searchDebounceTimer->setSingleShot(true);
    m_searchDebounceTimer->setInterval(150);
    connect(m_searchDebounceTimer, &QTimer::timeout,
            this, &MainWindow::applySearchFromToolbar);

    m_themeSettleTimer = new QTimer(this);
    m_themeSettleTimer->setSingleShot(true);
    m_themeSettleTimer->setInterval(0);
    connect(m_themeSettleTimer, &QTimer::timeout,
            this, &MainWindow::onThemeSettled);

    m_colorSchemeWatcher = new FreedesktopColorSchemeWatcher(this);
    connect(m_colorSchemeWatcher, &FreedesktopColorSchemeWatcher::colorSchemeChanged,
            this, &MainWindow::onFreedesktopColorSchemeChanged);

    m_watchController = new FilesystemWatchController(this);
    connect(m_watchController, &FilesystemWatchController::refreshRequested,
            this, &MainWindow::launchIncrementalRefresh);
    syncFilesystemWatchControllerState();
}

MainWindow::~MainWindow()
{
    if (qApp) {
        qApp->removeEventFilter(this);
    }
}

QMenu* MainWindow::createPopupMenu()
{
    return nullptr;
}

void MainWindow::syncFilesystemWatchControllerState()
{
    if (!m_watchController) {
        return;
    }

    m_watchController->setPaused(
        m_closeRequested
        || m_scanInProgress
        || m_incrementalRefreshInProgress
        || m_postProcessInProgress);
    m_watchController->setTreeContext(
        m_scanResult.root,
        m_treemapWidget ? m_treemapWidget->currentNode() : nullptr);
}

void MainWindow::openInitialPath(const QString& path)
{
    activatePath(path, false);
}

void MainWindow::updateToolbarChrome()
{
    if (!m_toolbar) {
        return;
    }

    m_toolbar->setAutoFillBackground(false);
    m_toolbar->setPalette(QPalette());
}

void MainWindow::updateSizeFilterChrome()
{
    if (!m_sizeFilterCombo) {
        return;
    }

    const QPalette palette = qApp ? qApp->palette() : QApplication::palette();
    m_sizeFilterCombo->setPalette(palette);
    if (QAbstractItemView* popupView = m_sizeFilterCombo->view()) {
        popupView->setPalette(palette);
    }
    m_sizeFilterCombo->setStyleSheet(QString());
}

void MainWindow::changeEvent(QEvent* event)
{
    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
    case QEvent::ThemeChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    case QEvent::DevicePixelRatioChange:
#endif
    case QEvent::ScreenChangeInternal: {
        QMainWindow::changeEvent(event);

        if (qApp) dumpThemeState("changeEvent:before", *qApp);
        const bool darkMode = systemUsesDarkColorScheme();
        if (qApp) {
            syncApplicationPaletteToColorScheme(*qApp, darkMode);
            applyMenuFontPolicy(*qApp);
        }
        const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
        const bool chromeDark = widgetChromeUsesDarkColorScheme();
        const bool trustChrome = scheme == Qt::ColorScheme::Unknown || chromeDark != darkMode;
        const bool treemapDark = trustChrome ? chromeDark : darkMode;
        // qDebug("[theme:changeEvent] eventType=%d  darkMode=%d  chromeDark=%d"
        //        "  trustChrome=%d  treemapDark=%d",
        //        (int)event->type(), darkMode, chromeDark, trustChrome, treemapDark);
        applyThemeChange(darkMode, treemapDark);
        break;
    }
    default:
        QMainWindow::changeEvent(event);
        break;
    }
}

void MainWindow::onThemeSettled()
{
    if (qApp) dumpThemeState("onThemeSettled", *qApp);
    if (m_toolbar) {
        updateToolbarChrome();
        m_toolbar->style()->unpolish(m_toolbar);
        m_toolbar->style()->polish(m_toolbar);
        const auto toolbarButtons = m_toolbar->findChildren<QToolButton*>();
        for (QToolButton* button : toolbarButtons) {
            button->style()->unpolish(button);
            button->style()->polish(button);
            button->updateGeometry();
        }
        if (auto* layout = m_toolbar->layout()) {
            layout->invalidate();
            layout->activate();
        }
        m_toolbar->updateGeometry();
        updateToolbarResponsiveLayout();
    }

    if (m_sizeFilterCombo) {
        const QPalette palette = qApp ? qApp->palette() : QApplication::palette();
        m_sizeFilterCombo->setPalette(palette);
        updateSizeFilterChrome();
        m_sizeFilterCombo->style()->unpolish(m_sizeFilterCombo);
        m_sizeFilterCombo->style()->polish(m_sizeFilterCombo);
        if (QAbstractItemView* popupView = m_sizeFilterCombo->view()) {
            popupView->setPalette(palette);
            popupView->style()->unpolish(popupView);
            popupView->style()->polish(popupView);
            popupView->viewport()->update();
        }
        m_sizeFilterCombo->updateGeometry();
        m_sizeFilterCombo->update();
    }

    if (m_directoryTree) {
        for (int i = 0; i < m_directoryTree->topLevelItemCount(); ++i)
            refreshDirectoryTreeIcons(m_directoryTree->topLevelItem(i));
        m_directoryTree->viewport()->update();
    }
    updateTypeLegendPanel();
}

void MainWindow::applyThemeChange(bool darkMode, bool treemapDark)
{
    if (qApp) {
        syncApplicationPaletteToColorScheme(*qApp, darkMode);
        applyMenuFontPolicy(*qApp);
    }
    syncColorThemeWithSystem(treemapDark, true);
    updatePathBarChrome();
    updateLandingPageChrome();
    updateToolbarChrome();
    updateSizeFilterChrome();
    clearIconCaches();
    updateToolbarIcons();
    if (m_themeSettleTimer)
        m_themeSettleTimer->start();
}

void MainWindow::onFreedesktopColorSchemeChanged(bool dark)
{
    if (qApp) dumpThemeState("freedesktop", *qApp);
    // qDebug("[theme:freedesktop] dark=%d", dark);
    applyThemeChange(dark, dark);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    if (m_treemapSplitter && m_treemapPage
            && m_treemapPage->isVisible() && m_treemapSplitter->count() > 0) {
        rebuildTreemapSplitterLayout();
    }
    updateToolbarResponsiveLayout();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if ((watched == m_searchEdit || watched == m_sizeFilterCombo || watched == m_pathBar)
            && (event->type() == QEvent::FontChange
                || event->type() == QEvent::ApplicationFontChange
                || event->type() == QEvent::StyleChange
                || event->type() == QEvent::PolishRequest)) {
        QTimer::singleShot(0, this, [this]() {
            updatePathBarChrome();
        });
    }

    if (m_landingPage && watched
            && watched->objectName() == QStringLiteral("landingLocationContainer")
            && event->type() == QEvent::Resize) {
        auto* container = qobject_cast<QWidget*>(watched);
        auto* layout = container ? qobject_cast<QVBoxLayout*>(container->layout()) : nullptr;
        const int availableWidth = (container && layout)
            ? qMax(1, container->width() - layout->contentsMargins().left() - layout->contentsMargins().right())
            : 1;
        const int desiredColumns = qMax(1, (availableWidth + kLandingTileSpacing)
                                           / (landingTileWidth() + kLandingTileSpacing));
        const int currentColumns = container ? container->property("landingColumns").toInt() : 0;
        const bool rebuildPending = container ? container->property("landingRebuildPending").toBool() : false;
        if (container && desiredColumns != currentColumns && !rebuildPending) {
            container->setProperty("landingRebuildPending", true);
            QTimer::singleShot(0, this, [this]() {
                relayoutLandingLocationSections();
            });
        }
    }

    if (m_directoryTree && watched == m_directoryTree->viewport()
            && event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (QTreeWidgetItem* item = m_directoryTree->itemAt(mouseEvent->position().toPoint())) {
            if (!item->data(0, kDirectoryLoadedRole).toBool()) {
                prepareDirectoryTreeItem(item);
            }
        }
    }

    if (m_directoryTree && watched == m_directoryTree
            && (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride)) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = keyEvent->key();
        const bool mightExpandCurrent = key == Qt::Key_Right
            || key == Qt::Key_Plus
            || key == Qt::Key_Return
            || key == Qt::Key_Enter
            || key == Qt::Key_Space;
        if (mightExpandCurrent) {
            if (QTreeWidgetItem* item = m_directoryTree->currentItem()) {
                if (!item->data(0, kDirectoryLoadedRole).toBool()) {
                    prepareDirectoryTreeItem(item);
                }
            }
        }
    }

    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (!keyEvent->isAutoRepeat() && keyEvent->key() == Qt::Key_Escape
                && (m_scanInProgress || m_incrementalRefreshInProgress || m_postProcessInProgress)) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }

            cancelRefreshOperation();
            event->accept();
            return true;
        }

        const Qt::KeyboardModifiers mods = keyEvent->modifiers();
        const bool ctrlOnly = (mods & Qt::ControlModifier)
            && !(mods & (Qt::AltModifier | Qt::MetaModifier));
        if (ctrlOnly) {
            const QString text = keyEvent->text();
            const bool isZoomIn = keyEvent->key() == Qt::Key_Equal
                || keyEvent->key() == Qt::Key_Plus
                || text == QStringLiteral("=")
                || text == QStringLiteral("+");
            const bool isZoomOut = keyEvent->key() == Qt::Key_Minus
                || keyEvent->key() == Qt::Key_Underscore
                || text == QStringLiteral("-")
                || text == QStringLiteral("_");
            const bool isReset = keyEvent->key() == Qt::Key_0 || text == QStringLiteral("0");

            if (isZoomIn || isZoomOut || isReset) {
                if (event->type() == QEvent::ShortcutOverride) {
                    event->accept();
                    return true;
                }

                if (isZoomIn) {
                    zoomInCentered();
                } else if (isZoomOut) {
                    zoomOutCentered();
                } else {
                    resetZoom();
                }
                event->accept();
                return true;
            }
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setLandingVisible(bool visible)
{
    if (!m_centralStack || !m_landingPage || !m_treemapPage) {
        return;
    }

    if (visible) {
        if (m_searchDebounceTimer) {
            m_searchDebounceTimer->stop();
        }
        if (m_searchEdit) {
            QSignalBlocker blocker(m_searchEdit);
            m_searchEdit->clear();
            m_searchEdit->setClearButtonEnabled(false);
            m_searchEdit->setEnabled(false);
        }
        if (m_sizeFilterCombo) {
            QSignalBlocker blocker(m_sizeFilterCombo);
            m_sizeFilterCombo->setCurrentIndex(0);
            m_sizeFilterCombo->setEnabled(false);
        }
        if (m_treemapWidget) {
            m_treemapWidget->setSizeFilter(0, 0);
            m_treemapWidget->setSearchPattern(QString());
        }
    } else {
        if (m_searchEdit) {
            m_searchEdit->setClearButtonEnabled(true);
            m_searchEdit->setEnabled(true);
        }
        if (m_sizeFilterCombo) {
            m_sizeFilterCombo->setEnabled(true);
        }
    }

    m_centralStack->setCurrentWidget(visible ? m_landingPage : m_treemapPage);
    if (visible) {
        setSearchBusy(false);
    }
    if (visible) {
        QTimer::singleShot(0, this, [this]() {
            if (!m_landingPage || !m_centralStack || m_centralStack->currentWidget() != m_landingPage) {
                return;
            }
            rebuildLandingLocationSections();
        });
    }
}

void MainWindow::openDirectory()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open Directory"), QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (dir.isEmpty()) return;

    startScan(dir);
}

void MainWindow::refreshCurrentScan()
{
    if (m_scanInProgress || m_incrementalRefreshInProgress || m_postProcessInProgress) {
        return;
    }

    FileNode* const scanRoot = m_scanResult.root;
    const QString path = scanRoot && !scanRoot->isVirtual
        ? scanRoot->computePath().trimmed()
        : m_currentPath.trimmed();
    if (path.isEmpty()) {
        statusBar()->showMessage(tr("No scan to refresh"));
        return;
    }

    launchIncrementalRefresh(QDir(path).absolutePath());
}

void MainWindow::onRefreshActionTriggered()
{
    if (m_scanInProgress || m_incrementalRefreshInProgress || m_postProcessInProgress) {
        cancelRefreshOperation();
        return;
    }

    if (m_centralStack && m_landingPage && m_centralStack->currentWidget() == m_landingPage) {
        rebuildLandingLocationSections();
        statusBar()->showMessage(tr("Landing page refreshed"), 2000);
        return;
    }

    refreshCurrentScan();
}

void MainWindow::cancelRefreshOperation()
{
    if (m_scanInProgress) {
        cancelScan();
        return;
    }

    if (m_incrementalRefreshInProgress) {
        m_incrementalRefreshCancelled = true;
        if (m_refreshCancelToken) {
            m_refreshCancelToken->store(true, std::memory_order_relaxed);
        }
        statusBar()->showMessage(tr("Cancelling refresh..."));
        return;
    }

    if (m_postProcessInProgress) {
        m_postProcessStale = true;
        statusBar()->showMessage(tr("Cancelling refresh..."));
    }
}

void MainWindow::openPathFromToolbar()
{
    const QString enteredPath = m_pathBar ? m_pathBar->path().trimmed() : QString();
    activatePath(enteredPath, false);
}

void MainWindow::activatePath(const QString& path, bool forceScan)
{
    const QString enteredPath = path.trimmed();
    if (enteredPath.isEmpty()) {
        return;
    }

    const QString absolutePath = QDir(enteredPath).absolutePath();
    const QFileInfo targetInfo(absolutePath);
    if (!targetInfo.exists() || !targetInfo.isDir()) {
        statusBar()->showMessage(
            tr("Directory does not exist: %1").arg(QDir::toNativeSeparators(absolutePath)),
            4000);
        return;
    }

    if (!forceScan && pathIsWithinRoot(absolutePath, m_currentPath) && m_scanResult.root) {
        if (FileNode* node = findNodeByPath(m_scanResult.root, absolutePath)) {
            navigateTo(node, true);
            return;
        }
    }

    if (forceScan && !m_scanInProgress) {
        clearCurrentTreemap();
    }
    startScan(absolutePath, forceScan, false);
}

void MainWindow::startScan(const QString& dir, bool forceRescan, bool backgroundRefresh)
{
    const QString normalizedDir = QFileInfo(dir).absoluteFilePath();
    if (m_closeRequested) {
        return;
    }

    if (m_postProcessInProgress) {
        m_postProcessStale = true;
    }

    if (m_scanInProgress) {
        m_pendingScanPath = normalizedDir;
        m_pendingScanForceRescan = true;
        m_pendingScanBackgroundRefresh = backgroundRefresh;
        m_hasPendingScanRequest = true;
        cancelScan();
        statusBar()->showMessage(tr("Cancelling current scan and switching to %1...")
                                     .arg(QDir::toNativeSeparators(normalizedDir)));
        return;
    }

    const FileNode* currentNode = m_treemapWidget ? m_treemapWidget->currentNode() : nullptr;
    if (!forceRescan && !m_scanInProgress && currentNode && currentNode->computePath() == normalizedDir) {
        syncPathCombo(normalizedDir);
        return;
    }

    m_currentPath = normalizedDir;
    m_scanInProgress = true;
    m_scanCancelled = false;
    m_backgroundRefreshInProgress = backgroundRefresh;
    m_scanCancelToken = std::make_shared<std::atomic_bool>(false);
    syncFilesystemWatchControllerState();
    setRefreshBusy(true);
    m_liveScanResult = {};
    m_latestScannedBytes = 0;
    m_latestScanActivityPath = normalizedDir;
    if (m_completedFilesStatusLabel) {
        m_completedFilesStatusLabel->clear();
        m_completedFilesStatusLabel->setVisible(false);
    }
    if (m_completedTotalStatusLabel) {
        m_completedTotalStatusLabel->clear();
        m_completedTotalStatusLabel->setVisible(false);
    }
    if (m_completedFreeStatusLabel) {
        m_completedFreeStatusLabel->clear();
        m_completedFreeStatusLabel->setVisible(false);
    }
    {
        QMutexLocker locker(&m_scanProgressMutex);
        m_pendingScanProgress.reset();
        m_scanProgressQueued = false;
    }
    {
        QMutexLocker locker(&m_scanActivityMutex);
        m_pendingScanActivity.reset();
        m_scanActivityQueued = false;
    }
    m_permissionErrors.clear();
    {
        QMutexLocker locker(&m_permissionErrorMutex);
        m_pendingPermissionErrors.clear();
        m_permissionErrorQueued = false;
    }
    if (m_permissionErrorList) m_permissionErrorList->clear();
    if (m_permissionWarningAction) {
        m_permissionWarningAction->setVisible(false);
        m_permissionWarningAction->setChecked(false);
    }
    if (m_warningsMenuAction) {
        m_warningsMenuAction->setVisible(false);
        m_warningsMenuAction->setChecked(false);
    }
    m_showPermissionPanel = false;
    rebuildTreemapSplitterLayout();
    syncPathCombo(normalizedDir);
    updateRecentPaths(normalizedDir);
    if (!backgroundRefresh) {
        m_history.clear();
        statusBar()->showMessage(tr("Scanning..."));
        setLandingVisible(false);
        setSearchBusy(false);
        updateDirectoryTreePanel();
        m_treemapWidget->setScanPath(normalizedDir);
        m_treemapWidget->setScanInProgress(true);
        m_treemapWidget->setWheelZoomEnabled(false);
        m_treemapWidget->setRoot(nullptr, {}, false, false);
        m_treemapWidget->viewport()->setCursor(Qt::WaitCursor);
    } else {
        statusBar()->showMessage(tr("Refreshing %1...").arg(QDir::toNativeSeparators(normalizedDir)));
    }
    updateNavigationActions();

    const std::shared_ptr<std::atomic_bool> cancelToken = m_scanCancelToken;
    m_scanPreviewSlotOpen = std::make_shared<std::atomic_bool>(true);
    const std::shared_ptr<std::atomic_bool> previewSlotOpen = m_scanPreviewSlotOpen;
    const QPointer<MainWindow> self = this;
    const bool liveScanPreview = m_settings.liveScanPreview;
    const TreemapSettings settings = m_settings;
    QFuture<ScanResult> future = QtConcurrent::run([self, liveScanPreview, settings, normalizedDir, backgroundRefresh, cancelToken, previewSlotOpen]() {
        const Scanner::ProgressCallback progressCallback = (!backgroundRefresh && liveScanPreview)
            ? Scanner::ProgressCallback([self](ScanResult snapshot) {
            if (!self) return;
            bool shouldQueue = false;
            {
                QMutexLocker locker(&self->m_scanProgressMutex);
                self->m_pendingScanProgress = std::move(snapshot);
                if (!self->m_scanProgressQueued) {
                    self->m_scanProgressQueued = true;
                    shouldQueue = true;
                }
            }
            if (shouldQueue) {
                QMetaObject::invokeMethod(self.data(), &MainWindow::processQueuedScanProgress, Qt::QueuedConnection);
            }
        })
            : Scanner::ProgressCallback{};
        const Scanner::ProgressReadyCallback progressReadyCallback = (!backgroundRefresh && liveScanPreview)
            ? Scanner::ProgressReadyCallback([previewSlotOpen]() {
                return previewSlotOpen && previewSlotOpen->load(std::memory_order_relaxed);
            })
            : Scanner::ProgressReadyCallback{};
        const Scanner::ActivityCallback activityCallback = !backgroundRefresh
            ? Scanner::ActivityCallback([self](const QString& currentPath, qint64 totalBytesSeen) {
            if (!self) return;
            bool shouldQueue = false;
            {
                QMutexLocker locker(&self->m_scanActivityMutex);
                self->m_pendingScanActivity = ScanActivityUpdate{currentPath, totalBytesSeen};
                if (!self->m_scanActivityQueued) {
                    self->m_scanActivityQueued = true;
                    shouldQueue = true;
                }
            }
            if (shouldQueue) {
                QMetaObject::invokeMethod(self.data(), &MainWindow::processQueuedScanActivity, Qt::QueuedConnection);
            }
        })
            : Scanner::ActivityCallback{};
        const Scanner::ErrorCallback errorCallback = [self](const ScanWarning& warning) {
            if (!self) return;
            bool shouldQueue = false;
            {
                QMutexLocker locker(&self->m_permissionErrorMutex);
                self->m_pendingPermissionErrors.append(warning);
                if (!self->m_permissionErrorQueued) {
                    self->m_permissionErrorQueued = true;
                    shouldQueue = true;
                }
            }
            if (shouldQueue) {
                QMetaObject::invokeMethod(self.data(),
                    &MainWindow::processQueuedPermissionErrors,
                    Qt::QueuedConnection);
            }
        };
        if (previewSlotOpen) {
            previewSlotOpen->store(false, std::memory_order_relaxed);
        }
        ScanResult result = Scanner::scan(normalizedDir, settings, progressCallback,
                                          progressReadyCallback, activityCallback,
                                          errorCallback, cancelToken.get());
        sortChildrenBySizeRecursive(result.root);
        ColorUtils::assignColors(result.root, settings);
        return result;
    });
    m_watcher->setFuture(future);
}

void MainWindow::navigateBack()
{
    if (m_scanInProgress && !m_backgroundRefreshInProgress)
        return;

    if (FileNode* current = m_treemapWidget->currentNode()) {
        const TreemapWidget::ViewState currentView = m_treemapWidget->currentViewState();
        const TreemapWidget::ViewState overviewView = m_treemapWidget->overviewViewState(current);
        if (!sameViewState(currentView, overviewView)) {
            m_history.erase(
                std::remove_if(m_history.begin(), m_history.end(),
                               [](const TreemapWidget::ViewState& state) {
                                   return !isOverviewState(state);
                               }),
                m_history.end());
            m_treemapWidget->restoreViewState(overviewView);
            refreshTypeLegendAsync(m_treemapWidget->currentNode());
            updateCurrentViewUi();
            updateNavigationActions();
            return;
        }
    }

    while (!m_history.empty()) {
        const TreemapWidget::ViewState previous = m_history.back();
        m_history.pop_back();
        if (previous.node) {
            if (m_showFreeSpaceInOverview && previous.node != m_treemapWidget->currentNode())
                placeFreeSpaceNodes(previous.node);
            m_treemapWidget->restoreViewState(previous);
            refreshTypeLegendAsync(m_treemapWidget->currentNode());
            updateCurrentViewUi();
            updateNavigationActions();
            return;
        }
    }

    updateNavigationActions();
}

void MainWindow::navigateUp()
{
    if (m_scanInProgress && !m_backgroundRefreshInProgress)
        return;

    FileNode* current = m_treemapWidget->currentNode();
    if (!current) return;

    FileNode* parent = current->parent;
    if (!parent) return;

    navigateTo(parent, true);
}

void MainWindow::zoomInCentered()
{
    if (m_scanInProgress || !m_treemapWidget) {
        return;
    }

    m_treemapWidget->zoomCenteredIn();
    updateNavigationActions();
}

void MainWindow::zoomOutCentered()
{
    if (m_scanInProgress || !m_treemapWidget) {
        return;
    }

    if (m_treemapWidget->cameraScale() <= 1.0 + 0.0001) {
        navigateBack();
        return;
    }

    m_treemapWidget->zoomCenteredOut();
    updateNavigationActions();
}

void MainWindow::resetZoom()
{
    if (m_scanInProgress || !m_treemapWidget) {
        return;
    }

    FileNode* current = m_treemapWidget->currentNode();
    if (current && m_scanResult.root && current != m_scanResult.root) {
        if (isOverviewState(m_treemapWidget->currentViewState())) {
            navigateTo(m_scanResult.root, true);
            return;
        }
    }

    m_treemapWidget->resetZoomToActualSize();
    updateNavigationActions();
}

void MainWindow::toggleFreeSpaceView(bool enabled)
{
    m_showFreeSpaceInOverview = enabled;
    saveSettingsAsync([enabled](QSettings& store) {
        store.setValue("treemap/showFreeSpaceInOverview", enabled);
    });

    if (!m_treemapWidget || m_scanInProgress || !m_scanResult.root) {
        if (m_toggleFreeSpaceAction) {
            m_toggleFreeSpaceAction->blockSignals(true);
            m_toggleFreeSpaceAction->setChecked(m_showFreeSpaceInOverview);
            m_toggleFreeSpaceAction->blockSignals(false);
        }
        return;
    }

    if (m_freeSpaceNodes.empty()) {
        if (m_toggleFreeSpaceAction) {
            m_toggleFreeSpaceAction->blockSignals(true);
            m_toggleFreeSpaceAction->setChecked(m_showFreeSpaceInOverview);
            m_toggleFreeSpaceAction->blockSignals(false);
        }
        return;
    }

    setFreeSpaceVisible(enabled);
}

void MainWindow::onScanProgress(ScanResult scanResult)
{
    if (!m_scanInProgress || m_scanCancelled || !scanResult.root) {
        return;
    }

    m_liveScanResult = std::move(scanResult);

    // Live preview snapshots already preserve the scanner-assigned colors.
    // Reassigning here makes top-level hues unstable because the preview clone
    // can reorder children to keep the active branch visible during scanning.
    m_treemapWidget->setScanPath(m_liveScanResult.currentScanPath);
    m_treemapWidget->setScanInProgress(true);
    m_treemapWidget->setRoot(m_liveScanResult.root, m_liveScanResult.arena, false, false);

    m_latestScannedBytes = std::max(m_latestScannedBytes, m_liveScanResult.root->size);
    if (!m_liveScanResult.currentScanPath.isEmpty()) {
        m_latestScanActivityPath = m_liveScanResult.currentScanPath;
    }
    updateScanStatusMessage();
}

void MainWindow::populateDirectoryTreeChildren(QTreeWidgetItem* item)
{
    if (!item || !m_directoryTree || !m_scanResult.root) {
        return;
    }

    if (item->data(0, kDirectoryLoadedRole).toBool()) {
        return;
    }

    const QString path = item->data(0, kDirectoryPathRole).toString();
    FileNode* node = findNodeByPath(m_scanResult.root, path);
    if (!node || !node->isDirectory || node->isVirtual) {
        item->setData(0, kDirectoryLoadedRole, true);
        return;
    }

    const bool sortingEnabled = m_directoryTree->isSortingEnabled();
    const int sortColumn = m_directoryTree->sortColumn();
    const Qt::SortOrder sortOrder = m_directoryTree->header()
        ? m_directoryTree->header()->sortIndicatorOrder()
        : Qt::AscendingOrder;
    m_directoryTree->setSortingEnabled(false);
    const auto oldChildren = item->takeChildren();
    qDeleteAll(oldChildren);

    const qint64 rootSize = m_scanResult.root->size;
    const qint64 parentSize = std::max<qint64>(1, node->size);
    const qint64 filesSize = directFileBytes(node);
    bool filesInserted = false;
    for (FileNode* child : node->children) {
        if (!child || !child->isDirectory || child->isVirtual) {
            continue;
        }

        if (!filesInserted && filesSize > child->size) {
            auto* filesItem = new DirectoryTreeItem(item);
            filesItem->setText(0, tr("Files"));
            filesItem->setIcon(0, directoryTreeFilesIcon());
            filesItem->setText(2, QLocale::system().formattedDataSize(filesSize));
            filesItem->setData(0, kDirectorySyntheticFilesRole, true);
            filesItem->setData(0, kDirectorySortValueRole, QStringLiteral("files"));
            filesItem->setData(1, kDirectoryUsagePercentRole,
                               static_cast<double>(filesSize) / static_cast<double>(parentSize));
            filesItem->setData(1, kDirectorySortValueRole, filesSize);
            filesItem->setData(2, kDirectorySortValueRole, filesSize);
            filesItem->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            filesItem->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            filesItem->setToolTip(0, tr("Total size of files directly inside this folder"));
            filesItem->setToolTip(1, tr("%1 of parent folder").arg(relativeUsageText(filesSize, parentSize)));
            filesItem->setToolTip(2, QLocale::system().formattedDataSize(filesSize));
            filesInserted = true;
        }

        auto* childItem = new DirectoryTreeItem(item);
        const QString childPath = child->computePath();
        childItem->setText(0, directoryDisplayName(child));
        const QColor childColor = QColor::fromRgba(child->color);
        childItem->setIcon(0, directoryTreeFolderIcon(childColor));
        childItem->setText(2, QLocale::system().formattedDataSize(child->size));
        childItem->setData(0, kDirectoryPathRole, childPath);
        childItem->setData(0, kDirectoryLoadedRole, false);
        childItem->setData(0, kDirectorySortValueRole, directoryDisplayName(child).toCaseFolded());
        childItem->setData(0, kDirectoryColorRole, childColor);
        childItem->setData(0, kDirectoryNodeRole,
                           QVariant::fromValue(reinterpret_cast<quintptr>(child)));
        childItem->setData(1, kDirectoryUsagePercentRole,
                           static_cast<double>(child->size) / static_cast<double>(parentSize));
        childItem->setData(1, kDirectorySortValueRole, child->size);
        childItem->setData(2, kDirectorySortValueRole, child->size);
        childItem->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        childItem->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        childItem->setToolTip(0, QDir::toNativeSeparators(childPath));
        childItem->setToolTip(1, tr("%1 of parent folder").arg(relativeUsageText(child->size, parentSize)));
        childItem->setToolTip(2, QLocale::system().formattedDataSize(child->size));
        if (hasDirectoryChildren(child)) {
            auto* placeholder = new QTreeWidgetItem(childItem);
            placeholder->setData(0, kDirectoryDummyRole, true);
        }
    }

    if (!filesInserted && filesSize > 0) {
        auto* filesItem = new DirectoryTreeItem(item);
        filesItem->setText(0, tr("Files"));
        filesItem->setIcon(0, directoryTreeFilesIcon());
        filesItem->setText(2, QLocale::system().formattedDataSize(filesSize));
        filesItem->setData(0, kDirectorySyntheticFilesRole, true);
        filesItem->setData(0, kDirectorySortValueRole, QStringLiteral("files"));
        filesItem->setData(1, kDirectoryUsagePercentRole,
                           static_cast<double>(filesSize) / static_cast<double>(parentSize));
        filesItem->setData(1, kDirectorySortValueRole, filesSize);
        filesItem->setData(2, kDirectorySortValueRole, filesSize);
        filesItem->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        filesItem->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        filesItem->setToolTip(0, tr("Total size of files directly inside this folder"));
        filesItem->setToolTip(1, tr("%1 of parent folder").arg(relativeUsageText(filesSize, parentSize)));
        filesItem->setToolTip(2, QLocale::system().formattedDataSize(filesSize));
    }

    item->setData(0, kDirectoryLoadedRole, true);
    m_directoryTree->setSortingEnabled(sortingEnabled);
    if (sortingEnabled) {
        m_directoryTree->sortItems(sortColumn, sortOrder);
    }
}

void MainWindow::prepareDirectoryTreeItem(QTreeWidgetItem* item)
{
    if (!item) {
        return;
    }
    populateDirectoryTreeChildren(item);
}

void MainWindow::syncDirectoryTreeSelection()
{
    if (!m_directoryTree) {
        return;
    }

    const QSignalBlocker blocker(m_directoryTree);
    FileNode* current = m_treemapWidget ? m_treemapWidget->currentNode() : nullptr;
    if (!m_scanResult.root || !current || current->isVirtual || !current->isDirectory) {
        m_directoryTree->clearSelection();
        m_directoryTree->setCurrentItem(nullptr);
        return;
    }

    const QString targetPath = current->computePath();
    QTreeWidgetItem* item = m_directoryTree->topLevelItemCount() > 0 ? m_directoryTree->topLevelItem(0) : nullptr;
    if (!item) {
        return;
    }

    while (item) {
        const QString itemPath = item->data(0, kDirectoryPathRole).toString();
        if (itemPath == targetPath) {
            break;
        }

        populateDirectoryTreeChildren(item);

        QTreeWidgetItem* matchingChild = nullptr;
        for (int i = 0; i < item->childCount(); ++i) {
            QTreeWidgetItem* child = item->child(i);
            const QString childPath = child->data(0, kDirectoryPathRole).toString();
            if (childPath == targetPath || pathIsWithinRoot(targetPath, childPath)) {
                matchingChild = child;
                break;
            }
        }

        if (!matchingChild) {
            break;
        }

        item->setExpanded(true);
        item = matchingChild;
    }

    if (item && item->data(0, kDirectoryPathRole).toString() == targetPath) {
        m_directoryTree->setCurrentItem(item);
        item->setSelected(true);
        m_directoryTree->scrollToItem(item);
        return;
    }

    m_directoryTree->clearSelection();
    m_directoryTree->setCurrentItem(nullptr);
}

void MainWindow::updateDirectoryTreePanel()
{
    if (!m_directoryTree) {
        return;
    }

    const QSignalBlocker blocker(m_directoryTree);
    m_directoryTree->clear();

    if (m_scanInProgress && !m_backgroundRefreshInProgress) {
        return;
    }

    if (!m_scanResult.root) {
        return;
    }

    const bool sortingEnabled = m_directoryTree->isSortingEnabled();
    const int sortColumn = m_directoryTree->sortColumn();
    const Qt::SortOrder sortOrder = m_directoryTree->header()
        ? m_directoryTree->header()->sortIndicatorOrder()
        : Qt::AscendingOrder;
    m_directoryTree->setSortingEnabled(false);

    auto* rootItem = new DirectoryTreeItem(m_directoryTree);
    const QString rootPath = m_scanResult.root->computePath();
    rootItem->setText(0, directoryDisplayName(m_scanResult.root));
    const QColor rootColor = QColor::fromRgba(m_scanResult.root->color);
    rootItem->setIcon(0, directoryTreeFolderIcon(rootColor));
    rootItem->setText(2, QLocale::system().formattedDataSize(m_scanResult.root->size));
    rootItem->setData(0, kDirectoryPathRole, rootPath);
    rootItem->setData(0, kDirectoryLoadedRole, false);
    rootItem->setData(0, kDirectorySortValueRole, directoryDisplayName(m_scanResult.root).toCaseFolded());
    rootItem->setData(0, kDirectoryColorRole, rootColor);
    rootItem->setData(0, kDirectoryNodeRole,
                      QVariant::fromValue(reinterpret_cast<quintptr>(m_scanResult.root)));
    rootItem->setData(1, kDirectoryUsagePercentRole, 1.0);
    rootItem->setData(1, kDirectorySortValueRole, m_scanResult.root->size);
    rootItem->setData(2, kDirectorySortValueRole, m_scanResult.root->size);
    rootItem->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    rootItem->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    rootItem->setToolTip(0, QDir::toNativeSeparators(rootPath));
    rootItem->setToolTip(1, tr("100.0% of scan root"));
    rootItem->setToolTip(2, QLocale::system().formattedDataSize(m_scanResult.root->size));
    if (hasDirectoryChildren(m_scanResult.root)) {
        auto* placeholder = new QTreeWidgetItem(rootItem);
        placeholder->setData(0, kDirectoryDummyRole, true);
    }

    populateDirectoryTreeChildren(rootItem);
    rootItem->setExpanded(true);
    m_directoryTree->setSortingEnabled(sortingEnabled);
    if (sortingEnabled) {
        m_directoryTree->sortItems(sortColumn, sortOrder);
    }
    syncDirectoryTreeSelection();
}

void MainWindow::processQueuedScanProgress()
{
    while (true) {
        std::optional<ScanResult> pending;
        {
            QMutexLocker locker(&m_scanProgressMutex);
            if (!m_pendingScanProgress.has_value()) {
                m_scanProgressQueued = false;
                break;
            }
            pending = std::move(m_pendingScanProgress);
            m_pendingScanProgress.reset();
        }
        if (m_scanPreviewSlotOpen) {
            m_scanPreviewSlotOpen->store(true, std::memory_order_relaxed);
        }

        onScanProgress(std::move(*pending));
    }
}

void MainWindow::processQueuedScanActivity()
{
    while (true) {
        std::optional<ScanActivityUpdate> pending;
        {
            QMutexLocker locker(&m_scanActivityMutex);
            if (!m_pendingScanActivity.has_value()) {
                m_scanActivityQueued = false;
                break;
            }
            pending = std::move(m_pendingScanActivity);
            m_pendingScanActivity.reset();
        }

        if (!m_scanInProgress || m_scanCancelled) {
            continue;
        }

        m_latestScanActivityPath = pending->path;
        m_latestScannedBytes = std::max(m_latestScannedBytes, pending->totalBytesSeen);
        updateScanStatusMessage();
    }
}

void MainWindow::processQueuedPermissionErrors()
{
    QList<ScanWarning> drained;
    {
        QMutexLocker locker(&m_permissionErrorMutex);
        drained = std::move(m_pendingPermissionErrors);
        m_pendingPermissionErrors.clear();
        m_permissionErrorQueued = false;
    }

    if (drained.isEmpty())
        return;

    const bool wasEmpty = m_permissionErrors.isEmpty();
    for (const ScanWarning& warning : drained) {
        const QString entry = warning.message.isEmpty()
            ? warning.path
            : tr("%1: %2").arg(warning.path, warning.message);
        m_permissionErrors.append(entry);
        if (m_permissionErrorList) {
            m_permissionErrorList->addItem(entry);
        }
    }

    if (m_permissionWarningAction) {
        if (wasEmpty) {
            m_permissionWarningAction->setVisible(true);
            if (m_warningsMenuAction) {
                m_warningsMenuAction->setVisible(true);
            }
        }
    }
}

void MainWindow::updateScanStatusMessage()
{
    if (!m_scanInProgress || m_scanCancelled) {
        if (m_scanSeenStatusLabel) {
            m_scanSeenStatusLabel->clear();
            m_scanSeenStatusLabel->setVisible(false);
        }
        if (m_scanPathStatusLabel) {
            m_scanPathStatusLabel->clear();
            m_scanPathStatusLabel->setToolTip({});
            m_scanPathStatusLabel->setVisible(false);
        }
        return;
    }

    if (m_completedFilesStatusLabel) {
        m_completedFilesStatusLabel->clear();
        m_completedFilesStatusLabel->setVisible(false);
    }
    if (m_completedTotalStatusLabel) {
        m_completedTotalStatusLabel->clear();
        m_completedTotalStatusLabel->setVisible(false);
    }
    if (m_completedFreeStatusLabel) {
        m_completedFreeStatusLabel->clear();
        m_completedFreeStatusLabel->setVisible(false);
    }

    const QString scanPath = m_latestScanActivityPath.isEmpty()
        ? QDir::toNativeSeparators(m_currentPath)
        : QDir::toNativeSeparators(m_latestScanActivityPath);
    const QString scannedStr = formatPinnedDataSize(m_latestScannedBytes);
    statusBar()->showMessage(tr("Scanning..."));
    if (m_scanSeenStatusLabel) {
        m_scanSeenStatusLabel->setText(tr("Seen: %1").arg(scannedStr));
        m_scanSeenStatusLabel->setVisible(true);
    }
    if (m_scanPathStatusLabel) {
        m_scanPathStatusLabel->setText(tr("Scanning: %1").arg(scanPath));
        m_scanPathStatusLabel->setToolTip(scanPath);
        m_scanPathStatusLabel->setVisible(true);
    }
}

void MainWindow::refreshTypeLegendAsync(FileNode* root)
{
    if (!root) {
        updateTypeLegendPanel();
        return;
    }

    auto summaries = std::make_shared<QList<FileTypeSummary>>();
    FileNode* const legendRoot = root;
    FileNode* const scanRoot = m_scanResult.root;
    const std::shared_ptr<NodeArena> legendArena = m_scanResult.arena;
    auto* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this,
            [this, watcher, legendRoot, scanRoot, summaries]() {
                watcher->deleteLater();
        if (m_typeLegendTree) {
            if (m_scanResult.root == scanRoot
                    && m_treemapWidget
                    && m_treemapWidget->currentNode() == legendRoot) {
                populateTypeLegendItems(m_typeLegendTree, nullptr, m_treemapWidget, *summaries);
            } else {
                updateTypeLegendPanel();
            }
        }
            });
    std::vector<bool> searchReach = m_treemapWidget ? m_treemapWidget->captureSearchReachSnapshot()
                                                    : std::vector<bool>{};
    watcher->setFuture(QtConcurrent::run([legendRoot, legendArena, summaries,
                                          searchReach = std::move(searchReach)]() {
        if (!legendArena) {
            return;
        }
        *summaries = collectAndSortFileSummaries(legendRoot, searchReach);
    }));
}

void MainWindow::scheduleTreeMaintenance()
{
    QTimer::singleShot(0, this, [this]() {
        if (m_closeRequested) {
            return;
        }
        rebuildFilesystemWatchers();
        refreshTypeLegendAsync(m_scanResult.root);
    });
}

void MainWindow::restoreHistoryFromPaths(const std::vector<ViewStatePaths>& historyPaths, FileNode* root)
{
    m_history = remapHistoryPaths(historyPaths, root);
}

void MainWindow::onScanFinished()
{
    if (m_scanCancelled) {
        const bool backgroundRefresh = m_backgroundRefreshInProgress;
        const bool hasPendingScanRequest = m_hasPendingScanRequest;
        const QString pendingScanPath = m_pendingScanPath;
        const bool pendingScanForceRescan = m_pendingScanForceRescan;
        const bool pendingScanBackgroundRefresh = m_pendingScanBackgroundRefresh;
        m_scanCancelled = false;
        m_scanInProgress = false;
        m_backgroundRefreshInProgress = false;
        setRefreshBusy(false);
        {
            QMutexLocker locker(&m_scanProgressMutex);
            m_pendingScanProgress.reset();
            m_scanProgressQueued = false;
        }
        m_liveScanResult = {};
        m_treemapWidget->setScanInProgress(false);
        m_treemapWidget->setWheelZoomEnabled(true);
        if (!backgroundRefresh) {
            m_treemapWidget->viewport()->unsetCursor();
            setLandingVisible(true);
        }
        m_hasPendingScanRequest = false;
        m_pendingScanPath.clear();
        m_pendingScanForceRescan = false;
        m_pendingScanBackgroundRefresh = false;
        syncFilesystemWatchControllerState();
        setSearchBusy(false);
        updateScanStatusMessage();
        statusBar()->showMessage(hasPendingScanRequest ? tr("Starting next scan...") : tr("Scan cancelled"));
        m_scanCancelToken.reset();
        if (m_closeRequested) {
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
            return;
        }
        if (hasPendingScanRequest) {
            startScan(pendingScanPath, pendingScanForceRescan, pendingScanBackgroundRefresh);
            return;
        }
        return;
    }

    const bool backgroundRefresh = m_backgroundRefreshInProgress;
    m_backgroundRefreshInProgress = false;
    m_scanInProgress = false;
    m_scanCancelToken.reset();
    setRefreshBusy(false);
    setSearchBusy(false);
    updateScanStatusMessage();
    {
        QMutexLocker locker(&m_scanProgressMutex);
        m_pendingScanProgress.reset();
        m_scanProgressQueued = false;
    }
    const ViewStatePaths previousViewPaths = backgroundRefresh
        ? captureViewStatePaths(m_treemapWidget->currentViewState())
        : ViewStatePaths{};
    const std::vector<ViewStatePaths> previousHistoryPaths = [&]() {
        std::vector<ViewStatePaths> captured;
        if (!backgroundRefresh) {
            return captured;
        }
        captured.reserve(m_history.size());
        for (const TreemapWidget::ViewState& state : m_history) {
            captured.push_back(captureViewStatePaths(state));
        }
        return captured;
    }();
    m_postProcessStale = true;
    if (m_directoryTree) m_directoryTree->clear();
    m_mountPointFreeNode = nullptr;
    m_scanResult = m_watcher->future().takeResult();
    m_freeSpaceNodes.clear();
    if (!m_scanResult.root) {
        updateDirectoryTreePanel();
        if (!backgroundRefresh) {
            m_treemapWidget->viewport()->unsetCursor();
            setLandingVisible(true);
        }
        m_treemapWidget->setWheelZoomEnabled(true);
        m_treemapWidget->setScanInProgress(false);
        statusBar()->showMessage(backgroundRefresh ? tr("Refresh failed") : tr("Scan failed"));
        if (m_closeRequested) {
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        }
        return;
    }

    m_freeSpaceNodes = prepareRootResultForDisplay(m_scanResult, m_currentPath,
                                                   m_showFreeSpaceInOverview, m_settings, true);
    updateDirectoryTreePanel();

    m_treemapWidget->setWheelZoomEnabled(true);
    m_treemapWidget->setScanInProgress(false);
    m_treemapWidget->setScanPath(m_currentPath);
    setLandingVisible(false);
    if (backgroundRefresh) {
        m_treemapWidget->setRoot(m_scanResult.root, m_scanResult.arena, false, false);
        FileNode* const refreshedRoot = m_scanResult.root;
        QTimer::singleShot(0, this, [this, previousHistoryPaths, previousViewPaths, refreshedRoot]() {
            if (m_closeRequested || m_scanResult.root != refreshedRoot) {
                return;
            }
            restoreHistoryFromPaths(previousHistoryPaths, m_scanResult.root);
            const TreemapWidget::ViewState remappedView =
                remapViewStatePaths(previousViewPaths, m_scanResult.root);
            m_treemapWidget->restoreViewStateImmediate(remappedView);
            refreshTypeLegendAsync(m_treemapWidget->currentNode());
            updateNavigationActions();
            updateCurrentViewUi();
        });
    } else {
        m_treemapWidget->viewport()->unsetCursor();
        m_history.clear();
        m_treemapWidget->setRoot(m_scanResult.root, m_scanResult.arena, false);
        updateNavigationActions();
        updateCurrentViewUi();
    }

    scheduleTreeMaintenance();
    m_liveScanResult = {};
    m_dirtyPaths.clear();
    m_incrementalRefreshInProgress = false;
    syncFilesystemWatchControllerState();

    // Status bar info
    const QLocale locale = QLocale::system();
    const int count = countFilesRecursive(m_scanResult.root);
    const QString countStr = locale.toString(count);
    QString totalStr = locale.formattedDataSize(m_scanResult.root->size);
    QString freeStr = locale.formattedDataSize(m_scanResult.freeBytes);
    if (m_completedFilesStatusLabel) {
        m_completedFilesStatusLabel->setText(tr("Files: %1").arg(countStr));
        m_completedFilesStatusLabel->setVisible(true);
    }
    if (m_completedTotalStatusLabel) {
        m_completedTotalStatusLabel->setText(tr("Total: %1").arg(totalStr));
        m_completedTotalStatusLabel->setVisible(true);
    }
    if (m_completedFreeStatusLabel) {
        m_completedFreeStatusLabel->setText(tr("Free: %1").arg(freeStr));
        m_completedFreeStatusLabel->setVisible(true);
    }
    if (backgroundRefresh) {
        statusBar()->showMessage(tr("Refreshed"));
    } else {
        statusBar()->clearMessage();
    }
    if (m_closeRequested) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
}

void MainWindow::cancelScan()
{
    if (!m_scanInProgress) {
        return;
    }
    m_scanCancelled = true;
    if (m_scanCancelToken) {
        m_scanCancelToken->store(true, std::memory_order_relaxed);
    }
    if (!m_backgroundRefreshInProgress) {
        m_treemapWidget->viewport()->unsetCursor();
        m_liveScanResult = {};
        m_treemapWidget->setRoot(nullptr, {}, false, false);
        m_treemapWidget->setScanInProgress(false);
    }
    statusBar()->showMessage(m_backgroundRefreshInProgress ? tr("Cancelling refresh...") : tr("Cancelling scan..."));
}

void MainWindow::returnToLanding()
{
    if (m_scanInProgress) {
        cancelScan();
    }

    clearCurrentTreemap();
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::onLimitToSameFilesystemToggled(bool checked)
{
    m_settings.limitToSameFilesystem = checked;
    QSettings store = appSettings();
    m_settings.save(store);

    if (m_scanInProgress && !m_backgroundRefreshInProgress) {
        startScan(m_currentPath, true);
    }
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QSettings store = appSettings();
    store.setValue("treemap/showTypeLegend", m_showTypeLegend);
    store.setValue("treemap/showDirectoryTree", m_showDirectoryTree);
    store.setValue("treemap/directoryPanelWidth", m_directoryPanelWidth);
    store.setValue("treemap/typeLegendPanelWidth", m_typeLegendPanelWidth);
    store.setValue("treemap/permissionPanelWidth", m_permissionErrorPanelWidth);
    if (m_directoryTree) {
        store.setValue("treemap/directoryTreeHeaderState", m_directoryTree->header()->saveState());
    }
    if (m_typeLegendTree) {
        store.setValue("treemap/typeLegendHeaderState", m_typeLegendTree->header()->saveState());
    }

    if (!m_scanInProgress && !m_incrementalRefreshInProgress
            && !m_postProcessInProgress) {
        event->accept();
        return;
    }

    m_closeRequested = true;
    m_hasPendingScanRequest = false;
    m_pendingScanPath.clear();
    m_pendingScanForceRescan = false;
    m_pendingScanBackgroundRefresh = false;
    if (m_watchController) {
        m_watchController->stop();
    }
    if (m_scanInProgress) {
        cancelScan();
    } else if (m_incrementalRefreshInProgress || m_postProcessInProgress) {
        cancelRefreshOperation();
    }
    hide();
    event->ignore();
}

void MainWindow::onNodeActivated(FileNode* node)
{
    if (m_scanInProgress)
        return;

    if (!node || node->isVirtual) return;

    if (node->isDirectory) {
        navigateTo(node, true);
    }
}

void MainWindow::onZoomInRequested(FileNode* node, QPointF anchorPos)
{
    if (m_scanInProgress || !node || !node->isDirectory || node->isVirtual) {
        return;
    }

    navigateTo(node, true, anchorPos, true);
}

void MainWindow::onZoomOutRequested(QPointF anchorPos)
{
    if (m_scanInProgress) {
        return;
    }

    FileNode* current = m_treemapWidget->currentNode();
    if (!current || !current->parent) {
        return;
    }

    navigateTo(current->parent, false, anchorPos, true);
}

void MainWindow::openSettings()
{
    SettingsDialog dialog(m_settings, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    applyTreemapSettings(dialog.settings(), true);
}

void MainWindow::recolorCurrentTree()
{
    if (!m_scanResult.root) {
        return;
    }

    FileNode* const root = m_scanResult.root;
    FileNode* const liveRoot = m_liveScanResult.root;
    FileNode* const legendRoot = m_treemapWidget && m_treemapWidget->currentNode()
        ? m_treemapWidget->currentNode()
        : root;
    std::vector<FileNode*> const freeSpaceNodes = m_freeSpaceNodes;
    const TreemapSettings settings = m_settings;
    auto summaries = std::make_shared<QList<FileTypeSummary>>();
    const auto arena = m_scanResult.arena;
    const auto liveArena = m_liveScanResult.arena;

    auto* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, root, legendRoot, summaries]() {
        watcher->deleteLater();
        m_treemapWidget->viewport()->update();
        if (m_directoryTree && m_scanResult.root == root) {
            for (int i = 0; i < m_directoryTree->topLevelItemCount(); ++i) {
                refreshDirectoryTreeIcons(m_directoryTree->topLevelItem(i));
            }
            m_directoryTree->viewport()->update();
        }
        if (m_typeLegendTree) {
            if (m_scanResult.root == root
                    && m_treemapWidget
                    && m_treemapWidget->currentNode() == legendRoot) {
                populateTypeLegendItems(m_typeLegendTree, nullptr, m_treemapWidget, *summaries);
            } else {
                updateTypeLegendPanel();
            }
        }
    });
    std::vector<bool> searchReach = m_treemapWidget ? m_treemapWidget->captureSearchReachSnapshot()
                                                    : std::vector<bool>{};
    watcher->setFuture(QtConcurrent::run([root, liveRoot, freeSpaceNodes, settings, summaries, arena, liveArena,
                                          legendRoot, searchReach = std::move(searchReach)]() {
        ColorUtils::assignColors(root, liveRoot, settings);
        for (FileNode* freeNode : freeSpaceNodes) freeNode->color = settings.freeSpaceColor.rgba();
        if (liveRoot) {
            applyFreeSpaceNodeColor(liveRoot, settings);
        }
        *summaries = collectAndSortFileSummaries(legendRoot, searchReach);
    }));
}

void MainWindow::applyTreemapSettings(const TreemapSettings& settings, bool persist)
{
    const bool freeSpaceFilterChanged =
        settings.hideNonLocalFreeSpace != m_settings.hideNonLocalFreeSpace;

    m_settings = settings;
    m_settings.sanitize();
    if (persist) {
        const TreemapSettings savedSettings = m_settings;
        saveSettingsAsync([savedSettings](QSettings& store) {
            savedSettings.save(store);
        });
    }

    if (freeSpaceFilterChanged && m_treemapWidget && m_scanResult.root
            && !m_scanResult.filesystems.isEmpty()) {
        const ViewStatePaths previousViewPaths =
            captureViewStatePaths(m_treemapWidget->currentViewState());
        // Detach the mount-point node before re-injecting (it may belong to an old FS entry).
        if (m_mountPointFreeNode && m_mountPointFreeNode->parent) {
            auto& ch = m_mountPointFreeNode->parent->children;
            ch.erase(std::remove(ch.begin(), ch.end(), m_mountPointFreeNode), ch.end());
            m_mountPointFreeNode->parent = nullptr;
        }
        m_freeSpaceNodes = reinjectFreeSpaceNodes(
            m_scanResult, m_currentPath, m_showFreeSpaceInOverview, m_settings);
        placeFreeSpaceNodes(m_treemapWidget->currentNode());
        m_treemapWidget->setRoot(m_scanResult.root, m_scanResult.arena, false, false);
        m_treemapWidget->restoreViewStateImmediate(
            remapViewStatePaths(previousViewPaths, m_scanResult.root));
        updateNavigationActions();
        updateCurrentViewUi();
        refreshTypeLegendAsync(m_scanResult.root);
    }

    if (m_treemapWidget) {
        m_treemapWidget->applySettings(m_settings);
    }
    recolorCurrentTree();
}

void MainWindow::syncColorThemeWithSystem(bool darkMode, bool persist)
{
    if (!m_settings.followSystemColorTheme) {
        return;
    }

    TreemapSettings syncedSettings = m_settings;
    const QString targetThemeId = syncedSettings.colorThemeIdForSystemScheme(darkMode);
    if (targetThemeId == syncedSettings.activeColorThemeId) {
        return;
    }

    syncedSettings.activeColorThemeId = targetThemeId;
    syncedSettings.sanitize();
    applyTreemapSettings(syncedSettings, persist);
}

void MainWindow::launchIncrementalRefresh(const QString& refreshPath)
{
    m_activeRefreshPath = refreshPath;
    m_preRefreshViewPaths = captureViewStatePaths(m_treemapWidget->currentViewState());
    m_preRefreshHistoryPaths.clear();
    m_preRefreshHistoryPaths.reserve(m_history.size());
    for (const TreemapWidget::ViewState& s : m_history) {
        m_preRefreshHistoryPaths.push_back(captureViewStatePaths(s));
    }

    m_incrementalRefreshInProgress = true;
    m_incrementalRefreshCancelled = false;
    m_refreshCancelToken = std::make_shared<std::atomic_bool>(false);
    syncFilesystemWatchControllerState();
    setRefreshBusy(true);
    m_permissionErrors.clear();
    {
        QMutexLocker locker(&m_permissionErrorMutex);
        m_pendingPermissionErrors.clear();
        m_permissionErrorQueued = false;
    }
    if (m_permissionErrorList) m_permissionErrorList->clear();
    if (m_permissionWarningAction) {
        m_permissionWarningAction->setVisible(false);
        m_permissionWarningAction->setChecked(false);
    }
    if (m_warningsMenuAction) {
        m_warningsMenuAction->setVisible(false);
        m_warningsMenuAction->setChecked(false);
    }
    m_showPermissionPanel = false;
    rebuildTreemapSplitterLayout();
    statusBar()->showMessage(tr("Refreshing %1...").arg(QDir::toNativeSeparators(refreshPath)));
    const TreemapSettings settings = m_settings;
    const std::shared_ptr<std::atomic_bool> cancelToken = m_refreshCancelToken;
    const QPointer<MainWindow> self = this;
    QFuture<ScanResult> future = QtConcurrent::run([self, refreshPath, settings, cancelToken]() {
        const Scanner::ErrorCallback errorCallback = [self](const ScanWarning& warning) {
            if (!self) return;
            bool shouldQueue = false;
            {
                QMutexLocker locker(&self->m_permissionErrorMutex);
                self->m_pendingPermissionErrors.append(warning);
                if (!self->m_permissionErrorQueued) {
                    self->m_permissionErrorQueued = true;
                    shouldQueue = true;
                }
            }
            if (shouldQueue) {
                QMetaObject::invokeMethod(self.data(),
                    &MainWindow::processQueuedPermissionErrors,
                    Qt::QueuedConnection);
            }
        };
        return Scanner::scan(refreshPath, settings, {}, {}, {}, errorCallback, cancelToken.get());
    });
    m_refreshWatcher->setFuture(future);
}

void MainWindow::onIncrementalRefreshFinished()
{
    ScanResult refreshed = m_refreshWatcher->future().takeResult();
    m_incrementalRefreshInProgress = false;
    const bool refreshCancelled = m_incrementalRefreshCancelled;
    m_incrementalRefreshCancelled = false;
    m_refreshCancelToken.reset();
    syncFilesystemWatchControllerState();

    if (refreshCancelled || !refreshed.root || m_scanInProgress) {
        setRefreshBusy(false);
        rebuildFilesystemWatchers();
    } else {
        const QString refreshPath = m_activeRefreshPath;
        IncrementalRefreshResult input;
        input.refreshed = std::move(refreshed);
        input.rootReplaced = (refreshPath == m_currentPath);

        if (!input.rootReplaced) {
            FileNode* targetNode = findNodeByPath(m_scanResult.root, refreshPath);
            if (!targetNode || !targetNode->parent) {
                setRefreshBusy(false);
                rebuildFilesystemWatchers();
                if (m_closeRequested) {
                    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
                }
                return;
            }

            input.subtreeDepth = nodeDepth(targetNode->parent) + 1;
            input.subtreeBranchHue = ColorUtils::hueFromColor(
                QColor::fromRgba(targetNode->color),
                ColorUtils::topLevelFolderBranchHue(targetNode->name, m_settings));
        }

        m_postProcessInProgress = true;
        m_postProcessStale = false;
        QFuture<IncrementalRefreshResult> future = QtConcurrent::run(
            [input = std::move(input), currentPath = m_currentPath,
             showFreeSpaceInOverview = m_showFreeSpaceInOverview,
             settings = m_settings]() mutable {
                sortChildrenBySizeRecursive(input.refreshed.root);

                if (input.rootReplaced) {
                    input.preparedFreeSpaceNodes = prepareRootResultForDisplay(input.refreshed, currentPath,
                                                                               showFreeSpaceInOverview,
                                                                               settings, true);
                } else {
                    ColorUtils::assignColorsForSubtree(input.refreshed.root,
                                                       input.subtreeDepth,
                                                       input.subtreeBranchHue,
                                                       settings);
                }

                return std::move(input);
            });
        m_postProcessWatcher->setFuture(future);
    }

    if (m_closeRequested) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }
}

void MainWindow::onPostProcessFinished()
{
    IncrementalRefreshResult refreshed = m_postProcessWatcher->future().takeResult();
    m_postProcessInProgress = false;
    syncFilesystemWatchControllerState();

    if (m_postProcessStale || m_scanInProgress || !refreshed.refreshed.root) {
        m_postProcessStale = false;
        setRefreshBusy(false);
        rebuildFilesystemWatchers();
    } else {
        finalizeIncrementalRefresh(std::move(refreshed));
    }

    if (m_closeRequested) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }
}

void MainWindow::onNodeContextMenuRequested(FileNode* node, QPoint globalPos)
{
    if (!node || node->isVirtual) {
        return;
    }

    const QString path = node->computePath();
    const bool canDelete = !m_scanInProgress && !m_incrementalRefreshInProgress
                           && !m_postProcessInProgress
                           && (!m_scanResult.root || node != m_scanResult.root);
    const bool parentWritable = QFileInfo(QFileInfo(path).absolutePath()).isWritable();
    QMenu menu(this);
    QAction* zoomAction = nullptr;
    QAction* openAction = nullptr;
    QAction* revealAction = nullptr;
    QAction* copyPathAction = nullptr;
    QAction* deleteAction = nullptr;
    QAction* refreshAction = nullptr;
    QAction* propertiesAction = nullptr;
    if (node->isDirectory) {
        zoomAction = menu.addAction(tr("Zoom In"));
        zoomAction->setIcon(menuActionIcon({"zoom-in"},
            QStringLiteral(":/assets/tabler-icons/zoom-in.svg"),
            QStringLiteral(":/assets/tabler-icons/zoom-in.svg"),
            QStyle::SP_ArrowDown));
        QFont zoomFont = zoomAction->font();
        zoomFont.setBold(true);
        zoomAction->setFont(zoomFont);
        menu.setDefaultAction(zoomAction);
        openAction = menu.addAction(tr("Open"));
        openAction->setIcon(menuActionIcon({"document-open", "folder-open"},
            QStringLiteral(":/assets/tabler-icons/folder.svg"),
            QStringLiteral(":/assets/tabler-icons/folder.svg"),
            QStyle::SP_DialogOpenButton));
        menu.addSeparator();
        copyPathAction = menu.addAction(tr("Copy Path"));
        copyPathAction->setIcon(menuActionIcon({"edit-copy"},
            QStringLiteral(":/assets/tabler-icons/copy.svg"),
            QStringLiteral(":/assets/tabler-icons/copy.svg"),
            QStyle::SP_FileIcon));
        deleteAction = menu.addAction(QString());
        menu.addSeparator();
        const bool canRefresh = !m_scanInProgress && !m_incrementalRefreshInProgress && !m_postProcessInProgress;
        refreshAction = menu.addAction(tr("Refresh"));
        refreshAction->setIcon(menuActionIcon({"view-refresh", "refresh"},
            QStringLiteral(":/assets/tabler-icons/refresh.svg"),
            QStringLiteral(":/assets/tabler-icons/refresh.svg"),
            QStyle::SP_BrowserReload));
        refreshAction->setEnabled(canRefresh);
        menu.addSeparator();
        propertiesAction = menu.addAction(tr("Properties"));
        propertiesAction->setIcon(menuActionIcon({"document-properties", "file-info"},
            QStringLiteral(":/assets/tabler-icons/file-info.svg"),
            QStringLiteral(":/assets/tabler-icons/file-info.svg"),
            QStyle::SP_FileDialogContentsView));
    } else {
        openAction = menu.addAction(tr("Open"));
        openAction->setIcon(menuActionIcon({"document-open"},
            QStringLiteral(":/assets/tabler-icons/folder.svg"),
            QStringLiteral(":/assets/tabler-icons/folder.svg"),
            QStyle::SP_DialogOpenButton));
        revealAction = menu.addAction(tr("Show in File Manager"));
        revealAction->setIcon(menuActionIcon({"system-file-manager", "folder-open"},
            QStringLiteral(":/assets/tabler-icons/folder.svg"),
            QStringLiteral(":/assets/tabler-icons/folder.svg"),
            QStyle::SP_DirOpenIcon));
        menu.addSeparator();
        copyPathAction = menu.addAction(tr("Copy Path"));
        copyPathAction->setIcon(menuActionIcon({"edit-copy"},
            QStringLiteral(":/assets/tabler-icons/copy.svg"),
            QStringLiteral(":/assets/tabler-icons/copy.svg"),
            QStyle::SP_FileIcon));
        deleteAction = menu.addAction(QString());
        menu.addSeparator();
        propertiesAction = menu.addAction(tr("Properties"));
        propertiesAction->setIcon(menuActionIcon({"document-properties", "file-info"},
            QStringLiteral(":/assets/tabler-icons/file-info.svg"),
            QStringLiteral(":/assets/tabler-icons/file-info.svg"),
            QStyle::SP_FileDialogContentsView));
    }

    if (deleteAction) {
        deleteAction->setEnabled(canDelete && parentWritable);
        if (!parentWritable) {
            deleteAction->setToolTip(tr("You do not have permission to delete this item"));
        } else if (!canDelete) {
            deleteAction->setToolTip(tr("Cannot delete while a scan is in progress"));
        }
        auto* deleteLabelWatcher = new DeleteActionModifierWatcher(deleteAction, &menu);
        menu.installEventFilter(deleteLabelWatcher);
    }

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) {
        return;
    }

    if (zoomAction && chosen == zoomAction) {
        navigateTo(node, true);
        return;
    }

    if (refreshAction && chosen == refreshAction) {
        launchIncrementalRefresh(path);
        return;
    }

    if (chosen == openAction) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }

    if (revealAction && chosen == revealAction) {
        revealPathInFileManager(path, node->isDirectory);
        return;
    }

    if (chosen == copyPathAction) {
        QApplication::clipboard()->setText(path);
        statusBar()->showMessage(tr("Copied path: %1").arg(statusBarDisplayPath(path)), 2000);
        return;
    }

    if (chosen == propertiesAction) {
        showPathProperties(node, m_scanResult.root, m_scanResult.arena);
        return;
    }

    if (deleteAction && chosen == deleteAction) {
        const bool permanentDelete = deleteAction->data().toBool();
        const QFileInfo info(path);
        if (!confirmDeletion(info, permanentDelete)) {
            return;
        }

        if (!deletePath(info, permanentDelete)) {
            statusBar()->showMessage(
                permanentDelete
                    ? tr("Failed to delete: %1").arg(statusBarDisplayPath(path))
                    : tr("Failed to move to wastebin: %1").arg(statusBarDisplayPath(path)),
                4000);
            return;
        }

        refreshPathAfterMutation(info);
        return;
    }
}

void MainWindow::revealPathInFileManager(const QString& path, bool isDirectory)
{
    const QFileInfo info(path);
    QString program = QStringLiteral("xdg-open");
    QStringList arguments;
    const QString fallbackTarget = isDirectory ? path : info.absolutePath();
#ifdef Q_OS_WIN
    program = QStringLiteral("explorer");
    arguments << (isDirectory
        ? QDir::toNativeSeparators(path)
        : QStringLiteral("/select,") + QDir::toNativeSeparators(path));
#elif defined(Q_OS_MACOS)
    program = QStringLiteral("open");
    if (isDirectory) {
        arguments << path;
    } else {
        arguments << QStringLiteral("-R") << path;
    }
#else
    arguments << fallbackTarget;
#endif
    if (!QProcess::startDetached(program, arguments)) {
        statusBar()->showMessage(tr("Failed to open: %1").arg(statusBarDisplayPath(fallbackTarget)));
    }
}

void MainWindow::showPathProperties(const FileNode* node, const FileNode* scanRoot,
                                    std::shared_ptr<NodeArena> arena)
{
    auto* dlg = new NodePropertiesDialog(node, scanRoot, std::move(arena), m_settings, this);
    dlg->show();
}

bool MainWindow::confirmDeletion(const QFileInfo& info, bool permanentDelete)
{
    const QString targetName = info.fileName().isEmpty() ? info.absoluteFilePath() : info.fileName();
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(permanentDelete ? tr("Delete Permanently") : tr("Move to Wastebin"));
    box.setText(permanentDelete
        ? tr("Delete %1 permanently?").arg(targetName)
        : tr("Move %1 to the wastebin?").arg(targetName));
    box.setInformativeText(permanentDelete
        ? tr("This will permanently remove it from disk.")
        : tr("You can restore it later from the system wastebin if supported."));
    box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Yes);
    box.setDefaultButton(QMessageBox::Cancel);
    box.button(QMessageBox::Yes)->setText(permanentDelete ? tr("Delete Permanently")
                                                          : tr("Move to Wastebin"));
    return box.exec() == QMessageBox::Yes;
}

bool MainWindow::deletePath(const QFileInfo& info, bool permanentDelete)
{
    if (permanentDelete) {
        if (info.isDir() && !info.isSymLink()) {
            QDir dir(info.absoluteFilePath());
            return dir.removeRecursively();
        }
        return QFile::remove(info.absoluteFilePath());
    }

    QString pathInTrash;
    const bool moved = QFile::moveToTrash(info.absoluteFilePath(), &pathInTrash);
    if (moved) {
        notifyTrashChanged();
    }
    return moved;
}

void MainWindow::notifyTrashChanged() const
{
#ifdef DISKSCAPE_HAS_QT_DBUS
    // Dolphin and other KDE file views listen for KDirNotify updates on trash:/.
    QDBusMessage message = QDBusMessage::createSignal(
        QStringLiteral("/KDirNotify"),
        QStringLiteral("org.kde.KDirNotify"),
        QStringLiteral("FilesAdded"));
    message << QStringLiteral("trash:/");
    QDBusConnection::sessionBus().send(message);
#endif
}

void MainWindow::refreshPathAfterMutation(const QFileInfo& info)
{
    const QString refreshPath = info.absolutePath();
    if (!refreshPath.isEmpty()) {
        launchIncrementalRefresh(refreshPath);
    }
}

void MainWindow::navigateTo(FileNode* node, bool pushHistory,
                            const QPointF& anchorPos, bool useAnchor)
{
    if (!node)
        return;

    FileNode* current = m_treemapWidget->currentNode();
    if (node == current) {
        updateCurrentViewUi();
        updateNavigationActions();
        return;
    }

    if (pushHistory && current) {
        const TreemapWidget::ViewState currentView = m_treemapWidget->currentViewState();
        const TreemapWidget::ViewState overviewView = m_treemapWidget->overviewViewState(current);
        m_history.erase(
            std::remove_if(m_history.begin(), m_history.end(),
                           [](const TreemapWidget::ViewState& state) {
                               return !isOverviewState(state);
                           }),
            m_history.end());
        if (!sameViewState(currentView, overviewView)
                && (m_history.empty() || !sameViewState(m_history.back(), overviewView))) {
            m_history.push_back(overviewView);
        }
        if (m_history.empty() || !sameViewState(m_history.back(), currentView)) {
            m_history.push_back(currentView);
        }
    }

    if (m_showFreeSpaceInOverview && !m_freeSpaceNodes.empty())
        placeFreeSpaceNodes(node);

    m_treemapWidget->setCurrentNode(node, anchorPos, useAnchor);
    rebuildFilesystemWatchers();
    refreshTypeLegendAsync(node);
    updateCurrentViewUi();
    updateNavigationActions();
}

void MainWindow::updateNavigationActions()
{
    FileNode* current = m_treemapWidget->currentNode();
    const bool navigationEnabled = !m_scanInProgress || m_backgroundRefreshInProgress;
    m_backAction->setEnabled(navigationEnabled && !m_history.empty());
    m_upAction->setEnabled(navigationEnabled && current && current->parent != nullptr);
    if (m_zoomInAction) {
        m_zoomInAction->setEnabled(navigationEnabled && current);
    }
    if (m_zoomOutAction) {
        m_zoomOutAction->setEnabled(navigationEnabled && current);
    }
    if (m_resetZoomAction) {
        m_resetZoomAction->setEnabled(navigationEnabled && current);
    }
    if (m_toggleFreeSpaceAction) {
        const bool hasFreeSpaceNode = m_scanResult.root && !m_freeSpaceNodes.empty();
        m_toggleFreeSpaceAction->setVisible(hasFreeSpaceNode);
        m_toggleFreeSpaceAction->setEnabled(navigationEnabled && hasFreeSpaceNode);
        m_toggleFreeSpaceAction->blockSignals(true);
        m_toggleFreeSpaceAction->setChecked(m_showFreeSpaceInOverview);
        m_toggleFreeSpaceAction->blockSignals(false);
    }
    updateToolbarResponsiveLayout();
}

void MainWindow::updateWindowTitle()
{
    const QString appTitle = QStringLiteral("Diskscape");
    if (!m_treemapWidget) {
        setWindowTitle(appTitle);
        return;
    }

    FileNode* current = m_treemapWidget->currentNode();
    if (!current) {
        setWindowTitle(appTitle);
        return;
    }

    const QString sizeText = QLocale::system().formattedDataSize(current->size);
    setWindowTitle(QStringLiteral("%1 (%2) - %3").arg(current->name, sizeText, appTitle));
}

void MainWindow::updateCurrentViewUi()
{
    updateWindowTitle();

    if (!m_treemapWidget) {
        return;
    }

    FileNode* current = m_treemapWidget->currentNode();
    if (!current) {
        syncPathCombo(QString());
        syncDirectoryTreeSelection();
        return;
    }

    if (current->isVirtual) {
        syncPathCombo(m_currentPath);
        syncDirectoryTreeSelection();
        statusBar()->showMessage(
            tr("Free Space: %1").arg(QLocale::system().formattedDataSize(current->size)));
        return;
    }

    const QString path = current->computePath();
    syncPathCombo(path);
    syncDirectoryTreeSelection();
    statusBar()->showMessage(statusBarDisplayPath(path));
}

// Inject or remove the per-sub-filesystem free-space node for the given navigation target.
// Root-level free-space nodes (m_freeSpaceNodes) are NEVER moved — this keeps the root
// layout stable so returning to the scan root never triggers a re-layout.
// A single reusable m_mountPointFreeNode is allocated lazily from the arena and recycled
// across navigations.
void MainWindow::placeFreeSpaceNodes(FileNode* currentNode)
{
    if (!m_scanResult.root)
        return;

    // Detach any previously active mount-point free-space node.
    if (m_mountPointFreeNode && m_mountPointFreeNode->parent) {
        auto& ch = m_mountPointFreeNode->parent->children;
        ch.erase(std::remove(ch.begin(), ch.end(), m_mountPointFreeNode), ch.end());
        m_mountPointFreeNode->parent = nullptr;
    }

    if (!m_showFreeSpaceInOverview || !currentNode || currentNode == m_scanResult.root)
        return;

    // Check whether currentNode is a sub-filesystem mount point.
    const QString currentPath = QFileInfo(currentNode->computePath()).canonicalFilePath();
    const QString scanRootPath = QFileInfo(m_scanResult.root->computePath()).canonicalFilePath();
    const FsInfo* matchingFs = nullptr;
    for (const FsInfo& fs : m_scanResult.filesystems) {
        if (fs.canonicalMountRoot == currentPath && currentPath != scanRootPath) {
            matchingFs = &fs;
            break;
        }
    }
    if (!matchingFs || matchingFs->freeBytes <= 0)
        return;
    if (!matchingFs->isLocal && m_settings.hideNonLocalFreeSpace)
        return;

    // Allocate the reusable node on first use.
    if (!m_mountPointFreeNode)
        m_mountPointFreeNode = m_scanResult.arena->alloc();

    m_mountPointFreeNode->name = QCoreApplication::translate("ColorUtils", "Free Space");
    m_mountPointFreeNode->size = matchingFs->freeBytes;
    m_mountPointFreeNode->isVirtual = true;
    m_mountPointFreeNode->isDirectory = false;
    m_mountPointFreeNode->color = m_settings.freeSpaceColor.rgba();
    m_mountPointFreeNode->parent = currentNode;
    currentNode->children.push_back(m_mountPointFreeNode);
    std::sort(currentNode->children.begin(), currentNode->children.end(),
              [](const FileNode* a, const FileNode* b) { return a->size > b->size; });
}

void MainWindow::setFreeSpaceVisible(bool visible)
{
    if (!m_scanResult.root || m_freeSpaceNodes.empty() || !m_treemapWidget) {
        return;
    }

    auto& children = m_scanResult.root->children;
    const bool currentlyVisible = std::any_of(children.begin(), children.end(),
                                               [](const FileNode* c) { return c && c->isVirtual; });
    if (currentlyVisible == visible) {
        refreshTypeLegendAsync(m_scanResult.root);
        updateNavigationActions();
        updateCurrentViewUi();
        return;
    }

    const ViewStatePaths previousViewPaths = captureViewStatePaths(m_treemapWidget->currentViewState());
    std::vector<ViewStatePaths> previousHistoryPaths;
    previousHistoryPaths.reserve(m_history.size());
    for (const TreemapWidget::ViewState& state : m_history) {
        previousHistoryPaths.push_back(captureViewStatePaths(state));
    }

    if (visible) {
        for (FileNode* freeNode : m_freeSpaceNodes) {
            children.push_back(freeNode);
        }
        std::sort(children.begin(), children.end(),
                  [](const FileNode* a, const FileNode* b) {
                      return a->size > b->size;
                  });
        placeFreeSpaceNodes(m_treemapWidget->currentNode());
    } else {
        children.erase(std::remove_if(children.begin(), children.end(),
                                      [](FileNode* c) { return c && c->isVirtual; }),
                       children.end());
        // Also remove mount-point node if currently active.
        if (m_mountPointFreeNode && m_mountPointFreeNode->parent) {
            auto& ch = m_mountPointFreeNode->parent->children;
            ch.erase(std::remove(ch.begin(), ch.end(), m_mountPointFreeNode), ch.end());
            m_mountPointFreeNode->parent = nullptr;
        }
    }

    m_history.clear();
    m_history.reserve(previousHistoryPaths.size());
    for (const ViewStatePaths& state : previousHistoryPaths) {
        TreemapWidget::ViewState remapped = remapViewStatePaths(state, m_scanResult.root);
        if (!remapped.node) {
            continue;
        }
        if (m_history.empty() || !sameViewState(m_history.back(), remapped)) {
            m_history.push_back(remapped);
        }
    }

    const TreemapWidget::ViewState remappedView = remapViewStatePaths(previousViewPaths, m_scanResult.root);
    m_treemapWidget->setRoot(m_scanResult.root, m_scanResult.arena, false, false);
    m_treemapWidget->restoreViewStateImmediate(remappedView);
    rebuildFilesystemWatchers();
    updateDirectoryTreePanel();
    refreshTypeLegendAsync(m_scanResult.root);
    updateCurrentViewUi();
    updateNavigationActions();
}

void MainWindow::clearCurrentTreemap()
{
    m_liveScanResult = {};
    m_scanResult = {};
    m_history.clear();
    m_freeSpaceNodes.clear();
    m_mountPointFreeNode = nullptr;
    m_currentPath.clear();
    m_dirtyPaths.clear();
    m_backgroundRefreshInProgress = false;
    m_postProcessStale = true;
    m_permissionErrors.clear();
    {
        QMutexLocker locker(&m_permissionErrorMutex);
        m_pendingPermissionErrors.clear();
        m_permissionErrorQueued = false;
    }
    if (m_permissionErrorList) {
        m_permissionErrorList->clear();
    }
    if (m_permissionWarningAction) {
        m_permissionWarningAction->setVisible(false);
        m_permissionWarningAction->setChecked(false);
        m_permissionWarningAction->setIcon(m_permissionWarningIcon);
    }
    if (m_warningsMenuAction) {
        m_warningsMenuAction->setVisible(false);
        m_warningsMenuAction->setChecked(false);
        m_warningsMenuAction->setIcon(m_permissionWarningIcon);
    }
    m_showPermissionPanel = false;
    if (m_completedFilesStatusLabel) {
        m_completedFilesStatusLabel->clear();
        m_completedFilesStatusLabel->setVisible(false);
    }
    if (m_completedTotalStatusLabel) {
        m_completedTotalStatusLabel->clear();
        m_completedTotalStatusLabel->setVisible(false);
    }
    if (m_completedFreeStatusLabel) {
        m_completedFreeStatusLabel->clear();
        m_completedFreeStatusLabel->setVisible(false);
    }

    m_treemapWidget->setScanPath(QString());
    m_treemapWidget->setScanInProgress(false);
    m_treemapWidget->setWheelZoomEnabled(true);
    m_treemapWidget->setRoot(nullptr);

    syncFilesystemWatchControllerState();
    rebuildFilesystemWatchers();
    rebuildTreemapSplitterLayout();
    updateDirectoryTreePanel();
    updateTypeLegendPanel();
    setLandingVisible(true);
    updateCurrentViewUi();
    updateNavigationActions();
}

void MainWindow::onNodeHovered(FileNode* node)
{
    if (!node) {
        if (m_completedFilesStatusLabel && m_completedFilesStatusLabel->isVisible()) {
            statusBar()->clearMessage();
        } else {
            updateCurrentViewUi();
        }
        return;
    }

    QString sizeStr = QLocale::system().formattedDataSize(node->size);
    if (node->isVirtual) {
        statusBar()->showMessage(tr("Free Space: %1").arg(sizeStr));
    } else {
        statusBar()->showMessage(tr("%1  (%2)").arg(statusBarDisplayPath(node->computePath()), sizeStr));
    }
}

void MainWindow::loadRecentPaths(QSettings& store)
{
    m_recentPaths = sanitizedLandingPaths(store.value("recentPaths").toStringList(), kMaxRecentPaths);
}

void MainWindow::loadFavouritePaths(QSettings& store)
{
    if (store.contains(QStringLiteral("favouritePaths"))) {
        m_favouritePaths = sanitizedLandingPaths(store.value("favouritePaths").toStringList(), kMaxFavouritePaths);
        return;
    }

    m_favouritePaths = sanitizedLandingPaths(defaultFavouritePaths(), kMaxFavouritePaths);
}

void MainWindow::updateRecentPaths(const QString& path)
{
    if (path.isEmpty()) {
        return;
    }

    m_recentPaths.removeAll(path);
    m_recentPaths.prepend(path);
    while (m_recentPaths.size() > kMaxRecentPaths) {
        m_recentPaths.removeLast();
    }

    if (m_pathBar) {
        m_pathBar->blockSignals(true);
        m_pathBar->setRecentPaths(m_recentPaths);
        m_pathBar->setPath(path);
        m_pathBar->blockSignals(false);
    }

    const QStringList recentPaths = m_recentPaths;
    saveSettingsAsync([recentPaths](QSettings& store) {
        store.setValue("recentPaths", recentPaths);
    });

    rebuildLandingLocationSections();
}

void MainWindow::promoteRecentPath(const QString& path)
{
    const QString normalized = normalizedFilesystemPath(path);
    if (normalized.isEmpty()) {
        return;
    }

    const int existingIndex = m_recentPaths.indexOf(normalized);
    if (existingIndex <= 0) {
        return;
    }

    m_recentPaths.removeAt(existingIndex);
    m_recentPaths.prepend(normalized);

    if (m_pathBar) {
        m_pathBar->blockSignals(true);
        m_pathBar->setRecentPaths(m_recentPaths);
        m_pathBar->blockSignals(false);
    }

    const QStringList recentPaths = m_recentPaths;
    saveSettingsAsync([recentPaths](QSettings& store) {
        store.setValue("recentPaths", recentPaths);
    });

    rebuildLandingLocationSections();
}

void MainWindow::removeRecentPath(const QString& path)
{
    const QString normalized = normalizedFilesystemPath(path);
    if (normalized.isEmpty()) {
        return;
    }

    const QStringList previous = m_recentPaths;
    m_recentPaths.removeAll(normalized);
    if (m_recentPaths == previous) {
        return;
    }

    if (m_pathBar) {
        m_pathBar->blockSignals(true);
        m_pathBar->setRecentPaths(m_recentPaths);
        m_pathBar->blockSignals(false);
    }

    const QStringList recentPaths = m_recentPaths;
    saveSettingsAsync([recentPaths](QSettings& store) {
        store.setValue("recentPaths", recentPaths);
    });

    rebuildLandingLocationSections();
}

void MainWindow::clearRecentPaths()
{
    if (m_recentPaths.isEmpty()) {
        return;
    }

    m_recentPaths.clear();
    if (m_pathBar) {
        m_pathBar->blockSignals(true);
        m_pathBar->setRecentPaths(m_recentPaths);
        m_pathBar->blockSignals(false);
    }

    saveSettingsAsync([](QSettings& store) {
        store.setValue("recentPaths", QStringList());
    });

    rebuildLandingLocationSections();
}

void MainWindow::setPathFavourite(const QString& path, bool favourite)
{
    const QString normalized = normalizedFilesystemPath(path);
    if (normalized.isEmpty()) {
        return;
    }

    const QStringList previous = m_favouritePaths;
    m_favouritePaths.removeAll(normalized);
    if (favourite) {
        m_favouritePaths.prepend(normalized);
        while (m_favouritePaths.size() > kMaxFavouritePaths) {
            m_favouritePaths.removeLast();
        }
    }

    if (m_favouritePaths == previous) {
        return;
    }

    const QStringList favouritePaths = m_favouritePaths;
    saveSettingsAsync([favouritePaths](QSettings& store) {
        store.setValue("favouritePaths", favouritePaths);
    });

    rebuildLandingLocationSections();
}

void MainWindow::moveFavouritePath(const QString& path, const QString& targetPath, bool insertAfterTarget)
{
    const QString normalizedPath = normalizedFilesystemPath(path);
    const QString normalizedTargetPath = normalizedFilesystemPath(targetPath);
    if (normalizedPath.isEmpty() || normalizedTargetPath.isEmpty() || normalizedPath == normalizedTargetPath) {
        return;
    }

    const int sourceIndex = m_favouritePaths.indexOf(normalizedPath);
    const int targetIndex = m_favouritePaths.indexOf(normalizedTargetPath);
    if (sourceIndex < 0 || targetIndex < 0) {
        return;
    }

    m_favouritePaths.removeAt(sourceIndex);
    int insertIndex = m_favouritePaths.indexOf(normalizedTargetPath);
    if (insertIndex < 0) {
        return;
    }
    if (insertAfterTarget) {
        ++insertIndex;
    }
    insertIndex = qBound(0, insertIndex, m_favouritePaths.size());
    m_favouritePaths.insert(insertIndex, normalizedPath);

    const QStringList favouritePaths = m_favouritePaths;
    saveSettingsAsync([favouritePaths](QSettings& store) {
        store.setValue("favouritePaths", favouritePaths);
    });

    rebuildLandingLocationSections();
}

void MainWindow::clearFavouritePaths()
{
    if (m_favouritePaths.isEmpty()) {
        return;
    }

    m_favouritePaths.clear();
    saveSettingsAsync([](QSettings& store) {
        store.setValue("favouritePaths", QStringList());
    });

    rebuildLandingLocationSections();
}

void MainWindow::syncPathCombo(const QString& path)
{
    if (!m_pathBar) {
        return;
    }

    m_pathBar->blockSignals(true);
    m_pathBar->setScanRootPath(m_currentPath);
    m_pathBar->setPath(path);
    m_pathBar->blockSignals(false);
}

void MainWindow::rebuildFilesystemWatchers()
{
    syncFilesystemWatchControllerState();
}

void MainWindow::setRefreshBusy(bool busy)
{
    if (!m_refreshAction) {
        return;
    }

    QToolButton* refreshButton = m_toolbar
        ? qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_refreshAction))
        : nullptr;

    const bool showCancel = m_scanInProgress
        || m_incrementalRefreshInProgress
        || m_postProcessInProgress;

    if (!busy) {
        m_refreshAction->setIcon(IconUtils::themeIcon({"view-refresh", "refresh"},
            QStringLiteral(":/assets/tabler-icons/refresh.svg"),
            QStringLiteral(":/assets/tabler-icons/refresh.svg")));
        m_refreshAction->setText(tr("Refresh"));
        m_refreshAction->setEnabled(true);
        if (refreshButton) {
            refreshButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        }
        updateToolbarResponsiveLayout();
        return;
    }

    if (showCancel) {
        m_refreshAction->setIcon(IconUtils::themeIcon({"process-stop", "dialog-cancel"},
            QStringLiteral(":/assets/tabler-icons/refresh-off.svg"),
            QStringLiteral(":/assets/tabler-icons/refresh-off.svg")));
        m_refreshAction->setText(tr("Cancel"));
        m_refreshAction->setEnabled(true);
        if (refreshButton) {
            refreshButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        updateToolbarResponsiveLayout();
        return;
    }

    m_refreshAction->setIcon(IconUtils::themeIcon({"view-refresh", "refresh"},
        QStringLiteral(":/assets/tabler-icons/refresh.svg"),
        QStringLiteral(":/assets/tabler-icons/refresh.svg")));
    m_refreshAction->setText(tr("Refreshing"));
    m_refreshAction->setEnabled(false);
    if (refreshButton) {
        refreshButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    updateToolbarResponsiveLayout();
}

void MainWindow::updateToolbarResponsiveLayout()
{
    if (!m_toolbar) {
        return;
    }

    auto applyToggleLabelStyle = [this](Qt::ToolButtonStyle style) {
#ifndef Q_OS_WIN
        if (m_limitToSameFilesystemAction) {
            if (QToolButton* button = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_limitToSameFilesystemAction))) {
                button->setToolButtonStyle(style);
            }
        }
#endif
        if (m_toggleFreeSpaceAction) {
            if (QToolButton* button = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_toggleFreeSpaceAction))) {
                button->setToolButtonStyle(style);
            }
        }
        if (m_toggleDirectoryTreeAction) {
            if (QToolButton* button = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_toggleDirectoryTreeAction))) {
                button->setToolButtonStyle(style);
            }
        }
        if (m_toggleTypeLegendAction) {
            if (QToolButton* button = qobject_cast<QToolButton*>(m_toolbar->widgetForAction(m_toggleTypeLegendAction))) {
                button->setToolButtonStyle(style);
            }
        }
    };

    applyToggleLabelStyle(Qt::ToolButtonTextBesideIcon);
    if (m_toolbar->sizeHint().width() <= m_toolbar->width()) {
        return;
    }

    applyToggleLabelStyle(Qt::ToolButtonIconOnly);
}

void MainWindow::setSearchBusy(bool busy)
{
    const bool landingVisible = m_centralStack && m_landingPage
        && m_centralStack->currentWidget() == m_landingPage;
    const bool blockSearchControls = landingVisible || (m_scanInProgress && !m_backgroundRefreshInProgress);
    if (m_searchEdit) {
        m_searchEdit->setEnabled(!busy && !blockSearchControls);
    }
    if (m_sizeFilterCombo) {
        m_sizeFilterCombo->setEnabled(!blockSearchControls);
    }
    if (m_searchStatusLabel) {
        m_searchStatusLabel->setVisible(busy);
    }
}

void MainWindow::applySearchFromToolbar()
{
    if (m_searchDebounceTimer) m_searchDebounceTimer->stop();

    if (!m_treemapWidget || !m_searchEdit) {
        return;
    }

    // Extract size filter bounds from the combo box.
    qint64 sizeMin = 0, sizeMax = 0;
    if (m_sizeFilterCombo) {
        const QVariantList bounds = m_sizeFilterCombo->currentData().toList();
        if (bounds.size() == 2) {
            sizeMin = bounds[0].toLongLong();
            sizeMax = bounds[1].toLongLong();
        }
    }
    // Apply size filter first to avoid double rebuild when pattern also changes.
    m_treemapWidget->setSizeFilter(sizeMin, sizeMax);

    const QString pattern = m_searchEdit->text().trimmed();
    m_treemapWidget->setSearchPattern(pattern);
    setSearchBusy(false);
    updateCurrentViewUi();
}

void MainWindow::finalizeIncrementalRefresh(IncrementalRefreshResult refreshed)
{
    const QString refreshPath = m_activeRefreshPath;

    if (refreshed.rootReplaced) {
        if (m_directoryTree) m_directoryTree->clear();
        m_scanResult = std::move(refreshed.refreshed);
        m_freeSpaceNodes = std::move(refreshed.preparedFreeSpaceNodes);
    } else {
        if (!spliceRefreshedSubtree(m_scanResult, refreshPath, std::move(refreshed.refreshed))) {
            setRefreshBusy(false);
            return;
        }
    }

    restoreHistoryFromPaths(m_preRefreshHistoryPaths, m_scanResult.root);

    const TreemapWidget::ViewState remappedView = remapViewStatePaths(m_preRefreshViewPaths, m_scanResult.root);
    if (refreshed.rootReplaced) {
        // Pass animateLayout=false: m_current still points into the just-freed old arena here,
        // and setRoot would dereference it to capture a "previous frame" pixmap before
        // updating m_current — that would be a use-after-free.
        m_treemapWidget->setRoot(m_scanResult.root, m_scanResult.arena, false, false);
    } else {
        m_treemapWidget->notifyTreeChanged();
    }
    if (remappedView.node) {
        m_treemapWidget->restoreViewStateImmediate(remappedView);
    } else if (!refreshed.rootReplaced) {
        // The viewed node no longer exists in the refreshed subtree (e.g. it was deleted).
        // setRoot already reset m_current for the rootReplaced path; for subtree-only
        // replacements we need to fall back explicitly so m_current is not left pointing
        // at the orphaned old node.
        TreemapWidget::ViewState fallback;
        fallback.node = m_scanResult.root;
        m_treemapWidget->restoreViewStateImmediate(fallback);
    }
    refreshTypeLegendAsync(m_treemapWidget->currentNode());
    scheduleTreeMaintenance();
    m_dirtyPaths.insert(refreshPath);
    setRefreshBusy(false);
    updateDirectoryTreePanel();
    updateCurrentViewUi();
    statusBar()->showMessage(tr("Refreshed: %1").arg(statusBarDisplayPath(refreshPath)), 3000);
    updateNavigationActions();
}

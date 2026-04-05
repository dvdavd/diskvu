// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "mainwindow.h"
#include "breadcrumbpathbar.h"

#include "iconutils.h"
#include "mainwindow_utils.h"

#include <algorithm>
#include <functional>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QEvent>
#include <QFileIconProvider>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QScrollArea>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

constexpr auto kLandingFavouriteMimeType = "application/x-diskscape-favourite-path";

Qt::Alignment landingTextAlignment(const QWidget* widget, Qt::Alignment vertical)
{
    const Qt::LayoutDirection direction = widget
        ? widget->layoutDirection()
        : (qApp ? qApp->layoutDirection() : Qt::LeftToRight);
    const Qt::Alignment horizontal = (direction == Qt::RightToLeft)
        ? Qt::AlignRight
        : Qt::AlignLeft;
    return horizontal | vertical;
}

QIcon landingPinIcon(bool filled)
{
    return filled
        ? IconUtils::themeIcon({},
            QStringLiteral(":/assets/tabler-icons/pin_filled.svg"),
            QStringLiteral(":/assets/tabler-icons/pin_filled.svg"))
        : IconUtils::themeIcon({},
            QStringLiteral(":/assets/tabler-icons/pin.svg"),
            QStringLiteral(":/assets/tabler-icons/pin.svg"));
}

class LandingLocationTile : public QWidget {
public:
    explicit LandingLocationTile(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_Hover, true);
    }

    void setMainButton(QToolButton* button)
    {
        if (m_mainButton) {
            m_mainButton->removeEventFilter(this);
        }
        m_mainButton = button;
        if (m_mainButton) {
            m_mainButton->installEventFilter(this);
            m_mainButton->setAcceptDrops(m_reorderEnabled);
        }
    }

    void setPinButton(QToolButton* pinButton)
    {
        if (m_pinButton) {
            m_pinButton->removeEventFilter(this);
        }
        m_pinButton = pinButton;
        if (m_pinButton) {
            m_pinButton->installEventFilter(this);
        }
        updatePinVisibility();
    }

    void setLandingPath(const QString& path)
    {
        m_path = path;
    }

    void setReorderEnabled(bool enabled)
    {
        m_reorderEnabled = enabled;
        if (m_mainButton) {
            m_mainButton->setAcceptDrops(enabled);
        }
    }

    void setMoveHandler(std::function<void(const QString&, const QString&, bool)> moveHandler)
    {
        m_moveHandler = std::move(moveHandler);
    }

    void setPinFilled(bool filled)
    {
        m_pinFilled = filled;
        updatePinPresentation();
    }

protected:
    bool event(QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::HoverEnter:
            m_hovered = true;
            updatePinVisibility();
            break;
        case QEvent::Leave:
        case QEvent::HoverLeave:
            m_hovered = false;
            m_pinHovered = false;
            setMainButtonPinHovered(false);
            updatePinVisibility();
            break;
        default:
            break;
        }
        return QWidget::event(event);
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == m_mainButton) {
            switch (event->type()) {
            case QEvent::MouseButtonPress: {
                auto* mouseEvent = static_cast<QMouseEvent*>(event);
                if (mouseEvent->button() == Qt::LeftButton) {
                    m_dragStartPos = mouseEvent->pos();
                    m_dragInProgress = false;
                }
                break;
            }
            case QEvent::MouseMove: {
                auto* mouseEvent = static_cast<QMouseEvent*>(event);
                if (!m_reorderEnabled || m_path.isEmpty()
                        || !(mouseEvent->buttons() & Qt::LeftButton)
                        || m_dragInProgress) {
                    break;
                }
                if ((mouseEvent->pos() - m_dragStartPos).manhattanLength()
                        < QApplication::startDragDistance()) {
                    break;
                }

                m_dragInProgress = true;
                auto* mimeData = new QMimeData;
                mimeData->setData(QString::fromLatin1(kLandingFavouriteMimeType), m_path.toUtf8());
                auto* drag = new QDrag(m_mainButton);
                drag->setMimeData(mimeData);
                drag->setPixmap(grab());
                drag->setHotSpot(mapFromGlobal(QCursor::pos()));
                drag->exec(Qt::MoveAction);
                m_dragInProgress = false;
                resetMainButtonVisualState();
                return true;
            }
            case QEvent::MouseButtonRelease:
                m_dragInProgress = false;
                break;
            case QEvent::DragEnter: {
                auto* dragEvent = static_cast<QDragEnterEvent*>(event);
                if (canAcceptDrop(dragEvent->mimeData())) {
                    dragEvent->acceptProposedAction();
                    return true;
                }
                break;
            }
            case QEvent::DragMove: {
                auto* dragEvent = static_cast<QDragMoveEvent*>(event);
                if (canAcceptDrop(dragEvent->mimeData())) {
                    dragEvent->acceptProposedAction();
                    return true;
                }
                break;
            }
            case QEvent::Drop: {
                auto* dropEvent = static_cast<QDropEvent*>(event);
                const QString sourcePath = dropPath(dropEvent->mimeData());
                if (sourcePath.isEmpty() || sourcePath == m_path || !m_moveHandler) {
                    break;
                }

                const bool insertAfterTarget = isDropAfter(static_cast<QWidget*>(watched),
                                                           dropEvent->position().toPoint());
                dropEvent->acceptProposedAction();
                QTimer::singleShot(0, this, [moveHandler = m_moveHandler,
                                             sourcePath,
                                             targetPath = m_path,
                                             insertAfterTarget]() {
                    moveHandler(sourcePath, targetPath, insertAfterTarget);
                });
                return true;
            }
            default:
                break;
            }
        }

        if (watched == m_pinButton) {
            switch (event->type()) {
            case QEvent::Enter:
            case QEvent::HoverEnter:
                m_pinHovered = true;
                setMainButtonPinHovered(true);
                updatePinPresentation();
                break;
            case QEvent::Leave:
            case QEvent::HoverLeave:
                m_pinHovered = false;
                setMainButtonPinHovered(false);
                updatePinPresentation();
                break;
            default:
                break;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

private:
    void setMainButtonPinHovered(bool hovered)
    {
        if (!m_mainButton) {
            return;
        }
        m_mainButton->setProperty("pinHovered", hovered);
        m_mainButton->style()->unpolish(m_mainButton);
        m_mainButton->style()->polish(m_mainButton);
    }

    void updatePinVisibility()
    {
        if (!m_pinButton) {
            return;
        }
        updatePinPresentation();
    }

    void updatePinPresentation()
    {
        if (!m_pinButton) {
            return;
        }
        m_pinButton->setVisible(m_hovered || m_pinHovered);
        m_pinButton->setIcon(landingPinIcon(m_pinFilled || m_pinHovered));
    }

    bool canAcceptDrop(const QMimeData* mimeData) const
    {
        const QString sourcePath = dropPath(mimeData);
        return m_reorderEnabled && !m_path.isEmpty() && !sourcePath.isEmpty() && sourcePath != m_path;
    }

    QString dropPath(const QMimeData* mimeData) const
    {
        if (!mimeData || !mimeData->hasFormat(QString::fromLatin1(kLandingFavouriteMimeType))) {
            return {};
        }
        return QString::fromUtf8(mimeData->data(QString::fromLatin1(kLandingFavouriteMimeType)));
    }

    void resetMainButtonVisualState()
    {
        if (!m_mainButton) {
            return;
        }

        if (m_mainButton->isDown()) {
            m_mainButton->setDown(false);
        }
        m_mainButton->update();
    }

    bool isDropAfter(const QWidget* target, const QPoint& pos) const
    {
        if (!target) {
            return false;
        }
        return pos.x() >= target->width() / 2;
    }

    QToolButton* m_mainButton = nullptr;
    QToolButton* m_pinButton = nullptr;
    QString m_path;
    QPoint m_dragStartPos;
    std::function<void(const QString&, const QString&, bool)> m_moveHandler;
    bool m_hovered = false;
    bool m_pinHovered = false;
    bool m_pinFilled = false;
    bool m_reorderEnabled = false;
    bool m_dragInProgress = false;
};

QIcon landingThemeIcon(QWidget* widget, const QStringList& iconNames, QStyle::StandardPixmap fallback)
{
    QIcon icon;
    for (const QString& iconName : iconNames) {
        icon = QIcon::fromTheme(iconName);
        if (!icon.isNull()) {
            break;
        }
    }

    if (icon.isNull() && widget) {
        icon = widget->style()->standardIcon(fallback);
    }
    return icon;
}

QIcon landingPathIcon(const QString& path)
{
    static QFileIconProvider iconProvider;
    const QFileInfo info(path);
    return iconProvider.icon(info);
}

QString landingPathLabel(const QString& path)
{
    const QString normalized = normalizedFilesystemPath(path);
    if (normalized.isEmpty()) {
        return {};
    }
    if (normalized == normalizedFilesystemPath(QDir::homePath())) {
        return QObject::tr("Home");
    }
    if (normalized == QStringLiteral("/")) {
        return QStringLiteral("/");
    }

    const QFileInfo info(normalized);
    return info.fileName().isEmpty() ? QDir::toNativeSeparators(normalized) : info.fileName();
}

QStringList existingLandingPaths(const QStringList& paths)
{
    QStringList existing;
    existing.reserve(paths.size());
    for (const QString& path : paths) {
        if (QFileInfo::exists(path)) {
            existing.push_back(path);
        }
    }
    return existing;
}

void clearLayout(QLayout* layout, bool deleteWidgets = true)
{
    if (!layout) {
        return;
    }

    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QLayout* childLayout = item->layout()) {
            clearLayout(childLayout, deleteWidgets);
            delete childLayout;
        }
        if (deleteWidgets) {
            delete item->widget();
        }
        delete item;
    }
}

void relayoutLandingSection(QWidget* section, int columns)
{
    auto* layout = section ? qobject_cast<QGridLayout*>(section->layout()) : nullptr;
    if (!layout) {
        return;
    }

    auto tiles = section->findChildren<QWidget*>(QStringLiteral("landingLocationTileShell"),
                                                 Qt::FindDirectChildrenOnly);
    std::sort(tiles.begin(), tiles.end(), [](const QWidget* lhs, const QWidget* rhs) {
        return lhs->property("landingIndex").toInt() < rhs->property("landingIndex").toInt();
    });

    clearLayout(layout, false);
    for (int i = 0; i <= tiles.size(); ++i) {
        layout->setColumnStretch(i, 0);
    }

    for (int i = 0; i < tiles.size(); ++i) {
        layout->addWidget(tiles.at(i), i / columns, i % columns, Qt::AlignLeft | Qt::AlignTop);
    }

    layout->setColumnStretch(columns, 1);
}

}

void MainWindow::updatePathBarChrome()
{
    if (!m_pathBar || !m_searchEdit) {
        return;
    }

    int controlHeight = m_searchEdit->sizeHint().height();
    if (m_sizeFilterCombo) {
        m_sizeFilterCombo->ensurePolished();
        controlHeight = qMax(controlHeight, m_sizeFilterCombo->sizeHint().height());
    }
    m_pathBar->setFixedHeight(controlHeight);
    if (m_sizeFilterCombo) {
        m_sizeFilterCombo->setFixedHeight(controlHeight);
        m_sizeFilterCombo->updateGeometry();
    }
    m_pathBar->setChromeBorderColor(landingLocationBorderColor());
}

void MainWindow::updateLandingPageChrome()
{
    if (!m_landingPage) {
        return;
    }

    const QFont generalFont = generalUiFont();

    if (auto* appTitle = m_landingPage->findChild<QLabel*>(QStringLiteral("landingAppTitle"))) {
        QFont font = generalFont;
        font.setWeight(QFont::Bold);
        appTitle->setFont(font);
        appTitle->setAlignment(landingTextAlignment(appTitle, Qt::AlignVCenter));
    }

    if (auto* tagline = m_landingPage->findChild<QLabel*>(QStringLiteral("landingTagline"))) {
        tagline->setFont(generalFont);
        tagline->setAlignment(landingTextAlignment(tagline, Qt::AlignTop));
    }

    const auto sectionHeaders = m_landingPage->findChildren<QLabel*>(QStringLiteral("landingSectionHeader"));
    for (QLabel* sectionHeader : sectionHeaders) {
        QFont font = generalFont;
        font.setWeight(QFont::Medium);
        sectionHeader->setFont(font);
        sectionHeader->setAlignment(landingTextAlignment(sectionHeader, Qt::AlignVCenter));
    }

    if (auto* divider = m_landingPage->findChild<QFrame*>(QStringLiteral("landingDivider"))) {
        divider->setStyleSheet(QStringLiteral("background: %1;")
            .arg(landingLocationBorderColor().name(QColor::HexArgb)));
    }

    if (auto* locContainer = m_landingPage->findChild<QWidget*>(QStringLiteral("landingLocationContainer"))) {
        locContainer->setStyleSheet(landingLocationStyleSheet());
        locContainer->style()->unpolish(locContainer);
        locContainer->style()->polish(locContainer);
        locContainer->update();
    }

    const auto buttons = m_landingPage->findChildren<QToolButton*>();
    for (QToolButton* button : buttons) {
        button->setFont(generalFont);
        if (button->objectName() == QStringLiteral("landingLocationPin")) {
            button->setIcon(landingPinIcon(false));
            continue;
        }
        const QString landingPath = button->property("landingPath").toString();
        if (!landingPath.isEmpty()) {
            button->setIcon(landingPathIcon(landingPath));
            continue;
        }
        const QStringList iconNames = button->property("themeIconNames").toStringList();
        if (iconNames.isEmpty()) {
            continue;
        }

        const auto fallback = static_cast<QStyle::StandardPixmap>(button->property("fallbackIcon").toInt());
        QIcon icon;
        for (const QString& iconName : iconNames) {
            icon = QIcon::fromTheme(iconName);
            if (!icon.isNull()) {
                break;
            }
        }
        if (icon.isNull()) {
            icon = style()->standardIcon(fallback);
        }
        button->setIcon(icon);
    }

    const auto emptyLabels = m_landingPage->findChildren<QLabel*>(QStringLiteral("landingLocationEmpty"));
    for (QLabel* emptyLabel : emptyLabels) {
        emptyLabel->setFont(generalFont);
    }
}

void MainWindow::updateToolbarIcons(const QColor& iconColor)
{
    const QColor color = iconColor.isValid()
        ? iconColor
        : (qApp ? qApp->palette().color(QPalette::ButtonText) : QColor(QStringLiteral("#444444")));
    auto icon = [&](const QString& resource) {
        return makeRecoloredSvgIcon(resource, color);
    };

    if (m_scanCustomAction)
        m_scanCustomAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/folder-open.svg")));
    if (m_homeAction)
        m_homeAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/home.svg")));
    if (m_backAction)
        m_backAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/chevron-left.svg")));
    if (m_upAction)
        m_upAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/chevron-up.svg")));
    if (m_refreshAction) {
        const bool showCancel = m_scanInProgress
            || m_incrementalRefreshInProgress
            || m_postProcessInProgress;
        m_refreshAction->setIcon(showCancel
            ? icon(QStringLiteral(":/assets/tabler-icons/refresh-off.svg"))
            : icon(QStringLiteral(":/assets/tabler-icons/refresh.svg")));
    }
    m_permissionWarningIcon = icon(QStringLiteral(":/assets/tabler-icons/alert-triangle.svg"));
    if (m_permissionWarningAction)
        m_permissionWarningAction->setIcon(m_permissionWarningIcon);
    if (m_warningsMenuAction)
        m_warningsMenuAction->setIcon(m_permissionWarningIcon);
#ifndef Q_OS_WIN
    if (m_limitToSameFilesystemAction)
        m_limitToSameFilesystemAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/freeze-row-column.svg")));
#endif
    if (m_toggleFreeSpaceAction)
        m_toggleFreeSpaceAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/chart-donut.svg")));
    if (m_toggleDirectoryTreeAction)
        m_toggleDirectoryTreeAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/folders.svg")));
    if (m_zoomInAction)
        m_zoomInAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/zoom-in.svg")));
    if (m_zoomOutAction)
        m_zoomOutAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/zoom-out.svg")));
    if (m_resetZoomAction)
        m_resetZoomAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/zoom-reset.svg")));
    if (m_toggleTypeLegendAction)
        m_toggleTypeLegendAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/list-details.svg")));
    if (m_settingsAction)
        m_settingsAction->setIcon(icon(QStringLiteral(":/assets/tabler-icons/settings.svg")));
    if (m_menuButton)
        m_menuButton->setIcon(icon(QStringLiteral(":/assets/tabler-icons/menu-2.svg")));
}


void MainWindow::updateTypeLegendPanel()
{
    if (!m_typeLegendTree) {
        return;
    }

    if (!m_scanResult.root) {
        m_typeLegendTree->clear();
        return;
    }

    FileNode* const legendRoot = m_treemapWidget && m_treemapWidget->currentNode()
        ? m_treemapWidget->currentNode()
        : m_scanResult.root;
    const std::vector<bool> searchReach = m_treemapWidget
        ? m_treemapWidget->captureSearchReachSnapshot() : std::vector<bool>{};
    populateTypeLegendItems(m_typeLegendTree, nullptr,
                            m_treemapWidget, collectAndSortFileSummaries(legendRoot, searchReach));
}

QWidget* MainWindow::createLandingPage()
{
    auto* page = new QWidget(this);
    const QFont generalFont = generalUiFont();
    page->setAutoFillBackground(true);
    page->setBackgroundRole(QPalette::Window);

    auto* rootLayout = new QHBoxLayout(page);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* idlePage = new QWidget(page);
    idlePage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    idlePage->setFixedWidth(420);
    {
        auto* leftLayout = new QVBoxLayout(idlePage);
        leftLayout->setContentsMargins(56, 0, 48, 0);
        leftLayout->setSpacing(0);
        leftLayout->addStretch(1);

        auto* logo = new QLabel(idlePage);
        logo->setPixmap(QIcon(QStringLiteral(":/assets/diskscape.svg")).pixmap(220, 220));
        logo->setFixedSize(220, 220);
        logo->setAlignment(landingTextAlignment(logo, Qt::AlignVCenter));
        leftLayout->addWidget(logo);
        leftLayout->addSpacing(22);

        auto* appTitle = new QLabel(QStringLiteral("Diskscape"), idlePage);
        appTitle->setObjectName(QStringLiteral("landingAppTitle"));
        appTitle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        {
            QFont f = generalFont;
            f.setWeight(QFont::Bold);
            appTitle->setFont(f);
        }
        appTitle->setAlignment(landingTextAlignment(appTitle, Qt::AlignVCenter));
        leftLayout->addWidget(appTitle);
        leftLayout->addSpacing(10);

        auto* tagline = new QLabel(
            tr("Visualise disk usage as an interactive treemap.\nFind what\u2019s using space at a glance."),
            idlePage);
        tagline->setObjectName(QStringLiteral("landingTagline"));
        tagline->setWordWrap(true);
        tagline->setFont(generalFont);
        tagline->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        tagline->setAlignment(landingTextAlignment(tagline, Qt::AlignTop));
        leftLayout->addWidget(tagline);
        leftLayout->addStretch(1);
    }

    auto* divider = new QFrame(page);
    divider->setObjectName(QStringLiteral("landingDivider"));
    divider->setFrameShape(QFrame::NoFrame);
    divider->setFixedWidth(1);
    divider->setAttribute(Qt::WA_StyledBackground, true);
    divider->setStyleSheet(QStringLiteral("background: %1;")
        .arg(landingLocationBorderColor().name(QColor::HexArgb)));

    auto* rightPanel = new QScrollArea(page);
    rightPanel->setObjectName(QStringLiteral("landingRightPanel"));
    rightPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rightPanel->setFrameShape(QFrame::NoFrame);
    rightPanel->setWidgetResizable(true);
    rightPanel->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rightPanel->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* rightPanelContent = new QWidget(rightPanel);
    rightPanelContent->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    auto* rightLayout = new QVBoxLayout(rightPanelContent);
    rightLayout->setContentsMargins(56, 16, 56, 16);
    rightLayout->setSpacing(0);
    rightLayout->addStretch(1);

    auto* locContainer = new QWidget(rightPanelContent);
    locContainer->setObjectName(QStringLiteral("landingLocationContainer"));
    locContainer->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    locContainer->installEventFilter(this);
    locContainer->setStyleSheet(landingLocationStyleSheet());
    auto* locLayout = new QVBoxLayout(locContainer);
    locLayout->setContentsMargins(18, 18, 18, 18);
    locLayout->setSpacing(12);

    rightLayout->addWidget(locContainer);
    rightLayout->addStretch(1);
    rightPanel->setWidget(rightPanelContent);

    rootLayout->addWidget(idlePage, 5);
    rootLayout->addWidget(divider);
    rootLayout->addWidget(rightPanel, 15);

    return page;
}

void MainWindow::collectLandingLocationPaths(QStringList& favouritePaths,
                                             QStringList& recentPaths,
                                             QStringList& devicePaths) const
{
    favouritePaths = existingLandingPaths(m_favouritePaths);
    recentPaths = existingLandingPaths(m_recentPaths);
    for (const QString& favouritePath : favouritePaths) {
        recentPaths.removeAll(favouritePath);
    }

    devicePaths = existingLandingPaths(mountedDevicePaths());
    for (const QString& favouritePath : favouritePaths) {
        devicePaths.removeAll(favouritePath);
    }
    for (const QString& recentPath : recentPaths) {
        devicePaths.removeAll(recentPath);
    }
}

void MainWindow::populateOpenLocationMenu(QMenu* menu)
{
    if (!menu) {
        return;
    }

    menu->clear();
    QStringList favouritePaths;
    QStringList recentPaths;
    QStringList devicePaths;
    collectLandingLocationPaths(favouritePaths, recentPaths, devicePaths);

    const auto addPathActions = [this, menu](const QStringList& paths) {
        for (const QString& path : paths) {
            const QString normalized = normalizedFilesystemPath(path);
            if (normalized.isEmpty()) {
                continue;
            }

            QAction* action = menu->addAction(landingPathIcon(normalized),
                                              landingPathLabel(normalized));
            action->setToolTip(QDir::toNativeSeparators(normalized));
            connect(action, &QAction::triggered, this, [this, normalized] {
                startScan(normalized);
            });
        }
    };

    addPathActions(favouritePaths);
    menu->addSeparator();
    for (const QString& path : recentPaths) {
        const QString normalized = normalizedFilesystemPath(path);
        if (normalized.isEmpty()) {
            continue;
        }

        QAction* action = menu->addAction(landingPathIcon(normalized),
                                          landingPathLabel(normalized));
        action->setToolTip(QDir::toNativeSeparators(normalized));
        connect(action, &QAction::triggered, this, [this, normalized] {
            promoteRecentPath(normalized);
            startScan(normalized);
        });
    }
    menu->addSeparator();
    addPathActions(devicePaths);
    menu->addSeparator();

    QAction* browseAction = menu->addAction(
        landingThemeIcon(this,
                         {QStringLiteral("document-open-folder"),
                          QStringLiteral("document-open"),
                          QStringLiteral("folder-open")},
                         QStyle::SP_DirOpenIcon),
        tr("Browse\u2026"));
    connect(browseAction, &QAction::triggered, this, &MainWindow::openDirectory);
}

void MainWindow::rebuildLandingLocationSections()
{
    if (!m_landingPage) {
        return;
    }

    const QFont generalFont = generalUiFont();
    auto* locContainer = m_landingPage->findChild<QWidget*>(QStringLiteral("landingLocationContainer"));
    auto* locLayout = locContainer ? qobject_cast<QVBoxLayout*>(locContainer->layout()) : nullptr;
    if (!locLayout) {
        return;
    }

    const int tileWidth = landingTileWidth();
    const int tileIconSize = tileWidth / 2;
    int effectiveWidth = locContainer->width();
    if (effectiveWidth <= 1) {
        if (QWidget* parent = locContainer->parentWidget()) {
            effectiveWidth = qMax(effectiveWidth, parent->width());
            effectiveWidth = qMax(effectiveWidth, parent->minimumWidth());
        }
    }
    const int availableWidth = qMax(1, effectiveWidth - locLayout->contentsMargins().left()
                                       - locLayout->contentsMargins().right());
    const int columns = qMax(1, (availableWidth + kLandingTileSpacing)
                                / (tileWidth + kLandingTileSpacing));
    locContainer->setProperty("landingColumns", columns);
    locContainer->setProperty("landingRebuildPending", false);

    clearLayout(locLayout);

    QStringList favouritePaths;
    QStringList recentPaths;
    QStringList devicePaths;
    collectLandingLocationPaths(favouritePaths, recentPaths, devicePaths);

    const auto addEmptyState = [&](const QString& text) {
        auto* label = new QLabel(text, locContainer);
        label->setObjectName(QStringLiteral("landingLocationEmpty"));
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        label->setAlignment(landingTextAlignment(label, Qt::AlignTop));
        locLayout->addWidget(label);
    };

    const auto addLocationSection = [&](const QString& title,
                                        const QStringList& paths,
                                        bool allowPinning,
                                        bool promoteRecentOnOpen,
                                        bool allowFavouriteReorder) {
        auto* header = new QLabel(title, locContainer);
        header->setObjectName(QStringLiteral("landingSectionHeader"));
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        header->setAlignment(landingTextAlignment(header, Qt::AlignVCenter));
        locLayout->addWidget(header);

        auto* section = new QWidget(locContainer);
        section->setProperty("landingTileSection", true);
        auto* sectionLayout = new QGridLayout(section);
        sectionLayout->setContentsMargins(0, 0, 0, 0);
        sectionLayout->setHorizontalSpacing(kLandingTileSpacing);
        sectionLayout->setVerticalSpacing(kLandingTileSpacing);

        int index = 0;
        for (const QString& path : paths) {
            const QString normalized = normalizedFilesystemPath(path);
            auto* tile = new LandingLocationTile(section);
            tile->setObjectName(QStringLiteral("landingLocationTileShell"));
            tile->setProperty("landingIndex", index);
            tile->setProperty("landingPath", normalized);
            tile->setFixedWidth(tileWidth);
            auto* tileLayout = new QGridLayout(tile);
            tileLayout->setContentsMargins(0, 0, 0, 0);
            tileLayout->setSpacing(0);

            auto* button = new QToolButton(tile);
            button->setObjectName(QStringLiteral("landingLocationTile"));
            button->setProperty("landingPath", normalized);
            button->setIcon(landingPathIcon(normalized));
            button->setIconSize(QSize(tileIconSize, tileIconSize));
            button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            button->setText(landingPathLabel(normalized));
            button->setToolTip(QDir::toNativeSeparators(normalized));
            button->setFont(generalFont);
            button->setFixedWidth(tileWidth);
            button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Minimum);
            button->setMinimumHeight(button->sizeHint().height());
            button->setCursor(Qt::PointingHandCursor);
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            tile->setMainButton(button);
            tile->setLandingPath(normalized);
            tile->setReorderEnabled(allowFavouriteReorder);
            if (allowFavouriteReorder) {
                tile->setMoveHandler([this](const QString& sourcePath,
                                            const QString& targetPath,
                                            bool insertAfterTarget) {
                    moveFavouritePath(sourcePath, targetPath, insertAfterTarget);
                });
            }
            connect(button, &QToolButton::clicked, this, [this, normalized, promoteRecentOnOpen] {
                if (promoteRecentOnOpen) {
                    promoteRecentPath(normalized);
                }
                startScan(normalized);
            });
            const auto showContextMenu = [this, normalized, allowPinning](QWidget* source, const QPoint& pos) {
                QMenu menu(source);
                QAbstractButton* sourceButton = qobject_cast<QAbstractButton*>(source);
                if (sourceButton) {
                    sourceButton->setDown(true);
                    sourceButton->update();
                }

                const bool isFavourite = m_favouritePaths.contains(normalized);
                QAction* pinAction = menu.addAction(isFavourite
                    ? tr("Unpin from Favourites")
                    : tr("Pin to Favourites"));
                pinAction->setIcon(landingPinIcon(isFavourite));
                QAction* removeRecentAction = nullptr;
                if (allowPinning) {
                    removeRecentAction = menu.addAction(tr("Remove from Recents"));
                }
                menu.addSeparator();
                QAction* clearAction = menu.addAction(allowPinning
                    ? tr("Clear Recents")
                    : tr("Clear Favourites"));
                QAction* chosenAction = menu.exec(source->mapToGlobal(pos));
                if (sourceButton) {
                    sourceButton->setDown(false);
                    sourceButton->update();
                }
                if (!chosenAction) {
                    return;
                }
                if (chosenAction == pinAction) {
                    QTimer::singleShot(0, this, [this, normalized, isFavourite]() {
                        setPathFavourite(normalized, !isFavourite);
                    });
                    return;
                }
                if (chosenAction == removeRecentAction) {
                    QTimer::singleShot(0, this, [this, normalized]() {
                        removeRecentPath(normalized);
                    });
                    return;
                }
                if (chosenAction == clearAction) {
                    QTimer::singleShot(0, this, [this, allowPinning]() {
                        if (allowPinning) {
                            clearRecentPaths();
                        } else {
                            clearFavouritePaths();
                        }
                    });
                }
            };
            connect(button, &QWidget::customContextMenuRequested, this,
                    [showContextMenu, button](const QPoint& pos) { showContextMenu(button, pos); });
            tileLayout->addWidget(button, 0, 0);

            if (allowPinning || m_favouritePaths.contains(normalized)) {
                auto* pinButton = new QToolButton(tile);
                pinButton->setObjectName(QStringLiteral("landingLocationPin"));
                const bool isFavourite = m_favouritePaths.contains(normalized);
                pinButton->setIcon(landingPinIcon(isFavourite));
                pinButton->setAutoRaise(true);
                pinButton->setToolTip(allowPinning ? tr("Pin to favourites") : tr("Remove from favourites"));
                pinButton->setFixedSize(30, 30);
                pinButton->setCursor(Qt::PointingHandCursor);
                pinButton->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(pinButton, &QToolButton::clicked, this, [this, normalized, allowPinning] {
                    setPathFavourite(normalized, allowPinning);
                });
                connect(pinButton, &QWidget::customContextMenuRequested, this,
                        [showContextMenu, pinButton](const QPoint& pos) { showContextMenu(pinButton, pos); });
                tile->setPinButton(pinButton);
                tile->setPinFilled(isFavourite);
                tileLayout->addWidget(pinButton, 0, 0, Qt::AlignTop | Qt::AlignRight);
            }

            sectionLayout->addWidget(tile, index / columns, index % columns, Qt::AlignLeft | Qt::AlignTop);
            ++index;
        }

        sectionLayout->setColumnStretch(columns, 1);

        locLayout->addWidget(section);
    };

    if (!favouritePaths.isEmpty()) {
        addLocationSection(tr("Favourites"), favouritePaths, false, false, true);
    } else {
        auto* header = new QLabel(tr("Favourites"), locContainer);
        header->setObjectName(QStringLiteral("landingSectionHeader"));
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        header->setAlignment(landingTextAlignment(header, Qt::AlignVCenter));
        locLayout->addWidget(header);
        addEmptyState(tr("Pin a recent location to keep it on the landing page."));
    }

    locLayout->addSpacing(4);

    if (!recentPaths.isEmpty()) {
        addLocationSection(tr("Recent"), recentPaths, true, true, false);
    } else {
        auto* header = new QLabel(tr("Recent"), locContainer);
        header->setObjectName(QStringLiteral("landingSectionHeader"));
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        header->setAlignment(landingTextAlignment(header, Qt::AlignVCenter));
        locLayout->addWidget(header);
        addEmptyState(tr("Recent locations appear here after you scan a folder."));
    }

    locLayout->addSpacing(4);

    if (!devicePaths.isEmpty()) {
        addLocationSection(tr("Mounted Devices"), devicePaths, true, false, false);
        locLayout->addSpacing(4);
    }

    auto* browseHeader = new QLabel(tr("Browse"), locContainer);
    browseHeader->setObjectName(QStringLiteral("landingSectionHeader"));
    browseHeader->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    browseHeader->setAlignment(landingTextAlignment(browseHeader, Qt::AlignVCenter));
    locLayout->addWidget(browseHeader);

    auto* browseSection = new QWidget(locContainer);
    auto* browseRow = new QHBoxLayout(browseSection);
    browseRow->setContentsMargins(0, 0, 0, 0);
    browseRow->setSpacing(10);

    auto* browseButton = new QToolButton(browseSection);
    browseButton->setObjectName(QStringLiteral("landingLocationTile"));
    const QStringList browseIcons = {QStringLiteral("document-open-folder"),
                                     QStringLiteral("document-open"),
                                     QStringLiteral("folder-open")};
    browseButton->setProperty("themeIconNames", browseIcons);
    browseButton->setProperty("fallbackIcon", static_cast<int>(QStyle::SP_DirOpenIcon));
    browseButton->setIcon(landingThemeIcon(this, browseIcons, QStyle::SP_DirOpenIcon));
    browseButton->setIconSize(QSize(tileIconSize, tileIconSize));
    browseButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    browseButton->setText(tr("Browse\u2026"));
    browseButton->setFont(generalFont);
    browseButton->setFixedWidth(tileWidth);
    browseButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Minimum);
    browseButton->setMinimumHeight(browseButton->sizeHint().height());
    browseButton->setCursor(Qt::PointingHandCursor);
    connect(browseButton, &QToolButton::clicked, this, &MainWindow::openDirectory);
    browseRow->addWidget(browseButton, 0, Qt::AlignLeft | Qt::AlignTop);
    browseRow->addStretch(1);
    locLayout->addWidget(browseSection);
    locLayout->addStretch(1);

    updateLandingPageChrome();
}

void MainWindow::relayoutLandingLocationSections()
{
    if (!m_landingPage) {
        return;
    }

    auto* locContainer = m_landingPage->findChild<QWidget*>(QStringLiteral("landingLocationContainer"));
    auto* locLayout = locContainer ? qobject_cast<QVBoxLayout*>(locContainer->layout()) : nullptr;
    if (!locLayout) {
        return;
    }

    const int availableWidth = qMax(1, locContainer->width() - locLayout->contentsMargins().left()
                                       - locLayout->contentsMargins().right());
    const int columns = qMax(1, (availableWidth + kLandingTileSpacing)
                                / (landingTileWidth() + kLandingTileSpacing));
    locContainer->setProperty("landingColumns", columns);
    locContainer->setProperty("landingRebuildPending", false);

    const auto sections = locContainer->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* section : sections) {
        if (section->property("landingTileSection").toBool()) {
            relayoutLandingSection(section, columns);
        }
    }

    locLayout->activate();
    locContainer->updateGeometry();
}

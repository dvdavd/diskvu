// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "breadcrumbpathbar.h"
#include "iconutils.h"
#include "mainwindow_utils.h"

#include <QCompleter>
#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QEvent>
#include <QStackedLayout>
#include <QStyle>
#include <QStringListModel>
#include <QToolButton>

namespace {

}

BreadcrumbPathBar::BreadcrumbPathBar(QWidget* parent)
    : QWidget(parent)
{
    const QFont generalFont = generalUiFont();

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_fieldFrame = new QWidget(this);
    m_fieldFrame->setObjectName(QStringLiteral("breadcrumbField"));
    auto* fieldLayout = new QHBoxLayout(m_fieldFrame);
    fieldLayout->setContentsMargins(6, 0, 4, 0);
    fieldLayout->setSpacing(0);

    m_modeLayout = new QStackedLayout();
    m_modeLayout->setContentsMargins(0, 0, 0, 0);
    m_modeLayout->setStackingMode(QStackedLayout::StackOne);

    m_breadcrumbView = new QWidget(this);
    auto* breadcrumbLayout = new QHBoxLayout(m_breadcrumbView);
    breadcrumbLayout->setContentsMargins(0, 0, 0, 0);
    breadcrumbLayout->setSpacing(0);

    m_crumbContainer = new QWidget(m_breadcrumbView);
    m_crumbLayout = new QHBoxLayout(m_crumbContainer);
    m_crumbLayout->setContentsMargins(0, 0, 0, 0);
    m_crumbLayout->setSpacing(0);
    breadcrumbLayout->addWidget(m_crumbContainer);
    breadcrumbLayout->addStretch(1);

    m_lineEdit = new QLineEdit(this);
    m_lineEdit->setFont(generalFont);
    m_completionModel = new QStringListModel(this);
    m_completer = new QCompleter(m_completionModel, this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_lineEdit->setCompleter(m_completer);

    m_modeLayout->addWidget(m_breadcrumbView);
    m_modeLayout->addWidget(m_lineEdit);

    auto* modeHost = new QWidget(m_fieldFrame);
    modeHost->setLayout(m_modeLayout);
    fieldLayout->addWidget(modeHost, 1);

    m_editButton = new QToolButton(m_fieldFrame);
    m_editButton->setAutoRaise(true);
    m_editButton->setCursor(Qt::PointingHandCursor);
    m_editButton->setIcon(IconUtils::themeIcon({"document-edit"},
        QStringLiteral(":/assets/tabler-icons/pencil.svg"),
        QStringLiteral(":/assets/tabler-icons/pencil.svg")));
    m_editButton->setToolTip(tr("Edit path"));
    fieldLayout->addWidget(m_editButton);

    rootLayout->addWidget(m_fieldFrame, 1);

    connect(m_editButton, &QToolButton::clicked, this, [this]() {
        if (m_modeLayout->currentWidget() == m_lineEdit) {
            exitEditMode();
            return;
        }
        enterEditMode();
    });
    connect(m_lineEdit, &QLineEdit::returnPressed, this, &BreadcrumbPathBar::activateCurrentEditorPath);
    connect(m_lineEdit, &QLineEdit::editingFinished, this, [this]() {
        QWidget* focusWidget = qApp->focusWidget();
        if (focusWidget == m_editButton) {
            return;
        }
        exitEditMode();
    });
    m_lineEdit->installEventFilter(this);

    refreshChromeStyles();
    m_breadcrumbView->setFont(generalFont);
    rebuildCrumbs();
}

void BreadcrumbPathBar::setPath(const QString& path)
{
    const QString normalized = normalizedPath(path);
    if (m_path == normalized) {
        m_lineEdit->setText(QDir::toNativeSeparators(normalized));
        return;
    }

    m_path = normalized;
    m_lineEdit->setText(QDir::toNativeSeparators(m_path));
    rebuildCrumbs();
}

void BreadcrumbPathBar::setScanRootPath(const QString& path)
{
    const QString normalized = normalizedPath(path);
    if (m_scanRootPath == normalized) {
        return;
    }

    m_scanRootPath = normalized;
    rebuildCrumbs();
}

QString BreadcrumbPathBar::path() const
{
    if (m_modeLayout->currentWidget() == m_lineEdit) {
        return m_lineEdit->text().trimmed();
    }
    return m_path;
}

void BreadcrumbPathBar::setPlaceholderText(const QString& text)
{
    m_lineEdit->setPlaceholderText(text);
}

void BreadcrumbPathBar::setRecentPaths(const QStringList& recentPaths)
{
    m_recentPaths = recentPaths;
    m_completionModel->setStringList(m_recentPaths);
}

void BreadcrumbPathBar::setChromeBorderColor(const QColor& color)
{
    if (m_chromeBorderColor == color) {
        return;
    }

    m_chromeBorderColor = color;
    refreshChromeStyles();
}

void BreadcrumbPathBar::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);

    switch (event->type()) {
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
    case QEvent::StyleChange:
    case QEvent::ThemeChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
        m_breadcrumbView->setFont(generalUiFont());
        m_lineEdit->setFont(generalUiFont());
        rebuildCrumbs();
        refreshChromeStyles();
        if (m_editButton) {
            m_editButton->setIcon(IconUtils::themeIcon({"document-edit"},
                QStringLiteral(":/assets/tabler-icons/pencil.svg"),
                QStringLiteral(":/assets/tabler-icons/pencil.svg")));
        }
        break;
    default:
        break;
    }
}

void BreadcrumbPathBar::refreshChromeStyles()
{
    const QColor borderColor = m_chromeBorderColor.isValid()
        ? m_chromeBorderColor
        : palette().color(QPalette::Mid);

    m_fieldFrame->setStyleSheet(QStringLiteral(
        "QWidget#breadcrumbField {"
        "  background: palette(base);"
        "  border: none;"
        "  border-bottom: 1px solid %1;"
        "  border-radius: 0px;"
        "}"
        "QToolButton {"
        "  border: 1px solid transparent;"
        "  border-radius: 5px;"
        "  background: transparent;"
        "  padding: 2px 6px;"
        "  margin: 0;"
        "}"
        "QToolButton:hover {"
        "  border-color: palette(mid);"
        "  background: palette(button);"
        "}"
        "QToolButton:pressed {"
        "  border-color: palette(highlight);"
        "  background: palette(midlight);"
        "}"
        "QLineEdit {"
        "  border: none;"
        "  background: transparent;"
        "  padding: 0 6px;"
        "}"
        "QLabel {"
        "  background: transparent;"
        "}"
    ).arg(borderColor.name(QColor::HexArgb)));

    m_completer->popup()->setObjectName(QStringLiteral("pathCompleterPopup"));
    m_completer->popup()->setStyleSheet(QStringLiteral(
        "QAbstractItemView#pathCompleterPopup {"
        "  background: palette(base);"
        "  color: palette(text);"
        "  border: 1px solid palette(mid);"
        "  selection-background-color: palette(highlight);"
        "  selection-color: palette(highlighted-text);"
        "}"
        "QAbstractItemView#pathCompleterPopup:hover {"
        "  border-color: palette(highlight);"
        "}"
    ));
}

void BreadcrumbPathBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    enterEditMode();
    QWidget::mouseDoubleClickEvent(event);
}

bool BreadcrumbPathBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_lineEdit && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            exitEditMode();
            return true;
        }
    }

    if (m_modeLayout->currentWidget() == m_lineEdit
            && event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        QWidget* popup = m_completer ? m_completer->popup() : nullptr;
        const auto containsGlobalPos = [mouseEvent](QWidget* widget) {
            if (!widget || !widget->isVisible()) {
                return false;
            }

            const QPoint localPos = widget->mapFromGlobal(mouseEvent->globalPosition().toPoint());
            return widget->rect().contains(localPos);
        };
        const bool clickInsideEditor = containsGlobalPos(m_lineEdit);
        const bool clickInsidePopup = containsGlobalPos(popup);
        const bool clickInsideToggleButton = containsGlobalPos(m_editButton);
        if (!clickInsideEditor && !clickInsidePopup && !clickInsideToggleButton) {
            exitEditMode();
        }
    }

    return QWidget::eventFilter(watched, event);
}

void BreadcrumbPathBar::rebuildCrumbs()
{
    const QColor mutedTextColor = [&]() {
        QColor color = palette().color(QPalette::WindowText);
        color.setAlphaF(0.72);
        return color;
    }();
    const QString mutedTextCss = mutedTextColor.name(QColor::HexArgb);

    while (QLayoutItem* item = m_crumbLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    const QList<BreadcrumbPathSegment> segments = breadcrumbPathSegments(m_path);
    if (segments.isEmpty()) {
        auto* placeholder = new QLabel(tr("Open a directory"), m_crumbContainer);
        placeholder->setFont(generalUiFont());
        placeholder->setStyleSheet(QStringLiteral("color: %1;").arg(mutedTextCss));
        m_crumbLayout->addWidget(placeholder);
        return;
    }

    for (int i = 0; i < segments.size(); ++i) {
        const QString targetPath = segments.at(i).path;
        const bool isInsideScanTree = pathIsWithinRoot(targetPath, m_scanRootPath);
        auto* button = new QToolButton(m_crumbContainer);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setText(segments.at(i).label);
        button->setCursor(Qt::PointingHandCursor);
        button->setFont(generalUiFont());
        if (!isInsideScanTree) {
            button->setToolTip(tr("Rescan from here"));
        }
        button->setStyleSheet(isInsideScanTree
            ? QStringLiteral(
                "QToolButton {"
                "  color: palette(button-text);"
                "  background: transparent;"
                "  border: 1px solid transparent;"
                "  border-radius: 4px;"
                "  padding: 0px 2px 2px 2px;"
                "}"
                "QToolButton:hover {"
                "  border-color: palette(highlight);"
                "  background: transparent;"
                "}"
                "QToolButton:pressed {"
                "  border-color: palette(highlight);"
                "  background: palette(alternate-base);"
                "}")
            : QStringLiteral(
                "QToolButton {"
                "  color: %1;"
                "  background: transparent;"
                "  border: 1px solid transparent;"
                "  border-radius: 4px;"
                "  padding: 0px 2px 2px 2px;"
                "}"
                "QToolButton:hover {"
                "  color: palette(button-text);"
                "  border-color: palette(highlight);"
                "  background: transparent;"
                "}"
                "QToolButton:pressed {"
                "  color: palette(button-text);"
                "  border-color: palette(highlight);"
                "  background: palette(alternate-base);"
                "}").arg(mutedTextCss));
        connect(button, &QToolButton::clicked, this, [this, targetPath, isInsideScanTree]() {
            emit pathActivated(targetPath, !isInsideScanTree);
        });
        m_crumbLayout->addWidget(button);

        if (i + 1 < segments.size()) {
            auto* separator = new QToolButton(m_crumbContainer);
            separator->setAutoRaise(true);
            separator->setToolButtonStyle(Qt::ToolButtonTextOnly);
            //: Path separator in breadcrumb navigation bar; use ‹ for RTL locales
            separator->setText(tr("›"));
            separator->setCursor(Qt::PointingHandCursor);
            separator->setPopupMode(QToolButton::InstantPopup);
            separator->setProperty("menuOpen", false);
            separator->setFont(generalUiFont());
            separator->setStyleSheet(QStringLiteral(
                "QToolButton {"
                "  color: palette(window-text);"
                "  border: 1px solid transparent;"
                "  border-radius: 3px;"
                "  padding: 0px 0px 2px 0px;"
                "}"
                "QToolButton:hover {"
                "  border-color: palette(highlight);"
                "  background: transparent;"
                "}"
                "QToolButton[menuOpen=\"true\"] {"
                "  border-color: palette(highlight);"
                "  background: palette(alternate-base);"
                "}"
                "QToolButton:pressed {"
                "  border-color: palette(highlight);"
                "  background: palette(alternate-base);"
                "}"
                "QToolButton::menu-indicator { image: none; width: 0; }"));
            QMenu* menu = createChildMenu(targetPath);
            separator->setMenu(menu);
            const auto syncSeparatorState = [separator]() {
                separator->style()->unpolish(separator);
                separator->style()->polish(separator);
                separator->update();
            };
            connect(menu, &QMenu::aboutToShow, separator, [separator, syncSeparatorState]() {
                separator->setProperty("menuOpen", true);
                syncSeparatorState();
            });
            connect(menu, &QMenu::aboutToHide, separator, [separator, syncSeparatorState]() {
                separator->setProperty("menuOpen", false);
                syncSeparatorState();
            });
            m_crumbLayout->addWidget(separator);
        }
    }
    m_crumbLayout->addStretch(1);
}

QMenu* BreadcrumbPathBar::createChildMenu(const QString& parentPath) const
{
    auto* self = const_cast<BreadcrumbPathBar*>(this);
    auto* menu = new QMenu(self);
    menu->setFont(generalUiFont());
    const QFileInfo parentInfo(parentPath);
    if (!parentInfo.exists() || !parentInfo.isDir()) {
        return menu;
    }

    const QDir dir(parentInfo.absoluteFilePath());
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& entry : entries) {
        const QString childPath = entry.absoluteFilePath();
        const bool isInsideScanTree = pathIsWithinRoot(childPath, m_scanRootPath);
        QAction* action = menu->addAction(entry.fileName());
        action->setData(childPath);
        QObject::connect(action, &QAction::triggered, self,
                         [self, childPath, isInsideScanTree]() {
            emit self->pathActivated(childPath, !isInsideScanTree);
        });
    }

    if (menu->isEmpty()) {
        QAction* emptyAction = menu->addAction(tr("No subfolders"));
        emptyAction->setEnabled(false);
    }

    return menu;
}

void BreadcrumbPathBar::enterEditMode()
{
    qApp->installEventFilter(this);
    m_editButton->hide();
    m_modeLayout->setCurrentWidget(m_lineEdit);
    m_lineEdit->setText(QDir::toNativeSeparators(m_path));
    m_lineEdit->selectAll();
    m_lineEdit->setFocus();
}

void BreadcrumbPathBar::exitEditMode()
{
    qApp->removeEventFilter(this);
    m_lineEdit->setText(QDir::toNativeSeparators(m_path));
    m_modeLayout->setCurrentWidget(m_breadcrumbView);
    m_editButton->show();
}

void BreadcrumbPathBar::activateCurrentEditorPath()
{
    const QString enteredPath = m_lineEdit->text().trimmed();
    if (!enteredPath.isEmpty()) {
        emit pathActivated(enteredPath, false);
    }
}

QString BreadcrumbPathBar::normalizedPath(const QString& path)
{
    return normalizedFilesystemPath(path);
}

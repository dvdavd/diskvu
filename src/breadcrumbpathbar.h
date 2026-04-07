// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QColor>
#include <QLineEdit>
#include <QStringList>
#include <QWidget>

class QCompleter;
class QFrame;
class QHBoxLayout;
class QLineEdit;
class QMenu;
class QMouseEvent;
class QStackedLayout;
class QStringListModel;
class QToolButton;

class BreadcrumbPathBar : public QWidget {
    Q_OBJECT

public:
    explicit BreadcrumbPathBar(QWidget* parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    void setPath(const QString& path);
    void setScanRootPath(const QString& path);
    QString path() const;
    void setPlaceholderText(const QString& text);
    void setRecentPaths(const QStringList& recentPaths);
    void setChromeBorderColor(const QColor& color);
    QLineEdit* lineEdit() const { return m_lineEdit; }
    QStringListModel* completionModel() const { return m_completionModel; }

signals:
    void pathActivated(const QString& path, bool forceScan);

protected:
    void changeEvent(QEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refreshChromeStyles();
    void rebuildCrumbs();
    QMenu* createChildMenu(const QString& parentPath) const;
    void enterEditMode();
    void exitEditMode();
    void activateCurrentEditorPath();
    static QString normalizedPath(const QString& path);

    QString m_path;
    QString m_scanRootPath;
    QStringList m_recentPaths;
    QColor m_chromeBorderColor;
    QWidget* m_fieldFrame = nullptr;
    QWidget* m_breadcrumbView = nullptr;
    QWidget* m_crumbContainer = nullptr;
    QHBoxLayout* m_crumbLayout = nullptr;
    QStackedLayout* m_modeLayout = nullptr;
    QLineEdit* m_lineEdit = nullptr;
    QStringListModel* m_completionModel = nullptr;
    QCompleter* m_completer = nullptr;
    QToolButton* m_editButton = nullptr;
};

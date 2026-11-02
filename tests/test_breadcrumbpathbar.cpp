// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "breadcrumbpathbar.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QtTest/QtTest>

class TestBreadcrumbPathBar : public QObject {
    Q_OBJECT

private slots:
    void returnPressed_emitsForExistingDirectory()
    {
        BreadcrumbPathBar bar;
        bar.show();
        QApplication::processEvents();

        QToolButton* editButton = nullptr;
        for (auto* button : bar.findChildren<QToolButton*>()) {
            if (button->objectName() != QStringLiteral("scanTypeBadge")) {
                editButton = button;
            }
        }
        QVERIFY(editButton != nullptr);

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        QSignalSpy activatedSpy(&bar, &BreadcrumbPathBar::pathActivated);

        QTest::mouseClick(editButton, Qt::LeftButton);
        QVERIFY(bar.lineEdit()->isVisible());
        bar.lineEdit()->setText(tempDir.path());
        QTest::keyClick(bar.lineEdit(), Qt::Key_Return);

        QCOMPARE(activatedSpy.count(), 1);
        const QList<QVariant> arguments = activatedSpy.takeFirst();
        QCOMPARE(arguments.at(0).toString(), QFileInfo(tempDir.path()).absoluteFilePath());
        QCOMPARE(arguments.at(1).toBool(), false);
    }

    void returnPressed_ignoresMissingDirectory()
    {
        BreadcrumbPathBar bar;
        bar.show();
        QApplication::processEvents();

        QToolButton* editButton = nullptr;
        for (auto* button : bar.findChildren<QToolButton*>()) {
            if (button->objectName() != QStringLiteral("scanTypeBadge")) {
                editButton = button;
            }
        }
        QVERIFY(editButton != nullptr);

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString missingPath = tempDir.path() + QStringLiteral("/missing");

        QSignalSpy activatedSpy(&bar, &BreadcrumbPathBar::pathActivated);

        QTest::mouseClick(editButton, Qt::LeftButton);
        QVERIFY(bar.lineEdit()->isVisible());
        bar.lineEdit()->setText(missingPath);
        QTest::keyClick(bar.lineEdit(), Qt::Key_Return);

        QCOMPARE(activatedSpy.count(), 0);
    }
};

QTEST_MAIN(TestBreadcrumbPathBar)
#include "test_breadcrumbpathbar.moc"

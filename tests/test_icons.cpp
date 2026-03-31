// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "mainwindow_utils.h"

#include <QApplication>
#include <QIcon>
#include <QtTest/QtTest>

class TestIcons : public QObject {
    Q_OBJECT

private slots:
    void recoloredSvgIcon_providesHighDpiPixmapVariants()
    {
        const QString svgPath = QFINDTESTDATA("../assets/tabler-icons/refresh.svg");
        QVERIFY(!svgPath.isEmpty());

        const QIcon icon = makeRecoloredSvgIcon(svgPath,
                                                QColor(QStringLiteral("#336699")));
        QVERIFY(!icon.isNull());

        const QPixmap px2x = icon.pixmap(QSize(24, 24), 2.0, QIcon::Normal, QIcon::Off);
        QVERIFY(!px2x.isNull());
        QCOMPARE(px2x.devicePixelRatio(), 2.0);
        QCOMPARE(px2x.deviceIndependentSize().toSize(), QSize(24, 24));

        const QPixmap px3x = icon.pixmap(QSize(24, 24), 3.0, QIcon::Normal, QIcon::Off);
        QVERIFY(!px3x.isNull());
        QCOMPARE(px3x.devicePixelRatio(), 3.0);
        QCOMPARE(px3x.deviceIndependentSize().toSize(), QSize(24, 24));
    }
};

QTEST_MAIN(TestIcons)
#include "test_icons.moc"

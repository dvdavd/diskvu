// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "searchfilterpanel.h"
#include "treemapsettings.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QtTest/QtTest>

class TestSearchFilterPanel : public QObject {
    Q_OBJECT

private slots:
    void setSettings_sortsFileTypeGroupsAlphabetically()
    {
        SearchFilterPanel panel;
        TreemapSettings settings;
        settings.fileTypeGroups = {
            { QStringLiteral("Zed"), QColor(Qt::red), { QStringLiteral("z") } },
            { QStringLiteral("Alpha"), QColor(Qt::green), { QStringLiteral("a") } },
            { QStringLiteral("Beta"), QColor(Qt::blue), { QStringLiteral("b") } }
        };

        panel.setSettings(settings);

        auto* combo = panel.findChild<QComboBox*>(QStringLiteral("m_typeCombo"));
        QVERIFY(combo != nullptr);

        // Index 0: "All types"
        // Index 1: "Alpha"
        // Index 2: "Beta"
        // Index 3: "Zed"
        QCOMPARE(combo->count(), 4);
        QCOMPARE(combo->itemText(0), QStringLiteral("All types"));
        QCOMPARE(combo->itemText(1), QStringLiteral("Alpha"));
        QCOMPARE(combo->itemText(2), QStringLiteral("Beta"));
        QCOMPARE(combo->itemText(3), QStringLiteral("Zed"));
    }

    void setSettings_isCaseInsensitive()
    {
        SearchFilterPanel panel;
        TreemapSettings settings;
        settings.fileTypeGroups = {
            { QStringLiteral("banana"), QColor(Qt::yellow), { QStringLiteral("b") } },
            { QStringLiteral("Apple"), QColor(Qt::red), { QStringLiteral("a") } },
            { QStringLiteral("cherry"), QColor(Qt::red), { QStringLiteral("c") } }
        };

        panel.setSettings(settings);

        auto* combo = panel.findChild<QComboBox*>(QStringLiteral("m_typeCombo"));
        QVERIFY(combo != nullptr);

        QCOMPARE(combo->count(), 4);
        QCOMPARE(combo->itemText(1), QStringLiteral("Apple"));
        QCOMPARE(combo->itemText(2), QStringLiteral("banana"));
        QCOMPARE(combo->itemText(3), QStringLiteral("cherry"));
    }

    void currentParams_normalizesSizeAndDateRanges()
    {
        SearchFilterPanel panel;
        TreemapSettings settings;
        panel.setSettings(settings);

        auto* minSpin = panel.findChild<QDoubleSpinBox*>(QStringLiteral("m_sizeMinSpin"));
        auto* maxSpin = panel.findChild<QDoubleSpinBox*>(QStringLiteral("m_sizeMaxSpin"));
        auto* minUnit = panel.findChild<QComboBox*>(QStringLiteral("m_sizeMinUnit"));
        auto* maxUnit = panel.findChild<QComboBox*>(QStringLiteral("m_sizeMaxUnit"));

        QVERIFY(minSpin && maxSpin && minUnit && maxUnit);

        // Set range in reverse: 1GB to 100MB
        minSpin->setValue(1.0);
        minUnit->setCurrentIndex(3); // GB
        maxSpin->setValue(100.0);
        maxUnit->setCurrentIndex(2); // MiB

        FilterParams p = panel.currentParams();
        QCOMPARE(p.sizeMin, 100LL * 1024 * 1024);
        QCOMPARE(p.sizeMax, 1LL * 1024 * 1024 * 1024);

        // Date range normalization
        auto* fromCheck = panel.findChild<QCheckBox*>(QStringLiteral("m_dateFromCheck"));
        auto* toCheck   = panel.findChild<QCheckBox*>(QStringLiteral("m_dateToCheck"));
        auto* fromEdit  = panel.findChild<QDateEdit*>(QStringLiteral("m_dateFromEdit"));
        auto* toEdit    = panel.findChild<QDateEdit*>(QStringLiteral("m_dateToEdit"));

        QVERIFY(fromCheck && toCheck && fromEdit && toEdit);

        fromCheck->setChecked(true);
        toCheck->setChecked(true);
        fromEdit->setDate(QDate(2026, 12, 31));
        toEdit->setDate(QDate(2026, 1, 1));

        p = panel.currentParams();
        QVERIFY(p.dateMin < p.dateMax);
    }
};

QTEST_MAIN(TestSearchFilterPanel)
#include "test_searchfilterpanel.moc"

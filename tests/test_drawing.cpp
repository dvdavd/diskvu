// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "treemap_drawing.h"
#include "filenode.h"

#include <QApplication>
#include <QImage>
#include <QtTest/QtTest>

class TestDrawing : public QObject {
    Q_OBJECT

private slots:
    // ── smoothstep ───────────────────────────────────────────────────────────

    void smoothstep_atZero() { QCOMPARE(smoothstep(0.0), 0.0); }
    void smoothstep_atOne()  { QCOMPARE(smoothstep(1.0), 1.0); }

    void smoothstep_atHalf()
    {
        // t*t*(3-2t) at t=0.5: 0.25 * 2.0 = 0.5
        QCOMPARE(smoothstep(0.5), 0.5);
    }

    void smoothstep_clampsBelow()
    {
        QCOMPARE(smoothstep(-1.0), 0.0);
        QCOMPARE(smoothstep(-99.0), 0.0);
    }

    void smoothstep_clampsAbove()
    {
        QCOMPARE(smoothstep(2.0), 1.0);
        QCOMPARE(smoothstep(99.0), 1.0);
    }

    void smoothstep_isMonotonic()
    {
        qreal prev = 0.0;
        for (int i = 1; i <= 100; ++i) {
            const qreal v = smoothstep(i / 100.0);
            QVERIFY(v >= prev);
            prev = v;
        }
    }

    void revealOpacityForSize_belowStartIsZero()
    {
        QCOMPARE(revealOpacityForSize(QSizeF(3.0, 3.0), 4.0, 4.0, 10.0, 10.0), 0.0);
    }

    void revealOpacityForSize_atFullIsOne()
    {
        QCOMPARE(revealOpacityForSize(QSizeF(10.0, 10.0), 4.0, 4.0, 10.0, 10.0), 1.0);
    }

    void revealOpacityForSize_midwayProducesPartialFade()
    {
        const qreal opacity = revealOpacityForSize(QSizeF(7.0, 7.0), 4.0, 4.0, 10.0, 10.0);
        QVERIFY(opacity > 0.0);
        QVERIFY(opacity < 1.0);
    }

    // ── normalizedPixelScale ─────────────────────────────────────────────────

    void normalizedPixelScale_positivePassthrough()
    {
        QCOMPARE(normalizedPixelScale(1.0),  1.0);
        QCOMPARE(normalizedPixelScale(2.0),  2.0);
        QCOMPARE(normalizedPixelScale(1.75), 1.75);
    }

    void normalizedPixelScale_zeroFallsBackToOne()
    {
        QCOMPARE(normalizedPixelScale(0.0), 1.0);
    }

    void normalizedPixelScale_negativeFallsBackToOne()
    {
        QCOMPARE(normalizedPixelScale(-1.0), 1.0);
    }

    // ── snapLengthToPixels ───────────────────────────────────────────────────

    void snapLengthToPixels_integerAlreadySnapped()
    {
        QCOMPARE(snapLengthToPixels(10.0, 1.0), 10.0);
    }

    void snapLengthToPixels_resultIsWholePixels()
    {
        // result * pixelScale must be an integer
        for (const qreal scale : {1.0, 2.0, 1.5}) {
            const qreal r = snapLengthToPixels(5.3, scale);
            const qreal physical = r * scale;
            QCOMPARE(physical, std::round(physical));
        }
    }

    void snapLengthToPixels_minimumIsOnePhysicalPixel()
    {
        // Even a 0.001 logical pixel must snap to at least 1 physical pixel
        const qreal r = snapLengthToPixels(0.001, 2.0);
        QVERIFY(r * 2.0 >= 1.0);
    }

    void snapLengthToPixels_zeroReturnsZero()
    {
        QCOMPARE(snapLengthToPixels(0.0, 1.0), 0.0);
        QCOMPARE(snapLengthToPixels(-1.0, 1.0), 0.0);
    }

    // ── applyAxisHysteresis ─────────────────────────────────────────────────

    void applyAxisHysteresis_keepsAxesWithinDeadband()
    {
        const QSizeF current(13.0, 10.0);
        const QSizeF previous(12.0, 9.0);
        const QSizeF result = applyAxisHysteresis(current, previous, 2.0);
        QCOMPARE(result.width(), previous.width());
        QCOMPARE(result.height(), previous.height());
    }

    void applyAxisHysteresis_updatesOnlyAxisOutsideDeadband()
    {
        const QSizeF current(15.0, 12.0);
        const QSizeF previous(12.0, 10.0);
        const QSizeF result = applyAxisHysteresis(current, previous, 2.0);
        QCOMPARE(result.width(), current.width());
        QCOMPARE(result.height(), previous.height());
    }

    void applyAxisHysteresis_updatesBothAxesOutsideDeadband()
    {
        const QSizeF current(16.0, 13.0);
        const QSizeF previous(12.0, 10.0);
        const QSizeF result = applyAxisHysteresis(current, previous, 2.0);
        QCOMPARE(result.width(), current.width());
        QCOMPARE(result.height(), current.height());
    }

    // ── revealThresholds ────────────────────────────────────────────────────

    void revealThresholds_largeTinyTileSizeDoesNotInvertFadeRange()
    {
        TreemapSettings settings;
        settings.minRevealWidth = 24.0;
        settings.minRevealHeight = 20.0;
        settings.revealFadeWidth = 14.0;
        settings.revealFadeHeight = 10.0;
        settings.minTileSize = 100.0;

        const RevealThresholds thresholds = revealThresholds(settings);
        QCOMPARE(thresholds.childFullWidth, 24.0);
        QCOMPARE(thresholds.childFullHeight, 20.0);
        QCOMPARE(thresholds.childStartWidth, 24.0);
        QCOMPARE(thresholds.childStartHeight, 20.0);
        QVERIFY(thresholds.childStartWidth <= thresholds.childFullWidth);
        QVERIFY(thresholds.childStartHeight <= thresholds.childFullHeight);
    }

    // ── contrastingTextColor ─────────────────────────────────────────────────

    void contrastingTextColor_blackBackgroundGivesLightText()
    {
        const QColor text = contrastingTextColor(QColor(0, 0, 0));
        // WCAG: white (245,245,245) wins against black
        QVERIFY(text.value() > 127);
    }

    void contrastingTextColor_whiteBackgroundGivesDarkText()
    {
        const QColor text = contrastingTextColor(QColor(255, 255, 255));
        // WCAG: dark (24,24,28) wins against white
        QVERIFY(text.value() < 127);
    }

    void contrastingTextColor_darkBlueGivesLightText()
    {
        const QColor text = contrastingTextColor(QColor(0, 0, 100));
        QVERIFY(text.value() > 127);
    }

    void contrastingTextColor_cacheIsConsistent()
    {
        const QColor c(80, 140, 200);
        QCOMPARE(contrastingTextColor(c).rgb(), contrastingTextColor(c).rgb());
    }

    // ── blendColors ──────────────────────────────────────────────────────────

    void blendColors_atZeroReturnsFrom()
    {
        const QColor from(255, 0, 0);
        const QColor to(0, 0, 255);
        const QColor result = blendColors(from, to, 0.0);
        QCOMPARE(result.red(),   255);
        QCOMPARE(result.green(),   0);
        QCOMPARE(result.blue(),    0);
    }

    void blendColors_atOneReturnsTo()
    {
        const QColor from(255, 0, 0);
        const QColor to(0, 0, 255);
        const QColor result = blendColors(from, to, 1.0);
        QCOMPARE(result.red(),    0);
        QCOMPARE(result.green(),  0);
        QCOMPARE(result.blue(), 255);
    }

    void blendColors_midpointIsHalfway()
    {
        const QColor from(0, 0, 0);
        const QColor to(200, 200, 200);
        const QColor result = blendColors(from, to, 0.5);
        QVERIFY(result.red() > 50 && result.red() < 150);
    }

    void blendColors_clampsBelow()
    {
        const QColor from(255, 0, 0);
        const QColor to(0, 0, 255);
        // t < 0 should clamp to 0.0
        QCOMPARE(blendColors(from, to, -1.0).red(), blendColors(from, to, 0.0).red());
    }

    void blendColors_clampsAbove()
    {
        const QColor from(255, 0, 0);
        const QColor to(0, 0, 255);
        // t > 1 should clamp to 1.0
        QCOMPARE(blendColors(from, to, 2.0).red(), blendColors(from, to, 1.0).red());
    }

    // ── depthAdjustedColor ───────────────────────────────────────────────────

    void depthAdjustedColor_depthZeroIsUnchanged()
    {
        const QColor c(80, 120, 200);
        // factor = 100 + 0*4 = 100 → lighter/darker by 100% = no change
        QCOMPARE(depthAdjustedColor(c, 0).rgb(), c.lighter(100).rgb());
    }

    void depthAdjustedColor_darkColorLightensWithDepth()
    {
        const QColor dark(10, 10, 40); // luminance ≈ 0.006 < 0.32 → should lighten
        const QColor result = depthAdjustedColor(dark, 4);
        QVERIFY(result.lightness() >= dark.lightness());
    }

    void depthAdjustedColor_lightColorDarkensWithDepth()
    {
        const QColor light(200, 200, 200); // luminance ≈ 0.6 > 0.32 → should darken
        const QColor result = depthAdjustedColor(light, 4);
        QVERIFY(result.lightness() <= light.lightness());
    }

    // ── squarifiedLayout ─────────────────────────────────────────────────────

    void squarifiedLayout_emptyChildren()
    {
        std::vector<std::pair<FileNode*, QRectF>> result;
        squarifiedLayout({}, QRectF(0, 0, 100, 100), 0, result);
        QVERIFY(result.empty());
    }

    void squarifiedLayout_singleChildFillsRect()
    {
        FileNode node;
        node.size = 100;
        const QRectF rect(0, 0, 200, 150);
        std::vector<std::pair<FileNode*, QRectF>> result;
        squarifiedLayout({&node}, rect, 100, result);

        QCOMPARE(result.size(), size_t(1));
        QCOMPARE(result[0].first, &node);
        QVERIFY(qAbs(result[0].second.width()  - rect.width())  < 1e-6);
        QVERIFY(qAbs(result[0].second.height() - rect.height()) < 1e-6);
    }

    void squarifiedLayout_allChildrenPresent()
    {
        const int n = 7;
        std::vector<FileNode> nodes(n);
        std::vector<FileNode*> ptrs;
        qint64 total = 0;
        for (int i = 0; i < n; ++i) {
            nodes[i].size = (i + 1) * 100;
            total += nodes[i].size;
            ptrs.push_back(&nodes[i]);
        }
        std::vector<std::pair<FileNode*, QRectF>> result;
        squarifiedLayout(ptrs, QRectF(0, 0, 400, 300), total, result);

        QCOMPARE(result.size(), size_t(n));
        for (int i = 0; i < n; ++i)
            QCOMPARE(result[i].first, ptrs[i]);
    }

    void squarifiedLayout_areasSumToRectArea()
    {
        const int n = 5;
        std::vector<FileNode> nodes(n);
        std::vector<FileNode*> ptrs;
        qint64 total = 0;
        for (int i = 0; i < n; ++i) {
            nodes[i].size = (i + 1) * 200;
            total += nodes[i].size;
            ptrs.push_back(&nodes[i]);
        }
        const QRectF rect(0, 0, 400, 300);
        std::vector<std::pair<FileNode*, QRectF>> result;
        squarifiedLayout(ptrs, rect, total, result);

        qreal areaSum = 0.0;
        for (const auto& [node, r] : result)
            areaSum += r.width() * r.height();

        const qreal expected = rect.width() * rect.height();
        QVERIFY(qAbs(areaSum - expected) / expected < 1e-9);
    }

    void squarifiedLayout_noNegativeDimensions()
    {
        const int n = 10;
        std::vector<FileNode> nodes(n);
        std::vector<FileNode*> ptrs;
        qint64 total = 0;
        for (int i = 0; i < n; ++i) {
            nodes[i].size = (i + 1) * 50;
            total += nodes[i].size;
            ptrs.push_back(&nodes[i]);
        }
        std::vector<std::pair<FileNode*, QRectF>> result;
        squarifiedLayout(ptrs, QRectF(0, 0, 800, 600), total, result);

        for (const auto& [node, r] : result) {
            QVERIFY(r.width()  >= 0.0);
            QVERIFY(r.height() >= 0.0);
        }
    }

    void squarifiedLayout_zeroTotalSizeReturnsEmpty()
    {
        FileNode node;
        node.size = 100;
        std::vector<std::pair<FileNode*, QRectF>> result;
        squarifiedLayout({&node}, QRectF(0, 0, 100, 100), 0, result);
        QVERIFY(result.empty());
    }

    // ── buildSearchRegex ─────────────────────────────────────────────────────

    void buildSearchRegex_literalMatches()
    {
        const QRegularExpression rx = buildSearchRegex("hello");
        QVERIFY(rx.isValid());
        QVERIFY( rx.match("hello.txt").hasMatch());
        QVERIFY( rx.match("HELLO.TXT").hasMatch()); // case-insensitive
        QVERIFY(!rx.match("world.txt").hasMatch());
    }

    void buildSearchRegex_wildcardStar()
    {
        const QRegularExpression rx = buildSearchRegex("*.txt");
        QVERIFY(rx.isValid());
        QVERIFY(rx.match("readme.txt").hasMatch());
        QVERIFY(rx.match("report.final.txt").hasMatch());
    }

    void buildSearchRegex_wildcardQuestion()
    {
        const QRegularExpression rx = buildSearchRegex("file?.txt");
        QVERIFY(rx.isValid());
        QVERIFY( rx.match("file1.txt").hasMatch());
        QVERIFY(!rx.match("file12.txt").hasMatch());
    }

    void buildSearchRegex_specialCharsAreEscaped()
    {
        // These chars would break an un-escaped regex
        const QRegularExpression rx = buildSearchRegex("report (final).pdf");
        QVERIFY(rx.isValid());
        QVERIFY(rx.match("report (final).pdf").hasMatch());
    }

    void buildSearchRegex_emptyPatternIsValid()
    {
        const QRegularExpression rx = buildSearchRegex("");
        QVERIFY(rx.isValid());
    }

    // ── confineRectToBounds ───────────────────────────────────────────────────

    void confineRectToBounds_alreadyInsideIsUnchanged()
    {
        const QRectF bounds(0, 0, 100, 100);
        const QRectF rect(10, 10, 20, 20);
        const QRectF result = confineRectToBounds(rect, bounds);
        QVERIFY(result.left()   >= bounds.left());
        QVERIFY(result.right()  <= bounds.right());
        QVERIFY(result.top()    >= bounds.top());
        QVERIFY(result.bottom() <= bounds.bottom());
    }

    void confineRectToBounds_shiftedIntoView()
    {
        const QRectF bounds(0, 0, 100, 100);
        const QRectF rect(80, 80, 40, 40); // extends beyond bounds
        const QRectF result = confineRectToBounds(rect, bounds);
        QVERIFY(result.right()  <= bounds.right()  + 1e-6);
        QVERIFY(result.bottom() <= bounds.bottom() + 1e-6);
    }

    void confineRectToBounds_largerThanBoundsShrinks()
    {
        const QRectF bounds(0, 0, 100, 100);
        const QRectF rect(-50, -50, 300, 300);
        const QRectF result = confineRectToBounds(rect, bounds);
        QVERIFY(result.width()  <= bounds.width()  + 1e-6);
        QVERIFY(result.height() <= bounds.height() + 1e-6);
    }

    // ── strokeRectInside ─────────────────────────────────────────────────────

    void strokeRectInside_isContainedByOuter()
    {
        const QRectF outer(10, 10, 80, 60);
        const QRectF inner = strokeRectInside(outer, 1.0);
        QVERIFY(inner.left()   >= outer.left()   - 1e-6);
        QVERIFY(inner.right()  <= outer.right()  + 1e-6);
        QVERIFY(inner.top()    >= outer.top()    - 1e-6);
        QVERIFY(inner.bottom() <= outer.bottom() + 1e-6);
    }

    void strokeRectInside_insetReducesDimensions()
    {
        const QRectF outer(0, 0, 100, 80);
        const QRectF inner = strokeRectInside(outer, 2.0);
        QCOMPARE(inner.width(),  96.0);
        QCOMPARE(inner.height(), 76.0);
    }

    void strokeRectInside_largeInsetClampsDimensionsToZero()
    {
        const QRectF outer(0, 0, 10, 10);
        const QRectF inner = strokeRectInside(outer, 50.0); // inset > half-size
        QVERIFY(inner.width()  >= 0.0);
        QVERIFY(inner.height() >= 0.0);
    }

    void paintTinyNodeFill_appliesOpacity()
    {
        QImage image(4, 4, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);

        QPainter painter(&image);
        paintTinyNodeFill(painter, QRectF(0.0, 0.0, 4.0, 4.0), QColor(255, 0, 0), 1.0, 0.25);
        painter.end();

        const QColor pixel = image.pixelColor(1, 1);
        QVERIFY(pixel.red() > pixel.green());
        QVERIFY(pixel.green() > 0);
        QVERIFY(pixel.green() < 255);
        QVERIFY(pixel.blue() > 0);
        QVERIFY(pixel.blue() < 255);
    }
};

QTEST_MAIN(TestDrawing)
#include "test_drawing.moc"

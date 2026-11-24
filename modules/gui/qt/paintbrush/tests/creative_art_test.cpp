/*****************************************************************************
 * creative_art_test.cpp : standalone harness for pb_creative_art.hpp
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

/* pb_creative_art.hpp is deliberately VLC-free: this test compiles with Qt
 * Core/Gui only and verifies the contracts the Creative Pad patches rely on:
 *   - deterministic output (same seed -> byte-identical pixels),
 *   - alpha discipline (nothing paints outside the subject silhouette),
 *   - luminance preservation in the selective recolour,
 *   - every scene helper actually produces pixels. */

#include "pb_creative_art.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QString>

#include <cmath>
#include <cstdio>

using namespace paintbrush;

namespace {

int failures = 0;

void check(const char *name, bool ok)
{
    std::printf("ASSERT.%s|%s\n", name, ok ? "PASS" : "FAIL");
    if (!ok)
        ++failures;
}

quint64 hashImage(const QImage &image)
{
    if (image.isNull())
        return 0;
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    quint64 h = 1469598103934665603ull;
    for (int y = 0; y < src.height(); ++y)
    {
        const uchar *line = src.constScanLine(y);
        const int bytes = src.width() * 4;
        for (int i = 0; i < bytes; ++i)
        {
            h ^= quint64(line[i]);
            h *= 1099511628211ull;
        }
    }
    return h;
}

bool hasAlpha(const QImage &image)
{
    if (image.isNull())
        return false;
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x)
            if (qAlpha(line[x]) > 0)
                return true;
    }
    return false;
}

/* A synthetic cutout: a soft-edged ellipse with a hue ramp and a bright band
 * on the right, which is what a garment with a fold/highlight looks like to
 * the recolour. */
QImage subjectImage(int w = 220, int h = 300)
{
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    const qreal cx = w * 0.5, cy = h * 0.5;
    const qreal rx = w * 0.42, ry = h * 0.46;
    for (int y = 0; y < h; ++y)
    {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x)
        {
            const qreal nx = (x - cx) / rx;
            const qreal ny = (y - cy) / ry;
            const qreal d = nx * nx + ny * ny;
            if (d > 1.0)
                continue;
            const int hue = (x * 300) / w;
            const int value = x > w * 0.72 ? 245 : 110 + (y * 80) / h;
            const QColor c = QColor::fromHsv(hue % 360, 165, value);
            const qreal edge = std::min(1.0, (1.0 - d) * 14.0);
            line[x] = qRgba(c.red(), c.green(), c.blue(),
                            int(edge * 255.0 + 0.5));
        }
    }
    return image;
}

QImage flatPatch(const QColor &color, int size = 64)
{
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}

QImage fullMask(const QSize &size)
{
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(255);
    return mask;
}

void testMasks()
{
    const QImage subject = subjectImage();
    const QImage mask = creative::maskFromAlpha(subject);
    check("mask.size", mask.size() == subject.size());
    check("mask.center", mask.constScanLine(150)[110] > 200);
    check("mask.corner", mask.constScanLine(0)[0] == 0);

    QPoint offset;
    const QImage trimmed = creative::trimTransparent(subject, &offset);
    check("trim.not-null", !trimmed.isNull());
    check("trim.smaller", trimmed.width() < subject.width());
    check("trim.offset-matches", offset.x() > 0 && offset.y() > 0);

    const QImage cut = creative::maskToCutout(flatPatch(QColor(200, 40, 90)),
                                              fullMask(QSize(64, 64)));
    check("cutout.keeps-alpha", qAlpha(cut.pixel(10, 10)) == 255);

    QImage halfMask = fullMask(QSize(64, 64));
    for (int x = 32; x < 64; ++x)
        halfMask.scanLine(10)[x] = 0;
    const QImage halfCut = creative::maskToCutout(flatPatch(QColor(200, 40, 90)),
                                                  halfMask);
    check("cutout.masks", qAlpha(halfCut.pixel(10, 10)) == 255
                              && qAlpha(halfCut.pixel(40, 10)) == 0);

    const QImage band = creative::bandFromMask(mask, 8);
    check("band.size", band.size() == mask.size());
    check("band.empty-inside", band.constScanLine(150)[110] == 0);
    check("band.ring", band.constScanLine(150)[16] > 0
                           || band.constScanLine(150)[204] > 0);
    check("band.deterministic",
          hashImage(creative::bandFromMask(mask, 8)) == hashImage(band));

    const QImage inner = creative::innerBand(mask, 6);
    check("inner.empty-center", inner.constScanLine(150)[110] == 0);
    check("inner.edge", inner.constScanLine(150)[22] > 0);
}

void testRecolor()
{
    const QImage subject = subjectImage();
    const QImage mask = creative::maskFromAlpha(subject);

    creative::RecolorParams params;
    params.palette = { QColor(0, 234, 255), QColor(253, 48, 199),
                       QColor(183, 255, 24) };
    params.strength = 0.9f;
    params.textureKeep = 0.2f;
    params.skinGuard = 0.0f;
    params.seed = 1234;

    const QImage a = creative::recolorMasked(subject, mask, params);
    const QImage b = creative::recolorMasked(subject, mask, params);
    check("recolor.size", a.size() == subject.size());
    check("recolor.deterministic", hashImage(a) == hashImage(b));

    /* untouched outside the silhouette */
    check("recolor.outside-transparent", qAlpha(a.pixel(0, 0)) == 0);
    check("recolor.outside-identical",
          a.pixel(0, 0) == subject.pixel(0, 0));

    int changed = 0;
    int checked = 0;
    float worstL = 0.0f;
    for (int y = 80; y < 220; y += 7)
    {
        for (int x = 40; x < 180; x += 7)
        {
            const QRgb before = subject.pixel(x, y);
            const QRgb after = a.pixel(x, y);
            if (qAlpha(before) < 200)
                continue;
            ++checked;
            const QColor cb = QColor::fromRgba(before);
            const QColor ca = QColor::fromRgba(after);
            const float dL = std::fabs(creative::oklabLightness(cb)
                                       - creative::oklabLightness(ca));
            if (dL > worstL)
                worstL = dL;
            if (qRed(before) != qRed(after) || qGreen(before) != qGreen(after)
                || qBlue(before) != qBlue(after))
                ++changed;
        }
    }
    check("recolor.sampled", checked > 200);
    check("recolor.changes-hue", changed > checked / 2);
    std::printf("METRIC.recolor.worst-lightness-delta|%.4f\n", double(worstL));
    check("recolor.preserves-lightness", worstL < 0.035f);

    /* the skin guard dims the effect on a skin-coloured patch */
    const QImage skin = flatPatch(QColor(198, 134, 66), 96);
    const QImage skinMask = fullMask(skin.size());
    creative::RecolorParams noGuard = params;
    noGuard.skinGuard = 0.0f;
    noGuard.palette = { QColor(0, 234, 255), QColor(253, 48, 199) };
    creative::RecolorParams guarded = noGuard;
    guarded.skinGuard = 1.0f;

    const QImage aSkin = creative::recolorMasked(skin, skinMask, noGuard);
    const QImage bSkin = creative::recolorMasked(skin, skinMask, guarded);
    long long diffA = 0, diffB = 0;
    for (int y = 0; y < skin.height(); ++y)
    {
        for (int x = 0; x < skin.width(); ++x)
        {
            const QRgb s = skin.pixel(x, y);
            const QRgb ra = aSkin.pixel(x, y);
            const QRgb rb = bSkin.pixel(x, y);
            diffA += std::abs(qRed(s) - qRed(ra))
                     + std::abs(qGreen(s) - qGreen(ra))
                     + std::abs(qBlue(s) - qBlue(ra));
            diffB += std::abs(qRed(s) - qRed(rb))
                     + std::abs(qGreen(s) - qGreen(rb))
                     + std::abs(qBlue(s) - qBlue(rb));
        }
    }
    check("recolor.skin-guard", diffB < diffA);
    check("recolor.alpha-preserved",
          qAlpha(aSkin.pixel(5, 5)) == qAlpha(skin.pixel(5, 5)));
}

void testSheenAndLight()
{
    const QImage subject = subjectImage();
    const QImage mask = creative::maskFromAlpha(subject);
    const QImage sheen = creative::spectralSheen(
        subject, mask, { QColor(0, 229, 255), QColor(255, 61, 218) }, 1.0f,
        -28.0f, 0.4f);
    check("sheen.size", sheen.size() == subject.size());
    check("sheen.deterministic",
          hashImage(sheen)
              == hashImage(creative::spectralSheen(
                     subject, mask,
                     { QColor(0, 229, 255), QColor(255, 61, 218) }, 1.0f,
                     -28.0f, 0.4f)));
    check("sheen.dark-pixel-empty", qAlpha(sheen.pixel(6, 6)) == 0);

    const QImage rim = creative::rimLight(mask, 3, 14, QColor(0, 229, 255),
                                          QColor(255, 61, 218), 0.9f);
    check("rim.size", rim.size() == mask.size());
    int outside = 0;
    for (int y = 0; y < rim.height(); ++y)
        for (int x = 0; x < rim.width(); ++x)
            if (mask.constScanLine(y)[x] == 0 && qAlpha(rim.pixel(x, y)) > 0)
                ++outside;
    check("rim.spills-outside", outside > 100);

    const QImage glow = creative::glowField(mask, 12, QColor(124, 77, 255),
                                            0.8f);
    check("glow.has-pixels", hasAlpha(glow));

    const QImage gloss = creative::glossOverlay(QSize(220, 300), subject,
                                                QRectF(0, 0, 220, 300),
                                                QColor(255, 255, 255));
    check("gloss.clipped", qAlpha(gloss.pixel(0, 0)) == 0
                               && hasAlpha(gloss));
}

void testElectricity()
{
    creative::ArcParams params;
    params.halo = QColor(0, 234, 255);
    params.core = QColor(255, 255, 255);
    const QImage arc = creative::electricArc(QSize(320, 240), QPointF(4, 30),
                                             QPointF(300, 190), 777, params);
    check("arc.size", arc.size() == QSize(320, 240));
    check("arc.has-pixels", hasAlpha(arc));
    check("arc.deterministic",
          hashImage(arc)
              == hashImage(creative::electricArc(QSize(320, 240),
                                                 QPointF(4, 30),
                                                 QPointF(300, 190), 777,
                                                 params)));
    check("arc.seed-matters",
          hashImage(arc)
              != hashImage(creative::electricArc(QSize(320, 240),
                                                 QPointF(4, 30),
                                                 QPointF(300, 190), 778,
                                                 params)));

    const QImage subject = subjectImage();
    const QImage mask = creative::maskFromAlpha(subject);
    const QImage sparks = creative::sparkField(
        mask, subject.size(), 42,
        { QColor(0, 234, 255), QColor(253, 48, 199) }, 80, 0.9f);
    check("sparks.has-pixels", hasAlpha(sparks));
    check("sparks.deterministic",
          hashImage(sparks)
              == hashImage(creative::sparkField(
                     mask, subject.size(), 42,
                     { QColor(0, 234, 255), QColor(253, 48, 199) }, 80, 0.9f)));
}

void testColorOps()
{
    const QImage subject = subjectImage();
    const QImage mask = creative::maskFromAlpha(subject);

    creative::Grade grade;
    grade.brightness = 1.05f;
    grade.contrast = 1.2f;
    grade.saturation = 0.6f;
    grade.tint = QColor(40, 18, 90);
    grade.tintAmount = 0.3f;
    const QImage graded = creative::gradeImage(subject, grade);
    check("grade.size", graded.size() == subject.size());
    check("grade.alpha", qAlpha(graded.pixel(110, 150)) == 255);
    check("grade.deterministic",
          hashImage(graded) == hashImage(creative::gradeImage(subject, grade)));

    const QImage shifted = creative::hueShift(subject, 90);
    check("hue.size", shifted.size() == subject.size());
    check("hue.alpha", qAlpha(shifted.pixel(110, 150)) == 255);
    const QColor before = QColor::fromRgba(subject.pixel(110, 150));
    const QColor after = QColor::fromRgba(shifted.pixel(110, 150));
    check("hue.actually-shifts", before.hsvHue() != after.hsvHue());

    const QImage mirror = creative::reflection(subject, 0.5, 0.2);
    check("mirror.size", mirror.size() == subject.size());
    check("mirror.fades", qAlpha(mirror.pixel(110, 299)) < 40);

    check("oklab.black", creative::oklabLightness(QColor(0, 0, 0)) < 0.02f);
    check("oklab.white", creative::oklabLightness(QColor(255, 255, 255)) > 0.98f);
}

void testSceneHelpers()
{
    const QSize stage(320, 240);
    const QImage backdrop = creative::backdrop(
        stage, 7, { QColor(20, 4, 36), QColor(255, 47, 185),
                    QColor(0, 234, 255) },
        0.95f);
    check("backdrop.size", backdrop.size() == stage);
    check("backdrop.opaque", qAlpha(backdrop.pixel(2, 2)) == 255);
    check("backdrop.deterministic",
          hashImage(backdrop)
              == hashImage(creative::backdrop(
                     stage, 7,
                     { QColor(20, 4, 36), QColor(255, 47, 185),
                       QColor(0, 234, 255) },
                     0.95f)));

    const QImage gradient = creative::gradientField(
        stage, QColor(4, 16, 26), QColor(4, 3, 10), QColor(40, 16, 80), 0.55);
    check("gradient.size", gradient.size() == stage);

    const QImage halftone = creative::halftoneField(
        stage, QColor(255, 110, 190, 85), 9.0, 2.4, -18.0, 0.0);
    check("halftone.has-pixels", hasAlpha(halftone));

    const QImage bokeh = creative::bokehField(
        stage, 3, { QColor(255, 240, 200), QColor(0, 234, 255) }, 40, 1.5, 7.0);
    check("bokeh.has-pixels", hasAlpha(bokeh));

    const QImage grain = creative::grainField(stage, 5, 30);
    check("grain.size", grain.size() == stage);
    check("grain.has-pixels", hasAlpha(grain));

    const QImage scan = creative::scanlineField(stage, QColor(8, 8, 20), 4,
                                                0.10);
    check("scan.has-pixels", hasAlpha(scan));

    const QImage ink = creative::inkSplatter(
        stage, 9, QRectF(40, 30, 220, 170), QColor(20, 18, 16), 30);
    check("ink.has-pixels", hasAlpha(ink));

    const QImage spiral = creative::spiralField(
        stage, QPointF(160, 120), 90, 11,
        { QColor(255, 211, 122), QColor(255, 122, 200) }, 120);
    check("spiral.has-pixels", hasAlpha(spiral));

    const QImage confetti = creative::confettiField(
        stage, 13, { QColor(255, 59, 78), QColor(43, 89, 255) }, 40, 2.0, 6.0);
    check("confetti.has-pixels", hasAlpha(confetti));

    const QImage stars = creative::starField(stage, 17, QColor(255, 255, 255),
                                             40);
    check("stars.has-pixels", hasAlpha(stars));
    check("stars.palette-overload",
          hasAlpha(creative::starField(stage, 17,
                                       { QColor(255, 255, 255),
                                         QColor(0, 229, 255) },
                                       40)));

    const QImage butterflies = creative::butterflyField(
        stage, 19, { QColor(255, 61, 218), QColor(0, 229, 255) },
        { QPointF(80, 60), QPointF(240, 90) }, 6);
    check("butterflies.has-pixels", hasAlpha(butterflies));
    check("butterflies.deterministic",
          hashImage(butterflies)
              == hashImage(creative::butterflyField(
                     stage, 19,
                     { QColor(255, 61, 218), QColor(0, 229, 255) },
                     { QPointF(80, 60), QPointF(240, 90) }, 6)));

    QImage subjectCanvas(stage, QImage::Format_ARGB32);
    subjectCanvas.fill(Qt::transparent);
    {
        QPainter p(&subjectCanvas);
        p.setBrush(QColor(200, 120, 40, 255));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(160, 120), 60, 80);
    }
    const QImage kaleido = creative::wedgeKaleidoscope(
        subjectCanvas, stage, 6, 1.0, 0.0);
    check("kaleido.size", kaleido.size() == stage);
    check("kaleido.has-pixels", hasAlpha(kaleido));

    const QImage reflected = creative::reflection(subjectImage(), 0.3, 0.2);
    check("reflection.size", reflected.size() == subjectImage().size());
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testMasks();
    testRecolor();
    testSheenAndLight();
    testElectricity();
    testColorOps();
    testSceneHelpers();

    std::printf("SUMMARY|%s|failures=%d\n", failures == 0 ? "PASS" : "FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}

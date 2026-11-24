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
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

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

QImage screenComposite(const QImage &base, const QImage &light)
{
    QImage out = base.convertToFormat(QImage::Format_ARGB32);
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_Screen);
    p.drawImage(0, 0, light);
    p.end();
    return out;
}

double meanConfidence(const QImage &conf, const QImage &mask)
{
    if (conf.isNull() || mask.isNull() || conf.size() != mask.size())
        return 0.0;
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < conf.height(); ++y)
    {
        const uchar *cf = conf.constScanLine(y);
        const uchar *mk = mask.constScanLine(y);
        for (int x = 0; x < conf.width(); ++x)
        {
            if (mk[x] <= 8)
                continue;
            sum += cf[x];
            ++n;
        }
    }
    return n > 0 ? sum / (double(n) * 255.0) : 0.0;
}

struct LightnessReport
{
    double mean = 0.0;
    double p95 = 0.0;
    double clipped = 0.0;
    int n = 0;
};

LightnessReport lightnessReport(const QImage &src, const QImage &out,
                                const QImage &mask)
{
    LightnessReport report;
    std::vector<float> errors;
    int clipped = 0;
    for (int y = 0; y < src.height(); ++y)
    {
        const uchar *mk = mask.constScanLine(y);
        for (int x = 0; x < src.width(); ++x)
        {
            if (mk[x] <= 8)
                continue;
            const QRgb a = src.pixel(x, y);
            const QRgb b = out.pixel(x, y);
            const float la = creative::oklabLightness(QColor::fromRgba(a));
            const float lb = creative::oklabLightness(QColor::fromRgba(b));
            errors.push_back(std::fabs(la - lb));
            if (qRed(b) >= 254 || qGreen(b) >= 254 || qBlue(b) >= 254
                || qRed(b) <= 1 || qGreen(b) <= 1 || qBlue(b) <= 1)
                ++clipped;
        }
    }
    if (errors.empty())
        return report;
    double sum = 0.0;
    for (float e : errors)
        sum += e;
    std::sort(errors.begin(), errors.end());
    report.n = int(errors.size());
    report.mean = sum / double(errors.size());
    report.p95 = errors[size_t(double(errors.size()) * 0.95)];
    report.clipped = double(clipped) / double(errors.size());
    return report;
}

void testHoloMaterials()
{
    const QImage subject = subjectImage();
    const QImage mask = creative::maskFromAlpha(subject);
    const QImage normals = creative::estimateNormals(subject, mask, 0.8f);
    check("normals.size", normals.size() == subject.size());
    check("normals.deterministic",
          hashImage(normals)
              == hashImage(creative::estimateNormals(subject, mask, 0.8f)));

    const QImage flat = flatPatch(QColor(128, 128, 128), 64);
    const QImage flatNormals =
        creative::estimateNormals(flat, fullMask(flat.size()), 0.8f);
    check("normals.flat-z",
          qBlue(flatNormals.pixel(32, 32)) >= 253
              && qAbs(qRed(flatNormals.pixel(32, 32)) - 128) <= 2);

    const char *labels[8] = { "liquid", "opal",   "chrome", "prism",
                              "wrap",   "chameleon", "glitter", "livery" };
    QSet<quint64> fingerprints;
    QSet<QString> names;
    QVector<QImage> composites;
    double meanR[8] = { 0 }, meanG[8] = { 0 }, meanB[8] = { 0 };
    double meanL[8] = { 0 }, meanA[8] = { 0 }, meanOklabB[8] = { 0 };
    for (int i = 0; i < 8; ++i)
    {
        const auto id = static_cast<creative::HoloMaterialId>(i);
        const creative::HoloMaterial material =
            creative::holoMaterialPreset(id);
        const QByteArray tag =
            QStringLiteral("holo.%1").arg(labels[i]).toUtf8();
        check((tag + ".name").constData(), !material.name.isEmpty());
        names.insert(material.name);

        const creative::HoloMaterialResult a = creative::holographicMaterial(
            subject, mask, normals, material, 1.0f, 4242);
        const creative::HoloMaterialResult b = creative::holographicMaterial(
            subject, mask, normals, material, 1.0f, 4242);
        check((tag + ".size").constData(),
              a.albedo.size() == subject.size()
                  && a.sheen.size() == subject.size());
        check((tag + ".deterministic").constData(),
              hashImage(a.albedo) == hashImage(b.albedo)
                  && hashImage(a.sheen) == hashImage(b.sheen));

        /* alpha over the WHOLE image, not two sample points */
        int alphaMismatch = 0;
        int sheenOutside = 0;
        double sumR = 0.0, sumG = 0.0, sumB = 0.0;
        double sumL = 0.0, sumA = 0.0, sumB2 = 0.0;
        int meanN = 0;
        const QImage composite = screenComposite(a.albedo, a.sheen);
        composites.append(composite);
        for (int y = 0; y < subject.height(); ++y)
        {
            const uchar *mk = mask.constScanLine(y);
            for (int x = 0; x < subject.width(); ++x)
            {
                const int expected =
                    qMin(qAlpha(subject.pixel(x, y)), int(mk[x]));
                if (qAlpha(a.albedo.pixel(x, y)) != expected)
                    ++alphaMismatch;
                if (mk[x] == 0 && qAlpha(a.sheen.pixel(x, y)) > 0)
                    ++sheenOutside;
                if (mk[x] > 8)
                {
                    const QRgb p = composite.pixel(x, y);
                    sumR += qRed(p);
                    sumG += qGreen(p);
                    sumB += qBlue(p);
                    const creative::OklabColor oc =
                        creative::oklabColor(QColor::fromRgba(p));
                    sumL += oc.L;
                    sumA += oc.a;
                    sumB2 += oc.b;
                    ++meanN;
                }
            }
        }
        check((tag + ".alpha-full").constData(), alphaMismatch == 0);
        check((tag + ".sheen-boundary").constData(), sheenOutside == 0);
        if (meanN > 0)
        {
            meanR[i] = sumR / (double(meanN) * 255.0);
            meanG[i] = sumG / (double(meanN) * 255.0);
            meanB[i] = sumB / (double(meanN) * 255.0);
            meanL[i] = sumL / double(meanN);
            meanA[i] = sumA / double(meanN);
            meanOklabB[i] = sumB2 / double(meanN);
        }

        const LightnessReport lr =
            lightnessReport(subject, a.albedo, mask);
        std::printf("METRIC.holo.%s.albedo|mean=%.4f|p95=%.4f|clip=%.4f|"
                    "pixels=%d\n",
                    labels[i], lr.mean, lr.p95, lr.clipped, lr.n);
        check((tag + ".albedo-lightness-mean").constData(), lr.mean < 0.15);
        check((tag + ".albedo-lightness-p95").constData(), lr.p95 < 0.35);
        check((tag + ".albedo-clipping").constData(), lr.clipped < 0.25);

        const LightnessReport lc =
            lightnessReport(subject, composite, mask);
        std::printf("METRIC.holo.%s.composite|mean=%.4f|p95=%.4f|clip=%.4f\n",
                    labels[i], lc.mean, lc.p95, lc.clipped);
        check((tag + ".composite-lightness-mean").constData(),
              lc.mean < 0.18);
        check((tag + ".composite-clipping").constData(), lc.clipped < 0.30);
        check((tag + ".sheen").constData(), hasAlpha(a.sheen));
        fingerprints.insert(hashImage(a.albedo) ^ (hashImage(a.sheen) << 1));
    }
    check("holo.recipes-unique", fingerprints.size() == 8);
    check("holo.names-unique", names.size() == 8);

    double minPair = 1.0e9;
    double minMean = 1.0e9;
    double minRms = 1.0e9;
    int minRmsI = 0, minRmsJ = 0;
    for (int i = 0; i < 8; ++i)
    {
        for (int j = i + 1; j < 8; ++j)
        {
            const double dr = meanR[i] - meanR[j];
            const double dg = meanG[i] - meanG[j];
            const double db = meanB[i] - meanB[j];
            const double rgb =
                std::sqrt(dr * dr + dg * dg + db * db);
            if (rgb < minPair)
                minPair = rgb;
            const double dl = meanL[i] - meanL[j];
            const double da = meanA[i] - meanA[j];
            const double dbb = meanOklabB[i] - meanOklabB[j];
            const double delta = std::sqrt(dl * dl + da * da + dbb * dbb);
            if (delta < minMean)
                minMean = delta;

            /* spatial perceptual difference, not only the mean colour */
            if (composites.size() == 8)
            {
                double sum = 0.0;
                int n = 0;
                for (int y = 0; y < subject.height(); ++y)
                {
                    const uchar *mk = mask.constScanLine(y);
                    for (int x = 0; x < subject.width(); ++x)
                    {
                        if (mk[x] <= 8)
                            continue;
                        const creative::OklabColor a = creative::oklabColor(
                            QColor::fromRgba(composites[i].pixel(x, y)));
                        const creative::OklabColor b = creative::oklabColor(
                            QColor::fromRgba(composites[j].pixel(x, y)));
                        const double dL = a.L - b.L;
                        const double dA = a.a - b.a;
                        const double dB = a.b - b.b;
                        sum += dL * dL + dA * dA + dB * dB;
                        ++n;
                    }
                }
                const double rms = n > 0 ? std::sqrt(sum / double(n)) : 0.0;
                if (rms < minRms)
                {
                    minRms = rms;
                    minRmsI = i;
                    minRmsJ = j;
                }
            }
        }
    }
    std::printf("METRIC.holo.pairwise|rms-oklab=%.4f|mean-oklab=%.4f|"
                "mean-rgb=%.4f|%s|%s\n",
                minRms, minMean, minPair, labels[minRmsI], labels[minRmsJ]);
    check("holo.pairwise-perceptual", minRms > 0.02);
    check("holo.pairwise-mean-rgb", minPair > 0.015);
    check("holo.automotive-names",
          names.contains(QStringLiteral("Holographic wrap"))
              && names.contains(QStringLiteral("Chameleon paint"))
              && names.contains(QStringLiteral("Glitter metallic"))
              && names.contains(QStringLiteral("Electric livery")));

    const QImage styled =
        creative::holographicMaterial(
            subject, mask, normals,
            creative::holoMaterialPreset(
                creative::HoloMaterialId::LiquidHologram),
            1.0f, 4242)
            .albedo;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    int n = 0;
    for (int y = 80; y < 220; y += 3)
    {
        for (int x = 40; x < 180; x += 3)
        {
            if (qAlpha(subject.pixel(x, y)) < 200)
                continue;
            const double a =
                creative::oklabLightness(QColor::fromRgba(subject.pixel(x, y)));
            const double b =
                creative::oklabLightness(QColor::fromRgba(styled.pixel(x, y)));
            sx += a;
            sy += b;
            sxx += a * a;
            syy += b * b;
            sxy += a * b;
            ++n;
        }
    }
    const double denom = std::sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
    const double corr = denom > 0 ? (n * sxy - sx * sy) / denom : 0.0;
    std::printf("METRIC.holo.luminance-correlation|%.4f\n", corr);
    check("holo.luminance-correlation", corr > 0.60);

    check("coverage.inherited",
          creative::composeCoverage(
              128, 128, creative::CoverageMode::InheritedCoverage) == 128);
    check("coverage.independent",
          creative::composeCoverage(
              128, 128, creative::CoverageMode::IndependentProbability) == 64);
    check("coverage.user",
          creative::composeCoverage(
              255, 128, creative::CoverageMode::UserConstraint) == 128);

    /* the same material at 2x must downscale back to the 1x look */
    const QSize bigSize = subject.size() * 2;
    const QImage big = subject.scaled(bigSize, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
    const QImage bigMask = mask.scaled(bigSize, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
    QImage bigConf;
    const QImage bigNormals =
        creative::estimateNormals(big, bigMask, 0.8f, &bigConf);
    QImage oneConf;
    const QImage oneNormals =
        creative::estimateNormals(subject, mask, 0.8f, &oneConf);
    creative::HoloMaterial resMaterial = creative::holoMaterialPreset(
        creative::HoloMaterialId::LiquidHologram);
    resMaterial.sparkle = 0.0f; /* glints are deliberately resolution-specific */
    const creative::HoloMaterialResult one = creative::holographicMaterial(
        subject, mask, oneNormals, resMaterial, 1.0f, 4242,
        creative::CoverageMode::InheritedCoverage, oneConf);
    const creative::HoloMaterialResult two = creative::holographicMaterial(
        big, bigMask, bigNormals, resMaterial, 1.0f, 4242,
        creative::CoverageMode::InheritedCoverage, bigConf);
    const QImage twoSmall = two.albedo.scaled(
        subject.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    double sx2 = 0, sy2 = 0, sxx2 = 0, syy2 = 0, sxy2 = 0;
    int n2 = 0;
    for (int y = 40; y < subject.height() - 20; y += 3)
    {
        for (int x = 40; x < subject.width() - 20; x += 3)
        {
            if (mask.constScanLine(y)[x] < 200)
                continue;
            const double a =
                creative::oklabLightness(QColor::fromRgba(one.albedo.pixel(x, y)));
            const double b = creative::oklabLightness(
                QColor::fromRgba(twoSmall.pixel(x, y)));
            sx2 += a;
            sy2 += b;
            sxx2 += a * a;
            syy2 += b * b;
            sxy2 += a * b;
            ++n2;
        }
    }
    const double denom2 =
        std::sqrt((n2 * sxx2 - sx2 * sx2) * (n2 * syy2 - sy2 * sy2));
    const double resCorr =
        denom2 > 0 ? (n2 * sxy2 - sx2 * sy2) / denom2 : 0.0;
    std::printf("METRIC.holo.resolution-correlation|%.4f|samples=%d\n",
                resCorr, n2);
    check("holo.resolution-independent", resCorr > 0.85);

    /* a smooth fold must score higher confidence than printed stripes */
    QImage smooth(220, 300, QImage::Format_ARGB32);
    for (int y = 0; y < smooth.height(); ++y)
    {
        QRgb *row = reinterpret_cast<QRgb *>(smooth.scanLine(y));
        for (int x = 0; x < smooth.width(); ++x)
        {
            const int v = 60 + (x * 100) / smooth.width();
            row[x] = qRgba(v, v, v, 255);
        }
    }
    const QImage smoothMask = fullMask(smooth.size());
    QImage smoothConf;
    creative::estimateNormals(smooth, smoothMask, 0.8f, &smoothConf);
    QImage stripes = smooth;
    for (int y = 0; y < stripes.height(); ++y)
    {
        QRgb *row = reinterpret_cast<QRgb *>(stripes.scanLine(y));
        for (int x = 0; x < stripes.width(); ++x)
        {
            if (((x / 6) % 2) != 0)
                continue;
            row[x] = qRgba(40, 40, 40, 255);
        }
    }
    QImage stripeConf;
    creative::estimateNormals(stripes, smoothMask, 0.8f, &stripeConf);
    const double foldConf = meanConfidence(smoothConf, smoothMask);
    const double stripeConfMean = meanConfidence(stripeConf, smoothMask);
    std::printf("METRIC.normals.confidence|fold=%.4f|stripes=%.4f\n",
                foldConf, stripeConfMean);
    check("normals.confidence-separates-texture",
          foldConf > stripeConfMean + 0.08);
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
    testHoloMaterials();

    std::printf("SUMMARY|%s|failures=%d\n", failures == 0 ? "PASS" : "FAIL",
                failures);
    return failures == 0 ? 0 : 1;
}

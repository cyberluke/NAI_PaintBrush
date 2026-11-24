/*****************************************************************************
 * creative_recipe_test.cpp : standalone harness for pb_creative_recipe.hpp
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

/* pb_creative_recipe.hpp is VLC-free and has no QObject/controller/document
 * dependency. This probe compiles the EXACT production builder with Qt
 * Core/Gui only and verifies the contracts the asynchronous commit relies on:
 *   - a build at a native canvas larger than the legacy 1600 px cap really is
 *     authored at the native size (no upscale, no second renderer),
 *   - the same snapshot + seed produces byte-identical pieces and composite,
 *   - cancellation stops the build early,
 *   - preview composition is bounded and deterministic,
 *   - the direct-renderer memory bound refuses oversized canvases,
 *   - every existing fashion/automotive patch id still maps to the same
 *     material recipe. */

#include "pb_creative_recipe.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QStringList>

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

/* A soft-edged figure with a hue ramp and a highlight band: enough structure
 * for rim bands, recolour and material normals without a GUI. */
QImage subjectImage(int w = 200, int h = 300)
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

CreativeBuildInput makeInput(const QString &patchId, const QSize &stage,
                             const QImage &subject)
{
    CreativeBuildInput input;
    input.patchId = patchId;
    input.patchName = patchId;
    input.subject = subject;
    const qreal margin = 0.12;
    input.subjectRect = QRectF(stage.width() * 0.30, stage.height() * margin,
                               stage.width() * 0.40,
                               stage.height() * (1.0 - margin * 2.0));
    input.stage = stage;
    input.previewSize = creative::recipe::previewStage(stage);
    input.seed = 0x5EED1234u;
    input.frame.x = 0.68f;
    input.frame.y = 0.62f;
    input.frame.energy = 0.35f;
    input.frame.hold = true;
    input.frame.time = 1.7f;
    return input;
}

bool samePieces(const QVector<CreativePiece> &a,
                const QVector<CreativePiece> &b)
{
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); ++i)
    {
        const CreativePiece &pa = a.at(i);
        const CreativePiece &pb = b.at(i);
        if (pa.name != pb.name || pa.role != pb.role
            || pa.rect != pb.rect || pa.pivot != pb.pivot
            || pa.blendMode != pb.blendMode
            || !qFuzzyCompare(pa.opacity, pb.opacity)
            || pa.image.size() != pb.image.size()
            || hashImage(pa.image) != hashImage(pb.image))
            return false;
    }
    return true;
}

void testPreviewCap()
{
    const QSize small = creative::recipe::previewStage(QSize(1440, 1800));
    const QSize large = creative::recipe::previewStage(QSize(3840, 2160));
    check("preview.valid", small.isValid() && large.isValid());
    check("preview.bounded",
          large.width() <= 561 && large.height() <= 681);
    check("preview.aspect",
          std::abs(double(large.width()) / large.height()
                       - 3840.0 / 2160.0)
              < 0.02);
}

void testBudget()
{
    check("budget.normal-canvas",
          creative::recipe::fullResolutionBudgetError(QSize(2048, 2560))
              .isEmpty());
    check("budget.native-4k",
          creative::recipe::fullResolutionBudgetError(QSize(3840, 2160))
              .isEmpty());
    check("budget.refuses-huge",
          !creative::recipe::fullResolutionBudgetError(QSize(12000, 12000))
               .isEmpty());
}

void testFullResolutionDeterminism()
{
    const QSize stage(2048, 2560); /* both sides > the legacy 1600 cap */
    const QImage subject = subjectImage();
    const CreativeBuildInput input = makeInput("neon-idol", stage, subject);
    check("fullres.preview-capped",
          input.previewSize.width() <= 561
              && input.previewSize.height() <= 681);

    QElapsedTimer clock;
    clock.start();
    const QVector<CreativePiece> first = creative::recipe::build(input);
    const qint64 firstMs = clock.elapsed();
    clock.restart();
    const QVector<CreativePiece> second = creative::recipe::build(input);
    const qint64 secondMs = clock.elapsed();

    std::printf("INFO.fullres|stage=%dx%d pieces=%d build_ms=%lld/%lld\n",
                stage.width(), stage.height(), first.size(),
                static_cast<long long>(firstMs),
                static_cast<long long>(secondMs));

    check("fullres.pieces", first.size() >= 8);
    check("fullres.deterministic", samePieces(first, second));

    int nativePieces = 0;
    for (const CreativePiece &p : first)
        if (p.image.size() == stage)
            ++nativePieces;
    std::printf("INFO.fullres.native_pieces|%d\n", nativePieces);
    check("fullres.native-size", nativePieces >= 1);

    /* The baked playback geometry must also be deterministic. */
    const CreativeFrame frame = input.frame;
    const QVector<CreativePiece> bakedA =
        creative::recipe::bake(first, frame, stage);
    const QVector<CreativePiece> bakedB =
        creative::recipe::bake(second, frame, stage);
    check("fullres.bake-deterministic", samePieces(bakedA, bakedB));
    check("fullres.bake-opacity", bakedA.size() == first.size());

    /* Composition at the native stage and at the bounded preview must both be
     * deterministic and must not change the underlying pieces. */
    const QImage composedFullA = creative::recipe::compose(
        bakedA, stage, stage, QColor(12, 12, 18), frame);
    const QImage composedFullB = creative::recipe::compose(
        bakedB, stage, stage, QColor(12, 12, 18), frame);
    check("compose.full-size", composedFullA.size() == stage);
    check("compose.full-deterministic",
          hashImage(composedFullA) == hashImage(composedFullB));

    const QImage composedPreview = creative::recipe::compose(
        bakedA, stage, input.previewSize, QColor(12, 12, 18), frame);
    check("compose.preview-size",
          composedPreview.size() == input.previewSize);
    check("compose.preview-not-empty", hashImage(composedPreview) != 0);
}

void testCancellation()
{
    const QSize stage(1024, 1280);
    const CreativeBuildInput input =
        makeInput("neon-idol", stage, subjectImage());
    int observed = 0;
    const QVector<CreativePiece> pieces = creative::recipe::build(
        input, [&observed](const QVector<CreativePiece> &, int count) {
            observed = count;
            return count < 2; /* cancel after the second piece */
        });
    std::printf("INFO.cancel|observed=%d returned=%d\n", observed,
                pieces.size());
    check("cancel.stops-early", observed == 2);
    check("cancel.partial-result", pieces.size() == 2);
}

void testPatches()
{
    using creative::recipe::detail::holoMaterialIdForPatch;
    check("patch.holo-fashion-id",
          !creative::recipe::detail::isHoloMaterialPatch(
               QStringLiteral("holo-fashion"))
              && holoMaterialIdForPatch(QStringLiteral("holo-fashion"))
                     == creative::HoloMaterialId::LiquidHologram);
    check("patch.wrap-id",
          holoMaterialIdForPatch(QStringLiteral("holo-wrap"))
              == creative::HoloMaterialId::HolographicWrap);
    check("patch.chameleon-id",
          holoMaterialIdForPatch(QStringLiteral("holo-chameleon"))
              == creative::HoloMaterialId::ChameleonPaint);
    check("patch.glitter-automotive",
          creative::recipe::detail::isAutomotivePatch(
              QStringLiteral("holo-glitter"))
              && holoMaterialIdForPatch(QStringLiteral("holo-glitter"))
                     == creative::HoloMaterialId::GlitterMetallic);
    check("patch.livery-id",
          holoMaterialIdForPatch(QStringLiteral("holo-livery"))
              == creative::HoloMaterialId::ElectricLivery);

    /* Both a fashion id and an automotive id must still produce their
     * material pieces through the one renderer. */
    const CreativeBuildInput fashion = makeInput(
        QStringLiteral("holo-fashion"), QSize(480, 600), subjectImage());
    const QVector<CreativePiece> fashionPieces =
        creative::recipe::build(fashion);
    check("patch.holo-fashion-build", fashionPieces.size() >= 6);

    const CreativeBuildInput wrap = makeInput(
        QStringLiteral("holo-wrap"), QSize(480, 600), subjectImage());
    const QVector<CreativePiece> wrapPieces = creative::recipe::build(wrap);
    bool hasSheen = false;
    for (const CreativePiece &p : wrapPieces)
        if (p.name == QLatin1String("Material sheen"))
            hasSheen = true;
    check("patch.holo-wrap-build", wrapPieces.size() >= 5);
    check("patch.holo-wrap-sheen", hasSheen);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    try
    {
        testPreviewCap();
        testBudget();
        testFullResolutionDeterminism();
        testCancellation();
        testPatches();
    }
    catch (...)
    {
        check("unexpected-exception", false);
    }

    std::printf("SUMMARY|%s|failures=%d\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

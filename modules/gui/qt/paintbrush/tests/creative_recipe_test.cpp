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
 *   - a build at a native canvas really is authored at the native size (no
 *     upscale, no second renderer), with an opt-in 3840x2160 render,
 *   - the same snapshot + seed produces byte-identical pieces and composite,
 *   - cancellation stops the build early,
 *   - preview composition follows the 4K budget and stays deterministic,
 *   - a preview build carries a dynamic vector field that reacts to the pad,
 *   - the direct-renderer memory bound refuses oversized canvases,
 *   - every existing fashion/automotive patch id still maps to the same
 *     material recipe. */

#include "pb_creative_recipe.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QStringList>

#include <cmath>
#include <cstdio>
#include <cstring>

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

void testPreviewResolution()
{
    const QSize small = creative::recipe::previewStage(QSize(1440, 1800));
    const QSize large = creative::recipe::previewStage(QSize(3840, 2160));
    const QSize huge = creative::recipe::previewStage(QSize(12000, 12000));
    const QSize motion =
        creative::recipe::interactivePreviewStage(QSize(3840, 2160));
    check("preview.valid", small.isValid() && large.isValid() && huge.isValid());
    /* The old 560x680 thumbnail is gone: a normal canvas previews at its
     * native size and a UHD canvas stays 3840x2160. */
    check("preview.native", small == QSize(1440, 1800)
                                 && large == QSize(3840, 2160));
    check("preview.4k-budget",
          qint64(huge.width()) * huge.height()
              <= qint64(3840) * 2160 * 101 / 100);
    check("preview.motion-budget",
          qint64(motion.width()) * motion.height()
              <= qint64(1280) * 720 * 101 / 100);
    check("preview.motion-aspect",
          std::abs(double(motion.width()) / motion.height()
                       - 3840.0 / 2160.0)
              < 0.02);
    check("preview.aspect",
          std::abs(double(large.width()) / large.height()
                       - 3840.0 / 2160.0)
              < 0.02);
}

void testVectorModulation()
{
    const QSize stage(640, 800);
    const QImage subject = subjectImage();

    auto pieceNamed = [](const QVector<CreativePiece> &pieces,
                         const char *name) -> const CreativePiece * {
        for (const CreativePiece &p : pieces)
            if (p.name == QLatin1String(name))
                return &p;
        return nullptr;
    };

    CreativeBuildInput low = makeInput("neon-idol", stage, subject);
    low.frame.x = 0.12f;
    low.frame.y = 0.18f;
    low.frame.energy = 0.0f;
    low.frame.hold = false;
    CreativeBuildInput high = low;
    high.frame.x = 0.92f;
    high.frame.y = 0.86f;
    high.frame.energy = 0.85f;
    high.frame.hold = true;

    const QVector<CreativePiece> lowPieces = creative::recipe::build(low);
    const QVector<CreativePiece> highPieces = creative::recipe::build(high);
    const CreativePiece *lowField =
        pieceNamed(lowPieces, "Vector discharge");
    const CreativePiece *highField =
        pieceNamed(highPieces, "Vector discharge");
    check("vector.field-present", lowField && highField);
    check("vector.field-full-stage",
          lowField && highField && lowField->image.size() == stage
              && highField->image.size() == stage);
    check("vector.field-react",
          lowField && highField
              && hashImage(lowField->image) != hashImage(highField->image));

    /* Voltage Idol's bolt count and projection come from the vector graph,
     * including the sequential charge -> discharge dependency. */
    CreativeBuildInput boltLow = makeInput("electric-kpop", stage, subject);
    boltLow.frame.x = 0.12f;
    boltLow.frame.y = 0.18f;
    boltLow.frame.energy = 0.0f;
    boltLow.frame.hold = false;
    CreativeBuildInput boltHigh = boltLow;
    boltHigh.frame.x = 0.92f;
    boltHigh.frame.y = 0.86f;
    boltHigh.frame.energy = 0.85f;
    boltHigh.frame.hold = true;

    auto boltCount = [](const QVector<CreativePiece> &pieces) {
        int count = 0;
        for (const CreativePiece &p : pieces)
            if (p.name.startsWith(QLatin1String("Voltage bolt")))
                ++count;
        return count;
    };
    const QVector<CreativePiece> lowVoltage =
        creative::recipe::build(boltLow);
    const QVector<CreativePiece> highVoltage =
        creative::recipe::build(boltHigh);
    const int lowBolts = boltCount(lowVoltage);
    const int highBolts = boltCount(highVoltage);
    std::printf("INFO.vector|bolts=%d/%d\n", lowBolts, highBolts);
    check("vector.voltage-bolts-react", lowBolts < highBolts);

    const CreativePiece *lowProjection =
        pieceNamed(lowVoltage, "Voltage projection");
    const CreativePiece *highProjection =
        pieceNamed(highVoltage, "Voltage projection");
    check("vector.projection-react",
          lowProjection && highProjection
              && hashImage(lowProjection->image)
                     != hashImage(highProjection->image));
}

void testDynamicPreview()
{
    const QSize stage(640, 800);
    CreativeBuildInput input = makeInput("neon-idol", stage, subjectImage());
    input.preview = true;
    const QVector<CreativePiece> pieces = creative::recipe::build(input);
    const CreativePiece *live = nullptr;
    for (const CreativePiece &p : pieces)
        if (p.dynamicVector)
        {
            live = &p;
            break;
        }
    check("vector.preview-dynamic-piece", live != nullptr);
    if (!live)
        return;

    CreativeFrame low = input.frame;
    low.x = 0.15f;
    low.y = 0.20f;
    low.energy = 0.10f;
    low.hold = false;
    low.time = 1.0f;
    CreativeFrame high = low;
    high.x = 0.90f;
    high.y = 0.85f;
    high.energy = 0.90f;
    high.hold = true;
    high.time = 3.0f;

    const QSize target(320, 400);
    const QImage lowOverlay = creative::recipe::detail::liveVectorOverlay(
        live->vectorOverlay, low, target);
    const QImage highOverlay = creative::recipe::detail::liveVectorOverlay(
        live->vectorOverlay, high, target);
    check("vector.preview-overlay-size",
          lowOverlay.size() == target && highOverlay.size() == target);
    check("vector.preview-overlay-react",
          hashImage(lowOverlay) != hashImage(highOverlay));

    if (qEnvironmentVariableIsSet("PB_TEST_DUMP"))
    {
        const auto dumpOverlay = [&](const CreativeVectorOverlay &spec,
                                     const CreativeFrame &frame,
                                     const QString &name) {
            const QImage overlay =
                creative::recipe::detail::liveVectorOverlay(spec, frame,
                                                            stage);
            QImage canvas(stage, QImage::Format_ARGB32_Premultiplied);
            canvas.fill(QColor(12, 12, 18));
            QPainter painter(&canvas);
            painter.drawImage(0, 0, overlay);
            painter.end();
            canvas.save(QDir::tempPath() + "/" + name);
        };
        dumpOverlay(live->vectorOverlay, low, "pb-live-vector-low.png");
        dumpOverlay(live->vectorOverlay, high, "pb-live-vector-high.png");
    }

    CreativeBuildInput volt = makeInput("electric-kpop", stage, subjectImage());
    volt.preview = true;
    const QVector<CreativePiece> voltPieces = creative::recipe::build(volt);
    bool voltLive = false;
    for (const CreativePiece &p : voltPieces)
        if (p.dynamicVector && p.vectorOverlay.style == 1)
            voltLive = true;
    check("vector.preview-voltage-live", voltLive);

    if (qEnvironmentVariableIsSet("PB_TEST_DUMP"))
    {
        for (const CreativePiece &p : voltPieces)
        {
            if (!p.dynamicVector)
                continue;
            const QImage overlay = creative::recipe::detail::liveVectorOverlay(
                p.vectorOverlay, high, stage);
            QImage canvas(stage, QImage::Format_ARGB32_Premultiplied);
            canvas.fill(QColor(12, 12, 18));
            QPainter painter(&canvas);
            painter.drawImage(0, 0, overlay);
            painter.end();
            canvas.save(QDir::tempPath() + "/pb-live-vector-voltage.png");
        }
    }

    /* Interactive cost with the live overlay at the real preview build
     * resolution and the 1 MP compose target. */
    const QSize previewStage(1440, 1800);
    const QSize previewTarget =
        creative::recipe::interactivePreviewStage(previewStage);
    CreativeBuildInput liveInput =
        makeInput("neon-idol", previewStage, subjectImage());
    liveInput.preview = true;
    const QVector<CreativePiece> livePieces =
        creative::recipe::build(liveInput);
    QElapsedTimer liveClock;
    liveClock.start();
    QImage liveFrame;
    for (int i = 0; i < 3; ++i)
        liveFrame = creative::recipe::compose(
            livePieces, previewStage, previewTarget, QColor(12, 12, 18),
            liveInput.frame);
    const qint64 liveMs = liveClock.elapsed() / 3;
    std::printf("INFO.live-vector|target=%dx%d ms=%lld\n",
                previewTarget.width(), previewTarget.height(),
                static_cast<long long>(liveMs));
    check("vector.live-frame-size", liveFrame.size() == previewTarget);
}

void testProtectionScaling()
{
    const QSize stage(640, 800);
    const QImage subject = subjectImage(320, 400);
    CreativeBuildInput input = makeInput("holo-liquid", stage, subject);

    /* The subjectRect is not the native cutout size, so this is exactly the
     * path that used to drop the mask at commit. If the mask is ignored, the
     * two builds are identical; if it is scaled, the protected build keeps the
     * original subject pixels and the unprotected build runs the material. */
    QImage protection(subject.size(), QImage::Format_Grayscale8);
    protection.fill(255);
    input.protection = protection;
    const QVector<CreativePiece> protectedPieces =
        creative::recipe::build(input);
    input.protection = QImage();
    const QVector<CreativePiece> plainPieces = creative::recipe::build(input);

    const QImage protectedImage = creative::recipe::compose(
        protectedPieces, stage, stage, QColor(12, 12, 18), input.frame);
    const QImage plainImage = creative::recipe::compose(
        plainPieces, stage, stage, QColor(12, 12, 18), input.frame);
    check("protection.scaled-applied",
          hashImage(protectedImage) != hashImage(plainImage));
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
    /* 2048x2560 is under the 4K preview budget, so the preview is native. */
    check("fullres.preview-native", input.previewSize == stage);

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

    /* Interactive recomposition budget: the Play Pad calls compose() on the
     * idle tick and on every pad move, so this is the metric that decides
     * whether a 4K preview needs a lower-resolution interactive path. */
    clock.restart();
    for (int i = 0; i < 5; ++i)
        creative::recipe::compose(bakedA, stage, input.previewSize,
                                  QColor(12, 12, 18), frame);
    const qint64 composeMs = clock.elapsed() / 5;
    std::printf("INFO.compose.preview|ms=%lld size=%dx%d\n",
                static_cast<long long>(composeMs), input.previewSize.width(),
                input.previewSize.height());

    const QSize interactive =
        creative::recipe::interactivePreviewStage(input.previewSize);
    clock.restart();
    for (int i = 0; i < 5; ++i)
        creative::recipe::compose(bakedA, stage, interactive,
                                  QColor(12, 12, 18), frame);
    const qint64 interactiveMs = clock.elapsed() / 5;
    std::printf("INFO.compose.interactive|ms=%lld size=%dx%d\n",
                static_cast<long long>(interactiveMs), interactive.width(),
                interactive.height());
    check("compose.interactive-smaller",
          interactive.width() < input.previewSize.width()
              && interactive.height() < input.previewSize.height());
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

void test4KNative()
{
    const QSize stage(3840, 2160);
    check("fullres.4k-budget",
          creative::recipe::fullResolutionBudgetError(stage).isEmpty());

    CreativeBuildInput neon = makeInput("neon-idol", stage, subjectImage());
    QElapsedTimer clock;
    clock.start();
    const QVector<CreativePiece> neonPieces = creative::recipe::build(neon);
    const qint64 neonBuildMs = clock.elapsed();
    clock.restart();
    const QImage neonFrame = creative::recipe::compose(
        neonPieces, stage, stage, QColor(12, 12, 18), neon.frame);
    const qint64 neonComposeMs = clock.elapsed();
    std::printf("INFO.4k-neon|stage=%dx%d pieces=%d build_ms=%lld "
                "compose_ms=%lld\n",
                stage.width(), stage.height(), int(neonPieces.size()),
                static_cast<long long>(neonBuildMs),
                static_cast<long long>(neonComposeMs));
    check("fullres.4k-neon-size", neonFrame.size() == stage);
    check("fullres.4k-neon-pieces", neonPieces.size() >= 8);

    /* The material path is the heaviest 4K target: it estimates normals and
     * runs the full per-pixel holographic material over the subject. */
    CreativeBuildInput holo = makeInput("holo-liquid", stage, subjectImage());
    clock.restart();
    const QVector<CreativePiece> holoPieces = creative::recipe::build(holo);
    const qint64 holoBuildMs = clock.elapsed();
    clock.restart();
    const QImage holoFrame = creative::recipe::compose(
        holoPieces, stage, stage, QColor(12, 12, 18), holo.frame);
    const qint64 holoComposeMs = clock.elapsed();
    std::printf("INFO.4k-material|stage=%dx%d pieces=%d build_ms=%lld "
                "compose_ms=%lld\n",
                stage.width(), stage.height(), int(holoPieces.size()),
                static_cast<long long>(holoBuildMs),
                static_cast<long long>(holoComposeMs));
    check("fullres.4k-material-size", holoFrame.size() == stage);
    check("fullres.4k-material-pieces", holoPieces.size() >= 5);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    bool fourK = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--4k") == 0)
            fourK = true;

    try
    {
        testPreviewResolution();
        testVectorModulation();
        testDynamicPreview();
        testProtectionScaling();
        testBudget();
        testFullResolutionDeterminism();
        testCancellation();
        testPatches();
        if (fourK)
            test4KNative();
    }
    catch (...)
    {
        check("unexpected-exception", false);
    }

    std::printf("SUMMARY|%s|failures=%d\n",
                failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

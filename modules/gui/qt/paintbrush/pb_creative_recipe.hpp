/*****************************************************************************
 * pb_creative_recipe.hpp : NAI Paintbrush — pure creative patch recipes
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_CREATIVE_RECIPE_HPP
#define PB_CREATIVE_RECIPE_HPP

/* The Creative Pad recipes are pure functions of a CreativeBuildInput value:
 * no QObject, controller, document or global state is read here. That is what
 * makes a full-resolution build safe to run on a worker thread while the GUI
 * keeps painting, and what keeps a committed/undone/replayed look identical
 * for the same seed and pad state.
 *
 * This header is deliberately VLC-free (QImage/QPainter + pb_creative_art.hpp)
 * so the standalone recipe harness can compile the exact production builder. */

#include "pb_blend.hpp"
#include "pb_creative_art.hpp"
#include "pb_creative_types.hpp"

#include <QColor>
#include <QImage>
#include <QLinearGradient>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QRadialGradient>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>
#include <QVariantMap>
#include <QVector>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <functional>

namespace paintbrush {

namespace creative {
namespace recipe {

/* ------------------------------------------------------------- primitives */

inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

namespace detail {

inline QImage tintPreserveAlpha(const QImage &img, const QColor &color)
{
    if (img.isNull())
        return {};
    QImage out = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < out.height(); ++y)
    {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x)
        {
            const int a = qAlpha(line[x]);
            line[x] = qRgba(color.red(), color.green(), color.blue(), a);
        }
    }
    return out;
}

inline void fadeHorizontally(QImage &img, bool leftStrong)
{
    if (img.isNull())
        return;
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    QLinearGradient g(0, 0, img.width(), 0);
    if (leftStrong)
    {
        g.setColorAt(0.0, QColor(0, 0, 0, 255));
        g.setColorAt(0.55, QColor(0, 0, 0, 150));
        g.setColorAt(1.0, QColor(0, 0, 0, 0));
    }
    else
    {
        g.setColorAt(0.0, QColor(0, 0, 0, 0));
        g.setColorAt(0.45, QColor(0, 0, 0, 150));
        g.setColorAt(1.0, QColor(0, 0, 0, 255));
    }
    p.fillRect(img.rect(), g);
}

inline QRectF scaleRectAbout(const QRectF &r, qreal factor, qreal dx, qreal dy)
{
    const QPointF c = r.center();
    const QSizeF s = r.size() * factor;
    return QRectF(c.x() - s.width() / 2.0 + dx,
                  c.y() - s.height() / 2.0 + dy,
                  s.width(), s.height());
}

inline QImage stageCanvas(const QSize &size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

inline void drawScaled(QImage &dst, const QImage &src, const QRectF &rect)
{
    if (src.isNull() || rect.isEmpty())
        return;
    QPainter p(&dst);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(rect, src);
}

inline bool isHoloMaterialPatch(const QString &patchId)
{
    return patchId == QLatin1String("holo-liquid")
           || patchId == QLatin1String("holo-opal")
           || patchId == QLatin1String("holo-chrome")
           || patchId == QLatin1String("holo-prism")
           || patchId == QLatin1String("holo-wrap")
           || patchId == QLatin1String("holo-chameleon")
           || patchId == QLatin1String("holo-glitter")
           || patchId == QLatin1String("holo-livery");
}

inline bool isAutomotivePatch(const QString &patchId)
{
    return patchId == QLatin1String("holo-wrap")
           || patchId == QLatin1String("holo-chameleon")
           || patchId == QLatin1String("holo-glitter")
           || patchId == QLatin1String("holo-livery");
}

inline HoloMaterialId holoMaterialIdForPatch(const QString &patchId)
{
    if (patchId == QLatin1String("holo-opal"))
        return HoloMaterialId::OpalDream;
    if (patchId == QLatin1String("holo-chrome"))
        return HoloMaterialId::ChromaticChrome;
    if (patchId == QLatin1String("holo-prism"))
        return HoloMaterialId::PrismaticFantasy;
    if (patchId == QLatin1String("holo-wrap"))
        return HoloMaterialId::HolographicWrap;
    if (patchId == QLatin1String("holo-chameleon"))
        return HoloMaterialId::ChameleonPaint;
    if (patchId == QLatin1String("holo-glitter"))
        return HoloMaterialId::GlitterMetallic;
    if (patchId == QLatin1String("holo-livery"))
        return HoloMaterialId::ElectricLivery;
    return HoloMaterialId::LiquidHologram;
}

} /* namespace detail */

/* ----------------------------------------------------------- stage policy */

/* Bounded preview panel size for a canvas: same 560x680 fit the Play Pad has
 * always used, so the live preview stays cheap regardless of canvas size. */
inline QSize previewStage(const QSize &canvas)
{
    if (!canvas.isValid() || canvas.isEmpty())
        return {};
    const qreal fit = qMin(560.0 / canvas.width(), 680.0 / canvas.height());
    return QSize(qMax(160, int(std::lround(canvas.width() * fit))),
                 qMax(160, int(std::lround(canvas.height() * fit))));
}

/* Rough upper bound used only for progress reporting; the recipes themselves
 * decide how many pieces they emit. */
inline int expectedPieceCount(const QString &patchId)
{
    if (patchId == QLatin1String("neon-idol")
        || patchId == QLatin1String("electric-kpop")
        || patchId == QLatin1String("sticker-forge"))
        return 12;
    if (patchId == QLatin1String("kaleido")
        || patchId == QLatin1String("motion-echo"))
        return 10;
    if (detail::isHoloMaterialPatch(patchId))
        return 11;
    return 10;
}

/* Full-resolution direct renderer (v1) memory bound.
 *
 * Commits build at the native canvas resolution, never at the legacy 1600 px
 * cap and never by upscaling a smaller build. Each recipe keeps several
 * full-stage ARGB32 pieces alive at once, so the direct path is bounded:
 * above the budget below the engine refuses the commit with an explicit
 * message instead of silently lowering the resolution.
 *
 * TILING GAP (documented, not approximated): a tiled renderer must evaluate
 * every stage-global primitive (gradients, procedural fields, blurs/bloom
 * halos, arcs, kaleidoscope wedges) in global coordinates and give each tile
 * a sufficient overlap halo so blur/bloom kernels are not clipped at tile
 * borders. That is deliberately not attempted here; the direct path keeps
 * global coordinates and one seed, so its output is deterministic. */
inline QString fullResolutionBudgetError(const QSize &canvas)
{
    if (!canvas.isValid() || canvas.isEmpty())
        return QStringLiteral("Open a canvas first.");
    constexpr qint64 kMaxNativePixels = qint64(64) * 1024 * 1024;
    constexpr qint64 kBudgetBytes = qint64(2560) * 1024 * 1024;
    constexpr qint64 kAssumedStageImages = 16;
    const qint64 pixels = qint64(canvas.width()) * qint64(canvas.height());
    const qint64 estimate = pixels * 4 * kAssumedStageImages;
    if (pixels > kMaxNativePixels || estimate > kBudgetBytes)
    {
        return QStringLiteral(
                   "This canvas is %1x%2 (%3 MP). A full-resolution direct "
                   "render needs about %4 GB and the bounded renderer refuses "
                   "rather than dropping resolution or upscaling a smaller "
                   "build; tiled rendering is the documented next step.")
            .arg(canvas.width())
            .arg(canvas.height())
            .arg(double(pixels) / 1000000.0, 0, 'f', 1)
            .arg(double(estimate) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
    return {};
}

/* --------------------------------------------------------------- builder */

/* Build the patch pieces. Pure: the same input and seed produce byte-identical
 * pixels on any thread. `progress` (optional) is invoked after every appended
 * piece with the pieces so far; returning false cancels the build at the next
 * checked boundary. */
inline QVector<CreativePiece> build(
    const CreativeBuildInput &input,
    const std::function<bool(const QVector<CreativePiece> &, int)> &progress = {})
{
    QVector<CreativePiece> pieces;
    const QString &patchId = input.patchId;
    if (input.subject.isNull() || !input.stage.isValid()
        || input.stage.isEmpty())
        return pieces;

    QImage subject = input.subject;
    const QRectF subjectRect = input.subjectRect;
    const QSize stage = input.stage;
    const quint32 seed = input.seed;

    bool cancelled = false;
    auto pieceAdded = [&]() -> bool {
        if (cancelled)
            return false;
        if (!progress)
            return true;
        if (!progress(pieces, pieces.size()))
        {
            cancelled = true;
            return false;
        }
        return true;
    };

    QImage s = subject;
    if (s.size() != subjectRect.size().toSize())
        s = s.scaled(subjectRect.size().toSize(), Qt::IgnoreAspectRatio,
                     Qt::SmoothTransformation);

    const qreal W = stage.width();
    const qreal H = stage.height();
    const QImage subjMask = creative::maskFromAlpha(s);

    struct Add
    {
        QVector<CreativePiece> *out;
        QPointF pivot;
        const std::function<bool(const QVector<CreativePiece> &, int)> *progress;
        bool *cancelled;
        void operator()(const QString &name, const QString &role,
                        const QImage &image, const QRectF &rect,
                        int blend = BLEND_NORMAL, float opacity = 1.0f,
                        const CreativeMotion &motion = CreativeMotion()) const
        {
            if (*cancelled)
                return;
            CreativePiece p;
            p.name = name;
            p.role = role;
            p.image = image;
            p.rect = rect;
            p.pivot = pivot;
            p.blendMode = blend;
            p.opacity = opacity;
            p.motion = motion;
            out->append(p);
            if (progress && *progress
                && !(*progress)(*out, out->size()))
                *cancelled = true;
        }
    } add{ &pieces, subjectRect.center(), &progress, &cancelled };

    const QRectF full(QPointF(0, 0), QSizeF(stage));

    auto drawAt = [](QImage &dst, const QImage &src, const QRectF &r) {
        detail::drawScaled(dst, src, r);
    };
    auto band = [&](qreal fraction, const QColor &color, const QString &name,
                    const QString &role, float opacity, int blend,
                    const CreativeMotion &motion) {
        if (cancelled)
            return;
        const int radius = qMax(1, int(std::lround(subjectRect.height() * fraction)));
        QImage bandMask = creative::bandFromMask(subjMask, radius);
        QImage fullBand = detail::stageCanvas(stage);
        drawAt(fullBand, creative::colorizeBand(bandMask, color, false, seed),
               subjectRect);
        CreativePiece p;
        p.name = name;
        p.role = role;
        p.image = fullBand;
        p.rect = full;
        p.pivot = subjectRect.center();
        p.blendMode = blend;
        p.opacity = opacity;
        p.motion = motion;
        pieces.append(p);
        pieceAdded();
    };
    auto subjectFull = [&](const QImage &graded, const QString &name,
                           float opacity, const CreativeMotion &motion) {
        if (cancelled)
            return;
        QImage fullSubj = detail::stageCanvas(stage);
        drawAt(fullSubj, graded, subjectRect);
        CreativePiece p;
        p.name = name;
        p.role = QStringLiteral("subject");
        p.image = fullSubj;
        p.rect = full;
        p.pivot = subjectRect.center();
        p.blendMode = BLEND_NORMAL;
        p.opacity = opacity;
        p.motion = motion;
        pieces.append(p);
        pieceAdded();
    };
    auto solid = [&](const QImage &src, const QColor &color) {
        return detail::tintPreserveAlpha(src, color);
    };

    if (patchId == QLatin1String("neon-idol"))
    {
        const QColor ink(4, 5, 16), cobalt(40, 90, 255), cyan(90, 235, 255),
            rose(255, 80, 160), coral(255, 110, 80), cream(255, 244, 248);
        add(QStringLiteral("Neon wash"), QStringLiteral("backdrop"),
            creative::gradientField(stage, ink, QColor(30, 10, 54),
                                    QColor(80, 45, 170, 130), 0.34),
            full);
        CreativeMotion dotsA;
        dotsA.opacityX = 0.22f;
        add(QStringLiteral("Neon halftone rose"), QStringLiteral("backdrop"),
            creative::halftoneField(stage, QColor(255, 110, 190, 85),
                                    H * 0.022, H * 0.0065, -18, 0.0),
            full, BLEND_SCREEN, 0.55f, dotsA);
        CreativeMotion dotsB;
        dotsB.opacityY = 0.18f;
        add(QStringLiteral("Neon halftone ice"), QStringLiteral("backdrop"),
            creative::halftoneField(stage, QColor(90, 210, 255, 75),
                                    H * 0.027, H * 0.006, 22, 0.5),
            full, BLEND_SCREEN, 0.40f, dotsB);
        if (cancelled)
            return pieces;

        QImage echo = creative::gradeImage(creative::softBlur(s, 7),
                                           creative::Grade{ 1.06f, 1.22f, 0.22f,
                                                            rose, 0.55f, cyan, 0.38f });
        QImage echoFull = detail::stageCanvas(stage);
        drawAt(echoFull, echo,
               detail::scaleRectAbout(subjectRect, 1.72, -W * 0.055, -H * 0.035));
        CreativeMotion echoM;
        echoM.opacityX = 0.16f;
        echoM.dxX = 0.03f;
        add(QStringLiteral("Neon echo"), QStringLiteral("echo"), echoFull, full,
            BLEND_NORMAL, 0.24f, echoM);

        CreativeMotion bloomM;
        bloomM.opacityY = 0.16f;
        bloomM.opacityHold = 0.22f;
        bloomM.scaleHold = 0.06f;
        add(QStringLiteral("Neon bloom"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 qMax(subjectRect.width(), subjectRect.height()) * 0.66,
                                 cobalt, 0.42f),
            full, BLEND_SCREEN, 0.62f, bloomM);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.05f, 1.12f, 1.20f, cobalt, 0.22f, coral, 0.10f });
        subjectFull(graded, QStringLiteral("Neon idol"), 1.0f, CreativeMotion());
        if (cancelled)
            return pieces;

        CreativeMotion rimCyanM;
        rimCyanM.opacityX = -0.34f;
        rimCyanM.opacityY = 0.16f;
        rimCyanM.opacityHold = 0.18f;
        band(0.012, cyan, QStringLiteral("Neon rim ice"),
             QStringLiteral("rim"), 0.85f, BLEND_SCREEN, rimCyanM);
        CreativeMotion rimRoseM;
        rimRoseM.opacityX = 0.34f;
        rimRoseM.opacityY = 0.16f;
        rimRoseM.opacityHold = 0.18f;
        band(0.012, rose, QStringLiteral("Neon rim rose"),
             QStringLiteral("rim"), 0.85f, BLEND_SCREEN, rimRoseM);

        /* split the two rims left/right so the pad reads as directional light */
        if (pieces.size() >= 2)
        {
            detail::fadeHorizontally(pieces[pieces.size() - 2].image, true);
            detail::fadeHorizontally(pieces[pieces.size() - 1].image, false);
        }

        CreativeMotion lightM;
        lightM.opacityY = 0.28f;
        add(QStringLiteral("Neon light"), QStringLiteral("finish"),
            creative::glossOverlay(stage, s, subjectRect, cream),
            full, BLEND_SCREEN, 0.30f, lightM);
        if (cancelled)
            return pieces;

        CreativeMotion sparkM;
        sparkM.opacityEnergy = 0.38f;
        sparkM.opacityHold = 0.18f;
        sparkM.scaleEnergy = 0.10f;
        sparkM.rotationEnergy = 26.0f;
        add(QStringLiteral("Neon sparks"), QStringLiteral("accent"),
            creative::bokehField(stage, seed,
                                 { cream, cyan, rose, coral }, 120,
                                 H * 0.006, H * 0.032),
            full, BLEND_SCREEN, 0.55f, sparkM);

        add(QStringLiteral("Neon scan"), QStringLiteral("finish"),
            creative::scanlineField(stage, QColor(8, 8, 20),
                                    qMax(3, int(H * 0.007)), 0.10),
            full, BLEND_OVERLAY, 0.14f);
    }

    else if (patchId == QLatin1String("electric-kpop"))
    {
        const QColor ink(8, 3, 18), cyan(0, 234, 255), rose(253, 48, 199),
            magenta(245, 0, 212), lime(183, 255, 24), white(244, 252, 255);

        /* scene: a real backdrop, not a sticker behind the cutout */
        add(QStringLiteral("Voltage wash"), QStringLiteral("backdrop"),
            creative::backdrop(stage, seed, { ink, rose, cyan }, 0.90f), full);
        CreativeMotion bloomM;
        bloomM.opacityHold = 0.20f;
        bloomM.scaleEnergy = 0.06f;
        add(QStringLiteral("Voltage bloom"), QStringLiteral("accent"),
            creative::radialGlow(
                stage, subjectRect.center(),
                qMax(subjectRect.width(), subjectRect.height()) * 0.52, rose,
                0.55f),
            full, BLEND_SCREEN, 0.62f, bloomM);
        if (cancelled)
            return pieces;

        /* subject: luminance-preserving recolour, so the fabric keeps its
         * folds instead of turning into a flat gradient */
        creative::RecolorParams recolor;
        recolor.palette = { cyan, rose, magenta, lime };
        recolor.strength = 0.88f;
        recolor.textureKeep = 0.18f;
        recolor.skinGuard = 0.45f;
        recolor.hueDrift = 0.0f;
        recolor.seed = seed;
        QImage voltageSubject =
            creative::recolorMasked(s, subjMask, recolor);
        CreativeMotion recolorM;
        recolorM.opacityX = 0.10f;
        subjectFull(voltageSubject, QStringLiteral("Voltage idol"), 1.0f,
                    recolorM);
        if (cancelled)
            return pieces;

        /* light that moves across the highlights */
        QImage sheen = creative::spectralSheen(
            s, subjMask, { cyan, rose, lime, white }, 0.95f, 62.0f,
            float(seed % 628) / 100.0f);
        QImage sheenFull = detail::stageCanvas(stage);
        drawAt(sheenFull, sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY = 0.22f;
        sheenM.opacityHold = 0.16f;
        add(QStringLiteral("Voltage sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN, 0.85f, sheenM);

        CreativeMotion rimIceM;
        rimIceM.opacityX = -0.24f;
        rimIceM.opacityY = 0.14f;
        band(0.009, cyan, QStringLiteral("Voltage rim ice"),
             QStringLiteral("rim"), 0.90f, BLEND_SCREEN, rimIceM);
        CreativeMotion rimRoseM;
        rimRoseM.opacityX = 0.24f;
        rimRoseM.opacityY = 0.14f;
        band(0.016, rose, QStringLiteral("Voltage rim rose"),
             QStringLiteral("rim"), 0.80f, BLEND_SCREEN, rimRoseM);
        /* directional light: each rim owns one side of the silhouette */
        if (pieces.size() >= 2)
        {
            detail::fadeHorizontally(pieces[pieces.size() - 2].image, true);
            detail::fadeHorizontally(pieces[pieces.size() - 1].image, false);
        }
        if (cancelled)
            return pieces;

        /* bolts land on the real silhouette, not on its bounding box */
        QImage subjectOnStage = detail::stageCanvas(stage);
        drawAt(subjectOnStage, s, subjectRect);
        const QImage stageMask = creative::maskFromAlpha(subjectOnStage);
        int leftX = stage.width(), rightX = -1;
        int leftY = int(subjectRect.center().y());
        int rightY = leftY;
        for (int y = 0; y < stageMask.height(); y += 2)
        {
            const uchar *row = stageMask.constScanLine(y);
            for (int x = 0; x < stageMask.width(); x += 2)
            {
                if (row[x] < 40)
                    continue;
                if (x < leftX) { leftX = x; leftY = y; }
                if (x > rightX) { rightX = x; rightY = y; }
            }
        }
        const QPointF targetL =
            rightX >= 0 ? QPointF(leftX + W * 0.004, leftY)
                        : QPointF(subjectRect.x()
                                      + subjectRect.width() * 0.14,
                                  subjectRect.y()
                                      + subjectRect.height() * 0.40);
        const QPointF targetR =
            rightX >= 0 ? QPointF(rightX - W * 0.004, rightY)
                        : QPointF(subjectRect.right()
                                      - subjectRect.width() * 0.10,
                                  subjectRect.y()
                                      + subjectRect.height() * 0.56);

        /* two real bolts, seeded so replay and commit match */
        creative::ArcParams arc;
        arc.halo = cyan;
        arc.core = white;
        arc.glowWidth = qMax(5.0f, float(H) * 0.016f);
        arc.coreWidth = qMax(1.2f, float(H) * 0.0026f);
        arc.sparkCount = 72;

        CreativeMotion arcM;
        arcM.opacityHold = 0.15f;
        arcM.opacityY = 0.12f;
        add(QStringLiteral("Voltage arc ice"), QStringLiteral("accent"),
            creative::electricArc(stage, QPointF(-W * 0.02, H * 0.12),
                                  targetL, seed + 31, arc),
            full, BLEND_SCREEN, 0.96f, arcM);
        arc.halo = rose;
        arc.core = QColor(255, 240, 250);
        add(QStringLiteral("Voltage arc rose"), QStringLiteral("accent"),
            creative::electricArc(stage, QPointF(W * 1.02, H * 0.20),
                                  targetR, seed + 47, arc),
            full, BLEND_SCREEN, 0.92f, arcM);
        if (cancelled)
            return pieces;

        /* sparks leave the real silhouette, not a random circle */
        CreativeMotion sparkM;
        sparkM.opacityEnergy = 0.35f;
        sparkM.opacityHold = 0.20f;
        add(QStringLiteral("Voltage sparks"), QStringLiteral("accent"),
            creative::sparkField(stageMask, stage, seed + 77,
                                 { cyan, rose, lime, white }, 90, 0.85f),
            full, BLEND_SCREEN, 0.85f, sparkM);

        CreativeMotion starM;
        starM.opacityHold = 0.30f;
        starM.scaleEnergy = 0.08f;
        add(QStringLiteral("Voltage stars"), QStringLiteral("accent"),
            creative::starField(stage, seed + 5, { white, lime, cyan }, 46),
            full, BLEND_SCREEN, 0.72f, starM);
    }
    else if (patchId == QLatin1String("sticker-forge"))
    {
        const QColor paper(255, 248, 236), ink(22, 20, 24), red(255, 59, 78),
            blue(43, 89, 255), yellow(255, 210, 63), mint(72, 224, 176);
        add(QStringLiteral("Sticker sheet"), QStringLiteral("backdrop"),
            creative::gradientField(stage, paper, QColor(255, 212, 208),
                                    QColor(255, 255, 255, 170), 0.18),
            full);
        add(QStringLiteral("Sticker grain"), QStringLiteral("backdrop"),
            creative::halftoneField(stage, QColor(30, 25, 35, 26),
                                    H * 0.030, H * 0.0032, 45, 0.0),
            full, BLEND_MULTIPLY, 0.8f);
        if (cancelled)
            return pieces;

        QImage card = detail::stageCanvas(stage);
        {
            QPainter p(&card);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 238));
            p.translate(subjectRect.center());
            p.rotate(-2.0);
            const QRectF r = detail::scaleRectAbout(
                QRectF(-subjectRect.width() / 2.0, -subjectRect.height() / 2.0,
                       subjectRect.width(), subjectRect.height()),
                1.06, 0, H * 0.018);
            p.drawRoundedRect(r, H * 0.03, H * 0.03);
        }
        add(QStringLiteral("Sticker backing"), QStringLiteral("backdrop"),
            card, full, BLEND_NORMAL, 0.85f);

        QImage shadowSubj = creative::softBlur(solid(s, ink), 6);
        QImage shadowFull = detail::stageCanvas(stage);
        drawAt(shadowFull, shadowSubj, subjectRect.translated(0, H * 0.012));
        add(QStringLiteral("Sticker shadow"), QStringLiteral("echo"),
            shadowFull, full, BLEND_NORMAL, 0.30f);
        if (cancelled)
            return pieces;

        CreativeMotion cutM;
        cutM.scaleY = 0.10f;
        cutM.scaleHold = 0.05f;
        band(0.020, QColor(255, 255, 255), QStringLiteral("Sticker die-cut"),
             QStringLiteral("rim"), 1.0f, BLEND_NORMAL, cutM);
        CreativeMotion colorM;
        colorM.opacityX = 0.28f;
        colorM.scaleHold = 0.12f;
        band(0.011, red, QStringLiteral("Sticker cut line"),
             QStringLiteral("rim"), 0.9f, BLEND_NORMAL, colorM);
        /* the accent band crossfades red -> blue with X */
        {
            const int radius = qMax(1, int(std::lround(subjectRect.height() * 0.011)));
            QImage bandMask = creative::bandFromMask(subjMask, radius);
            QImage blueBand = detail::stageCanvas(stage);
            drawAt(blueBand, creative::colorizeBand(bandMask, blue, false, seed + 9),
                   subjectRect);
            CreativeMotion blueM;
            blueM.opacityX = -0.28f;
            blueM.scaleHold = 0.12f;
            add(QStringLiteral("Sticker cut line ice"), QStringLiteral("rim"),
                blueBand, full, BLEND_NORMAL, 0.9f, blueM);
        }
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.07f, 1.05f, 1.18f, ink, 0.05f, yellow, 0.10f });
        subjectFull(graded, QStringLiteral("Sticker idol"), 1.0f, CreativeMotion());

        CreativeMotion glossM;
        glossM.opacityY = 0.30f;
        glossM.opacityHold = 0.15f;
        add(QStringLiteral("Sticker gloss"), QStringLiteral("finish"),
            creative::glossOverlay(stage, s, subjectRect, QColor(255, 255, 255)),
            full, BLEND_SCREEN, 0.38f, glossM);
        if (cancelled)
            return pieces;

        CreativeMotion confettiM;
        confettiM.opacityEnergy = 0.30f;
        confettiM.scaleEnergy = 0.10f;
        add(QStringLiteral("Sticker confetti"), QStringLiteral("accent"),
            creative::confettiField(stage, seed, { red, blue, yellow, mint },
                                    70, H * 0.006, H * 0.018),
            full, BLEND_NORMAL, 0.6f, confettiM);
        CreativeMotion starsM;
        starsM.opacityHold = 0.35f;
        add(QStringLiteral("Sticker sparkles"), QStringLiteral("accent"),
            creative::starField(stage, seed + 3, QColor(255, 255, 255), 55),
            full, BLEND_SCREEN, 0.4f, starsM);
    }
    else if (patchId == QLatin1String("pastel-stage"))
    {
        const QColor pink(255, 199, 230), lavender(199, 187, 255),
            mint(207, 247, 232), ink(90, 74, 107);
        add(QStringLiteral("Pastel sky"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(255, 230, 244),
                                    QColor(209, 202, 255),
                                    QColor(255, 255, 255, 190), 0.22),
            full);
        add(QStringLiteral("Pastel air"), QStringLiteral("accent"),
            creative::radialGlow(stage, QPointF(W * 0.72, H * 0.20),
                                 H * 0.52, QColor(255, 255, 255), 0.35f),
            full, BLEND_SCREEN, 0.55f);

        QImage floor = detail::stageCanvas(stage);
        {
            QPainter p(&floor);
            QLinearGradient g(0, H * 0.62, 0, H);
            g.setColorAt(0.0, QColor(255, 218, 238, 210));
            g.setColorAt(1.0, QColor(247, 168, 216, 235));
            p.fillRect(QRectF(0, H * 0.62, W, H * 0.38), g);
            QLinearGradient horizon(0, H * 0.60, 0, H * 0.66);
            horizon.setColorAt(0.0, QColor(255, 255, 255, 0));
            horizon.setColorAt(0.6, QColor(255, 255, 255, 170));
            horizon.setColorAt(1.0, QColor(255, 255, 255, 0));
            p.fillRect(QRectF(0, H * 0.595, W, H * 0.07), horizon);
        }
        add(QStringLiteral("Pastel floor"), QStringLiteral("backdrop"),
            floor, full);
        if (cancelled)
            return pieces;

        QImage mirrored = creative::reflection(s, 0.55, 0.38);
        QImage mirrorFull = detail::stageCanvas(stage);
        drawAt(mirrorFull, mirrored,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        add(QStringLiteral("Pastel reflection"), QStringLiteral("echo"),
            mirrorFull, full, BLEND_NORMAL, 0.55f);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.10f, 0.94f, 0.92f, lavender, 0.16f,
                                QColor(255, 255, 255), 0.12f });
        subjectFull(graded, QStringLiteral("Pastel idol"), 1.0f, CreativeMotion());

        CreativeMotion bloomM;
        bloomM.opacityY = 0.20f;
        bloomM.opacityHold = 0.30f;
        bloomM.scaleHold = 0.07f;
        add(QStringLiteral("Pastel bloom"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.30, QColor(255, 255, 255), 0.45f),
            full, BLEND_SCREEN, 0.5f, bloomM);
        if (cancelled)
            return pieces;

        CreativeMotion confettiM;
        confettiM.opacityEnergy = 0.30f;
        confettiM.scaleEnergy = 0.12f;
        add(QStringLiteral("Pastel confetti"), QStringLiteral("accent"),
            creative::confettiField(stage, seed, { pink, mint, lavender,
                                                   QColor(214, 236, 255) },
                                    60, H * 0.006, H * 0.017),
            full, BLEND_NORMAL, 0.55f, confettiM);
        add(QStringLiteral("Pastel grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 2, 26), full,
            BLEND_OVERLAY, 0.35f);
    }
    else if (patchId == QLatin1String("nocturne"))
    {
        const QColor night(6, 6, 14), violet(122, 77, 255),
            lilac(201, 167, 255), teal(75, 224, 208), silver(234, 230, 255);
        add(QStringLiteral("Nocturne sky"), QStringLiteral("backdrop"),
            creative::gradientField(stage, night, QColor(24, 14, 44),
                                    QColor(82, 52, 160, 110), 0.36),
            full);
        add(QStringLiteral("Nocturne stars"), QStringLiteral("backdrop"),
            creative::starField(stage, seed, silver, 220), full,
            BLEND_SCREEN, 0.75f);
        if (cancelled)
            return pieces;

        CreativeMotion auraM;
        auraM.opacityY = 0.16f;
        auraM.opacityHold = 0.28f;
        auraM.scaleHold = 0.06f;
        add(QStringLiteral("Nocturne aura"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.48, violet, 0.55f),
            full, BLEND_SCREEN, 0.6f, auraM);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 0.94f, 1.18f, 0.88f, QColor(40, 18, 90), 0.34f,
                                silver, 0.12f });
        subjectFull(graded, QStringLiteral("Nocturne figure"), 1.0f,
                    CreativeMotion());
        if (cancelled)
            return pieces;

        CreativeMotion rimVioletM;
        rimVioletM.opacityX = 0.30f;
        rimVioletM.opacityHold = 0.20f;
        band(0.010, lilac, QStringLiteral("Nocturne rim"), QStringLiteral("rim"),
             0.75f, BLEND_SCREEN, rimVioletM);
        CreativeMotion rimTealM;
        rimTealM.opacityX = -0.30f;
        band(0.010, teal, QStringLiteral("Nocturne rim teal"),
             QStringLiteral("rim"), 0.55f, BLEND_SCREEN, rimTealM);
        if (pieces.size() >= 2)
        {
            detail::fadeHorizontally(pieces[pieces.size() - 2].image, true);
            detail::fadeHorizontally(pieces[pieces.size() - 1].image, false);
        }
        if (cancelled)
            return pieces;

        CreativeMotion flightM;
        flightM.rotationEnergy = 18.0f;
        flightM.opacityHold = 0.22f;
        flightM.dxEnergy = 0.012f;
        add(QStringLiteral("Nocturne butterflies"), QStringLiteral("accent"),
            creative::butterflyField(
                stage, seed, { lilac, violet, teal, silver },
                { QPointF(subjectRect.x() + subjectRect.width() * 0.10,
                          subjectRect.y() + subjectRect.height() * 0.18),
                  QPointF(subjectRect.right() - subjectRect.width() * 0.08,
                          subjectRect.y() + subjectRect.height() * 0.36),
                  QPointF(subjectRect.x() + subjectRect.width() * 0.20,
                          subjectRect.bottom() - subjectRect.height() * 0.10) },
                7),
            full, BLEND_NORMAL, 0.8f, flightM);

        add(QStringLiteral("Nocturne petals"), QStringLiteral("accent"),
            creative::confettiField(stage, seed + 5,
                                    { QColor(120, 40, 90), lilac, teal },
                                    34, H * 0.008, H * 0.022),
            full, BLEND_NORMAL, 0.5f);

        add(QStringLiteral("Nocturne grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 9, 22), full,
            BLEND_OVERLAY, 0.3f);
    }

    else if (patchId == QLatin1String("holo-fashion"))
    {
        const QColor deep(4, 16, 26), cyan(0, 229, 255), violet(124, 77, 255),
            pink(255, 61, 218), gold(255, 230, 109), silver(232, 246, 255);

        add(QStringLiteral("Holo night"), QStringLiteral("backdrop"),
            creative::gradientField(stage, deep, QColor(4, 3, 10),
                                    QColor(40, 16, 80), 0.55),
            full);
        CreativeMotion auraM;
        auraM.opacityX = -0.18f;
        auraM.opacityY = 0.20f;
        add(QStringLiteral("Holo aura"), QStringLiteral("accent"),
            creative::radialGlow(
                stage, subjectRect.center(),
                qMax(subjectRect.width(), subjectRect.height()) * 0.66, violet,
                0.52f),
            full, BLEND_SCREEN, 0.70f, auraM);
        if (cancelled)
            return pieces;

        /* the material: hue and chroma move, the fabric's own lightness,
         * texture and shading stay exactly where they were */
        creative::RecolorParams recolor;
        recolor.palette = { cyan, violet, pink, gold };
        recolor.strength = 0.92f;
        recolor.textureKeep = 0.42f;
        recolor.skinGuard = 0.70f;
        recolor.hueDrift = 0.55f;
        recolor.seed = seed;
        QImage holoSubject = creative::recolorMasked(s, subjMask, recolor);
        CreativeMotion holoM;
        holoM.opacityX = 0.08f;
        subjectFull(holoSubject, QStringLiteral("Holo muse"), 1.0f, holoM);
        if (cancelled)
            return pieces;

        QImage sheen = creative::spectralSheen(
            s, subjMask, { cyan, pink, gold, silver }, 1.0f, -28.0f,
            float(seed % 628) / 100.0f);
        QImage sheenFull = detail::stageCanvas(stage);
        drawAt(sheenFull, sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY = 0.30f;
        sheenM.opacityHold = 0.20f;
        add(QStringLiteral("Liquid sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN, 0.92f, sheenM);
        if (cancelled)
            return pieces;

        CreativeMotion chromeM;
        chromeM.opacityY = 0.24f;
        band(0.007, silver, QStringLiteral("Chrome edge"),
             QStringLiteral("rim"), 1.0f, BLEND_SCREEN, chromeM);
        CreativeMotion iridescentM;
        iridescentM.opacityX = -0.26f;
        iridescentM.opacityY = 0.18f;
        band(0.020, pink, QStringLiteral("Chromatic bloom"),
             QStringLiteral("rim"), 0.55f, BLEND_SCREEN, iridescentM);

        QList<QPointF> anchors;
        anchors << QPointF(subjectRect.x() + subjectRect.width() * 0.12,
                           subjectRect.y() + subjectRect.height() * 0.20)
                << QPointF(subjectRect.right() - subjectRect.width() * 0.10,
                           subjectRect.y() + subjectRect.height() * 0.38)
                << QPointF(subjectRect.x() + subjectRect.width() * 0.18,
                           subjectRect.bottom() - subjectRect.height() * 0.14)
                << QPointF(subjectRect.right() - subjectRect.width() * 0.22,
                           subjectRect.y() + subjectRect.height() * 0.62)
                << QPointF(subjectRect.center());
        CreativeMotion flightM;
        flightM.dyY = 0.05f;
        flightM.rotationHold = 12.0f;
        flightM.opacityHold = 0.18f;
        add(QStringLiteral("Holo butterflies"), QStringLiteral("accent"),
            creative::butterflyField(stage, seed,
                                     { pink, cyan, violet, gold }, anchors, 9),
            full, BLEND_SCREEN, 0.90f, flightM);
        if (cancelled)
            return pieces;

        CreativeMotion prismM;
        prismM.opacityEnergy = 0.30f;
        prismM.opacityHold = 0.25f;
        add(QStringLiteral("Prism sparkle"), QStringLiteral("accent"),
            creative::starField(stage, seed + 4, { silver, gold, cyan }, 80),
            full, BLEND_SCREEN, 0.62f, prismM);

        QImage mirror = creative::reflection(s, 0.20, 0.15);
        QImage mirrorFull = detail::stageCanvas(stage);
        drawAt(mirrorFull, mirror,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        CreativeMotion mirrorM;
        mirrorM.opacityY = -0.10f;
        add(QStringLiteral("Holo reflection"), QStringLiteral("echo"),
            mirrorFull, full, BLEND_SCREEN, 0.45f, mirrorM);
    }
    else if (detail::isHoloMaterialPatch(patchId))
    {
        const creative::HoloMaterial material =
            creative::holoMaterialPreset(detail::holoMaterialIdForPatch(patchId));
        const bool automotive = detail::isAutomotivePatch(patchId);
        const QColor deep(4, 16, 26);
        const QColor c0 = material.palette.value(0, QColor(0, 229, 255));
        const QColor c1 = material.palette.value(1, QColor(124, 77, 255));
        const QColor c2 = material.palette.value(2, QColor(255, 61, 218));
        const QColor c3 = material.palette.value(3, QColor(255, 230, 109));

        add(QStringLiteral("Holo night"), QStringLiteral("backdrop"),
            creative::gradientField(stage, deep, QColor(4, 3, 10),
                                    c1.darker(260), 0.50),
            full);
        CreativeMotion auraM;
        auraM.opacityX = -0.18f;
        auraM.opacityY = 0.20f;
        add(QStringLiteral("Material aura"), QStringLiteral("accent"),
            creative::radialGlow(
                stage, subjectRect.center(),
                qMax(subjectRect.width(), subjectRect.height()) * 0.66, c1,
                0.52f),
            full, BLEND_SCREEN, 0.70f, auraM);
        if (cancelled)
            return pieces;

        QImage confidence;
        const QImage normals = creative::estimateNormals(
            s, subjMask, material.normalStrength, &confidence);
        if (cancelled)
            return pieces;
        /* Fashion semantic protection (FASHN garment/hair/skin masks) is a
         * detail-preservation plane, not a second coverage mask: the subject
         * alpha still defines what exists, while protection keeps skin/hair
         * pixels original. Automotive patches style the whole bodywork and
         * never receive the fashion plane. */
        QImage protection;
        if (!automotive && input.protection.size() == s.size())
            protection = input.protection;
        const creative::HoloMaterialResult mat =
            creative::holographicMaterial(
                s, subjMask, normals, material, 1.0f, seed,
                creative::CoverageMode::InheritedCoverage, confidence,
                protection);
        CreativeMotion holoM;
        holoM.opacityX = 0.06f;
        holoM.opacityY = 0.14f;
        subjectFull(mat.albedo, material.name, 1.0f, holoM);
        if (cancelled)
            return pieces;

        QImage sheenFull = detail::stageCanvas(stage);
        drawAt(sheenFull, mat.sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY = 0.30f;
        sheenM.opacityHold = 0.20f;
        add(QStringLiteral("Material sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN, 0.92f, sheenM);
        if (cancelled)
            return pieces;

        CreativeMotion edgeM;
        edgeM.opacityY = 0.24f;
        band(0.007, c3, QStringLiteral("Material edge"),
             QStringLiteral("rim"), 0.90f, BLEND_SCREEN, edgeM);
        CreativeMotion bloomM;
        bloomM.opacityX = -0.24f;
        bloomM.opacityY = 0.18f;
        band(0.020, c2, QStringLiteral("Material bloom"),
             QStringLiteral("rim"), 0.50f, BLEND_SCREEN, bloomM);

        if (!automotive)
        {
            QList<QPointF> anchors;
            anchors << QPointF(subjectRect.x() + subjectRect.width() * 0.12,
                               subjectRect.y() + subjectRect.height() * 0.20)
                    << QPointF(subjectRect.right()
                                   - subjectRect.width() * 0.10,
                               subjectRect.y() + subjectRect.height() * 0.38)
                    << QPointF(subjectRect.x() + subjectRect.width() * 0.18,
                               subjectRect.bottom()
                                   - subjectRect.height() * 0.14)
                    << QPointF(subjectRect.right()
                                   - subjectRect.width() * 0.22,
                               subjectRect.y() + subjectRect.height() * 0.62)
                    << QPointF(subjectRect.center());
            CreativeMotion flightM;
            flightM.dyY = 0.05f;
            flightM.rotationHold = 12.0f;
            flightM.opacityHold = 0.18f;
            add(QStringLiteral("Material butterflies"),
                QStringLiteral("accent"),
                creative::butterflyField(stage, seed, { c2, c0, c1, c3 },
                                         anchors, 9),
                full, BLEND_SCREEN, 0.90f, flightM);
            CreativeMotion sparkleM;
            sparkleM.opacityEnergy = 0.30f;
            sparkleM.opacityHold = 0.25f;
            add(QStringLiteral("Material sparkle"), QStringLiteral("accent"),
                creative::starField(stage, seed + 4, { c3, c0, c2 }, 70),
                full, BLEND_SCREEN, 0.62f, sparkleM);
        }
        else if (detail::holoMaterialIdForPatch(patchId)
                 == creative::HoloMaterialId::GlitterMetallic)
        {
            CreativeMotion flakeM;
            flakeM.opacityEnergy = 0.25f;
            add(QStringLiteral("Glitter sparkle"), QStringLiteral("accent"),
                creative::starField(stage, seed + 4, { c3, c0 }, 55),
                full, BLEND_SCREEN, 0.55f, flakeM);
        }
        if (cancelled)
            return pieces;

        QImage mirror = creative::reflection(s, 0.20, 0.15);
        QImage mirrorFull = detail::stageCanvas(stage);
        drawAt(mirrorFull, mirror,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        CreativeMotion mirrorM;
        mirrorM.opacityY = -0.10f;
        add(QStringLiteral("Material reflection"), QStringLiteral("echo"),
            mirrorFull, full, BLEND_SCREEN, 0.45f, mirrorM);
    }
    else if (patchId == QLatin1String("ink-creature"))
    {
        const QColor paper(246, 240, 228), ink(20, 18, 16), red(214, 69, 69);
        add(QStringLiteral("Ink paper"), QStringLiteral("backdrop"),
            creative::gradientField(stage, paper, QColor(235, 226, 208),
                                    QColor(255, 255, 255, 150), 0.25),
            full);
        add(QStringLiteral("Ink wash"), QStringLiteral("echo"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.42, QColor(40, 34, 28), 0.28f),
            full, BLEND_MULTIPLY, 0.5f);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.02f, 1.30f, 0.35f, ink, 0.20f,
                                QColor(255, 255, 255), 0.05f });
        subjectFull(graded, QStringLiteral("Ink figure"), 1.0f, CreativeMotion());
        if (cancelled)
            return pieces;

        CreativeMotion lineM;
        lineM.opacityX = 0.20f;
        lineM.opacityEnergy = 0.25f;
        band(0.006, ink, QStringLiteral("Ink line"), QStringLiteral("rim"),
             0.9f, BLEND_NORMAL, lineM);
        CreativeMotion lineSoftM;
        lineSoftM.opacityX = -0.20f;
        band(0.014, QColor(20, 18, 16, 120), QStringLiteral("Ink halo"),
             QStringLiteral("rim"), 0.35f, BLEND_NORMAL, lineSoftM);
        if (cancelled)
            return pieces;

        CreativeMotion splatM;
        splatM.opacityEnergy = 0.35f;
        splatM.scaleEnergy = 0.15f;
        add(QStringLiteral("Ink splatter"), QStringLiteral("accent"),
            creative::inkSplatter(stage, seed,
                                  subjectRect.adjusted(-W * 0.12, -H * 0.10,
                                                       W * 0.12, H * 0.10),
                                  ink, 80),
            full, BLEND_NORMAL, 0.7f, splatM);

        QImage eyes = detail::stageCanvas(stage);
        {
            QPainter p(&eyes);
            p.setRenderHint(QPainter::Antialiasing, true);
            const qreal er = H * 0.024;
            const QPointF left(subjectRect.x() + subjectRect.width() * 0.30,
                               subjectRect.y() + subjectRect.height() * 0.24);
            const QPointF right(subjectRect.x() + subjectRect.width() * 0.63,
                                subjectRect.y() + subjectRect.height() * 0.22);
            for (const QPointF &c : { left, right })
            {
                QRadialGradient glow(c, er * 2.6);
                glow.setColorAt(0.0, QColor(214, 69, 69, 150));
                glow.setColorAt(1.0, QColor(214, 69, 69, 0));
                p.setPen(Qt::NoPen);
                p.setBrush(glow);
                p.drawEllipse(c, er * 2.6, er * 2.6);
                p.setBrush(QColor(252, 248, 238, 245));
                p.setPen(QPen(QColor(20, 18, 16), qMax<qreal>(1.0, er * 0.18)));
                p.drawEllipse(c, er, er * 1.15);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(20, 18, 16));
                p.drawEllipse(c + QPointF(er * 0.15, 0), er * 0.42, er * 0.55);
                p.setBrush(QColor(255, 255, 255, 220));
                p.drawEllipse(c + QPointF(-er * 0.28, -er * 0.35), er * 0.16,
                              er * 0.16);
            }
        }
        CreativeMotion eyesM;
        eyesM.opacityY = 0.15f;
        eyesM.opacityHold = 0.55f;
        add(QStringLiteral("Ink creature eyes"), QStringLiteral("accent"),
            eyes, full, BLEND_NORMAL, 0.35f, eyesM);
        add(QStringLiteral("Ink grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 4, 34), full,
            BLEND_MULTIPLY, 0.4f);
    }
    else if (patchId == QLatin1String("spark-storm"))
    {
        const QColor night(7, 6, 13), gold(255, 211, 122), pink(255, 122, 200),
            cyan(122, 240, 255), white(255, 250, 240);
        add(QStringLiteral("Storm sky"), QStringLiteral("backdrop"),
            creative::gradientField(stage, night, QColor(24, 12, 40),
                                    QColor(70, 30, 110, 120), 0.30),
            full);
        add(QStringLiteral("Storm stars"), QStringLiteral("backdrop"),
            creative::starField(stage, seed, white, 150), full,
            BLEND_SCREEN, 0.6f);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.05f, 1.10f, 1.12f, QColor(40, 20, 80), 0.18f,
                                gold, 0.10f });
        subjectFull(graded, QStringLiteral("Storm figure"), 1.0f,
                    CreativeMotion());
        if (cancelled)
            return pieces;

        CreativeMotion goldRimM;
        goldRimM.opacityHold = 0.25f;
        band(0.010, gold, QStringLiteral("Storm rim gold"), QStringLiteral("rim"),
             0.8f, BLEND_SCREEN, goldRimM);

        const QPointF center = subjectRect.center();
        const qreal radius = qMax(subjectRect.width(), subjectRect.height()) * 0.68;
        CreativeMotion goldSpiralM;
        goldSpiralM.rotationEnergy = 42.0f;
        goldSpiralM.opacityHold = 0.20f;
        goldSpiralM.scaleEnergy = 0.10f;
        add(QStringLiteral("Storm spiral gold"), QStringLiteral("accent"),
            creative::spiralField(stage, center, radius, seed,
                                  { gold, pink, white }, 260),
            full, BLEND_SCREEN, 0.75f, goldSpiralM);
        if (cancelled)
            return pieces;
        CreativeMotion iceSpiralM;
        iceSpiralM.rotationEnergy = -34.0f;
        iceSpiralM.opacityX = 0.30f;
        add(QStringLiteral("Storm spiral ice"), QStringLiteral("accent"),
            creative::spiralField(stage, center, radius * 0.82, seed + 7,
                                  { cyan, white, QColor(170, 140, 255) }, 190),
            full, BLEND_SCREEN, 0.55f, iceSpiralM);
        add(QStringLiteral("Storm dust"), QStringLiteral("finish"),
            creative::bokehField(stage, seed + 2, { gold, pink, cyan }, 70,
                                 H * 0.004, H * 0.014),
            full, BLEND_SCREEN, 0.5f);
    }
    else if (patchId == QLatin1String("kaleido"))
    {
        const QColor navy(11, 16, 48), cyan(88, 224, 255), magenta(255, 94, 219),
            lime(200, 255, 106);
        add(QStringLiteral("Kaleido night"), QStringLiteral("backdrop"),
            creative::gradientField(stage, navy, QColor(24, 8, 46),
                                    QColor(60, 30, 140, 100), 0.5),
            full);
        if (cancelled)
            return pieces;
        QImage subjectCanvas = detail::stageCanvas(stage);
        drawAt(subjectCanvas, s, subjectRect);
        CreativeMotion k6M;
        k6M.rotationEnergy = 24.0f;
        k6M.opacityX = 0.15f;
        add(QStringLiteral("Kaleido six"), QStringLiteral("subject"),
            creative::wedgeKaleidoscope(subjectCanvas, stage, 6, 1.0, 0.0),
            full, BLEND_NORMAL, 0.9f, k6M);
        if (cancelled)
            return pieces;
        CreativeMotion k8M;
        k8M.rotationEnergy = -18.0f;
        k8M.opacityX = -0.15f;
        k8M.opacityHold = 0.25f;
        add(QStringLiteral("Kaleido eight"), QStringLiteral("subject"),
            creative::wedgeKaleidoscope(subjectCanvas, stage, 8, 1.22, 22.0),
            full, BLEND_SCREEN, 0.45f, k8M);
        for (CreativePiece &kp : pieces)
            if (kp.role == QLatin1String("subject"))
                kp.pivot = full.center();
        if (cancelled)
            return pieces;
        add(QStringLiteral("Kaleido ring"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(), H * 0.55,
                                 magenta, 0.30f),
            full, BLEND_SCREEN, 0.6f);
        add(QStringLiteral("Kaleido stars"), QStringLiteral("finish"),
            creative::starField(stage, seed + 4, cyan, 90), full,
            BLEND_SCREEN, 0.6f);
    }
    else if (patchId == QLatin1String("motion-echo"))
    {
        const QColor night(18, 19, 38), blue(78, 123, 255), pink(255, 111, 174);
        add(QStringLiteral("Echo stage"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(9, 9, 20), night,
                                    QColor(90, 60, 160, 90), 0.45),
            full);
        if (cancelled)
            return pieces;
        const qreal spacing = subjectRect.width() * 0.26;
        for (int i = 0; i < 6; ++i)
        {
            if (cancelled)
                return pieces;
            const int k = i - 3;
            if (k == 0)
                continue;
            QImage echo = creative::hueShift(s, k * 14);
            echo = creative::gradeImage(
                echo, creative::Grade{ 0.92f, 1.08f, 0.9f,
                                       k < 0 ? blue : pink, 0.22f,
                                       QColor(255, 255, 255), 0.05f });
            QImage echoFull = detail::stageCanvas(stage);
            drawAt(echoFull, echo,
                   subjectRect.translated(k * spacing, k * spacing * 0.05));
            CreativeMotion echoM;
            echoM.opacityX = 0.06f;
            echoM.dxX = 0.012f * k;
            add(QStringLiteral("Echo %1").arg(k > 0 ? k : -k),
                QStringLiteral("echo"), echoFull, full, BLEND_NORMAL,
                float(0.48 - qAbs(k) * 0.070), echoM);
        }
        if (cancelled)
            return pieces;
        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.03f, 1.10f, 1.14f, blue, 0.14f, pink, 0.10f });
        subjectFull(graded, QStringLiteral("Echo figure"), 1.0f, CreativeMotion());
        CreativeMotion trailM;
        trailM.opacityEnergy = 0.35f;
        add(QStringLiteral("Echo trail"), QStringLiteral("accent"),
            creative::bokehField(stage, seed, { blue, pink,
                                                QColor(255, 255, 255) },
                                 80, H * 0.004, H * 0.016),
            full, BLEND_SCREEN, 0.45f, trailM);
        add(QStringLiteral("Echo grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 3, 20), full,
            BLEND_OVERLAY, 0.3f);
    }
    else
    {
        /* Unknown/empty patch: keep the stage honest. */
        add(QStringLiteral("Stage"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(18, 18, 26),
                                    QColor(8, 8, 12), QColor(80, 60, 160, 90),
                                    0.4),
            full);
        subjectFull(s, QStringLiteral("Figure"), 1.0f, CreativeMotion());
    }

    return pieces;
}

/* --------------------------------------------------------------- playback */

/* Bake the captured pad state into the piece geometry. Deterministic: the
 * frame is a value, never read from the live pad. Used by commits so the
 * layers store exactly what the user saw. */
inline QVector<CreativePiece> bake(QVector<CreativePiece> pieces,
                                   const CreativeFrame &frame,
                                   const QSize &stage)
{
    for (CreativePiece &p : pieces)
    {
        const CreativeMotion &m = p.motion;
        const float op = clamp01(p.opacity
                                 + m.opacityX * frame.x
                                 + m.opacityY * frame.y
                                 + m.opacityEnergy * frame.energy
                                 + m.opacityHold * (frame.hold ? 1.0f : 0.0f));
        const qreal dx = (m.dxX * (frame.x - 0.5f)
                          + m.dxEnergy * frame.energy) * stage.width();
        const qreal dy = (m.dyY * (frame.y - 0.5f)
                          + m.dyEnergy * frame.energy) * stage.height();
        const qreal sc = 1.0 + m.scaleY * (frame.y - 0.5f)
                         + m.scaleEnergy * frame.energy
                         + m.scaleHold * (frame.hold ? 1.0 : 0.0);
        const qreal rot = m.rotationEnergy * frame.energy
                          + m.rotationHold * (frame.hold ? 1.0 : 0.0);

        p.opacity = op;
        if (qFuzzyIsNull(dx) && qFuzzyIsNull(dy) && qFuzzyIsNull(sc - 1.0)
            && qFuzzyIsNull(rot))
            continue;

        const QPointF pivot = p.pivot.isNull() ? p.rect.center() : p.pivot;
        if (qFuzzyIsNull(sc - 1.0) && qFuzzyIsNull(rot))
        {
            p.rect.translate(dx, dy);
            continue;
        }
        QTransform t;
        t.rotate(rot);
        t.scale(sc, sc);
        const QRectF local(0, 0, p.image.width(), p.image.height());
        const QRectF mapped = t.mapRect(local);
        const QSize outSize(qMax(1, int(std::ceil(mapped.width()))),
                            qMax(1, int(std::ceil(mapped.height()))));
        QImage out(outSize, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter painter(&out);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.translate(-mapped.x(), -mapped.y());
        painter.setTransform(t, true);
        painter.drawImage(0, 0, p.image);
        painter.end();
        const QPointF pivotOut = t.map(pivot) - mapped.topLeft();
        const QPointF target = pivot + QPointF(dx, dy);
        p.rect = QRectF(target.x() - pivotOut.x(), target.y() - pivotOut.y(),
                        outSize.width(), outSize.height());
        p.pivot = target;
        p.image = out;
    }
    return pieces;
}

/* Compose pieces into a stage (or a bounded preview of a larger stage). The
 * pieces keep their global stage coordinates; a preview is a scaled render,
 * never a re-seeded/re-tiled recipe. */
inline QImage compose(const QVector<CreativePiece> &pieces,
                      const QSize &stage, const QSize &target,
                      const QColor &background, const CreativeFrame &frame)
{
    if (!target.isValid() || target.isEmpty())
        return {};
    QImage out(target, QImage::Format_ARGB32_Premultiplied);
    out.fill(background);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (stage != target && stage.isValid() && !stage.isEmpty())
        p.scale(double(target.width()) / double(stage.width()),
                double(target.height()) / double(stage.height()));
    const int count = pieces.size();
    for (int i = 0; i < count; ++i)
    {
        const CreativePiece &piece = pieces.at(i);
        if (piece.image.isNull())
            continue;
        const CreativeMotion &m = piece.motion;
        float op = piece.opacity
                   + m.opacityX * frame.x
                   + m.opacityY * frame.y
                   + m.opacityEnergy * frame.energy
                   + m.opacityHold * (frame.hold ? 1.0f : 0.0f);
        if (piece.role == QLatin1String("rim")
            || piece.role == QLatin1String("accent"))
            op *= 1.0f + 0.035f * float(std::sin(frame.time * 1.8 + i * 1.3));
        op = clamp01(op);
        if (op <= 0.002f)
            continue;

        const qreal dx = (m.dxX * (frame.x - 0.5f)
                          + m.dxEnergy * frame.energy) * stage.width();
        const qreal dy = (m.dyY * (frame.y - 0.5f)
                          + m.dyEnergy * frame.energy) * stage.height();
        const qreal sc = 1.0 + m.scaleY * (frame.y - 0.5f)
                         + m.scaleEnergy * frame.energy
                         + m.scaleHold * (frame.hold ? 1.0 : 0.0);
        const qreal rot = m.rotationEnergy * frame.energy
                          + m.rotationHold * (frame.hold ? 1.0 : 0.0);

        p.setOpacity(op);
        p.setCompositionMode(toCompositionMode(piece.blendMode));
        p.save();
        const QPointF pivot = piece.pivot.isNull() ? piece.rect.center()
                                                   : piece.pivot;
        p.translate(pivot.x() + dx, pivot.y() + dy);
        if (!qFuzzyIsNull(rot))
            p.rotate(rot);
        if (!qFuzzyIsNull(sc - 1.0))
            p.scale(sc, sc);
        p.translate(-pivot.x(), -pivot.y());
        p.drawImage(piece.rect, piece.image);
        p.restore();
    }
    p.end();
    return out;
}

} /* namespace recipe */
} /* namespace creative */
} /* namespace paintbrush */

#endif /* PB_CREATIVE_RECIPE_HPP */

/*****************************************************************************
 * pb_creative.cpp : NAI Paintbrush — Creative Play Pad / patch synthesis
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_creative.hpp"
#include "pb_controller.hpp"
#include "pb_document.hpp"
#include "pb_creative_art.hpp"
#include "pb_creative_recipe.hpp"
#include "pb_stage.hpp"
#include "art/style/typography_style.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QTransform>
#include <QtMath>
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFutureWatcher>
#include <QPromise>
#include <QRandomGenerator>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>
#include <QTransform>
#include <QtConcurrent/QtConcurrentRun>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <random>
#include <memory>
#include <utility>

namespace paintbrush {

namespace {

using creative::recipe::clamp01;
using creative::recipe::detail::kTau;
using creative::recipe::detail::stageCanvas;
using creative::recipe::detail::drawScaled;
using creative::recipe::detail::tintPreserveAlpha;
using creative::recipe::detail::scaleRectAbout;
using creative::recipe::detail::fadeHorizontally;

QString qcolorHex(const QColor &c)
{
    return c.isValid() ? c.name(QColor::HexRgb) : QString();
}

QString suggestionKindFor(const QString &patchId)
{
    if (patchId == QLatin1String("sticker-forge")
        || patchId == QLatin1String("spark-storm"))
        return QStringLiteral("sparkle");
    if (patchId == QLatin1String("nocturne")
        || patchId == QLatin1String("kaleido")
        || patchId == QLatin1String("stage-mode"))
        return QStringLiteral("halo");
    if (patchId == QLatin1String("ink-creature"))
        return QStringLiteral("calligraphy");
    if (patchId == QLatin1String("auto-splatter"))
        return QStringLiteral("splat");
    return QStringLiteral("ribbon");
}

/* The material looks are one engine with surface-specific recipes (see
 * pb_creative_art.hpp). The patch id selects a preset; it never selects a
 * second renderer. */
bool isHoloMaterialPatch(const QString &patchId)
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

bool isAutomotivePatch(const QString &patchId)
{
    return patchId == QLatin1String("holo-wrap")
           || patchId == QLatin1String("holo-chameleon")
           || patchId == QLatin1String("holo-glitter")
           || patchId == QLatin1String("holo-livery");
}

creative::HoloMaterialId holoMaterialIdForPatch(const QString &patchId)
{
    if (patchId == QLatin1String("holo-opal"))
        return creative::HoloMaterialId::OpalDream;
    if (patchId == QLatin1String("holo-chrome"))
        return creative::HoloMaterialId::ChromaticChrome;
    if (patchId == QLatin1String("holo-prism"))
        return creative::HoloMaterialId::PrismaticFantasy;
    if (patchId == QLatin1String("holo-wrap"))
        return creative::HoloMaterialId::HolographicWrap;
    if (patchId == QLatin1String("holo-chameleon"))
        return creative::HoloMaterialId::ChameleonPaint;
    if (patchId == QLatin1String("holo-glitter"))
        return creative::HoloMaterialId::GlitterMetallic;
    if (patchId == QLatin1String("holo-livery"))
        return creative::HoloMaterialId::ElectricLivery;
    return creative::HoloMaterialId::LiquidHologram;
}

QColor stageLightColor(const QString &patchId)
{
    if (patchId == QLatin1String("neon-idol")
        || patchId == QLatin1String("electric-kpop")
        || patchId == QLatin1String("kaleido")
        || patchId == QLatin1String("motion-echo"))
        return QColor(130, 232, 255);
    if (patchId == QLatin1String("sticker-forge"))
        return QColor(255, 205, 90);
    if (patchId == QLatin1String("pastel-stage"))
        return QColor(180, 150, 255);
    if (patchId == QLatin1String("scene-forge")
        || patchId == QLatin1String("stage-mode")
        || patchId == QLatin1String("paper-theatre"))
        return QColor(255, 236, 196);
    if (patchId == QLatin1String("nocturne")
        || patchId == QLatin1String("holo-fashion"))
        return QColor(205, 160, 255);
    if (patchId == QLatin1String("holo-liquid")
        || patchId == QLatin1String("holo-opal"))
        return QColor(205, 160, 255);
    if (patchId == QLatin1String("holo-chrome"))
        return QColor(232, 246, 255);
    if (patchId == QLatin1String("holo-prism"))
        return QColor(255, 61, 218);
    if (patchId == QLatin1String("holo-wrap"))
        return QColor(0, 140, 255);
    if (patchId == QLatin1String("holo-chameleon"))
        return QColor(0, 195, 135);
    if (patchId == QLatin1String("holo-glitter"))
        return QColor(255, 224, 130);
    if (patchId == QLatin1String("holo-livery"))
        return QColor(0, 234, 255);
    if (patchId == QLatin1String("ink-creature"))
        return QColor(255, 230, 180);
    if (patchId == QLatin1String("auto-splatter"))
        return QColor(255, 194, 164);
    if (patchId == QLatin1String("spark-storm"))
        return QColor(255, 216, 142);
    return QColor(255, 244, 230);
}

/* Convert built pieces into publication-ready layers. Pure (no controller or
 * document access), so it runs on the worker. Trimming happens here once; the
 * same pxScale mapping the synchronous commit used is preserved. */
QList<CanvasLayer> creativeLayersFromPieces(const QVector<CreativePiece> &pieces,
                                            const CreativeFrame &frame,
                                            const QString &patchId,
                                            const QString &patchName,
                                            quint32 seed,
                                            const QString &group,
                                            int sourceLayerId,
                                            int sourceHarvestId)
{
    QList<CanvasLayer> layers;
    for (const CreativePiece &p : pieces)
    {
        if (p.image.isNull())
            continue;
        QPoint offset;
        QImage trimmed = creative::trimTransparent(p.image, &offset);
        if (trimmed.isNull())
            continue;
        const qreal pxScale = p.rect.width() / qMax(1, p.image.width());
        const QPointF target =
            p.rect.topLeft() + QPointF(offset) * pxScale;
        CanvasLayer layer =
            CanvasLayer::createImage(trimmed, QPointF(0, 0), p.name);
        layer.setTransform(QTransform(pxScale, 0.0, 0.0, pxScale, target.x(),
                                      target.y()));
        layer.type = LAYER_IMAGE;
        layer.opacity = p.opacity;
        layer.blendMode = p.blendMode;
        layer.metadata[QStringLiteral("creative.patch")] = patchId;
        layer.metadata[QStringLiteral("creative.patchName")] = patchName;
        layer.metadata[QStringLiteral("creative.role")] = p.role;
        layer.metadata[QStringLiteral("creative.seed")] = qint64(seed);
        layer.metadata[QStringLiteral("creative.group")] = group;
        layer.metadata[QStringLiteral("creative.sourceLayerId")] = sourceLayerId;
        layer.metadata[QStringLiteral("creative.sourceHarvestId")] =
            sourceHarvestId;
        QVariantMap params;
        params[QStringLiteral("x")] = double(frame.x);
        params[QStringLiteral("y")] = double(frame.y);
        params[QStringLiteral("energy")] = double(frame.energy);
        params[QStringLiteral("hold")] = frame.hold;
        layer.metadata[QStringLiteral("creative.params")] = params;
        /* Scene props remain self-describing after save/reopen. */
        for (const QString &key : { QStringLiteral("creative.prop"),
                                    QStringLiteral("creative.depth"),
                                    QStringLiteral("creative.parallax") })
            if (p.metadata.contains(key))
                layer.metadata.insert(key, p.metadata.value(key));
        if (p.role == QLatin1String("powder"))
        {
            layer.materialPreset =
                p.metadata.value(QStringLiteral("powderPreset")).toString();
            layer.materialState =
                p.metadata.value(QStringLiteral("powderState")).toByteArray();
        }
        layers.append(layer);
    }
    return layers;
}

/* One cancellable full-resolution build. Runs on the dedicated build pool.
 * It only reads the immutable snapshot and the atomic cancel token; previews
 * and the finished layers are streamed through the promise to the GUI thread. */
void runCreativeCommitJob(const CreativeBuildInput &input,
                          const std::shared_ptr<CreativeCancelToken> &token,
                          quint64 revision,
                          QPromise<CreativeCommitEvent> &promise)
{
    const auto cancelled = [&]() {
        return (token && token->isCancelled()) || promise.isCanceled();
    };

    CreativeCommitEvent finished;
    finished.kind = CreativeCommitEvent::Finished;
    finished.revision = revision;

    const auto finishCancelled = [&]() {
        finished.result.cancelled = true;
        promise.setProgressValue(100);
        promise.addResult(finished);
    };

    promise.setProgressRange(0, 100);
    if (cancelled())
    {
        finishCancelled();
        return;
    }

    QImage subject = input.subject;
    if (subject.isNull())
    {
        finished.result.error = QStringLiteral("The cutout is empty.");
        promise.addResult(finished);
        return;
    }
    /* Sampling above 1.25x the on-canvas size adds no visible detail at the
     * target resolution but would multiply memory and time. */
    const QSize subjectWant(
        qMax(1, int(std::lround(input.subjectRect.width() * 1.25))),
        qMax(1, int(std::lround(input.subjectRect.height() * 1.25))));
    if (subject.width() > subjectWant.width()
        || subject.height() > subjectWant.height())
        subject = subject.scaled(subjectWant, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);

    const QColor background(12, 12, 18);
    const int expected = creative::recipe::expectedPieceCount(input.patchId);
    QElapsedTimer previewClock;
    previewClock.start();
    int previews = 0;
    int progressPercent = 0;

    /* Progressive preview: compose a bounded preview from the pieces already
     * built. Bounded to four updates and >=400 ms apart so a full-resolution
     * build never turns progress reporting into its own workload. */
    const auto onPiece = [&](const QVector<CreativePiece> &pieces,
                             int count) -> bool {
        if (cancelled())
            return false;
        progressPercent = qBound(0, (count * 90) / qMax(1, expected), 90);
        promise.setProgressValue(progressPercent);
        if (previews < 4 && count < expected && input.previewSize.isValid()
            && previewClock.elapsed() >= 400)
        {
            CreativeCommitEvent progressEvent;
            progressEvent.kind = CreativeCommitEvent::Progress;
            progressEvent.revision = revision;
            progressEvent.percent = progressPercent;
            progressEvent.preview = creative::recipe::compose(
                pieces, input.stage, input.previewSize, background, input.frame);
            if (!progressEvent.preview.isNull())
            {
                promise.addResult(progressEvent);
                ++previews;
            }
            previewClock.restart();
        }
        return true;
    };

    QVector<CreativePiece> pieces = creative::recipe::build(
        input, onPiece);
    if (cancelled())
    {
        finishCancelled();
        return;
    }

    promise.setProgressValue(92);
    pieces = creative::recipe::bake(std::move(pieces), input.frame, input.stage);
    if (cancelled())
    {
        finishCancelled();
        return;
    }

    CreativeBuildResult result;
    result.layers = creativeLayersFromPieces(
        pieces, input.frame, input.patchId, input.patchName, input.seed,
        input.group, input.sourceLayerId, input.sourceHarvestId);
    if (cancelled())
    {
        finishCancelled();
        return;
    }
    result.preview = creative::recipe::compose(pieces, input.stage,
                                               input.previewSize, background,
                                               input.frame);
    if (result.layers.isEmpty())
        result.error = QStringLiteral("The patch produced no layers.");

    finished.result = std::move(result);
    finished.percent = 100;
    promise.setProgressValue(100);
    promise.addResult(finished);
}

} /* namespace */

/* ------------------------------------------------------------- image provider */

CreativePreviewProvider::CreativePreviewProvider(PaintbrushCreative *engine)
    : QQuickImageProvider(QQuickImageProvider::Image), m_engine(engine)
{
}

QImage CreativePreviewProvider::requestImage(const QString &id, QSize *size,
                                             const QSize &requestedSize)
{
    Q_UNUSED(id);
    QImage img = m_engine ? m_engine->previewImage() : QImage();
    if (img.isNull())
        return {};
    if (size)
        *size = img.size();
    if (requestedSize.isValid() && requestedSize.width() > 0
        && requestedSize.height() > 0
        && requestedSize != img.size())
        img = img.scaled(requestedSize, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    return img;
}

/* --------------------------------------------------------------- the catalog */

const QVector<PaintbrushCreative::PatchDef> &PaintbrushCreative::catalog()
{
    static const QVector<PatchDef> defs = {
        { QStringLiteral("neon-idol"), QStringLiteral("Neon Idol"),
          QStringLiteral("Electric stage. Idol light."),
          QStringLiteral("Stage"),
          QStringLiteral("Drag for heat, lift for spotlight, hold for bloom."),
          QStringLiteral("Add one hand-drawn sparkle where the light hits."),
          QColor(90, 235, 255), QColor(255, 80, 160), QColor(10, 10, 24) },
        { QStringLiteral("electric-kpop"), QStringLiteral("Voltage Idol"),
          QStringLiteral("Main-character voltage."),
          QStringLiteral("Stage"),
          QStringLiteral("Drag for voltage, lift for the drop, hold for the bloom."),
          QStringLiteral("Draw one star where the arc lands."),
          QColor(0, 234, 255), QColor(253, 48, 199), QColor(8, 3, 18) },
        { QStringLiteral("sticker-forge"), QStringLiteral("Sticker Forge"),
          QStringLiteral("Die-cut your own merch."),
          QStringLiteral("Sticker"),
          QStringLiteral("Drag for the cut line, lift for gloss, hold for sparkle."),
          QStringLiteral("Add a tiny star sticker by the shoulder."),
          QColor(255, 210, 63), QColor(255, 59, 78), QColor(22, 20, 24) },
        { QStringLiteral("pastel-stage"), QStringLiteral("Pastel Stage"),
          QStringLiteral("Soft light. Main-character energy."),
          QStringLiteral("Soft"),
          QStringLiteral("Drag through the palette, lift for air, hold for glow."),
          QStringLiteral("Add one confetti ribbon that follows the pose."),
          QColor(255, 199, 230), QColor(199, 187, 255), QColor(90, 74, 107) },
        { QStringLiteral("nocturne"), QStringLiteral("Nocturne"),
          QStringLiteral("Midnight garden, luminous."),
          QStringLiteral("Fantasy"),
          QStringLiteral("Drag between violet and teal, lift for moonlight."),
          QStringLiteral("Add one butterfly resting on the silhouette edge."),
          QColor(122, 77, 255), QColor(75, 224, 208), QColor(10, 10, 22) },
        { QStringLiteral("holo-fashion"), QStringLiteral("Prism Muse"),
          QStringLiteral("Liquid chrome, zero filter."),
          QStringLiteral("Material"),
          QStringLiteral("Drag across the spectrum, lift for chrome, hold for the sheen."),
          QStringLiteral("Give the next pose a different colourway."),
          QColor(255, 61, 218), QColor(0, 229, 255), QColor(4, 16, 26) },
        { QStringLiteral("holo-liquid"), QStringLiteral("Liquid Hologram"),
          QStringLiteral("Liquid light, zero filter."),
          QStringLiteral("Material"),
          QStringLiteral("Drag for flow, lift for the rainbow, hold for the gloss."),
          QStringLiteral("Let one fold keep a pure white highlight."),
          QColor(0, 229, 255), QColor(124, 77, 255), QColor(4, 16, 26) },
        { QStringLiteral("holo-opal"), QStringLiteral("Opal Dream"),
          QStringLiteral("Pearl light, soft future."),
          QStringLiteral("Material"),
          QStringLiteral("Drag for pearl drift, lift for softness."),
          QStringLiteral("Pair it with pastel paper."),
          QColor(184, 242, 230), QColor(255, 214, 224), QColor(4, 16, 26) },
        { QStringLiteral("holo-chrome"), QStringLiteral("Chromatic Chrome"),
          QStringLiteral("Liquid metal, main character."),
          QStringLiteral("Material"),
          QStringLiteral("Drag for the reflection, lift for the shine."),
          QStringLiteral("Add one hard white glint."),
          QColor(200, 205, 215), QColor(110, 140, 190), QColor(4, 16, 26) },
        { QStringLiteral("holo-prism"), QStringLiteral("Prismatic Fantasy"),
          QStringLiteral("Full-spectrum glitter storm."),
          QStringLiteral("Material"),
          QStringLiteral("Drag for diffraction, lift for the sparkle."),
          QStringLiteral("Scatter three more butterflies."),
          QColor(255, 47, 185), QColor(0, 229, 255), QColor(4, 16, 26) },
        { QStringLiteral("holo-wrap"), QStringLiteral("Holographic Wrap"),
          QStringLiteral("The whole body is the material."),
          QStringLiteral("Automotive"),
          QStringLiteral("Drag for the wrap flow, lift for the clearcoat."),
          QStringLiteral("Keep the panel lines readable."),
          QColor(0, 140, 255), QColor(110, 60, 200), QColor(4, 16, 26) },
        { QStringLiteral("holo-chameleon"), QStringLiteral("Chameleon Paint"),
          QStringLiteral("One colour, every angle."),
          QStringLiteral("Automotive"),
          QStringLiteral("Drag to shift the hue, lift for the gloss."),
          QStringLiteral("Let the roofline catch the shift."),
          QColor(0, 190, 130), QColor(80, 30, 150), QColor(4, 16, 26) },
        { QStringLiteral("holo-glitter"), QStringLiteral("Glitter Metallic"),
          QStringLiteral("Metal with a pulse."),
          QStringLiteral("Automotive"),
          QStringLiteral("Drag for the flake, lift for the glare."),
          QStringLiteral("Keep the glass clean."),
          QColor(255, 224, 130), QColor(120, 180, 255), QColor(4, 16, 26) },
        { QStringLiteral("holo-livery"), QStringLiteral("Electric Livery"),
          QStringLiteral("Neon on the bodywork."),
          QStringLiteral("Automotive"),
          QStringLiteral("Drag for the current, lift for the discharge."),
          QStringLiteral("Leave the badges untouched."),
          QColor(0, 234, 255), QColor(255, 0, 180), QColor(2, 2, 8) },
        { QStringLiteral("ink-creature"), QStringLiteral("Ink Creature"),
          QStringLiteral("Your silhouette grew a friend."),
          QStringLiteral("Mischief"),
          QStringLiteral("Drag for ink density, lift for wash, hold to wake it."),
          QStringLiteral("Add one ink splat that breaks the outline."),
          QColor(20, 18, 16), QColor(214, 69, 69), QColor(246, 240, 228) },
        { QStringLiteral("spark-storm"), QStringLiteral("Spark Storm"),
          QStringLiteral("A storm that sparkles."),
          QStringLiteral("Energy"),
          QStringLiteral("Drag to spin the storm, lift for density, hold for gold."),
          QStringLiteral("Add one extra spark ring around the waist."),
          QColor(255, 211, 122), QColor(255, 122, 200), QColor(7, 6, 13) },
        { QStringLiteral("kaleido"), QStringLiteral("Kaleido"),
          QStringLiteral("One pose, infinite angles."),
          QStringLiteral("Weird"),
          QStringLiteral("Drag for symmetry, lift for zoom, hold for bloom."),
          QStringLiteral("Add one mirrored echo in the empty corner."),
          QColor(88, 224, 255), QColor(255, 94, 219), QColor(11, 16, 48) },
        { QStringLiteral("motion-echo"), QStringLiteral("Motion Echo"),
          QStringLiteral("Every move leaves a ghost."),
          QStringLiteral("Motion"),
          QStringLiteral("Drag for spacing, lift for fade, hold to freeze."),
          QStringLiteral("Add one star trail behind the last echo."),
          QColor(78, 123, 255), QColor(255, 111, 174), QColor(18, 19, 38) },
        { QStringLiteral("scene-forge"), QStringLiteral("Scene Forge"),
          QStringLiteral("Build a little world."),
          QStringLiteral("Scene"),
          QStringLiteral("Drag for parallax, lift for spotlight, hold to open the curtains."),
          QStringLiteral("Add one paper star above the spotlight."),
          QColor(242, 181, 68), QColor(46, 139, 122), QColor(36, 27, 20) },
        { QStringLiteral("auto-splatter"), QStringLiteral("Auto Splatter"),
          QStringLiteral("Print it loud."),
          QStringLiteral("Street"),
          QStringLiteral("Drag for spray, lift for contrast, hold for drips."),
          QStringLiteral("Add one paint drip that cuts the outline."),
          QColor(255, 77, 46), QColor(18, 16, 14), QColor(244, 237, 226) },
        { QStringLiteral("stage-mode"), QStringLiteral("Stage Door"),
          QStringLiteral("House lights down."),
          QStringLiteral("Stage"),
          QStringLiteral("Drag the spotlight, lift the floor, hold for the reveal."),
          QStringLiteral("Add one halo over the spotlight."),
          QColor(255, 211, 122), QColor(184, 51, 106), QColor(8, 6, 12) },
        { QStringLiteral("paper-theatre"), QStringLiteral("Paper Theatre"),
          QStringLiteral("A little world, cut from paper."),
          QStringLiteral("Scene"),
          QStringLiteral("Drag to move the paper planes, lift for moonlight, hold to part the curtains."),
          QStringLiteral("Add one paper ribbon across the marquee."),
          QColor(214, 154, 64), QColor(172, 72, 52), QColor(38, 27, 20) },
    };
    return defs;
}

const PaintbrushCreative::PatchDef *PaintbrushCreative::patchDef(const QString &id) const
{
    for (const PatchDef &d : catalog())
        if (d.id == id)
            return &d;
    return nullptr;
}

QString PaintbrushCreative::defaultPatchId() const
{
    return QStringLiteral("neon-idol");
}

/* --------------------------------------------------------------- lifecycle */

PaintbrushCreative::PaintbrushCreative(PaintbrushController *controller,
                                       QObject *parent)
    : QObject(parent), m_controller(controller)
{
    m_patchId = defaultPatchId();
    m_status = QStringLiteral("Pick a cutout, then play.");
    m_idleTimer.setInterval(50);
    connect(&m_idleTimer, &QTimer::timeout, this, &PaintbrushCreative::onIdleTick);
    m_replayTimer.setInterval(33);
    connect(&m_replayTimer, &QTimer::timeout, this, &PaintbrushCreative::onReplayTick);
    m_recordUiTimer.setInterval(250);
    connect(&m_recordUiTimer, &QTimer::timeout, this,
            &PaintbrushCreative::stateChanged);

    /* Motion composes at an interactive resolution; once the gesture (or the
     * decaying idle energy) is done, rebuild the pieces at the settled frame
     * and render the full preview. A vector change (bolt count, sparks, hue)
     * is generation-time, so recompose() alone cannot show it. */
    m_previewSettleTimer.setSingleShot(true);
    m_previewSettleTimer.setInterval(220);
    connect(&m_previewSettleTimer, &QTimer::timeout, this, [this]() {
        if (m_committed)
            return;
        m_composeInteractive = false;
        const bool frameChanged =
            !m_previewBuilt || qAbs(m_builtFrame.x - m_frame.x) > 0.001f
            || qAbs(m_builtFrame.y - m_frame.y) > 0.001f
            || qAbs(m_builtFrame.energy - m_frame.energy) > 0.001f
            || m_builtFrame.hold != m_frame.hold
            || qAbs(m_builtFrame.time - m_frame.time) > 0.001f
            || m_builtStage != m_previewSize;
        if (frameChanged)
            rebuildPreview();
        else
            recompose();
    });
    /* Pad moves arrive at pointer-event rate; coalesce them into one compose
     * per ~30 fps so a drag cannot queue a composite per event. */
    m_composeCoalesceTimer.setSingleShot(true);
    m_composeCoalesceTimer.setInterval(33);
    connect(&m_composeCoalesceTimer, &QTimer::timeout, this,
            &PaintbrushCreative::recompose);
    /* One build at a time: a full-resolution render is heavy, and a monotonic
     * revision makes a replacement a cancel rather than a second concurrent
     * build. */
    m_buildPool.setMaxThreadCount(1);
    m_buildPool.setExpiryTimeout(30000);
    m_previewPool.setMaxThreadCount(1);
    m_previewPool.setExpiryTimeout(30000);
    connect(&m_previewWatcher, &QFutureWatcherBase::finished, this,
            &PaintbrushCreative::onPreviewBuildFinished);
    m_seed = QRandomGenerator::global()->generate();
    if (m_seed == 0)
        m_seed = 1;
    /* The controller owns the one persistent FASHN runtime; the pad only
     * consumes semantic results for its current subject. */
    if (m_controller)
    {
        if (HumanParserService *parser = m_controller->humanParser())
        {
            connect(parser, &HumanParserService::parseReady, this,
                    &PaintbrushCreative::onHumanParseReady);
            connect(parser, &HumanParserService::parseFailed, this,
                    &PaintbrushCreative::onHumanParseFailed);
            connect(parser, &HumanParserService::availabilityChanged, this,
                    [this]() {
                        emit fashionParserInstallChanged();
                        HumanParserService *service =
                            m_controller ? m_controller->humanParser() : nullptr;
                        if (service && service->available()
                            && usesFashionProtection()
                            && !m_subjectNative.isNull()
                            && m_semanticProtection.isNull()
                            && !m_humanParsePending)
                            requestHumanParse();
                    });
            connect(parser, &HumanParserService::installProgressChanged, this,
                    [this](int, const QString &) {
                        emit fashionParserInstallChanged();
                    });
            connect(parser, &HumanParserService::installFinished, this,
                    [this](bool ok, const QString &) {
                        emit fashionParserInstallChanged();
                        if (ok && !m_subjectNative.isNull())
                        {
                            m_semanticProtection = QImage();
                            m_humanParsePending = false;
                            m_humanParseRequest = 0;
                            requestHumanParse();
                        }
                    });
        }
    }
}

PaintbrushCreative::~PaintbrushCreative()
{
    if (m_commit.token)
        m_commit.token->cancel();
    cancelPreviewBuild();
    /* The worker polls the token at piece boundaries; this bounded wait keeps
     * shutdown deterministic without blocking on a healthy long build. */
    m_buildPool.waitForDone(5000);
    m_previewPool.waitForDone(5000);
}

QVariantList PaintbrushCreative::patchList() const
{
    QVariantList list;
    for (const PatchDef &d : catalog())
    {
        QVariantMap m;
        m[QStringLiteral("id")] = d.id;
        m[QStringLiteral("name")] = d.name;
        m[QStringLiteral("tagline")] = d.tagline;
        m[QStringLiteral("family")] = d.family;
        m[QStringLiteral("hint")] = d.hint;
        m[QStringLiteral("accent")] = qcolorHex(d.accent);
        m[QStringLiteral("secondary")] = qcolorHex(d.secondary);
        m[QStringLiteral("ink")] = qcolorHex(d.ink);
        list.append(m);
    }
    return list;
}

QVariantMap PaintbrushCreative::activePatch() const
{
    QVariantMap m;
    if (const PatchDef *d = patchDef(m_patchId))
    {
        m[QStringLiteral("id")] = d->id;
        m[QStringLiteral("name")] = d->name;
        m[QStringLiteral("tagline")] = d->tagline;
        m[QStringLiteral("family")] = d->family;
        m[QStringLiteral("hint")] = d->hint;
        m[QStringLiteral("accent")] = qcolorHex(d->accent);
        m[QStringLiteral("secondary")] = qcolorHex(d->secondary);
        m[QStringLiteral("ink")] = qcolorHex(d->ink);
    }
    return m;
}

QString PaintbrushCreative::activePatchId() const
{
    return m_patchId;
}

void PaintbrushCreative::setOpen(bool open)
{
    if (m_open == open)
        return;
    m_open = open;
    if (open)
    {
        startIdle();
        if (!hasSubject())
            setStatus(QStringLiteral("Pick a cutout first — or Keep a moment."));
    }
    else
    {
        /* Closing the pad discards any render that has not published yet. */
        cancelCommitInternal(false);
        cancelPreviewBuild();
        m_composeCoalesceTimer.stop();
        stopIdle();
        if (m_replayTimer.isActive())
        {
            m_replayTimer.stop();
            emit stateChanged();
        }
    }
    emit openChanged();
}

void PaintbrushCreative::close()
{
    setOpen(false);
}

void PaintbrushCreative::startIdle()
{
    if (!m_idleTimer.isActive())
        m_idleTimer.start();
}

void PaintbrushCreative::stopIdle()
{
    m_idleTimer.stop();
}

void PaintbrushCreative::setStatus(const QString &text)
{
    if (m_status == text)
        return;
    m_status = text;
    emit stateChanged();
}

bool PaintbrushCreative::canCommit() const
{
    return hasSubject() && !m_patchId.isEmpty() && !m_busy && !m_committed
           && !(usesFashionProtection() && m_humanParsePending
                && m_semanticProtection.isNull());
}

double PaintbrushCreative::recordSeconds() const
{
    if (m_recording)
        return double(m_recordClock.elapsed()) / 1000.0;
    return m_samples.isEmpty() ? 0.0 : double(m_samples.last().tMs) / 1000.0;
}

double PaintbrushCreative::replayProgress() const
{
    if (!hasRecording())
        return 0.0;
    const qint64 dur = qMax<qint64>(1, m_samples.last().tMs);
    if (!m_replayTimer.isActive())
        return 0.0;
    return double(m_replayClock.elapsed() % dur) / double(dur);
}

QImage PaintbrushCreative::previewImage() const
{
    QMutexLocker locker(&m_previewMutex);
    return m_preview;
}

/* ------------------------------------------------------------------ subject */

bool PaintbrushCreative::adoptSubject(const QImage &cutout,
                                      const QRectF &canvasRect,
                                      int sourceLayerId, const QString &label,
                                      const QString &missingMessage)
{
    /* A new subject invalidates any render that is still using the old one. */
    cancelCommitInternal(false);
    if (cutout.isNull())
    {
        setStatus(missingMessage);
        return false;
    }
    m_subjectNative = cutout.convertToFormat(QImage::Format_ARGB32);
    m_subjectCanvasRect = canvasRect;
    m_sourceLayerId = sourceLayerId;
    m_sourceHarvestId = -1;
    m_subjectLabel = label;
    /* A new subject invalidates every semantic plane and any parse still in
     * flight for the old one. */
    ++m_subjectRevision;
    m_semanticProtection = QImage();
    m_humanParseRequest = 0;
    m_humanParsePending = false;
    requestHumanParse();
    m_seed = QRandomGenerator::global()->generate();
    if (m_seed == 0)
        m_seed = 1;
    m_committed = false;
    m_living = false;
    m_livingAssetId = -1;
    m_livingClip = motion::PuppetMotionClip();
    m_previewRig = motion::PuppetRig();
    m_previewRigKey.clear();
    m_livingFrame = QImage();
    m_livingFrameClock.invalidate();
    if (m_suggestionVisible || !m_suggestionText.isEmpty())
    {
        m_suggestionVisible = false;
        m_suggestionText.clear();
        emit suggestionChanged();
    }
    refreshLivingSource();
    rebuildPreview();
    refreshHistory();
    emit subjectChanged();
    emit stateChanged();
    return true;
}

void PaintbrushCreative::clearSubject()
{
    cancelCommitInternal(false);
    cancelPreviewBuild();
    m_previewBuilt = false;
    m_composeCoalesceTimer.stop();
    ++m_subjectRevision;
    m_semanticProtection = QImage();
    m_humanParseRequest = 0;
    m_humanParsePending = false;
    if (m_controller)
    {
        if (HumanParserService *parser = m_controller->humanParser())
            parser->cancelAll();
    }
    setHumanParseStatus(QString());
    m_subjectNative = QImage();
    m_subject = QImage();
    m_subjectStatic = QImage();
    m_subjectCanvasRect = QRectF();
    m_subjectLabel.clear();
    m_sourceLayerId = -1;
    m_sourceHarvestId = -1;
    m_resumeGroup.clear();
    m_living = false;
    m_livingAssetId = -1;
    m_livingClip = motion::PuppetMotionClip();
    m_previewRig = motion::PuppetRig();
    m_previewRigKey.clear();
    m_livingFrame = QImage();
    m_livingFrameClock.invalidate();
    m_pieces.clear();
    {
        QMutexLocker locker(&m_previewMutex);
        m_preview = QImage();
    }
    ++m_previewRevision;
    emit subjectChanged();
    emit previewChanged();
    emit stateChanged();
}

/* ------------------------------------------------------- Photo Regime H */

void PaintbrushCreative::refreshLivingSource()
{
    m_living = false;
    m_livingAssetId = -1;
    m_livingClip = motion::PuppetMotionClip();
    m_motionAmount = 0.65f;
    if (m_controller)
    {
        motion::PuppetMotionClip clip;
        int assetId = -1;
        float amount = 0.65f;
        if (m_controller->creativeLivingSource(m_sourceLayerId, m_sourceHarvestId,
                                              &clip, &assetId, &amount))
        {
            m_living = true;
            m_livingAssetId = assetId;
            m_livingClip = clip;
            m_motionAmount = qBound(0.01f, amount, 1.0f);
        }
    }
    m_previewRig = motion::PuppetRig();
    m_previewRigKey.clear();
    m_livingFrame = QImage();
    m_livingFrameClock.invalidate();
    emit livingChanged();
}

void PaintbrushCreative::ensureLivingPreview()
{
    if ((!m_living && !m_stageCommit) || m_subjectStatic.isNull())
        return;
    const QString key = QStringLiteral("%1x%2:%3")
                            .arg(m_subjectStatic.width())
                            .arg(m_subjectStatic.height())
                            .arg(m_seed);
    if (m_previewRig.isValid() && key == m_previewRigKey)
    {
        if (m_livingClip.keys.size() < 2)
            m_livingClip = motion::PuppetMotionScanner::proceduralFallback(
                m_previewRig, quint32(StageRuntime::kLoopPeriodMs), m_seed | 1u);
        m_livingClip.loop = motion::LoopMode::Loop;
        return;
    }

    /* Rig ONCE per preview subject (never per gesture). The stored source clip
     * is rig-independent; a procedural photo-safe sway is the fallback. */
    m_previewRig = motion::PuppetRuntime::build(m_subjectStatic, QImage());
    m_previewRigKey = key;
    if (!m_previewRig.isValid())
    {
        m_livingClip = motion::PuppetMotionClip();
        return;
    }
    if (!m_livingClip.isValid(m_previewRig.skeleton))
        m_livingClip = motion::PuppetMotionScanner::proceduralFallback(
            m_previewRig, quint32(StageRuntime::kLoopPeriodMs), m_seed | 1u);
    m_livingClip.loop = motion::LoopMode::Loop;
}

qint64 PaintbrushCreative::livingTimeMs() const
{
    if (m_replayTimer.isActive() && hasRecording())
        return qint64(replayProgress() * double(StageRuntime::kLoopPeriodMs));
    return qint64(m_frame.time * 1000.0f);
}

QImage PaintbrushCreative::livingSubjectFrame()
{
    if (!m_living || m_subjectStatic.isNull())
        return QImage();
    /* exact static frame at the loop boundary (same contract as export) */
    if (((livingTimeMs() % StageRuntime::kLoopPeriodMs)
         + StageRuntime::kLoopPeriodMs)
            % StageRuntime::kLoopPeriodMs
        == 0)
        return m_subjectStatic;
    ensureLivingPreview();
    if (!m_previewRig.isValid() || m_livingClip.keys.size() < 2)
        return QImage();
    if (m_livingFrameClock.isValid() && m_livingFrameClock.elapsed() < 40
        && !m_livingFrame.isNull())
        return m_livingFrame;

    motion::PuppetEffect fx;
    fx.kind = motion::MotionKind::Custom;
    /* selected photo-safe amount is the floor; the pad gesture adds at most
     * +12% and never exceeds the Strong/photo-safe ceiling */
    float amplitude = m_motionAmount * (1.0f + 0.12f * m_frame.energy)
                      * (m_frame.hold ? 1.05f : 1.0f);
    fx.amplitude = qBound(0.01f, amplitude, 1.0f);
    fx.speed = 1.0f;
    fx.loop = motion::LoopMode::Loop;
    fx.durationMs = quint32(StageRuntime::kLoopPeriodMs);
    fx.phase = 0.0f;
    const qint64 clipDuration = m_livingClip.durationMs();
    if (clipDuration > 0)
        fx.speed = float(double(clipDuration)
                         / double(StageRuntime::kLoopPeriodMs));

    QImage frame = motion::PuppetRuntime::renderClipFrame(
        m_previewRig, m_livingClip, fx, livingTimeMs());
    if (frame.isNull())
        return QImage();
    if (frame.size() != m_subjectStatic.size())
        frame = frame.scaled(m_subjectStatic.size(), Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
    m_livingFrame = frame;
    m_livingFrameClock.restart();
    return frame;
}

QByteArray PaintbrushCreative::stageEnvelopeBytes() const
{
    if (m_samples.size() < 2)
        return QByteArray();
    QVector<StageSample> samples;
    const int stride = qMax(1, (m_samples.size() + 399) / 400);
    for (int i = 0; i < m_samples.size(); i += stride)
    {
        const CreativeSample &s = m_samples.at(i);
        StageSample out;
        out.x = s.x;
        out.y = s.y;
        out.energy = s.energy;
        out.hold = s.hold;
        out.tMs = s.tMs;
        samples.append(out);
    }
    if (samples.size() < 2)
        return QByteArray();
    return StageEnvelope::fromSamples(samples, StageRuntime::kLoopPeriodMs)
        .serialize();
}

CanvasLayer PaintbrushCreative::makeLivingSubjectLayer(const QString &group,
                                                       const QString &patchName) const
{
    CanvasLayer layer;
    if (m_subjectNative.isNull())
        return layer;

    layer = CanvasLayer::createImage(m_subjectNative, QPointF(0, 0),
                                     QStringLiteral("Living subject"));
    if (!m_subjectCanvasRect.isEmpty() && m_subjectNative.width() > 0)
    {
        const qreal scale = m_subjectCanvasRect.width()
                            / qreal(m_subjectNative.width());
        layer.setTransform(QTransform(scale, 0.0, 0.0, scale,
                                      m_subjectCanvasRect.x(),
                                      m_subjectCanvasRect.y()));
    }
    layer.type = LAYER_IMAGE;
    layer.visible = true;
    layer.metadata[QStringLiteral("creative.patch")] = m_patchId;
    layer.metadata[QStringLiteral("creative.patchName")] = patchName;
    layer.metadata[QStringLiteral("creative.role")] = QStringLiteral("subject");
    layer.metadata[QStringLiteral("creative.seed")] = qint64(m_seed);
    layer.metadata[QStringLiteral("creative.group")] = group;
    layer.metadata[QStringLiteral("creative.sourceLayerId")] = m_sourceLayerId;
    layer.metadata[QStringLiteral("creative.sourceHarvestId")] = m_sourceHarvestId;
    QVariantMap params;
    params[QStringLiteral("x")] = double(m_frame.x);
    params[QStringLiteral("y")] = double(m_frame.y);
    params[QStringLiteral("energy")] = double(m_frame.energy);
    params[QStringLiteral("hold")] = m_frame.hold;
    layer.metadata[QStringLiteral("creative.params")] = params;

    motion::PuppetMotionClip clip = m_livingClip;
    if (clip.keys.size() < 2)
    {
        const motion::PuppetRig rig =
            motion::PuppetRuntime::build(m_subjectNative, QImage());
        if (rig.isValid())
            clip = motion::PuppetMotionScanner::proceduralFallback(
                rig, quint32(StageRuntime::kLoopPeriodMs), m_seed | 1u);
    }
    if (clip.keys.size() >= 2)
    {
        clip.loop = motion::LoopMode::Loop;
        motion::PuppetEffect fx;
        fx.kind = motion::MotionKind::Custom;
        fx.amplitude = qBound(0.01f, m_motionAmount, 1.0f);
        fx.loop = motion::LoopMode::Loop;
        fx.durationMs = quint32(StageRuntime::kLoopPeriodMs);
        const qint64 clipDuration = clip.durationMs();
        fx.speed = clipDuration > 0
                       ? float(double(clipDuration)
                               / double(StageRuntime::kLoopPeriodMs))
                       : 1.0f;
        layer.puppetAssetId = -1;
        layer.puppetEffect = fx;
        layer.metadata[stagekeys::living()] = true;
        layer.metadata[stagekeys::motionClip()] =
            QString::fromLatin1(StageRuntime::encodeClip(clip));
        layer.metadata[stagekeys::loopPeriodMs()] =
            qint64(StageRuntime::kLoopPeriodMs);
        const QByteArray envelope = stageEnvelopeBytes();
        if (!envelope.isEmpty())
            layer.metadata[stagekeys::envelope()] =
                QString::fromLatin1(envelope.toBase64());
        StageRuntime::applyStageMetadata(&layer, group,
                                         QStringLiteral("subject"), 0.0f,
                                         0.0f, 0.0f, envelope);
    }
    return layer;
}

void PaintbrushCreative::attachDerivedLiving(CanvasLayer *layer,
                                             const QString &scene, bool stage) const
{
    if (!layer || m_livingClip.keys.size() < 2)
        return;
    motion::PuppetEffect fx;
    fx.kind = motion::MotionKind::Custom;
    fx.amplitude = qBound(0.01f, m_motionAmount, 1.0f);
    fx.loop = motion::LoopMode::Loop;
    fx.durationMs = quint32(StageRuntime::kLoopPeriodMs);
    const qint64 clipDuration = m_livingClip.durationMs();
    fx.speed = clipDuration > 0
                   ? float(double(clipDuration)
                           / double(StageRuntime::kLoopPeriodMs))
                   : 1.0f;
    layer->puppetAssetId = -1;
    layer->puppetEffect = fx;
    layer->metadata[stagekeys::living()] = true;
    layer->metadata[stagekeys::motionClip()] =
        QString::fromLatin1(StageRuntime::encodeClip(m_livingClip));
    layer->metadata[stagekeys::loopPeriodMs()] =
        qint64(StageRuntime::kLoopPeriodMs);
    const QByteArray envelope = stageEnvelopeBytes();
    if (!envelope.isEmpty())
        layer->metadata[stagekeys::envelope()] =
            QString::fromLatin1(envelope.toBase64());
    if (stage)
        StageRuntime::applyStageMetadata(layer, scene,
                                         QStringLiteral("subject"), 0.0f,
                                         0.0f, 0.0f, envelope);
}

void PaintbrushCreative::setMotionAmount(double amount)
{
    const float next = qBound(0.01f, float(amount), 1.0f);
    if (qFuzzyCompare(next, m_motionAmount))
        return;
    m_motionAmount = next;
    m_committed = false;
    m_livingFrame = QImage();
    m_livingFrameClock.invalidate();
    if (!m_subjectNative.isNull())
        recompose();
    emit livingChanged();
    emit stateChanged();
}

void PaintbrushCreative::setLivingMotion(bool enabled)
{
    if (m_living == enabled)
        return;
    m_living = enabled;
    m_livingFrame = QImage();
    m_livingFrameClock.invalidate();
    if (m_living && !m_subjectNative.isNull())
        ensureLivingPreview();
    m_committed = false;
    if (!m_subjectNative.isNull())
        recompose();
    emit livingChanged();
    emit stateChanged();
}

void PaintbrushCreative::openFromHarvest(int harvestId)
{
    QRectF rect;
    QString label;
    const QImage cutout = m_controller
                              ? m_controller->creativePadHarvest(harvestId, &rect, &label)
                              : QImage();
    m_patchId = defaultPatchId();
    m_chooserOpen = true;
    m_resumeGroup.clear();
    if (!adoptSubject(cutout, rect, -1, label,
                      QStringLiteral("That kept moment has no cutout yet.")))
    {
        setOpen(true);
        return;
    }
    m_sourceHarvestId = harvestId;
    m_committed = false;
    refreshLivingSource();
    rebuildPreview();
    setOpen(true);
    setStatus(QStringLiteral("Start with Neon Idol or Sticker Forge."));
    emit patchChanged();
}

void PaintbrushCreative::openForSelection()
{
    QRectF rect;
    QString label;
    int layerId = -1;
    const QImage cutout = m_controller
                              ? m_controller->creativePadSubject(&layerId, &rect, &label)
                              : QImage();
    m_chooserOpen = false;
    m_resumeGroup.clear();
    if (!adoptSubject(cutout, rect, layerId, label,
                      QStringLiteral("Pick a cutout first — or Keep a moment.")))
    {
        setOpen(true);
        return;
    }
    m_committed = false;
    setOpen(true);
    setStatus(QStringLiteral("Drag the pad. The stage is live."));
    emit patchChanged();
}

/* ------------------------------------------------------------------- preview */

void PaintbrushCreative::rebuildPreview()
{
    if (m_subjectNative.isNull() || !m_controller)
    {
        m_pieces.clear();
        return;
    }
    const QSize canvas = m_controller->creativePadCanvasSize();
    if (!canvas.isValid() || canvas.isEmpty())
        return;

    /* The live preview renders at a 4K-class resolution (native up to a
     * 3840x2160 frame budget). The stage panel scales the image down for
     * display, but the pixels the user sees are real preview detail, not a
     * 560x680 thumbnail. */
    const QSize previewSize = creative::recipe::previewStage(canvas);
    if (previewSize.isEmpty())
        return;
    m_previewSize = previewSize;

    const qreal sx = double(previewSize.width()) / double(canvas.width());
    const qreal sy = double(previewSize.height()) / double(canvas.height());
    const QRectF previewRect(m_subjectCanvasRect.x() * sx,
                             m_subjectCanvasRect.y() * sy,
                             m_subjectCanvasRect.width() * sx,
                             m_subjectCanvasRect.height() * sy);
    m_previewSubjectRect = previewRect;
    const QSize subjSize(qMax(1, int(std::lround(previewRect.width()))),
                         qMax(1, int(std::lround(previewRect.height()))));
    m_subject = m_subjectNative.scaled(subjSize, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);
    m_subjectStatic = m_subject;
    m_livingFrame = QImage();
    m_livingFrameClock.invalidate();
    if (m_living || m_stageCommit)
        ensureLivingPreview();

    CreativeBuildInput input;
    input.patchId = m_patchId;
    input.patchName = m_patchId;
    input.preview = true;
    if (const PatchDef *d = patchDef(m_patchId))
    {
        input.accent = d->accent;
        input.accentAlt = d->secondary;
    }
    input.subject = m_subject;
    input.subjectRect = previewRect;
    input.stage = previewSize;
    input.previewSize = previewSize;
    input.seed = m_seed;
    input.frame = m_frame;
    if (usesFashionProtection() && !m_semanticProtection.isNull())
    {
        QImage protection = m_semanticProtection.scaled(
            subjSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (protection.format() != QImage::Format_Grayscale8)
            protection = protection.convertToFormat(QImage::Format_Grayscale8);
        input.protection = protection;
    }

    /* The build is CPU-heavy (holo materials run normals + per-pixel shading),
     * so it must never block the GUI thread. The old preview stays visible;
     * the revision makes a completion from a previous patch harmless. */
    cancelPreviewBuild();
    m_previewBuilt = false;
    const auto token = std::make_shared<std::atomic_bool>(false);
    m_previewCancel = token;
    const quint64 revision = ++m_previewBuildRevision;
    m_composeInteractive = false;
    m_previewSettleTimer.stop();
    m_previewWatcher.setFuture(QtConcurrent::run(
        &m_previewPool, [input, token, revision]() -> PreviewBuildResult {
            PreviewBuildResult result;
            result.revision = revision;
            result.stage = input.stage;
            result.frame = input.frame;
            result.pieces = creative::recipe::build(
                input, [token](const QVector<CreativePiece> &, int) {
                    return !token->load();
                });
            result.cancelled = token->load();
            return result;
        }));
}

void PaintbrushCreative::recompose()
{
    /* While a commit or a preview build is running, the current pieces and
     * m_previewSize are not a matched pair; keep the last complete frame. */
    if (m_busy || m_previewSize.isEmpty() || m_previewWatcher.isRunning())
        return;
    QElapsedTimer frameClock;
    frameClock.start();

    QImage frame;
    if (m_compare)
    {
        /* Before: the untouched source on a neutral studio field. */
        frame = QImage(m_previewSize, QImage::Format_ARGB32_Premultiplied);
        frame.fill(QColor(12, 13, 20));
        QPainter p(&frame);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QLinearGradient bg(0, 0, 0, frame.height());
        bg.setColorAt(0.0, QColor(26, 28, 40));
        bg.setColorAt(1.0, QColor(8, 9, 14));
        p.fillRect(frame.rect(), bg);
        if (!m_subject.isNull())
        {
            const QRectF r = m_previewSubjectRect;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 70));
            p.drawEllipse(QPointF(r.center().x(), r.bottom() + r.height() * 0.03),
                          r.width() * 0.38, r.height() * 0.035);
            p.drawImage(r, m_subject);
        }
        p.end();
    }
    else if (m_living && !m_subjectStatic.isNull())
    {
        const QImage live = livingSubjectFrame();
        if (!live.isNull())
            m_pieces = buildPatch(m_patchId, live, m_previewSubjectRect,
                                  m_previewSize, m_seed);
        frame = composeStage(m_pieces, m_previewSize, QColor(12, 12, 18));
    }
    else
    {
        const QSize target =
            m_composeInteractive
                ? creative::recipe::interactivePreviewStage(m_previewSize)
                : m_previewSize;
        frame = creative::recipe::compose(m_pieces, m_previewSize, target,
                                          QColor(12, 12, 18), m_frame);
    }
    {
        QMutexLocker locker(&m_previewMutex);
        m_preview = frame;
    }
    ++m_previewRevision;

    m_previewTimes.append(double(frameClock.nsecsElapsed()) / 1.0e6);
    while (m_previewTimes.size() > 120)
        m_previewTimes.removeFirst();
    if (!m_previewTimes.isEmpty())
    {
        QVector<double> sorted = m_previewTimes;
        std::sort(sorted.begin(), sorted.end());
        const int idx = qBound(0, int(std::ceil(sorted.size() * 0.95)) - 1,
                               sorted.size() - 1);
        m_previewP95Ms = sorted.at(idx);
    }
    emit previewChanged();
    emit metricsChanged();
}

void PaintbrushCreative::setCompare(bool compare)
{
    if (m_compare == compare)
        return;
    m_compare = compare;
    recompose();
    emit compareChanged();
}

void PaintbrushCreative::renderPreviewStill()
{
    m_composeCoalesceTimer.stop();
    m_previewSettleTimer.stop();
    const auto waitForBuild = [this]() {
        if (!m_previewWatcher.isRunning())
            return;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&m_previewWatcher, &QFutureWatcherBase::finished, &loop,
                &QEventLoop::quit);
        timeout.start(30000);
        if (!m_previewWatcher.isFinished())
            loop.exec();
    };
    const auto frameDiff = [this]() {
        return !m_previewBuilt
               || qAbs(m_builtFrame.x - m_frame.x) > 0.001f
               || qAbs(m_builtFrame.y - m_frame.y) > 0.001f
               || qAbs(m_builtFrame.energy - m_frame.energy) > 0.001f
               || m_builtFrame.hold != m_frame.hold
               || m_builtStage != m_previewSize;
    };
    waitForBuild();
    /* A patch selection starts a build with the frame captured at that moment;
     * a test/capture that moved the pad afterwards needs one settled rebuild. */
    if (frameDiff())
    {
        rebuildPreview();
        waitForBuild();
    }
    m_composeInteractive = false;
    recompose();
}

void PaintbrushCreative::cancelPreviewBuild()
{
    if (m_previewCancel)
        m_previewCancel->store(true);
    m_previewCancel.reset();
    ++m_previewBuildRevision;
}

void PaintbrushCreative::onPreviewBuildFinished()
{
    if (!m_previewWatcher.isFinished())
        return;
    const PreviewBuildResult result = m_previewWatcher.result();
    if (result.cancelled || result.revision != m_previewBuildRevision)
        return;
    m_pieces = result.pieces;
    m_previewSize = result.stage;
    m_builtStage = result.stage;
    m_builtFrame = result.frame;
    m_previewBuilt = true;
    m_composeInteractive = false;
    recompose();
}

void PaintbrushCreative::scheduleRecompose()
{
    m_composeInteractive = true;
    m_previewSettleTimer.start();
    if (!m_composeCoalesceTimer.isActive())
        m_composeCoalesceTimer.start();
}

/* ------------------------------------------------------- human parser ---- */

bool PaintbrushCreative::usesFashionProtection() const
{
    if (!m_fashionProtection)
        return false;
    /* Only the eight material looks whose recipe consumes the FASHN plane.
     * holo-fashion (Prism Muse) is a full-subject look, so promising
     * garment-only protection there would be false. */
    static const QStringList kProtectedLooks = {
        QStringLiteral("holo-liquid"), QStringLiteral("holo-opal"),
        QStringLiteral("holo-chrome"), QStringLiteral("holo-prism"),
        QStringLiteral("holo-wrap"), QStringLiteral("holo-chameleon"),
        QStringLiteral("holo-glitter"), QStringLiteral("holo-livery"),
    };
    return kProtectedLooks.contains(m_patchId);
}

bool PaintbrushCreative::fashionParserInstalled() const
{
    HumanParserService *parser =
        m_controller ? m_controller->humanParser() : nullptr;
    return parser && parser->available();
}

bool PaintbrushCreative::fashionParserCanInstall() const
{
    HumanParserService *parser =
        m_controller ? m_controller->humanParser() : nullptr;
    return parser && parser->canInstall();
}

bool PaintbrushCreative::fashionParserInstalling() const
{
    HumanParserService *parser =
        m_controller ? m_controller->humanParser() : nullptr;
    return parser && parser->installing();
}

int PaintbrushCreative::fashionParserInstallProgress() const
{
    HumanParserService *parser =
        m_controller ? m_controller->humanParser() : nullptr;
    return parser ? parser->installProgress() : 0;
}

QString PaintbrushCreative::fashionParserInstallStatus() const
{
    HumanParserService *parser =
        m_controller ? m_controller->humanParser() : nullptr;
    return parser ? parser->installStatus() : QString();
}

void PaintbrushCreative::installFashionParser()
{
    HumanParserService *parser =
        m_controller ? m_controller->humanParser() : nullptr;
    if (!parser)
        return;
    parser->installHumanParser();
    emit fashionParserInstallChanged();
}

void PaintbrushCreative::setHumanParseStatus(const QString &text)
{
    if (m_humanParseStatus == text)
        return;
    m_humanParseStatus = text;
    emit humanParseStatusChanged();
    emit stateChanged(); /* canCommit is gated on a pending parse */
}

void PaintbrushCreative::clearHumanMasks()
{
    m_semanticProtection = QImage();
    m_humanParsePending = false;
    m_humanParseRequest = 0;
}

void PaintbrushCreative::requestHumanParse()
{
    if (m_subjectNative.isNull() || m_subjectNative.size().isEmpty())
    {
        clearHumanMasks();
        return;
    }
    HumanParserService *parser =
        m_controller ? m_controller->humanParser() : nullptr;
    if (!parser)
    {
        clearHumanMasks();
        setHumanParseStatus(
            QStringLiteral("Fashion parser is not available in this build."));
        return;
    }
    if (!parser->available())
    {
        const bool canInstall = parser->canInstall();
        clearHumanMasks();
        const QString reason = parser->unavailableReason();
        if (canInstall)
        {
            setHumanParseStatus(QStringLiteral(
                "Fashion parser is not installed. Tap Get the fashion parser "
                "to download it; the material will then stay on the outfit."));
        }
        else
        {
            setHumanParseStatus(
                reason.isEmpty()
                    ? QStringLiteral("Fashion parse unavailable — styling the "
                                     "whole subject.")
                    : QStringLiteral("Fashion parse unavailable: %1").arg(reason));
        }
        return;
    }

    /* The parser consumes the SAM3-owned cutout alpha; it is not allowed to
     * invent a new subject. The RGB and alpha snapshots are taken inside the
     * service before the job is queued. */
    const QImage rgb =
        m_subjectNative.convertToFormat(QImage::Format_RGB32);
    const quint64 requestId =
        parser->request(rgb, m_subjectNative, m_subjectRevision);
    if (requestId == 0)
    {
        clearHumanMasks();
        setHumanParseStatus(
            QStringLiteral("Fashion parser is idle; no parse was scheduled."));
        return;
    }
    m_humanParseRequest = requestId;
    m_humanParsePending = true;
    setHumanParseStatus(QStringLiteral("Reading the fit…"));
}

void PaintbrushCreative::setFashionProtection(bool enabled)
{
    if (m_fashionProtection == enabled)
        return;
    m_fashionProtection = enabled;
    if (enabled)
    {
        if (!m_subjectNative.isNull() && m_semanticProtection.isNull()
            && !m_humanParsePending)
            requestHumanParse();
        else if (!m_semanticProtection.isNull())
            setHumanParseStatus(QStringLiteral(
                "Fashion parse ready — skin, hair and accessories stay "
                "untouched."));
    }
    else
    {
        setHumanParseStatus(QStringLiteral("Styling the whole subject."));
    }
    if (!m_subjectNative.isNull())
        rebuildPreview();
    emit fashionProtectionChanged();
}

QString PaintbrushCreative::waitForHumanParse(int timeoutMs)
{
    if (!m_humanParsePending)
        return m_humanParseStatus;

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    const QMetaObject::Connection done = connect(
        this, &PaintbrushCreative::humanParseFinished, &loop,
        [&loop](bool) { loop.quit(); });
    if (timeoutMs > 0)
        timeout.start(timeoutMs);
    loop.exec();
    disconnect(done);
    if (timeout.isActive())
        timeout.stop();

    if (m_humanParsePending)
        return QStringLiteral("Fashion parse is still running.");
    return m_humanParseStatus;
}

void PaintbrushCreative::onHumanParseReady(
    const paintbrush::HumanParseResult &result, quint64 revision,
    quint64 requestId)
{
    if (revision != m_subjectRevision || requestId != m_humanParseRequest)
        return;
    m_humanParsePending = false;
    m_humanParseRequest = 0;

    if (m_subjectNative.isNull() || !result.isValid()
        || result.masks.garment.isNull()
        || result.subjectAlpha.size() != m_subjectNative.size())
    {
        m_semanticProtection = QImage();
        setHumanParseStatus(
            QStringLiteral("Fashion parse returned no usable outfit mask — "
                           "styling the whole subject."));
        if (usesFashionProtection())
            rebuildPreview();
        emit humanParseFinished(false);
        return;
    }

    const QImage alpha =
        result.subjectAlpha.convertToFormat(QImage::Format_Grayscale8);
    const QImage garment = composeSemanticCoverage(
        alpha, result.masks.garment, creative::CoverageMode::InheritedCoverage);
    if (garment.isNull())
    {
        m_semanticProtection = QImage();
        setHumanParseStatus(
            QStringLiteral("Fashion parse returned no outfit — styling the "
                           "whole subject."));
        if (usesFashionProtection())
            rebuildPreview();
        emit humanParseFinished(false);
        return;
    }

    quint64 subjectPixels = 0;
    quint64 garmentPixels = 0;
    for (int y = 0; y < alpha.height(); ++y)
    {
        const uchar *a = alpha.constScanLine(y);
        const uchar *g = garment.constScanLine(y);
        for (int x = 0; x < alpha.width(); ++x)
        {
            if (a[x] < 128)
                continue;
            ++subjectPixels;
            if (g[x] >= 128)
                ++garmentPixels;
        }
    }

    /* A confident garment region is required before protection is enabled.
     * On an image with no outfit (or a non-human subject) the material keeps
     * the previous whole-subject behavior instead of silently doing nothing. */
    if (subjectPixels == 0 || garmentPixels * 200 < subjectPixels)
    {
        m_semanticProtection = QImage();
        setHumanParseStatus(QStringLiteral(
            "No outfit found — the material covers the whole subject."));
        if (usesFashionProtection())
            rebuildPreview();
        emit humanParseFinished(true);
        return;
    }

    const QImage hair =
        result.masks.hair.size() == alpha.size()
            ? result.masks.hair.convertToFormat(QImage::Format_Grayscale8)
            : QImage();
    const QImage skin =
        result.masks.skin.size() == alpha.size()
            ? result.masks.skin.convertToFormat(QImage::Format_Grayscale8)
            : QImage();
    const QImage accessory =
        result.masks.accessory.size() == alpha.size()
            ? result.masks.accessory.convertToFormat(QImage::Format_Grayscale8)
            : QImage();

    QImage protection(alpha.size(), QImage::Format_Grayscale8);
    if (protection.isNull())
    {
        m_semanticProtection = QImage();
        setHumanParseStatus(
            QStringLiteral("Could not allocate the fashion protection mask."));
        emit humanParseFinished(false);
        return;
    }
    for (int y = 0; y < alpha.height(); ++y)
    {
        const uchar *a = alpha.constScanLine(y);
        const uchar *g = garment.constScanLine(y);
        const uchar *h = hair.isNull() ? nullptr : hair.constScanLine(y);
        const uchar *s = skin.isNull() ? nullptr : skin.constScanLine(y);
        const uchar *c =
            accessory.isNull() ? nullptr : accessory.constScanLine(y);
        uchar *p = protection.scanLine(y);
        for (int x = 0; x < alpha.width(); ++x)
        {
            if (a[x] < 128)
            {
                p[x] = 255; /* outside the subject: keep original */
                continue;
            }
            int keep = 255 - int(g[x]);
            if (h)
                keep = qMax(keep, int(h[x]));
            if (s)
                keep = qMax(keep, int(s[x]));
            if (c)
                keep = qMax(keep, int(c[x]));
            p[x] = uchar(keep);
        }
    }

    m_semanticProtection = protection;
    const int percent =
        int((garmentPixels * 100 + subjectPixels / 2) / subjectPixels);
    setHumanParseStatus(
        QStringLiteral("Fit found — %1% of the subject. Skin, hair and "
                       "accessories stay untouched.")
            .arg(percent));
    if (usesFashionProtection())
        rebuildPreview();
    emit humanParseFinished(true);
}

void PaintbrushCreative::onHumanParseFailed(quint64 requestId,
                                            const QString &error)
{
    if (requestId != m_humanParseRequest)
        return;
    m_humanParsePending = false;
    m_humanParseRequest = 0;
    m_semanticProtection = QImage();
    setHumanParseStatus(
        error.isEmpty()
            ? QStringLiteral("Fashion parse failed — styling the whole "
                             "subject.")
            : QStringLiteral("Fashion parse failed: %1").arg(error));
    if (usesFashionProtection() && !m_subjectNative.isNull())
        rebuildPreview();
    emit humanParseFinished(false);
}

void PaintbrushCreative::onIdleTick()
{
    if (!m_open || m_subject.isNull())
        return;
    m_frame.time += 0.05f;
    /* idle energy decays so the stage settles after a gesture */
    m_frame.energy *= 0.965f;
    if (m_frame.energy < 0.002f)
        m_frame.energy = 0.0f;
    if (m_frame.energy > 0.0f)
    {
        /* still moving: keep the 20 Hz tick at the interactive size */
        if (!m_committed)
            scheduleRecompose();
    }
    /* At rest the settle timer renders one full preview still and the idle
     * tick stops overwriting it; time keeps advancing for the next gesture. */
    if (!m_recording && !m_replayTimer.isActive())
        emit padChanged();
}

QVector<CreativePiece> PaintbrushCreative::bakeFrame(QVector<CreativePiece> pieces,
                                                    const QSize &stage) const
{
    for (CreativePiece &p : pieces)
    {
        const CreativeMotion &m = p.motion;
        const float op = clamp01(p.opacity
                                 + m.opacityX * m_frame.x
                                 + m.opacityY * m_frame.y
                                 + m.opacityEnergy * m_frame.energy
                                 + m.opacityHold * (m_frame.hold ? 1.0f : 0.0f));
        const qreal dx = (m.dxX * (m_frame.x - 0.5f)
                          + m.dxEnergy * m_frame.energy
                          + m.dxHold * (m_frame.hold ? 1.0f : 0.0f))
                         * stage.width();
        const qreal dy = (m.dyY * (m_frame.y - 0.5f)
                          + m.dyEnergy * m_frame.energy
                          + m.dyHold * (m_frame.hold ? 1.0f : 0.0f))
                         * stage.height();
        const qreal sc = 1.0 + m.scaleY * (m_frame.y - 0.5f)
                         + m.scaleEnergy * m_frame.energy
                         + m.scaleHold * (m_frame.hold ? 1.0 : 0.0);
        const qreal rot = m.rotationEnergy * m_frame.energy
                          + m.rotationHold * (m_frame.hold ? 1.0 : 0.0);

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

void PaintbrushCreative::beginPad()
{
    if (!hasSubject())
        return;
    setStatus(QStringLiteral("Play."));
    m_composeInteractive = true;
    m_previewSettleTimer.start();
}

void PaintbrushCreative::movePad(double x, double y, double energy, bool hold)
{
    if (!hasSubject())
        return;
    const float nx = clamp01(float(x));
    const float ny = clamp01(float(y));
    const float ne = clamp01(float(energy));
    /* low-pass the energy so a fast flick blooms instead of flickering */
    m_frame.energy = m_frame.energy * 0.55f + ne * 0.45f;
    if (qAbs(nx - m_frame.x) < 0.0015f && qAbs(ny - m_frame.y) < 0.0015f
        && qAbs(ne - m_frame.energy) < 0.002f && hold == m_frame.hold)
        return;
    m_frame.x = nx;
    m_frame.y = ny;
    m_frame.hold = hold;
    if (m_recording)
    {
        CreativeSample s;
        s.x = nx;
        s.y = ny;
        s.energy = m_frame.energy;
        s.hold = hold;
        s.tMs = m_recordClock.elapsed();
        m_samples.append(s);
    }
    scheduleRecompose();
    emit padChanged();
}

void PaintbrushCreative::endPad()
{
    /* The last frame is the committed look; nothing to do but let the idle
     * shimmer carry on. */
    emit padChanged();
}

void PaintbrushCreative::setHold(bool hold)
{
    if (m_frame.hold == hold)
        return;
    m_frame.hold = hold;
    if (m_recording)
    {
        CreativeSample s;
        s.x = m_frame.x;
        s.y = m_frame.y;
        s.energy = m_frame.energy;
        s.hold = hold;
        s.tMs = m_recordClock.elapsed();
        m_samples.append(s);
    }
    scheduleRecompose();
    emit padChanged();
}

/* ---------------------------------------------------------------- recording */

void PaintbrushCreative::toggleRecord()
{
    if (!hasSubject())
        return;
    if (m_recording)
    {
        m_recording = false;
        m_recordUiTimer.stop();
        if (m_samples.size() < 2)
            m_samples.clear();
        setStatus(m_samples.isEmpty()
                      ? QStringLiteral("That move was too short. Try again.")
                      : QStringLiteral("Saved. Loop it, or make it real."));
    }
    else
    {
        if (m_replayTimer.isActive())
            m_replayTimer.stop();
        m_samples.clear();
        m_recordClock.start();
        m_recording = true;
        m_recordUiTimer.start();
        CreativeSample s;
        s.x = m_frame.x;
        s.y = m_frame.y;
        s.energy = m_frame.energy;
        s.hold = m_frame.hold;
        s.tMs = 0;
        m_samples.append(s);
        setStatus(QStringLiteral("Recording. Drag the pad, then stop."));
    }
    emit stateChanged();
}

void PaintbrushCreative::toggleReplay()
{
    if (m_replayTimer.isActive())
    {
        m_replayTimer.stop();
        setStatus(QStringLiteral("Loop stopped."));
        emit stateChanged();
        return;
    }
    if (m_samples.size() < 2)
    {
        setStatus(QStringLiteral("Record a move first."));
        return;
    }
    if (m_recording)
    {
        m_recording = false;
    }
    m_replayClock.start();
    m_replayTimer.start();
    setStatus(QStringLiteral("Looping your move."));
    emit stateChanged();
}

void PaintbrushCreative::onReplayTick()
{
    if (m_samples.size() < 2)
    {
        m_replayTimer.stop();
        emit stateChanged();
        return;
    }
    const qint64 duration = qMax<qint64>(1, m_samples.last().tMs);
    const qint64 t = m_replayClock.elapsed() % duration;
    int i = 1;
    while (i < m_samples.size() && m_samples.at(i).tMs < t)
        ++i;
    const CreativeSample &a = m_samples.at(qMax(0, i - 1));
    const CreativeSample &b = m_samples.at(qMin(m_samples.size() - 1, i));
    const qint64 span = qMax<qint64>(1, b.tMs - a.tMs);
    const float f = float(double(t - a.tMs) / double(span));
    m_frame.x = a.x + (b.x - a.x) * f;
    m_frame.y = a.y + (b.y - a.y) * f;
    m_frame.energy = a.energy + (b.energy - a.energy) * f;
    m_frame.hold = f < 0.5f ? a.hold : b.hold;
    scheduleRecompose();
    emit padChanged();
    emit stateChanged(); /* progress + elapsed */
}

/* ------------------------------------------------------------------- actions */

void PaintbrushCreative::selectPatch(const QString &id)
{
    if (!patchDef(id))
        return;
    if (m_patchId == id && !m_chooserOpen)
        return;
    /* Switching the look replaces the recipe: discard any in-flight build. */
    cancelCommitInternal(false);
    m_patchId = id;
    m_chooserOpen = false;
    m_committed = false;
    if (m_suggestionVisible || !m_suggestionText.isEmpty())
    {
        m_suggestionVisible = false;
        m_suggestionText.clear();
        emit suggestionChanged();
    }
    if (!m_subjectNative.isNull())
        rebuildPreview();
    if (const PatchDef *d = patchDef(id))
        setStatus(d->hint);
    emit patchChanged();
    emit stateChanged();
}

void PaintbrushCreative::reseed()
{
    cancelCommitInternal(false);
    m_seed = QRandomGenerator::global()->generate();
    if (m_seed == 0)
        m_seed = 1;
    m_committed = false;
    if (!m_subjectNative.isNull())
        rebuildPreview();
    setStatus(QStringLiteral("New seed. Same pose, new world."));
    emit stateChanged();
}

void PaintbrushCreative::surprise()
{
    const QVector<PatchDef> &all = catalog();
    if (all.isEmpty())
        return;
    QString next = m_patchId;
    while (next == m_patchId && all.size() > 1)
        next = all.at(int(QRandomGenerator::global()->bounded(all.size()))).id;
    cancelCommitInternal(false);
    m_patchId = next;
    m_chooserOpen = false;
    m_seed = QRandomGenerator::global()->generate();
    if (m_seed == 0)
        m_seed = 1;
    m_frame.x = 0.2f + float(QRandomGenerator::global()->bounded(60)) / 100.0f;
    m_frame.y = 0.2f + float(QRandomGenerator::global()->bounded(60)) / 100.0f;
    m_committed = false;
    if (!m_subjectNative.isNull())
        rebuildPreview();
    if (const PatchDef *d = patchDef(next))
        setStatus(QStringLiteral("Surprise: %1. %2").arg(d->name, d->tagline));
    emit patchChanged();
    emit padChanged();
    emit stateChanged();
}

QString PaintbrushCreative::commit()
{
    /* Stage/Living/Rigged and resume/replace commits keep the master's
     * synchronous stage path; new looks use the cancellable recipe worker. */
    if (m_stageCommit || m_living || !m_resumeGroup.isEmpty())
        return commitLegacy();

    QString error;
    if (!beginCommit(&error))
        return error;
    return QString();
}

bool PaintbrushCreative::beginCommit(QString *error)
{
    const auto reject = [error](const QString &text) {
        if (error)
            *error = text;
        return false;
    };
    if (!hasSubject() || !m_controller)
        return reject(QStringLiteral("Pick a cutout first."));
    if (m_patchId.isEmpty())
        return reject(QStringLiteral("Choose a patch first."));
    if (m_committed)
        return reject(QStringLiteral(
            "Already committed. Pick a patch to start a new run."));
    if (usesFashionProtection() && m_humanParsePending
        && m_semanticProtection.isNull())
        return reject(QStringLiteral(
            "Reading the fit — try Make it real again in a moment."));
    cancelPreviewBuild();
    const PatchDef *def = patchDef(m_patchId);
    if (!def)
        return reject(QStringLiteral("Unknown patch."));

    const QSize canvas = m_controller->creativePadCanvasSize();
    if (!canvas.isValid() || canvas.isEmpty())
        return reject(QStringLiteral("Open a canvas first."));

    /* Full resolution, but bounded. Never upscale a smaller build and never
     * silently drop resolution: an over-budget canvas is refused loudly. */
    const QString budgetError =
        creative::recipe::fullResolutionBudgetError(canvas);
    if (!budgetError.isEmpty())
    {
        setStatus(budgetError);
        emit stateChanged();
        return reject(budgetError);
    }

    if (m_replayTimer.isActive())
        m_replayTimer.stop();
    if (m_recording)
        m_recording = false;

    /* Replace any in-flight render before taking the new snapshot: cancel its
     * token and invalidate its revision so a racing result can never publish. */
    cancelCommitInternal(false);

    QSize previewSize = m_previewSize;
    if (!previewSize.isValid() || previewSize.isEmpty())
        previewSize = creative::recipe::previewStage(canvas);
    m_previewSize = previewSize;

    CreativeBuildInput input;
    input.patchId = m_patchId;
    input.patchName = def->name;
    input.accent = def->accent;
    input.accentAlt = def->secondary;
    input.subject = m_subjectNative;
    input.subjectRect = m_subjectCanvasRect;
    input.stage = canvas; /* native canvas resolution, not a 1600 px cap */
    input.previewSize = previewSize;
    input.seed = m_seed;
    input.frame = m_frame;
    input.group = QStringLiteral("look-%1")
                      .arg(QDateTime::currentMSecsSinceEpoch());
    input.sourceLayerId = m_sourceLayerId;
    input.sourceHarvestId = m_sourceHarvestId;
    if (usesFashionProtection() && !m_semanticProtection.isNull())
        input.protection = m_semanticProtection;

    m_commit = CommitContext();
    m_commit.revision = ++m_commitRevision;
    m_commit.suggestion = def->suggestion;
    m_commit.desc = QStringLiteral("Create: %1").arg(def->name);
    m_commit.sourceLayerId = m_sourceLayerId;
    m_commit.canvasSize = canvas;
    m_commit.previewSize = previewSize;
    m_commit.token = std::make_shared<CreativeCancelToken>();

    m_busy = true;
    m_commitPercent = 0;
    m_consumedResults = 0;
    m_lastCommitError.clear();
    m_commitClock.start();
    emit stateChanged();
    emit commitProgressChanged();
    setStatus(QStringLiteral("Rendering at %1x%2 — full canvas resolution.")
                  .arg(canvas.width()).arg(canvas.height()));

    auto *watcher = new QFutureWatcher<CreativeCommitEvent>(this);
    m_commitWatcher = watcher;
    connect(watcher, &QFutureWatcherBase::resultReadyAt, this,
            &PaintbrushCreative::onCommitResultReady);
    connect(watcher, &QFutureWatcherBase::progressValueChanged, this,
            &PaintbrushCreative::onCommitProgress);
    connect(watcher, &QFutureWatcherBase::finished, this,
            &PaintbrushCreative::onCommitFinished);
    connect(watcher, &QObject::destroyed, this, [this, watcher]() {
        if (m_commitWatcher == watcher)
            m_commitWatcher = nullptr;
    });

    const quint64 revision = m_commit.revision;
    const std::shared_ptr<CreativeCancelToken> token = m_commit.token;
    watcher->setFuture(QtConcurrent::run(
        &m_buildPool,
        [input, token, revision](QPromise<CreativeCommitEvent> &promise) {
            runCreativeCommitJob(input, token, revision, promise);
        }));
    return true;
}

void PaintbrushCreative::cancelCommit()
{
    cancelCommitInternal(true);
}

void PaintbrushCreative::cancelCommitInternal(bool notify)
{
    if (m_commitWatcher)
    {
        QFutureWatcher<CreativeCommitEvent> *watcher = m_commitWatcher;
        m_commitWatcher = nullptr;
        /* Qt's cancel only flips the cooperative flag; the worker polls both
         * the promise and our token at piece boundaries. */
        watcher->future().cancel();
        disconnect(watcher, nullptr, this, nullptr);
        watcher->deleteLater();
    }
    if (m_commit.token)
        m_commit.token->cancel();
    m_commit = CommitContext();
    ++m_commitRevision; /* any already-queued event is now stale */
    m_consumedResults = 0;

    if (!m_busy)
        return;
    m_lastCommitError.clear();
    m_busy = false;
    m_commitPercent = 0;
    if (notify)
    {
        setStatus(QStringLiteral("Rendering cancelled — nothing changed."));
        emit stateChanged();
        emit commitProgressChanged();
        emit commitFinished(QString());
    }
}

void PaintbrushCreative::onCommitResultReady(int index)
{
    consumeCommitResults(index);
}

void PaintbrushCreative::consumeCommitResults(int uptoInclusive)
{
    if (!m_commitWatcher)
        return;
    const QFuture<CreativeCommitEvent> future = m_commitWatcher->future();
    const int count = future.resultCount();
    while (m_consumedResults <= uptoInclusive && m_consumedResults < count)
    {
        applyCommitEvent(future.resultAt(m_consumedResults));
        ++m_consumedResults;
    }
}

void PaintbrushCreative::applyCommitEvent(const CreativeCommitEvent &event)
{
    if (!m_commit.token || m_commit.token->isCancelled())
        return;
    if (event.revision != m_commit.revision
        || event.revision != m_commitRevision)
        return;

    if (event.kind == CreativeCommitEvent::Progress)
    {
        if (!event.preview.isNull())
            publishCommitPreview(event.preview);
        if (event.percent != m_commitPercent)
        {
            m_commitPercent = qBound(0, event.percent, 100);
            emit commitProgressChanged();
        }
        return;
    }

    publishCommitResult(event.result);
}

void PaintbrushCreative::publishCommitPreview(const QImage &preview)
{
    if (preview.isNull())
        return;
    m_previewSize = preview.size();
    {
        QMutexLocker locker(&m_previewMutex);
        m_preview = preview;
    }
    ++m_previewRevision;
    emit previewChanged();
}

void PaintbrushCreative::publishCommitResult(const CreativeBuildResult &result)
{
    const QString suggestion = m_commit.suggestion;
    const int sourceLayerId = m_commit.sourceLayerId;
    const QString desc = m_commit.desc;

    if (result.cancelled)
    {
        m_lastCommitError.clear();
        m_busy = false;
        m_commitPercent = 0;
        m_commit = CommitContext();
        setStatus(QStringLiteral("Rendering cancelled — nothing changed."));
        emit stateChanged();
        emit commitProgressChanged();
        emit commitFinished(QString());
        return;
    }
    if (!result.error.isEmpty() || result.layers.isEmpty())
    {
        const QString error = result.error.isEmpty()
                                  ? QStringLiteral("The patch produced no layers.")
                                  : result.error;
        m_lastCommitError = error;
        m_busy = false;
        m_commitPercent = 0;
        m_commit = CommitContext();
        setStatus(error);
        emit stateChanged();
        emit commitProgressChanged();
        emit commitFinished(error);
        return;
    }

    /* The worker snapshot is in canvas coordinates. If the document canvas was
     * resized while it rendered, publishing would place the layers against a
     * different geometry; discard instead of committing a wrong result. */
    if (m_controller
        && m_controller->creativePadCanvasSize() != m_commit.canvasSize)
    {
        const QString error = QStringLiteral(
            "The canvas changed while the look was rendering. Try again.");
        m_lastCommitError = error;
        m_busy = false;
        m_commitPercent = 0;
        m_commit = CommitContext();
        setStatus(error);
        emit stateChanged();
        emit commitProgressChanged();
        emit commitFinished(error);
        return;
    }

    /* Publish the exact committed look while still busy, so no idle tick can
     * replace it between the preview and the document publication. */
    if (!result.preview.isNull())
        publishCommitPreview(result.preview);

    const QString error =
        m_controller
            ? m_controller->commitCreativeLayers(result.layers, sourceLayerId,
                                                 desc, m_resumeGroup)
            : QStringLiteral("The canvas is not available.");
    m_busy = false;
    m_commitPercent = 100;
    m_commit = CommitContext();
    if (!error.isEmpty())
    {
        m_lastCommitError = error;
        setStatus(error);
        emit stateChanged();
        emit commitProgressChanged();
        emit commitFinished(error);
        return;
    }

    m_lastCommitMs = double(m_commitClock.nsecsElapsed()) / 1.0e6;
    m_committed = true;
    m_resumeGroup.clear();
    m_suggestionKind = suggestionKindFor(m_patchId);
    m_suggestionText = suggestion;
    m_suggestionVisible = !m_suggestionText.isEmpty();
    m_lastCommitError.clear();
    refreshHistory();
    setStatus(QStringLiteral("Committed at canvas resolution — every piece is "
                             "an editable layer, one undo."));
    emit suggestionChanged();
    emit metricsChanged();
    emit stateChanged();
    emit commitProgressChanged();
    emit commitFinished(QString());
}

void PaintbrushCreative::onCommitProgress(int percent)
{
    if (!m_busy || !m_commit.token || m_commit.token->isCancelled())
        return;
    const int value = qBound(0, percent, 100);
    if (value == m_commitPercent)
        return;
    m_commitPercent = value;
    if (value > 0 && value < 100)
        setStatus(QStringLiteral("Rendering %1% — full canvas resolution.")
                      .arg(value));
    emit commitProgressChanged();
}

void PaintbrushCreative::onCommitFinished()
{
    if (!m_commitWatcher)
        return;
    const int count = m_commitWatcher->future().resultCount();
    consumeCommitResults(count - 1);

    if (m_busy)
    {
        /* No terminal event was published (early cancellation or a broken
         * future): close the job so the pad never stays stuck busy. */
        const bool cancelled =
            !m_commit.token || m_commit.token->isCancelled();
        m_lastCommitError =
            cancelled ? QString()
                      : QStringLiteral("Rendering stopped before it produced a "
                                       "result.");
        m_busy = false;
        m_commitPercent = 0;
        m_commit = CommitContext();
        setStatus(cancelled
                      ? QStringLiteral("Rendering cancelled — nothing changed.")
                      : m_lastCommitError);
        emit stateChanged();
        emit commitProgressChanged();
        emit commitFinished(m_lastCommitError);
    }
    finishCommitJob();
}

void PaintbrushCreative::finishCommitJob()
{
    QFutureWatcher<CreativeCommitEvent> *watcher = m_commitWatcher;
    m_commitWatcher = nullptr;
    if (watcher)
    {
        disconnect(watcher, nullptr, this, nullptr);
        watcher->deleteLater();
    }
    m_consumedResults = 0;
    m_commit.token.reset();
    m_commit = CommitContext();
}

QString PaintbrushCreative::waitForCommit(int timeoutMs)
{
    if (!m_busy)
        return m_lastCommitError;

    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    const QMetaObject::Connection conn =
        connect(this, &PaintbrushCreative::commitFinished, &loop,
                [&loop](const QString &) { loop.quit(); });
    connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(qMax(1, timeoutMs));
    loop.exec();
    disconnect(conn);

    if (m_busy)
    {
        /* Automation must not leave a runaway job behind on timeout. */
        cancelCommitInternal(false);
        m_lastCommitError =
            QStringLiteral("Timed out waiting for the creative render.");
        setStatus(m_lastCommitError);
        emit stateChanged();
        emit commitProgressChanged();
        emit commitFinished(m_lastCommitError);
    }
    return m_lastCommitError;
}

QString PaintbrushCreative::makeRiggedStage()
{
    if (!hasSubject() || !m_controller)
        return QStringLiteral("Pick a cutout first.");
    if (m_committed)
        return QStringLiteral("Already committed. Pick a patch to start a new run.");
    /* Stage is a product mode with a predictable 2D scene; the existing Stage
     * Door recipe supplies the editable curtains/floor/spotlight props. */
    m_patchId = QStringLiteral("stage-mode");
    m_chooserOpen = false;
    m_stageCommit = true;
    m_living = true;
    m_committed = false;
    rebuildPreview();
    ensureLivingPreview();
    return commit();
}

void PaintbrushCreative::acceptSuggestion()
{
    if (!m_suggestionVisible || m_subjectCanvasRect.isEmpty() || !m_controller)
        return;
    const PatchDef *def = patchDef(m_patchId);
    const QColor accent = def ? def->accent : QColor(255, 226, 120);
    QString done;
    if (m_suggestionKind == QLatin1String("calligraphy"))
    {
        m_controller->addCreativeCalligraphy(QStringLiteral("ink"));
        done = QStringLiteral("Calligraphy contour added — it is an editable "
                              "vector layer.");
    }
    else if (m_suggestionKind == QLatin1String("ribbon")
        || m_suggestionKind == QLatin1String("halo")
        || m_suggestionKind == QLatin1String("splat"))
    {
        const int variant = m_suggestionKind == QLatin1String("halo")
                                ? 1
                                : (m_suggestionKind == QLatin1String("splat") ? 2 : 0);
        m_controller->addCreativeRibbon(accent, m_subjectCanvasRect, variant,
                                        m_suggestionKind == QLatin1String("splat"));
        done = QStringLiteral("Stroke added — drag or undo it like any layer.");
    }
    else
    {
        const QPointF at(m_subjectCanvasRect.x() + m_subjectCanvasRect.width() * 0.78,
                         m_subjectCanvasRect.y() + m_subjectCanvasRect.height() * 0.22);
        const qreal size = qMax<qreal>(24.0,
                                       m_subjectCanvasRect.height() * 0.11);
        m_controller->addCreativeSparkle(accent, at, size);
        done = QStringLiteral("Sparkle added — move or undo it like any layer.");
    }
    m_suggestionVisible = false;
    m_suggestionText.clear();
    setStatus(done);
    emit suggestionChanged();
    emit stateChanged();
}

void PaintbrushCreative::dismissSuggestion()
{
    m_suggestionVisible = false;
    emit suggestionChanged();
}

void PaintbrushCreative::refreshHistory()
{
    if (!m_controller)
        return;
    const QVariantList next = m_controller->creativeLookHistory();
    if (next == m_history)
        return;
    m_history = next;
    emit historyChanged();
}

bool PaintbrushCreative::restoreFromCommittedLayer(int layerIndex)
{
    if (!m_controller)
        return false;
    QVariantMap info;
    QImage cutout;
    QRectF rect;
    QString label;
    if (!m_controller->creativeLookInfo(layerIndex, &info, &cutout, &rect, &label))
    {
        setStatus(QStringLiteral("This layer has no resumable creative look."));
        return false;
    }
    const QString patch = info.value(QStringLiteral("patch")).toString();
    const PatchDef *def = patchDef(patch);
    if (!def)
        return false;
    m_patchId = patch;
    m_chooserOpen = false;
    m_resumeGroup = info.value(QStringLiteral("group")).toString();
    if (!adoptSubject(cutout, rect,
                      info.value(QStringLiteral("sourceLayerId")).toInt(),
                      label, QStringLiteral("The source cutout is gone.")))
        return false;
    m_sourceHarvestId =
        info.value(QStringLiteral("sourceHarvestId")).toInt();
    refreshLivingSource();
    m_seed = quint32(info.value(QStringLiteral("seed")).toLongLong());
    if (m_seed == 0)
        m_seed = 1;
    const QVariantMap params =
        info.value(QStringLiteral("params")).toMap();
    m_frame.x = float(params.value(QStringLiteral("x"), 0.5).toDouble());
    m_frame.y = float(params.value(QStringLiteral("y"), 0.5).toDouble());
    m_frame.energy = float(params.value(QStringLiteral("energy"), 0.0).toDouble());
    m_frame.hold = params.value(QStringLiteral("hold"), false).toBool();
    m_committed = false;
    rebuildPreview();
    refreshHistory();
    setOpen(true);
    setStatus(QStringLiteral("Resumed %1. Adjust and make it real — the next "
                             "commit replaces this look in one undo.")
                  .arg(def->name));
    emit patchChanged();
    emit padChanged();
    emit stateChanged();
    return true;
}

QString PaintbrushCreative::applyLookToSelection()
{
    if (!hasSubject() || !m_controller)
        return QStringLiteral("Nothing to apply.");
    const QString patch = m_patchId;
    const quint32 seed = m_seed;
    const CreativeFrame frame = m_frame;
    openForSelection();
    if (!hasSubject())
        return QStringLiteral("Select a cutout layer first.");
    m_patchId = patch;
    m_seed = seed;
    m_frame = frame;
    m_resumeGroup.clear();
    m_committed = false;
    rebuildPreview();
    const QString error = commit();
    if (error.isEmpty())
        setStatus(QStringLiteral("Look applied to %1 — one undo.")
                      .arg(m_subjectLabel));
    return error;
}

bool PaintbrushCreative::dumpStagePieces(const QString &directory) const
{
    if (m_pieces.isEmpty())
        return false;
    QDir dir(directory);
    if (!dir.exists() && !QDir().mkpath(directory))
        return false;
    QFile meta(dir.filePath(QStringLiteral("pieces.txt")));
    if (!meta.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QTextStream out(&meta);
    for (int i = 0; i < m_pieces.size(); ++i)
    {
        const CreativePiece &p = m_pieces.at(i);
        const QString base = QStringLiteral("%1-%2")
                                 .arg(i, 2, 10, QLatin1Char('0'))
                                 .arg(p.name);
        const QString path = dir.filePath(base + QStringLiteral(".png"));
        p.image.save(path);
        out << i << " " << p.name << " rect=" << p.rect.x() << ","
            << p.rect.y() << " " << p.rect.width() << "x" << p.rect.height()
            << " pivot=" << p.pivot.x() << "," << p.pivot.y()
            << " blend=" << p.blendMode << " opacity=" << p.opacity
            << " image=" << p.image.width() << "x" << p.image.height()
            << "\n";
    }
    meta.close();
    return true;
}

/* --------------------------------------------------------------- composition */

QImage PaintbrushCreative::composeStage(const QVector<CreativePiece> &pieces,
                                        const QSize &stage,
                                        const QColor &background) const
{
    QImage out(stage, QImage::Format_ARGB32_Premultiplied);
    out.fill(background);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const int count = pieces.size();
    for (int i = 0; i < count; ++i)
    {
        const CreativePiece &piece = pieces.at(i);
        if (piece.image.isNull())
            continue;
        const CreativeMotion &m = piece.motion;
        float op = piece.opacity
                   + m.opacityX * m_frame.x
                   + m.opacityY * m_frame.y
                   + m.opacityEnergy * m_frame.energy
                   + m.opacityHold * (m_frame.hold ? 1.0f : 0.0f);
        if (piece.role == QLatin1String("rim")
            || piece.role == QLatin1String("accent"))
            op *= 1.0f + 0.035f * float(std::sin(m_frame.time * 1.8 + i * 1.3));
        op = clamp01(op);
        if (op <= 0.002f)
            continue;

        const qreal dx = (m.dxX * (m_frame.x - 0.5f)
                          + m.dxEnergy * m_frame.energy
                          + m.dxHold * (m_frame.hold ? 1.0f : 0.0f))
                         * stage.width();
        const qreal dy = (m.dyY * (m_frame.y - 0.5f)
                          + m.dyEnergy * m_frame.energy
                          + m.dyHold * (m_frame.hold ? 1.0f : 0.0f))
                         * stage.height();
        const qreal sc = 1.0 + m.scaleY * (m_frame.y - 0.5f)
                         + m.scaleEnergy * m_frame.energy
                         + m.scaleHold * (m_frame.hold ? 1.0 : 0.0);
        const qreal rot = m.rotationEnergy * m_frame.energy
                          + m.rotationHold * (m_frame.hold ? 1.0 : 0.0);

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

/* --------------------------------------- paper theatre / scene prop geometry
 *
 * These builders are the missing scene-layer delta shared by Paper Theatre and
 * Nocturne. Every shape is vector geometry (QPainterPath) rasterized at the
 * recipe's stage resolution: curtains, planes, trees, castle, fan, ribbon,
 * florals, stars, butterflies and the proscenium. A stable mt19937 stream plus
 * the recipe seed decides every jitter, so the same seed always produces the
 * same pixels and the committed layers reproduce after undo/reopen/export.
 * Nothing here samples the source cutout except the subject itself. */

namespace {

/* std::mt19937 is specified bit-for-bit; masking to 24 bits keeps the float
 * conversion identical across compilers (unlike generate_canonical). */
struct SceneRng
{
    std::mt19937 engine;
    explicit SceneRng(quint32 seed)
        : engine(seed ? seed : 0x9E3779B9u)
    {
    }
    quint32 next() { return quint32(engine()); }
    float unit() { return float(next() & 0xFFFFFFu) / float(0x1000000u); }
    float range(float a, float b) { return a + (b - a) * unit(); }
    int pick(int count) { return count <= 0 ? 0 : int(next() % quint32(count)); }
};

struct SceneProp
{
    QImage image;
    QPointF topLeft;
};

template <typename DrawFn>
SceneProp renderProp(const QSize &stage, const QRectF &bounds, DrawFn &&draw)
{
    if (!stage.isValid() || stage.isEmpty())
        return {};
    const QRectF clipped =
        bounds.intersected(QRectF(QPointF(0, 0), QSizeF(stage)));
    if (clipped.width() < 1.0 || clipped.height() < 1.0)
        return {};
    const QPointF topLeft(std::floor(clipped.left()),
                          std::floor(clipped.top()));
    const QSize size(
        qMax(1, int(std::ceil(clipped.right() - topLeft.x()))),
        qMax(1, int(std::ceil(clipped.bottom() - topLeft.y()))));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(-topLeft);
    draw(painter);
    painter.end();
    return { image, topLeft };
}

QPainterPath fivePointStar(const QPointF &center, qreal radius,
                           qreal innerRatio = 0.43,
                           qreal rotationDeg = -90.0)
{
    QPainterPath path;
    for (int i = 0; i < 10; ++i)
    {
        const qreal r = (i % 2 == 0) ? radius : radius * innerRatio;
        const qreal a = qDegreesToRadians(rotationDeg + i * 36.0);
        const QPointF point(center.x() + std::cos(a) * r,
                            center.y() + std::sin(a) * r);
        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    path.closeSubpath();
    return path;
}

QPainterPath irregularBlob(const QPointF &center, qreal rx, qreal ry,
                           SceneRng &rng, int points = 11,
                           qreal jitter = 0.16)
{
    QPainterPath path;
    for (int i = 0; i < points; ++i)
    {
        const qreal a = kTau * qreal(i) / qreal(points);
        const qreal r = 1.0 + rng.range(float(-jitter), float(jitter));
        const QPointF point(center.x() + std::cos(a) * rx * r,
                            center.y() + std::sin(a) * ry * r);
        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    path.closeSubpath();
    return path;
}

/* A cut-paper horizon plane. Returns a tight strip; the caller places it at
 * `topLeft` and the bottom is always the stage bottom. */
SceneProp makePaperPlane(const QSize &stage, const QColor &paper,
                         const QColor &core, qreal horizon, qreal amplitude,
                         qreal phase, int waves)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal top = qMax<qreal>(0.0, horizon - amplitude * 1.9 - h * 0.012);
    QVector<QPointF> points;
    const int steps = 38;
    for (int i = 0; i <= steps; ++i)
    {
        const qreal t = qreal(i) / qreal(steps);
        const qreal x = w * t;
        const qreal y = horizon
                        + std::sin(t * waves * kTau + phase) * amplitude
                        + std::sin(t * waves * 2.7 * kTau + phase * 1.7)
                              * amplitude * 0.32;
        points.append(QPointF(x, y));
    }
    return renderProp(stage, QRectF(0, top, w, h - top),
                      [&](QPainter &p) {
        QPainterPath path;
        path.moveTo(0, h);
        path.lineTo(points.first());
        for (int i = 1; i < points.size(); ++i)
            path.lineTo(points[i]);
        path.lineTo(w, h);
        path.closeSubpath();

        const qreal thickness = qMax<qreal>(1.5, h * 0.0045);
        QPainterPath back = path;
        back.translate(0, -thickness);
        p.setPen(Qt::NoPen);
        p.setBrush(core);
        p.drawPath(back);

        QLinearGradient g(0, horizon - amplitude, 0, h);
        g.setColorAt(0.0, paper.lighter(108));
        g.setColorAt(0.55, paper);
        g.setColorAt(1.0, paper.darker(118));
        p.setBrush(g);
        p.drawPath(path);

        QPainterPath edge;
        edge.moveTo(points.first());
        for (int i = 1; i < points.size(); ++i)
            edge.lineTo(points[i]);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0024), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(edge);
    });
}

SceneProp makePaperTree(const QSize &stage, quint32 seed, const QColor &paper,
                        const QColor &core, qreal x, qreal baseY,
                        qreal height, qreal spread)
{
    const qreal h = stage.height();
    const qreal top = baseY - height;
    SceneRng rng(seed);
    return renderProp(stage,
                      QRectF(x - spread * 1.35, top - height * 0.10,
                             spread * 2.7, height * 1.10),
                      [&](QPainter &p) {
        const qreal lean = rng.range(float(-spread * 0.10f),
                                     float(spread * 0.10f));
        const qreal trunkTop = baseY - height * 0.58;
        const qreal trunkW = qMax<qreal>(2.0, spread * 0.10);

        QPainterPath trunk;
        trunk.moveTo(x - trunkW * 0.62, baseY);
        trunk.quadTo(x - trunkW * 0.28 + lean * 0.4, baseY - height * 0.28,
                     x + lean - trunkW * 0.34, trunkTop);
        trunk.lineTo(x + lean + trunkW * 0.34, trunkTop);
        trunk.quadTo(x + trunkW * 0.30 + lean * 0.4, baseY - height * 0.28,
                     x + trunkW * 0.72, baseY);
        trunk.closeSubpath();
        QLinearGradient trunkFill(x, 0, x + trunkW, 0);
        trunkFill.setColorAt(0.0, paper.darker(132));
        trunkFill.setColorAt(0.55, paper.darker(112));
        trunkFill.setColorAt(1.0, paper.darker(142));
        p.setPen(Qt::NoPen);
        p.setBrush(trunkFill);
        p.drawPath(trunk);

        const QPointF canopyMain(x + lean * 0.35, baseY - height * 0.72);
        const QPointF canopyLeft(x - spread * 0.44, baseY - height * 0.57);
        const QPointF canopyRight(x + spread * 0.46, baseY - height * 0.60);
        const QPointF canopyTop(x + spread * 0.10, baseY - height * 0.90);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(paper.darker(125), qMax<qreal>(1.6, h * 0.0045),
                      Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(x + lean, trunkTop), canopyMain);
        p.drawLine(QPointF(x + lean * 0.7, baseY - height * 0.46),
                   canopyLeft);
        p.drawLine(QPointF(x + lean * 0.7, baseY - height * 0.50),
                   canopyRight);

        struct Blob
        {
            QPointF center;
            qreal rx;
            qreal ry;
        };
        const Blob blobs[] = {
            { canopyTop, spread * 0.42, height * 0.16 },
            { canopyLeft, spread * 0.47, height * 0.19 },
            { canopyRight, spread * 0.49, height * 0.20 },
            { canopyMain, spread * 0.62, height * 0.25 },
        };
        for (const Blob &blob : blobs)
        {
            const QPainterPath shape =
                irregularBlob(blob.center, blob.rx, blob.ry, rng);
            QLinearGradient fill(0, blob.center.y() - blob.ry, 0,
                                 blob.center.y() + blob.ry);
            fill.setColorAt(0.0, paper.lighter(110));
            fill.setColorAt(0.55, paper);
            fill.setColorAt(1.0, paper.darker(122));
            p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0022),
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(fill);
            p.drawPath(shape);
        }
    });
}

SceneProp makePaperCastle(const QSize &stage, quint32 seed, const QColor &paper,
                          const QColor &core, qreal centerX, qreal baseY,
                          qreal width, qreal height)
{
    const qreal h = stage.height();
    Q_UNUSED(seed);
    return renderProp(stage,
                      QRectF(centerX - width * 0.62, baseY - height * 1.32,
                             width * 1.24, height * 1.34),
                      [&](QPainter &p) {
        QLinearGradient fill(0, baseY - height, 0, baseY);
        fill.setColorAt(0.0, paper.lighter(108));
        fill.setColorAt(0.6, paper);
        fill.setColorAt(1.0, paper.darker(120));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);

        const qreal wallH = height * 0.44;
        const qreal wallTop = baseY - wallH;
        p.drawRect(QRectF(centerX - width * 0.44, wallTop,
                          width * 0.88, wallH));

        struct Tower
        {
            qreal cx;
            qreal tw;
            qreal th;
            qreal roof;
        };
        const Tower towers[] = {
            { centerX - width * 0.34, width * 0.18, height * 0.72,
              height * 0.13 },
            { centerX, width * 0.21, height * 0.98, height * 0.16 },
            { centerX + width * 0.34, width * 0.17, height * 0.68,
              height * 0.12 },
        };

        const auto crenellate = [&](qreal left, qreal right, qreal top) {
            const qreal cw = width * 0.030;
            const int count = qMax(2, int((right - left) / (cw * 1.9)));
            for (int i = 0; i < count; ++i)
            {
                const qreal cx = left + (right - left) * (qreal(i) + 0.5)
                                            / qreal(count);
                p.drawRect(QRectF(cx - cw / 2, top - cw * 0.75, cw,
                                  cw * 0.75));
            }
        };

        for (const Tower &tower : towers)
        {
            const qreal left = tower.cx - tower.tw / 2;
            const qreal top = baseY - tower.th;
            p.drawRect(QRectF(left, top, tower.tw, tower.th));
            crenellate(left, left + tower.tw, top);
            QPainterPath roof;
            roof.moveTo(left - tower.tw * 0.10, top);
            roof.lineTo(tower.cx, top - tower.roof);
            roof.lineTo(left + tower.tw + tower.tw * 0.10, top);
            roof.closeSubpath();
            p.setBrush(paper.darker(118));
            p.drawPath(roof);
            p.setBrush(fill);
        }
        crenellate(centerX - width * 0.44, centerX + width * 0.44, wallTop);

        /* cut-paper windows and gate: the backdrop must show through */
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setBrush(Qt::black);
        p.setPen(Qt::NoPen);
        for (const Tower &tower : towers)
        {
            const qreal ww = tower.tw * 0.30;
            const qreal wh = qMin<qreal>(height * 0.13, tower.th * 0.24);
            const qreal wy = baseY - tower.th * 0.56;
            p.drawRoundedRect(QRectF(tower.cx - ww / 2, wy, ww, wh),
                              ww / 2, ww / 2);
        }
        p.drawRoundedRect(QRectF(centerX - width * 0.055,
                                 baseY - height * 0.24, width * 0.11,
                                 height * 0.24),
                          width * 0.055, width * 0.055);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);

        /* paper edges */
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0022), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(centerX - width * 0.44, wallTop),
                   QPointF(centerX + width * 0.44, wallTop));
        for (const Tower &tower : towers)
        {
            const qreal left = tower.cx - tower.tw / 2;
            const qreal top = baseY - tower.th;
            p.drawLine(QPointF(left, top), QPointF(left + tower.tw, top));
            QPainterPath roof;
            roof.moveTo(left - tower.tw * 0.10, top);
            roof.lineTo(tower.cx, top - tower.roof);
            roof.lineTo(left + tower.tw + tower.tw * 0.10, top);
            p.drawPath(roof);
        }
        /* pennant on the tallest tower */
        const qreal flagBase = baseY - height * 0.98 - height * 0.16;
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.002)));
        p.drawLine(QPointF(centerX, flagBase),
                   QPointF(centerX, flagBase - height * 0.11));
        QPainterPath flag;
        flag.moveTo(centerX, flagBase - height * 0.11);
        flag.lineTo(centerX + width * 0.075, flagBase - height * 0.085);
        flag.lineTo(centerX, flagBase - height * 0.06);
        flag.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(core);
        p.drawPath(flag);
    });
}

SceneProp makePaperCurtain(const QSize &stage, const QColor &paper,
                           const QColor &core, bool left)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal side = w * 0.34;
    const QRectF bounds = left ? QRectF(0, 0, side, h)
                               : QRectF(w - side, 0, side, h);
    return renderProp(stage, bounds, [&](QPainter &p) {
        const qreal outerX = left ? 0.0 : w;
        const qreal dir = left ? 1.0 : -1.0;
        const auto X = [&](qreal local) { return outerX + dir * local; };

        QPainterPath path;
        path.moveTo(X(0), 0);
        path.lineTo(X(w * 0.31), 0);
        path.cubicTo(X(w * 0.26), h * 0.20, X(w * 0.17), h * 0.40,
                     X(w * 0.19), h * 0.56);
        path.cubicTo(X(w * 0.21), h * 0.72, X(w * 0.31), h * 0.88,
                     X(w * 0.30), h);
        path.lineTo(X(0), h);
        path.closeSubpath();

        QLinearGradient fill(left ? 0 : w, 0, left ? w * 0.32 : w * 0.68, 0);
        fill.setColorAt(0.0, paper.darker(158));
        fill.setColorAt(0.55, paper.darker(120));
        fill.setColorAt(1.0, paper);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(path);

        p.save();
        p.setClipPath(path);
        for (int i = 0; i < 7; ++i)
        {
            const qreal lx = w * (0.035 + i * 0.042);
            const qreal sway = w * (i % 2 == 0 ? 0.016 : -0.014);
            QPainterPath fold;
            fold.moveTo(X(lx), 0);
            fold.cubicTo(X(lx + sway), h * 0.30, X(lx - sway), h * 0.62,
                         X(lx + sway * 0.5), h);
            QColor foldColor = (i % 2 == 0) ? paper.lighter(112)
                                            : paper.darker(132);
            foldColor.setAlpha(72);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(foldColor, qMax<qreal>(1.2, w * 0.010),
                          Qt::SolidLine, Qt::RoundCap));
            p.drawPath(fold);
        }
        p.restore();

        QPainterPath edge;
        edge.moveTo(X(w * 0.31), 0);
        edge.cubicTo(X(w * 0.26), h * 0.20, X(w * 0.17), h * 0.40,
                     X(w * 0.19), h * 0.56);
        edge.cubicTo(X(w * 0.21), h * 0.72, X(w * 0.31), h * 0.88,
                     X(w * 0.30), h);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.2, h * 0.0032), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(edge);

        /* tie-back: a paper band pinching the drape */
        p.save();
        p.translate(X(w * 0.185), h * 0.56);
        p.rotate(left ? -7.0 : 7.0);
        QLinearGradient band(-w * 0.07, 0, w * 0.07, 0);
        band.setColorAt(0.0, paper.darker(150));
        band.setColorAt(0.5, paper.darker(126));
        band.setColorAt(1.0, paper.darker(160));
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0018)));
        p.setBrush(band);
        p.drawRoundedRect(QRectF(-w * 0.075, -h * 0.013, w * 0.15,
                                 h * 0.026),
                          h * 0.008, h * 0.008);
        p.restore();
    });
}

SceneProp makePaperFan(const QSize &stage, quint32 seed, const QColor &paper,
                       const QColor &alt, const QColor &core,
                       const QPointF &center, qreal radius, qreal startDeg,
                       qreal spanDeg)
{
    SceneRng rng(seed);
    const QRectF bounds(center.x() - radius * 1.25,
                        center.y() - radius * 1.25, radius * 2.5,
                        radius * 1.85);
    return renderProp(stage, bounds, [&](QPainter &p) {
        const int sectors = 9;
        const qreal step = spanDeg / qreal(sectors);
        const QRectF disc(center.x() - radius, center.y() - radius,
                          radius * 2.0, radius * 2.0);
        for (int i = 0; i < sectors; ++i)
        {
            QPainterPath sector;
            sector.moveTo(center);
            sector.arcTo(disc, startDeg + i * step, step);
            sector.closeSubpath();
            QColor c = (i % 2 == 0) ? paper : alt;
            c = c.lighter(int(rng.range(-4.0f, 8.0f)));
            c.setAlpha(232);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawPath(sector);
        }
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i <= sectors; ++i)
        {
            const qreal a = qDegreesToRadians(startDeg + i * step);
            const QPointF tick(center.x() + std::cos(a) * radius,
                               center.y() - std::sin(a) * radius);
            p.setPen(QPen(core, qMax<qreal>(0.9, radius * 0.012),
                          Qt::SolidLine, Qt::RoundCap));
            p.drawLine(center, tick);
        }
        p.setPen(QPen(core, qMax<qreal>(1.2, radius * 0.020),
                      Qt::SolidLine, Qt::RoundCap));
        p.drawArc(disc, int(std::lround(startDeg * 16.0)),
                  int(std::lround(spanDeg * 16.0)));
        p.setPen(QPen(core, qMax<qreal>(0.9, radius * 0.012)));
        p.setBrush(paper.darker(145));
        p.drawEllipse(center, radius * 0.055, radius * 0.055);

        /* tassel */
        p.setPen(QPen(core, qMax<qreal>(0.9, radius * 0.010)));
        p.drawLine(center + QPointF(0, radius * 0.05),
                   center + QPointF(0, radius * 0.30));
        QPainterPath drop;
        drop.moveTo(center + QPointF(0, radius * 0.28));
        drop.lineTo(center + QPointF(-radius * 0.035, radius * 0.34));
        drop.lineTo(center + QPointF(0, radius * 0.42));
        drop.lineTo(center + QPointF(radius * 0.035, radius * 0.34));
        drop.closeSubpath();
        p.setPen(QPen(core, qMax<qreal>(0.7, radius * 0.008)));
        p.setBrush(paper.darker(135));
        p.drawPath(drop);
    });
}

SceneProp makePaperRibbon(const QSize &stage, const QColor &paper,
                          const QColor &core)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    return renderProp(stage, QRectF(0, 0, w, h), [&](QPainter &p) {
        QPainterPath upper;
        upper.moveTo(-w * 0.05, h * 0.33);
        upper.cubicTo(w * 0.18, h * 0.12, w * 0.36, h * 0.48,
                      w * 0.54, h * 0.40);
        upper.cubicTo(w * 0.72, h * 0.32, w * 0.82, h * 0.08,
                      w * 1.05, h * 0.24);

        QPainterPath lower;
        lower.moveTo(-w * 0.05, h * 0.47);
        lower.cubicTo(w * 0.20, h * 0.63, w * 0.40, h * 0.31,
                      w * 0.60, h * 0.45);
        lower.cubicTo(w * 0.78, h * 0.58, w * 0.92, h * 0.51,
                      w * 1.05, h * 0.63);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(paper.darker(150), h * 0.033, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(upper);
        p.setPen(QPen(paper, h * 0.026, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPath(upper);
        p.setPen(QPen(core, h * 0.005, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPath(upper);

        p.setPen(QPen(paper.darker(160), h * 0.023, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(lower);
        p.setPen(QPen(paper.darker(112), h * 0.018, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(lower);

        /* paper twist where the two ribbons cross */
        p.save();
        p.translate(w * 0.555, h * 0.425);
        p.rotate(-26.0);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0018)));
        p.setBrush(paper.darker(128));
        p.drawRoundedRect(QRectF(-w * 0.030, -h * 0.012, w * 0.060,
                                 h * 0.024),
                          h * 0.006, h * 0.006);
        p.restore();
    });
}

SceneProp makePaperFloral(const QSize &stage, quint32 seed,
                          const QList<QColor> &palette,
                          const QPointF &origin, qreal sizeScale, bool mirror)
{
    if (palette.isEmpty() || sizeScale <= 1.0)
        return {};
    const qreal dir = mirror ? -1.0 : 1.0;
    SceneRng rng(seed);
    const QRectF bounds(origin.x() - sizeScale * 2.45,
                        origin.y() - sizeScale * 3.05, sizeScale * 4.90,
                        sizeScale * 3.25);
    const QColor stem = palette.value(qMin(2, palette.size() - 1)).darker(138);
    const QColor leaf = palette.value(qMin(2, palette.size() - 1));
    return renderProp(stage, bounds, [&](QPainter &p) {
        p.setRenderHint(QPainter::Antialiasing, true);
        struct Stem
        {
            QPointF tip;
            qreal bend;
        };
        QVector<Stem> stems;
        for (int i = 0; i < 3; ++i)
        {
            const qreal spread = sizeScale * (0.55 + 0.55 * i);
            const qreal lift = sizeScale * (2.25 - 0.35 * i);
            const QPointF tip(origin.x() + dir * spread
                                  + rng.range(-sizeScale * 0.08f,
                                              sizeScale * 0.08f),
                              origin.y() - lift);
            QPainterPath path;
            path.moveTo(origin + QPointF(dir * sizeScale * 0.10 * i, 0));
            const QPointF control(origin.x() + dir * spread * 0.55
                                      + rng.range(-sizeScale * 0.15f,
                                                  sizeScale * 0.15f),
                                  origin.y() - lift * 0.55);
            path.quadTo(control, tip);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(stem, qMax<qreal>(1.2, sizeScale * 0.055),
                          Qt::SolidLine, Qt::RoundCap));
            p.drawPath(path);
            stems.append({ tip, spread });
        }

        const auto drawLeaf = [&](const QPointF &at, qreal len, qreal angle) {
            p.save();
            p.translate(at);
            p.rotate(angle);
            QPainterPath leafPath;
            leafPath.moveTo(0, 0);
            leafPath.quadTo(len * 0.55, -len * 0.36, len, 0);
            leafPath.quadTo(len * 0.55, len * 0.36, 0, 0);
            p.setPen(QPen(stem.lighter(118), qMax<qreal>(0.7,
                                                         len * 0.035)));
            p.setBrush(leaf);
            p.drawPath(leafPath);
            p.restore();
        };
        for (int i = 0; i < stems.size(); ++i)
        {
            const Stem &s = stems.at(i);
            for (int j = 0; j < 3; ++j)
            {
                const qreal t = 0.28 + 0.22 * j;
                const QPointF at(origin.x() + dir * s.bend * t
                                      + (j % 2 == 0 ? -1 : 1)
                                            * sizeScale * 0.16,
                                  origin.y() - (origin.y() - s.tip.y()) * t);
                drawLeaf(at, sizeScale * rng.range(0.42f, 0.62f),
                         dir * rng.range(-52.0f, 52.0f));
            }
        }

        const auto drawFlower = [&](const QPointF &at, qreal radius,
                                    const QColor &petal,
                                    const QColor &heart) {
            const int petals = 6;
            for (int i = 0; i < petals; ++i)
            {
                const qreal a = kTau * qreal(i) / qreal(petals)
                                + rng.range(-0.10f, 0.10f);
                p.save();
                p.translate(at);
                p.rotate(qRadiansToDegrees(a));
                QPainterPath petalPath;
                const qreal len = radius * (0.92 + rng.range(-0.10f, 0.16f));
                petalPath.addEllipse(QPointF(radius * 0.55, 0.0),
                                     len * 0.60, radius * 0.30);
                p.setPen(QPen(heart.lighter(118), qMax<qreal>(0.7,
                                                               radius * 0.035)));
                p.setBrush(petal);
                p.drawPath(petalPath);
                p.restore();
            }
            p.setPen(QPen(heart, qMax<qreal>(0.8, radius * 0.045)));
            p.setBrush(heart);
            p.drawEllipse(at, radius * 0.24, radius * 0.24);
        };
        for (int i = 0; i < stems.size(); ++i)
        {
            const QColor petal = palette.at(
                (i + 1) % qMax(1, palette.size()));
            const QColor heart = palette.at(
                (i + 2) % qMax(1, palette.size()));
            drawFlower(stems.at(i).tip,
                       sizeScale * (0.46 + 0.10 * (i % 2)), petal, heart);
        }
        /* one low bud between the stems */
        drawFlower(origin + QPointF(dir * sizeScale * 0.20,
                                    -sizeScale * 0.95),
                   sizeScale * 0.28, palette.at(0),
                   palette.value(qMin(1, palette.size() - 1)));
    });
}

SceneProp makePaperButterflies(const QSize &stage, quint32 seed,
                               const QList<QColor> &palette,
                               const QList<QPointF> &anchors, int count,
                               qreal wingSize)
{
    if (palette.isEmpty() || anchors.isEmpty() || count <= 0)
        return {};
    SceneRng rng(seed);
    qreal minX = stage.width();
    qreal minY = stage.height();
    qreal maxX = 0.0;
    qreal maxY = 0.0;
    for (const QPointF &anchor : anchors)
    {
        minX = qMin(minX, anchor.x() - wingSize * 4.0);
        minY = qMin(minY, anchor.y() - wingSize * 4.0);
        maxX = qMax(maxX, anchor.x() + wingSize * 4.0);
        maxY = qMax(maxY, anchor.y() + wingSize * 4.0);
    }
    const QRectF bounds(minX, minY, maxX - minX, maxY - minY);
    return renderProp(stage, bounds, [&](QPainter &p) {
        for (int i = 0; i < count; ++i)
        {
            const QPointF anchor = anchors.at(i % anchors.size());
            const QPointF c(
                anchor.x() + rng.range(float(-wingSize * 2.2f),
                                       float(wingSize * 2.2f)),
                anchor.y() + rng.range(float(-wingSize * 2.2f),
                                       float(wingSize * 2.2f)));
            const qreal s = wingSize * rng.range(0.78f, 1.18f);
            const QColor base = palette.at(rng.pick(palette.size()));
            QColor paper = base;
            paper.setAlpha(236);
            QColor edge = base.lighter(138);
            edge.setAlpha(245);
            p.save();
            p.translate(c);
            p.rotate(rng.range(-32.0f, 32.0f));
            p.setPen(QPen(edge, qMax<qreal>(0.8, s * 0.035)));
            p.setBrush(paper);
            QPainterPath upperL;
            upperL.addEllipse(QPointF(-s * 0.52, -s * 0.34), s * 0.62,
                              s * 0.46);
            QPainterPath upperR;
            upperR.addEllipse(QPointF(s * 0.52, -s * 0.34), s * 0.62,
                              s * 0.46);
            QPainterPath lowerL;
            lowerL.addEllipse(QPointF(-s * 0.40, s * 0.34), s * 0.46,
                              s * 0.34);
            QPainterPath lowerR;
            lowerR.addEllipse(QPointF(s * 0.40, s * 0.34), s * 0.46,
                              s * 0.34);
            p.drawPath(upperL);
            p.drawPath(upperR);
            p.drawPath(lowerL);
            p.drawPath(lowerR);
            p.setPen(Qt::NoPen);
            QColor inner = edge.lighter(112);
            inner.setAlpha(150);
            p.setBrush(inner);
            p.drawEllipse(QPointF(-s * 0.52, -s * 0.30), s * 0.26,
                          s * 0.18);
            p.drawEllipse(QPointF(s * 0.52, -s * 0.30), s * 0.26, s * 0.18);
            p.setBrush(paper.darker(165));
            p.setPen(QPen(edge, qMax<qreal>(0.6, s * 0.02)));
            p.drawRoundedRect(QRectF(-s * 0.075, -s * 0.62, s * 0.15,
                                     s * 1.20),
                              s * 0.07, s * 0.07);
            p.setPen(QPen(edge, qMax<qreal>(0.6, s * 0.02),
                          Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(0, -s * 0.58),
                       QPointF(-s * 0.28, -s * 0.94));
            p.drawLine(QPointF(0, -s * 0.58),
                       QPointF(s * 0.28, -s * 0.94));
            p.restore();
        }
    });
}

SceneProp makePaperStarGarland(const QSize &stage, quint32 seed,
                               const QColor &paper, const QColor &core,
                               int count, qreal y, qreal sag)
{
    if (count <= 0)
        return {};
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal radius = qMax<qreal>(3.0, h * 0.018);
    SceneRng rng(seed);
    return renderProp(
        stage,
        QRectF(0, y - radius * 2.4, w, sag + radius * 5.0),
        [&](QPainter &p) {
        const qreal margin = w * 0.06;
        QPainterPath string;
        const int steps = 40;
        for (int i = 0; i <= steps; ++i)
        {
            const qreal t = qreal(i) / qreal(steps);
            const qreal x = margin + (w - margin * 2.0) * t;
            const qreal yy = y + std::sin(t * 3.141592653589793) * sag;
            if (i == 0)
                string.moveTo(x, yy);
            else
                string.lineTo(x, yy);
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(0.8, h * 0.0018)));
        p.drawPath(string);

        for (int i = 0; i < count; ++i)
        {
            const qreal t = (qreal(i) + 0.5) / qreal(count);
            const qreal x = margin + (w - margin * 2.0) * t;
            const qreal yy = y + std::sin(t * 3.141592653589793) * sag;
            const qreal r = radius * rng.range(0.70f, 1.10f);
            const QPointF c(x, yy + r * 1.35);
            p.setPen(QPen(core, qMax<qreal>(0.8, r * 0.10)));
            p.setBrush(paper);
            p.drawPath(fivePointStar(c, r, 0.43,
                                     rng.range(-16.0f, 16.0f)));
            p.setPen(QPen(core, qMax<qreal>(0.6, r * 0.06)));
            p.drawLine(QPointF(x, yy), QPointF(x, yy + r * 0.55));
        }
    });
}

SceneProp makePaperMoon(const QSize &stage, quint32 seed, const QColor &paper,
                        const QColor &core, const QPointF &center,
                        qreal radius)
{
    SceneRng rng(seed);
    return renderProp(stage,
                      QRectF(center.x() - radius * 1.25,
                             center.y() - radius * 1.25, radius * 2.5,
                             radius * 2.5),
                      [&](QPainter &p) {
        QRadialGradient disc(
            QPointF(center.x() - radius * 0.28,
                    center.y() - radius * 0.30),
            radius * 1.35);
        disc.setColorAt(0.0, paper.lighter(112));
        disc.setColorAt(0.62, paper);
        disc.setColorAt(1.0, paper.darker(114));
        p.setPen(QPen(core, qMax<qreal>(1.0, radius * 0.025)));
        p.setBrush(disc);
        p.drawEllipse(center, radius, radius);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(paper.darker(122), qMax<qreal>(0.8, radius * 0.012)));
        p.drawEllipse(center, radius * 0.82, radius * 0.82);
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 7; ++i)
        {
            const qreal a = rng.range(0.0f, float(kTau));
            const qreal d = radius * rng.range(0.06f, 0.68f);
            const qreal r = radius * rng.range(0.03f, 0.10f);
            QColor crater = paper.darker(112);
            crater.setAlpha(int(rng.range(32.0f, 76.0f)));
            p.setBrush(crater);
            p.drawEllipse(QPointF(center.x() + std::cos(a) * d,
                                  center.y() + std::sin(a) * d),
                          r, r * 0.86);
        }
    });
}

SceneProp makePaperForegroundLip(const QSize &stage, quint32 seed,
                                 const QColor &paper, const QColor &core,
                                 qreal topY)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    SceneRng rng(seed);
    return renderProp(stage,
                      QRectF(0, topY - h * 0.04, w, h - topY + h * 0.04),
                      [&](QPainter &p) {
        QVector<QPointF> edge;
        const int steps = 44;
        for (int i = 0; i <= steps; ++i)
        {
            const qreal t = qreal(i) / qreal(steps);
            const qreal x = w * t;
            const qreal y = topY + std::sin(t * 3.2 * kTau + 0.8) * h * 0.008
                            + rng.range(float(-h * 0.003f),
                                        float(h * 0.003f));
            edge.append(QPointF(x, y));
        }
        QPainterPath path;
        path.moveTo(0, h);
        path.lineTo(edge.first());
        for (int i = 1; i < edge.size(); ++i)
            path.lineTo(edge[i]);
        path.lineTo(w, h);
        path.closeSubpath();

        QLinearGradient fill(0, topY, 0, h);
        fill.setColorAt(0.0, paper.lighter(104));
        fill.setColorAt(0.25, paper);
        fill.setColorAt(1.0, paper.darker(158));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(path);

        QPainterPath rim;
        rim.moveTo(edge.first());
        for (int i = 1; i < edge.size(); ++i)
            rim.lineTo(edge[i]);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0030), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(rim);

        /* paper grass / board seams */
        p.setPen(Qt::NoPen);
        p.setBrush(paper.darker(128));
        for (int i = 0; i < 26; ++i)
        {
            const qreal t = (qreal(i) + rng.range(0.1f, 0.9f)) / 26.0;
            const int idx = qBound(0, int(t * (edge.size() - 1)),
                                   edge.size() - 1);
            const QPointF at = edge.at(idx);
            const qreal bh = h * rng.range(0.008f, 0.030f);
            const qreal bw = w * rng.range(0.0015f, 0.0035f);
            QPainterPath blade;
            blade.moveTo(at.x() - bw, at.y());
            blade.lineTo(at.x() + rng.range(-bw, bw), at.y() - bh);
            blade.lineTo(at.x() + bw, at.y());
            blade.closeSubpath();
            p.drawPath(blade);
        }
        p.setPen(QPen(paper.darker(175), qMax<qreal>(0.8, h * 0.0015)));
        p.drawLine(QPointF(0, topY + h * 0.045),
                   QPointF(w, topY + h * 0.045));
        p.setPen(QPen(paper.darker(185), qMax<qreal>(0.7, h * 0.0012)));
        p.drawLine(QPointF(0, topY + h * 0.085),
                   QPointF(w, topY + h * 0.085));
    });
}

SceneProp makePaperCornerFoliage(const QSize &stage, quint32 seed,
                                 const QColor &paper, const QColor &core,
                                 bool left)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal bw = w * 0.30;
    const qreal boundsW = w * 0.36;
    const qreal bh = h * 0.34;
    const QRectF bounds = left ? QRectF(0, 0, boundsW, bh)
                               : QRectF(w - boundsW, 0, boundsW, bh);
    SceneRng rng(seed);
    return renderProp(stage, bounds, [&](QPainter &p) {
        const qreal dir = left ? 1.0 : -1.0;
        const qreal ox = left ? 0.0 : w;
        const auto X = [&](qreal local) { return ox + dir * local; };
        QPainterPath branch;
        branch.moveTo(X(0), h * 0.06);
        branch.cubicTo(X(bw * 0.35), h * 0.04, X(bw * 0.62), h * 0.18,
                       X(bw * 0.94), h * 0.34);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(paper.darker(124), qMax<qreal>(1.4, h * 0.0060),
                      Qt::SolidLine, Qt::RoundCap));
        p.drawPath(branch);

        const int leaves = 11;
        for (int i = 0; i < leaves; ++i)
        {
            const qreal t = 0.12 + 0.82 * qreal(i) / qreal(leaves - 1);
            const QPointF at = branch.pointAtPercent(t);
            const qreal len = h * rng.range(0.022f, 0.046f);
            p.save();
            p.translate(at + QPointF(0, h * rng.range(-0.010f, 0.010f)));
            p.rotate(dir * rng.range(-62.0f, 58.0f));
            QPainterPath leaf;
            leaf.moveTo(0, 0);
            leaf.quadTo(len * 0.55, -len * 0.34, len, 0);
            leaf.quadTo(len * 0.55, len * 0.34, 0, 0);
            p.setPen(QPen(core, qMax<qreal>(0.6, len * 0.045)));
            p.setBrush(paper);
            p.drawPath(leaf);
            p.restore();
        }
    });
}

SceneProp makePaperProscenium(const QSize &stage, const QColor &paper,
                              const QColor &core)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal t = w * 0.060;
    return renderProp(stage, QRectF(0, 0, w, h), [&](QPainter &p) {
        QPainterPath frame;
        frame.moveTo(0, h);
        frame.lineTo(0, h * 0.34);
        frame.cubicTo(0, h * 0.10, w * 0.22, h * 0.03, w * 0.5, h * 0.03);
        frame.cubicTo(w * 0.78, h * 0.03, w, h * 0.10, w, h * 0.34);
        frame.lineTo(w, h);
        frame.lineTo(w - t, h);
        frame.lineTo(w - t, h * 0.35);
        frame.cubicTo(w - t, h * 0.15, w * 0.74, h * 0.09, w * 0.5,
                      h * 0.09);
        frame.cubicTo(w * 0.26, h * 0.09, t, h * 0.15, t, h * 0.35);
        frame.lineTo(t, h);
        frame.closeSubpath();

        QLinearGradient fill(0, 0, 0, h);
        fill.setColorAt(0.0, paper.darker(112));
        fill.setColorAt(0.55, paper);
        fill.setColorAt(1.0, paper.darker(140));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(frame);

        QPainterPath inner;
        inner.moveTo(w - t, h * 0.35);
        inner.cubicTo(w - t, h * 0.15, w * 0.74, h * 0.09, w * 0.5,
                      h * 0.09);
        inner.cubicTo(w * 0.26, h * 0.09, t, h * 0.15, t, h * 0.35);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.2, h * 0.0030), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(inner);

        /* scalloped valance */
        const qreal vh = h * 0.085;
        const int scallops = 9;
        QPainterPath valance;
        valance.moveTo(0, 0);
        valance.lineTo(w, 0);
        valance.lineTo(w, vh);
        for (int i = scallops; i > 0; --i)
        {
            const qreal x0 = w * qreal(i) / qreal(scallops);
            const qreal x1 = w * qreal(i - 1) / qreal(scallops);
            valance.quadTo((x0 + x1) / 2.0, vh + h * 0.018, x1, vh);
        }
        valance.closeSubpath();
        QLinearGradient vfill(0, 0, 0, vh + h * 0.02);
        vfill.setColorAt(0.0, paper.darker(128));
        vfill.setColorAt(1.0, paper.darker(106));
        p.setPen(Qt::NoPen);
        p.setBrush(vfill);
        p.drawPath(valance);

        QPainterPath vedge;
        vedge.moveTo(0, vh);
        for (int i = scallops; i > 0; --i)
        {
            const qreal x0 = w * qreal(i) / qreal(scallops);
            const qreal x1 = w * qreal(i - 1) / qreal(scallops);
            vedge.quadTo((x0 + x1) / 2.0, vh + h * 0.018, x1, vh);
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0026), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(vedge);
    });
}

SceneProp makePaperTitle(const QSize &stage, const QString &text,
                         const QColor &color, const QColor &shadowColor,
                         qreal heightFraction, qreal y)
{
    if (text.trimmed().isEmpty() || stage.isEmpty())
        return {};
    art::TextTreatment treatment;
    treatment.text = text;
    treatment.styleId = QStringLiteral("condensed.impact");
    treatment.color = color;
    float px = float(stage.height()) * float(heightFraction);
    QImage textImage;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        treatment.sizePx = px;
        textImage = art::typography::render(treatment, 1.0f);
        if (textImage.isNull()
            || textImage.width() <= int(stage.width() * 0.90))
            break;
        px *= 0.84f;
    }
    if (textImage.isNull())
        return {};
    art::TextTreatment shadowTreatment = treatment;
    shadowTreatment.color = QColor(shadowColor.red(), shadowColor.green(),
                                   shadowColor.blue(), 150);
    QImage shadowImage = art::typography::render(shadowTreatment, 1.0f);

    const qreal maxWidth = stage.width() * 0.92;
    if (textImage.width() > maxWidth)
    {
        const int target = qMax(1, int(maxWidth));
        textImage = textImage.scaledToWidth(target, Qt::SmoothTransformation);
        if (!shadowImage.isNull())
            shadowImage =
                shadowImage.scaledToWidth(target, Qt::SmoothTransformation);
    }
    const qreal x = (stage.width() - textImage.width()) / 2.0;
    const qreal shadowOffset = stage.height() * 0.0045;
    return renderProp(
        stage,
        QRectF(x - stage.width() * 0.015, y - shadowOffset,
               textImage.width() + stage.width() * 0.03,
               textImage.height() + shadowOffset * 2.5),
        [&](QPainter &p) {
        if (!shadowImage.isNull())
            p.drawImage(QPointF(x, y + shadowOffset), shadowImage);
        p.drawImage(QPointF(x, y), textImage);
    });
}

} /* namespace */

/* -------------------------------------------------------------- patch recipes */
QVector<CreativePiece> PaintbrushCreative::buildPatch(const QString &patchId,
                                                      const QImage &subject,
                                                      const QRectF &subjectRect,
                                                      const QSize &stage,
                                                      quint32 seed) const
{
    QVector<CreativePiece> pieces;
    if (subject.isNull() || !stage.isValid() || stage.isEmpty())
        return pieces;

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
        void operator()(const QString &name, const QString &role,
                        const QImage &image, const QRectF &rect,
                        int blend = BLEND_NORMAL, float opacity = 1.0f,
                        const CreativeMotion &motion = CreativeMotion()) const
        {
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
        }
    } add{ &pieces, subjectRect.center() };

    const QRectF full(QPointF(0, 0), QSizeF(stage));

    /* a real body response: the subject itself breathes and lifts with Y so
     * the pad is never just a subtle tint change */
    CreativeMotion subjectM;
    subjectM.scaleY = 0.060f;
    subjectM.dyY = -0.025f;
    subjectM.opacityY = 0.03f;

    auto drawAt = [](QImage &dst, const QImage &src, const QRectF &r) {
        drawScaled(dst, src, r);
    };
    auto band = [&](qreal fraction, const QColor &color, const QString &name,
                    const QString &role, float opacity, int blend,
                    const CreativeMotion &motion) {
        const int radius = qMax(1, int(std::lround(subjectRect.height() * fraction)));
        QImage bandMask = creative::bandFromMask(subjMask, radius);
        QImage fullBand = stageCanvas(stage);
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
    };
    auto subjectFull = [&](const QImage &graded, const QString &name,
                           float opacity, const CreativeMotion &motion) {
        QImage fullSubj = stageCanvas(stage);
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
    };
    auto solid = [&](const QImage &src, const QColor &color) {
        return tintPreserveAlpha(src, color);
    };
    auto addSceneProp = [&](const SceneProp &prop, const QString &name,
                            const QString &role, int blend, float opacity,
                            const CreativeMotion &motion,
                            const QString &propKind, float depth) {
        if (prop.image.isNull())
            return;
        CreativePiece p;
        p.name = name;
        p.role = role;
        p.image = prop.image;
        p.rect = QRectF(prop.topLeft, QSizeF(prop.image.size()));
        p.pivot = p.rect.center();
        p.blendMode = blend;
        p.opacity = opacity;
        p.motion = motion;
        p.metadata[QStringLiteral("creative.prop")] = propKind;
        p.metadata[QStringLiteral("creative.depth")] = double(depth);
        p.metadata[QStringLiteral("creative.parallax")] = double(motion.dxX);
        pieces.append(p);
    };
    auto addScenePropShadowed =
        [&](const SceneProp &prop, const QString &name, const QString &role,
            int blend, float opacity, const CreativeMotion &motion,
            const QString &propKind, float depth, const QColor &shadowColor,
            qreal shadowDx, qreal shadowDy, float shadowOpacity) {
            if (prop.image.isNull())
                return;
            const QImage shadow = creative::silhouetteShadow(
                prop.image, shadowColor, 1.0f);
            if (!shadow.isNull())
            {
                SceneProp shadowProp{ shadow,
                                      prop.topLeft
                                          + QPointF(shadowDx, shadowDy) };
                addSceneProp(
                    shadowProp, name + QStringLiteral(" shadow"),
                    role == QLatin1String("backdrop")
                        ? QStringLiteral("backdrop")
                        : QStringLiteral("echo"),
                    BLEND_NORMAL, shadowOpacity, motion,
                    propKind + QStringLiteral("-shadow"), depth - 0.02f);
            }
            addSceneProp(prop, name, role, blend, opacity, motion, propKind,
                         depth);
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

        QImage echo = creative::gradeImage(creative::softBlur(s, 7),
                                           creative::Grade{ 1.06f, 1.22f, 0.22f,
                                                            rose, 0.55f, cyan, 0.38f });
        QImage echoFull = stageCanvas(stage);
        drawAt(echoFull, echo,
               scaleRectAbout(subjectRect, 1.72, -W * 0.055, -H * 0.035));
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

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.05f, 1.12f, 1.20f, cobalt, 0.22f, coral, 0.10f });
        subjectFull(graded, QStringLiteral("Neon idol"), 1.0f, subjectM);

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
        fadeHorizontally(pieces[pieces.size() - 2].image, true);
        fadeHorizontally(pieces[pieces.size() - 1].image, false);

        CreativeMotion lightM;
        lightM.opacityY = 0.28f;
        add(QStringLiteral("Neon light"), QStringLiteral("finish"),
            creative::glossOverlay(stage, s, subjectRect, cream),
            full, BLEND_SCREEN, 0.30f, lightM);

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
        recolorM.scaleY = 0.045f;
        recolorM.dyY = -0.020f;
        recolorM.opacityY = 0.03f;
        subjectFull(voltageSubject, QStringLiteral("Voltage idol"), 1.0f,
                    recolorM);

        /* light that moves across the highlights */
        QImage sheen = creative::spectralSheen(
            s, subjMask, { cyan, rose, lime, white }, 0.95f, 62.0f,
            float(seed % 628) / 100.0f);
        QImage sheenFull = stageCanvas(stage);
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
        fadeHorizontally(pieces[pieces.size() - 2].image, true);
        fadeHorizontally(pieces[pieces.size() - 1].image, false);

        /* bolts land on the real silhouette, not on its bounding box */
        QImage subjectOnStage = stageCanvas(stage);
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

        QImage card = stageCanvas(stage);
        {
            QPainter p(&card);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 238));
            p.translate(subjectRect.center());
            p.rotate(-2.0);
            const QRectF r = scaleRectAbout(
                QRectF(-subjectRect.width() / 2.0, -subjectRect.height() / 2.0,
                       subjectRect.width(), subjectRect.height()),
                1.06, 0, H * 0.018);
            p.drawRoundedRect(r, H * 0.03, H * 0.03);
        }
        add(QStringLiteral("Sticker backing"), QStringLiteral("backdrop"),
            card, full, BLEND_NORMAL, 0.85f);

        QImage shadowSubj = creative::softBlur(solid(s, ink), 6);
        QImage shadowFull = stageCanvas(stage);
        drawAt(shadowFull, shadowSubj, subjectRect.translated(0, H * 0.012));
        add(QStringLiteral("Sticker shadow"), QStringLiteral("echo"),
            shadowFull, full, BLEND_NORMAL, 0.30f);

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
            QImage blueBand = stageCanvas(stage);
            drawAt(blueBand, creative::colorizeBand(bandMask, blue, false, seed + 9),
                   subjectRect);
            CreativeMotion blueM;
            blueM.opacityX = -0.28f;
            blueM.scaleHold = 0.12f;
            add(QStringLiteral("Sticker cut line ice"), QStringLiteral("rim"),
                blueBand, full, BLEND_NORMAL, 0.9f, blueM);
        }

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.07f, 1.05f, 1.18f, ink, 0.05f, yellow, 0.10f });
        subjectFull(graded, QStringLiteral("Sticker idol"), 1.0f, subjectM);

        CreativeMotion glossM;
        glossM.opacityY = 0.30f;
        glossM.opacityHold = 0.15f;
        add(QStringLiteral("Sticker gloss"), QStringLiteral("finish"),
            creative::glossOverlay(stage, s, subjectRect, QColor(255, 255, 255)),
            full, BLEND_SCREEN, 0.38f, glossM);

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

        QImage floor = stageCanvas(stage);
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

        QImage mirrored = creative::reflection(s, 0.55, 0.38);
        QImage mirrorFull = stageCanvas(stage);
        drawAt(mirrorFull, mirrored,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        add(QStringLiteral("Pastel reflection"), QStringLiteral("echo"),
            mirrorFull, full, BLEND_NORMAL, 0.55f);

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.10f, 0.94f, 0.92f, lavender, 0.16f,
                                QColor(255, 255, 255), 0.12f });
        subjectFull(graded, QStringLiteral("Pastel idol"), 1.0f, subjectM);

        CreativeMotion bloomM;
        bloomM.opacityY = 0.20f;
        bloomM.opacityHold = 0.30f;
        bloomM.scaleHold = 0.07f;
        bloomM.dxX = 0.035f;
        add(QStringLiteral("Pastel bloom"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.30, QColor(255, 255, 255), 0.45f),
            full, BLEND_SCREEN, 0.5f, bloomM);

        CreativeMotion confettiM;
        confettiM.opacityEnergy = 0.30f;
        confettiM.scaleEnergy = 0.12f;
        confettiM.opacityX = 0.25f;
        confettiM.dxX = 0.030f;
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

        /* Scene-layer delta: velvet depth, a paper moon, a distant silhouette
         * and a second star plane. The existing night palette is untouched;
         * these sit behind the existing stars/aura/rims. */
        add(QStringLiteral("Nocturne velvet depth"), QStringLiteral("backdrop"),
            creative::velvetField(stage, seed + 41, QColor(10, 6, 24), 0.85f),
            full, BLEND_MULTIPLY, 0.55f);

        CreativeMotion moonM;
        moonM.dxX = 0.016f;
        moonM.dyY = 0.008f;
        addScenePropShadowed(
            makePaperMoon(stage, seed + 43, QColor(222, 216, 246), lilac,
                          QPointF(W * 0.73, H * 0.21), H * 0.100),
            QStringLiteral("Nocturne paper moon"),
            QStringLiteral("backdrop"), BLEND_SCREEN, 0.72f, moonM,
            QStringLiteral("moon"), -0.32f, QColor(6, 4, 16), W * 0.005,
            H * 0.008, 0.40f);

        CreativeMotion castleM;
        castleM.dxX = 0.026f;
        castleM.dyY = 0.009f;
        addScenePropShadowed(
            makePaperCastle(stage, seed + 47, QColor(14, 10, 30), violet,
                            W * 0.52, H * 0.64, W * 0.30, H * 0.17),
            QStringLiteral("Nocturne distant castle"),
            QStringLiteral("backdrop"), BLEND_NORMAL, 0.70f, castleM,
            QStringLiteral("castle"), -0.54f, QColor(4, 3, 12), W * 0.004,
            H * 0.007, 0.48f);

        CreativeMotion distantM;
        distantM.dxX = 0.020f;
        distantM.dyY = 0.008f;
        addScenePropShadowed(
            makePaperPlane(stage, QColor(10, 8, 24), violet, H * 0.66,
                           H * 0.032, 2.2, 2),
            QStringLiteral("Nocturne distant silhouettes"),
            QStringLiteral("backdrop"), BLEND_NORMAL, 0.80f, distantM,
            QStringLiteral("plane-back"), -0.60f, QColor(4, 3, 12),
            W * 0.004, H * 0.007, 0.42f);

        CreativeMotion bloomStarsM;
        bloomStarsM.dxX = 0.012f;
        bloomStarsM.dyY = 0.006f;
        add(QStringLiteral("Nocturne star bloom"), QStringLiteral("backdrop"),
            creative::starField(stage, seed + 31,
                                { lilac, silver, teal }, 140),
            full, BLEND_SCREEN, 0.55f, bloomStarsM);

        add(QStringLiteral("Nocturne stars"), QStringLiteral("backdrop"),
            creative::starField(stage, seed, silver, 220), full,
            BLEND_SCREEN, 0.75f);

        CreativeMotion auraM;
        auraM.opacityY = 0.16f;
        auraM.opacityHold = 0.28f;
        auraM.scaleHold = 0.06f;
        add(QStringLiteral("Nocturne aura"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.48, violet, 0.55f),
            full, BLEND_SCREEN, 0.6f, auraM);

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 0.94f, 1.18f, 0.88f, QColor(40, 18, 90), 0.34f,
                                silver, 0.12f });
        subjectFull(graded, QStringLiteral("Nocturne figure"), 1.0f,
                    CreativeMotion());

        CreativeMotion rimVioletM;
        rimVioletM.opacityX = 0.30f;
        rimVioletM.opacityHold = 0.20f;
        band(0.010, lilac, QStringLiteral("Nocturne rim"), QStringLiteral("rim"),
             0.75f, BLEND_SCREEN, rimVioletM);
        CreativeMotion rimTealM;
        rimTealM.opacityX = -0.30f;
        band(0.010, teal, QStringLiteral("Nocturne rim teal"),
             QStringLiteral("rim"), 0.55f, BLEND_SCREEN, rimTealM);
        fadeHorizontally(pieces[pieces.size() - 2].image, true);
        fadeHorizontally(pieces[pieces.size() - 1].image, false);

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

        /* Foreground scene layer: velvet edge, edge silhouettes and near
         * florals/butterflies. They occlude only the frame edges; the figure
         * stays readable. */
        add(QStringLiteral("Nocturne velvet edge"), QStringLiteral("finish"),
            creative::velvetField(stage, seed + 53, QColor(8, 4, 20), 0.70f),
            full, BLEND_MULTIPLY, 0.32f);

        CreativeMotion fgTreeLM;
        fgTreeLM.dxX = 0.105f;
        fgTreeLM.dyY = 0.020f;
        addScenePropShadowed(
            makePaperTree(stage, seed + 57, QColor(8, 7, 18), violet,
                          W * 0.02, H * 1.01, H * 0.70, W * 0.15),
            QStringLiteral("Nocturne foreground silhouette left"),
            QStringLiteral("accent"), BLEND_NORMAL, 0.94f, fgTreeLM,
            QStringLiteral("foreground"), 0.52f, QColor(3, 2, 10),
            W * 0.004, H * 0.007, 0.44f);
        CreativeMotion fgTreeRM;
        fgTreeRM.dxX = -0.105f;
        fgTreeRM.dyY = 0.020f;
        addScenePropShadowed(
            makePaperTree(stage, seed + 59, QColor(8, 7, 18), violet,
                          W * 0.98, H * 1.01, H * 0.66, W * 0.14),
            QStringLiteral("Nocturne foreground silhouette right"),
            QStringLiteral("accent"), BLEND_NORMAL, 0.94f, fgTreeRM,
            QStringLiteral("foreground"), 0.52f, QColor(3, 2, 10),
            W * 0.004, H * 0.007, 0.44f);

        CreativeMotion fgFloralLM;
        fgFloralLM.dxX = 0.070f;
        fgFloralLM.dyY = 0.016f;
        addSceneProp(
            makePaperFloral(stage, seed + 61,
                            { lilac, silver, teal, violet },
                            QPointF(W * 0.10, H * 0.90), H * 0.078, false),
            QStringLiteral("Nocturne floral left"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.90f, fgFloralLM, QStringLiteral("floral"), 0.34f);
        CreativeMotion fgFloralRM;
        fgFloralRM.dxX = -0.070f;
        fgFloralRM.dyY = 0.016f;
        addSceneProp(
            makePaperFloral(stage, seed + 63,
                            { lilac, silver, teal, violet },
                            QPointF(W * 0.90, H * 0.92), H * 0.074, true),
            QStringLiteral("Nocturne floral right"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.90f, fgFloralRM, QStringLiteral("floral"), 0.34f);

        CreativeMotion nearFlightM;
        nearFlightM.rotationEnergy = 6.0f;
        nearFlightM.dxEnergy = 0.008f;
        addSceneProp(
            makePaperButterflies(
                stage, seed + 67, { lilac, silver, teal },
                { QPointF(subjectRect.x() + subjectRect.width() * 0.08,
                          subjectRect.y() + subjectRect.height() * 0.30),
                  QPointF(subjectRect.right() - subjectRect.width() * 0.06,
                          subjectRect.y() + subjectRect.height() * 0.52) },
                3, H * 0.030),
            QStringLiteral("Nocturne near butterflies"),
            QStringLiteral("accent"), BLEND_NORMAL, 0.86f, nearFlightM,
            QStringLiteral("butterflies"), 0.30f);

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

        QImage sheen = creative::spectralSheen(
            s, subjMask, { cyan, pink, gold, silver }, 1.0f, -28.0f,
            float(seed % 628) / 100.0f);
        QImage sheenFull = stageCanvas(stage);
        drawAt(sheenFull, sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY = 0.30f;
        sheenM.opacityHold = 0.20f;
        add(QStringLiteral("Liquid sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN, 0.92f, sheenM);

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

        CreativeMotion prismM;
        prismM.opacityEnergy = 0.30f;
        prismM.opacityHold = 0.25f;
        add(QStringLiteral("Prism sparkle"), QStringLiteral("accent"),
            creative::starField(stage, seed + 4, { silver, gold, cyan }, 80),
            full, BLEND_SCREEN, 0.62f, prismM);

        QImage mirror = creative::reflection(s, 0.20, 0.15);
        QImage mirrorFull = stageCanvas(stage);
        drawAt(mirrorFull, mirror,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        CreativeMotion mirrorM;
        mirrorM.opacityY = -0.10f;
        add(QStringLiteral("Holo reflection"), QStringLiteral("echo"),
            mirrorFull, full, BLEND_SCREEN, 0.45f, mirrorM);
    }
    else if (isHoloMaterialPatch(patchId))
    {
        /* One material engine, surface-specific presets. Fashion recipes keep
         * a film-like replace value; the automotive recipes let the palette
         * own the paint hue while the source lightness keeps panel shading,
         * clearcoat, flake and the view-angle colour shift. */
        const creative::HoloMaterial material =
            creative::holoMaterialPreset(holoMaterialIdForPatch(patchId));
        const bool automotive = isAutomotivePatch(patchId);
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

        QImage confidence;
        const QImage normals = creative::estimateNormals(
            s, subjMask, material.normalStrength, &confidence);
        /* Coverage comes from the existing subject mask (the layer's editable
         * mask/cutout), so windows or trim the user has masked out are already
         * excluded. When a part/detail protection plane exists it is honored
         * by the engine; this path never derives new segmentation. */
        const creative::HoloMaterialResult mat =
            creative::holographicMaterial(
                s, subjMask, normals, material, 1.0f, seed,
                creative::CoverageMode::InheritedCoverage, confidence);
        CreativeMotion holoM;
        holoM.opacityX = 0.06f;
        holoM.opacityY = 0.14f;
        subjectFull(mat.albedo, material.name, 1.0f, holoM);

        QImage sheenFull = stageCanvas(stage);
        drawAt(sheenFull, mat.sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY = 0.30f;
        sheenM.opacityHold = 0.20f;
        add(QStringLiteral("Material sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN, 0.92f, sheenM);

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
        else if (holoMaterialIdForPatch(patchId)
                 == creative::HoloMaterialId::GlitterMetallic)
        {
            CreativeMotion flakeM;
            flakeM.opacityEnergy = 0.25f;
            add(QStringLiteral("Glitter sparkle"), QStringLiteral("accent"),
                creative::starField(stage, seed + 4, { c3, c0 }, 55),
                full, BLEND_SCREEN, 0.55f, flakeM);
        }

        QImage mirror = creative::reflection(s, 0.20, 0.15);
        QImage mirrorFull = stageCanvas(stage);
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

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.02f, 1.30f, 0.35f, ink, 0.20f,
                                QColor(255, 255, 255), 0.05f });
        subjectFull(graded, QStringLiteral("Ink figure"), 1.0f, CreativeMotion());

        CreativeMotion lineM;
        lineM.opacityX = 0.20f;
        lineM.opacityEnergy = 0.25f;
        band(0.006, ink, QStringLiteral("Ink line"), QStringLiteral("rim"),
             0.9f, BLEND_NORMAL, lineM);
        CreativeMotion lineSoftM;
        lineSoftM.opacityX = -0.20f;
        band(0.014, QColor(20, 18, 16, 120), QStringLiteral("Ink halo"),
             QStringLiteral("rim"), 0.35f, BLEND_NORMAL, lineSoftM);

        CreativeMotion splatM;
        splatM.opacityEnergy = 0.35f;
        splatM.scaleEnergy = 0.15f;
        add(QStringLiteral("Ink splatter"), QStringLiteral("accent"),
            creative::inkSplatter(stage, seed,
                                  subjectRect.adjusted(-W * 0.12, -H * 0.10,
                                                       W * 0.12, H * 0.10),
                                  ink, 80),
            full, BLEND_NORMAL, 0.7f, splatM);

        QImage eyes = stageCanvas(stage);
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

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.05f, 1.10f, 1.12f, QColor(40, 20, 80), 0.18f,
                                gold, 0.10f });
        subjectFull(graded, QStringLiteral("Storm figure"), 1.0f,
                    CreativeMotion());

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
        QImage subjectCanvas = stageCanvas(stage);
        drawAt(subjectCanvas, s, subjectRect);
        CreativeMotion k6M;
        k6M.rotationEnergy = 24.0f;
        k6M.opacityX = 0.15f;
        add(QStringLiteral("Kaleido six"), QStringLiteral("subject"),
            creative::wedgeKaleidoscope(subjectCanvas, stage, 6, 1.0, 0.0),
            full, BLEND_NORMAL, 0.9f, k6M);
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
        const qreal spacing = subjectRect.width() * 0.26;
        for (int i = 0; i < 6; ++i)
        {
            const int k = i - 3;
            if (k == 0)
                continue;
            QImage echo = creative::hueShift(s, k * 14);
            echo = creative::gradeImage(
                echo, creative::Grade{ 0.92f, 1.08f, 0.9f,
                                       k < 0 ? blue : pink, 0.22f,
                                       QColor(255, 255, 255), 0.05f });
            QImage echoFull = stageCanvas(stage);
            drawAt(echoFull, echo,
                   subjectRect.translated(k * spacing, k * spacing * 0.05));
            CreativeMotion echoM;
            echoM.opacityX = 0.06f;
            echoM.dxX = 0.012f * k;
            add(QStringLiteral("Echo %1").arg(k > 0 ? k : -k),
                QStringLiteral("echo"), echoFull, full, BLEND_NORMAL,
                float(0.48 - qAbs(k) * 0.070), echoM);
        }
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
    else if (patchId == QLatin1String("scene-forge"))
    {
        const QColor cream(244, 230, 200), teal(34, 92, 84),
            ochre(214, 154, 64), ink(36, 27, 20), rust(178, 70, 44);
        add(QStringLiteral("Paper sky"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(248, 238, 214),
                                    QColor(226, 198, 156),
                                    QColor(255, 255, 255, 160), 0.18),
            full);

        const auto hill = [&](const QColor &color, qreal y, qreal amp,
                              int phase) {
            QImage img = stageCanvas(stage);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            QPainterPath path;
            path.moveTo(0, H);
            path.lineTo(0, y);
            const int steps = 8;
            for (int i = 0; i < steps; ++i)
            {
                const qreal x0 = W * i / steps;
                const qreal x1 = W * (i + 1) / steps;
                const qreal yy = y + std::sin((i + phase) * 1.7) * amp;
                path.quadTo((x0 + x1) / 2.0,
                            y + std::sin((i + phase + 0.5) * 1.7) * amp * 1.2,
                            x1, yy);
            }
            path.lineTo(W, H);
            path.closeSubpath();
            p.drawPath(path);
            return img;
        };
        CreativeMotion hillBackM;
        hillBackM.dxX = 0.018f;
        add(QStringLiteral("Back hill"), QStringLiteral("backdrop"),
            hill(teal, H * 0.46, H * 0.045, 0), full, BLEND_NORMAL, 0.95f,
            hillBackM);
        CreativeMotion hillMidM;
        hillMidM.dxX = -0.022f;
        add(QStringLiteral("Mid hill"), QStringLiteral("backdrop"),
            hill(ochre, H * 0.57, H * 0.032, 1), full, BLEND_NORMAL, 0.95f,
            hillMidM);
        add(QStringLiteral("Front stage"), QStringLiteral("backdrop"),
            hill(ink, H * 0.72, H * 0.012, 2), full, BLEND_NORMAL, 1.0f);

        const auto curtain = [&](bool left) {
            QImage img = stageCanvas(stage);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath path;
            if (left)
            {
                path.moveTo(0, 0);
                path.lineTo(W * 0.26, 0);
                path.cubicTo(W * 0.18, H * 0.35, W * 0.27, H * 0.70,
                             W * 0.16, H);
                path.lineTo(0, H);
            }
            else
            {
                path.moveTo(W, 0);
                path.lineTo(W * 0.74, 0);
                path.cubicTo(W * 0.82, H * 0.35, W * 0.73, H * 0.70,
                             W * 0.84, H);
                path.lineTo(W, H);
            }
            path.closeSubpath();
            QLinearGradient g(0, 0, W * (left ? 0.3 : 0.7), 0);
            g.setColorAt(0.0, rust.darker(170));
            g.setColorAt(0.55, rust);
            g.setColorAt(1.0, rust.lighter(125));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawPath(path);
            p.setPen(QPen(QColor(60, 12, 20, 110), qMax(1.0, W * 0.003)));
            for (int i = 1; i <= 3; ++i)
            {
                const qreal x = W * (left ? 0.06 + 0.05 * i
                                          : 0.94 - 0.05 * i);
                p.drawLine(QPointF(x, 0), QPointF(x + (left ? 8 : -8), H));
            }
            return img;
        };
        CreativeMotion curtainL;
        curtainL.dxX = -0.055f;
        curtainL.dxHold = -0.045f;
        add(QStringLiteral("Curtain left"), QStringLiteral("backdrop"),
            curtain(true), full, BLEND_NORMAL, 0.95f, curtainL);
        CreativeMotion curtainR;
        curtainR.dxX = 0.055f;
        curtainR.dxHold = 0.045f;
        add(QStringLiteral("Curtain right"), QStringLiteral("backdrop"),
            curtain(false), full, BLEND_NORMAL, 0.95f, curtainR);

        QImage cone = stageCanvas(stage);
        {
            QPainter p(&cone);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath beam;
            beam.moveTo(W * 0.50, H * 0.04);
            beam.lineTo(W * 0.16, H * 0.82);
            beam.lineTo(W * 0.84, H * 0.82);
            beam.closeSubpath();
            QLinearGradient g(0, H * 0.04, 0, H * 0.82);
            g.setColorAt(0.0, QColor(255, 240, 190, 120));
            g.setColorAt(1.0, QColor(255, 230, 170, 0));
            p.fillPath(beam, g);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 240, 200, 90));
            p.drawEllipse(QPointF(W * 0.5, H * 0.82), W * 0.34, H * 0.035);
        }
        CreativeMotion spotM;
        spotM.opacityY = 0.20f;
        spotM.opacityHold = 0.15f;
        spotM.dxX = 0.035f;
        add(QStringLiteral("Spotlight"), QStringLiteral("accent"), cone, full,
            BLEND_SCREEN, 0.75f, spotM);

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.06f, 1.10f, 1.04f, teal, 0.12f, cream, 0.12f });
        subjectFull(graded, QStringLiteral("Paper figure"), 1.0f,
                    CreativeMotion());
        CreativeMotion starM;
        starM.opacityEnergy = 0.20f;
        add(QStringLiteral("Paper stars"), QStringLiteral("accent"),
            creative::starField(stage, seed, QColor(255, 244, 210), 80),
            full, BLEND_SCREEN, 0.5f, starM);
        add(QStringLiteral("Paper grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 5, 30), full,
            BLEND_OVERLAY, 0.35f);
    }
    else if (patchId == QLatin1String("paper-theatre"))
    {
        const QColor cream(246, 233, 208), sand(232, 210, 172),
            ochre(214, 154, 64), rust(172, 72, 52), teal(38, 96, 86),
            ink(38, 27, 20), rose(191, 86, 96), dusk(70, 40, 64);

        add(QStringLiteral("Paper stock"), QStringLiteral("backdrop"),
            creative::paperStockField(stage, seed, cream, ochre, 0.85f), full);
        add(QStringLiteral("Paper dusk wash"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(255, 248, 232, 0),
                                    QColor(66, 34, 52, 120),
                                    QColor(255, 226, 170, 96), 0.30),
            full, BLEND_MULTIPLY, 0.55f);

        CreativeMotion moonM;
        moonM.dxX = 0.018f;
        moonM.dyY = 0.008f;
        const SceneProp moon = makePaperMoon(
            stage, seed + 3, QColor(252, 242, 214), ochre,
            QPointF(W * 0.62, H * 0.21), H * 0.085);
        addScenePropShadowed(moon, QStringLiteral("Paper moon"),
                             QStringLiteral("backdrop"), BLEND_NORMAL, 0.92f,
                             moonM, QStringLiteral("moon"), -0.30f,
                             QColor(52, 30, 30), W * 0.006, H * 0.010, 0.30f);

        CreativeMotion archM;
        archM.dxX = 0.012f;
        archM.dyY = 0.006f;
        addSceneProp(makePaperProscenium(stage, rust, cream),
                     QStringLiteral("Paper proscenium"),
                     QStringLiteral("backdrop"), BLEND_NORMAL, 1.0f, archM,
                     QStringLiteral("proscenium"), -0.45f);

        CreativeMotion castleM;
        castleM.dxX = 0.030f;
        castleM.dyY = 0.010f;
        addScenePropShadowed(
            makePaperCastle(stage, seed + 5, ochre.darker(118), cream,
                            W * 0.56, H * 0.61, W * 0.32, H * 0.20),
            QStringLiteral("Paper castle"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.92f, castleM, QStringLiteral("castle"), -0.48f,
            QColor(58, 32, 28), W * 0.005, H * 0.009, 0.34f);

        CreativeMotion backM;
        backM.dxX = 0.020f;
        backM.dyY = 0.008f;
        addScenePropShadowed(
            makePaperPlane(stage, sand, cream, H * 0.50, H * 0.045, 0.5, 2),
            QStringLiteral("Paper back plane"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.96f, backM, QStringLiteral("plane-back"), -0.60f,
            QColor(60, 34, 30), W * 0.004, H * 0.008, 0.30f);

        CreativeMotion treeLM;
        treeLM.dxX = 0.045f;
        treeLM.dyY = 0.014f;
        addScenePropShadowed(
            makePaperTree(stage, seed + 7, teal.darker(108), cream, W * 0.30,
                          H * 0.72, H * 0.42, W * 0.10),
            QStringLiteral("Paper tree left"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.95f, treeLM, QStringLiteral("tree"), -0.36f,
            QColor(40, 30, 28), W * 0.006, H * 0.010, 0.36f);
        CreativeMotion treeRM;
        treeRM.dxX = 0.050f;
        treeRM.dyY = 0.014f;
        addScenePropShadowed(
            makePaperTree(stage, seed + 11, teal.darker(122), cream, W * 0.70,
                          H * 0.74, H * 0.38, W * 0.10),
            QStringLiteral("Paper tree right"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.95f, treeRM, QStringLiteral("tree"), -0.35f,
            QColor(40, 30, 28), W * 0.006, H * 0.010, 0.36f);

        CreativeMotion midM;
        midM.dxX = -0.035f;
        midM.dyY = 0.012f;
        addScenePropShadowed(
            makePaperPlane(stage, ochre, cream, H * 0.70, H * 0.030, 1.7, 3),
            QStringLiteral("Paper mid plane"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.96f, midM, QStringLiteral("plane-mid"), -0.20f,
            QColor(52, 32, 28), W * 0.004, H * 0.008, 0.32f);

        CreativeMotion fanM;
        fanM.dxX = -0.055f;
        fanM.rotationHold = 3.0f;
        addScenePropShadowed(
            makePaperFan(stage, seed + 13, rose, cream, ink,
                         QPointF(W * 0.30, H * 0.73), H * 0.165, 195.0,
                         150.0),
            QStringLiteral("Paper fan"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.95f, fanM, QStringLiteral("fan"), 0.05f,
            QColor(48, 28, 28), W * 0.005, H * 0.008, 0.32f);

        CreativeMotion ribbonM;
        ribbonM.dxX = 0.070f;
        ribbonM.dyY = 0.016f;
        ribbonM.rotationEnergy = 2.0f;
        addScenePropShadowed(makePaperRibbon(stage, rust, cream),
                             QStringLiteral("Paper ribbon"),
                             QStringLiteral("accent"), BLEND_NORMAL, 0.82f,
                             ribbonM, QStringLiteral("ribbon"), 0.12f,
                             QColor(48, 26, 24), W * 0.005, H * 0.008, 0.28f);

        CreativeMotion garlandM;
        garlandM.dxX = 0.024f;
        garlandM.rotationEnergy = 1.2f;
        addSceneProp(makePaperStarGarland(stage, seed + 23, cream, ochre, 9,
                                          H * 0.155, H * 0.035),
                     QStringLiteral("Paper star garland"),
                     QStringLiteral("accent"), BLEND_NORMAL, 0.95f, garlandM,
                     QStringLiteral("stars"), -0.05f);

        CreativeMotion curtainLM;
        curtainLM.dxX = -0.075f;
        curtainLM.dxHold = -0.040f;
        addScenePropShadowed(
            makePaperCurtain(stage, rust, cream, true),
            QStringLiteral("Paper curtain left"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.98f, curtainLM, QStringLiteral("curtain"),
            -0.15f, QColor(40, 20, 18), W * 0.006, H * 0.008, 0.35f);
        CreativeMotion curtainRM;
        curtainRM.dxX = 0.075f;
        curtainRM.dxHold = 0.040f;
        addScenePropShadowed(
            makePaperCurtain(stage, rust, cream, false),
            QStringLiteral("Paper curtain right"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.98f, curtainRM, QStringLiteral("curtain"),
            -0.15f, QColor(40, 20, 18), W * 0.006, H * 0.008, 0.35f);

        /* floor shadow anchors the cutout on the paper stage */
        {
            QImage ellipse(stage, QImage::Format_ARGB32_Premultiplied);
            ellipse.fill(Qt::transparent);
            QPainter ep(&ellipse);
            ep.setRenderHint(QPainter::Antialiasing, true);
            ep.setPen(Qt::NoPen);
            ep.setBrush(QColor(0, 0, 0, 255));
            ep.drawEllipse(
                QRectF(subjectRect.center().x() - subjectRect.width() * 0.55,
                       subjectRect.bottom() - H * 0.012,
                       subjectRect.width() * 1.10, H * 0.038));
            ep.end();
            add(QStringLiteral("Paper floor shadow"), QStringLiteral("echo"),
                creative::softBlur(ellipse, 0), full, BLEND_NORMAL, 0.32f);
        }

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.03f, 1.08f, 1.00f, dusk, 0.14f, cream,
                                0.10f });
        subjectFull(graded, QStringLiteral("Paper figure"), 1.0f, subjectM);

        CreativeMotion lipM;
        lipM.dxX = 0.095f;
        lipM.dyY = 0.022f;
        addSceneProp(makePaperForegroundLip(stage, seed + 29, rust, cream,
                                            H * 0.91),
                     QStringLiteral("Paper stage lip"), QStringLiteral("accent"),
                     BLEND_NORMAL, 1.0f, lipM,
                     QStringLiteral("foreground"), 0.55f);

        CreativeMotion floralLM;
        floralLM.dxX = -0.060f;
        floralLM.dyY = 0.018f;
        addSceneProp(
            makePaperFloral(stage, seed + 17, { rose, cream, teal, ochre },
                            QPointF(W * 0.105, H * 0.84), H * 0.075, false),
            QStringLiteral("Paper floral left"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.95f, floralLM, QStringLiteral("floral"), 0.10f);
        CreativeMotion floralRM;
        floralRM.dxX = 0.060f;
        floralRM.dyY = 0.018f;
        addSceneProp(
            makePaperFloral(stage, seed + 19, { rose, cream, teal, ochre },
                            QPointF(W * 0.895, H * 0.86), H * 0.070, true),
            QStringLiteral("Paper floral right"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.95f, floralRM, QStringLiteral("floral"), 0.10f);

        CreativeMotion cornerLM;
        cornerLM.dxX = -0.110f;
        cornerLM.dyY = 0.030f;
        addSceneProp(makePaperCornerFoliage(stage, seed + 31,
                                            teal.darker(118), cream, true),
                     QStringLiteral("Paper corner foliage left"),
                     QStringLiteral("accent"), BLEND_NORMAL, 0.94f, cornerLM,
                     QStringLiteral("foreground"), 0.50f);
        CreativeMotion cornerRM;
        cornerRM.dxX = 0.110f;
        cornerRM.dyY = 0.030f;
        addSceneProp(makePaperCornerFoliage(stage, seed + 33,
                                            teal.darker(118), cream, false),
                     QStringLiteral("Paper corner foliage right"),
                     QStringLiteral("accent"), BLEND_NORMAL, 0.94f, cornerRM,
                     QStringLiteral("foreground"), 0.50f);

        CreativeMotion butterflyM;
        butterflyM.rotationEnergy = 7.0f;
        butterflyM.dxEnergy = 0.010f;
        addSceneProp(
            makePaperButterflies(
                stage, seed + 37, { cream, rose, ochre, teal },
                { QPointF(subjectRect.x() + subjectRect.width() * 0.12,
                          subjectRect.y() + subjectRect.height() * 0.16),
                  QPointF(subjectRect.right() - subjectRect.width() * 0.10,
                          subjectRect.y() + subjectRect.height() * 0.34),
                  QPointF(subjectRect.x() + subjectRect.width() * 0.20,
                          subjectRect.bottom() - subjectRect.height() * 0.14) },
                5, H * 0.022),
            QStringLiteral("Paper butterflies"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.90f, butterflyM, QStringLiteral("butterflies"),
            0.30f);

        addSceneProp(makePaperTitle(stage, QStringLiteral("PAPER THEATRE"),
                                    cream, ink, 0.048, H * 0.022),
                     QStringLiteral("Paper title"), QStringLiteral("finish"),
                     BLEND_NORMAL, 1.0f, CreativeMotion(),
                     QStringLiteral("typography"), 0.60f);

        add(QStringLiteral("Paper grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 41, 28), full, BLEND_OVERLAY,
            0.34f);
    }
    else if (patchId == QLatin1String("auto-splatter"))
    {
        const QColor paper(244, 237, 226), ink(18, 16, 14), red(255, 77, 46);
        add(QStringLiteral("Torn paper"), QStringLiteral("backdrop"),
            creative::gradientField(stage, paper, QColor(226, 214, 196),
                                    QColor(255, 255, 255, 130), 0.18),
            full);
        add(QStringLiteral("Poster halftone"), QStringLiteral("backdrop"),
            creative::halftoneField(stage, QColor(20, 18, 16, 34),
                                    H * 0.022, H * 0.0045, 20, 0.0),
            full, BLEND_MULTIPLY, 0.8f);

        QImage spot = stageCanvas(stage);
        {
            QPainter p(&spot);
            p.setRenderHint(QPainter::Antialiasing, true);
            QRadialGradient g(QPointF(W * 0.30, H * 0.32), H * 0.32);
            g.setColorAt(0.0, QColor(255, 77, 46, 230));
            g.setColorAt(0.72, QColor(255, 77, 46, 200));
            g.setColorAt(1.0, QColor(255, 77, 46, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(QPointF(W * 0.30, H * 0.32), H * 0.30, H * 0.24);
        }
        add(QStringLiteral("Poster spot"), QStringLiteral("backdrop"), spot,
            full, BLEND_MULTIPLY, 0.85f);

        CreativeMotion splatM;
        splatM.opacityEnergy = 0.25f;
        splatM.scaleEnergy = 0.12f;
        splatM.rotationEnergy = 8.0f;
        add(QStringLiteral("Ink blast"), QStringLiteral("accent"),
            creative::inkSplatter(stage, seed,
                                  subjectRect.adjusted(-W * 0.20, -H * 0.14,
                                                       W * 0.20, H * 0.14),
                                  ink, 150),
            full, BLEND_NORMAL, 0.85f, splatM);
        CreativeMotion redM;
        redM.opacityX = 0.20f;
        redM.opacityHold = 0.20f;
        add(QStringLiteral("Red blast"), QStringLiteral("accent"),
            creative::inkSplatter(stage, seed + 9,
                                  subjectRect.adjusted(-W * 0.14, -H * 0.10,
                                                       W * 0.14, H * 0.10),
                                  red, 80),
            full, BLEND_MULTIPLY, 0.75f, redM);

        QImage drips = stageCanvas(stage);
        {
            QPainter p(&drips);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            QRandomGenerator rng(seed + 31);
            for (int i = 0; i < 9; ++i)
            {
                const qreal x = W * (0.12 + 0.76 * rng.bounded(1000) / 1000.0);
                const qreal y = H * (0.52 + 0.20 * rng.bounded(1000) / 1000.0);
                const qreal len = H * (0.06 + 0.16 * rng.bounded(1000) / 1000.0);
                const qreal r = qMax(2.0, H * 0.006);
                p.setBrush(QColor(i % 3 == 0 ? red : ink));
                p.drawRoundedRect(QRectF(x - r, y, r * 2.0, len), r, r);
                p.drawEllipse(QPointF(x, y + len), r * 1.4, r * 1.4);
            }
        }
        CreativeMotion dripM;
        dripM.opacityHold = 0.55f;
        add(QStringLiteral("Paint drips"), QStringLiteral("accent"), drips,
            full, BLEND_NORMAL, 0.0f, dripM);

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.04f, 1.22f, 0.90f, ink, 0.18f, red, 0.10f });
        subjectFull(graded, QStringLiteral("Poster figure"), 1.0f,
                    CreativeMotion());

        QImage tape = stageCanvas(stage);
        {
            QPainter p(&tape);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 250, 235, 145));
            p.translate(W * 0.16, H * 0.10);
            p.rotate(-32);
            p.drawRect(QRectF(-W * 0.06, -H * 0.015, W * 0.24, H * 0.03));
            p.resetTransform();
            p.translate(W * 0.84, H * 0.88);
            p.rotate(28);
            p.drawRect(QRectF(-W * 0.06, -H * 0.015, W * 0.24, H * 0.03));
        }
        add(QStringLiteral("Tape"), QStringLiteral("finish"), tape, full,
            BLEND_NORMAL, 0.8f);
        add(QStringLiteral("Poster grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 7, 34), full,
            BLEND_OVERLAY, 0.4f);
    }
    else if (patchId == QLatin1String("stage-mode"))
    {
        const QColor night(8, 6, 12), gold(255, 211, 122),
            rose(184, 51, 106), cream(255, 244, 226);
        add(QStringLiteral("House lights"), QStringLiteral("backdrop"),
            creative::gradientField(stage, night, QColor(18, 10, 24),
                                    QColor(70, 20, 50, 120), 0.30),
            full);
        QImage floor = stageCanvas(stage);
        {
            QPainter p(&floor);
            p.setRenderHint(QPainter::Antialiasing, true);
            QLinearGradient g(0, H * 0.68, 0, H);
            g.setColorAt(0.0, QColor(28, 16, 30, 230));
            g.setColorAt(1.0, QColor(6, 4, 8, 255));
            p.fillRect(QRectF(0, H * 0.68, W, H * 0.32), g);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 220, 170, 30));
            p.drawEllipse(QPointF(W * 0.5, H * 0.84), W * 0.40, H * 0.04);
        }
        add(QStringLiteral("Stage floor"), QStringLiteral("backdrop"), floor,
            full);
        const auto curtain = [&](bool left) {
            QImage img = stageCanvas(stage);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath path;
            if (left)
            {
                path.moveTo(0, 0);
                path.lineTo(W * 0.24, 0);
                path.cubicTo(W * 0.17, H * 0.38, W * 0.25, H * 0.72,
                             W * 0.14, H);
                path.lineTo(0, H);
            }
            else
            {
                path.moveTo(W, 0);
                path.lineTo(W * 0.76, 0);
                path.cubicTo(W * 0.83, H * 0.38, W * 0.75, H * 0.72,
                             W * 0.86, H);
                path.lineTo(W, H);
            }
            path.closeSubpath();
            QLinearGradient g(0, 0, W * (left ? 0.28 : 0.72), 0);
            g.setColorAt(0.0, rose.darker(190));
            g.setColorAt(0.5, rose.darker(135));
            g.setColorAt(1.0, rose);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawPath(path);
            return img;
        };
        CreativeMotion curtainL;
        curtainL.dxX = -0.05f;
        curtainL.dxHold = -0.06f;
        add(QStringLiteral("Stage curtain left"), QStringLiteral("backdrop"),
            curtain(true), full, BLEND_NORMAL, 0.95f, curtainL);
        CreativeMotion curtainR;
        curtainR.dxX = 0.05f;
        curtainR.dxHold = 0.06f;
        add(QStringLiteral("Stage curtain right"), QStringLiteral("backdrop"),
            curtain(false), full, BLEND_NORMAL, 0.95f, curtainR);

        QImage cone = stageCanvas(stage);
        {
            QPainter p(&cone);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath beam;
            beam.moveTo(W * 0.50, H * 0.03);
            beam.lineTo(W * 0.12, H * 0.86);
            beam.lineTo(W * 0.88, H * 0.86);
            beam.closeSubpath();
            QLinearGradient g(0, H * 0.03, 0, H * 0.86);
            g.setColorAt(0.0, QColor(255, 240, 200, 130));
            g.setColorAt(1.0, QColor(255, 220, 160, 0));
            p.fillPath(beam, g);
        }
        CreativeMotion spotM;
        spotM.opacityY = 0.20f;
        spotM.opacityHold = 0.20f;
        spotM.dxX = 0.04f;
        add(QStringLiteral("Stage spotlight"), QStringLiteral("accent"), cone,
            full, BLEND_SCREEN, 0.8f, spotM);
        add(QStringLiteral("Stage glow"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(), H * 0.45,
                                  gold, 0.36f),
            full, BLEND_SCREEN, 0.55f);
        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 0.98f, 1.16f, 1.04f, QColor(40, 18, 50), 0.22f,
                                gold, 0.08f });
        subjectFull(graded, QStringLiteral("Stage figure"), 1.0f,
                    CreativeMotion());
        add(QStringLiteral("Stage dust"), QStringLiteral("finish"),
            creative::starField(stage, seed, cream, 70), full,
            BLEND_SCREEN, 0.45f);
        add(QStringLiteral("Stage grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 2, 24), full,
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

    /* Universal stage light: every patch visibly answers the pad. X sweeps
     * the light, Y lifts it, Hold blooms it, speed lets it breathe. It is a
     * normal committed layer, never a UI-only effect. */
    {
        const QPointF lightCenter =
            subjectRect.isEmpty() ? full.center() : subjectRect.center();
        CreativeMotion lightM;
        lightM.opacityY = 0.30f;
        lightM.opacityHold = 0.30f;
        lightM.dxX = 0.040f;
        lightM.dxEnergy = 0.015f;
        lightM.scaleY = 0.18f;
        lightM.scaleHold = 0.10f;
        CreativePiece light;
        light.name = QStringLiteral("Stage light");
        light.role = QStringLiteral("finish");
        light.image = creative::radialGlow(
            stage, lightCenter,
            qMax(subjectRect.width(), subjectRect.height()) * 0.95,
            stageLightColor(patchId), 0.30f);
        light.rect = full;
        light.pivot = lightCenter;
        light.blendMode = BLEND_SCREEN;
        light.opacity = 0.14f;
        light.motion = lightM;
        pieces.append(light);
    }

    return pieces;
}

#include "pb_creative_legacy_commit.inc"

} /* namespace paintbrush */

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
#include "pb_creative_recipe.hpp"

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
#include <memory>
#include <utility>

namespace paintbrush {

namespace {

using creative::recipe::clamp01;

QString qcolorHex(const QColor &c)
{
    return c.isValid() ? c.name(QColor::HexRgb) : QString();
}

/* Convert built pieces into publication-ready layers. Pure (no controller or
 * document access), so it runs on the worker. Trimming happens here once; the
 * same pxScale mapping the synchronous commit used is preserved. */
QList<CanvasLayer> creativeLayersFromPieces(const QVector<CreativePiece> &pieces,
                                            const CreativeFrame &frame,
                                            const QString &patchId,
                                            const QString &patchName,
                                            quint32 seed)
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
        QVariantMap params;
        params[QStringLiteral("x")] = double(frame.x);
        params[QStringLiteral("y")] = double(frame.y);
        params[QStringLiteral("energy")] = double(frame.energy);
        params[QStringLiteral("hold")] = frame.hold;
        layer.metadata[QStringLiteral("creative.params")] = params;
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
    result.layers = creativeLayersFromPieces(pieces, input.frame, input.patchId,
                                             input.patchName, input.seed);
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
    /* One build at a time: a full-resolution render is heavy, and a monotonic
     * revision makes a replacement a cancel rather than a second concurrent
     * build. */
    m_buildPool.setMaxThreadCount(1);
    m_buildPool.setExpiryTimeout(30000);
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
    /* The worker polls the token at piece boundaries; this bounded wait keeps
     * shutdown deterministic without blocking on a healthy long build. */
    m_buildPool.waitForDone(5000);
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
    return hasSubject() && !m_patchId.isEmpty() && !m_busy && !m_committed;
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
    if (m_suggestionVisible || !m_suggestionText.isEmpty())
    {
        m_suggestionVisible = false;
        m_suggestionText.clear();
        emit suggestionChanged();
    }
    rebuildPreview();
    emit subjectChanged();
    emit stateChanged();
    return true;
}

void PaintbrushCreative::clearSubject()
{
    cancelCommitInternal(false);
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
    m_subjectCanvasRect = QRectF();
    m_subjectLabel.clear();
    m_sourceLayerId = -1;
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

void PaintbrushCreative::openFromHarvest(int harvestId)
{
    QRectF rect;
    QString label;
    const QImage cutout = m_controller
                              ? m_controller->creativePadHarvest(harvestId, &rect, &label)
                              : QImage();
    m_patchId = defaultPatchId();
    m_chooserOpen = true;
    if (!adoptSubject(cutout, rect, -1, label,
                      QStringLiteral("That kept moment has no cutout yet.")))
    {
        setOpen(true);
        return;
    }
    m_committed = false;
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

    /* The live preview is bounded (560x680 fit) no matter how large the
     * canvas is: full resolution is reserved for commits/exports. */
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
    const QSize subjSize(qMax(1, int(std::lround(previewRect.width()))),
                         qMax(1, int(std::lround(previewRect.height()))));
    m_subject = m_subjectNative.scaled(subjSize, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);

    CreativeBuildInput input;
    input.patchId = m_patchId;
    input.patchName = m_patchId;
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
    m_pieces = creative::recipe::build(input);
    recompose();
}

void PaintbrushCreative::recompose()
{
    /* While a commit is rendering, the preview belongs to the job: progressive
     * previews must not be overwritten by 20 Hz idle recompositions. */
    if (m_busy || m_previewSize.isEmpty())
        return;
    QImage frame = creative::recipe::compose(m_pieces, m_previewSize,
                                             m_previewSize, QColor(12, 12, 18),
                                             m_frame);
    {
        QMutexLocker locker(&m_previewMutex);
        m_preview = frame;
    }
    ++m_previewRevision;
    emit previewChanged();
}

/* ------------------------------------------------------- human parser ---- */

bool PaintbrushCreative::usesFashionProtection() const
{
    if (!m_fashionProtection)
        return false;
    const PatchDef *def = patchDef(m_patchId);
    return def && def->family == QLatin1String("Material");
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
    recompose();
    if (!m_recording && !m_replayTimer.isActive())
        emit padChanged();
}

void PaintbrushCreative::beginPad()
{
    if (!hasSubject())
        return;
    setStatus(QStringLiteral("Play."));
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
    recompose();
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
    recompose();
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
    recompose();
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
    input.subject = m_subjectNative;
    input.subjectRect = m_subjectCanvasRect;
    input.stage = canvas; /* native canvas resolution, not a 1600 px cap */
    input.previewSize = previewSize;
    input.seed = m_seed;
    input.frame = m_frame;
    if (usesFashionProtection() && !m_semanticProtection.isNull()
        && m_semanticProtection.size() == m_subjectNative.size())
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
                                                 desc)
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

    m_committed = true;
    m_suggestionText = suggestion;
    m_suggestionVisible = !m_suggestionText.isEmpty();
    m_lastCommitError.clear();
    setStatus(QStringLiteral("Committed at canvas resolution — every piece is "
                             "an editable layer, one undo."));
    emit suggestionChanged();
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

void PaintbrushCreative::acceptSuggestion()
{
    if (!m_suggestionVisible || m_subjectCanvasRect.isEmpty() || !m_controller)
        return;
    const PatchDef *def = patchDef(m_patchId);
    const QColor accent = def ? def->accent : QColor(255, 226, 120);
    const QPointF at(m_subjectCanvasRect.x() + m_subjectCanvasRect.width() * 0.78,
                     m_subjectCanvasRect.y() + m_subjectCanvasRect.height() * 0.22);
    const qreal size = qMax<qreal>(24.0,
                                   m_subjectCanvasRect.height() * 0.11);
    m_controller->addCreativeSparkle(accent, at, size);
    m_suggestionVisible = false;
    m_suggestionText.clear();
    setStatus(QStringLiteral("Sparkle added — move or undo it like any layer."));
    emit suggestionChanged();
    emit stateChanged();
}

void PaintbrushCreative::dismissSuggestion()
{
    m_suggestionVisible = false;
    emit suggestionChanged();
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


} /* namespace paintbrush */

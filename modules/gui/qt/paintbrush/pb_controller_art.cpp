/*****************************************************************************
 * pb_controller_art.cpp : NAI Paintbrush — Harvest / Compose / Boost surface
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_controller.hpp"
#include "pb_canvas_item.hpp"
#include "pb_sticker.hpp"
#include "pb_export.hpp"
#include "pb_creative.hpp"
#include "pb_creative_art.hpp"
#include "pb_creative_poses.hpp"
#include "pb_stage.hpp"
#include "art/style/mood.hpp"
#include "art/style/typography_style.hpp"
#include "art/style/badges.hpp"
#include "art/surface/paper_renderer.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace paintbrush {

namespace {

constexpr double kTau = 6.283185307179586;

QString creativeLinkGroup(const CanvasLayer &layer)
{
    return layer.metadata.value(QStringLiteral("creative.linkGroup")).toString();
}

/* Committed playlist layers are results, never subjects for the next look. */
bool isCreativeResult(const CanvasLayer &layer)
{
    return !layer.metadata.value(QStringLiteral("creative.patch"))
                .toString()
                .isEmpty();
}

} /* namespace */

namespace {

QString dataUrl(const QImage &img)
{
    if (img.isNull())
        return QString();
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

} // namespace

/* ------------------------------------------------------------------ harvest */

void PaintbrushController::keepSelection()
{
    if (m_objectMask.isNull())
    {
        setStatus(QStringLiteral("Select the moment first (Object Select), then Keep."));
        return;
    }
    /* The exact frame this selection was made on comes from the ONE
     * authoritative ObjectSelectionContext (P0#6): for a live video the
     * context holds the native decoded frame + native mask + exact ref. */
    FrameRef ref;
    QImage frame;
    QImage mask = m_objectMask;
    if ((m_selection.sourceKind == ObjectSelectionContext::SourceLiveVideo
         || m_selection.sourceKind == ObjectSelectionContext::SourceCanvasLayer)
        && !m_selection.frame.isNull())
    {
        ref = m_selection.ref;
        frame = m_selection.frame;
        /* The mask is ALREADY in native frame space; use it byte-identical —
         * never rescale the authoritative mask for Keep (P0#3). */
        if (!m_selection.mask.isNull()
            && m_selection.mask.size() == frame.size())
            mask = m_selection.mask;
        else if (mask.size() != frame.size())
            mask = mask.scaled(frame.size(), Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
    }
    else
    {
        /* compose / still mode: decode the captured reference frame */
        ref = m_document.sourceFrame();
        if (ref.isValid())
        {
            const DecodedFrame f = m_editSource.request(ref);
            if (f.ok())
            {
                frame = f.image;
                ref.ptsMs = f.ptsMs; /* the verified PTS, never a guess */
            }
        }
        if (frame.isNull())
        {
            /* still image / legacy document: harvest from the canvas pixels */
            frame = pristineSourceFrame();
            if (frame.isNull() && m_canvas)
                frame = m_canvas->grabCanvasRegion(QRectF(QPointF(0, 0),
                                                          m_document.canvasSize()));
            if (ref.mrl.isEmpty())
            {
                ref.mrl = m_document.sourceMediaMRL().isEmpty()
                              ? QStringLiteral("canvas://%1").arg(m_projectName.isEmpty()
                                                                      ? QStringLiteral("untitled")
                                                                      : m_projectName)
                              : m_document.sourceMediaMRL();
                ref.title = m_document.sourceMediaTitle();
                ref.ptsMs = m_document.sourceTimeMs();
            }
        }
        /* the mask lives in canvas space; map it onto the compose frame */
        if (mask.size() != frame.size())
            mask = mask.scaled(frame.size(), Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
    }
    if (frame.isNull())
    {
        setStatus(QStringLiteral("Could not read the source frame for this moment."));
        return;
    }

    HarvestItem item = makeHarvestItem(0, ref, frame, mask, m_objectMaskScore,
                                       m_selectedTrackId >= 0
                                           ? QStringLiteral("track:%1").arg(m_selectedTrackId)
                                           : QString());
    if (!item.isValid())
    {
        setStatus(QStringLiteral("The selection is empty."));
        return;
    }
    /* dedup: the same moment kept twice is one tray item */
    for (const HarvestItem &h : m_document.harvest()->items())
    {
        if (h.contentHash == item.contentHash)
        {
            setStatus(QStringLiteral("Already in your tray (%1).").arg(harvestTimeText(h.sourceFrame.ptsMs)));
            emit harvestKept(h.id);
            return;
        }
    }
    const int id = m_document.harvest()->add(item);
    m_document.setDirty(true);
    setStatus(QStringLiteral("Kept %1 — drag it from the tray when you want it.")
                  .arg(harvestTimeText(item.sourceFrame.ptsMs)));
    triggerCompanionEvent(QStringLiteral("harvest-kept"));
    emit harvestKept(id);
    Q_UNUSED(id);
}

void PaintbrushController::placeHarvest(int harvestId, const QPointF &canvasPos)
{
    const HarvestItem *h = m_document.harvest()->itemById(harvestId);
    if (!h || h->cutout.isNull())
        return;
    /* a comfortable default size: ~45% of the canvas height, never larger
     * than the source */
    const double targetH = m_document.canvasSize().height() * 0.45;
    const double scale = qBound(0.1, targetH / qMax(1, h->cutout.height()), 1.0);
    CanvasLayer layer = CanvasLayer::createImage(h->cutout, QPointF(0, 0),
                                                 QStringLiteral("Pose %1")
                                                     .arg(harvestTimeText(h->sourceFrame.ptsMs)));
    layer.type = LAYER_STICKER;
    layer.harvestId = h->id;
    layer.scale = float(scale);
    layer.metadata[QStringLiteral("sourceMRL")] = h->sourceFrame.mrl;
    layer.metadata[QStringLiteral("sourceTitle")] = h->sourceFrame.title;
    layer.metadata[QStringLiteral("sourceTimeMs")] = qint64(h->sourceFrame.ptsMs);
    QPointF center = canvasPos;
    if (center.isNull())
        center = QPointF(m_document.canvasSize().width() / 2.0,
                         m_document.canvasSize().height() / 2.0);
    layer.position = center - QPointF(layer.raster.rect().center());

    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Place pose");
    QList<CanvasLayer> layers = m_document.layers();
    layers.append(std::move(layer));
    cmd.index = layers.size() - 1;
    cmd.prevCurrentIndex = m_document.currentLayerIndex();
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(cmd.index);
    cmd.layer = *m_document.layerAt(cmd.index);
    m_undoStack.pushInsertLayer(cmd);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Placed. Drag, pinch or rotate it; Boost when you have a few."));
}

void PaintbrushController::removeHarvest(int harvestId)
{
    m_document.harvest()->remove(harvestId);
    m_document.setDirty(true);
}

void PaintbrushController::toggleHarvestFavorite(int harvestId)
{
    const HarvestItem *h = m_document.harvest()->itemById(harvestId);
    if (h)
        m_document.harvest()->setFavorite(harvestId, !h->favorite);
}

void PaintbrushController::seekToHarvest(int harvestId)
{
    const HarvestItem *h = m_document.harvest()->itemById(harvestId);
    if (h && h->sourceFrame.ptsMs >= 0 && h->sourceFrame.mrl == m_document.sourceMediaMRL())
        playerSeek(h->sourceFrame.ptsMs);
}

/* ------------------------------------------------------------ living harvest */

namespace {

QVector<motion::PuppetMotionScanner::FrameSample> traySamples(
    const HarvestTrayModel *tray, const QString &mrl)
{
    QVector<motion::PuppetMotionScanner::FrameSample> samples;
    for (const HarvestItem *it : tray->sameSourceSequence(mrl))
    {
        motion::PuppetMotionScanner::FrameSample s;
        s.ptsMs = it->sourceFrame.ptsMs;
        s.mask = it->mask;
        s.quality = it->segmentationScore > 0.0 ? it->segmentationScore : 1.0;
        samples.append(s);
    }
    return samples;
}

} // namespace

int PaintbrushController::rigHarvest(int harvestId)
{
    const HarvestItem *h = m_document.harvest()->itemById(harvestId);
    if (!h || !h->isValid() || h->cutout.isNull())
        return 0;
    /* already rigged? the asset is reusable (rig once) */
    const int existing = m_document.puppets()->assetIdByHarvest(harvestId);
    if (existing > 0)
        return existing;

    motion::PuppetAsset asset =
        makeLivingAsset(*h, traySamples(m_document.harvest(), h->sourceFrame.mrl));
    if (!asset.isValid())
    {
        setStatus(QStringLiteral("This harvest has no riggable motion nearby — keep more poses of the same subject first."));
        return 0;
    }

    auto cmd = std::make_unique<AddPuppetCommand>();
    cmd->asset = asset;
    cmd->desc = QStringLiteral("Rig living asset");
    m_undoStack.pushCommand(std::move(cmd));
    const int id = m_document.puppets()->assetIdByHarvest(harvestId);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Living asset ready — one undo removes it, the original harvest is untouched."));
    return id;
}

void PaintbrushController::setLayerPuppet(int index, int assetId, double amplitude)
{
    const CanvasLayer *layer = m_document.layerAt(index);
    if (!layer)
        return;
    const motion::PuppetAsset *asset = m_document.puppets()->byId(assetId);
    if (!asset)
        return;
    motion::PuppetEffect fx;
    fx.kind = motion::MotionKind::Custom;
    fx.amplitude = float(qBound(0.01, amplitude, 1.0));
    fx.speed = 1.0f;
    fx.loop = motion::LoopMode::Loop;
    fx.durationMs = quint32(qMax<qint64>(1, asset->motion.durationMs()));

    auto cmd = std::make_unique<SetStageLivingCommand>();
    cmd->layerId = layer->id;
    cmd->beforeAssetId = layer->puppetAssetId;
    cmd->beforeEffect = layer->puppetEffect;
    cmd->beforeMetadata = layer->metadata;
    cmd->afterAssetId = assetId;
    cmd->afterEffect = fx;
    cmd->afterMetadata = layer->metadata;
    cmd->afterMetadata.remove(stagekeys::living());
    cmd->afterMetadata.remove(stagekeys::motionClip());
    cmd->afterMetadata.remove(stagekeys::envelope());
    cmd->desc = QStringLiteral("Living pose");
    m_undoStack.pushCommand(std::move(cmd));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Living pose on layer — undo restores the static figure."));
}

void PaintbrushController::removeLayerPuppet(int index)
{
    const CanvasLayer *layer = m_document.layerAt(index);
    if (!layer || (layer->puppetAssetId <= 0 && !layer->hasDerivedLiving()))
        return;
    auto cmd = std::make_unique<SetStageLivingCommand>();
    cmd->layerId = layer->id;
    cmd->beforeAssetId = layer->puppetAssetId;
    cmd->beforeEffect = layer->puppetEffect;
    cmd->beforeMetadata = layer->metadata;
    cmd->afterAssetId = -1;
    cmd->afterEffect = motion::PuppetEffect();
    cmd->afterMetadata = layer->metadata;
    cmd->afterMetadata.remove(stagekeys::living());
    cmd->afterMetadata.remove(stagekeys::motionClip());
    cmd->afterMetadata.remove(stagekeys::envelope());
    cmd->desc = QStringLiteral("Freeze living pose");
    m_undoStack.pushCommand(std::move(cmd));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Frozen — undo brings the living loop back."));
}

bool PaintbrushController::layerHasPuppet(int index) const
{
    const CanvasLayer *layer = m_document.layerAt(index);
    return layer && layer->hasPuppet();
}

double PaintbrushController::layerPuppetAmplitude(int index) const
{
    const CanvasLayer *layer = m_document.layerAt(index);
    return layer ? double(layer->puppetEffect.amplitude) : 0.0;
}

int PaintbrushController::layerPuppetAssetId(int index) const
{
    const CanvasLayer *layer = m_document.layerAt(index);
    return layer ? layer->puppetAssetId : -1;
}

int PaintbrushController::riggedAssetCount() const
{
    return m_document.puppets()->assets().size();
}

int PaintbrushController::firstRiggedAssetId() const
{
    const auto &assets = m_document.puppets()->assets();
    return assets.isEmpty() ? -1 : assets.first().id;
}

QString PaintbrushController::exportLivingPoster(const QUrl &dir, int durationMs,
                                                 int fps, bool paper)
{
    if (dir.isEmpty())
        return QStringLiteral("No folder chosen.");
    QString path = dir.toLocalFile();
    if (path.isEmpty())
        path = dir.path();
    const QString err = ExportService::exportLivingPoster(
        &m_document, path, QStringLiteral("living_poster"), qint64(durationMs), fps, paper, 1.0);
    if (err.isEmpty())
        setStatus(QStringLiteral("Living Poster frames written to %1").arg(path));
    else
        setStatus(err);
    return err;
}

QString PaintbrushController::exportLivingVideo(const QUrl &dir, int durationMs,
                                                int fps, const QString &format,
                                                const QUrl &outFile)
{
    QString dirPath = dir.toLocalFile();
    if (dirPath.isEmpty())
        dirPath = dir.path();
    const QString outPath = outFile.toLocalFile().isEmpty()
        ? outFile.path() : outFile.toLocalFile();
    if (outPath.isEmpty())
        return QStringLiteral("No output file chosen.");
    const QString stem = QFileInfo(outPath).completeBaseName();
    const QString err = ExportService::exportLivingVideo(
        &m_document, dirPath, stem.isEmpty() ? QStringLiteral("living_poster") : stem,
        qint64(durationMs), fps, format, outPath);
    if (err.isEmpty())
        setStatus(QStringLiteral("Living Poster written to %1 — don't generate the moment, keep it alive.")
                      .arg(outPath));
    else
        setStatus(err);
    return err;
}

bool PaintbrushController::canExportLivingVideo(const QString &format) const
{
    return ExportService::canExportLivingVideo(format);
}

/* -------------------------------------------------------------------- boost */

bool PaintbrushController::canBoost() const
{
    for (const CanvasLayer &l : m_document.layers())
        if ((l.type == LAYER_STICKER || l.type == LAYER_IMAGE) && l.visible && !l.raster.isNull())
            return true;
    return false;
}

void PaintbrushController::boost()
{
    if (m_boostBusy)
        return;
    const art::Mood *mood = art::mood::findById(m_document.moodId());
    m_boostOptions = BoostOptions();
    if (mood)
    {
        m_boostOptions.motionStrength = mood->motionStrength;
        m_boostOptions.shadowBlur = mood->shadowBlur;
        m_boostOptions.shadowOpacity = mood->shadowOpacity;
    }
    m_boostAnalysis = BoostEngine::analyze(m_document);
    if (m_boostAnalysis.sequence.isEmpty())
    {
        m_boostSummary = BoostEngine::composeSummary({});
        emit boostStateChanged();
        setStatus(m_boostSummary);
        return;
    }

    m_boostBusy = true;
    m_boostSummary.clear();
    emit boostStateChanged();
    m_boostTx = std::make_unique<BoostTransaction>(m_document);
    BoostEngine::planDeterministic(*m_boostTx, m_boostAnalysis, m_boostOptions);
    /* Living Harvest hidden tool: a subtle living loop on the hero joins the
     * same undo step (skipped when a living layer already exists). */
    BoostEngine::animateHero(*m_boostTx, m_boostAnalysis);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();

    /* bridge pose: real frames from the real video, segmented with the
     * same engine the user's own Keep uses (spec §27) */
    if (m_boostOptions.allowBridgePose && m_boostAnalysis.wantsBridge() &&
        m_segmentation.isConfigured() && m_editSource.canDecode())
    {
        const BoostAnalysis::Pose *pa = m_boostAnalysis.pose(m_boostAnalysis.bridgeFromLayerId);
        const BoostAnalysis::Pose *pb = m_boostAnalysis.pose(m_boostAnalysis.bridgeToLayerId);
        const HarvestItem *A = pa ? m_document.harvest()->itemById(pa->harvestId) : nullptr;
        const HarvestItem *B = pb ? m_document.harvest()->itemById(pb->harvestId) : nullptr;
        if (A && B)
        {
            m_boostCandidates.clear();
            for (qint64 t : BoostEngine::bridgeTimestamps(m_boostAnalysis.bridgeFromMs,
                                                          m_boostAnalysis.bridgeToMs, 3))
            {
                const DecodedFrame f = m_editSource.request(FrameRef{ A->sourceFrame.mrl, {}, t });
                if (!f.ok())
                    continue; /* PTS not verified -> never used (spec §27) */
                BridgeCandidate c;
                c.ptsMs = f.ptsMs;
                c.frame = f.image;
                m_boostCandidates.append(c);
            }
            m_boostBestCandidate = BoostEngine::scoreBridgeCandidates(
                m_boostCandidates, *A, *B, m_boostAnalysis.bridgeFromMs, m_boostAnalysis.bridgeToMs);
            if (m_boostBestCandidate >= 0)
            {
                const BridgeCandidate &best = m_boostCandidates[m_boostBestCandidate];
                const QPointF seed = BoostEngine::bridgeSeedPoint(*A, *B, best.ptsMs);
                m_boostSavedMask = m_objectMask;
                m_boostAwaitingMask = true;
                m_boostTimeout.start();
                m_segmentation.segment(best.frame, { seed }, {}, {});
                setStatus(QStringLiteral("Boost: finding a bridge pose in the video..."));
                return; /* continues in boostInsertBridge() */
            }
        }
    }
    finishBoost(QString());
}

void PaintbrushController::boostInsertBridge(const QImage &mask, double score)
{
    m_boostTimeout.stop();
    if (!m_boostTx || m_boostBestCandidate < 0 || m_boostBestCandidate >= m_boostCandidates.size())
    {
        finishBoost(QString());
        return;
    }
    const BridgeCandidate &best = m_boostCandidates[m_boostBestCandidate];
    const BoostAnalysis::Pose *pa = m_boostAnalysis.pose(m_boostAnalysis.bridgeFromLayerId);
    const HarvestItem *A = pa ? m_document.harvest()->itemById(pa->harvestId) : nullptr;
    QString note;
    if (A && !mask.isNull())
    {
        FrameRef ref = A->sourceFrame;
        ref.ptsMs = best.ptsMs;
        HarvestItem derived = makeHarvestItem(0, ref, best.frame, mask, score, A->objectIdentity);
        /* a bridge must be a real, reasonably complete pose: tiny or
         * fragmentary masks are rejected rather than placed (spec §27) */
        const double areaRatio = double(derived.sourceBounds.width() * derived.sourceBounds.height()) /
                                 double(qMax(1, A->sourceBounds.width() * A->sourceBounds.height()));
        if (derived.isValid() && areaRatio > 0.35 && areaRatio < 3.0 && score > 0.5)
        {
            if (BoostEngine::insertBridgePose(*m_boostTx, m_boostAnalysis, m_boostOptions, derived) <= 0)
                note = QStringLiteral("(The bridge pose could not be placed.)");
        }
        else
            note = QStringLiteral("(No clean bridge pose was found, so the structure pass stands on its own.)");
    }
    finishBoost(note);
}

void PaintbrushController::finishBoost(const QString &note)
{
    m_boostTimeout.stop();
    m_boostAwaitingMask = false;
    if (!m_boostSavedMask.isNull() || m_objectMask.isNull())
    {
        /* the segmentation we borrowed must not disturb the user's selection */
        m_objectMask = m_boostSavedMask;
        m_boostSavedMask = QImage();
    }
    QStringList lines;
    if (m_boostTx)
    {
        lines = m_boostTx->summary();
        auto cmd = m_boostTx->finish(QStringLiteral("Boost"));
        if (cmd)
            m_undoStack.pushCommand(std::move(cmd));
        else
            lines << QStringLiteral("The composition already reads well — nothing to change.");
        m_boostTx.reset();
    }
    if (!note.isEmpty())
        lines << note;
    m_boostSummary = BoostEngine::composeSummary(lines);
    m_boostCandidates.clear();
    m_boostBestCandidate = -1;
    m_boostBusy = false;
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    emit boostStateChanged();
    setStatus(m_boostSummary);
    triggerCompanionEvent(QStringLiteral("boost-applied"));
}

void PaintbrushController::cancelBoost()
{
    if (!m_boostBusy)
        return;
    m_boostTimeout.stop();
    m_boostAwaitingMask = false;
    if (m_boostTx)
    {
        m_boostTx->rollback();
        m_boostTx.reset();
    }
    m_objectMask = m_boostSavedMask;
    m_boostSavedMask = QImage();
    m_boostCandidates.clear();
    m_boostBusy = false;
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    emit boostStateChanged();
    setStatus(QStringLiteral("Boost cancelled — nothing changed."));
}

/* ---------------------------------------------------------- art direction */

QVariantList PaintbrushController::moodList() const
{
    QVariantList out;
    for (const art::Mood *m : art::mood::presets())
        out.append(m->toVariant());
    return out;
}

QVariantList PaintbrushController::paperList() const
{
    QVariantList out;
    for (const art::PaperStock *p : art::paper::presets())
    {
        QVariantMap m;
        m.insert(QStringLiteral("id"), p->id);
        m.insert(QStringLiteral("name"), p->displayName);
        m.insert(QStringLiteral("color"), p->baseColor.name());
        out.append(m);
    }
    return out;
}

QVariantList PaintbrushController::typographyList() const
{
    QVariantList out;
    for (const art::TypographyStyle *s : art::typography::styles())
        out.append(s->toVariant());
    return out;
}

QStringList PaintbrushController::badgeList() const
{
    return art::badges::allNames();
}

QVariantList PaintbrushController::moodPalette() const
{
    const art::Mood *m = art::mood::findById(m_document.moodId());
    if (!m)
        m = &art::mood::defaultMood();
    return { m->palette.primary.name(), m->palette.secondary.name(), m->palette.accent.name(),
             m->palette.ink.name(), m->palette.glitter.name() };
}

void PaintbrushController::applyMood(const QString &moodId)
{
    const art::Mood *m = art::mood::findById(moodId);
    if (!m)
        return;
    SetArtStateCommand cmd;
    cmd.before = SetArtStateCommand::capture(m_document);
    cmd.after.paperId = m->paperId;
    cmd.after.moodId = m->id;
    cmd.after.light = m->light;
    cmd.desc = QStringLiteral("Mood: %1").arg(m->displayName);
    cmd.apply(m_document);
    m_undoStack.pushCommand(std::make_unique<SetArtStateCommand>(cmd));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("%1 — paper, light and palette set. Your artwork is untouched.")
                  .arg(m->displayName));
}

void PaintbrushController::setPaper(const QString &paperId)
{
    if (!art::paper::findById(paperId) || paperId == m_document.paperStockId())
        return;
    SetArtStateCommand cmd;
    cmd.before = SetArtStateCommand::capture(m_document);
    cmd.after = cmd.before;
    cmd.after.paperId = paperId;
    cmd.desc = QStringLiteral("Paper");
    cmd.apply(m_document);
    m_undoStack.pushCommand(std::make_unique<SetArtStateCommand>(cmd));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::applyMoodToCanvasTools()
{
    if (!m_canvas)
        return;
    const art::Mood *m = art::mood::findById(m_document.moodId());
    if (!m)
        return;
    /* the glitter grain palette follows the mood (bounded set, §14.4) */
    QVector<QColor> pal{ m->palette.glitter, m->palette.glitter.lighter(118),
                         m->palette.glitter.darker(125), m->palette.accent };
    m_canvas->setGlitterPalette(pal, m->id);
    m_canvas->setGlitterShimmer(m->glitterShimmer);
    m_canvas->setGlitterFlakeSize(m->glitterFlake);
}

void PaintbrushController::setLayerRole(int index, const QString &role)
{
    CanvasLayer *l = m_document.layerAt(index);
    if (!l)
        return;
    SetRoleCommand cmd;
    cmd.layerId = l->id;
    cmd.before = l->role;
    cmd.after = int(art::layerRoleFromName(role));
    cmd.desc = QStringLiteral("Role: %1").arg(role);
    if (cmd.before == cmd.after)
        return;
    cmd.apply(m_document);
    m_undoStack.pushCommand(std::make_unique<SetRoleCommand>(cmd));
    emit documentChanged();
}

QString PaintbrushController::layerRole(int index) const
{
    const CanvasLayer *l = m_document.layerAt(index);
    return l ? art::layerRoleName(art::LayerRole(l->role)) : QString();
}

void PaintbrushController::addLayerShadow(int index, double blur, double opacity)
{
    const CanvasLayer *l = m_document.layerAt(index);
    if (!l)
        return;
    QVector<art::EffectNode> fx = l->effects;
    fx.erase(std::remove_if(fx.begin(), fx.end(),
                            [](const art::EffectNode &e) { return e.type == art::EffectNode::Shadow; }),
             fx.end());
    fx.append(art::EffectNode::makeShadow(float(blur), float(opacity)));
    applyLayerEffects(index, fx, QStringLiteral("Shadow"));
}

void PaintbrushController::addLayerGlow(int index, const QString &color, double radius)
{
    const CanvasLayer *l = m_document.layerAt(index);
    if (!l)
        return;
    QVector<art::EffectNode> fx = l->effects;
    fx.erase(std::remove_if(fx.begin(), fx.end(),
                            [](const art::EffectNode &e) { return e.type == art::EffectNode::Glow; }),
             fx.end());
    fx.append(art::EffectNode::makeGlow(QColor(color), float(radius), 1.0f));
    applyLayerEffects(index, fx, QStringLiteral("Glow"));
}

void PaintbrushController::addLayerOutline(int index, const QString &color, double width)
{
    const CanvasLayer *l = m_document.layerAt(index);
    if (!l)
        return;
    QVector<art::EffectNode> fx = l->effects;
    fx.erase(std::remove_if(fx.begin(), fx.end(),
                            [](const art::EffectNode &e) { return e.type == art::EffectNode::Outline; }),
             fx.end());
    fx.append(art::EffectNode::makeOutline(QColor(color), float(width), 0.2f));
    applyLayerEffects(index, fx, QStringLiteral("Outline"));
}

void PaintbrushController::clearLayerEffects(int index)
{
    const CanvasLayer *l = m_document.layerAt(index);
    if (!l || l->effects.isEmpty())
        return;
    applyLayerEffects(index, {}, QStringLiteral("Clear finish"));
}

void PaintbrushController::applyLayerEffects(int index,
                                             const QVector<art::EffectNode> &after,
                                             const QString &desc)
{
    const CanvasLayer *anchor = m_document.layerAt(index);
    if (!anchor)
        return;
    const QString group = creativeLinkGroup(*anchor);

    QVector<int> targets;
    if (!group.isEmpty())
    {
        for (int i = 0; i < m_document.layers().size(); ++i)
        {
            const CanvasLayer *l = m_document.layerAt(i);
            if (l && creativeLinkGroup(*l) == group)
                targets.append(i);
        }
    }
    if (targets.isEmpty())
        targets.append(index);

    if (targets.size() == 1)
    {
        CanvasLayer *l = m_document.layerAt(targets.first());
        if (!l)
            return;
        SetEffectsCommand cmd;
        cmd.layerId = l->id;
        cmd.before = l->effects;
        cmd.after = after;
        cmd.desc = desc;
        cmd.apply(m_document);
        m_undoStack.pushCommand(std::make_unique<SetEffectsCommand>(cmd));
    }
    else
    {
        auto composite = std::make_unique<CompositeCommand>();
        composite->desc = QStringLiteral("%1 (linked)").arg(desc);
        for (int i : targets)
        {
            CanvasLayer *l = m_document.layerAt(i);
            if (!l)
                continue;
            SetEffectsCommand cmd;
            cmd.layerId = l->id;
            cmd.before = l->effects;
            cmd.after = after;
            cmd.desc = desc;
            cmd.apply(m_document);
            composite->children.push_back(std::make_unique<SetEffectsCommand>(cmd));
        }
        if (composite->children.empty())
            return;
        m_undoStack.pushCommand(std::move(composite));
    }
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

int PaintbrushController::layerEffectCount(int index) const
{
    const CanvasLayer *l = m_document.layerAt(index);
    return l ? l->effects.size() : 0;
}

void PaintbrushController::addStyledText(const QString &text, const QString &styleId,
                                         double sizePx, const QString &color)
{
    art::TextTreatment t;
    t.text = text;
    t.styleId = styleId;
    t.sizePx = float(sizePx);
    t.color = QColor(color);
    if (!t.color.isValid())
    {
        const art::Mood *m = art::mood::findById(m_document.moodId());
        t.color = m ? m->palette.ink : QColor(0x1F, 0x1B, 0x18);
    }
    const QImage img = art::typography::render(t, 1.0f);
    if (img.isNull())
        return;
    const QPointF pos((m_document.canvasSize().width() - img.width()) / 2.0,
                      (m_document.canvasSize().height() - img.height()) / 2.0);
    CanvasLayer layer = CanvasLayer::createImage(img, pos, text.left(24));
    layer.type = LAYER_TEXT;
    layer.text = text;
    /* text stays editable: the treatment is stored, the raster is a render */
    layer.metadata[QStringLiteral("textTreatment")] = t.toVariant();
    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Add text");
    QList<CanvasLayer> layers = m_document.layers();
    layers.append(std::move(layer));
    cmd.index = layers.size() - 1;
    cmd.prevCurrentIndex = m_document.currentLayerIndex();
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(cmd.index);
    cmd.layer = *m_document.layerAt(cmd.index);
    m_undoStack.pushInsertLayer(cmd);
    if (m_canvas) m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::editTextLayer(int index, const QString &text,
                                         const QString &styleId, double sizePx,
                                         const QString &color, double tracking)
{
    CanvasLayer *l = m_document.layerAt(index);
    if (!l || l->type != LAYER_TEXT)
        return;
    if (!l->metadata.contains(QStringLiteral("textTreatment")))
        return;

    const art::TextTreatment before = art::TextTreatment::fromVariant(
        l->metadata.value(QStringLiteral("textTreatment")).toMap());

    art::TextTreatment t = before;
    t.text = text;
    if (art::typography::findById(styleId))
        t.styleId = styleId;
    if (sizePx >= 4.0 && sizePx <= 2048.0)
        t.sizePx = float(sizePx);
    QColor c(color);
    if (c.isValid())
        t.color = c;
    if (tracking > -98.0 && tracking <= 2.0)
        t.trackingOverride = float(tracking);

    const QImage after = art::typography::render(t, 1.0f);

    /* keep the visual center anchored: re-anchor the new raster so the text
     * stays where it was while its size changes (spec §57: text stays
     * editable without dialog hell — the glyphs move, the text doesn't) */
    const QPointF oldCenter = l->position + QPointF(l->raster.rect().center());
    const QPointF newPos = after.isNull()
                               ? oldCenter
                               : oldCenter - QPointF(after.rect().center());

    SetTextCommand cmd;
    cmd.layerId = l->id;
    cmd.beforeText = before.text;
    cmd.afterText = t.text;
    cmd.beforeTreatment = before;
    cmd.afterTreatment = t;
    cmd.beforeRaster = l->raster;
    cmd.afterRaster = after;
    cmd.beforePosition = l->position;
    cmd.afterPosition = newPos;
    cmd.desc = QStringLiteral("Edit text");
    if (cmd.beforeText == cmd.afterText &&
        cmd.beforeRaster == cmd.afterRaster &&
        cmd.beforeTreatment.styleId == cmd.afterTreatment.styleId &&
        cmd.beforeTreatment.sizePx == cmd.afterTreatment.sizePx &&
        cmd.beforeTreatment.color == cmd.afterTreatment.color &&
        cmd.beforeTreatment.trackingOverride == cmd.afterTreatment.trackingOverride)
        return; /* nothing changed */
    cmd.apply(m_document);
    m_undoStack.pushCommand(std::make_unique<SetTextCommand>(cmd));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Text updated — one undo step undoes it."));
}

QVariantMap PaintbrushController::textTreatmentFor(int index) const
{
    const CanvasLayer *l = m_document.layerAt(index);
    if (!l || l->type != LAYER_TEXT)
        return {};
    const QVariant v = l->metadata.value(QStringLiteral("textTreatment"));
    if (!v.isValid())
        return {};
    return v.toMap();
}

void PaintbrushController::addBadge(const QString &kind, const QString &color, double size)
{
    QColor c(color);
    if (!c.isValid())
    {
        const art::Mood *m = art::mood::findById(m_document.moodId());
        c = m ? m->palette.accent : QColor(0xE0, 0xA4, 0x3C);
    }
    const double s = qBound(16.0, size, 1024.0);
    const QImage img = art::badges::render(art::badges::kindFromName(kind), QSizeF(s, s), c,
                                           QColor(0, 0, 0, 0), 0.0f, 1.0f, m_document.artSeed());
    if (img.isNull())
        return;
    const QPointF pos((m_document.canvasSize().width() - img.width()) / 2.0,
                      (m_document.canvasSize().height() - img.height()) / 2.0);
    CanvasLayer layer = CanvasLayer::createImage(img, pos, QStringLiteral("Badge: %1").arg(kind));
    layer.type = LAYER_STICKER;
    layer.metadata[QStringLiteral("badge")] = kind;
    layer.metadata[QStringLiteral("badgeColor")] = c.name();
    InsertLayerCommand cmd;
    cmd.desc = QStringLiteral("Add badge");
    QList<CanvasLayer> layers = m_document.layers();
    layers.append(std::move(layer));
    cmd.index = layers.size() - 1;
    cmd.prevCurrentIndex = m_document.currentLayerIndex();
    m_document.setLayers(layers);
    m_document.setCurrentLayerIndex(cmd.index);
    cmd.layer = *m_document.layerAt(cmd.index);
    m_undoStack.pushInsertLayer(cmd);
    if (m_canvas) m_canvas->markCanvasDirty();
    emit documentChanged();
}

QString PaintbrushController::badgePreviewDataUrl(const QString &kind, const QString &color) const
{
    QColor c(color);
    if (!c.isValid())
        c = QColor(0xE0, 0xA4, 0x3C);
    return dataUrl(art::badges::render(art::badges::kindFromName(kind), QSizeF(40, 40), c));
}

QString PaintbrushController::paperPreviewDataUrl(const QString &paperId) const
{
    const art::PaperStock *p = art::paper::findById(paperId);
    if (!p)
        return QString();
    art::PaperRenderer r;
    art::PaperRenderer::State st;
    st.stock = p;
    const art::ArtLightRig rig = m_document.lightRig();
    st.rig = &rig;
    st.scale = m_document.artScale(1.0f);
    return dataUrl(r.render(QSize(56, 56), st, QRect(0, 0, 56, 56)));
}

/* Synthetic acceptance subject: a transparent, stylized idol silhouette with
 * real skin/fabric shading so every recipe (rim light, recolour, halftone,
 * die-cut) has structure to work with. Not shipped artwork — a test fixture. */
static QImage makeCreativeSelfTestIdol()
{
    const QSize size(720, 1120);
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    const QColor skin(232, 185, 148), skinShade(196, 143, 112);
    const QColor hair(24, 18, 30), jacket(38, 96, 244), jacketDark(18, 44, 128);
    const QColor pants(18, 18, 26), boot(8, 8, 12), accent(255, 76, 150);

    /* legs */
    QLinearGradient legGrad(0, 700, 0, 1040);
    legGrad.setColorAt(0.0, pants.lighter(125));
    legGrad.setColorAt(1.0, pants);
    p.setBrush(legGrad);
    p.drawRoundedRect(QRectF(298, 700, 74, 350), 30, 30);
    p.drawRoundedRect(QRectF(372, 700, 74, 350), 30, 30);

    /* boots */
    p.setBrush(boot);
    p.drawRoundedRect(QRectF(286, 1022, 96, 62), 18, 18);
    p.drawRoundedRect(QRectF(364, 1022, 96, 62), 18, 18);
    p.setBrush(accent.darker(115));
    p.drawRoundedRect(QRectF(292, 1058, 84, 10), 5, 5);
    p.drawRoundedRect(QRectF(370, 1058, 84, 10), 5, 5);

    /* torso / jacket */
    QPainterPath torso;
    torso.moveTo(258, 396);
    torso.cubicTo(286, 360, 340, 344, 372, 344);
    torso.cubicTo(404, 344, 458, 360, 486, 396);
    torso.lineTo(512, 610);
    torso.cubicTo(470, 660, 420, 690, 372, 690);
    torso.cubicTo(324, 690, 274, 660, 232, 610);
    torso.closeSubpath();
    QLinearGradient jacketGrad(250, 0, 500, 0);
    jacketGrad.setColorAt(0.0, jacketDark);
    jacketGrad.setColorAt(0.45, jacket);
    jacketGrad.setColorAt(1.0, jacket.lighter(112));
    p.setBrush(jacketGrad);
    p.drawPath(torso);

    /* arms */
    p.setBrush(jacketDark);
    p.drawRoundedRect(QRectF(214, 402, 62, 300), 28, 28);
    p.drawRoundedRect(QRectF(468, 402, 62, 300), 28, 28);
    p.setBrush(skin);
    p.drawEllipse(QPointF(245, 722), 34, 42);
    p.drawEllipse(QPointF(499, 722), 34, 42);

    /* belt + zipper + accents */
    p.setBrush(QColor(12, 12, 18));
    p.drawRoundedRect(QRectF(238, 626, 272, 30), 12, 12);
    p.setBrush(accent);
    p.drawRoundedRect(QRectF(246, 632, 256, 8), 4, 4);
    p.setPen(QPen(QColor(220, 232, 255, 190), 3));
    p.drawLine(QPointF(372, 366), QPointF(372, 626));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 220, 120));
    p.drawEllipse(QPointF(372, 380), 9, 9);

    /* neck */
    p.setBrush(skinShade);
    p.drawRoundedRect(QRectF(332, 292, 80, 78), 26, 26);

    /* head */
    QRadialGradient face(QPointF(340, 206), 150);
    face.setColorAt(0.0, skin.lighter(108));
    face.setColorAt(1.0, skinShade);
    p.setBrush(face);
    p.drawEllipse(QPointF(372, 206), 126, 150);

    /* hair: crown + side locks */
    QPainterPath hairPath;
    hairPath.moveTo(246, 210);
    hairPath.cubicTo(238, 76, 320, 36, 382, 38);
    hairPath.cubicTo(472, 42, 520, 104, 506, 218);
    hairPath.cubicTo(492, 150, 450, 110, 392, 108);
    hairPath.cubicTo(330, 106, 286, 148, 246, 210);
    hairPath.closeSubpath();
    p.setBrush(hair);
    p.drawPath(hairPath);
    p.drawRoundedRect(QRectF(238, 168, 54, 220), 26, 26);
    p.drawRoundedRect(QRectF(452, 168, 54, 220), 26, 26);
    p.setBrush(QColor(70, 40, 110));
    p.drawEllipse(QPointF(300, 130), 56, 34);

    /* simple face so the fixture reads as a figure, not a blob */
    p.setBrush(QColor(28, 22, 30));
    p.drawEllipse(QPointF(330, 210), 9, 13);
    p.drawEllipse(QPointF(414, 210), 9, 13);
    p.setBrush(QColor(255, 255, 255, 210));
    p.drawEllipse(QPointF(333, 205), 3, 4);
    p.drawEllipse(QPointF(417, 205), 3, 4);
    p.setBrush(QColor(196, 70, 96));
    p.drawEllipse(QPointF(372, 266), 22, 9);

    /* floor shadow in fixture space (part of the cutout, intentionally) */
    p.setBrush(QColor(0, 0, 0, 26));
    p.drawEllipse(QPointF(372, 1096), 190, 22);

    p.end();
    return img;
}

QString PaintbrushController::runCreativeSelfTest()
{
    if (!m_creative)
        return QStringLiteral("FAIL: creative engine missing");
    QStringList report;
    QFile progress(QDir::tempPath() + QStringLiteral("/nai-pb-selftest-progress.txt"));
    progress.open(QIODevice::WriteOnly | QIODevice::Truncate);
    auto note = [&](const QString &line) {
        report << line;
        if (progress.isOpen())
        {
            progress.write(line.toUtf8());
            progress.write("\n");
            progress.flush();
        }
    };
    auto failed = [&]() {
        for (const QString &line : report)
            if (line.startsWith(QLatin1String("FAIL")))
                return true;
        return false;
    };

    /* The test replaces the document. Never do that over real work: a
     * startup flag must not be able to discard a project or the recovery
     * copy, and a blank default canvas (one empty layer) is safe. */
    bool hasWork = m_recoveryAvailable || !m_document.projectPath().isEmpty();
    if (!hasWork)
    {
        for (const CanvasLayer &l : m_document.layers())
        {
            if (l.raster.isNull())
                continue;
            const QImage img =
                l.raster.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < img.height() && !hasWork; y += 4)
            {
                const QRgb *line = reinterpret_cast<const QRgb *>(
                    img.constScanLine(y));
                for (int x = 0; x < img.width(); x += 4)
                {
                    if (qAlpha(line[x]) > 0)
                    {
                        hasWork = true;
                        break;
                    }
                }
            }
            if (hasWork)
                break;
        }
    }
    if (hasWork)
    {
        note(QStringLiteral("SKIP: the document contains work; the self-test "
                            "needs a pristine canvas"));
        note(QStringLiteral("RESULT: SKIP"));
        return report.join(QLatin1Char('\n'));
    }

    /* The test canvas must never become the recovery file that hides the
     * user's next session. */
    m_autosaveTimer.stop();

    newBlankCanvas(1440, 1800);
    const QImage idol = makeCreativeSelfTestIdol();
    addImageLayer(idol, QPointF(360, 340),
                  QStringLiteral("Self-test cutout"), LAYER_STICKER);
    m_document.setCurrentLayerIndex(m_document.layers().size() - 1);
    /* scale the source so the commit must honour the on-canvas size, not the
     * raster's pixel size */
    if (!m_document.layers().isEmpty())
        m_document.layersRef().last().scale = 0.8f;
    emit documentChanged();

    m_creative->openForSelection();
    if (!m_creative->hasSubject())
    {
        note(QStringLiteral("FAIL: pad has no subject"));
        return report.join(QLatin1Char('\n'));
    }
    note(QStringLiteral("subject: %1").arg(m_creative->subjectLabel()));

    const QString dir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("nai-paintbrush-creative-selftest"));
    QDir().mkpath(dir);
    const QStringList patches = {
        QStringLiteral("neon-idol"), QStringLiteral("sticker-forge"),
        QStringLiteral("pastel-stage"), QStringLiteral("nocturne"),
        QStringLiteral("ink-creature"), QStringLiteral("spark-storm"),
        QStringLiteral("kaleido"), QStringLiteral("motion-echo"),
        QStringLiteral("electric-kpop"), QStringLiteral("holo-fashion"),
        QStringLiteral("holo-liquid"), QStringLiteral("holo-opal"),
        QStringLiteral("holo-chrome"), QStringLiteral("holo-prism"),
        QStringLiteral("holo-wrap"), QStringLiteral("holo-chameleon"),
        QStringLiteral("holo-glitter"), QStringLiteral("holo-livery"),
        QStringLiteral("scene-forge"), QStringLiteral("auto-splatter"),
        QStringLiteral("stage-mode"), QStringLiteral("paper-theatre")
    };
    for (const QString &id : patches)
    {
        m_creative->selectPatch(id);
        m_creative->movePad(0.68, 0.62, 0.35, false);
        const QImage preview = m_creative->previewImage();
        if (preview.isNull())
        {
            note(QStringLiteral("FAIL: %1 produced no preview").arg(id));
            continue;
        }
        const QString path = QStringLiteral("%1/selftest-%2.png").arg(dir, id);
        if (!preview.save(path))
            note(QStringLiteral("FAIL: cannot save %1").arg(path));
        else
            note(QStringLiteral("preview %1 %2x%3")
                     .arg(id).arg(preview.width()).arg(preview.height()));
    }

    /* Pad reaction gate: the starter looks and Paper Theatre's parallax must
     * visibly change pixels between a low/quiet pose and a high/energetic/
     * hold pose. This is the regression test for "the pad does nothing"
     * reports. */
    {
        const QStringList reactive = {
            QStringLiteral("neon-idol"), QStringLiteral("electric-kpop"),
            QStringLiteral("sticker-forge"), QStringLiteral("pastel-stage"),
            QStringLiteral("paper-theatre")
        };
        for (const QString &id : reactive)
        {
            m_creative->selectPatch(id);
            m_creative->movePad(0.22, 0.22, 0.0, false);
            const QImage low =
                m_creative->previewImage().convertToFormat(QImage::Format_ARGB32);
            m_creative->movePad(0.78, 0.78, 0.85, true);
            const QImage high =
                m_creative->previewImage().convertToFormat(QImage::Format_ARGB32);
            if (low.isNull() || high.isNull() || low.size() != high.size())
            {
                note(QStringLiteral("FAIL: pad reaction %1 has no frames").arg(id));
                continue;
            }
            qint64 sum = 0;
            int count = 0;
            for (int y = 0; y < low.height(); y += 3)
            {
                const QRgb *a = reinterpret_cast<const QRgb *>(
                    low.constScanLine(y));
                const QRgb *b = reinterpret_cast<const QRgb *>(
                    high.constScanLine(y));
                for (int x = 0; x < low.width(); x += 3)
                {
                    sum += qAbs(qRed(a[x]) - qRed(b[x]))
                           + qAbs(qGreen(a[x]) - qGreen(b[x]))
                           + qAbs(qBlue(a[x]) - qBlue(b[x]));
                    ++count;
                }
            }
            const double avg = count > 0 ? double(sum) / (count * 3.0) : 0.0;
            note(QStringLiteral("pad reaction %1: %2/255")
                     .arg(id).arg(avg, 0, 'f', 2));
            if (avg < 0.5)
                note(QStringLiteral("FAIL: pad does not change %1").arg(id));
        }
        m_creative->setHold(false);
        m_creative->movePad(0.5, 0.5, 0.0, false);
    }

    /* Gesture capture: record a two-point move, verify the loop starts. */
    m_creative->toggleRecord();
    m_creative->movePad(0.22, 0.24, 0.12, false);
    m_creative->movePad(0.78, 0.68, 0.55, true);
    m_creative->toggleRecord();
    note(m_creative->hasRecording()
             ? QStringLiteral("gesture record: OK")
             : QStringLiteral("FAIL: gesture record empty"));
    m_creative->toggleReplay();
    note(m_creative->replaying() ? QStringLiteral("gesture loop: OK")
                                 : QStringLiteral("FAIL: loop did not start"));
    m_creative->toggleReplay();
    m_creative->setHold(false);

    note(QStringLiteral("preview p95: %1 ms (%2 samples)")
             .arg(m_creative->previewP95Ms(), 0, 'f', 2)
             .arg(patches.size()));

    const int baseLayers = m_document.layers().size();

    /* Neon Idol: commit + optional suggestion + one undo restores exactly. */
    m_creative->selectPatch(QStringLiteral("neon-idol"));
    m_creative->movePad(0.72, 0.60, 0.30, true);
    m_creative->dumpStagePieces(QStringLiteral("%1/pieces-neon").arg(dir));
    QString error = m_creative->commit();
    const int neonAfter = m_document.layers().size();
    if (!error.isEmpty())
        note(QStringLiteral("FAIL: neon commit: %1").arg(error));
    if (neonAfter - baseLayers < 4)
        note(QStringLiteral("FAIL: neon commit added %1 layers")
                 .arg(neonAfter - baseLayers));
    else
        note(QStringLiteral("commit neon: +%1 layers").arg(neonAfter - baseLayers));
    note(QStringLiteral("commit neon: %1 ms")
             .arg(m_creative->lastCommitMs(), 0, 'f', 1));
    m_creative->refreshHistory();
    const QVariantList lookHistory = m_creative->history();
    note(lookHistory.isEmpty()
             ? QStringLiteral("FAIL: recent-look history empty")
             : QStringLiteral("recent look history: %1 (%2)")
                   .arg(lookHistory.size())
                   .arg(lookHistory.first().toMap()
                            .value(QStringLiteral("patchName")).toString()));

    /* Rename/delete a committed look; delete is one undo. */
    if (!lookHistory.isEmpty())
    {
        const QString group =
            lookHistory.first().toMap().value(QStringLiteral("group")).toString();
        const QString renameError =
            renameCreativeGroup(group, QStringLiteral("Test Look"));
        note(renameError.isEmpty()
                 ? QStringLiteral("look rename: OK")
                 : QStringLiteral("FAIL: look rename: %1").arg(renameError));
        m_creative->refreshHistory();
        const QString renamedLabel =
            m_creative->history().isEmpty()
                ? QString()
                : m_creative->history().first().toMap()
                      .value(QStringLiteral("label")).toString();
        if (renamedLabel != QLatin1String("Test Look"))
            note(QStringLiteral("FAIL: look rename label '%1'")
                     .arg(renamedLabel));
        const int beforeDelete = m_document.layers().size();
        const QString deleteError = deleteCreativeGroup(group);
        if (!deleteError.isEmpty())
            note(QStringLiteral("FAIL: look delete: %1").arg(deleteError));
        else
            note(QStringLiteral("look delete: -%1 layers")
                     .arg(beforeDelete - m_document.layers().size()));
        undo();
        if (m_document.layers().size() != beforeDelete)
            note(QStringLiteral("FAIL: look delete undo (%1 != %2)")
                     .arg(m_document.layers().size()).arg(beforeDelete));
        else
            note(QStringLiteral("look delete undo: OK"));
        m_creative->refreshHistory();
    }

    /* Save -> load -> export round trip over the committed layers. */
    {
        const QString projectPath =
            QDir(QStandardPaths::writableLocation(
                     QStandardPaths::TempLocation))
                .filePath(QStringLiteral("nai-pb-selftest.pbproj"));
        const QString exportPath =
            QDir(QStandardPaths::writableLocation(
                     QStandardPaths::TempLocation))
                .filePath(QStringLiteral("nai-pb-selftest-export.png"));
        ProjectStore store;
        const QString saveError = store.save(&m_document, projectPath);
        if (!saveError.isEmpty())
            note(QStringLiteral("FAIL: project save: %1").arg(saveError));
        else
        {
            PaintbrushDocument loaded;
            const QString loadError = store.load(&loaded, projectPath);
            if (!loadError.isEmpty())
                note(QStringLiteral("FAIL: project load: %1").arg(loadError));
            else if (loaded.layers().size() != m_document.layers().size())
                note(QStringLiteral("FAIL: reload layer count %1 != %2")
                         .arg(loaded.layers().size())
                         .arg(m_document.layers().size()));
            else
            {
                bool creativeMeta = false;
                for (const CanvasLayer &l : loaded.layers())
                    if (!l.metadata.value(QStringLiteral("creative.patch"))
                             .toString().isEmpty())
                        creativeMeta = true;
                if (!creativeMeta)
                    note(QStringLiteral("FAIL: reload lost creative metadata"));
                const QString exportError = ExportService::exportImage(
                    &loaded, exportPath, QStringLiteral("png"));
                if (!exportError.isEmpty())
                    note(QStringLiteral("FAIL: export: %1").arg(exportError));
                else if (QFileInfo(exportPath).size() <= 0)
                    note(QStringLiteral("FAIL: export produced an empty file"));
                else if (!creativeMeta)
                    note(QStringLiteral("FAIL: reload/export metadata gap"));
                else
                    note(QStringLiteral(
                             "save/reopen/export: OK (%1 layers, %2 KB)")
                             .arg(loaded.layers().size())
                             .arg(QFileInfo(exportPath).size() / 1024));
            }
        }
    }

    if (m_creative->suggestionVisible())
    {
        note(QStringLiteral("suggestion kind: %1")
                 .arg(m_creative->suggestionKind()));
        m_creative->acceptSuggestion();
        note(QStringLiteral("suggestion: +%1 layer")
                 .arg(m_document.layers().size() - neonAfter));
        undo();
    }

    /* Resume the committed look from its metadata and replace it in one undo. */
    {
        int topCreative = -1;
        for (int i = m_document.layers().size() - 1; i >= 0; --i)
        {
            const CanvasLayer *l = m_document.layerAt(i);
            if (l && !l->metadata.value(QStringLiteral("creative.patch"))
                          .toString().isEmpty())
            {
                topCreative = i;
                break;
            }
        }
        if (topCreative < 0)
            note(QStringLiteral("FAIL: no resumable creative layer"));
        else if (!m_creative->restoreFromCommittedLayer(topCreative))
            note(QStringLiteral("FAIL: resume look did not open"));
        else
        {
            note(QStringLiteral("resume look: OK (%1)")
                     .arg(m_creative->activePatchId()));
            m_creative->movePad(0.35, 0.62, 0.20, false);
            const QString resumeError = m_creative->commit();
            if (!resumeError.isEmpty())
                note(QStringLiteral("FAIL: resume commit: %1").arg(resumeError));
            else
            {
                note(QStringLiteral("resume replace: %1 layers")
                         .arg(m_document.layers().size()));
                undo();
                if (m_document.layers().size() != neonAfter)
                    note(QStringLiteral("FAIL: resume undo (%1 != %2)")
                             .arg(m_document.layers().size()).arg(neonAfter));
                else
                    note(QStringLiteral("resume one-undo: OK"));
            }
        }
    }

    undo();
    if (m_document.layers().size() != baseLayers)
        note(QStringLiteral("FAIL: one-undo contract broke (%1 != %2)")
                 .arg(m_document.layers().size()).arg(baseLayers));
    else
        note(QStringLiteral("one-undo contract: OK"));

    /* OpenFill Expand and true contour calligraphy. */
    {
        const QSize before = m_document.canvasSize();
        const QString expandError = expandCanvas(10);
        const QSize after = m_document.canvasSize();
        if (!expandError.isEmpty())
            note(QStringLiteral("FAIL: expand canvas: %1").arg(expandError));
        else if (after.width() <= before.width()
                 || after.height() <= before.height())
            note(QStringLiteral("FAIL: expand canvas did not grow"));
        else
            note(QStringLiteral("expand canvas: %1x%2 -> %3x%4")
                     .arg(before.width()).arg(before.height())
                     .arg(after.width()).arg(after.height()));
        undo();
        if (m_document.canvasSize() != before)
            note(QStringLiteral("FAIL: expand canvas undo"));
        else
            note(QStringLiteral("expand canvas undo: OK"));
    }
    {
        for (int i = 0; i < m_document.layers().size(); ++i)
        {
            const CanvasLayer *l = m_document.layerAt(i);
            if (l && l->name == QLatin1String("Self-test cutout"))
            {
                m_document.setCurrentLayerIndex(i);
                emit documentChanged();
                break;
            }
        }
        const int before = m_document.layers().size();
        addCreativeCalligraphy(QStringLiteral("ink"));
        const int added = m_document.layers().size() - before;
        note(QStringLiteral("contour calligraphy: +%1 layer").arg(added));
        if (added != 1)
            note(QStringLiteral("FAIL: calligraphy added %1").arg(added));
        undo();
        if (m_document.layers().size() != baseLayers)
            note(QStringLiteral("FAIL: calligraphy undo"));
        else
            note(QStringLiteral("contour calligraphy undo: OK"));
    }

    /* Reuse the current look on the selected cutout (one undo). */
    {
        for (int i = 0; i < m_document.layers().size(); ++i)
        {
            const CanvasLayer *l = m_document.layerAt(i);
            if (l && l->name == QLatin1String("Self-test cutout"))
            {
                m_document.setCurrentLayerIndex(i);
                emit documentChanged();
                break;
            }
        }
        const QString applyError = m_creative->applyLookToSelection();
        if (!applyError.isEmpty())
            note(QStringLiteral("FAIL: apply look: %1").arg(applyError));
        else
        {
            note(QStringLiteral("apply look: +%1 layers")
                     .arg(m_document.layers().size() - baseLayers));
            undo();
            if (m_document.layers().size() != baseLayers)
                note(QStringLiteral("FAIL: apply look undo (%1 != %2)")
                         .arg(m_document.layers().size()).arg(baseLayers));
            else
                note(QStringLiteral("apply look undo: OK"));
        }
    }

    /* Sticker Forge: commit + one undo. */
    m_creative->selectPatch(QStringLiteral("sticker-forge"));
    m_creative->movePad(0.42, 0.70, 0.20, false);
    error = m_creative->commit();
    const int stickerAfter = m_document.layers().size();
    if (!error.isEmpty())
        note(QStringLiteral("FAIL: sticker commit: %1").arg(error));
    if (stickerAfter - baseLayers < 4)
        note(QStringLiteral("FAIL: sticker commit added %1 layers")
                 .arg(stickerAfter - baseLayers));
    else
        note(QStringLiteral("commit sticker: +%1 layers").arg(stickerAfter - baseLayers));
    undo();
    if (m_document.layers().size() != baseLayers)
        note(QStringLiteral("FAIL: sticker undo (%1 != %2)")
                 .arg(m_document.layers().size()).arg(baseLayers));
    else
        note(QStringLiteral("sticker undo: OK"));

    /* Electric K-pop: the full subject-aware look in one undo. */
    m_creative->openForSelection();
    m_creative->selectPatch(QStringLiteral("electric-kpop"));
    m_creative->movePad(0.62, 0.58, 0.42, false);
    error = m_creative->commit();
    const int electricAfter = m_document.layers().size();
    if (!error.isEmpty())
        note(QStringLiteral("FAIL: electric commit: %1").arg(error));
    else if (electricAfter - baseLayers < 6)
        note(QStringLiteral("FAIL: electric added %1 layers")
                 .arg(electricAfter - baseLayers));
    else
        note(QStringLiteral("commit electric: +%1 layers")
                 .arg(electricAfter - baseLayers));

    /* The committed subject must honour the on-canvas scale (0.8), and a
     * second open must reuse the hidden source rather than an effect layer. */
    {
        const QList<CanvasLayer> built = m_document.layers();
        const CanvasLayer *source = nullptr;
        const CanvasLayer *subject = nullptr;
        for (const CanvasLayer &l : built)
        {
            if (l.name == QStringLiteral("Self-test cutout"))
                source = &l;
            else if (l.name == QStringLiteral("Voltage idol"))
                subject = &l;
        }
        if (!source || !subject)
            note(QStringLiteral("FAIL: committed subject layer missing"));
        else
        {
            QPoint srcOffset;
            const QImage srcCut = creative::trimTransparent(
                creative::maskToCutout(source->raster,
                                       creative::layerMask(*source)),
                &srcOffset);
            const QRectF srcRect =
                srcCut.isNull()
                    ? QRectF()
                    : source->transform().mapRect(
                          QRectF(srcOffset, srcCut.size()));
            const QRectF outRect =
                subject->transform().mapRect(subject->raster.rect());
            note(QStringLiteral("source content %1x%2 raster %3x%4 scale %5")
                     .arg(srcRect.width()).arg(srcRect.height())
                     .arg(source->raster.width()).arg(source->raster.height())
                     .arg(source->scale));
            note(QStringLiteral("subject content %1x%2 raster %3x%4 scale %5")
                     .arg(outRect.width()).arg(outRect.height())
                     .arg(subject->raster.width()).arg(subject->raster.height())
                     .arg(subject->scale));
            const qreal ratio = outRect.width() / qMax(1.0, srcRect.width());
            if (std::fabs(ratio - 1.0) > 0.03)
                note(QStringLiteral(
                         "FAIL: committed subject scale %1 vs source %2")
                         .arg(outRect.width()).arg(srcRect.width()));
            else
                note(QStringLiteral("committed subject scale: OK"));
        }
    }
    m_creative->openForSelection();
    if (m_creative->subjectLabel() != QStringLiteral("Self-test cutout"))
        note(QStringLiteral("FAIL: repeated apply picked '%1' instead of the "
                            "hidden source")
                 .arg(m_creative->subjectLabel()));
    else
        note(QStringLiteral("repeated apply: original source kept"));

    undo();
    if (m_document.layers().size() != baseLayers)
        note(QStringLiteral("FAIL: electric undo (%1 != %2)")
                 .arg(m_document.layers().size()).arg(baseLayers));
    else
        note(QStringLiteral("electric undo: OK"));

    /* Holographic fashion: material recolour + sheen + butterflies. */
    m_creative->openForSelection();
    m_creative->selectPatch(QStringLiteral("holo-fashion"));
    m_creative->movePad(0.55, 0.66, 0.24, true);
    error = m_creative->commit();
    const int holoAfter = m_document.layers().size();
    if (!error.isEmpty())
        note(QStringLiteral("FAIL: holo commit: %1").arg(error));
    else if (holoAfter - baseLayers < 6)
        note(QStringLiteral("FAIL: holo added %1 layers")
                 .arg(holoAfter - baseLayers));
    else
        note(QStringLiteral("commit holo: +%1 layers")
                 .arg(holoAfter - baseLayers));
    undo();
    if (m_document.layers().size() != baseLayers)
        note(QStringLiteral("FAIL: holo undo (%1 != %2)")
                 .arg(m_document.layers().size()).arg(baseLayers));
    else
        note(QStringLiteral("holo undo: OK"));

    /* Paper Theatre: the layered paper scene commits as independent props in
     * one undo; each prop keeps its kind/depth metadata across a project
     * reload (the same Layer::metadata map the project store round-trips). */
    for (int i = 0; i < m_document.layers().size(); ++i)
    {
        const CanvasLayer *l = m_document.layerAt(i);
        if (l && l->name == QLatin1String("Self-test cutout"))
        {
            m_document.setCurrentLayerIndex(i);
            emit documentChanged();
            break;
        }
    }
    m_creative->openForSelection();
    m_creative->selectPatch(QStringLiteral("paper-theatre"));
    m_creative->movePad(0.64, 0.44, 0.22, false);
    error = m_creative->commit();
    const int theatreAfter = m_document.layers().size();
    if (!error.isEmpty())
        note(QStringLiteral("FAIL: paper-theatre commit: %1").arg(error));
    else if (theatreAfter - baseLayers < 12)
        note(QStringLiteral("FAIL: paper-theatre added %1 layers")
                 .arg(theatreAfter - baseLayers));
    else
        note(QStringLiteral("commit paper-theatre: +%1 layers")
                 .arg(theatreAfter - baseLayers));
    {
        int propLayers = 0;
        QStringList propKinds;
        for (int i = baseLayers; i < m_document.layers().size(); ++i)
        {
            const CanvasLayer *l = m_document.layerAt(i);
            if (!l
                || l->metadata.value(QStringLiteral("creative.patch"))
                       .toString() != QLatin1String("paper-theatre"))
                continue;
            const QString prop =
                l->metadata.value(QStringLiteral("creative.prop")).toString();
            if (prop.isEmpty())
                continue;
            ++propLayers;
            if (!propKinds.contains(prop))
                propKinds.append(prop);
        }
        if (propLayers < 12 || propKinds.size() < 8)
            note(QStringLiteral("FAIL: paper-theatre props %1 layers / %2 kinds")
                     .arg(propLayers).arg(propKinds.size()));
        else
            note(QStringLiteral("paper-theatre props: %1 layers / %2 kinds")
                     .arg(propLayers).arg(propKinds.size()));

        const QString theatreProject =
            QDir(QStandardPaths::writableLocation(
                     QStandardPaths::TempLocation))
                .filePath(QStringLiteral("nai-pb-paper-theatre.pbproj"));
        ProjectStore theatreStore;
        const QString theatreSaveError =
            theatreStore.save(&m_document, theatreProject);
        if (!theatreSaveError.isEmpty())
            note(QStringLiteral("FAIL: paper-theatre save: %1")
                     .arg(theatreSaveError));
        else
        {
            PaintbrushDocument loaded;
            const QString theatreLoadError =
                theatreStore.load(&loaded, theatreProject);
            int loadedProps = 0;
            for (const CanvasLayer &l : loaded.layers())
                if (l.metadata.value(QStringLiteral("creative.patch"))
                            .toString()
                        == QLatin1String("paper-theatre")
                    && !l.metadata.value(QStringLiteral("creative.prop"))
                            .toString().isEmpty())
                    ++loadedProps;
            if (!theatreLoadError.isEmpty())
                note(QStringLiteral("FAIL: paper-theatre reload: %1")
                         .arg(theatreLoadError));
            else if (loadedProps < 12)
                note(QStringLiteral("FAIL: paper-theatre reload props %1")
                         .arg(loadedProps));
            else
                note(QStringLiteral("paper-theatre reload props: %1")
                         .arg(loadedProps));
        }
    }
    undo();
    if (m_document.layers().size() != baseLayers)
        note(QStringLiteral("FAIL: paper-theatre undo (%1 != %2)")
                 .arg(m_document.layers().size()).arg(baseLayers));
    else
        note(QStringLiteral("paper-theatre undo: OK"));

    /* Linked lineup: five poses, one undo. */
    m_document.setCurrentLayerIndex(0);
    emit documentChanged();
    duplicateIntoLineup();
    const int lineupAfter = m_document.layers().size();
    if (lineupAfter - baseLayers != 5)
        note(QStringLiteral("FAIL: lineup added %1 layers")
                 .arg(lineupAfter - baseLayers));
    else
        note(QStringLiteral("lineup: +5 linked poses"));
    undo();
    if (m_document.layers().size() != baseLayers)
        note(QStringLiteral("FAIL: lineup undo (%1 != %2)")
                 .arg(m_document.layers().size()).arg(baseLayers));
    else
        note(QStringLiteral("lineup undo: OK"));

    auto selectSelfTestCutout = [&]() {
        for (int i = 0; i < m_document.layers().size(); ++i)
        {
            const CanvasLayer *l = m_document.layerAt(i);
            if (l && l->name == QLatin1String("Self-test cutout"))
            {
                m_document.setCurrentLayerIndex(i);
                emit documentChanged();
                break;
            }
        }
    };

    /* Pastel Lineup and Motion Echo: still-source fallback in this fixture. */
    {
        selectSelfTestCutout();
        const int before = m_document.layers().size();
        const QString lineupError = createPastelLineup();
        if (!lineupError.isEmpty())
            note(QStringLiteral("FAIL: pastel lineup: %1").arg(lineupError));
        else
        {
            const int added = m_document.layers().size() - before;
            note(QStringLiteral("pastel lineup: +%1 layers").arg(added));
            if (added < 5)
                note(QStringLiteral("FAIL: pastel lineup too few layers"));
        }
        undo();
        if (m_document.layers().size() != baseLayers)
            note(QStringLiteral("FAIL: pastel lineup undo (%1 != %2)")
                     .arg(m_document.layers().size()).arg(baseLayers));
        else
            note(QStringLiteral("pastel lineup undo: OK"));
    }
    {
        selectSelfTestCutout();
        const int before = m_document.layers().size();
        const QString echoError = createMotionEcho();
        if (!echoError.isEmpty())
            note(QStringLiteral("FAIL: motion echo: %1").arg(echoError));
        else
        {
            const int added = m_document.layers().size() - before;
            note(QStringLiteral("motion echo: +%1 layers").arg(added));
            if (added < 5)
                note(QStringLiteral("FAIL: motion echo too few layers"));
        }
        undo();
        if (m_document.layers().size() != baseLayers)
            note(QStringLiteral("FAIL: motion echo undo (%1 != %2)")
                     .arg(m_document.layers().size()).arg(baseLayers));
        else
            note(QStringLiteral("motion echo undo: OK"));
    }

    /* Generic linked duplicate presets. */
    for (int preset = 1; preset <= 3; ++preset)
    {
        selectSelfTestCutout();
        const int before = m_document.layers().size();
        duplicateLinked(preset);
        const int added = m_document.layers().size() - before;
        const int expected = preset == 1 ? 2 : (preset == 2 ? 4 : 3);
        if (added != expected)
            note(QStringLiteral("FAIL: duplicate preset %1 added %2 (want %3)")
                     .arg(preset).arg(added).arg(expected));
        else
            note(QStringLiteral("duplicate preset %1: OK").arg(preset));
        undo();
        if (m_document.layers().size() != baseLayers)
            note(QStringLiteral("FAIL: duplicate preset %1 undo").arg(preset));
    }

    /* Editable agent stroke suggestion. */
    {
        selectSelfTestCutout();
        const int before = m_document.layers().size();
        addCreativeStrokeAccent(QStringLiteral("ribbon"));
        const int added = m_document.layers().size() - before;
        note(QStringLiteral("agent stroke: +%1 layer").arg(added));
        if (added != 1)
            note(QStringLiteral("FAIL: agent stroke added %1").arg(added));
        undo();
        if (m_document.layers().size() != baseLayers)
            note(QStringLiteral("FAIL: agent stroke undo"));
        else
            note(QStringLiteral("agent stroke undo: OK"));
    }

    /* The assistant tool exposes the same editable stroke action. */
    {
        selectSelfTestCutout();
        const int before = m_document.layers().size();
        const QVariantMap tool = executeAssistantTool(
            QStringLiteral("draw_creative_stroke"),
            QStringLiteral("{\"kind\":\"halo\"}"));
        const bool ok = tool.value(QStringLiteral("ok")).toBool();
        note(ok ? QStringLiteral("assistant stroke tool: OK")
                : QStringLiteral("FAIL: assistant stroke tool: %1")
                      .arg(tool.value(QStringLiteral("summary")).toString()));
        if (m_document.layers().size() != before + 1)
            note(QStringLiteral("FAIL: assistant stroke added %1")
                     .arg(m_document.layers().size() - before));
        undo();
        if (m_document.layers().size() != baseLayers)
            note(QStringLiteral("FAIL: assistant stroke undo"));
    }

    /* Post-Keep moment: a kept cutout opens the pad on the two-card chooser. */
    HarvestItem kept;
    kept.cutout = idol;
    kept.sourceFrame.ptsMs = 4200;
    kept.sourceFrame.title = QStringLiteral("Self-test");
    kept.semanticLabel = QStringLiteral("Self-test kept");
    const int keptId = m_document.harvest()->add(kept);
    m_creative->openFromHarvest(keptId);
    note(m_creative->chooserOpen() && m_creative->hasSubject()
             ? QStringLiteral("post-keep chooser: OK")
             : QStringLiteral("FAIL: post-keep chooser did not open"));
    m_creative->selectPatch(QStringLiteral("sticker-forge"));
    note(!m_creative->chooserOpen()
             ? QStringLiteral("chooser selection: OK")
             : QStringLiteral("FAIL: chooser stayed open"));

    /* Opt-in real-video track: decode three real PTS frames through the same
     * edit source used by tracking, then exercise the controller pose path and
     * both tracked recipes. The test track uses a synthetic foreground mask;
     * the acceptance harness covers real SAM3 masks separately. */
    const QString realTrackVideo =
        qEnvironmentVariable("PB_CREATIVE_TRACK_VIDEO");
    if (!realTrackVideo.isEmpty() && QFileInfo::exists(realTrackVideo))
    {
        const QString mrl = QUrl::fromLocalFile(realTrackVideo).toString();
        const QVector<qint64> times{ 53600, 64320, 96480 };
        const DecodedFrame probe =
            m_editSource.request(FrameRef{ mrl, {}, times.first() });
        if (!probe.ok())
        {
            note(QStringLiteral("FAIL: real-track decode at %1 ms")
                     .arg(times.first()));
        }
        else
        {
            QImage mask(probe.image.size(), QImage::Format_ARGB32);
            mask.fill(Qt::transparent);
            QPainter maskPainter(&mask);
            maskPainter.setRenderHint(QPainter::Antialiasing, true);
            maskPainter.setPen(Qt::NoPen);
            maskPainter.setBrush(Qt::white);
            maskPainter.drawEllipse(
                QRectF(mask.width() * 0.18, mask.height() * 0.08,
                       mask.width() * 0.64, mask.height() * 0.84));
            maskPainter.end();

            ObjectTrack track;
            track.id = 9001;
            track.sourceMRL = mrl;
            track.startMs = times.first();
            track.endMs = times.last();
            track.complete = true;
            for (int i = 0; i < times.size(); ++i)
            {
                TrackKeyframe kf;
                kf.frameIndex = i;
                kf.timeMs = times.at(i);
                kf.mask = mask;
                kf.confidence = 0.9f;
                track.keyframes.append(kf);
            }

            const QVector<ObjectTrack> oldTracks = m_document.tracks();
            const int oldSelected = m_selectedTrackId;
            QVector<ObjectTrack> tracks = oldTracks;
            tracks.append(track);
            m_document.setTracks(tracks);
            setSelectedTrackId(track.id);

            QVector<qint64> posePts;
            const QVector<QImage> poses = collectCreativePoses(3, &posePts);
            note(poses.size() == 3 && posePts == times
                     ? QStringLiteral("real-track controller decode: 3 exact PTS")
                     : QStringLiteral("FAIL: real-track controller decode (%1 poses)")
                           .arg(poses.size()));

            const int beforeLineup = m_document.layers().size();
            const QString lineupError = createPastelLineup();
            const int lineupAdded = m_document.layers().size() - beforeLineup;
            note(lineupError.isEmpty() && lineupAdded > 0
                     ? QStringLiteral("real-track pastel lineup: +%1 layers")
                           .arg(lineupAdded)
                     : QStringLiteral("FAIL: real-track pastel lineup %1")
                           .arg(lineupError));
            undo();
            if (m_document.layers().size() != beforeLineup)
                note(QStringLiteral("FAIL: real-track lineup undo"));

            const int beforeEcho = m_document.layers().size();
            const QString echoError = createMotionEcho();
            const int echoAdded = m_document.layers().size() - beforeEcho;
            note(echoError.isEmpty() && echoAdded > 0
                     ? QStringLiteral("real-track motion echo: +%1 layers")
                           .arg(echoAdded)
                     : QStringLiteral("FAIL: real-track motion echo %1")
                           .arg(echoError));
            undo();
            if (m_document.layers().size() != beforeEcho)
                note(QStringLiteral("FAIL: real-track echo undo"));

            m_document.setTracks(oldTracks);
            setSelectedTrackId(oldSelected);
        }
    }

    /* Opt-in LIVE tracking acceptance (PB_CREATIVE_LIVE_TRACK_VIDEO): start a
     * real TrackingService run from a real video frame through the same
     * asynchronous controller path trackObject() uses, wait for completion,
     * verify the persisted ObjectTrack, then run collectCreativePoses(),
     * Pastel Lineup and Motion Echo with one-undo checks. Dormant unless the
     * env var is set. PB_CREATIVE_LIVE_TRACK_SEED_MASK supplies a real SAM3
     * seed mask (e.g. the harness mask for 64320 ms); without it keyframe 0
     * is a clearly labelled synthetic Grayscale8 ellipse. The propagated
     * keyframes are real SAM3 output in both cases. */
    const QString liveTrackVideo =
        qEnvironmentVariable("PB_CREATIVE_LIVE_TRACK_VIDEO");
    if (!liveTrackVideo.isEmpty())
    {
        const auto runLiveTrackAcceptance = [&]() {
            const QFileInfo videoInfo(liveTrackVideo);
            if (!videoInfo.exists())
            {
                note(QStringLiteral("FAIL: live-track video not found: %1")
                         .arg(liveTrackVideo));
                return;
            }
            if (!m_tracking.isConfigured() || !m_tracking.isAvailable())
            {
                /* the env var is an explicit acceptance request: a missing
                 * backend must fail the run, never pass it silently */
                note(QStringLiteral("FAIL: live tracking unavailable (%1)")
                         .arg(m_tracking.unavailableReason()));
                return;
            }
            if (m_tracking.isTracking())
            {
                note(QStringLiteral("FAIL: live track: a tracking run is "
                                    "already active"));
                return;
            }

            const QString mrl =
                QUrl::fromLocalFile(videoInfo.absoluteFilePath()).toString();
            const qint64 requestedSeedPts = 64320;
            const DecodedFrame seed =
                m_editSource.request(FrameRef{ mrl, {}, requestedSeedPts });
            if (!seed.ok())
            {
                note(QStringLiteral("FAIL: live-track seed decode at %1 ms "
                                    "(actual %2 ms)")
                         .arg(requestedSeedPts).arg(seed.ptsMs));
                return;
            }
            /* Seed the tracker at the frame's ACTUAL PTS, not the requested
             * sampling time: the sequential decoder's first propagated frame
             * must be strictly after the seed keyframe. */
            const qint64 seedPts = seed.ptsMs;

            QImage seedMask;
            bool syntheticSeed = true;
            QString seedLabel;
            const QString seedMaskPath =
                qEnvironmentVariable("PB_CREATIVE_LIVE_TRACK_SEED_MASK");
            if (!seedMaskPath.isEmpty())
            {
                const QImage loadedMask =
                    QFileInfo::exists(seedMaskPath) ? QImage(seedMaskPath)
                                                    : QImage();
                if (loadedMask.isNull())
                {
                    note(QStringLiteral("FAIL: live-track seed mask cannot be "
                                        "loaded: %1").arg(seedMaskPath));
                    return;
                }
                QImage normalized =
                    loadedMask.convertToFormat(QImage::Format_Grayscale8);
                if (normalized.size() != seed.image.size())
                {
                    /* The harness mask is a 960x540 segmentation of this
                     * frame while the edit source decodes the native video
                     * size (3840x2160). Accept only a proportional mask and
                     * state the rescale; a crop from another geometry is a
                     * mistake, not a mask. */
                    if (qint64(normalized.width()) * seed.image.height()
                        != qint64(normalized.height()) * seed.image.width())
                    {
                        note(QStringLiteral("FAIL: live-track seed mask "
                                            "geometry %1x%2 does not match "
                                            "the seed frame %3x%4")
                                 .arg(normalized.width()).arg(normalized.height())
                                 .arg(seed.image.width())
                                 .arg(seed.image.height()));
                        return;
                    }
                    const QSize maskSize(normalized.width(),
                                         normalized.height());
                    normalized = normalized.scaled(
                        seed.image.size(), Qt::IgnoreAspectRatio,
                        Qt::FastTransformation);
                    normalized =
                        normalized.convertToFormat(QImage::Format_Grayscale8);
                    seedLabel =
                        QStringLiteral("real mask %1 (%2x%3 scaled to %4x%5)")
                            .arg(seedMaskPath)
                            .arg(maskSize.width()).arg(maskSize.height())
                            .arg(seed.image.width())
                            .arg(seed.image.height());
                }
                else
                {
                    seedLabel =
                        QStringLiteral("real mask %1").arg(seedMaskPath);
                }
                seedMask = normalized;
                syntheticSeed = false;
            }
            if (seedMask.isNull())
            {
                QImage painted(seed.image.size(), QImage::Format_ARGB32);
                painted.fill(Qt::transparent);
                QPainter maskPainter(&painted);
                maskPainter.setRenderHint(QPainter::Antialiasing, true);
                maskPainter.setPen(Qt::NoPen);
                maskPainter.setBrush(Qt::white);
                maskPainter.drawEllipse(
                    QRectF(painted.width() * 0.18, painted.height() * 0.08,
                           painted.width() * 0.64, painted.height() * 0.84));
                maskPainter.end();
                seedMask = painted.convertToFormat(QImage::Format_Grayscale8);
                seedLabel = QStringLiteral(
                    "synthetic Grayscale8 ellipse (keyframe 0 is "
                    "seeded-synthetic; propagated keyframes are real SAM3)");
            }
            if (seedMask.isNull() || seedMask.size() != seed.image.size()
                || seedMask.format() != QImage::Format_Grayscale8)
            {
                note(QStringLiteral("FAIL: live-track seed mask must be "
                                    "Grayscale8 at the seed frame size (%1x%2), "
                                    "got %3x%4")
                         .arg(seed.image.width()).arg(seed.image.height())
                         .arg(seedMask.width()).arg(seedMask.height()));
                return;
            }

            /* a positive prompt point on the mask: the centroid, or the
             * nearest mask pixel when the centroid falls into a hole */
            const auto positivePoint = [](const QImage &mask) {
                const int w = mask.width();
                const int h = mask.height();
                qint64 sx = 0, sy = 0, count = 0;
                for (int y = 0; y < h; ++y)
                {
                    const uchar *line = mask.constScanLine(y);
                    for (int x = 0; x < w; ++x)
                        if (line[x] >= 128)
                        {
                            sx += x;
                            sy += y;
                            ++count;
                        }
                }
                if (count == 0)
                    return QPointF(-1, -1);
                const qreal cx = qreal(sx) / qreal(count);
                const qreal cy = qreal(sy) / qreal(count);
                const int px = qBound(0, int(cx), w - 1);
                const int py = qBound(0, int(cy), h - 1);
                if (mask.constScanLine(py)[px] >= 128)
                    return QPointF(cx, cy);
                qint64 best = -1;
                int bx = px, by = py;
                for (int y = 0; y < h; ++y)
                {
                    const uchar *line = mask.constScanLine(y);
                    for (int x = 0; x < w; ++x)
                    {
                        if (line[x] < 128)
                            continue;
                        const qint64 dx = x - px;
                        const qint64 dy = y - py;
                        const qint64 dist = dx * dx + dy * dy;
                        if (best < 0 || dist < best)
                        {
                            best = dist;
                            bx = x;
                            by = y;
                        }
                    }
                }
                return QPointF(qreal(bx) + 0.5, qreal(by) + 0.5);
            };
            const QPointF livePoint = positivePoint(seedMask);
            if (livePoint.x() < 0)
            {
                note(QStringLiteral("FAIL: live-track seed mask is empty"));
                return;
            }

            note(QStringLiteral("live-track seed: %1").arg(seedLabel));

            const QVector<ObjectTrack> oldTracks = m_document.tracks();
            const int oldSelected = m_selectedTrackId;

            /* pre-populate the pending track exactly like trackObject(), so
             * onTrackKeyframe() keeps the real masks and onTrackFinished()
             * publishes the track into the document */
            m_pendingTrack.clear();
            ObjectTrack pending;
            pending.id = m_trackIdCounter++;
            pending.sourceMRL = mrl;
            pending.startMs = seedPts;
            pending.complete = false;
            m_pendingTrack.append(pending);
            const int liveTrackId = pending.id;

            QEventLoop wait;
            bool finished = false;
            bool finishedComplete = false;
            bool finishedCancelled = false;
            QString finishedError;
            const QMetaObject::Connection finishConn = QObject::connect(
                &m_tracking, &TrackingService::trackFinished, &wait,
                [&](bool complete, bool cancelled, const QString &error) {
                    finished = true;
                    finishedComplete = complete;
                    finishedCancelled = cancelled;
                    finishedError = error;
                    wait.quit();
                });
            const QMetaObject::Connection progressConn = QObject::connect(
                &m_tracking, &TrackingService::progressChanged, &wait,
                [&](int done, int total) {
                    note(QStringLiteral("live-track progress: %1/%2")
                             .arg(done).arg(total));
                });
            QTimer timeout;
            timeout.setSingleShot(true);
            QObject::connect(&timeout, &QTimer::timeout, &wait,
                             &QEventLoop::quit);
            /* hard observation bound for the nested wait (real CPU SAM3 can
             * take minutes per propagated frame); env override for slower
             * machines. This is not a build timeout: the service remains
             * asynchronous and a hit here is reported as a failed run. */
            int waitMs = 30 * 60 * 1000;
            bool waitOverrideOk = false;
            const int waitOverride = qEnvironmentVariableIntValue(
                "PB_CREATIVE_LIVE_TRACK_TIMEOUT_MS", &waitOverrideOk);
            if (waitOverrideOk && waitOverride > 0)
                waitMs = waitOverride;
            timeout.start(waitMs);
            QElapsedTimer clock;
            clock.start();

            m_tracking.startTrack(&m_sequentialSource, mrl, seed.image,
                                  seedMask, { livePoint },
                                  TrackingService::TRACK_FORWARD, 6, seedPts);
            emit trackingStateChanged();
            note(QStringLiteral("live-track: started id=%1 seed=%2 ms")
                     .arg(liveTrackId).arg(seedPts));
            if (!finished)
                wait.exec();
            timeout.stop();
            QObject::disconnect(finishConn);
            QObject::disconnect(progressConn);

            const auto restoreDocument = [&]() {
                m_pendingTrack.clear();
                /* the discarded track's sticker frames are no longer
                 * addressable from the document; do not leave 4K frames in
                 * the cache for a track that does not exist */
                m_stickerFrames.clear();
                m_document.setTracks(oldTracks);
                setSelectedTrackId(oldSelected);
            };

            if (!finished)
            {
                note(QStringLiteral("FAIL: live track timed out after %1 s "
                                    "(progress %2/%3, status '%4')")
                         .arg(waitMs / 1000)
                         .arg(m_tracking.progressDone())
                         .arg(m_tracking.progressTotal())
                         .arg(m_tracking.statusText()));
                m_tracking.cancel();
                restoreDocument();
                return;
            }

            const double elapsedS = clock.elapsed() / 1000.0;
            if (!finishedComplete || finishedCancelled
                || !finishedError.isEmpty())
            {
                note(QStringLiteral("FAIL: live track finished complete=%1 "
                                    "cancelled=%2 error='%3' (%4 s)")
                         .arg(finishedComplete ? 1 : 0)
                         .arg(finishedCancelled ? 1 : 0)
                         .arg(finishedError)
                         .arg(elapsedS, 0, 'f', 1));
                restoreDocument();
                return;
            }

            ObjectTrack liveTrack;
            bool found = false;
            for (const ObjectTrack &candidate : m_document.tracks())
                if (candidate.id == liveTrackId)
                {
                    liveTrack = candidate;
                    found = true;
                    break;
                }
            if (!found)
            {
                note(QStringLiteral("FAIL: live track finished but track id %1 "
                                    "is missing from the document")
                         .arg(liveTrackId));
                restoreDocument();
                return;
            }

            const int keyframeCount = liveTrack.keyframes.size();
            bool masks8Bit = true;
            bool ptsOrdered = keyframeCount > 0
                              && liveTrack.keyframes.first().timeMs == seedPts;
            int changedMasks = 0;
            for (int i = 0; i < keyframeCount; ++i)
            {
                const TrackKeyframe &kf = liveTrack.keyframes.at(i);
                if (kf.mask.isNull() || kf.mask.depth() != 8
                    || kf.mask.size() != seed.image.size())
                    masks8Bit = false;
                if (i > 0 && kf.timeMs <= liveTrack.keyframes.at(i - 1).timeMs)
                    ptsOrdered = false;
                if (i > 0 && !kf.mask.isNull() && kf.mask != seedMask)
                    ++changedMasks;
            }

            if (!liveTrack.complete)
                note(QStringLiteral("FAIL: live track is not complete"));
            if (keyframeCount < 2)
                note(QStringLiteral("FAIL: live track has %1 keyframe(s) "
                                    "(want >= 2)").arg(keyframeCount));
            if (!masks8Bit)
                note(QStringLiteral("FAIL: live track masks are not 8-bit at "
                                    "the seed frame size"));
            if (!ptsOrdered)
                note(QStringLiteral("FAIL: live track PTS are not monotonic "
                                    "from %1 ms").arg(seedPts));
            if (changedMasks == 0)
                note(QStringLiteral("FAIL: no propagated mask differs from the "
                                    "seed mask"));
            note(QStringLiteral("live track: id=%1 complete keyframes=%2 "
                                "pts %3..%4 ms (%5 s)")
                     .arg(liveTrack.id)
                     .arg(keyframeCount)
                     .arg(keyframeCount > 0 ? liveTrack.keyframes.first().timeMs
                                            : -1)
                     .arg(keyframeCount > 0 ? liveTrack.keyframes.last().timeMs
                                            : -1)
                     .arg(elapsedS, 0, 'f', 1));
            note(QStringLiteral("live track: seed frame %1x%2; %3 propagated "
                                "frame(s) differ from the seed")
                     .arg(seed.image.width()).arg(seed.image.height())
                     .arg(changedMasks));

            setSelectedTrackId(liveTrackId);

            QVector<qint64> posePts;
            const QVector<QImage> poses = collectCreativePoses(6, &posePts);
            bool posesReal =
                poses.size() >= 2 && poses.size() == posePts.size();
            for (int i = 0; i < posePts.size(); ++i)
            {
                if (posePts.at(i) < 0)
                    posesReal = false;
                if (i > 0 && posePts.at(i) <= posePts.at(i - 1))
                    posesReal = false;
            }
            if (posesReal)
                note(syntheticSeed
                         ? QStringLiteral("live-track poses: %1 ordered PTS "
                                          "(%2..%3 ms); the keyframe-0 cutout "
                                          "uses the seeded-synthetic mask")
                               .arg(poses.size())
                               .arg(posePts.first())
                               .arg(posePts.last())
                         : QStringLiteral("live-track poses: %1 real ordered "
                                          "PTS (%2..%3 ms)")
                               .arg(poses.size())
                               .arg(posePts.first())
                               .arg(posePts.last()));
            else
                note(QStringLiteral("FAIL: live-track poses: %1 pose(s), %2 "
                                    "PTS (still fallback or missing)")
                         .arg(poses.size()).arg(posePts.size()));

            /* tracked recipes: real layers, one undo restores the count */
            const int beforeLineup = m_document.layers().size();
            const QString lineupError = createPastelLineup();
            const int lineupAdded = m_document.layers().size() - beforeLineup;
            note(lineupError.isEmpty() && lineupAdded > 0
                     ? QStringLiteral("live-track pastel lineup: +%1 layers")
                           .arg(lineupAdded)
                     : QStringLiteral("FAIL: live-track pastel lineup: %1")
                           .arg(lineupError));
            if (lineupAdded > 0)
            {
                undo();
                if (m_document.layers().size() != beforeLineup)
                    note(QStringLiteral("FAIL: live-track lineup undo "
                                        "(%1 != %2)")
                             .arg(m_document.layers().size())
                             .arg(beforeLineup));
                else
                    note(QStringLiteral("live-track lineup undo: OK"));
            }

            const int beforeEcho = m_document.layers().size();
            const QString echoError = createMotionEcho();
            const int echoAdded = m_document.layers().size() - beforeEcho;
            note(echoError.isEmpty() && echoAdded > 0
                     ? QStringLiteral("live-track motion echo: +%1 layers")
                           .arg(echoAdded)
                     : QStringLiteral("FAIL: live-track motion echo: %1")
                           .arg(echoError));
            if (echoAdded > 0)
            {
                undo();
                if (m_document.layers().size() != beforeEcho)
                    note(QStringLiteral("FAIL: live-track echo undo "
                                        "(%1 != %2)")
                             .arg(m_document.layers().size()).arg(beforeEcho));
                else
                    note(QStringLiteral("live-track echo undo: OK"));
            }
        };
        runLiveTrackAcceptance();
    }

    /* leave a composed stage open for the screenshot */
    for (int i = 0; i < m_document.layers().size(); ++i)
    {
        const CanvasLayer *l = m_document.layerAt(i);
        if (l && l->name == QLatin1String("Self-test cutout"))
        {
            m_document.setCurrentLayerIndex(i);
            emit documentChanged();
            break;
        }
    }
    m_creative->openForSelection();
    m_creative->selectPatch(QStringLiteral("neon-idol"));
    m_creative->movePad(0.74, 0.58, 0.42, false);
    m_creative->setHold(false);
    const QImage finalPreview = m_creative->previewImage();
    if (!finalPreview.isNull())
        finalPreview.save(QStringLiteral("%1/selftest-final.png").arg(dir));

    note(failed() ? QStringLiteral("RESULT: FAIL") : QStringLiteral("RESULT: PASS"));
    const QString reportText = report.join(QLatin1Char('\n'));
    QFile reportFile(QStringLiteral("%1/selftest-report.txt").arg(dir));
    if (reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        reportFile.write(reportText.toUtf8());
        reportFile.write("\n");
        reportFile.close();
    }
    return reportText;
}


/* ----------------------------------------------------- creative Play Pad */

namespace {

bool rasterQualifiesAsCutout(const QImage &raster)
{
    if (raster.isNull())
        return false;
    const QImage img = raster.convertToFormat(QImage::Format_ARGB32);
    const qint64 total = qint64(img.width()) * qint64(img.height());
    if (total <= 0)
        return false;
    qint64 opaque = 0;
    qint64 clear = 0;
    for (int y = 0; y < img.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
        {
            const int a = qAlpha(line[x]);
            if (a > 16)
                ++opaque;
            if (a < 250)
                ++clear;
        }
    }
    /* A real cutout has both real pixels and transparency. An empty layer is
     * fully transparent; a background is fully opaque; neither is a subject. */
    return clear > 0 && opaque > total / 500; /* at least 0.2% real pixels */
}

QRectF fitCreativeSubject(const QSize &src, const QSize &canvas,
                          qreal maxHeight, qreal maxWidth)
{
    if (!src.isValid() || src.isEmpty() || !canvas.isValid() || canvas.isEmpty())
        return {};
    const qreal s = qMin(double(canvas.width()) * maxWidth / double(src.width()),
                         double(canvas.height()) * maxHeight / double(src.height()));
    const QSizeF box(src.width() * s, src.height() * s);
    return QRectF((canvas.width() - box.width()) / 2.0,
                  (canvas.height() - box.height()) / 2.0,
                  box.width(), box.height());
}

} /* namespace */

QSize PaintbrushController::creativePadCanvasSize() const
{
    const QSize size = m_document.canvasSize();
    if (size.isValid() && !size.isEmpty())
        return size;
    return QSize(1440, 1800);
}

QString PaintbrushController::expandCanvas(int percent)
{
    percent = qBound(2, percent, 200);
    const QSize oldSize = m_document.canvasSize();
    if (!oldSize.isValid() || oldSize.isEmpty())
        return QStringLiteral("Open a canvas first.");
    const int pad = qMax(8, int(std::lround(
        qMax(oldSize.width(), oldSize.height()) * percent / 100.0)));
    const QSize newSize(oldSize.width() + pad * 2, oldSize.height() + pad * 2);
    if (newSize.width() > 16000 || newSize.height() > 16000)
        return QStringLiteral("That expansion is too large.");
    m_undoStack.pushCommand(m_document.makeExpandCanvasCommand(pad));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Canvas expanded by %1 px on every side — one undo.")
                  .arg(pad));
    triggerCompanionEvent(QStringLiteral("expand-canvas"));
    return {};
}

QImage PaintbrushController::creativeSubjectFromLayer(const CanvasLayer &layer,
                                                      QRectF *canvasRect,
                                                      QString *label) const
{
    if (layer.raster.isNull())
        return {};
    QImage cutout = creative::maskToCutout(layer.raster,
                                           creative::layerMask(layer));
    if (cutout.isNull())
        return {};
    QRectF rect = layer.transform().mapRect(layer.raster.rect());
    if (!qFuzzyIsNull(layer.rotation))
    {
        /* bake the rotation into canvas space so the stage sees exactly
         * the silhouette the user has on the canvas */
        const QSize canvas = creativePadCanvasSize();
        QImage baked(canvas, QImage::Format_ARGB32);
        baked.fill(Qt::transparent);
        {
            QPainter p(&baked);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.setTransform(layer.transform());
            p.drawImage(QPointF(0, 0), cutout);
        }
        QPoint offset;
        cutout = creative::trimTransparent(baked, &offset);
        if (cutout.isNull())
            return {};
        rect = QRectF(offset, cutout.size());
    }
    if (canvasRect)
        *canvasRect = rect;
    if (label)
        *label = layer.name;
    return cutout;
}

QImage PaintbrushController::creativePadSubject(int *sourceLayerId,
                                                QRectF *canvasRect,
                                                QString *label) const
{
    if (sourceLayerId)
        *sourceLayerId = -1;
    if (canvasRect)
        *canvasRect = QRectF();
    if (label)
        label->clear();

    const QList<CanvasLayer> layers = m_document.layers();
    auto layerRole = [](const CanvasLayer &l) {
        return l.metadata.value(QStringLiteral("creative.role")).toString();
    };
    auto isFinishRole = [](const QString &role) {
        return role == QLatin1String("backdrop")
               || role == QLatin1String("finish")
               || role == QLatin1String("rim")
               || role == QLatin1String("accent")
               || role == QLatin1String("echo")
               || role == QLatin1String("texture");
    };
    const CanvasLayer *chosen = m_document.layerAt(m_document.currentLayerIndex());
    if (!chosen || chosen->raster.isNull()
        || !rasterQualifiesAsCutout(chosen->raster)
        || isCreativeResult(*chosen)
        || isFinishRole(layerRole(*chosen)))
        chosen = nullptr;
    if (!chosen)
    {
        /* after a commit the top layer is a finish; prefer a real subject
         * layer over a scanline/glow layer */
        for (int i = layers.size() - 1; i >= 0; --i)
        {
            const CanvasLayer &l = layers.at(i);
            if (!l.visible || l.raster.isNull() || isCreativeResult(l))
                continue;
            if (layerRole(l) != QLatin1String("subject"))
                continue;
            if (!rasterQualifiesAsCutout(l.raster))
                continue;
            chosen = &layers.at(i);
            break;
        }
    }
    if (!chosen)
    {
        for (int i = layers.size() - 1; i >= 0; --i)
        {
            const CanvasLayer &l = layers.at(i);
            if (l.type == LAYER_TEXT || l.type == LAYER_VECTOR)
                continue;
            if (!l.visible || l.raster.isNull() || isCreativeResult(l))
                continue;
            if (isFinishRole(layerRole(l)))
                continue;
            if (!rasterQualifiesAsCutout(l.raster))
                continue;
            chosen = &layers.at(i);
            break;
        }
    }
    if (!chosen)
    {
        /* A committed look hides its source layer. On the next apply, that
         * hidden source is the right subject; the visible effect layers were
         * already filtered out above. */
        for (int i = layers.size() - 1; i >= 0; --i)
        {
            const CanvasLayer &l = layers.at(i);
            if (l.type == LAYER_TEXT || l.type == LAYER_VECTOR)
                continue;
            if (l.raster.isNull() || isCreativeResult(l))
                continue;
            if (isFinishRole(layerRole(l)))
                continue;
            if (!rasterQualifiesAsCutout(l.raster))
                continue;
            chosen = &layers.at(i);
            break;
        }
    }

    if (chosen)
    {
        const QImage cutout = creativeSubjectFromLayer(*chosen, canvasRect, label);
        if (!cutout.isNull() && sourceLayerId)
            *sourceLayerId = chosen->id;
        return cutout;
    }

    /* no cutout layer: the last kept moment is a fine subject */
    const QVector<HarvestItem> &items = m_document.harvest()->items();
    if (!items.isEmpty())
    {
        const HarvestItem &item = items.last();
        if (!item.cutout.isNull())
        {
            const QSize canvas = creativePadCanvasSize();
            if (canvasRect)
                *canvasRect = fitCreativeSubject(item.cutout.size(), canvas,
                                                 0.80, 0.80);
            if (label)
                *label = QStringLiteral("Kept %1")
                             .arg(harvestTimeText(item.sourceFrame.ptsMs));
            return item.cutout;
        }
    }
    return {};
}

QImage PaintbrushController::creativePadHarvest(int harvestId,
                                                QRectF *canvasRect,
                                                QString *label) const
{
    const HarvestItem *item = m_document.harvest()->itemById(harvestId);
    if (!item || item->cutout.isNull())
        return {};
    const QSize canvas = creativePadCanvasSize();
    if (canvasRect)
        *canvasRect = fitCreativeSubject(item->cutout.size(), canvas, 0.78, 0.78);
    if (label)
        *label = item->semanticLabel.isEmpty()
                     ? QStringLiteral("Kept %1")
                           .arg(harvestTimeText(item->sourceFrame.ptsMs))
                     : item->semanticLabel;
    return item->cutout;
}

bool PaintbrushController::creativeLookInfo(int layerIndex, QVariantMap *info,
                                            QImage *cutout, QRectF *canvasRect,
                                            QString *label) const
{
    const CanvasLayer *layer = m_document.layerAt(layerIndex);
    if (!layer || !info || !cutout)
        return false;
    const QString patch =
        layer->metadata.value(QStringLiteral("creative.patch")).toString();
    if (patch.isEmpty())
        return false;
    const int sourceLayerId =
        layer->metadata.value(QStringLiteral("creative.sourceLayerId")).toInt();
    const int sourceHarvestId =
        layer->metadata.value(QStringLiteral("creative.sourceHarvestId")).toInt();

    QImage source;
    QRectF rect;
    QString name;
    if (sourceLayerId > 0)
    {
        const CanvasLayer *src = m_document.layerById(sourceLayerId);
        if (src)
            source = creativeSubjectFromLayer(*src, &rect, &name);
    }
    if (source.isNull() && sourceHarvestId > 0)
        source = creativePadHarvest(sourceHarvestId, &rect, &name);
    if (source.isNull())
    {
        /* Source removed: the committed subject piece is still a valid
         * starting point (already graded, but editable). */
        source = creativeSubjectFromLayer(*layer, &rect, &name);
    }
    if (source.isNull())
        return false;

    info->clear();
    (*info)[QStringLiteral("patch")] = patch;
    (*info)[QStringLiteral("patchName")] =
        layer->metadata.value(QStringLiteral("creative.patchName"));
    (*info)[QStringLiteral("seed")] =
        layer->metadata.value(QStringLiteral("creative.seed"));
    (*info)[QStringLiteral("params")] =
        layer->metadata.value(QStringLiteral("creative.params"));
    (*info)[QStringLiteral("group")] =
        layer->metadata.value(QStringLiteral("creative.group"));
    (*info)[QStringLiteral("sourceLayerId")] = sourceLayerId;
    (*info)[QStringLiteral("sourceHarvestId")] = sourceHarvestId;
    *cutout = source;
    if (canvasRect)
        *canvasRect = rect;
    if (label)
        *label = name;
    return true;
}

bool PaintbrushController::creativeLivingSource(int sourceLayerId, int harvestId,
                                                motion::PuppetMotionClip *clip,
                                                int *assetId,
                                                float *defaultAmplitude) const
{
    const auto assignAsset = [&](const motion::PuppetAsset *asset) {
        if (!asset || clip == nullptr)
            return false;
        *clip = asset->motion;
        if (assetId)
            *assetId = asset->id;
        if (defaultAmplitude)
            *defaultAmplitude = asset->defaultAmplitude;
        return clip->keys.size() >= 2;
    };

    if (sourceLayerId > 0)
    {
        const CanvasLayer *layer = m_document.layerById(sourceLayerId);
        if (layer)
        {
            if (layer->puppetAssetId > 0
                && assignAsset(m_document.puppets()->byId(layer->puppetAssetId)))
            {
                if (defaultAmplitude && layer->puppetEffect.amplitude > 0.01f)
                    *defaultAmplitude = layer->puppetEffect.amplitude;
                return true;
            }
            if (layer->hasDerivedLiving())
            {
                const motion::PuppetMotionClip stored =
                    StageRuntime::storedClip(*layer);
                if (stored.keys.size() >= 2)
                {
                    if (clip)
                        *clip = stored;
                    if (assetId)
                        *assetId = -1;
                    if (defaultAmplitude)
                        *defaultAmplitude =
                            layer->puppetEffect.amplitude > 0.01f
                                ? layer->puppetEffect.amplitude
                                : 0.65f;
                    return true;
                }
            }
            if (layer->harvestId > 0
                && assignAsset(m_document.puppets()->assetByHarvest(
                    layer->harvestId)))
                return true;
        }
    }
    if (harvestId > 0
        && assignAsset(m_document.puppets()->assetByHarvest(harvestId)))
        return true;
    return false;
}

QString PaintbrushController::commitCreativeLayers(const QList<CanvasLayer> &layers,
                                                   int hideLayerId,
                                                   const QString &desc,
                                                   const QString &replaceGroup)
{
    if (layers.isEmpty())
        return QStringLiteral("Nothing to commit.");

    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = desc;

    /* Resuming a look replaces its previous group instead of stacking a
     * second copy. Delete top-down; composite revert re-inserts bottom-up. */
    if (!replaceGroup.isEmpty())
    {
        for (int i = m_document.layers().size() - 1; i >= 0; --i)
        {
            CanvasLayer *old = m_document.layerAt(i);
            if (!old
                || old->metadata.value(QStringLiteral("creative.group"))
                       .toString() != replaceGroup)
                continue;
            DeleteLayerCommand del;
            del.index = i;
            del.layer = *old;
            del.prevCurrentIndex = m_document.currentLayerIndex();
            del.desc = desc;
            del.apply(m_document);
            composite->children.push_back(
                std::make_unique<DeleteLayerCommand>(del));
        }
    }

    QList<CanvasLayer> all = m_document.layers();
    const int firstNew = all.size();
    for (const CanvasLayer &layer : layers)
    {
        CanvasLayer copy = layer;
        copy.id = 0;
        all.append(copy);
    }
    m_document.setLayers(all);

    if (hideLayerId > 0)
    {
        const int hideIndex = m_document.indexOfLayerId(hideLayerId);
        if (hideIndex >= 0)
        {
            CanvasLayer *src = m_document.layerAt(hideIndex);
            if (src && src->visible)
            {
                SetVisibilityCommand vis;
                vis.layerIndex = hideIndex;
                vis.before = true;
                vis.after = false;
                vis.desc = desc;
                src->visible = false;
                composite->children.push_back(
                    std::make_unique<SetVisibilityCommand>(vis));
            }
        }
    }

    const int end = m_document.layers().size();
    for (int i = firstNew; i < end; ++i)
    {
        const CanvasLayer *added = m_document.layerAt(i);
        if (!added)
            continue;
        InsertLayerCommand cmd;
        cmd.desc = desc;
        cmd.index = i;
        cmd.prevCurrentIndex = qMax(0, firstNew - 1);
        cmd.layer = *added;
        composite->children.push_back(std::make_unique<InsertLayerCommand>(cmd));
    }
    if (composite->children.empty())
        return QStringLiteral("The patch produced no layers.");

    m_document.setCurrentLayerIndex(qBound(0, end - 1,
                                           m_document.layers().size() - 1));
    m_undoStack.pushCommand(std::move(composite));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Committed as editable layers — one undo."));
    triggerCompanionEvent(QStringLiteral("creative-made-real"));
    return QString();
}

void PaintbrushController::openCreativePad(const QString &patchId)
{
    if (!m_creative)
        return;
    m_creative->openForSelection();
    if (!patchId.isEmpty())
        m_creative->selectPatch(patchId);
}

QString PaintbrushController::makeRiggedStage()
{
    if (!m_creative)
        return QStringLiteral("The Creative Pad is not available.");
    if (!m_creative->hasSubject())
        m_creative->openForSelection();
    if (!m_creative->hasSubject())
        return QStringLiteral("Pick a cutout first — or Keep a moment.");
    return m_creative->makeRiggedStage();
}

QString PaintbrushController::applyCreativePatch(const QString &patchId)
{
    if (!m_creative)
        return QStringLiteral("The Creative Pad is not available.");
    bool known = false;
    const QVariantList patches = m_creative->patchList();
    for (const QVariant &entry : patches)
    {
        if (entry.toMap().value(QStringLiteral("id")).toString() == patchId)
        {
            known = true;
            break;
        }
    }
    if (!known)
        return QStringLiteral("Unknown look '%1'.").arg(patchId);

    m_creative->openForSelection();
    if (!m_creative->hasSubject())
        return QStringLiteral(
            "Select a cutout or image layer first — the look is built from "
            "its own pixels.");
    m_creative->selectPatch(patchId);
    const QString error = m_creative->commit();
    if (!error.isEmpty())
        return error;
    setStatus(QStringLiteral(
        "Look applied — every piece is an editable layer."));
    return {};
}

void PaintbrushController::addCreativeSparkle(const QColor &color,
                                              const QPointF &canvasPos,
                                              double sizePx)
{
    const double size = qBound(8.0, sizePx, 2048.0);
    const double half = size / 2.0;
    QPainterPath path;
    path.moveTo(half, 0.0);
    path.quadTo(half * 0.56, half * 0.56, size, half);
    path.quadTo(half * 0.56, half * 1.44, half, size);
    path.quadTo(half * 1.44, half * 1.44, 0.0, half);
    path.quadTo(half * 1.44, half * 0.56, half, 0.0);
    path.closeSubpath();

    QImage preview(QSize(int(std::ceil(size)), int(std::ceil(size))),
                   QImage::Format_ARGB32);
    preview.fill(Qt::transparent);
    {
        QPainter p(&preview);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color.isValid() ? color : QColor(255, 226, 120));
        pen.setWidthF(qMax<qreal>(1.0, size * 0.035));
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
    insertVectorLayer(path, canvasPos - QPointF(half, half), preview,
                      color.isValid() ? color : QColor(255, 226, 120),
                      qMax<qreal>(1.0, size * 0.035),
                      QStringLiteral("Sparkle"));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::addCreativeRibbon(const QColor &color,
                                             const QRectF &subjectRect,
                                             int variant, bool splat)
{
    if (subjectRect.isEmpty())
        return;
    const qreal w = qMax<qreal>(48.0, subjectRect.width() * 0.94);
    const qreal h = qMax<qreal>(48.0, subjectRect.height() * 1.02);
    const QRectF box(subjectRect.x() - w * 0.03,
                     subjectRect.y() - h * 0.02, w, h);
    QPainterPath path;
    qreal width = qMax<qreal>(1.5, h * 0.006);

    if (variant == 1)
    {
        /* halo: an offset ring behind the head */
        const QRectF ring(w * 0.16, h * 0.05, w * 0.68, h * 0.26);
        path.addEllipse(ring);
        path.addEllipse(ring.adjusted(w * 0.02, h * 0.012,
                                      -w * 0.02, -h * 0.012));
        width = qMax<qreal>(1.5, h * 0.005);
    }
    else if (splat)
    {
        /* a torn ink burst at the shoulder */
        const QPointF c(w * 0.72, h * 0.24);
        const qreal R = w * 0.16;
        for (int i = 0; i < 22; ++i)
        {
            const qreal a = kTau * i / 22.0;
            const qreal r = (i & 1) ? R * 0.42 : R;
            const QPointF p(c.x() + std::cos(a) * r,
                            c.y() + std::sin(a) * r * 0.82);
            if (i == 0)
                path.moveTo(p);
            else
                path.lineTo(p);
        }
        path.closeSubpath();
        width = qMax<qreal>(1.5, h * 0.0045);
    }
    else
    {
        /* flowing ribbon: a deliberate line alongside the silhouette */
        path.moveTo(w * 0.04, h * 0.86);
        path.cubicTo(w * -0.02, h * 0.60, w * 0.16, h * 0.46,
                     w * 0.31, h * 0.50);
        path.cubicTo(w * 0.47, h * 0.54, w * 0.36, h * 0.20,
                     w * 0.62, h * 0.15);
        path.cubicTo(w * 0.82, h * 0.11, w * 0.96, h * 0.30,
                     w * 0.97, h * 0.55);
        width = qMax<qreal>(1.5, h * 0.0065);
    }

    QImage preview(QSize(qMax(1, int(std::ceil(w))), qMax(1, int(std::ceil(h)))),
                   QImage::Format_ARGB32);
    preview.fill(Qt::transparent);
    {
        QPainter p(&preview);
        p.setRenderHint(QPainter::Antialiasing, true);
        QColor glow = color.isValid() ? color : QColor(255, 226, 120);
        glow.setAlpha(70);
        QPen glowPen(glow);
        glowPen.setWidthF(width * 3.2);
        glowPen.setCapStyle(Qt::RoundCap);
        glowPen.setJoinStyle(Qt::RoundJoin);
        p.setPen(glowPen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        QPen pen(color.isValid() ? color : QColor(255, 226, 120));
        pen.setWidthF(width);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawPath(path);
    }
    const QString name = splat ? QStringLiteral("Ink burst")
                               : (variant == 1 ? QStringLiteral("Halo")
                                                : QStringLiteral("Ribbon"));
    insertVectorLayer(path, box.topLeft(), preview,
                      color.isValid() ? color : QColor(255, 226, 120),
                      width, name);
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("%1 added — it is an editable vector layer.")
                  .arg(name));
    triggerCompanionEvent(QStringLiteral("creative-stroke"));
}

void PaintbrushController::addCreativeStrokeAccent(const QString &kind)
{
    int sourceLayerId = -1;
    QRectF rect;
    QString label;
    const QImage cutout = creativePadSubject(&sourceLayerId, &rect, &label);
    if (cutout.isNull() || rect.isEmpty())
    {
        setStatus(QStringLiteral("Select a cutout first — a stroke needs a pose."));
        return;
    }
    QColor color(255, 226, 120);
    if (m_creative)
    {
        const QColor accent(
            m_creative->activePatch().value(QStringLiteral("accent")).toString());
        if (accent.isValid())
            color = accent;
    }
    if (kind == QLatin1String("calligraphy"))
    {
        addCreativeCalligraphy(QStringLiteral("ink"));
        return;
    }
    const bool splat = kind == QLatin1String("splat");
    const int variant = kind == QLatin1String("halo") ? 1 : 0;
    if (kind == QLatin1String("ink"))
        color = QColor(20, 18, 16);
    addCreativeRibbon(color, rect, variant, splat);
}

void PaintbrushController::addCreativeCalligraphy(const QString &mode)
{
    int sourceLayerId = -1;
    QRectF rect;
    QString label;
    const QImage cutout = creativePadSubject(&sourceLayerId, &rect, &label);
    if (cutout.isNull() || rect.isEmpty())
    {
        setStatus(QStringLiteral("Select a cutout first — calligraphy needs a pose."));
        return;
    }
    const QImage alpha = cutout.convertToFormat(QImage::Format_ARGB32);
    const int w = alpha.width();
    const int h = alpha.height();
    if (w < 8 || h < 8)
        return;
    QVector<QPointF> left;
    QVector<QPointF> right;
    const int step = qMax(1, h / 160);
    for (int y = 0; y < h; y += step)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(alpha.constScanLine(y));
        int first = -1;
        int last = -1;
        for (int x = 0; x < w; ++x)
        {
            if (qAlpha(line[x]) <= 40)
                continue;
            if (first < 0)
                first = x;
            last = x;
        }
        if (first < 0 || last <= first)
            continue;
        const qreal sx = rect.width() / double(w);
        const qreal sy = rect.height() / double(h);
        left.append(QPointF(rect.x() + first * sx, rect.y() + y * sy));
        right.append(QPointF(rect.x() + last * sx, rect.y() + y * sy));
    }
    if (left.size() < 3)
    {
        setStatus(QStringLiteral("That cutout is too thin to trace."));
        return;
    }
    QPainterPath path;
    auto appendSmooth = [&](const QVector<QPointF> &points) {
        if (points.size() < 2)
            return;
        path.moveTo(points.first());
        for (int i = 1; i + 1 < points.size(); ++i)
            path.quadTo(points.at(i),
                        (points.at(i) + points.at(i + 1)) * 0.5);
        path.lineTo(points.last());
    };
    appendSmooth(left);
    appendSmooth(right);

    QColor color(20, 18, 16);
    if (mode != QLatin1String("ink") && m_creative)
    {
        const QColor accent(
            m_creative->activePatch().value(QStringLiteral("accent")).toString());
        if (accent.isValid())
            color = accent;
    }
    const qreal width = qMax<qreal>(1.5, rect.height() * 0.006);
    const QSize canvas = creativePadCanvasSize();
    QImage preview(canvas, QImage::Format_ARGB32);
    preview.fill(Qt::transparent);
    {
        QPainter p(&preview);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color);
        pen.setWidthF(width);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
    insertVectorLayer(path, QPointF(0, 0), preview, color, width,
                      QStringLiteral("Calligraphy"));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Calligraphy contour added — edit or undo it like "
                             "any vector layer."));
    triggerCompanionEvent(QStringLiteral("creative-calligraphy"));
}

void PaintbrushController::resumeCreativeLook(int layerIndex)
{
    if (!m_creative)
        return;
    if (layerIndex < 0)
    {
        layerIndex = m_document.currentLayerIndex();
        const CanvasLayer *current = m_document.layerAt(layerIndex);
        if (!current
            || current->metadata.value(QStringLiteral("creative.patch"))
                   .toString().isEmpty())
        {
            for (int i = m_document.layers().size() - 1; i >= 0; --i)
            {
                const CanvasLayer *l = m_document.layerAt(i);
                if (l && !l->metadata.value(QStringLiteral("creative.patch"))
                              .toString().isEmpty())
                {
                    layerIndex = i;
                    break;
                }
            }
        }
    }
    if (!m_creative->restoreFromCommittedLayer(layerIndex))
        setStatus(QStringLiteral("Select a creative layer to resume its look."));
}

QVariantList PaintbrushController::creativeLookHistory() const
{
    QMap<QString, QVariantMap> groups;
    const QList<CanvasLayer> layers = m_document.layers();
    for (int i = 0; i < layers.size(); ++i)
    {
        const CanvasLayer &l = layers.at(i);
        const QString group =
            l.metadata.value(QStringLiteral("creative.group")).toString();
        if (group.isEmpty())
            continue;
        auto it = groups.find(group);
        if (it == groups.end())
        {
            QVariantMap entry;
            entry[QStringLiteral("group")] = group;
            entry[QStringLiteral("patchName")] =
                l.metadata.value(QStringLiteral("creative.patchName")).toString();
            const QString custom =
                l.metadata.value(QStringLiteral("creative.label")).toString();
            entry[QStringLiteral("label")] =
                custom.isEmpty() ? entry.value(QStringLiteral("patchName"))
                                 : custom;
            entry[QStringLiteral("layerIndex")] = i;
            const qint64 stamp = group.section(QLatin1Char('-'), -1).toLongLong();
            entry[QStringLiteral("ageMs")] =
                stamp > 0 ? QDateTime::currentMSecsSinceEpoch() - stamp : 0;
            groups.insert(group, entry);
        }
        else
        {
            it->insert(QStringLiteral("layerIndex"), i);
        }
    }
    QVariantList history;
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it)
        history.append(it.value());
    std::sort(history.begin(), history.end(),
              [](const QVariant &a, const QVariant &b) {
                  return a.toMap().value(QStringLiteral("ageMs")).toLongLong()
                         < b.toMap().value(QStringLiteral("ageMs")).toLongLong();
              });
    return history;
}

QString PaintbrushController::renameCreativeGroup(const QString &group,
                                                  const QString &name)
{
    if (group.isEmpty())
        return QStringLiteral("No look selected.");
    const QString label = name.trimmed();
    if (label.isEmpty())
        return QStringLiteral("A look needs a name.");
    QList<CanvasLayer> layers = m_document.layers();
    int changed = 0;
    for (CanvasLayer &l : layers)
    {
        if (l.metadata.value(QStringLiteral("creative.group")).toString() != group)
            continue;
        l.metadata[QStringLiteral("creative.label")] = label;
        ++changed;
    }
    if (changed == 0)
        return QStringLiteral("That look is no longer in the document.");
    m_document.setLayers(layers);
    m_document.setDirty(true);
    emit documentChanged();
    if (m_creative)
        m_creative->refreshHistory();
    setStatus(QStringLiteral("Look renamed to \"%1\".").arg(label));
    return {};
}

QString PaintbrushController::deleteCreativeGroup(const QString &group)
{
    if (group.isEmpty())
        return QStringLiteral("No look selected.");
    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = QStringLiteral("Delete look");
    int sourceLayerId = -1;
    int removed = 0;
    for (int i = m_document.layers().size() - 1; i >= 0; --i)
    {
        const CanvasLayer *l = m_document.layerAt(i);
        if (!l
            || l->metadata.value(QStringLiteral("creative.group")).toString()
                   != group)
            continue;
        if (sourceLayerId < 0)
            sourceLayerId =
                l->metadata.value(QStringLiteral("creative.sourceLayerId")).toInt();
        DeleteLayerCommand del;
        del.index = i;
        del.layer = *l;
        del.prevCurrentIndex = m_document.currentLayerIndex();
        del.desc = composite->desc;
        del.apply(m_document);
        composite->children.push_back(
            std::make_unique<DeleteLayerCommand>(del));
        ++removed;
    }
    if (removed == 0)
        return QStringLiteral("That look is no longer in the document.");

    if (sourceLayerId > 0)
    {
        const int sourceIndex = m_document.indexOfLayerId(sourceLayerId);
        if (sourceIndex >= 0)
        {
            CanvasLayer *source = m_document.layerAt(sourceIndex);
            if (source && !source->visible)
            {
                SetVisibilityCommand vis;
                vis.layerIndex = sourceIndex;
                vis.before = false;
                vis.after = true;
                vis.desc = composite->desc;
                source->visible = true;
                composite->children.push_back(
                    std::make_unique<SetVisibilityCommand>(vis));
            }
        }
    }
    if (m_document.layers().isEmpty())
        m_document.setCurrentLayerIndex(-1);
    else
        m_document.setCurrentLayerIndex(
            qBound(0, m_document.currentLayerIndex(),
                   m_document.layers().size() - 1));
    m_undoStack.pushCommand(std::move(composite));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    if (m_creative)
        m_creative->refreshHistory();
    setStatus(QStringLiteral("Look deleted — one undo brings it back."));
    return {};
}

void PaintbrushController::applyCreativeLookToSelection()
{
    if (!m_creative)
        return;
    const QString error = m_creative->applyLookToSelection();
    if (!error.isEmpty())
        setStatus(error);
}

QVector<QImage> PaintbrushController::collectCreativePoses(int want,
                                                           QVector<qint64> *pts)
{
    want = qBound(1, want, 8);
    if (pts)
        pts->clear();
    const ObjectTrack *track = nullptr;
    for (const ObjectTrack &t : m_document.tracks())
    {
        if (t.id == m_selectedTrackId && !t.keyframes.isEmpty())
        {
            track = &t;
            break;
        }
    }
    if (!track)
    {
        for (const ObjectTrack &t : m_document.tracks())
        {
            if (t.keyframes.isEmpty())
                continue;
            if (!track || t.keyframes.size() > track->keyframes.size())
                track = &t;
        }
    }

    QVector<QImage> poses;
    if (track)
    {
        poses = creative::collectTrackedPoses(
            *track, want, pts,
            [this](const ObjectTrack &t, const TrackKeyframe &kf) {
                return decodeKeyframeFrame(t, kf);
            },
            [this](const QImage &frame, const QImage &mask) {
                const QImage sticker = renderStickerFrame(
                    frame, mask, 0, QColor(), false, 1.0, nullptr);
                if (sticker.isNull())
                    return QImage();
                QPoint offset;
                return creative::trimTransparent(sticker, &offset);
            });
    }

    if (poses.isEmpty())
    {
        /* No track: use the still cutout, never invented anatomy. */
        int sourceLayerId = -1;
        QRectF rect;
        QString label;
        const QImage cutout = creativePadSubject(&sourceLayerId, &rect, &label);
        if (!cutout.isNull())
        {
            for (int i = 0; i < want; ++i)
            {
                poses.append(cutout);
                if (pts)
                    pts->append(-1);
            }
        }
    }
    return poses;
}

QString PaintbrushController::createPastelLineup()
{
    const QSize canvas = creativePadCanvasSize();
    QVector<qint64> pts;
    const QVector<QImage> poses = collectCreativePoses(5, &pts);
    if (poses.isEmpty())
    {
        const QString msg =
            QStringLiteral("Keep a subject or track a person first.");
        setStatus(msg);
        return msg;
    }
    const bool tracked = !pts.isEmpty() && pts.first() >= 0;
    const int count = poses.size();
    const qreal targetH = canvas.height() * 0.46;
    const qreal margin = canvas.width() * 0.06;
    const qreal slot = canvas.width() * 0.88 / count;
    const qreal baseline = canvas.height() * 0.86;
    const QString group =
        QStringLiteral("lineup-%1").arg(QDateTime::currentMSecsSinceEpoch());

    QList<CanvasLayer> layers;
    const QColor lavender(199, 187, 255), pink(255, 199, 230);
    const QColor ink(90, 74, 107);

    QImage bg = creative::gradientField(canvas, QColor(255, 231, 245),
                                        QColor(207, 198, 255),
                                        QColor(255, 255, 255, 190), 0.20);
    {
        QPainter p(&bg);
        QLinearGradient floor(0, canvas.height() * 0.62, 0, canvas.height());
        floor.setColorAt(0.0, QColor(255, 219, 239, 220));
        floor.setColorAt(1.0, QColor(244, 166, 213, 245));
        p.fillRect(QRectF(0, canvas.height() * 0.62, canvas.width(),
                          canvas.height() * 0.38), floor);
        QLinearGradient horizon(0, canvas.height() * 0.60, 0,
                                canvas.height() * 0.66);
        horizon.setColorAt(0.0, QColor(255, 255, 255, 0));
        horizon.setColorAt(0.6, QColor(255, 255, 255, 170));
        horizon.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.fillRect(QRectF(0, canvas.height() * 0.595, canvas.width(),
                          canvas.height() * 0.07), horizon);
    }
    {
        CanvasLayer l = CanvasLayer::createImage(bg, QPointF(0, 0),
                                                 QStringLiteral("Stage"));
        l.metadata[QStringLiteral("creative.role")] = QStringLiteral("backdrop");
        layers.append(l);
    }

    /* reflections: one plate under the whole lineup */
    QImage reflectPlate(canvas, QImage::Format_ARGB32);
    reflectPlate.fill(Qt::transparent);
    {
        QPainter rp(&reflectPlate);
        rp.setOpacity(0.42);
        for (int i = 0; i < count; ++i)
        {
            const qreal maxW = slot * 1.08;
            const qreal scale =
                qMin(targetH / qMax(1, poses[i].height()),
                     maxW / qMax(1, poses[i].width()));
            const QSize sz(qMax(1, int(poses[i].width() * scale)),
                           qMax(1, int(poses[i].height() * scale)));
            const QImage scaled = poses[i].scaled(
                sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            const QImage mirror = creative::reflection(scaled, 0.55, 0.42);
            const qreal cx = margin + slot * (i + 0.5);
            rp.drawImage(QPointF(cx - mirror.width() / 2.0,
                                 baseline + canvas.height() * 0.004),
                         mirror);
        }
    }
    {
        CanvasLayer l = CanvasLayer::createImage(
            reflectPlate, QPointF(0, 0), QStringLiteral("Floor reflections"));
        l.metadata[QStringLiteral("creative.role")] = QStringLiteral("echo");
        layers.append(l);
    }

    for (int i = 0; i < count; ++i)
    {
        const qreal maxW = slot * 1.08;
        const qreal scale =
            qMin(targetH / qMax(1, poses[i].height()),
                 maxW / qMax(1, poses[i].width()));
        const QSize sz(qMax(1, int(poses[i].width() * scale)),
                       qMax(1, int(poses[i].height() * scale)));
        QImage pose = poses[i].scaled(sz, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
        pose = creative::gradeImage(
            pose, creative::Grade{ 1.10f, 0.94f, 0.92f, lavender, 0.18f,
                                   QColor(255, 255, 255), 0.10f });
        const qreal cx = margin + slot * (i + 0.5);
        CanvasLayer l = CanvasLayer::createImage(
            pose, QPointF(0, 0), QStringLiteral("Pose %1").arg(i + 1));
        l.setTransform(QTransform::fromTranslate(cx - sz.width() / 2.0,
                                                 baseline - sz.height()));
        l.effects.append(art::EffectNode::makeShadow(
            float(qMax<qreal>(3.0, canvas.height() * 0.010)), 0.24f));
        l.metadata[QStringLiteral("creative.linkGroup")] = group;
        l.metadata[QStringLiteral("creative.role")] = QStringLiteral("lineup");
        if (tracked)
            l.metadata[QStringLiteral("creative.ptsMs")] = pts.value(i, -1);
        layers.append(l);
    }

    art::TextTreatment title;
    title.text = QStringLiteral("MAIN CHARACTER");
    title.styleId = QStringLiteral("editorial");
    title.sizePx = float(canvas.height() * 0.052);
    title.color = ink;
    const QImage titleImg = art::typography::render(title, 1.0f);
    if (!titleImg.isNull())
    {
        CanvasLayer l = CanvasLayer::createImage(
            titleImg,
            QPointF((canvas.width() - titleImg.width()) / 2.0,
                    canvas.height() * 0.055),
            QStringLiteral("Main Character"));
        l.type = LAYER_TEXT;
        l.text = title.text;
        l.opacity = 0.82f;
        l.metadata[QStringLiteral("textTreatment")] = title.toVariant();
        l.metadata[QStringLiteral("creative.role")] = QStringLiteral("finish");
        layers.append(l);
    }

    int hideLayerId = -1;
    const CanvasLayer *current = m_document.layerAt(m_document.currentLayerIndex());
    if (current && current->visible && current->id > 0 && !current->raster.isNull()
        && current->metadata.value(QStringLiteral("creative.patch"))
               .toString().isEmpty())
        hideLayerId = current->id;

    const QString error = commitCreativeLayers(
        layers, hideLayerId, QStringLiteral("Pastel Lineup"));
    if (!error.isEmpty())
    {
        setStatus(error);
        return error;
    }
    setStatus(tracked
                  ? QStringLiteral("Pastel lineup from %1 real tracked poses — "
                                   "one undo, every pose editable.")
                        .arg(count)
                  : QStringLiteral("Pastel lineup from the still cutout — one "
                                   "undo. Track a video for real poses."));
    triggerCompanionEvent(QStringLiteral("lineup"));
    return {};
}

QString PaintbrushController::createMotionEcho()
{
    const QSize canvas = creativePadCanvasSize();
    QVector<qint64> pts;
    const QVector<QImage> poses = collectCreativePoses(6, &pts);
    if (poses.isEmpty())
    {
        const QString msg =
            QStringLiteral("Keep a subject or track a person first.");
        setStatus(msg);
        return msg;
    }
    const bool tracked = !pts.isEmpty() && pts.first() >= 0;
    const int count = poses.size();
    const qreal targetH = canvas.height() * 0.55;
    const qreal baseline = canvas.height() * 0.80;
    const QString group =
        QStringLiteral("echo-%1").arg(QDateTime::currentMSecsSinceEpoch());

    QList<CanvasLayer> layers;
    QImage bg = creative::gradientField(canvas, QColor(9, 9, 20),
                                        QColor(24, 19, 42),
                                        QColor(90, 60, 160, 100), 0.42);
    layers.append(CanvasLayer::createImage(bg, QPointF(0, 0),
                                           QStringLiteral("Echo stage")));
    QImage trail(canvas, QImage::Format_ARGB32);
    trail.fill(Qt::transparent);
    {
        QPainter p(&trail);
        p.drawImage(0, 0, creative::starField(canvas, 17,
                                              QColor(240, 240, 255), 110));
        p.drawImage(0, 0, creative::bokehField(
                             canvas, 23,
                             { QColor(78, 123, 255), QColor(255, 111, 174) },
                             70, canvas.height() * 0.004,
                             canvas.height() * 0.018));
    }
    {
        CanvasLayer l = CanvasLayer::createImage(trail, QPointF(0, 0),
                                                 QStringLiteral("Echo trail"));
        l.metadata[QStringLiteral("creative.role")] = QStringLiteral("accent");
        layers.append(l);
    }

    const auto addEcho = [&](int i, const QImage &pose, bool main) {
        const qreal maxW = canvas.width() * 0.28;
        const qreal scale = qMin(targetH / qMax(1, pose.height()),
                                 maxW / qMax(1, pose.width()));
        const QSize sz(qMax(1, int(pose.width() * scale)),
                       qMax(1, int(pose.height() * scale)));
        QImage img = pose.scaled(sz, Qt::IgnoreAspectRatio,
                                 Qt::SmoothTransformation);
        const int hue = (i - 2) * 14;
        if (hue)
            img = creative::hueShift(img, hue);
        img = creative::gradeImage(
            img, creative::Grade{ 0.96f, 1.06f, 1.0f,
                                  main ? QColor(78, 123, 255)
                                       : QColor(255, 111, 174),
                                  0.16f, QColor(255, 255, 255), 0.06f });
        const qreal cx = canvas.width() * (0.12 + 0.145 * i);
        CanvasLayer l = CanvasLayer::createImage(
            img, QPointF(0, 0),
            main ? QStringLiteral("Echo main") : QStringLiteral("Echo %1").arg(i));
        l.setTransform(QTransform::fromTranslate(cx - sz.width() / 2.0,
                                                 baseline - sz.height()));
        l.opacity = main ? 1.0f : float(0.20 + 0.40 * (1.0 - qAbs(i - 2) / 3.0));
        if (main)
            l.effects.append(art::EffectNode::makeShadow(
                float(qMax<qreal>(3.0, canvas.height() * 0.010)), 0.25f));
        l.metadata[QStringLiteral("creative.linkGroup")] = group;
        l.metadata[QStringLiteral("creative.role")] =
            main ? QStringLiteral("subject") : QStringLiteral("echo");
        if (tracked)
            l.metadata[QStringLiteral("creative.ptsMs")] = pts.value(i, -1);
        layers.append(l);
    };

    const int mainIndex = qBound(0, count / 2, count - 1);
    for (int i = 0; i < count; ++i)
        if (i != mainIndex)
            addEcho(i, poses[i], false);
    addEcho(mainIndex, poses[mainIndex], true);

    int hideLayerId = -1;
    const CanvasLayer *current = m_document.layerAt(m_document.currentLayerIndex());
    if (current && current->visible && current->id > 0 && !current->raster.isNull()
        && current->metadata.value(QStringLiteral("creative.patch"))
               .toString().isEmpty())
        hideLayerId = current->id;

    const QString error = commitCreativeLayers(
        layers, hideLayerId, QStringLiteral("Motion Echo"));
    if (!error.isEmpty())
    {
        setStatus(error);
        return error;
    }
    setStatus(tracked
                  ? QStringLiteral("Motion echo from %1 real tracked poses — "
                                   "one undo.")
                        .arg(count)
                  : QStringLiteral("Motion echo from the still cutout — one "
                                   "undo. Track a video for real poses."));
    triggerCompanionEvent(QStringLiteral("motion-echo"));
    return {};
}

void PaintbrushController::duplicateLinked(int preset)
{
    if (preset <= 0)
    {
        duplicateIntoLineup();
        return;
    }
    const int index = m_document.currentLayerIndex();
    const CanvasLayer *src = m_document.layerAt(index);
    if (!src || src->raster.isNull())
    {
        setStatus(QStringLiteral("Select a cutout layer to duplicate."));
        return;
    }
    const QSize canvas = creativePadCanvasSize();
    struct CreativeSlot { qreal cx; qreal baseline; qreal height; qreal rotation; };
    QVector<CreativeSlot> layoutSlots;
    QString desc;
    switch (preset)
    {
    case 1:
        layoutSlots = { { 0.38, 0.82, 0.48, -4.0 }, { 0.62, 0.84, 0.48, 4.0 } };
        desc = QStringLiteral("Duo");
        break;
    case 2:
        layoutSlots = { { 0.30, 0.50, 0.34, -3.0 }, { 0.70, 0.52, 0.34, 3.0 },
                  { 0.30, 0.86, 0.34, 3.0 }, { 0.70, 0.88, 0.34, -3.0 } };
        desc = QStringLiteral("Grid x4");
        break;
    default:
        layoutSlots = { { 0.34, 0.60, 0.50, -6.0 }, { 0.50, 0.54, 0.52, 0.0 },
                  { 0.66, 0.60, 0.50, 6.0 } };
        desc = QStringLiteral("Overlap x3");
        break;
    }
    const QString group =
        QStringLiteral("duo-%1").arg(QDateTime::currentMSecsSinceEpoch());

    QList<CanvasLayer> all = m_document.layers();
    const int firstNew = all.size();
    for (int i = 0; i < layoutSlots.size(); ++i)
    {
        const CreativeSlot &slot = layoutSlots.at(i);
        const qreal targetH = canvas.height() * slot.height;
        const qreal factor = qMin<qreal>(
            targetH / qMax(1, src->raster.height()),
            canvas.width() * 0.42 / qMax(1, src->raster.width()));
        CanvasLayer copy = *src;
        copy.id = 0;
        copy.name = QStringLiteral("%1 %2").arg(desc).arg(i + 1);
        copy.visible = true;
        copy.rotation = float(slot.rotation);
        copy.scale = float(factor);
        const qreal w = src->raster.width() * factor;
        const qreal h = src->raster.height() * factor;
        copy.position = QPointF(canvas.width() * slot.cx - w / 2.0,
                                canvas.height() * slot.baseline - h);
        copy.metadata[QStringLiteral("creative.linkGroup")] = group;
        copy.metadata[QStringLiteral("creative.role")] = QStringLiteral("lineup");
        copy.effects.erase(
            std::remove_if(copy.effects.begin(), copy.effects.end(),
                           [](const art::EffectNode &e) {
                               return e.type == art::EffectNode::Shadow;
                           }),
            copy.effects.end());
        copy.effects.append(art::EffectNode::makeShadow(
            float(qMax<qreal>(3.0, canvas.height() * 0.012)), 0.24f));
        all.append(copy);
    }
    const bool srcVisible = src->visible;
    const int srcId = src->id;
    m_document.setLayers(all);

    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = desc;
    if (srcVisible && srcId > 0)
    {
        SetVisibilityCommand vis;
        vis.layerIndex = index;
        vis.before = true;
        vis.after = false;
        vis.desc = composite->desc;
        m_document.layerAt(index)->visible = false;
        composite->children.push_back(
            std::make_unique<SetVisibilityCommand>(vis));
    }
    const int end = m_document.layers().size();
    for (int i = firstNew; i < end; ++i)
    {
        const CanvasLayer *added = m_document.layerAt(i);
        if (!added)
            continue;
        InsertLayerCommand cmd;
        cmd.desc = composite->desc;
        cmd.index = i;
        cmd.prevCurrentIndex = index;
        cmd.layer = *added;
        composite->children.push_back(std::make_unique<InsertLayerCommand>(cmd));
    }
    m_document.setCurrentLayerIndex(end - 1);
    m_undoStack.pushCommand(std::move(composite));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("%1 created — linked finish, one undo.").arg(desc));
    triggerCompanionEvent(QStringLiteral("smart-duplicate"));
}

void PaintbrushController::duplicateIntoLineup()
{
    const int index = m_document.currentLayerIndex();
    const CanvasLayer *src = m_document.layerAt(index);
    if (!src || src->raster.isNull())
    {
        setStatus(QStringLiteral("Select a cutout layer to build a lineup."));
        return;
    }
    const QSize canvas = creativePadCanvasSize();
    const int count = 5;
    const qreal targetH = qMin<qreal>(canvas.height() * 0.46,
                                      double(src->raster.height()) * src->scale);
    if (targetH < 8.0)
    {
        setStatus(QStringLiteral("That layer is too small to line up."));
        return;
    }
    const qreal factor = targetH / src->raster.height();
    const qreal margin = canvas.width() * 0.06;
    const qreal usable = canvas.width() * 0.88;
    const qreal slot = usable / count;
    const qreal baseline = canvas.height() * 0.86;
    const QString group = QStringLiteral("lineup-%1")
                              .arg(QDateTime::currentMSecsSinceEpoch());

    QList<CanvasLayer> all = m_document.layers();
    const int firstNew = all.size();
    for (int i = 0; i < count; ++i)
    {
        CanvasLayer copy = *src;
        copy.id = 0;
        copy.name = QStringLiteral("Pose %1").arg(i + 1);
        copy.visible = true;
        copy.rotation = 0.0f;
        copy.scale = float(factor);
        const qreal w = src->raster.width() * factor;
        const qreal h = src->raster.height() * factor;
        const qreal cx = margin + slot * (i + 0.5);
        copy.position = QPointF(cx - w / 2.0, baseline - h);
        copy.metadata[QStringLiteral("creative.linkGroup")] = group;
        copy.metadata[QStringLiteral("creative.role")] = QStringLiteral("lineup");
        copy.effects.erase(
            std::remove_if(copy.effects.begin(), copy.effects.end(),
                           [](const art::EffectNode &e) {
                               return e.type == art::EffectNode::Shadow;
                           }),
            copy.effects.end());
        copy.effects.append(art::EffectNode::makeShadow(
            float(qMax<qreal>(3.0, canvas.height() * 0.012)), 0.24f));
        all.append(copy);
    }
    const bool srcVisible = src->visible;
    const int srcId = src->id;
    m_document.setLayers(all);

    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = QStringLiteral("Lineup");
    if (srcVisible && srcId > 0)
    {
        SetVisibilityCommand vis;
        vis.layerIndex = index;
        vis.before = true;
        vis.after = false;
        vis.desc = composite->desc;
        m_document.layerAt(index)->visible = false;
        composite->children.push_back(
            std::make_unique<SetVisibilityCommand>(vis));
    }
    const int end = m_document.layers().size();
    for (int i = firstNew; i < end; ++i)
    {
        const CanvasLayer *added = m_document.layerAt(i);
        if (!added)
            continue;
        InsertLayerCommand cmd;
        cmd.desc = composite->desc;
        cmd.index = i;
        cmd.prevCurrentIndex = index;
        cmd.layer = *added;
        composite->children.push_back(std::make_unique<InsertLayerCommand>(cmd));
    }
    m_document.setCurrentLayerIndex(end - 1);
    m_undoStack.pushCommand(std::move(composite));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Lineup created — style edits stay linked across the poses."));
    triggerCompanionEvent(QStringLiteral("lineup"));
}

} // namespace paintbrush

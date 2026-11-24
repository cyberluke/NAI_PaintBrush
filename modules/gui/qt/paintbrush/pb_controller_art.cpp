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
#include "art/style/mood.hpp"
#include "art/style/typography_style.hpp"
#include "art/style/badges.hpp"
#include "art/surface/paper_renderer.hpp"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace paintbrush {

namespace {

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

    auto cmd = std::make_unique<SetPuppetCommand>();
    cmd->layerId = layer->id;
    cmd->beforeAssetId = layer->puppetAssetId;
    cmd->beforeEffect = layer->puppetEffect;
    cmd->afterAssetId = assetId;
    cmd->afterEffect = fx;
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
    if (!layer || layer->puppetAssetId <= 0)
        return;
    auto cmd = std::make_unique<SetPuppetCommand>();
    cmd->layerId = layer->id;
    cmd->beforeAssetId = layer->puppetAssetId;
    cmd->beforeEffect = layer->puppetEffect;
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
    auto note = [&](const QString &line) {
        report << line;
    };
    auto failed = [&]() {
        for (const QString &line : report)
            if (line.startsWith(QLatin1String("FAIL")))
                return true;
        return false;
    };
    /* Commits are asynchronous now: wait for the full-resolution job to
     * publish on the GUI thread so the one-undo/layer-count checks stay
     * exactly the same contract as before. */
    auto commitNow = [this]() -> QString {
        const QString startError = m_creative->commit();
        if (!startError.isEmpty())
            return startError;
        return m_creative->waitForCommit(120000);
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
    /* The FASHN parse is optional at runtime: report exactly what happened
     * (ready, no outfit, unavailable) without turning a missing model into a
     * creative failure. The separate human-parser smoke is the hard gate. */
    note(QStringLiteral("fashion parser: %1")
             .arg(m_creative->waitForHumanParse(120000)));

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
        QStringLiteral("holo-glitter"), QStringLiteral("holo-livery")
    };
    for (const QString &id : patches)
    {
        m_creative->selectPatch(id);
        m_creative->movePad(0.68, 0.62, 0.35, false);
        m_creative->renderPreviewStill();
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

    const int baseLayers = m_document.layers().size();

    /* Neon Idol: commit + optional suggestion + one undo restores exactly. */
    m_creative->selectPatch(QStringLiteral("neon-idol"));
    m_creative->movePad(0.72, 0.60, 0.30, true);
    m_creative->dumpStagePieces(QStringLiteral("%1/pieces-neon").arg(dir));
    QString error = commitNow();
    const int neonAfter = m_document.layers().size();
    if (!error.isEmpty())
        note(QStringLiteral("FAIL: neon commit: %1").arg(error));
    if (neonAfter - baseLayers < 4)
        note(QStringLiteral("FAIL: neon commit added %1 layers")
                 .arg(neonAfter - baseLayers));
    else
        note(QStringLiteral("commit neon: +%1 layers").arg(neonAfter - baseLayers));

    if (m_creative->suggestionVisible())
    {
        m_creative->acceptSuggestion();
        note(QStringLiteral("suggestion: +%1 layer")
                 .arg(m_document.layers().size() - neonAfter));
        undo();
    }
    undo();
    if (m_document.layers().size() != baseLayers)
        note(QStringLiteral("FAIL: one-undo contract broke (%1 != %2)")
                 .arg(m_document.layers().size()).arg(baseLayers));
    else
        note(QStringLiteral("one-undo contract: OK"));

    /* Sticker Forge: commit + one undo. */
    m_creative->selectPatch(QStringLiteral("sticker-forge"));
    m_creative->movePad(0.42, 0.70, 0.20, false);
    error = commitNow();
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
    error = commitNow();
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
    error = commitNow();
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

    /* Real-fashion semantic pass. The synthetic idol has no garment, so the
     * parser correctly falls back; this fixture (a real fashion model parsed
     * by the same FASHN runtime) proves the garment protection end to end. */
    const QString fashionCutoutPath = QStringLiteral(
        "D:/_SATIN_AI/models/holo-reference/fashion-cutout.png");
    if (!QFileInfo::exists(fashionCutoutPath))
    {
        note(QStringLiteral("fashion fixture: skipped (not found)"));
    }
    else
    {
        const QImage fashionCutout(fashionCutoutPath);
        if (fashionCutout.isNull())
        {
            note(QStringLiteral("FAIL: fashion fixture is unreadable"));
        }
        else
        {
            newBlankCanvas(1440, 1800);
            addImageLayer(fashionCutout, QPointF(260, 220),
                          QStringLiteral("Fashion parser cutout"),
                          LAYER_STICKER);
            if (!m_document.layers().isEmpty())
                m_document.layersRef().last().scale = 0.55f;
            m_document.setCurrentLayerIndex(m_document.layers().size() - 1);
            emit documentChanged();

            m_creative->openForSelection();
            if (!m_creative->hasSubject())
            {
                note(QStringLiteral("FAIL: real fashion cutout has no subject"));
            }
            else
            {
                note(QStringLiteral("fashion subject: %1")
                         .arg(m_creative->subjectLabel()));
                note(QStringLiteral("fashion parser: %1")
                         .arg(m_creative->waitForHumanParse(120000)));
                m_creative->selectPatch(QStringLiteral("holo-liquid"));
                m_creative->movePad(0.62, 0.58, 0.35, false);
                m_creative->renderPreviewStill();
                const QImage fashionPreview = m_creative->previewImage();
                if (fashionPreview.isNull())
                {
                    note(QStringLiteral("FAIL: real fashion preview is empty"));
                }
                else
                {
                    const QString path = QStringLiteral(
                        "%1/selftest-fashion-real.png").arg(dir);
                    if (!fashionPreview.save(path))
                        note(QStringLiteral("FAIL: cannot save %1").arg(path));
                    else
                        note(QStringLiteral("preview fashion-real %1x%2")
                                 .arg(fashionPreview.width())
                                 .arg(fashionPreview.height()));
                }
            }
        }
    }

    m_creative->renderPreviewStill();
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
        QImage cutout = creative::maskToCutout(chosen->raster,
                                                creative::layerMask(*chosen));
        if (cutout.isNull())
            return {};
        QRectF rect = chosen->transform().mapRect(chosen->raster.rect());
        if (!qFuzzyIsNull(chosen->rotation))
        {
            /* bake the rotation into canvas space so the stage sees exactly
             * the silhouette the user has on the canvas */
            const QSize canvas = creativePadCanvasSize();
            QImage baked(canvas, QImage::Format_ARGB32);
            baked.fill(Qt::transparent);
            {
                QPainter p(&baked);
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                p.setTransform(chosen->transform());
                p.drawImage(QPointF(0, 0), cutout);
            }
            QPoint offset;
            cutout = creative::trimTransparent(baked, &offset);
            if (cutout.isNull())
                return {};
            rect = QRectF(offset, cutout.size());
        }
        if (sourceLayerId)
            *sourceLayerId = chosen->id;
        if (canvasRect)
            *canvasRect = rect;
        if (label)
            *label = chosen->name;
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

QString PaintbrushController::commitCreativeLayers(const QList<CanvasLayer> &layers,
                                                   int hideLayerId,
                                                   const QString &desc)
{
    if (layers.isEmpty())
        return QStringLiteral("Nothing to commit.");

    QList<CanvasLayer> all = m_document.layers();
    const int firstNew = all.size();
    for (const CanvasLayer &layer : layers)
    {
        CanvasLayer copy = layer;
        copy.id = 0;
        all.append(copy);
    }
    m_document.setLayers(all);

    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = desc;

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
    /* The render is asynchronous: the pad publishes the layers and the final
     * status on the GUI thread when the full-resolution build completes. */
    setStatus(QStringLiteral("Building the look at canvas resolution…"));
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
    m_document.setLayers(all);

    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = QStringLiteral("Lineup");
    if (src->visible && src->id > 0)
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

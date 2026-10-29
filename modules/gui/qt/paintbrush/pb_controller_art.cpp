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
#include "art/style/mood.hpp"
#include "art/style/typography_style.hpp"
#include "art/style/badges.hpp"
#include "art/surface/paper_renderer.hpp"

#include <QBuffer>
#include <QFileInfo>
#include <QPainter>

namespace paintbrush {

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
    /* the exact frame this selection was made on */
    FrameRef ref = m_document.sourceFrame();
    QImage frame;
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
            frame = m_canvas->grabCanvasRegion(QRectF(QPointF(0, 0), m_document.canvasSize()));
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
    if (frame.isNull())
    {
        setStatus(QStringLiteral("Could not read the source frame for this moment."));
        return;
    }
    /* the mask lives in canvas space; map it onto the frame */
    QImage mask = m_objectMask;
    if (mask.size() != frame.size())
        mask = mask.scaled(frame.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

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
            return;
        }
    }
    const int id = m_document.harvest()->add(item);
    m_document.setDirty(true);
    setStatus(QStringLiteral("Kept %1 — drag it from the tray when you want it.")
                  .arg(harvestTimeText(item.sourceFrame.ptsMs)));
    triggerCompanionEvent(QStringLiteral("harvest-kept"));
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

static void pushEffects(PaintbrushDocument &doc, UndoStack &undo, CanvasLayer *l,
                        const QVector<art::EffectNode> &after, const QString &desc)
{
    SetEffectsCommand cmd;
    cmd.layerId = l->id;
    cmd.before = l->effects;
    cmd.after = after;
    cmd.desc = desc;
    cmd.apply(doc);
    undo.pushCommand(std::make_unique<SetEffectsCommand>(cmd));
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
    CanvasLayer *l = m_document.layerAt(index);
    if (!l)
        return;
    QVector<art::EffectNode> fx = l->effects;
    fx.erase(std::remove_if(fx.begin(), fx.end(),
                            [](const art::EffectNode &e) { return e.type == art::EffectNode::Shadow; }),
             fx.end());
    fx.append(art::EffectNode::makeShadow(float(blur), float(opacity)));
    pushEffects(m_document, m_undoStack, l, fx, QStringLiteral("Shadow"));
    if (m_canvas) m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::addLayerGlow(int index, const QString &color, double radius)
{
    CanvasLayer *l = m_document.layerAt(index);
    if (!l)
        return;
    QVector<art::EffectNode> fx = l->effects;
    fx.erase(std::remove_if(fx.begin(), fx.end(),
                            [](const art::EffectNode &e) { return e.type == art::EffectNode::Glow; }),
             fx.end());
    fx.append(art::EffectNode::makeGlow(QColor(color), float(radius), 1.0f));
    pushEffects(m_document, m_undoStack, l, fx, QStringLiteral("Glow"));
    if (m_canvas) m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::addLayerOutline(int index, const QString &color, double width)
{
    CanvasLayer *l = m_document.layerAt(index);
    if (!l)
        return;
    QVector<art::EffectNode> fx = l->effects;
    fx.erase(std::remove_if(fx.begin(), fx.end(),
                            [](const art::EffectNode &e) { return e.type == art::EffectNode::Outline; }),
             fx.end());
    fx.append(art::EffectNode::makeOutline(QColor(color), float(width), 0.2f));
    pushEffects(m_document, m_undoStack, l, fx, QStringLiteral("Outline"));
    if (m_canvas) m_canvas->markCanvasDirty();
    emit documentChanged();
}

void PaintbrushController::clearLayerEffects(int index)
{
    CanvasLayer *l = m_document.layerAt(index);
    if (!l || l->effects.isEmpty())
        return;
    pushEffects(m_document, m_undoStack, l, {}, QStringLiteral("Clear finish"));
    if (m_canvas) m_canvas->markCanvasDirty();
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

} // namespace paintbrush

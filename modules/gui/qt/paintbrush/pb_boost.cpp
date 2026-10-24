/*****************************************************************************
 * pb_boost.cpp : NAI Paintbrush — Teen Art Director (Boost) engine
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_boost.hpp"
#include "art/finish/effects.hpp"

#include <QtMath>
#include <algorithm>
#include <cmath>

namespace paintbrush {

/* ---------------------------------------------------------- analysis */

const BoostAnalysis::Pose *BoostAnalysis::pose(int layerId) const
{
    for (const Pose &p : sequence)
        if (p.layerId == layerId)
            return &p;
    return nullptr;
}

namespace {

bool poseSource(const CanvasLayer &layer, const PaintbrushDocument &doc,
                QString *mrl, qint64 *pts, int *harvestId)
{
    *harvestId = layer.harvestId;
    if (layer.harvestId > 0)
    {
        if (const HarvestItem *h = doc.harvest()->itemById(layer.harvestId))
        {
            *mrl = h->sourceFrame.mrl;
            *pts = h->sourceFrame.ptsMs;
            return true;
        }
    }
    const QVariant m = layer.metadata.value(QStringLiteral("sourceMRL"));
    const QVariant t = layer.metadata.value(QStringLiteral("sourceTimeMs"));
    if (!m.toString().isEmpty() && t.isValid())
    {
        *mrl = m.toString();
        *pts = qint64(t.toDouble());
        return true;
    }
    return false;
}

QRectF canvasBoundsOf(const CanvasLayer &layer)
{
    const QTransform t = layer.transform();
    return t.mapRect(QRectF(layer.raster.rect()));
}

} // namespace

BoostAnalysis BoostEngine::analyze(const PaintbrushDocument &doc)
{
    BoostAnalysis a;
    struct Cand
    {
        QString mrl;
        BoostAnalysis::Pose pose;
    };
    QVector<Cand> cands;
    const QList<CanvasLayer> &layers = doc.layers();
    for (int i = 0; i < layers.size(); ++i)
    {
        const CanvasLayer &l = layers[i];
        if (!l.visible || l.raster.isNull())
            continue;
        if (l.type != LAYER_STICKER && l.type != LAYER_IMAGE)
            continue;
        QString mrl;
        qint64 pts = 0;
        int hid = 0;
        if (!poseSource(l, doc, &mrl, &pts, &hid))
            continue;
        Cand c;
        c.mrl = mrl;
        c.pose.layerId = l.id;
        c.pose.layerIndex = i;
        c.pose.harvestId = hid;
        c.pose.ptsMs = pts;
        c.pose.canvasBounds = canvasBoundsOf(l);
        c.pose.canvasCenter = c.pose.canvasBounds.center();
        c.pose.canvasArea = c.pose.canvasBounds.width() * c.pose.canvasBounds.height();
        cands.append(c);
    }
    if (cands.isEmpty())
    {
        a.notes << QStringLiteral("No harvested poses on the canvas.");
        return a;
    }

    /* dominant source: the media with the most poses (ties -> first seen) */
    QString best;
    int bestCount = 0;
    for (const Cand &c : cands)
    {
        int n = 0;
        for (const Cand &d : cands)
            if (d.mrl == c.mrl)
                ++n;
        if (n > bestCount)
        {
            bestCount = n;
            best = c.mrl;
        }
    }
    a.sourceMrl = best;
    for (const Cand &c : cands)
        if (c.mrl == best)
            a.sequence.append(c.pose);
    std::sort(a.sequence.begin(), a.sequence.end(),
              [](const BoostAnalysis::Pose &x, const BoostAnalysis::Pose &y)
              {
                  if (x.ptsMs != y.ptsMs)
                      return x.ptsMs < y.ptsMs;
                  return x.layerId < y.layerId;
              });

    /* motion vector: temporal order -> canvas displacement (spec §29) */
    QPointF sum;
    double dist = 0.0;
    int n = 0;
    for (int i = 1; i < a.sequence.size(); ++i)
    {
        const QPointF d = a.sequence[i].canvasCenter - a.sequence[i - 1].canvasCenter;
        sum += d;
        dist += std::hypot(d.x(), d.y());
        ++n;
    }
    const double len = std::hypot(sum.x(), sum.y());
    if (len > 1e-3)
        a.motionDirection = sum / len;
    else
        a.motionDirection = QPointF(1.0, 0.0);
    a.motionDistance = n > 0 ? dist / n : 0.0;

    /* hero: largest on-canvas area, weighted by centrality; ties -> latest */
    const QPointF cc(doc.canvasSize().width() / 2.0, doc.canvasSize().height() / 2.0);
    const double diag = std::hypot(cc.x(), cc.y());
    double maxArea = 1.0;
    for (const BoostAnalysis::Pose &p : a.sequence)
        maxArea = std::max(maxArea, p.canvasArea);
    double bestScore = -1.0;
    for (const BoostAnalysis::Pose &p : a.sequence)
    {
        const double areaN = p.canvasArea / maxArea;
        const double dc = std::hypot(p.canvasCenter.x() - cc.x(), p.canvasCenter.y() - cc.y()) / diag;
        const double score = areaN * (1.0 - 0.5 * dc) + 1e-6 * p.ptsMs;
        if (score > bestScore)
        {
            bestScore = score;
            a.heroLayerId = p.layerId;
        }
    }

    /* spacing evenness along the motion axis */
    if (a.sequence.size() >= 3)
    {
        QVector<double> proj;
        for (const BoostAnalysis::Pose &p : a.sequence)
            proj.append(QPointF::dotProduct(p.canvasCenter, a.motionDirection));
        QVector<double> gaps;
        for (int i = 1; i < proj.size(); ++i)
            gaps.append(proj[i] - proj[i - 1]);
        double mean = 0.0;
        for (double g : gaps) mean += g;
        mean /= gaps.size();
        double dev = 0.0;
        for (double g : gaps) dev = std::max(dev, std::fabs(g - mean));
        a.spacingUneven = mean > 1.0 && dev > 0.35 * std::fabs(mean) &&
                          dev > 0.06 * doc.canvasSize().width();
    }

    /* sparsest temporal gap worth bridging (both ends need harvest masks) */
    if (a.sequence.size() >= 2)
    {
        QVector<qint64> gaps;
        for (int i = 1; i < a.sequence.size(); ++i)
            gaps.append(a.sequence[i].ptsMs - a.sequence[i - 1].ptsMs);
        int bi = -1;
        qint64 bg = 0;
        for (int i = 0; i < gaps.size(); ++i)
        {
            const BoostAnalysis::Pose &pa = a.sequence[i];
            const BoostAnalysis::Pose &pb = a.sequence[i + 1];
            if (pa.harvestId <= 0 || pb.harvestId <= 0)
                continue;
            if (gaps[i] > bg)
            {
                bg = gaps[i];
                bi = i;
            }
        }
        /* the biggest gap is "sparse" when it is clearly longer than the
         * typical spacing of the other gaps (evenly spaced sequences get no
         * bridge) and long enough to hold a distinct real frame */
        QVector<qint64> others;
        for (int i = 0; i < gaps.size(); ++i)
            if (i != bi)
                others.append(gaps[i]);
        qint64 threshold = 300;
        if (!others.isEmpty())
        {
            std::sort(others.begin(), others.end());
            const qint64 medianOthers = others[others.size() / 2];
            threshold = std::max<qint64>(400, qint64(1.3 * medianOthers));
        }
        if (bi >= 0 && bg >= threshold)
        {
            a.bridgeFromLayerId = a.sequence[bi].layerId;
            a.bridgeToLayerId = a.sequence[bi + 1].layerId;
            a.bridgeFromMs = a.sequence[bi].ptsMs;
            a.bridgeToMs = a.sequence[bi + 1].ptsMs;
        }
    }

    a.notes << QStringLiteral("%1 pose(s) from one source, motion %2 px per step.")
                  .arg(a.sequence.size()).arg(int(a.motionDistance));
    return a;
}

/* ------------------------------------------------------ transaction */

void BoostTransaction::setRole(int layerId, art::LayerRole role)
{
    CanvasLayer *l = m_doc.layerById(layerId);
    if (!l || l->role == int(role))
        return;
    auto cmd = std::make_unique<SetRoleCommand>();
    cmd->layerId = layerId;
    cmd->before = l->role;
    cmd->after = int(role);
    cmd->desc = QStringLiteral("Set role %1").arg(art::layerRoleName(role));
    cmd->apply(m_doc);
    m_ops.push_back(std::move(cmd));
}

void BoostTransaction::setEffects(int layerId, const QVector<art::EffectNode> &effects)
{
    CanvasLayer *l = m_doc.layerById(layerId);
    if (!l)
        return;
    auto cmd = std::make_unique<SetEffectsCommand>();
    cmd->layerId = layerId;
    cmd->before = l->effects;
    cmd->after = effects;
    cmd->desc = QStringLiteral("Set finish");
    cmd->apply(m_doc);
    m_ops.push_back(std::move(cmd));
}

void BoostTransaction::setTransform(int layerId, const QTransform &after, const QString &desc)
{
    CanvasLayer *l = m_doc.layerById(layerId);
    if (!l)
        return;
    const int idx = m_doc.indexOfLayerId(layerId);
    auto cmd = std::make_unique<SetTransformCommand>(layerId, idx, l->transform(), after, desc);
    cmd->apply(m_doc);
    m_ops.push_back(std::move(cmd));
}

void BoostTransaction::reorder(int index, int target, const QString &desc)
{
    if (index == target || index < 0 || target < 0 ||
        index >= m_doc.layerCount() || target >= m_doc.layerCount())
        return;
    auto cmd = std::make_unique<ReorderCommand>();
    cmd->index = index;
    cmd->target = target;
    cmd->prevCurrentIndex = m_doc.currentLayerIndex();
    cmd->desc = desc;
    cmd->apply(m_doc);
    m_ops.push_back(std::move(cmd));
}

int BoostTransaction::insertLayer(CanvasLayer layer, int index, const QString &desc)
{
    auto cmd = std::make_unique<InsertLayerCommand>();
    cmd->desc = desc;
    cmd->prevCurrentIndex = m_doc.currentLayerIndex();
    QList<CanvasLayer> layers = m_doc.layers();
    const int idx = qBound(0, index, layers.size());
    layer.id = 0;
    layers.insert(idx, std::move(layer));
    m_doc.setLayers(layers);          /* assigns a fresh stable id */
    cmd->index = idx;
    cmd->layer = *m_doc.layerAt(idx); /* recorded WITH its id */
    const int id = cmd->layer.id;
    m_ops.push_back(std::move(cmd));
    return id;
}

int BoostTransaction::addHarvest(const HarvestItem &item, const QString &desc)
{
    auto cmd = std::make_unique<AddHarvestCommand>();
    cmd->item = std::make_shared<HarvestItem>(item);
    cmd->item->id = 0;
    cmd->desc = desc;
    cmd->apply(m_doc);
    const int id = cmd->item->id;
    m_ops.push_back(std::move(cmd));
    return id;
}

void BoostTransaction::setArtState(const SetArtStateCommand::State &after, const QString &desc)
{
    auto cmd = std::make_unique<SetArtStateCommand>();
    cmd->before = SetArtStateCommand::capture(m_doc);
    cmd->after = after;
    cmd->desc = desc;
    cmd->apply(m_doc);
    m_ops.push_back(std::move(cmd));
}

std::unique_ptr<CompositeCommand> BoostTransaction::finish(const QString &desc)
{
    if (m_ops.empty())
        return nullptr;
    auto composite = std::make_unique<CompositeCommand>();
    composite->desc = desc;
    composite->children = std::move(m_ops);
    m_ops.clear();
    return composite;
}

void BoostTransaction::rollback()
{
    for (int i = int(m_ops.size()) - 1; i >= 0; --i)
        m_ops[size_t(i)]->revert(m_doc);
    m_ops.clear();
    m_summary.clear();
}

/* ---------------------------------------------------- deterministic plan */

void BoostEngine::planDeterministic(BoostTransaction &tx, const BoostAnalysis &a,
                                    const BoostOptions &o)
{
    PaintbrushDocument &doc = tx.document();
    if (!a.hasSequence())
    {
        if (a.sequence.size() == 1)
        {
            /* one pose: give it hierarchy and depth, nothing else */
            const int id = a.sequence[0].layerId;
            tx.setRole(id, art::LayerRole::Hero);
            if (o.allowShadow)
            {
                art::EffectNode sh = art::EffectNode::makeShadow(o.shadowBlur, o.shadowOpacity);
                tx.setEffects(id, { sh });
                tx.note(QStringLiteral("Set the single pose as hero with a soft shadow."));
            }
        }
        return;
    }

    /* streak length: a fraction of the pose spacing, capped so it never
     * becomes a wall of smear (spec §19 "directional continuity, not blur
     * everything") */
    const double motionDist = qBound(8.0, a.motionDistance * 0.22, 90.0) * o.motionStrength;
    int ghosts = 0, supports = 0;
    const int heroIdx = [&]() -> int
    {
        for (int i = 0; i < a.sequence.size(); ++i)
            if (a.sequence[i].layerId == a.heroLayerId)
                return i;
        return int(a.sequence.size()) - 1;
    }();

    /* 1) hierarchy + motion continuity: earlier poses echo (ghost), later
     *    poses support, the hero stays clear (spec §20, §45). */
    for (int i = 0; i < a.sequence.size(); ++i)
    {
        const int id = a.sequence[i].layerId;
        art::LayerRole role;
        if (i == heroIdx)
            role = art::LayerRole::Hero;
        else if (i < heroIdx)
            role = art::LayerRole::Ghost;
        else
            role = art::LayerRole::Support;
        tx.setRole(id, role);

        QVector<art::EffectNode> fx;
        if (role == art::LayerRole::Hero)
        {
            if (o.allowShadow)
            {
                art::EffectNode sh = art::EffectNode::makeShadow(o.shadowBlur, o.shadowOpacity);
                sh.lightDistance = o.shadowBlur;
                fx.append(sh);
            }
        }
        else if (role == art::LayerRole::Ghost)
        {
            /* an echo, not a smear: the ghost stays legible (its silhouette
             * is the whole point) and only its trailing edge streaks a short
             * way opposite to the motion. The farthest-in-time ghost fades most. */
            const double k = double(heroIdx - i) / double(std::max(1, heroIdx));
            art::EffectNode g = art::EffectNode::makeGhost(0.70f - 0.30f * float(k), 0.25f + 0.35f * float(k));
            if (o.allowMotionBlur)
            {
                /* order matters: blur the full-strength silhouette first, then
                 * fade it, so the streak never out-shouts the echo */
                fx.append(art::EffectNode::makeMotionBlur(a.motionDirection,
                                                          float(motionDist), 0.85f, 16));
            }
            fx.append(g);
            ++ghosts;
        }
        else
        {
            if (o.allowShadow)
            {
                art::EffectNode sh = art::EffectNode::makeShadow(o.shadowBlur * 0.75f,
                                                                 o.shadowOpacity * 0.7f);
                sh.lightDistance = o.shadowBlur * 0.6f;
                fx.append(sh);
            }
            ++supports;
        }
        tx.setEffects(id, fx);
    }

    /* 2) depth: hero in front of every other pose */
    {
        int heroIndex = doc.indexOfLayerId(a.heroLayerId);
        int frontmost = heroIndex;
        for (const BoostAnalysis::Pose &p : a.sequence)
            frontmost = std::max(frontmost, doc.indexOfLayerId(p.layerId));
        if (heroIndex >= 0 && frontmost > heroIndex)
        {
            tx.reorder(heroIndex, frontmost, QStringLiteral("Bring hero forward"));
            tx.note(QStringLiteral("Brought the hero pose in front."));
        }
    }

    /* 3) spacing: even out the sequence along the motion axis (bounded moves) */
    if (o.allowSpacingCleanup && a.spacingUneven && a.sequence.size() >= 3)
    {
        QVector<double> proj;
        for (const BoostAnalysis::Pose &p : a.sequence)
            proj.append(QPointF::dotProduct(p.canvasCenter, a.motionDirection));
        const double lo = proj.first(), hi = proj.last();
        const double maxMove = 0.15 * doc.canvasSize().width();
        int moved = 0;
        for (int i = 1; i < a.sequence.size() - 1; ++i)
        {
            const double target = lo + (hi - lo) * double(i) / double(a.sequence.size() - 1);
            double delta = target - proj[i];
            delta = qBound(-maxMove, delta, maxMove);
            if (std::fabs(delta) < 2.0)
                continue;
            CanvasLayer *l = doc.layerById(a.sequence[i].layerId);
            if (!l)
                continue;
            QTransform t = l->transform();
            t = t * QTransform::fromTranslate(a.motionDirection.x() * delta,
                                              a.motionDirection.y() * delta);
            tx.setTransform(l->id, t, QStringLiteral("Even spacing"));
            ++moved;
        }
        if (moved > 0)
            tx.note(QStringLiteral("Evened the spacing of %1 pose(s) along the motion.").arg(moved));
    }

    QString dirWord;
    {
        const QPointF d = a.motionDirection;
        if (std::fabs(d.x()) >= std::fabs(d.y()))
            dirWord = d.x() >= 0 ? QStringLiteral("rightward") : QStringLiteral("leftward");
        else
            dirWord = d.y() >= 0 ? QStringLiteral("downward") : QStringLiteral("upward");
    }
    if (ghosts > 0)
        tx.note(QStringLiteral("Faded %1 earlier pose(s) into ghosts%2 to carry the motion %3.")
                    .arg(ghosts)
                    .arg(o.allowMotionBlur ? QStringLiteral(" with a short directional blur") : QString())
                    .arg(dirWord));
    if (supports > 0 && o.allowShadow)
        tx.note(QStringLiteral("Grounded %1 supporting pose(s) with soft shadows.").arg(supports));
}

/* ------------------------------------------------------- bridge pose */

QVector<qint64> BoostEngine::bridgeTimestamps(qint64 fromMs, qint64 toMs, int count)
{
    QVector<qint64> out;
    if (toMs <= fromMs || count <= 0)
        return out;
    const qint64 span = toMs - fromMs;
    /* midpoint first, then symmetric offsets: 0.5, 0.38, 0.62, 0.3, 0.7 ... */
    const double fracs[5] = { 0.5, 0.38, 0.62, 0.30, 0.70 };
    for (int i = 0; i < std::min(count, 5); ++i)
    {
        const qint64 t = fromMs + qint64(std::llround(span * fracs[i]));
        if (t > fromMs && t < toMs && !out.contains(t))
            out.append(t);
    }
    return out;
}

namespace {

double laplacianVariance(const QImage &img, const QRect &r)
{
    const QRect rect = r.intersected(img.rect());
    if (rect.width() < 3 || rect.height() < 3)
        return 0.0;
    const QImage g = img.convertToFormat(QImage::Format_Grayscale8);
    double sum = 0.0, sum2 = 0.0;
    qint64 n = 0;
    for (int y = rect.top() + 1; y < rect.bottom(); ++y)
    {
        const uchar *up = g.constScanLine(y - 1);
        const uchar *row = g.constScanLine(y);
        const uchar *dn = g.constScanLine(y + 1);
        for (int x = rect.left() + 1; x < rect.right(); ++x)
        {
            const int lap = 4 * row[x] - row[x - 1] - row[x + 1] - up[x] - dn[x];
            sum += lap;
            sum2 += double(lap) * lap;
            ++n;
        }
    }
    if (n == 0)
        return 0.0;
    const double mean = sum / n;
    return sum2 / n - mean * mean;
}

/* Mean absolute gray difference between the candidate frame (inside the
 * pose's source bounds, over its mask) and the pose's cutout pixels. */
double poseDifference(const QImage &frame, const HarvestItem &h)
{
    if (h.cutout.isNull() || h.mask.isNull())
        return 0.0;
    const QImage f = frame.convertToFormat(QImage::Format_ARGB32);
    double acc = 0.0;
    qint64 n = 0;
    for (int y = 0; y < h.mask.height(); ++y)
    {
        const int fy = h.sourceBounds.y() + y;
        if (fy < 0 || fy >= f.height())
            continue;
        const uchar *m = h.mask.constScanLine(y);
        const QRgb *cut = reinterpret_cast<const QRgb *>(h.cutout.constScanLine(y));
        const QRgb *fr = reinterpret_cast<const QRgb *>(f.constScanLine(fy));
        for (int x = 0; x < h.mask.width(); ++x)
        {
            if (m[x] < 128)
                continue;
            const int fx = h.sourceBounds.x() + x;
            if (fx < 0 || fx >= f.width())
                continue;
            acc += std::abs(qGray(cut[x]) - qGray(fr[fx]));
            ++n;
        }
    }
    return n > 0 ? acc / n / 255.0 : 0.0;
}

} // namespace

int BoostEngine::scoreBridgeCandidates(QVector<BridgeCandidate> &cands,
                                       const HarvestItem &a, const HarvestItem &b,
                                       qint64 fromMs, qint64 toMs)
{
    if (cands.isEmpty() || toMs <= fromMs)
        return -1;
    const QRect search = a.sourceBounds.united(b.sourceBounds)
                             .marginsAdded(QMargins(16, 16, 16, 16));
    double maxSharp = 1e-9;
    for (BridgeCandidate &c : cands)
    {
        if (c.frame.isNull())
            continue;
        c.sharpness = laplacianVariance(c.frame, search);
        maxSharp = std::max(maxSharp, c.sharpness);
        const double dA = poseDifference(c.frame, a);
        const double dB = poseDifference(c.frame, b);
        /* a bridge must differ from BOTH neighbours (not a near-duplicate) */
        c.distinctness = std::min(dA, dB);
        const double mid = fromMs + (toMs - fromMs) / 2.0;
        c.midpoint = 1.0 - std::fabs(double(c.ptsMs) - mid) / ((toMs - fromMs) / 2.0);
    }
    int best = -1;
    double bestScore = -1.0;
    for (int i = 0; i < cands.size(); ++i)
    {
        BridgeCandidate &c = cands[i];
        if (c.frame.isNull())
            continue;
        const double sharpN = c.sharpness / maxSharp;
        const double distinctN = qBound(0.0, c.distinctness / 0.25, 1.0);
        c.score = 0.45 * qBound(0.0, c.midpoint, 1.0) + 0.30 * distinctN + 0.25 * sharpN;
        if (c.score > bestScore)
        {
            bestScore = c.score;
            best = i;
        }
    }
    return best;
}

QPointF BoostEngine::bridgeSeedPoint(const HarvestItem &a, const HarvestItem &b, qint64 ptsMs)
{
    const qint64 ta = a.sourceFrame.ptsMs, tb = b.sourceFrame.ptsMs;
    double k = 0.5;
    if (tb > ta)
        k = qBound(0.0, double(ptsMs - ta) / double(tb - ta), 1.0);
    const QPointF ca = a.centroid(), cb = b.centroid();
    return ca + (cb - ca) * k;
}

int BoostEngine::insertBridgePose(BoostTransaction &tx, const BoostAnalysis &a,
                                  const BoostOptions &o, HarvestItem derived)
{
    PaintbrushDocument &doc = tx.document();
    const BoostAnalysis::Pose *pa = a.pose(a.bridgeFromLayerId);
    const BoostAnalysis::Pose *pb = a.pose(a.bridgeToLayerId);
    const CanvasLayer *la = doc.layerById(a.bridgeFromLayerId);
    const CanvasLayer *lb = doc.layerById(a.bridgeToLayerId);
    if (!pa || !pb || !la || !lb || derived.cutout.isNull())
        return 0;

    /* derived harvest asset into the tray (provenance kept, spec §54) */
    derived.metadata.insert(QStringLiteral("derivedFrom"),
                            QVariantList{ pa->harvestId, pb->harvestId });
    derived.metadata.insert(QStringLiteral("boostOp"), QStringLiteral("bridge"));
    derived.semanticLabel = QStringLiteral("bridge");
    const int hid = tx.addHarvest(derived, QStringLiteral("Harvest bridge pose"));

    /* size to the neighbours: match their mean on-canvas height */
    const double hA = pa->canvasBounds.height(), hB = pb->canvasBounds.height();
    const double targetH = (hA + hB) / 2.0;
    const double scale = qBound(0.05, targetH / std::max(1.0, double(derived.cutout.height())), 20.0);
    const QPointF center = (pa->canvasCenter + pb->canvasCenter) / 2.0;

    CanvasLayer layer = CanvasLayer::createImage(derived.cutout, QPointF(0, 0),
                                                 QStringLiteral("Bridge pose %1")
                                                     .arg(harvestTimeText(derived.sourceFrame.ptsMs)));
    layer.type = LAYER_STICKER;
    layer.harvestId = hid;
    layer.scale = float(scale);
    layer.rotation = float((la->rotation + lb->rotation) / 2.0f);
    layer.metadata[QStringLiteral("sourceMRL")] = derived.sourceFrame.mrl;
    layer.metadata[QStringLiteral("sourceTitle")] = derived.sourceFrame.title;
    layer.metadata[QStringLiteral("sourceTimeMs")] = qint64(derived.sourceFrame.ptsMs);
    layer.metadata[QStringLiteral("boostOp")] = QStringLiteral("bridge");
    /* position so the content center lands at `center` (pivot = raster center) */
    const QPointF c(layer.raster.rect().center());
    layer.position = center - c;
    layer.role = int(art::LayerRole::Transition);

    const double motionDist = qBound(8.0, a.motionDistance * 0.22, 90.0) * o.motionStrength;
    if (o.allowMotionBlur)
    {
        layer.effects = { art::EffectNode::makeMotionBlur(a.motionDirection, float(motionDist), 0.85f, 16),
                          art::EffectNode::makeGhost(0.6f, 0.2f) };
    }
    else
        layer.effects = { art::EffectNode::makeGhost(0.55f, 0.25f) };

    /* behind both neighbours */
    const int idx = std::min(doc.indexOfLayerId(la->id), doc.indexOfLayerId(lb->id));
    const int newId = tx.insertLayer(layer, std::max(0, idx), QStringLiteral("Insert bridge pose"));
    tx.note(QStringLiteral("Added one real bridge pose (%1) between %2 and %3 and pushed it behind.")
                .arg(harvestTimeText(derived.sourceFrame.ptsMs),
                     harvestTimeText(pa->ptsMs), harvestTimeText(pb->ptsMs)));
    return newId;
}

QString BoostEngine::composeSummary(const QStringList &lines)
{
    if (lines.isEmpty())
        return QStringLiteral("Nothing to boost yet — harvest a few poses first.");
    return lines.join(QLatin1Char(' '));
}

} // namespace paintbrush

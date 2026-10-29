/*****************************************************************************
 * pb_boost.hpp : NAI Paintbrush — Teen Art Director (Boost) engine
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_BOOST_HPP
#define PB_BOOST_HPP

#include "pb_document.hpp"
#include "pb_history.hpp"
#include "pb_harvest.hpp"
#include "art/style/mood.hpp"

#include <QPointF>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

namespace paintbrush {

/* What the art director understood about the raw composition (spec §24.1,
 * §26, §29). Pure, deterministic for the same document state. */
struct BoostAnalysis
{
    struct Pose
    {
        int layerId = 0;
        int layerIndex = -1;
        int harvestId = 0;
        qint64 ptsMs = 0;
        QPointF canvasCenter;   /* content center on the canvas */
        double canvasArea = 0.0; /* on-canvas area (scale applied) */
        QRectF canvasBounds;
    };

    QString sourceMrl;              /* the dominant same-source media */
    QVector<Pose> sequence;         /* temporal order (by source PTS) */
    QPointF motionDirection{ 1.0, 0.0 }; /* unit, canvas space */
    double motionDistance = 0.0;    /* mean spacing between poses (canvas px) */
    int heroLayerId = 0;
    /* the sparsest temporal gap worth bridging (A -> B), or 0/0 */
    int bridgeFromLayerId = 0;
    int bridgeToLayerId = 0;
    qint64 bridgeFromMs = 0;
    qint64 bridgeToMs = 0;
    bool spacingUneven = false;
    QStringList notes;

    bool hasSequence() const { return sequence.size() >= 2; }
    bool wantsBridge() const { return bridgeFromLayerId != 0 && bridgeToLayerId != 0; }
    const Pose *pose(int layerId) const;
};

struct BoostOptions
{
    bool allowBridgePose = true;
    bool allowMotionBlur = true;
    bool allowShadow = true;
    bool allowSpacingCleanup = true;
    float motionStrength = 1.0f;    /* from the Mood */
    float shadowBlur = 8.0f;
    float shadowOpacity = 0.4f;
};

/* A real-frame bridge candidate (spec §27, §28): one exact decoded frame
 * inside the A->B source interval plus its deterministic score. */
struct BridgeCandidate
{
    qint64 ptsMs = 0;
    QImage frame;
    double score = 0.0;
    double sharpness = 0.0;
    double distinctness = 0.0;
    double midpoint = 0.0;
};

/* One Boost = one grouped, reversible transaction (spec §46). Operations are
 * applied to the document immediately and recorded; finish() wraps them into a
 * single CompositeCommand for the undo stack ("Undo Boost"). If anything
 * fails, rollback() reverts every applied step in reverse order. */
class BoostTransaction
{
public:
    explicit BoostTransaction(PaintbrushDocument &doc) : m_doc(doc) {}
    ~BoostTransaction() = default;

    PaintbrushDocument &document() { return m_doc; }
    int operationCount() const { return int(m_ops.size()); }
    const QStringList &summary() const { return m_summary; }
    void note(const QString &line) { m_summary.append(line); }

    /* Typed operations (applied now, recorded for undo). */
    void setRole(int layerId, art::LayerRole role);
    void setEffects(int layerId, const QVector<art::EffectNode> &effects);
    void setTransform(int layerId, const QTransform &after, const QString &desc);
    /* Move the layer at `index` to `target` (front = last). */
    void reorder(int index, int target, const QString &desc);
    /* Insert `layer` at `index` (0 = back). Returns the assigned layer id. */
    int insertLayer(CanvasLayer layer, int index, const QString &desc);
    /* Add a derived harvest item to the tray (undo removes it). Returns id. */
    int addHarvest(const HarvestItem &item, const QString &desc);
    void setArtState(const SetArtStateCommand::State &after, const QString &desc);
    /* Living Harvest: attach a puppet asset + effect to one layer (one undo
     * restores the static figure). */
    void setPuppet(int layerId, int assetId, const motion::PuppetEffect &effect,
                   const QString &desc);
    /* Living Harvest: add a rigged asset to the store (undo removes it).
     * Returns the assigned asset id (0 on failure). */
    int addPuppetAsset(const motion::PuppetAsset &asset, const QString &desc);

    /* Wrap everything into one undo step. Empty transactions return null. */
    std::unique_ptr<CompositeCommand> finish(const QString &desc);
    /* Revert every applied operation (failure path). */
    void rollback();

private:
    PaintbrushDocument &m_doc;
    std::vector<std::unique_ptr<UndoCommand>> m_ops;
    QStringList m_summary;
};

/* The deterministic art-direction brain. No LLM is required for any of this
 * (spec §48, §49): structure, spacing, depth, motion continuity, hierarchy
 * and restrained finish are computed from the document + harvest metadata. */
class BoostEngine
{
public:
    /* Inspect the composition: same-source poses, temporal order, motion
     * vector, hero, sparsest gap. */
    static BoostAnalysis analyze(const PaintbrushDocument &doc);

    /* Apply the deterministic plan into `tx` (roles, depth, spacing, finish).
     * The bridge pose is NOT inserted here (it needs real frames + a mask);
     * see bridgeTimestamps()/scoreBridgeCandidates()/insertBridgePose(). */
    static void planDeterministic(BoostTransaction &tx, const BoostAnalysis &a,
                                  const BoostOptions &o);

    /* Candidate PTS values inside (fromMs, toMs): real frames to decode. */
    static QVector<qint64> bridgeTimestamps(qint64 fromMs, qint64 toMs, int count = 3);

    /* Score decoded candidates against the two harvested poses (temporal
     * midpoint preference, distinctness from both, sharpness). Returns the
     * index of the best candidate or -1. Deterministic. */
    static int scoreBridgeCandidates(QVector<BridgeCandidate> &cands,
                                     const HarvestItem &a, const HarvestItem &b,
                                     qint64 fromMs, qint64 toMs);

    /* The positive seed point (frame coords) for segmenting a candidate at
     * `ptsMs`: the A->B centroid interpolated in time. */
    static QPointF bridgeSeedPoint(const HarvestItem &a, const HarvestItem &b,
                                   qint64 ptsMs);

    /* Insert a derived Transition pose between A and B: adds the harvest item
     * to the tray, inserts the layer behind both, sizes it to the neighbours,
     * assigns the Transition role + motion finish. Returns the new layer id. */
    static int insertBridgePose(BoostTransaction &tx, const BoostAnalysis &a,
                                const BoostOptions &o, HarvestItem derived);

    /* Living Harvest hidden tool (animateHero): a subtle living loop on the
     * HERO layer only — ghosts and transitions stay static. Rules:
     *  - at most ONE living layer per poster (existing living layer wins);
     *  - the hero must derive from a harvest with a riggable tray track;
     *  - the asset is created on demand (rig once + auto motion window) and
     *    joins the same undo step as the rest of the Boost.
     * The effect amplitude is the asset's photo-safe default (0.65). */
    static void animateHero(BoostTransaction &tx, const BoostAnalysis &a);

    /* One concise, confident sentence set (spec §44). */
    static QString composeSummary(const QStringList &lines);
};

} // namespace paintbrush

#endif // PB_BOOST_HPP

/*****************************************************************************
 * pb_living.hpp : NAI Paintbrush — Living Harvest: puppet asset store
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_LIVING_HPP
#define PB_LIVING_HPP

/* Living Harvest (Stage 3 product layer over the Motion Toys runtime):
 *
 *   HarvestItem (immutable bitmap)
 *     -> PuppetAssetStore::makeFromHarvest (rig once + auto source-motion
 *        window + photo-regime adaptation)
 *     -> layer references { assetId, PuppetEffect }  (one typed undo)
 *     -> DocumentRenderer::renderAtTime(t) renders the living frame
 *
 * The store is document-scoped and additive: assets are derived data that
 * rebuild deterministically from the tray harvests (manifest = ids + clip +
 * settings, no pixels). This file is the ONLY place that touches both the
 * VLC-free puppet runtime and the HarvestItem type.
 */

#include "../pb_harvest.hpp"
#include "pb_puppet.hpp"

#include <QByteArray>
#include <QString>
#include <QVector>
#include <functional>

namespace paintbrush {

/* Living Harvest factory: rig the harvest once, scan its track for the best
 * source-motion window, retarget with photo-regime adaptation. Falls back
 * to a procedural subtle sway when the track has no usable window. The
 * HarvestItem bitmap is never modified. */
motion::PuppetAsset makeLivingAsset(const HarvestItem &harvest,
                            const QVector<motion::PuppetMotionScanner::FrameSample> &samples,
                            const motion::PuppetMotionScanner::ScanOptions &opts
                                = motion::PuppetMotionScanner::ScanOptions());

/* Document-scoped store of rigged living assets (plain value type; the
 * document owns one and re-emits its own signals). */
class PuppetAssetStore
{
public:
    /* Rebuilds a rig from a harvest id (used on project load). */
    using RigBuilder = std::function<motion::PuppetRig(int harvestId)>;

    explicit PuppetAssetStore(RigBuilder builder = {});
    void setRigBuilder(RigBuilder builder) { m_builder = std::move(builder); }

    int add(motion::PuppetAsset asset);  /* assigns the id when 0 */
    bool remove(int id);
    void clear();
    int count() const { return m_assets.size(); }
    int nextId() const { return m_nextId; }

    const QVector<motion::PuppetAsset> &assets() const { return m_assets; }
    const motion::PuppetAsset *byId(int id) const;
    const motion::PuppetAsset *assetByHarvest(int harvestId) const;
    int assetIdByHarvest(int harvestId) const;

    /* Manifest persistence: ids + clip + settings only; rigs are rebuilt
     * through the rig builder, so a load is byte-deterministic for the
     * same tray. Assets whose harvest cannot be rebuilt are dropped. */
    QByteArray serializeManifest() const;
    bool loadManifest(const QByteArray &data);

private:
    RigBuilder m_builder;
    QVector<motion::PuppetAsset> m_assets;
    int m_nextId = 1;
};

} // namespace paintbrush

#endif // PB_LIVING_HPP
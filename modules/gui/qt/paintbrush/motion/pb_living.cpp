/*****************************************************************************
 * pb_living.cpp : NAI Paintbrush — Living Harvest: puppet asset store
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_living.hpp"

#include <cstring>

namespace paintbrush {

motion::PuppetAsset makeLivingAsset(const HarvestItem &harvest,
                            const QVector<motion::PuppetMotionScanner::FrameSample> &samples,
                            const motion::PuppetMotionScanner::ScanOptions &opts)
{
    motion::PuppetAsset asset;
    if (!harvest.isValid())
        return asset;
    asset.rig = motion::PuppetRuntime::build(harvest.cutout, harvest.mask);
    if (!asset.rig.isValid())
        return asset;

    /* harvest skeleton = fit of the harvested cutout mask (same space as
     * the tray sample masks — angles are translation/scale invariant) */
    const motion::PuppetSkeleton harvestSkeleton = motion::PuppetBuilder::fitDefaultHumanoid(harvest.mask);
    if (!harvestSkeleton.isValid())
        return asset;

    const motion::PuppetMotionScanner::ScanResult result =
        motion::PuppetMotionScanner::scan(samples, harvest.sourceFrame.ptsMs, harvestSkeleton, opts);
    if (result.ok && result.clip.keys.size() >= 2) {
        asset.motion = motion::PuppetRetargeter::retargetClip(
            result.sourceSkeleton, asset.rig.skeleton, result.clip,
            motion::PuppetRetargeter::Options());
    } else {
        /* no usable source window: a subtle procedural sway (photo-safe) */
        asset.motion = motion::PuppetMotionScanner::proceduralFallback(asset.rig);
    }
    if (asset.motion.keys.size() < 2)
        return asset;

asset.sourceHarvestId = harvest.id;
    asset.regime = motion::PuppetRegime::Photo;  /* photo-safe by default */
    asset.defaultAmplitude = 0.65f;              /* Normal — product scale:
                                                  *   Subtle 0.35 barely alive
                                                  *   Normal 0.65 product default
                                                  *   Strong 1.00 clearly alive, still photo-safe
                                                  * (eyes-on audit 2026-10-24: keep 0.65; if the
                                                  * live GUI at 100% zoom needs >0.5 s to notice
                                                  * motion, go straight to 0.75 — never micro-tune) */
    asset.name = QStringLiteral("harvest %1 living").arg(harvest.id);
    return asset;
}

PuppetAssetStore::PuppetAssetStore(RigBuilder builder)
    : m_builder(std::move(builder))
{
}

int PuppetAssetStore::add(motion::PuppetAsset asset)
{
    if (asset.id == 0)
        asset.id = m_nextId;
    m_nextId = qMax(m_nextId, asset.id + 1);
    m_assets.append(asset);
    return asset.id;
}

bool PuppetAssetStore::remove(int id)
{
    for (int i = 0; i < m_assets.size(); ++i) {
        if (m_assets[i].id == id) {
            m_assets.removeAt(i);
            return true;
        }
    }
    return false;
}

void PuppetAssetStore::clear()
{
    m_assets.clear();
    m_nextId = 1;
}

const motion::PuppetAsset *PuppetAssetStore::byId(int id) const
{
    for (const motion::PuppetAsset &a : m_assets)
        if (a.id == id)
            return &a;
    return nullptr;
}

const motion::PuppetAsset *PuppetAssetStore::assetByHarvest(int harvestId) const
{
    for (const motion::PuppetAsset &a : m_assets)
        if (a.sourceHarvestId == harvestId)
            return &a;
    return nullptr;
}

int PuppetAssetStore::assetIdByHarvest(int harvestId) const
{
    const motion::PuppetAsset *a = assetByHarvest(harvestId);
    return a ? a->id : -1;
}

QByteArray PuppetAssetStore::serializeManifest() const
{
    QByteArray out;
    out.append(char(1));                          /* version */
    const auto u32 = [](quint32 v) {
        QByteArray b(4, char(0));
        b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
        b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
        return b;
    };
    out.append(u32(quint32(m_assets.size())));
    for (const motion::PuppetAsset &a : m_assets) {
        const QByteArray m = a.serializeManifest();
        out.append(u32(quint32(m.size())));
        out.append(m);
    }
    return out;
}

bool PuppetAssetStore::loadManifest(const QByteArray &data)
{
    if (data.size() < 5 || data[0] != char(1))
        return false;
    /* load replaces the store (project reload into the same document) */
    m_assets.clear();
    m_nextId = 1;
    const auto u32at = [&](int at) {
        quint32 v = 0;
        for (int i = 0; i < 4; ++i)
            v |= quint32(quint8(data[at + i])) << (8 * i);
        return v;
    };
    const quint32 count = u32at(1);
    if (count > 1024)
        return false;
    int at = 5;
    for (quint32 i = 0; i < count; ++i) {
        if (at + 4 > data.size())
            break;
        const quint32 len = u32at(at);
        at += 4;
        if (at + int(len) > data.size())
            break;
        const QByteArray entry = data.mid(at, int(len));
        at += int(len);
        /* the rig is rebuilt from the tray harvest; when the harvest is
         * missing the rebuilt rig is invalid and the asset is dropped
         * (layer refs are pruned afterwards) */
        const auto e32at = [&entry](int o) {
            quint32 v = 0;
            for (int i = 0; i < 4; ++i)
                v |= quint32(quint8(entry[o + i])) << (8 * i);
            return v;
        };
        const quint32 harvestId = entry.size() >= 9 ? e32at(5) : 0;
        const motion::PuppetRig rig = m_builder ? m_builder(int(harvestId)) : motion::PuppetRig();
        motion::PuppetAsset asset = motion::PuppetAsset::fromManifest(entry, rig);
        if (asset.isValid()) {
            m_assets.append(asset);
            m_nextId = qMax(m_nextId, asset.id + 1);
        }
    }
    return true;
}

} // namespace paintbrush
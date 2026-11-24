/*****************************************************************************
 * pb_stage.cpp : NAI Paintbrush — Photo Regime H / Rigged Stage runtime
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_stage.hpp"
#include "pb_document.hpp"

#include "art/materials/glitter/glitter_powder.hpp"
#include "art/runtime/art_scale.hpp"
#include "art/runtime/light_rig.hpp"

#include <QDataStream>
#include <QIODevice>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace paintbrush {

namespace {

StageSample neutralSample()
{
    return StageSample();
}

QString metadataString(const CanvasLayer &layer, const QString &key)
{
    return layer.metadata.value(key).toString();
}

float metadataFloat(const CanvasLayer &layer, const QString &key, float fallback = 0.0f)
{
    return float(layer.metadata.value(key, double(fallback)).toDouble());
}

qint64 layerPeriodMs(const CanvasLayer &layer)
{
    const qint64 period = layer.metadata.value(stagekeys::loopPeriodMs(),
                                               qint64(StageRuntime::kLoopPeriodMs))
                              .toLongLong();
    return period > 0 ? period : StageRuntime::kLoopPeriodMs;
}

/* Deterministic idle automation used when the user did not record a move.
 * Same 3 s period as the Living Harvest loop; neutral at the boundary. */
StageSample autoSample(qint64 timeMs, qint64 periodMs, quint32 seed)
{
    if (periodMs <= 0 || timeMs % periodMs == 0)
        return neutralSample();
    const double t = double(((timeMs % periodMs) + periodMs) % periodMs)
                     / double(periodMs);
    const double phase = 2.0 * M_PI * t
                         + double(seed % 997) / 997.0 * 2.0 * M_PI;
    StageSample s;
    s.x = float(0.5 + 0.5 * std::sin(phase));
    s.y = float(0.5 + 0.5 * std::cos(phase * 0.83 + 0.7));
    s.energy = float(0.18 + 0.22 * (0.5 + 0.5 * std::sin(phase * 1.7)));
    s.hold = false;
    s.tMs = timeMs % periodMs;
    return s;
}

/* Derived rig cache: rig build is deterministic but relatively expensive
 * (mesh + ARAP setup). A committed creative/stage layer is rendered at 30 Hz
 * by the canvas clock, so the rig is built once per layer content. */
struct RigCache
{
    QMutex mutex;
    QHash<QString, motion::PuppetRig> rigs;
};

RigCache &rigCache()
{
    static RigCache cache;
    return cache;
}

constexpr int kMaxRigSide = 1024;

QString rigCacheKey(const CanvasLayer &layer)
{
    return QStringLiteral("%1:%2:%3")
        .arg(layer.raster.cacheKey())
        .arg(layer.mask.isNull() ? qint64(0) : layer.mask.cacheKey())
        .arg(kMaxRigSide);
}

QImage scaledForRig(const QImage &src)
{
    if (src.isNull())
        return QImage();
    const int side = qMax(src.width(), src.height());
    if (side <= kMaxRigSide)
        return src;
    const qreal f = qreal(kMaxRigSide) / qreal(side);
    return src.scaled(qMax(1, int(std::lround(src.width() * f))),
                      qMax(1, int(std::lround(src.height() * f))),
                      Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace

/* ------------------------------------------------------------- envelope */

QByteArray StageEnvelope::serialize() const
{
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);
    ds << quint8(1);
    ds << quint32(samples.size());
    ds << qint64(periodMs);
    for (const StageSample &s : samples)
    {
        ds << float(s.x) << float(s.y) << float(s.energy);
        ds << quint8(s.hold ? 1 : 0);
        ds << qint64(s.tMs);
    }
    return out;
}

StageEnvelope StageEnvelope::deserialize(const QByteArray &data)
{
    StageEnvelope env;
    if (data.isEmpty())
        return env;
    QDataStream ds(data);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);
    quint8 version = 0;
    quint32 count = 0;
    qint64 period = 0;
    ds >> version >> count >> period;
    if (version != 1 || count > 100000 || period <= 0)
        return env;
    env.periodMs = period;
    env.samples.reserve(int(count));
    for (quint32 i = 0; i < count && !ds.atEnd(); ++i)
    {
        float x = 0.5f, y = 0.5f, energy = 0.0f;
        quint8 hold = 0;
        qint64 tMs = 0;
        ds >> x >> y >> energy >> hold >> tMs;
        StageSample s;
        s.x = qBound(0.0f, x, 1.0f);
        s.y = qBound(0.0f, y, 1.0f);
        s.energy = qBound(0.0f, energy, 1.0f);
        s.hold = hold != 0;
        s.tMs = tMs;
        env.samples.append(s);
    }
    if (env.samples.size() < 2)
        env.samples.clear();
    return env;
}

StageEnvelope StageEnvelope::fromSamples(const QVector<StageSample> &samples,
                                         qint64 periodMs)
{
    StageEnvelope env;
    env.periodMs = periodMs > 0 ? periodMs : StageRuntime::kLoopPeriodMs;
    env.samples = samples;
    if (!env.samples.isEmpty())
    {
        std::sort(env.samples.begin(), env.samples.end(),
                  [](const StageSample &a, const StageSample &b) {
                      return a.tMs < b.tMs;
                  });
        /* A recording may be shorter or longer than the 3 s product loop;
         * normalize its whole gesture onto one loop instead of clipping the
         * tail (which would collapse every later sample onto the boundary). */
        const qint64 recorded = env.samples.last().tMs;
        const double scale = recorded > 0
                                 ? double(env.periodMs) / double(recorded)
                                 : 1.0;
        for (StageSample &s : env.samples)
        {
            s.x = qBound(0.0f, s.x, 1.0f);
            s.y = qBound(0.0f, s.y, 1.0f);
            s.energy = qBound(0.0f, s.energy, 1.0f);
            s.tMs = qBound<qint64>(0, qint64(std::llround(double(s.tMs) * scale)),
                                   env.periodMs);
        }
    }
    if (env.samples.size() < 2)
        env.samples.clear();
    return env;
}

StageSample StageEnvelope::sampleAt(qint64 timeMs) const
{
    if (!isValid())
        return neutralSample();
    const qint64 wrapped = ((timeMs % periodMs) + periodMs) % periodMs;
    if (wrapped == 0)
        return neutralSample();
    int i = 1;
    while (i < samples.size() && samples.at(i).tMs < wrapped)
        ++i;
    const StageSample &a = samples.at(qMax(0, i - 1));
    const StageSample &b = samples.at(qMin(samples.size() - 1, i));
    const qint64 span = qMax<qint64>(1, b.tMs - a.tMs);
    const float f = float(double(qBound<qint64>(0, wrapped - a.tMs, span))
                          / double(span));
    StageSample s;
    s.x = a.x + (b.x - a.x) * f;
    s.y = a.y + (b.y - a.y) * f;
    s.energy = a.energy + (b.energy - a.energy) * f;
    s.hold = f < 0.5f ? a.hold : b.hold;
    s.tMs = wrapped;

    /* Short ease at both loop edges so a recorded gesture never snaps against
     * the exact static t=0 frame. */
    constexpr qint64 kFadeMs = 120;
    const qint64 toEnd = periodMs - wrapped;
    float gain = 1.0f;
    if (wrapped < kFadeMs)
        gain = float(wrapped) / float(kFadeMs);
    else if (toEnd < kFadeMs)
        gain = float(toEnd) / float(kFadeMs);
    if (gain < 1.0f)
    {
        s.x = 0.5f + (s.x - 0.5f) * gain;
        s.y = 0.5f + (s.y - 0.5f) * gain;
        s.energy *= gain;
    }
    return s;
}

/* ------------------------------------------------------------ stage runtime */

bool StageRuntime::isDerivedLiving(const CanvasLayer &layer)
{
    return layer.puppetAssetId <= 0
        && layer.metadata.value(stagekeys::living()).toBool()
        && layer.puppetEffect.amplitude > 0.01f;
}

bool StageRuntime::isStageAnimated(const CanvasLayer &layer)
{
    if (!layer.metadata.contains(stagekeys::scene()))
        return false;
    return metadataFloat(layer, stagekeys::parallax()) > 0.0f
        || metadataFloat(layer, stagekeys::lightPulse()) > 0.0f;
}

motion::PuppetMotionClip StageRuntime::storedClip(const CanvasLayer &layer)
{
    const QString b64 = metadataString(layer, stagekeys::motionClip());
    if (b64.isEmpty())
        return motion::PuppetMotionClip();
    return motion::PuppetMotionClip::deserialize(
        QByteArray::fromBase64(b64.toLatin1()));
}

QByteArray StageRuntime::encodeClip(const motion::PuppetMotionClip &clip)
{
    return clip.serialize().toBase64();
}

motion::PuppetRig StageRuntime::rigForLayer(const CanvasLayer &layer)
{
    if (layer.raster.isNull())
        return motion::PuppetRig();
    const QString key = rigCacheKey(layer);
    RigCache &cache = rigCache();
    {
        QMutexLocker lock(&cache.mutex);
        const auto it = cache.rigs.constFind(key);
        if (it != cache.rigs.constEnd())
            return it.value();
    }

    const QImage texture = scaledForRig(
        layer.raster.convertToFormat(QImage::Format_ARGB32));
    QImage mask;
    if (!layer.mask.isNull())
    {
        mask = scaledForRig(layer.mask).scaled(texture.size(),
                                               Qt::IgnoreAspectRatio,
                                               Qt::FastTransformation);
    }
    motion::PuppetRig rig = motion::PuppetRuntime::build(texture, mask);

    if (rig.isValid())
    {
        QMutexLocker lock(&cache.mutex);
        if (cache.rigs.size() > 12)
            cache.rigs.clear();
        cache.rigs.insert(key, rig);
    }
    return rig;
}

StageEnvelope StageRuntime::envelopeForLayer(const CanvasLayer &layer)
{
    const QString b64 = metadataString(layer, stagekeys::envelope());
    if (b64.isEmpty())
        return StageEnvelope();
    return StageEnvelope::deserialize(QByteArray::fromBase64(b64.toLatin1()));
}

motion::PuppetMotionClip StageRuntime::clipForLayer(const CanvasLayer &layer,
                                                    const motion::PuppetRig &rig)
{
    motion::PuppetMotionClip clip = storedClip(layer);
    if (clip.isValid(rig.skeleton))
    {
        clip.loop = motion::LoopMode::Loop;
        return clip;
    }
    const quint32 seed = quint32(quint64(layer.id) * 2654435761u) | 11u;
    return motion::PuppetMotionScanner::proceduralFallback(
        rig, quint32(layerPeriodMs(layer)), seed);
}

motion::PuppetEffect StageRuntime::effectAtTime(const CanvasLayer &layer,
                                                qint64 timeMs)
{
    motion::PuppetEffect fx = layer.puppetEffect;
    fx.loop = motion::LoopMode::Loop;
    fx.durationMs = quint32(layerPeriodMs(layer));
    if (fx.amplitude <= 0.0f)
        return fx;

    const StageEnvelope env = envelopeForLayer(layer);
    StageSample control = env.isValid()
                              ? env.sampleAt(timeMs)
                              : autoSample(timeMs, layerPeriodMs(layer),
                                           quint32(layer.id) | 1u);
    /* Play Pad energy is the material/motion intensity lane: the product
     * amount (Subtle .35 / Normal .65 / Strong 1.0) is the floor and a
     * gesture can add at most +12% (clamped at the photo-safe 1.0 ceiling). */
    float amplitude = fx.amplitude * (1.0f + 0.12f * control.energy);
    if (control.hold)
        amplitude *= 1.05f;
    fx.amplitude = qBound(0.0f, amplitude, 1.0f);
    fx.speed = 1.0f; /* derived frames map 3 s -> one clip loop below */
    fx.phase = 0.0f;
    return fx;
}

QImage StageRuntime::renderDerivedFrame(const CanvasLayer &layer, qint64 timeMs)
{
    if (!isDerivedLiving(layer) || layer.raster.isNull())
        return QImage();
    /* exact static pixel path at t=0 / loop boundary: the committed raster is
     * returned untouched, so the still look and undo state are exact. */
    const qint64 period = layerPeriodMs(layer);
    if (((timeMs % period) + period) % period == 0)
        return layer.raster;
    const motion::PuppetRig rig = rigForLayer(layer);
    if (!rig.isValid())
        return QImage();
    const motion::PuppetMotionClip clip = clipForLayer(layer, rig);
    if (clip.keys.size() < 2)
        return QImage();

    motion::PuppetEffect fx = effectAtTime(layer, timeMs);
    const qint64 clipDuration = clip.durationMs();
    if (clipDuration > 0)
        fx.speed = float(double(clipDuration) / double(qMax<qint64>(1, period)));

    QImage frame = motion::PuppetRuntime::renderClipFrame(rig, clip, fx, timeMs);
    if (frame.isNull())
        return frame;
    if (frame.size() != layer.raster.size())
        frame = frame.scaled(layer.raster.size(), Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
    return frame;
}

QPointF StageRuntime::parallaxOffset(const CanvasLayer &layer,
                                     const QSize &canvas, qint64 timeMs)
{
    if (!isStageAnimated(layer) || canvas.isEmpty())
        return QPointF();
    const qint64 period = layerPeriodMs(layer);
    const qint64 wrapped = ((timeMs % period) + period) % period;
    if (wrapped == 0)
        return QPointF(); /* static/rest frame contract */

    const float depth = metadataFloat(layer, stagekeys::depth());
    const float strength = metadataFloat(layer, stagekeys::parallax());
    if (qFuzzyIsNull(depth) || strength <= 0.0f)
        return QPointF();

    const StageEnvelope env = envelopeForLayer(layer);
    const StageSample control = env.isValid()
                                    ? env.sampleAt(wrapped)
                                    : autoSample(wrapped, period,
                                                 quint32(layer.id) | 1u);
    qreal dx = (double(control.x) - 0.5) * 2.0 * strength * depth
               * canvas.width() * 0.012;
    qreal dy = (double(control.y) - 0.5) * 2.0 * strength * depth
               * canvas.height() * 0.008;

    if (!env.isValid())
    {
        const double phase = 2.0 * M_PI * double(wrapped) / double(period);
        dx += std::sin(phase + depth * 1.9) * canvas.width() * 0.006
              * strength * depth;
        dy += std::cos(phase * 0.91 + depth * 2.3) * canvas.height() * 0.004
              * strength * depth;
    }
    return QPointF(dx, dy);
}

float StageRuntime::lightGain(const CanvasLayer &layer, qint64 timeMs)
{
    const float pulse = metadataFloat(layer, stagekeys::lightPulse());
    if (pulse <= 0.0f)
        return 1.0f;
    const qint64 period = layerPeriodMs(layer);
    const qint64 wrapped = ((timeMs % period) + period) % period;
    if (wrapped == 0)
        return 1.0f;
    const StageEnvelope env = envelopeForLayer(layer);
    const StageSample control = env.isValid()
                                    ? env.sampleAt(wrapped)
                                    : autoSample(wrapped, period,
                                                 quint32(layer.id) | 1u);
    return qBound(0.6f, 1.0f + pulse * 0.22f * (control.y - 0.5f) * 2.0f,
                  1.5f);
}

void StageRuntime::applyStageMetadata(CanvasLayer *layer, const QString &scene,
                                      const QString &role, float depth,
                                      float parallax, float lightPulse,
                                      const QByteArray &envelope)
{
    if (!layer)
        return;
    layer->metadata[stagekeys::scene()] = scene;
    layer->metadata[stagekeys::role()] = role;
    layer->metadata[stagekeys::depth()] = double(depth);
    layer->metadata[stagekeys::parallax()] = double(parallax);
    layer->metadata[stagekeys::lightPulse()] = double(lightPulse);
    /* Motion automation is always on; a recorded envelope replaces the
     * deterministic idle curve when present. */
    layer->metadata[stagekeys::autoMotion()] = true;
    layer->metadata[stagekeys::loopPeriodMs()] = qint64(kLoopPeriodMs);
    if (!envelope.isEmpty())
        layer->metadata[stagekeys::envelope()] =
            QString::fromLatin1(envelope.toBase64());
}

void StageRuntime::clearLivingMetadata(CanvasLayer *layer)
{
    if (!layer)
        return;
    layer->metadata.remove(stagekeys::living());
    layer->metadata.remove(stagekeys::motionClip());
    layer->metadata.remove(stagekeys::envelope());
}

QImage StageRuntime::renderPowderField(const QSize &stageSize, quint32 seed,
                                       const QColor &color,
                                       QByteArray *stateOut)
{
    if (stateOut)
        stateOut->clear();
    if (!stageSize.isValid() || stageSize.isEmpty())
        return QImage();

    const float cell = qMax(8.0f, float(qMin(stageSize.width(),
                                             stageSize.height())) / 72.0f);
    QSize cells(qMax(1, int(std::ceil(stageSize.width() / cell))),
                qMax(1, int(std::ceil(stageSize.height() / cell))));
    cells = cells.boundedTo(QSize(256, 256));

    art::glitter::GlitterPowder powder;
    powder.reset(cells, cell, seed | 1u);

    art::GlitterParams params;
    params.seed = seed | 1u;
    params.color = color;
    params.amount = 0.9f;
    params.flakeSize = 1.1f;
    params.shimmer = 0.65f;
    params.adhesion = 0.85f;
    params.looseMix = 0.25f;
    params.holographic = 0.45f;
    params.dispersion = 0.55f;
    params.palette = { color.lighter(120), color, QColor(255, 255, 255) };

    const float w = float(stageSize.width());
    const float h = float(stageSize.height());
    powder.paintGlue(w * 0.50f, h * 0.94f, w * 0.34f, 0.85f);
    powder.paintGlue(w * 0.12f, h * 0.20f, w * 0.10f, 0.35f);
    powder.paintGlue(w * 0.88f, h * 0.16f, w * 0.08f, 0.30f);
    powder.pourGlitterMain(true, params);
    powder.absorb();
    powder.relax();

    if (stateOut)
        *stateOut = powder.serialize();

    art::glitter::PowderPalette palette;
    palette.colors = params.palette;
    palette.holographic = 0.55f;
    palette.paper = QColor(255, 255, 255);
    art::ArtLightRig rig;
    art::ArtScale scale;
    return powder.render(stageSize, 0.0f, 0.0f, 1.0f, rig, palette, nullptr,
                         scale, 0)
        .convertToFormat(QImage::Format_ARGB32);
}

/* ------------------------------------------------- typed derived undo */

void SetStageLivingCommand::apply(PaintbrushDocument &doc)
{
    if (CanvasLayer *l = doc.layerById(layerId))
    {
        l->puppetAssetId = afterAssetId;
        l->puppetEffect = afterEffect;
        l->metadata = afterMetadata;
        doc.setDirty(true);
        emit doc.layersChanged();
    }
}

void SetStageLivingCommand::revert(PaintbrushDocument &doc)
{
    if (CanvasLayer *l = doc.layerById(layerId))
    {
        l->puppetAssetId = beforeAssetId;
        l->puppetEffect = beforeEffect;
        l->metadata = beforeMetadata;
        doc.setDirty(true);
        emit doc.layersChanged();
    }
}

} // namespace paintbrush

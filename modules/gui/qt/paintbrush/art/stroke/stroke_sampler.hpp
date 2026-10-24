/*****************************************************************************
 * stroke_sampler.hpp : NAI Paintbrush — stroke sample normalization + smoothing
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_STROKE_SAMPLER_HPP
#define PB_ART_STROKE_SAMPLER_HPP

#include <QPointF>
#include <QQueue>
#include <QVector>
#include <QtGlobal>
#include <cmath>

namespace paintbrush {
namespace art {

/* A single normalized pointer sample (spec §13.1). Coordinates are in a stable
 * reference space (canvas-local world units), with a raw pressure (0..1), a
 * tilt (0..1) and a time stamp for velocity estimation. */
struct StrokeSample
{
    QPointF pos;
    float pressure = 1.0f;
    float tilt = 0.0f;
    float timeSec = 0.0f;   /* wall time since stroke start */
    bool synthetic = false; /* true when inserted by the interpolator */
};

/* Temporal smoothing + optional stabilizer (spec §13: "temporal smoothing",
 * "optional stabilizer"). The stabilizer is a low-pass follow filter: instead
 * of snapping the brush to the hardware point it moves toward it at a rate
 * proportional to speed, which removes hand jitter while still tracking fast
 * strokes. strokeSmoothing in [0,1] blends toward the stabilized point; 0 =
 * nothing, 1 = heavy lag. */
class StrokeStabilizer
{
public:
    StrokeStabilizer() = default;

    void begin(const QPointF &p, float smoothing)
    {
        m_smoothing = smoothing;
        m_target = p;
        m_stabilized = p;
    }

    /* Feed a raw point; returns the smoothed position to draw at. */
    QPointF process(const QPointF &raw)
    {
        m_target = raw;
        if (m_smoothing <= 0.0f)
        {
            m_stabilized = raw;
            return raw;
        }
        /* Exponential follow; stronger smoothing -> slower catch-up. */
        const float dt = 0.05f;             /* assumed 20 Hz sample cadence */
        const float k = 1.0f - std::exp(-m_smoothing * 12.0f * dt);
        m_stabilized = m_stabilized + (m_target - m_stabilized) * k;
        return m_stabilized;
    }

    QPointF last() const { return m_stabilized; }

private:
    QPointF m_target;
    QPointF m_stabilized;
    float m_smoothing = 0.0f;
};

/* Velocity estimation: exponential moving average of per-segment speed. The
 * velocity drives pressure/deposition modulation (fast strokes deposit less),
 * spacing (fast strokes are spaced out), and direction. */
class StrokeVelocity
{
public:
    void reset(const QPointF &p, float timeSec)
    {
        m_last = p;
        m_lastTime = timeSec;
        m_vel = 0.0f;
        m_dir = QPointF(1.0f, 0.0f);
        m_sum = 0.0f;
    }

    /* Feed one segment; updates running velocity (world units/second) and the
     * normalised stroke direction. */
    void update(const QPointF &p, float timeSec)
    {
        const float dt = std::max(1e-4f, timeSec - m_lastTime);
        const QPointF delta = p - m_last;
        const float dist = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
        const float inst = dist / dt;
        m_vel = m_vel * 0.75f + inst * 0.25f;   /* EMA */
        if (dist > 1e-5f)
            m_dir = delta / dist;
        /* accumulate distance for taper decisions */
        m_accumDist += dist;
        m_dirAccum.push_back(m_dir);
        if (m_dirAccum.size() > 16)
            m_dirAccum.removeFirst();
        m_last = p;
        m_lastTime = timeSec;
        (void)m_sum;
    }

    float velocity() const { return m_vel; }
    QPointF direction() const { return m_dir; }
    float accumulatedDist() const { return m_accumDist; }
    /* Smooth average direction over the recent window. */
    QPointF smoothedDirection() const
    {
        if (m_dirAccum.isEmpty())
            return m_dir;
        QPointF sum;
        for (const QPointF &d : m_dirAccum)
            sum += d;
        const float len = std::sqrt(sum.x() * sum.x() + sum.y() * sum.y());
        if (len < 1e-5f)
            return m_dir;
        return sum / len;
    }

private:
    QPointF m_last;
    float m_lastTime = 0.0f;
    float m_vel = 0.0f;
    float m_accumDist = 0.0f;
    float m_sum = 0.0f;
    QPointF m_dir{ 1.0f, 0.0f };
    QVector<QPointF> m_dirAccum;
};

/* Spacing scheduler: given a wanted spacing (in world units, derived from
 * brush size) and the current point, returns how many interpolation steps (if
 * any) are needed before the next dab so there are no visible gaps at normal
 * pointer rates (spec §13.2: "no visible disconnected gaps at normal pointer
 * rate"). */
inline int spacingStepsNeeded(const QPointF &from, const QPointF &to,
                              float spacing)
{
    const QPointF delta = to - from;
    const float dist = std::sqrt(delta.x() * delta.x() + delta.y() * delta.y());
    if (spacing <= 1e-4f || dist <= spacing)
        return 0;
    int steps = int(std::floor(dist / spacing));
    return qMax(0, steps);
}

} // namespace art
} // namespace paintbrush

#endif // PB_ART_STROKE_SAMPLER_HPP
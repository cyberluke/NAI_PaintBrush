/*****************************************************************************
 * pb_creative_vector.hpp : NAI Paintbrush — N-dimensional creative vector
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_CREATIVE_VECTOR_HPP
#define PB_CREATIVE_VECTOR_HPP

/* A pure, data-driven modulation system for every creative mode.
 *
 * Design:
 *   - VectorSource is the N-dimensional controller state (pad X/Y, energy,
 *     hold, time, plus the output of a previous node).
 *   - VectorRoute is one source->destination dependency with a curve and a
 *     polarity. Routes are the mod-matrix cells.
 *   - VectorNode is a composite: a leaf of routes, or a Sequential/Parallel
 *     group of children. Sequential nodes feed each child's output into the
 *     next child (the Wavestation-style chain); parallel nodes sum independent
 *     branches (the Kaoss-style layer).
 *   - VectorGraph is a profile. Profiles are built once and selected by a
 *     factory from the patch id, so new modes add data, not engine code.
 *
 * The recipe evaluates the graph once per build and reads only the named
 * destinations it owns. No QObject, no global state, no VLC.
 */

#include <QString>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace paintbrush {
namespace creative {
namespace vector {

enum class Source
{
    PadX,
    PadY,
    PadEnergy,
    PadHold,
    PadTime,
    NodeOut,
    Count
};

enum class Dest
{
    ParticleCount,
    ParticleSpeed,
    ParticleSpread,
    ParticleHue,
    ParticleLength,
    ParticleBrightness,
    ArcCount,
    ArcBendX,
    ArcBendY,
    ArcJitter,
    Glow,
    Bloom,
    ShadowX,
    ShadowY,
    ShadowOpacity,
    Recolor,
    Sheen,
    RimLeft,
    RimRight,
    HueShift,
    OverlayAmount,
    Count
};

enum class Curve
{
    Linear,
    Smooth,
    Exponential,
    Sine,
    Square
};

struct Route
{
    Source source = Source::PadX;
    Dest dest = Dest::ParticleCount;
    float amount = 0.0f;
    Curve curve = Curve::Linear;
    bool invert = false;
};

struct Node
{
    enum class Kind { Routes, Sequential, Parallel };

    Kind kind = Kind::Routes;
    QVector<Route> routes;
    QVector<Node> children;
    float gain = 1.0f;
    QString name;
};

struct State
{
    std::array<float, size_t(Dest::Count)> v{};

    static State base()
    {
        State s;
        s.v[size_t(Dest::ParticleCount)] = 0.35f;
        s.v[size_t(Dest::ParticleSpeed)] = 0.55f;
        s.v[size_t(Dest::ParticleSpread)] = 0.55f;
        s.v[size_t(Dest::ParticleHue)] = 0.0f;
        s.v[size_t(Dest::ParticleLength)] = 0.50f;
        s.v[size_t(Dest::ParticleBrightness)] = 0.55f;
        s.v[size_t(Dest::ArcCount)] = 0.35f;
        s.v[size_t(Dest::ArcBendX)] = 0.50f;
        s.v[size_t(Dest::ArcBendY)] = 0.50f;
        s.v[size_t(Dest::ArcJitter)] = 0.40f;
        s.v[size_t(Dest::Glow)] = 0.35f;
        s.v[size_t(Dest::Bloom)] = 0.35f;
        s.v[size_t(Dest::ShadowX)] = 0.0f;
        s.v[size_t(Dest::ShadowY)] = 0.0f;
        s.v[size_t(Dest::ShadowOpacity)] = 0.15f;
        s.v[size_t(Dest::Recolor)] = 0.40f;
        s.v[size_t(Dest::Sheen)] = 0.40f;
        s.v[size_t(Dest::RimLeft)] = 0.50f;
        s.v[size_t(Dest::RimRight)] = 0.50f;
        s.v[size_t(Dest::HueShift)] = 0.0f;
        s.v[size_t(Dest::OverlayAmount)] = 1.0f;
        return s;
    }

    float &operator[](Dest d) { return v[size_t(d)]; }
    float operator[](Dest d) const { return v[size_t(d)]; }
};

struct Sources
{
    float x = 0.5f;
    float y = 0.5f;
    float energy = 0.0f;
    float hold = 0.0f;
    float time = 0.0f;
};

struct Graph
{
    Node root;
    State base = State::base();
};

inline float clamp01(float value)
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

inline float curveValue(Curve curve, float value)
{
    value = clamp01(value);
    switch (curve)
    {
    case Curve::Smooth:
        return value * value * (3.0f - 2.0f * value);
    case Curve::Exponential:
        return value * value;
    case Curve::Sine:
        return 0.5f + 0.5f * std::sin((value - 0.5f) * 3.14159265f);
    case Curve::Square:
        return value >= 0.5f ? 1.0f : 0.0f;
    case Curve::Linear:
    default:
        return value;
    }
}

inline float sourceValue(Source source, const Sources &in, float nodeOut)
{
    switch (source)
    {
    case Source::PadX: return clamp01(in.x);
    case Source::PadY: return clamp01(in.y);
    case Source::PadEnergy: return clamp01(in.energy);
    case Source::PadHold: return clamp01(in.hold);
    case Source::PadTime:
        return 0.5f + 0.5f * std::sin(in.time * 1.7f);
    case Source::NodeOut: return clamp01(nodeOut);
    case Source::Count:
    default: return 0.5f;
    }
}

inline Node routes(std::initializer_list<Route> routes, const QString &name = {})
{
    Node n;
    n.kind = Node::Kind::Routes;
    n.name = name;
    for (const Route &r : routes)
        n.routes.append(r);
    return n;
}

inline Node sequential(std::initializer_list<Node> children, const QString &name = {})
{
    Node n;
    n.kind = Node::Kind::Sequential;
    n.name = name;
    for (const Node &c : children)
        n.children.append(c);
    return n;
}

inline Node parallel(std::initializer_list<Node> children, const QString &name = {})
{
    Node n;
    n.kind = Node::Kind::Parallel;
    n.name = name;
    for (const Node &c : children)
        n.children.append(c);
    return n;
}

inline void applyRoutes(const QVector<Route> &routes, const Sources &in,
                        State &state, float nodeOut, float *out)
{
    float sum = 0.0f;
    int count = 0;
    for (const Route &r : routes)
    {
        float raw = sourceValue(r.source, in, nodeOut);
        raw = curveValue(r.curve, raw);
        if (r.invert)
            raw = 1.0f - raw;
        state[r.dest] = clamp01(state[r.dest] + raw * r.amount);
        sum += raw;
        ++count;
    }
    if (out)
        *out = count > 0 ? clamp01(sum / float(count)) : nodeOut;
}

inline void evaluateNode(const Node &node, const Sources &in, State &state,
                         float *out)
{
    float local = out ? *out : 0.5f;
    applyRoutes(node.routes, in, state, local, &local);

    switch (node.kind)
    {
    case Node::Kind::Routes:
        break;
    case Node::Kind::Sequential:
        for (const Node &child : node.children)
        {
            evaluateNode(child, in, state, &local);
            local = clamp01(local * node.gain);
        }
        break;
    case Node::Kind::Parallel:
    {
        /* Parallel branches must not re-add the shared base state once per
         * child (that would saturate every destination). Capture the entry
         * state, evaluate each child from it, and sum only the deltas. */
        const State entry = state;
        State delta;
        delta.v.fill(0.0f);
        float total = 0.0f;
        for (const Node &child : node.children)
        {
            State childState = entry;
            float childOut = local;
            evaluateNode(child, in, childState, &childOut);
            for (size_t i = 0; i < size_t(Dest::Count); ++i)
                delta.v[i] += childState.v[i] - entry.v[i];
            total += childOut;
        }
        for (size_t i = 0; i < size_t(Dest::Count); ++i)
            state.v[i] = clamp01(entry.v[i] + delta.v[i]);
        if (!node.children.isEmpty())
            local = clamp01(total / float(node.children.size()));
        break;
    }
    }

    if (out)
        *out = clamp01(local);
}

inline State evaluate(const Graph &graph, const Sources &in)
{
    State state = graph.base;
    float out = 0.5f;
    evaluateNode(graph.root, in, state, &out);
    return state;
}

/* ---------------------------------------------------------- the profiles */

inline Graph defaultGraph()
{
    const Route speed { Source::PadX, Dest::ParticleSpeed, 0.85f,
                        Curve::Smooth };
    const Route spread { Source::PadY, Dest::ParticleSpread, 0.75f,
                         Curve::Smooth };
    const Route count { Source::PadEnergy, Dest::ParticleCount, 0.85f,
                        Curve::Smooth };
    const Route bright { Source::PadHold, Dest::ParticleBrightness, 0.55f,
                         Curve::Linear };

    Graph g;
    g.root = parallel({
        sequential({ routes({ speed, spread, count, bright }, "charge"),
                     routes({
                         { Source::NodeOut, Dest::ArcCount, 0.55f,
                           Curve::Smooth },
                         { Source::PadX, Dest::ArcBendX, 0.55f, Curve::Smooth },
                         { Source::PadY, Dest::ArcBendY, 0.55f, Curve::Smooth }
                     }, "arc") }, "discharge"),
        routes({
            { Source::PadX, Dest::ShadowX, 0.55f, Curve::Smooth },
            { Source::PadY, Dest::ShadowY, 0.55f, Curve::Smooth },
            { Source::PadEnergy, Dest::ShadowOpacity, 0.30f, Curve::Linear }
        }, "persona"),
        routes({
            { Source::PadX, Dest::HueShift, 0.80f, Curve::Smooth },
            { Source::PadY, Dest::Sheen, 0.55f, Curve::Smooth },
            { Source::PadEnergy, Dest::Recolor, 0.35f, Curve::Linear },
            { Source::PadHold, Dest::Bloom, 0.55f, Curve::Smooth }
        }, "surface"),
        routes({
            { Source::PadX, Dest::RimLeft, 0.35f, Curve::Smooth },
            { Source::PadY, Dest::RimRight, 0.35f, Curve::Smooth }
        }, "rims")
    }, "default");
    return g;
}

inline Graph voltageGraph()
{
    Graph g = defaultGraph();
    g.base[Dest::ParticleCount] = 0.50f;
    g.base[Dest::ArcCount] = 0.50f;
    g.base[Dest::ParticleBrightness] = 0.65f;
    g.base[Dest::ParticleLength] = 0.65f;
    g.base[Dest::OverlayAmount] = 1.0f;
    g.root.name = "voltage";
    /* The voltage chain adds a second dependency stage: charge feeds the
     * discharge, which feeds the bolt/particle layer. It is sequential by
     * data, not a special case in the recipe. */
    g.root.children.prepend(sequential({
        routes({
            { Source::PadX, Dest::ArcJitter, 0.90f, Curve::Smooth },
            { Source::PadY, Dest::ParticleHue, 0.90f, Curve::Smooth },
            { Source::PadEnergy, Dest::Glow, 0.75f, Curve::Smooth },
            { Source::PadHold, Dest::ParticleSpread, 0.45f, Curve::Linear }
        }, "vector"),
        routes({
            { Source::NodeOut, Dest::ArcBendX, 0.85f, Curve::Smooth },
            { Source::NodeOut, Dest::ArcBendY, 0.85f, Curve::Smooth },
            { Source::NodeOut, Dest::ParticleSpeed, 0.80f, Curve::Smooth },
            { Source::NodeOut, Dest::OverlayAmount, 0.25f, Curve::Linear }
        }, "advect")
    }, "voltage-chain"));
    return g;
}

/* Factory / registry. Every mode shares the default graph; Voltage Idol
 * declares a richer profile. New profiles are data additions. */
inline const Graph &profileFor(const QString &patchId)
{
    static const Graph fallback = defaultGraph();
    static const Graph voltage = voltageGraph();
    if (patchId == QLatin1String("electric-kpop"))
        return voltage;
    return fallback;
}

inline State evaluateForPatch(const QString &patchId, const Sources &in)
{
    return evaluate(profileFor(patchId), in);
}

} /* namespace vector */
} /* namespace creative */
} /* namespace paintbrush */

#endif /* PB_CREATIVE_VECTOR_HPP */

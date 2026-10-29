/*****************************************************************************
 * pb_puppet.cpp : NAI Paintbrush — Motion Toys: 2D puppet runtime
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_puppet.hpp"

#include <QHash>
#include <QPair>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr double kPi = 3.14159265358979323846;
}

namespace paintbrush {
namespace motion {

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

namespace {

/* FNV-1a 64 — deterministic content hashing for rig identity. */
quint64 fnv1a64(const quint8 *data, qsizetype n, quint64 h = 1469598103934665603ULL)
{
    for (qsizetype i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

quint64 fnv1a64(const QByteArray &b, quint64 h = 1469598103934665603ULL)
{
    return fnv1a64(reinterpret_cast<const quint8 *>(b.constData()), b.size(), h);
}

void hashPoint(quint64 &h, QPointF p, double quant)
{
    const qint64 x = llround(p.x() * quant);
    const qint64 y = llround(p.y() * quant);
    h = fnv1a64(reinterpret_cast<const quint8 *>(&x), sizeof(x), h);
    h = fnv1a64(reinterpret_cast<const quint8 *>(&y), sizeof(y), h);
}

float cross2(QPointF a, QPointF b, QPointF c)
{
    return float((b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x()));
}

bool pointInTriangle(QPointF p, QPointF a, QPointF b, QPointF c)
{
    const float d1 = cross2(a, b, p);
    const float d2 = cross2(b, c, p);
    const float d3 = cross2(c, a, p);
    const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(hasNeg && hasPos);
}

/* Binary occupancy from a Grayscale8 mask (values > 127 = inside). */
QVector<quint8> occupancyFromMask(const QImage &mask)
{
    QVector<quint8> occ(mask.width() * mask.height(), 0);
    if (mask.isNull())
        return occ;
    for (int y = 0; y < mask.height(); ++y) {
        const quint8 *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            occ[qsizetype(y) * mask.width() + x] = row[x] > 127 ? 1 : 0;
    }
    return occ;
}

/* Fill interior holes: everything not connected to the border becomes 1. */
QVector<quint8> fillHoles(const QVector<quint8> &occ, int w, int h)
{
    if (w <= 0 || h <= 0)
        return occ;
    QVector<quint8> work = occ;                 /* 0 bg, 1 occupied */
    QVector<int> queue;
    queue.reserve(qMax(16, w * h / 4));
    const auto push = [&](int x, int y) {
        const qsizetype i = qsizetype(y) * w + x;
        if (work[i] == 0) {
            work[i] = 2;                        /* background, reachable */
            queue.append(int(i));
        }
    };
    for (int x = 0; x < w; ++x) { push(x, 0); push(x, h - 1); }
    for (int y = 0; y < h; ++y) { push(0, y); push(w - 1, y); }
    for (qsizetype head = 0; head < queue.size(); ++head) {
        const int i = queue[head];
        const int x = i % w, y = i / w;
        if (x > 0)     push(x - 1, y);
        if (x + 1 < w) push(x + 1, y);
        if (y > 0)     push(x, y - 1);
        if (y + 1 < h) push(x, y + 1);
    }
    for (qsizetype i = 0; i < work.size(); ++i)
        work[i] = (work[i] == 2) ? 0 : 1;
    return work;
}

/* Remove disconnected speckle islands (segmentation noise): any component
 * smaller than `minArea` px is dropped. Deterministic 4-connected BFS. */
QVector<quint8> removeSpeckles(const QVector<quint8> &occ, int w, int h,
                               qint64 minArea)
{
    QVector<quint8> out(w * h, 0);
    QVector<quint8> seen = occ;
    QVector<int> stack;
    for (qsizetype s = 0; s < occ.size(); ++s) {
        if (!seen[s])
            continue;
        /* flood this component */
        stack.clear();
        stack.append(int(s));
        seen[s] = 0;
        QVector<int> cells;
        qint64 area = 0;
        while (!stack.isEmpty()) {
            const int i = stack.takeLast();
            cells.append(i);
            ++area;
            const int x = i % w, y = i / w;
            if (x > 0 && seen[i - 1])     { seen[i - 1] = 0; stack.append(i - 1); }
            if (x + 1 < w && seen[i + 1]) { seen[i + 1] = 0; stack.append(i + 1); }
            if (y > 0 && seen[i - w])     { seen[i - w] = 0; stack.append(i - w); }
            if (y + 1 < h && seen[i + w]) { seen[i + w] = 0; stack.append(i + w); }
        }
        if (area >= minArea)
            for (int i : cells)
                out[i] = 1;
    }
    return out;
}

/* Boundary extraction via corner cuts on the pixel grid.
 *
 * Model: the polygon vertices are the MIDPOINTS of boundary edges (grid
 * edges between an occupied and an empty pixel). Around every grid corner
 * the incident boundary-edge midpoints are connected by a polygon segment
 * (the "corner cut"):
 *   - 2 boundary edges  -> one cut connecting their midpoints;
 *   - 4 boundary edges  -> diagonal pinch (NW+SE or NE+SW occupied): two
 *     cuts, paired so the 8-connected region stays on one side
 *     (NW+SE -> (N,E)+(S,W); NE+SW -> (N,W)+(S,E)).
 * Every midpoint receives exactly two cuts (one per endpoint corner of its
 * edge), so the loops assemble unambiguously and the polygon is exactly
 * the union boundary of the occupied pixel squares: every occupied pixel
 * center lies strictly inside. The mask is hole-filled before this runs;
 * the LARGEST loop is returned (the outer boundary). */
QVector<QPointF> traceContour(const QVector<quint8> &occ, int w, int h)
{
    struct Seg { qint64 a; qint64 b; bool used = false; };
    QVector<Seg> segs;
    QHash<qint64, QVector<int>> at;      /* endpoint key -> segments */
    const auto addSeg = [&](qint64 ka, qint64 kb) {
        const int id = segs.size();
        segs.append({ ka, kb });
        at[ka].append(id);
        at[kb].append(id);
    };
    const auto quad = [&](int cx, int cy, int dx, int dy) {
        const int x = cx + dx, y = cy + dy;
        return (x >= 0 && y >= 0 && x < w && y < h)
            ? occ[qsizetype(y) * w + x] : 0;
    };
    const auto midKey = [](int cx, int cy, int edge) -> qint64 {
        switch (edge) {                      /* key = (2*px, 2*py) */
        case 0: return (qint64(2 * cx) << 32) | quint32(2 * cy - 1);      /* N */
        case 1: return (qint64(2 * cx + 1) << 32) | quint32(2 * cy);      /* E */
        case 2: return (qint64(2 * cx) << 32) | quint32(2 * cy + 1);      /* S */
        default: return (qint64(2 * cx - 1) << 32) | quint32(2 * cy);     /* W */
        }
    };
    for (int cy = 0; cy <= h; ++cy) {
        for (int cx = 0; cx <= w; ++cx) {
            const int nw = quad(cx, cy, -1, -1), ne = quad(cx, cy, 0, -1);
            const int sw = quad(cx, cy, -1, 0), se = quad(cx, cy, 0, 0);
            const bool bN = nw != ne, bE = ne != se, bS = se != sw, bW = sw != nw;
            const int count = int(bN) + int(bE) + int(bS) + int(bW);
            if (count == 0)
                continue;
            const qint64 kN = midKey(cx, cy, 0), kE = midKey(cx, cy, 1);
            const qint64 kS = midKey(cx, cy, 2), kW = midKey(cx, cy, 3);
            if (count == 2) {
                /* generic pairing: 1 or 3 occupied quadrants -> the two
                 * boundary edges share a corner; 2 ADJACENT occupied -> the
                 * boundary runs along the opposite edges (straight cut) */
                const bool b[4] = { bN, bE, bS, bW };
                const qint64 k[4] = { kN, kE, kS, kW };
                int e1 = -1, e2 = -1;
                for (int e = 0; e < 4; ++e) {
                    if (b[e]) { if (e1 < 0) e1 = e; else e2 = e; }
                }
                addSeg(k[e1], k[e2]);
            } else if (count == 4) {
                if (nw && se) {              /* NW+SE occupied */
                    addSeg(kN, kE);
                    addSeg(kS, kW);
                } else {                     /* NE+SW occupied */
                    addSeg(kN, kW);
                    addSeg(kS, kE);
                }
            }
        }
    }
    if (segs.isEmpty())
        return QVector<QPointF>();

    /* greedy loop assembly (largest loop wins; every midpoint has exactly
     * two incident segments, so the walk is unambiguous) */
    QVector<QPointF> best;
    for (int start = 0; start < segs.size(); ++start) {
        if (segs[start].used)
            continue;
        QVector<qint64> loopKeys;
        qint64 cur = segs[start].a;
        int segId = start;
        int guard = segs.size() * 2 + 16;
        while (guard-- > 0) {
            if (segs[segId].used)
                break;
            segs[segId].used = true;
            loopKeys.append(cur);
            const qint64 other = segs[segId].a == cur ? segs[segId].b : segs[segId].a;
            int nextId = -1;
            const QVector<int> &inc = at[other];
            for (int i = 0; i < inc.size(); ++i) {
                if (inc[i] != segId && !segs[inc[i]].used) { nextId = inc[i]; break; }
            }
            if (nextId < 0)
                break;                          /* loop closed or dead end */
            cur = other;
            segId = nextId;
        }
        if (loopKeys.size() > best.size()) {
            best.clear();
            for (qint64 k : loopKeys) {
                const int kx = int(k >> 32), ky = int(quint32(k));
                best.append(QPointF(kx * 0.5, ky * 0.5));
            }
        }
    }
    return best;
}

/* Ramer-Douglas-Peucker on a closed polyline. */
QVector<QPointF> simplifyPolyline(const QVector<QPointF> &pts, double epsilon)
{
    if (pts.size() < 3)
        return pts;
    QVector<QPointF> ring = pts;
    ring.append(pts.first());
    const int n = ring.size();
    QVector<quint8> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    QVector<QPair<int, int>> stack;
    stack.append(qMakePair(0, n - 1));
    while (!stack.isEmpty()) {
        const auto seg = stack.takeLast();
        const int s = seg.first, e = seg.second;
        if (e <= s + 1)
            continue;
        double dmax = -1.0;
        int imax = -1;
        const QPointF a = ring[s], b = ring[e];
        for (int i = s + 1; i < e; ++i) {
            const QPointF p = ring[i];
            const double len = hypot(b.x() - a.x(), b.y() - a.y());
            double d;
            if (len < 1e-12) {
                d = hypot(p.x() - a.x(), p.y() - a.y());
            } else {
                d = std::abs((b.x() - a.x()) * (a.y() - p.y())
                             - (a.x() - p.x()) * (b.y() - a.y())) / len;
            }
            if (d > dmax) { dmax = d; imax = i; }
        }
        if (dmax > epsilon && imax >= 0) {
            keep[imax] = 1;
            stack.append(qMakePair(s, imax));
            stack.append(qMakePair(imax, e));
        }
    }
    QVector<QPointF> out;
    for (int i = 0; i < n - 1; ++i)
        if (keep[i])
            out.append(ring[i]);
    return out;
}

double polygonArea(const QVector<QPointF> &poly)
{
    double a = 0.0;
    for (int i = 0; i < poly.size(); ++i) {
        const QPointF p = poly[i], q = poly[(i + 1) % poly.size()];
        a += double(p.x()) * q.y() - double(q.x()) * p.y();
    }
    return a * 0.5;
}

void cleanPolygon(QVector<QPointF> &poly)
{
    QVector<QPointF> clean;
    for (const QPointF &p : poly) {
        if (clean.isEmpty()
            || hypot(p.x() - clean.last().x(), p.y() - clean.last().y()) > 1e-6)
            clean.append(p);
    }
    while (clean.size() > 1
           && hypot(clean.first().x() - clean.last().x(),
                    clean.first().y() - clean.last().y()) <= 1e-6)
        clean.removeLast();
    poly = clean;
    /* drop collinear middle points */
    if (poly.size() < 3)
        return;
    QVector<QPointF> out;
    out.append(poly[0]);
    for (int i = 1; i < poly.size(); ++i) {
        const QPointF p = out.last();
        const QPointF q = poly[i];
        const QPointF r = poly[(i + 1) % poly.size()];
        if (std::abs(cross2(p, q, r)) > 1e-6)
            out.append(q);
    }
    poly = out;
}

/* Ear-clipping triangulation of a simple polygon (CCW). Returns the
 * triangle index triples; falls back to a fan when clipping stalls. */
QVector<std::array<int, 3>> earClip(const QVector<QPointF> &poly)
{
    QVector<std::array<int, 3>> tris;
    const int n = poly.size();
    if (n < 3)
        return tris;
    if (n == 3) {
        tris.append({ 0, 1, 2 });
        return tris;
    }
    QVector<int> idx(n);
    for (int i = 0; i < n; ++i)
        idx[i] = i;
    int guard = n * n + 16;
    int iter = 0;
    while (idx.size() > 3 && iter++ < guard) {
        bool clipped = false;
        const int m = idx.size();
        for (int i = 0; i < m; ++i) {
            const int a = idx[(i + m - 1) % m];
            const int b = idx[i];
            const int c = idx[(i + 1) % m];
            if (cross2(poly[a], poly[b], poly[c]) <= 1e-7)
                continue;               /* reflex or degenerate */
            bool contains = false;
            for (int k = 0; k < m; ++k) {
                const int t = idx[k];
                if (t == a || t == b || t == c)
                    continue;
                if (pointInTriangle(poly[t], poly[a], poly[b], poly[c])) {
                    contains = true;
                    break;
                }
            }
            if (contains)
                continue;
            tris.append({ a, b, c });
            idx.removeAt(i);
            clipped = true;
            break;
        }
        if (!clipped)
            break;                      /* stall -> fan fallback */
    }
    if (idx.size() > 3) {
        /* fallback: fan from the first remaining index */
        tris.clear();
        for (int i = 1; i + 1 < n; ++i)
            tris.append({ 0, i, i + 1 });
    } else if (idx.size() == 3) {
        tris.append({ idx[0], idx[1], idx[2] });
    }
    return tris;
}

/* 1-4 subdivision with exact-midpoint welding. */
void subdivide(PuppetMesh &mesh, int rounds, int maxVerts)
{
    for (int r = 0; r < rounds; ++r) {
        if (mesh.verts.size() >= maxVerts)
            break;
        QHash<quint64, int> edgeMid;
        QVector<QPointF> verts = mesh.verts;
        QVector<std::array<int, 3>> tris;
        tris.reserve(mesh.tris.size() * 4);
        const auto midOf = [&](int a, int b) {
            const int lo = qMin(a, b), hi = qMax(a, b);
            const quint64 key = (quint64(quint32(lo)) << 32) | quint32(hi);
            int &m = edgeMid[key];
            if (m == 0) {
                m = verts.size();
                verts.append(QPointF((mesh.verts[lo].x() + mesh.verts[hi].x()) * 0.5,
                                     (mesh.verts[lo].y() + mesh.verts[hi].y()) * 0.5));
            }
            return m;
        };
        for (const auto &t : mesh.tris) {
            const int a = t[0], b = t[1], c = t[2];
            const int ab = midOf(a, b), bc = midOf(b, c), ca = midOf(c, a);
            tris.append({ a, ab, ca });
            tris.append({ b, bc, ab });
            tris.append({ c, ca, bc });
            tris.append({ ab, bc, ca });
        }
        mesh.verts = verts;
        mesh.tris = tris;
    }
}

/* Deterministic [0,1) hash — per-joint phase offsets from the seed. */
quint32 mix32(quint32 x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

} // namespace

float puppetHash01(quint32 seed, int jointIndex)
{
    const quint32 h = mix32(seed ^ 0x9E3779B9u ^ mix32(quint32(jointIndex) * 0x85EBCA6Bu));
    return float((h >> 8) & 0xFFFFFF) / float(1 << 24);
}

/* ------------------------------------------------------------------ */
/* value types                                                         */
/* ------------------------------------------------------------------ */

int PuppetSkeleton::depth(int j) const
{
    int d = 0;
    while (j >= 0 && d < joints.size()) {
        j = joints[j].parent;
        ++d;
    }
    return d;
}

bool PuppetSkeleton::isValid() const
{
    if (joints.isEmpty())
        return false;
    int roots = 0;
    for (int j = 0; j < joints.size(); ++j) {
        const int p = joints[j].parent;
        if (p < -1 || p >= j)         /* parent must be earlier in the list */
            return false;
        if (p == -1)
            ++roots;
    }
    return roots == 1;
}

bool PuppetMesh::isValid() const
{
    if (verts.size() < 3 || tris.isEmpty())
        return false;
    for (const auto &t : tris) {
        for (int k = 0; k < 3; ++k)
            if (t[k] < 0 || t[k] >= verts.size())
                return false;
        if (cross2(verts[t[0]], verts[t[1]], verts[t[2]]) <= 1e-7)
            return false;
    }
    return true;
}

PuppetPose PuppetPose::rest(const PuppetSkeleton &skeleton)
{
    PuppetPose pose;
    pose.anglesDeg.fill(0.0f, skeleton.joints.size());
    return pose;
}

bool PuppetPose::isRest() const
{
    for (float a : anglesDeg)
        if (std::abs(a) > 1e-6f)
            return false;
    return std::abs(rootOffset.x()) <= 1e-6 && std::abs(rootOffset.y()) <= 1e-6;
}

PuppetPose PuppetPose::scaled(float a) const
{
    PuppetPose out = *this;
    for (float &angle : out.anglesDeg)
        angle *= a;
    out.rootOffset = QPointF(rootOffset.x() * a, rootOffset.y() * a);
    return out;
}

/* ------------------------------------------------------------------ */
/* PuppetEffect                                                        */
/* ------------------------------------------------------------------ */

QByteArray PuppetEffect::serialize() const
{
    QByteArray out;
    out.reserve(23);
    out.append(char(2));                          /* version (phase added) */
    out.append(char(quint8(kind)));
    out.append(char(quint8(loop)));
    quint32 amp; std::memcpy(&amp, &amplitude, 4);
    quint32 spd; std::memcpy(&spd, &speed, 4);
    quint32 phs; std::memcpy(&phs, &phase, 4);
    const auto le = [](quint32 v) {
        QByteArray b(4, char(0));
        b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
        b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
        return b;
    };
    out.append(le(amp));
    out.append(le(spd));
    out.append(le(seed));
    out.append(le(durationMs));
    out.append(le(phs));
    return out;
}

PuppetEffect PuppetEffect::deserialize(const QByteArray &data)
{
    PuppetEffect e;
    const bool v1 = data.size() == 19 && data[0] == char(1);
    const bool v2 = data.size() == 23 && data[0] == char(2);
    if (!v1 && !v2)
        return e;                                 /* default */
    const auto u32 = [&](int at) {
        quint32 v = 0;
        for (int i = 0; i < 4; ++i)
            v |= quint32(quint8(data[at + i])) << (8 * i);
        return v;
    };
    const quint8 k = quint8(data[1]), l = quint8(data[2]);
    if (k > quint8(MotionKind::Custom) || l > quint8(LoopMode::PingPong))
        return e;
    e.kind = MotionKind(k);
    e.loop = LoopMode(l);
    const quint32 amp = u32(3), spd = u32(7);
    std::memcpy(&e.amplitude, &amp, 4);
    std::memcpy(&e.speed, &spd, 4);
    e.seed = u32(11);
    e.durationMs = u32(15);
    if (v2) {
        const quint32 phs = u32(19);
        std::memcpy(&e.phase, &phs, 4);
    }
    if (e.durationMs == 0 || !std::isfinite(e.amplitude) || !std::isfinite(e.speed)
        || !std::isfinite(e.phase))
        return PuppetEffect();
    return e;
}

bool PuppetEffect::operator==(const PuppetEffect &o) const
{
    return kind == o.kind && amplitude == o.amplitude && speed == o.speed
        && loop == o.loop && seed == o.seed && durationMs == o.durationMs
        && phase == o.phase;
}

/* ------------------------------------------------------------------ */
/* PuppetAsset                                                         */
/* ------------------------------------------------------------------ */

QByteArray PuppetAsset::serializeManifest() const
{
    QByteArray out;
    out.reserve(64 + motion.serialize().size());
    out.append(char(1));                                  /* version */
    const auto u32 = [](quint32 v) {
        QByteArray b(4, char(0));
        b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
        b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
        return b;
    };
    const auto f32 = [&](double v) {
        float f = float(v); quint32 bits; std::memcpy(&bits, &f, 4);
        return u32(bits);
    };
    out.append(u32(quint32(id)));
    out.append(u32(quint32(sourceHarvestId)));
    out.append(char(quint8(regime)));
    out.append(f32(defaultAmplitude));
    const QByteArray nameBytes = name.toUtf8();
    out.append(char(quint8(nameBytes.size() >> 8)));
    out.append(char(quint8(nameBytes.size() & 0xFF)));
    out.append(nameBytes);
    const QByteArray clipBytes = motion.serialize();
    out.append(u32(quint32(clipBytes.size())));
    out.append(clipBytes);
    return out;
}

PuppetAsset PuppetAsset::fromManifest(const QByteArray &data, const PuppetRig &rig)
{
    PuppetAsset asset;
    if (data.size() < 15 || data[0] != char(1))
        return asset;
    const auto u32at = [&](int at) {
        quint32 v = 0;
        for (int i = 0; i < 4; ++i)
            v |= quint32(quint8(data[at + i])) << (8 * i);
        return v;
    };
    asset.id = int(u32at(1));
    asset.sourceHarvestId = int(u32at(5));
    const quint8 reg = quint8(data[9]);
    if (reg > quint8(PuppetRegime::Illustration))
        return asset;
    asset.regime = PuppetRegime(reg);
    const quint32 amp = u32at(10);
    std::memcpy(&asset.defaultAmplitude, &amp, 4);
    int at = 14;
    const int nameLen = (int(quint8(data[at])) << 8) | int(quint8(data[at + 1]));
    at += 2;
    if (at + nameLen + 4 > data.size())
        return asset;
    asset.name = QString::fromUtf8(data.mid(at, nameLen));
    at += nameLen;
    const quint32 clipLen = u32at(at);
    at += 4;
    if (at + int(clipLen) > data.size())
        return asset;
    asset.motion = PuppetMotionClip::deserialize(data.mid(at, int(clipLen)));
    if (asset.motion.keys.isEmpty())
        return asset;
    asset.rig = rig;
    return asset;
}

/* ------------------------------------------------------------------ */
/* PuppetBuilder                                                       */
/* ------------------------------------------------------------------ */

QVector<QPointF> PuppetBuilder::extractContour(const QImage &mask, float epsilonPx)
{
    QImage m = mask.convertToFormat(QImage::Format_Grayscale8);
    const QVector<quint8> occ = occupancyFromMask(m);
    QVector<QPointF> contour = traceContour(occ, m.width(), m.height());
    if (epsilonPx > 0.0f)
        contour = simplifyPolyline(contour, epsilonPx);
    cleanPolygon(contour);
    if (polygonArea(contour) < 0)
        std::reverse(contour.begin(), contour.end());
    return contour;
}

PuppetSkeleton PuppetBuilder::fitDefaultHumanoid(const QImage &maskIn)
{
    const QImage mask = maskIn.convertToFormat(QImage::Format_Grayscale8);
    const int w = mask.width(), h = mask.height();
    PuppetSkeleton skel;
    const auto add = [&](int parent, const QString &name, QPointF rest) {
        PuppetJoint j;
        j.parent = parent;
        j.rest = rest;
        j.name = name;
        skel.joints.append(j);
    };
    if (w <= 0 || h <= 0) {
        /* degenerate: geometric fallback, joints may lie outside the mask */
        const QPointF c(w * 0.5, h * 0.5);
        add(-1, "root", c + QPointF(0, h * 0.15));
        add(0, "chest", c);
        add(1, "neck", c - QPointF(0, h * 0.10));
        add(2, "head", c - QPointF(0, h * 0.18));
        add(1, "lShoulder", c + QPointF(-w * 0.20, -h * 0.02));
        add(4, "lElbow", c + QPointF(-w * 0.20, h * 0.12));
        add(1, "rShoulder", c + QPointF(w * 0.20, -h * 0.02));
        add(6, "rElbow", c + QPointF(w * 0.20, h * 0.12));
        add(0, "lHip", c + QPointF(-w * 0.10, h * 0.22));
        add(0, "rHip", c + QPointF(w * 0.10, h * 0.22));
        return skel;
    }

    const QVector<quint8> occ = occupancyFromMask(mask);
    const auto inside = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h
            && occ[qsizetype(y) * w + x];
    };

    /* row profile */
    struct Row { int count = 0; int minX = 0; int maxX = -1; double cx = 0.0; };
    QVector<Row> rows(h);
    for (int y = 0; y < h; ++y) {
        Row &r = rows[y];
        r.minX = w;
    }
    qint64 total = 0;
    int top = h, bottom = -1;
    for (int y = 0; y < h; ++y) {
        Row &r = rows[y];
        double sx = 0.0;
        for (int x = 0; x < w; ++x) {
            if (occ[qsizetype(y) * w + x]) {
                ++r.count;
                r.minX = qMin(r.minX, x);
                r.maxX = qMax(r.maxX, x);
                sx += x;
                ++total;
            }
        }
        if (r.count) {
            r.cx = sx / r.count;
            top = qMin(top, y);
            bottom = qMax(bottom, y);
        }
    }
    if (total < 8) {
        /* nearly empty: geometric fallback (same as the null-mask path) */
        const QPointF c(w * 0.5, h * 0.5);
        add(-1, "root", c + QPointF(0, h * 0.15));
        add(0, "chest", c);
        add(1, "neck", c - QPointF(0, h * 0.10));
        add(2, "head", c - QPointF(0, h * 0.18));
        add(1, "lShoulder", c + QPointF(-w * 0.20, -h * 0.02));
        add(4, "lElbow", c + QPointF(-w * 0.20, h * 0.12));
        add(1, "rShoulder", c + QPointF(w * 0.20, -h * 0.02));
        add(6, "rElbow", c + QPointF(w * 0.20, h * 0.12));
        add(0, "lHip", c + QPointF(-w * 0.10, h * 0.22));
        add(0, "rHip", c + QPointF(w * 0.10, h * 0.22));
        return skel;
    }

    const int H = bottom - top + 1;
    const auto bandMeanCx = [&](double y0f, double y1f) {
        double s = 0.0; int n = 0;
        for (int y = int(qRound(double(top) + y0f * H));
             y <= int(qRound(double(top) + y1f * H)); ++y) {
            if (y >= 0 && y < h && rows[y].count) { s += rows[y].cx; ++n; }
        }
        return n ? QPointF(s / n, 0) : QPointF(w * 0.5, 0);
    };
    /* nearest OCCUPIED row to y (the requested band can fall in a gap
     * between body parts — an empty row's extents are garbage and the
     * inside-mask clamp would drag the joint across the figure) */
    const auto rowAt = [&](double yf, int rangePx) {
        const int y0 = qBound(0, qRound(double(top) + yf * H), h - 1);
        for (int r = 0; r <= rangePx; ++r) {
            if (rows[y0].count)
                return y0;
            if (y0 + r < h && rows[y0 + r].count)
                return y0 + r;
            if (y0 - r >= 0 && rows[y0 - r].count)
                return y0 - r;
        }
        return -1;               /* no occupied row in range (degenerate) */
    };

    /* joints */
    QVector<QPointF> rest;
    rest.resize(10);

    /* head: row centroid just under the top */
    {
        const int hy = rowAt(0.07, 12);
        rest[3] = QPointF(hy >= 0 && rows[hy].count
                              ? rows[hy].cx : w * 0.5,
                          hy >= 0 ? hy : double(top) + 0.07 * H);
    }
    const double cx = bandMeanCx(0.18, 0.45).x();
    rest[0] = QPointF(cx, double(top) + 0.55 * H);    /* root / pelvis */
    rest[1] = QPointF(cx, double(top) + 0.27 * H);    /* chest */
    rest[2] = QPointF(cx, double(top) + 0.17 * H);    /* neck */
    const int sy = rowAt(0.22, 16);
    const int ey = rowAt(0.38, 16);
    const double sw = (sy >= 0 && rows[sy].count)
        ? double(rows[sy].maxX - rows[sy].minX + 1) : 0;
    const double ew = (ey >= 0 && rows[ey].count)
        ? double(rows[ey].maxX - rows[ey].minX + 1) : 0;
    rest[4] = QPointF(sy >= 0 ? rows[sy].minX + 0.08 * sw : cx, sy); /* lShoulder */
    rest[6] = QPointF(sy >= 0 ? rows[sy].maxX - 0.08 * sw : cx, sy); /* rShoulder */
    /* elbows: blend the shoulder edge toward the wrist-row edge */
    rest[5] = QPointF(sy >= 0 && ey >= 0
                          ? 0.5 * (rows[sy].minX + 0.08 * sw)
                              + 0.5 * (rows[ey].minX + 0.05 * ew)
                          : cx, ey);
    rest[7] = QPointF(sy >= 0 && ey >= 0
                          ? 0.5 * (rows[sy].maxX - 0.08 * sw)
                              + 0.5 * (rows[ey].maxX - 0.05 * ew)
                          : cx, ey);
    const int hy2 = rowAt(0.60, 16);
    double hipW = 0.0; int hipN = 0;
    for (int y = qRound(double(top) + 0.52 * H); y <= qRound(double(top) + 0.62 * H); ++y) {
        if (y >= 0 && y < h && rows[y].count) { hipW += rows[y].maxX - rows[y].minX + 1; ++hipN; }
    }
    hipW = hipN ? hipW / hipN : 0.0;
    rest[8] = QPointF(cx - 0.10 * hipW, hy2);
    rest[9] = QPointF(cx + 0.10 * hipW, hy2);

    /* clamp every joint into the mask (walk toward the torso center) */
    const QPointF anchor(cx, double(top) + 0.27 * H);
    for (QPointF &p : rest) {
        if (!inside(int(qRound(p.x())), int(qRound(p.y())))) {
            const QPointF start = p;
            QPointF q = start;
            for (int s = 0; s < 96 && !inside(int(qRound(q.x())), int(qRound(q.y()))); ++s) {
                q = QPointF(q.x() + (anchor.x() - start.x()) * 0.02,
                            q.y() + (anchor.y() - start.y()) * 0.02);
            }
            if (inside(int(qRound(q.x())), int(qRound(q.y()))))
                p = q;
        }
    }

    add(-1, "root", rest[0]);
    add(0, "chest", rest[1]);
    add(1, "neck", rest[2]);
    add(2, "head", rest[3]);
    add(1, "lShoulder", rest[4]);
    add(4, "lElbow", rest[5]);
    add(1, "rShoulder", rest[6]);
    add(6, "rElbow", rest[7]);
    add(0, "lHip", rest[8]);
    add(0, "rHip", rest[9]);
    return skel;
}

PuppetRig PuppetBuilder::build(const QImage &rgbaIn, const QImage &maskIn,
                               const PuppetBuilderConfig &config)
{
    PuppetRig rig;
    if (rgbaIn.isNull())
        return rig;
    rig.texture = rgbaIn.convertToFormat(QImage::Format_ARGB32);
    rig.bounds = rig.texture.rect();

    QImage mask = maskIn.isNull()
        ? QImage() : maskIn.convertToFormat(QImage::Format_Grayscale8);
    if (!mask.isNull()) {
        /* Asset semantics: the mask is BINARY. HarvestItem masks may carry
         * soft edges (SAM confidence); binarizing keeps the contour, the
         * fit and the render gating deterministic and makes the rest render
         * reproduce the cutout exactly. */
        for (int y = 0; y < mask.height(); ++y) {
            quint8 *mrow = mask.scanLine(y);
            for (int x = 0; x < mask.width(); ++x)
                mrow[x] = mrow[x] > 127 ? 255 : 0;
        }
    }
    if (mask.isNull()) {
        /* derive the mask from the texture alpha */
        mask = QImage(rig.texture.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < mask.height(); ++y) {
            const quint8 *row = rig.texture.constScanLine(y);
            quint8 *mrow = mask.scanLine(y);
            for (int x = 0; x < mask.width(); ++x)
                mrow[x] = row[x * 4 + 3] > 127 ? 255 : 0;
        }
    }
    rig.mask = mask;

    /* Asset semantics (HarvestItem.cutout carries soft edge alpha): the rig
     * texture's ALPHA is the binary mask, so the rest render reproduces the
     * cutout exactly (t=0 == the static figure). RGB keeps the soft-edge
     * pixels; only the alpha channel is binarized. */
    if (rig.mask.size() == rig.texture.size()) {
        for (int y = 0; y < rig.texture.height(); ++y) {
            const quint8 *mrow = rig.mask.constScanLine(y);
            quint8 *trow = rig.texture.scanLine(y);
            for (int x = 0; x < rig.texture.width(); ++x)
                trow[x * 4 + 3] = mrow[x] > 127 ? 255 : 0;
        }
    }

    const QVector<quint8> occ = occupancyFromMask(mask);
    qint64 total = 0;
    for (quint8 v : occ)
        total += v;
    if (total < 4)
        return rig;

    const QVector<quint8> filled = fillHoles(occ, mask.width(), mask.height());
    /* drop speckle islands (segmentation noise): keep components >= 0.5%
     * of the largest component (the figure), min 12 px */
    qint64 largest = 0;
    {
        QVector<quint8> probe = filled;
        QVector<int> stack;
        QVector<quint8> seen = probe;
        for (qsizetype s = 0; s < seen.size(); ++s) {
            if (!seen[s])
                continue;
            qint64 area = 0;
            stack.clear();
            stack.append(int(s));
            seen[s] = 0;
            while (!stack.isEmpty()) {
                const int i = stack.takeLast();
                ++area;
                const int x = i % mask.width(), y = i / mask.width();
                if (x > 0 && seen[i - 1])     { seen[i - 1] = 0; stack.append(i - 1); }
                if (x + 1 < mask.width() && seen[i + 1]) { seen[i + 1] = 0; stack.append(i + 1); }
                if (y > 0 && seen[i - mask.width()])     { seen[i - mask.width()] = 0; stack.append(i - mask.width()); }
                if (y + 1 < mask.height() && seen[i + mask.width()]) { seen[i + mask.width()] = 0; stack.append(i + mask.width()); }
            }
            largest = qMax(largest, area);
        }
    }
    const qint64 speckleMin = qMax<qint64>(12, largest / 200);
    /* the rig mask = speckle-cleaned ORIGINAL mask (holes stay transparent,
     * segmentation noise removed) — this is the mesh's alpha domain */
    const QVector<quint8> cleanMask = removeSpeckles(occ, mask.width(),
                                                     mask.height(), speckleMin);
    QImage maskImg(mask.size(), QImage::Format_Grayscale8);
    for (qsizetype i = 0; i < cleanMask.size(); ++i)
        maskImg.scanLine(int(i / mask.width()))[int(i % mask.width())]
            = cleanMask[i] ? 255 : 0;
    rig.mask = maskImg;
    /* contour domain: hole-filled + speckle-cleaned */
    const QVector<quint8> clean = removeSpeckles(filled, mask.width(),
                                                 mask.height(), speckleMin);
    QImage filledImg(mask.size(), QImage::Format_Grayscale8);
    for (qsizetype i = 0; i < clean.size(); ++i)
        filledImg.scanLine(int(i / mask.width()))[int(i % mask.width())]
            = clean[i] ? 255 : 0;

    /* contour -> simplify (respect maxBoundaryVerts) -> triangulate */
    const double diag = hypot(double(mask.width()), double(mask.height()));
    float eps = float(qMax(0.5, 0.004 * diag));
    QVector<QPointF> poly = traceContour(filled, mask.width(), mask.height());
    for (int tries = 0; tries < 12; ++tries) {
        QVector<QPointF> p = simplifyPolyline(poly, eps);
        cleanPolygon(p);
        if (polygonArea(p) < 0)
            std::reverse(p.begin(), p.end());
        if (p.size() <= config.maxBoundaryVerts || eps > 0.25 * diag) {
            poly = p;
            break;
        }
        eps *= 1.5f;
    }
    if (poly.size() < 3)
        return rig;

    PuppetMesh mesh;
    mesh.verts = poly;
    mesh.tris = earClip(poly);
    if (mesh.tris.isEmpty())
        return rig;

    /* interior vertices via 1-4 subdivision */
    const int rounds = qMin(config.maxSubdivisionRounds,
                            qMax(0, int(std::log(double(config.maxVerts)
                                                 / qMax(1, mesh.verts.size()))
                                        / std::log(2.6))));
    subdivide(mesh, rounds, config.maxVerts);
    rig.mesh = mesh;

    /* skeleton (synthetic humanoid fit on the FILLED mask) */
    if (config.fitHumanoid)
        rig.skeleton = fitDefaultHumanoid(filledImg);
    if (!rig.skeleton.isValid())
        return rig;

    /* handle mapping: nearest mesh vertex per joint; when two joints claim
     * the same vertex the deeper chain joint wins (it carries the parent's
     * rotation in its FK target). */
    rig.handleVerts.fill(-1, rig.skeleton.joints.size());
    QVector<int> claimOrder(rig.skeleton.joints.size());
    for (int j = 0; j < claimOrder.size(); ++j)
        claimOrder[j] = j;
    std::sort(claimOrder.begin(), claimOrder.end(), [&](int a, int b) {
        return rig.skeleton.depth(a) > rig.skeleton.depth(b);
    });
    QHash<int, int> claimed;               /* vertex -> joint */
    for (int j : claimOrder) {
        const QPointF target = rig.skeleton.joints[j].rest;
        int best = -1;
        double bestD = std::numeric_limits<double>::max();
        for (int v = 0; v < mesh.verts.size(); ++v) {
            const QPointF d = mesh.verts[v] - target;
            const double dist = d.x() * d.x() + d.y() * d.y();
            if (dist < bestD) { bestD = dist; best = v; }
        }
        if (best >= 0 && !claimed.contains(best)) {
            claimed.insert(best, j);
            rig.handleVerts[j] = best;
        }
    }

    rig.rigId = PuppetRuntime::rigId(rig);
    return rig;
}

/* ------------------------------------------------------------------ */
/* PuppetDeformer                                                      */
/* ------------------------------------------------------------------ */

QVector<QPointF> PuppetDeformer::fk(const PuppetRig &rig, const PuppetPose &pose)
{
    return PuppetDeformer::fk(rig.skeleton, pose);
}

QVector<QPointF> PuppetDeformer::fk(const PuppetSkeleton &s, const PuppetPose &pose)
{
    const int n = s.joints.size();
    QVector<QPointF> world(n);
    if (n == 0)
        return world;
    QVector<double> absAngle(n, 0.0);
    for (int j = 0; j < n; ++j) {
        const double a = pose.anglesDeg.value(j, 0.0f);
        if (s.joints[j].parent >= 0)
            absAngle[j] = absAngle[s.joints[j].parent] + a;
        else
            absAngle[j] = a;
    }
    for (int j = 0; j < n; ++j) {
        const PuppetJoint &jnt = s.joints[j];
        if (jnt.parent < 0) {
            world[j] = jnt.rest + pose.rootOffset;
            continue;
        }
        const QPointF dir = jnt.rest - s.joints[jnt.parent].rest;
        const double rad = qDegreesToRadians(absAngle[j]);
        const double c = std::cos(rad), sn = std::sin(rad);
        world[j] = world[jnt.parent]
            + QPointF(c * dir.x() - sn * dir.y(), sn * dir.x() + c * dir.y());
    }
    return world;
}

QVector<QPointF> PuppetDeformer::deform(const PuppetRig &rig, const PuppetPose &pose)
{
    return PuppetDeformer::deform(rig, pose, Options());
}

QVector<QPointF> PuppetDeformer::deform(const PuppetRig &rig, const PuppetPose &pose,
                                        const Options &opts)
{
    const PuppetMesh &mesh = rig.mesh;
    const int V = mesh.verts.size();
    QVector<QPointF> rest = mesh.verts;
    if (V == 0)
        return rest;

    /* adjacency (1-ring) */
    QVector<QVector<int>> adj(V);
    for (const auto &t : mesh.tris) {
        for (int k = 0; k < 3; ++k) {
            const int a = t[k], b = t[(k + 1) % 3];
            QVector<int> &va = adj[a];
            if (!va.contains(b))
                va.append(b);
            QVector<int> &vb = adj[b];
            if (!vb.contains(a))
                vb.append(a);
        }
    }

    /* handles: FK targets for the mapped joints, expressed as a RELATIVE
     * displacement of the joint's rest position applied to the handle
     * VERTEX — so the identity pose (fk == joint rest) leaves the mesh
     * exactly at rest even when the nearest vertex is not the joint. */
    const QVector<QPointF> targets = fk(rig, pose);
    QVector<int> handleV;
    QVector<QPointF> handleT;
    for (int j = 0; j < rig.handleVerts.size(); ++j) {
        const int v = rig.handleVerts[j];
        if (v >= 0 && v < V) {
            handleV.append(v);
            const QPointF delta = targets[j] - rig.skeleton.joints[j].rest;
            handleT.append(rest[v] + delta);
        }
    }
    if (handleV.isEmpty())
        return rest;                       /* nothing to drive */

    QVector<int> deg(V, 0);
    for (int i = 0; i < V; ++i)
        deg[i] = adj[i].size();

    /* local-global ARAP sweeps. Handles are DAMPED per sweep (linear ramp
     * to full strength on the final solve): the mesh distributes the
     * rotation instead of letting a coarse neighborhood snap, which keeps
     * small triangles from inverting during larger poses. The final solve
     * uses the full targets, so handle fidelity and the identity pose are
     * unaffected. */
    QVector<double> rx(V), ry(V);
    for (int i = 0; i < V; ++i) { rx[i] = rest[i].x(); ry[i] = rest[i].y(); }
    QVector<QPointF> p = rest;

    for (int iter = 0; iter < opts.iterations; ++iter) {
        /* local step: per-vertex rotation via the 2D closed form */
        QVector<double> cs(V, 1.0), sn(V, 0.0);
        for (int i = 0; i < V; ++i) {
            const int d = deg[i];
            if (d == 0)
                continue;
            double a = 0, b = 0, c = 0, dd = 0;
            for (int j : adj[i]) {
                const double drx = rx[j] - rx[i], dry = ry[j] - ry[i];
                const double dpx = p[j].x() - p[i].x(), dpy = p[j].y() - p[i].y();
                a += drx * dpx;  b += drx * dpy;
                c += dry * dpx;  dd += dry * dpy;
            }
            const double theta = std::atan2(b - c, a + dd);
            cs[i] = std::cos(theta);
            sn[i] = std::sin(theta);
        }

        /* global step: solve L p = b + handles via matrix-free CG */
        QVector<double> bx(V, 0.0), by(V, 0.0);
        for (int i = 0; i < V; ++i) {
            for (int j : adj[i]) {
                const double cij = (cs[i] + cs[j]) * 0.5;
                const double sij = (sn[i] + sn[j]) * 0.5;
                const double dx = rx[i] - rx[j], dy = ry[i] - ry[j];
                bx[i] += cij * dx - sij * dy;
                by[i] += sij * dx + cij * dy;
            }
        }
        const double lambda = opts.handleWeight;
        const double ramp = double(iter + 1) / double(opts.iterations);
        for (int h = 0; h < handleV.size(); ++h) {
            const int v = handleV[h];
            bx[v] += lambda * (rest[v].x() + (handleT[h].x() - rest[v].x()) * ramp);
            by[v] += lambda * (rest[v].y() + (handleT[h].y() - rest[v].y()) * ramp);
        }

        /* CG (both RHS share the matrix) */
        const int maxIt = opts.cgIterations;
        const double tol2 = opts.cgTolerance * opts.cgTolerance;
        QVector<double> px = rx, py = ry;          /* warm start = rest */
        QVector<double> rxx = bx, ryy = by;
        QVector<double> r2x(V), r2y(V);
        for (int i = 0; i < V; ++i) {
            double ax = deg[i] * px[i], ay = deg[i] * py[i];
            for (int j : adj[i]) { ax -= px[j]; ay -= py[j]; }
            const bool isH = handleV.contains(i);
            if (isH) { ax += lambda * px[i]; ay += lambda * py[i]; }
            rxx[i] -= ax;
            ryy[i] -= ay;
        }
        double n2x = 0, n2y = 0, b2x = 0, b2y = 0;
        for (int i = 0; i < V; ++i) {
            n2x += rxx[i] * rxx[i]; n2y += ryy[i] * ryy[i];
            b2x += bx[i] * bx[i];   b2y += by[i] * by[i];
        }
        QVector<double> dx = rxx, dy = ryy;
        for (int it = 0; it < maxIt; ++it) {
            if ((n2x <= tol2 * qMax(1.0, b2x)) && (n2y <= tol2 * qMax(1.0, b2y)))
                break;
            QVector<double> qx(V), qy(V);
            for (int i = 0; i < V; ++i) {
                double ax = deg[i] * dx[i], ay = deg[i] * dy[i];
                for (int j : adj[i]) { ax -= dx[j]; ay -= dy[j]; }
                const bool isH = handleV.contains(i);
                if (isH) { ax += lambda * dx[i]; ay += lambda * dy[i]; }
                qx[i] = ax; qy[i] = ay;
            }
            double dq = 0;
            for (int i = 0; i < V; ++i)
                dq += dx[i] * qx[i] + dy[i] * qy[i];
            if (dq <= 0)
                break;
            const double alphaX = n2x / dq, alphaY = n2y / dq;
            for (int i = 0; i < V; ++i) {
                px[i] += alphaX * dx[i]; py[i] += alphaY * dy[i];
                rxx[i] -= alphaX * qx[i]; ryy[i] -= alphaY * qy[i];
            }
            double n2xNew = 0, n2yNew = 0;
            for (int i = 0; i < V; ++i) {
                n2xNew += rxx[i] * rxx[i];
                n2yNew += ryy[i] * ryy[i];
            }
            const double betaX = n2xNew / qMax(1e-300, n2x);
            const double betaY = n2yNew / qMax(1e-300, n2y);
            for (int i = 0; i < V; ++i) {
                const double oldDx = dx[i], oldDy = dy[i];
                dx[i] = rxx[i] + betaX * oldDx;
                dy[i] = ryy[i] + betaY * oldDy;
            }
            n2x = n2xNew; n2y = n2yNew;
        }
        for (int i = 0; i < V; ++i)
            p[i] = QPointF(px[i], py[i]);
    }

    return p;
}

/* ------------------------------------------------------------------ */
/* PuppetRenderer                                                      */
/* ------------------------------------------------------------------ */

QImage PuppetRenderer::render(const QImage &textureIn, const QImage &maskIn,
                              const QVector<QPointF> &restVerts,
                              const QVector<QPointF> &deformedVerts,
                              const QVector<std::array<int, 3>> &tris)
{
    QImage tex = textureIn.convertToFormat(QImage::Format_ARGB32);
    QImage mask = maskIn.isNull()
        ? QImage() : maskIn.convertToFormat(QImage::Format_Grayscale8);
    QImage out(tex.size(), QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    if (tex.isNull() || restVerts.size() != deformedVerts.size() || tris.isEmpty())
        return out;

    const int W = tex.width(), H = tex.height();
    const auto clampX = [&](double v) { return int(qBound(0.0, v, double(W - 1))); };
    const auto clampY = [&](double v) { return int(qBound(0.0, v, double(H - 1))); };
    const auto sample = [&](double u, double v) {
        /* texel centers sit at k+0.5, so map the sample point (pixel
         * corner coordinates) into texel space by subtracting 0.5 */
        const double tu = qBound(0.0, u - 0.5, double(W - 1));
        const double tv = qBound(0.0, v - 0.5, double(H - 1));
        const int x0 = int(std::floor(tu)), y0 = int(std::floor(tv));
        const int x1 = qMin(W - 1, x0 + 1), y1 = qMin(H - 1, y0 + 1);
        const double fx = tu - x0, fy = tv - y0;
        double r = 0, g = 0, b = 0, a = 0;
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                const quint8 *p = tex.constScanLine(dy ? y1 : y0)
                                  + (dx ? x1 : x0) * 4;
                const double w = (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy);
                b += p[0] * w; g += p[1] * w; r += p[2] * w; a += p[3] * w;
            }
        }
        return std::array<double, 4>{ r, g, b, a };
    };

    for (const auto &t : tris) {
        const QPointF &pa = deformedVerts[t[0]];
        const QPointF &pb = deformedVerts[t[1]];
        const QPointF &pc = deformedVerts[t[2]];
        const double denom = (pb.x() - pa.x()) * (pc.y() - pa.y())
                           - (pb.y() - pa.y()) * (pc.x() - pa.x());
        if (denom <= 1e-9)
            continue;                    /* degenerate or flipped */
        const int x0 = qMax(0, clampX(qMin(pa.x(), qMin(pb.x(), pc.x())) - 0.5));
        const int x1 = qMin(W - 1, clampX(qMax(pa.x(), qMax(pb.x(), pc.x())) + 0.5));
        const int y0 = qMax(0, clampY(qMin(pa.y(), qMin(pb.y(), pc.y())) - 0.5));
        const int y1 = qMin(H - 1, clampY(qMax(pa.y(), qMax(pb.y(), pc.y())) + 0.5));
        const QPointF &ra = restVerts[t[0]];
        const QPointF &rb = restVerts[t[1]];
        const QPointF &rc = restVerts[t[2]];
        for (int y = y0; y <= y1; ++y) {
            quint8 *outRow = out.scanLine(y);
            for (int x = x0; x <= x1; ++x) {
                const QPointF p(x + 0.5, y + 0.5);
                const double v0x = pb.x() - pa.x(), v0y = pb.y() - pa.y();
                const double v1x = pc.x() - pa.x(), v1y = pc.y() - pa.y();
                const double v2x = p.x() - pa.x(), v2y = p.y() - pa.y();
                const double lb = (v2x * v1y - v2y * v1x) / denom;
                const double lc = (v0x * v2y - v0y * v2x) / denom;
                const double la = 1.0 - lb - lc;
                if (la < -1e-6 || lb < -1e-6 || lc < -1e-6)
                    continue;
                const double u = la * ra.x() + lb * rb.x() + lc * rc.x();
                const double v = la * ra.y() + lb * rb.y() + lc * rc.y();
                const auto s = sample(u, v);
                double alpha;
                if (!mask.isNull()) {
                    /* Asset semantics: output alpha is the BINARY mask
                     * (HarvestItem.cutout carries alpha = mask). The
                     * texture's own alpha only GATES visibility — interior
                     * holes stay transparent, semi-transparent fringe
                     * pixels inside the mask render at full alpha with
                     * their RGB. */
                    const quint8 m = mask.constScanLine(clampY(v))[clampX(u)];
                    if (m <= 127 || s[3] <= 0.5)
                        continue;
                    alpha = 255.0;
                } else {
                    alpha = s[3];                /* no mask: texture alpha */
                    if (alpha <= 0.5)
                        continue;
                }
                const auto c8 = [](double c) {
                    return quint8(qBound(0.0, std::round(c), 255.0));
                };
                outRow[x * 4 + 0] = c8(s[2]);
                outRow[x * 4 + 1] = c8(s[1]);
                outRow[x * 4 + 2] = c8(s[0]);
                outRow[x * 4 + 3] = c8(alpha);
            }
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* PuppetMotion                                                        */
/* ------------------------------------------------------------------ */

PuppetPose PuppetMotion::sample(const PuppetRig &rig, MotionKind kind, float t01,
                                float amplitude, quint32 seed)
{
    PuppetPose pose = PuppetPose::rest(rig.skeleton);
    const int n = pose.anglesDeg.size();
    const float twoPiT = float(2.0 * kPi) * t01;
    const float H = float(rig.bounds.height());
    const auto ph = [&](int j) { return float(2.0 * kPi) * puppetHash01(seed, j); };

    auto set = [&](int j, float deg) { if (j >= 0 && j < n) pose.anglesDeg[j] = deg; };

    switch (kind) {
    case MotionKind::Sway: {
        const float A = 2.5f * amplitude;
        const float w = std::sin(twoPiT + ph(1));
        const float wArm = std::sin(twoPiT + ph(4) + 0.7f);
        pose.rootOffset = QPointF(0.0035f * H * amplitude * std::sin(twoPiT + ph(0)), 0);
        set(1, A * w);                       /* chest roll */
        set(2, -0.6f * A * w);               /* neck counter */
        set(3, -0.25f * A * w);              /* head counter */
        set(4, 0.35f * A * wArm);            /* shoulders follow */
        set(6, 0.35f * A * wArm);
        set(5, 0.7f * A * std::sin(twoPiT + ph(4) + 1.1f));  /* elbows lag */
        set(7, 0.7f * A * std::sin(twoPiT + ph(4) - 1.1f));
        set(8, 0.2f * A * std::sin(twoPiT + ph(8)));
        set(9, -0.2f * A * std::sin(twoPiT + ph(8)));
        break;
    }
    case MotionKind::Bounce: {
        const float A = 2.0f * amplitude;
        const float sq = std::sin(float(kPi) * t01);
        pose.rootOffset = QPointF(0, -0.006f * H * amplitude * sq * sq);
        set(1, 0.6f * A * std::sin(twoPiT + ph(1)));
        set(3, 0.3f * A * std::sin(twoPiT + ph(3)));
        set(5, 1.0f * A * std::sin(twoPiT + ph(5) + 0.8f));
        set(7, 1.0f * A * std::sin(twoPiT + ph(7) - 0.8f));
        set(8, 0.4f * A * std::sin(twoPiT + ph(8) + 0.3f));
        set(9, -0.4f * A * std::sin(twoPiT + ph(9) + 0.3f));
        break;
    }
    case MotionKind::Breath: {
        const float A = amplitude;
        const float w = std::sin(twoPiT + ph(1));
        set(1, 0.7f * A * w);                /* ribcage */
        set(2, 0.15f * A * std::sin(twoPiT + ph(2) + 0.5f));
        set(4, 0.9f * A * w);                /* shoulders lift */
        set(6, 0.9f * A * w);
        break;                               /* head stays exactly 0 */
    }
    case MotionKind::HeadTilt: {
        const float w = std::sin(twoPiT + ph(3));
        set(3, 4.0f * amplitude * w);
        set(2, 0.3f * amplitude * std::sin(twoPiT + ph(3) + float(kPi)));
        break;
    }
    case MotionKind::Composite: {
        const PuppetPose sw = sample(rig, MotionKind::Sway, t01, amplitude * 0.6f, seed);
        const PuppetPose bo = sample(rig, MotionKind::Bounce, t01, amplitude * 0.5f, seed);
        const PuppetPose br = sample(rig, MotionKind::Breath, t01, amplitude, seed);
        const PuppetPose ht = sample(rig, MotionKind::HeadTilt, t01, amplitude * 0.25f, seed);
        for (int j = 0; j < n; ++j)
            pose.anglesDeg[j] = sw.anglesDeg.value(j) + bo.anglesDeg.value(j)
                              + br.anglesDeg.value(j) + ht.anglesDeg.value(j);
        pose.rootOffset = sw.rootOffset + bo.rootOffset;
        break;
    }
    case MotionKind::Custom:
    default:
        break;                               /* rest pose */
    }
    return pose;
}

PuppetPose PuppetMotion::poseAt(const PuppetRig &rig, const PuppetEffect &effect,
                                qint64 timeMs)
{
    const double period = qMax<quint32>(1, effect.durationMs);
    const double tS = double(timeMs) * effect.speed;
    const double cycles = tS / period;
    double t01;
    float ampScale = 1.0f;
    switch (effect.loop) {
    case LoopMode::Once: {
        const double c = qMin(cycles, 1.0);
        t01 = c;
        /* ease the amplitude to zero over the last quarter */
        const double e = qBound(0.0, (c - 0.75) / 0.25, 1.0);
        ampScale = float(1.0 - e * e * (3.0 - 2.0 * e));
        break;
    }
    case LoopMode::PingPong: {
        const double fold = std::fmod(cycles, 2.0);
        t01 = fold < 1.0 ? fold : 2.0 - fold;
        break;
    }
    case LoopMode::Loop:
    default:
        t01 = cycles - std::floor(cycles);
        break;
    }
    return sample(rig, effect.kind, float(t01), effect.amplitude * ampScale,
                  effect.seed);
}

/* ------------------------------------------------------------------ */
/* PuppetMotionClip                                                    */
/* ------------------------------------------------------------------ */

qint64 PuppetMotionClip::durationMs() const
{
    return keys.isEmpty() ? 1 : qMax<qint64>(1, keys.last().timeMs);
}

bool PuppetMotionClip::isValid(const PuppetSkeleton &skel) const
{
    if (keys.size() < 2)
        return false;
    qint64 prev = -1;
    for (const PuppetClipKey &k : keys) {
        if (k.timeMs < prev)
            return false;
        prev = k.timeMs;
        if (k.pose.anglesDeg.size() != skel.joints.size())
            return false;
    }
    return keys.first().timeMs == 0;
}

PuppetPose PuppetMotionClip::poseAt(const PuppetSkeleton &skel, qint64 timeMs) const
{
    PuppetPose rest = PuppetPose::rest(skel);
    if (keys.isEmpty())
        return rest;
    if (keys.first().pose.anglesDeg.size() != skel.joints.size())
        return rest;
    if (keys.size() == 1)
        return keys.first().pose;

    const double duration = double(durationMs());
    double t = double(timeMs);
    switch (loop) {
    case LoopMode::Once:
        t = qMin(t, duration);
        break;
    case LoopMode::PingPong: {
        const double fold = std::fmod(t, 2.0 * duration);
        t = fold < duration ? fold : 2.0 * duration - fold;
        break;
    }
    case LoopMode::Loop:
    default:
        t = std::fmod(t, duration);
        break;
    }
    if (t <= 0.0)
        return keys.first().pose;
    if (t >= duration)
        return keys.last().pose;

    /* linear segment search + interpolation (deterministic) */
    int lo = 0, hi = keys.size() - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (keys[mid].timeMs <= t)
            lo = mid;
        else
            hi = mid;
    }
    const PuppetPose &a = keys[lo].pose;
    const PuppetPose &b = keys[hi].pose;
    const double span = double(keys[hi].timeMs - keys[lo].timeMs);
    const double frac = span > 0 ? (t - double(keys[lo].timeMs)) / span : 0.0;
    PuppetPose out = a;
    for (int j = 0; j < a.anglesDeg.size(); ++j)
        out.anglesDeg[j] = float(a.anglesDeg[j]
                                 + (b.anglesDeg.value(j, 0.0f) - a.anglesDeg[j]) * frac);
    out.rootOffset = QPointF(a.rootOffset.x() + (b.rootOffset.x() - a.rootOffset.x()) * frac,
                             a.rootOffset.y() + (b.rootOffset.y() - a.rootOffset.y()) * frac);
    return out;
}

QByteArray PuppetMotionClip::serialize() const
{
    QByteArray out;
    out.reserve(16 + keys.size() * 64);
    out.append(char(1));                                   /* version */
    out.append(char(quint8(loop)));
    const auto u32 = [](quint32 v) {
        QByteArray b(4, char(0));
        b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
        b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
        return b;
    };
    const auto u64 = [&](quint64 v) {
        return u32(quint32(v & 0xFFFFFFFFu)) + u32(quint32(v >> 32));
    };
    out.append(u32(quint32(keys.size())));
    for (const PuppetClipKey &k : keys) {
        out.append(u64(quint64(k.timeMs)));
        out.append(u32(quint32(k.pose.anglesDeg.size())));
        for (float angle : k.pose.anglesDeg) {
            quint32 bits; std::memcpy(&bits, &angle, 4);
            out.append(u32(bits));
        }
        const auto f32 = [&](double v) {
            float f = float(v); quint32 bits; std::memcpy(&bits, &f, 4);
            return u32(bits);
        };
        out.append(f32(k.pose.rootOffset.x()));
        out.append(f32(k.pose.rootOffset.y()));
    }
    return out;
}

PuppetMotionClip PuppetMotionClip::deserialize(const QByteArray &data)
{
    PuppetMotionClip clip;
    if (data.size() < 6 || data[0] != char(1))
        return clip;
    const quint8 l = quint8(data[1]);
    if (l > quint8(LoopMode::PingPong))
        return clip;
    clip.loop = LoopMode(l);
    const auto u32at = [&](int at) {
        quint32 v = 0;
        for (int i = 0; i < 4; ++i)
            v |= quint32(quint8(data[at + i])) << (8 * i);
        return v;
    };
    const auto u64at = [&](int at) {
        return quint64(u32at(at)) | (quint64(u32at(at + 4)) << 32);
    };
    const quint32 keyCount = u32at(2);
    if (keyCount > 4096)
        return clip;
    int at = 6;
    bool ok = true;
    for (quint32 i = 0; i < keyCount && ok; ++i) {
        if (at + 8 + 4 > data.size()) { ok = false; break; }
        PuppetClipKey k;
        k.timeMs = qint64(u64at(at));
        at += 8;
        const quint32 n = u32at(at);
        at += 4;
        if (n > 64 || at + int(n) * 4 + 8 > data.size()) { ok = false; break; }
        k.pose.anglesDeg.resize(int(n));
        for (quint32 j = 0; j < n; ++j) {
            quint32 bits = u32at(at); at += 4;
            std::memcpy(&k.pose.anglesDeg[int(j)], &bits, 4);
        }
        auto f32at = [&]() {
            quint32 bits = u32at(at); at += 4;
            float f; std::memcpy(&f, &bits, 4);
            return f;
        };
        /* locals: QPointF(f32at(), f32at()) would be order-unspecified */
        const float rx = f32at();
        const float ry = f32at();
        k.pose.rootOffset = QPointF(rx, ry);
        clip.keys.append(k);
    }
    if (!ok || clip.keys.isEmpty() || clip.keys.first().timeMs != 0)
        return PuppetMotionClip();
    return clip;
}

/* ------------------------------------------------------------------ */
/* PuppetRetargeter                                                    */
/* ------------------------------------------------------------------ */

namespace {

QString normJointName(const QString &name)
{
    QString n = name.toLower();
    n.remove(QChar('_'));
    n.remove(QChar('-'));
    n.remove(QChar(' '));
    return n;
}

/* alias variants -> canonical normalized name ("", = unmapped) */
QString jointAlias(const QString &norm)
{
    static const QHash<QString, QString> aliases = {
        { "pelvis", "root" }, { "hips", "root" }, { "hip", "root" },
        { "spine", "chest" }, { "torso", "chest" }, { "thorax", "chest" },
        { "shoulderl", "lshoulder" }, { "shoulderleft", "lshoulder" },
        { "shoulderr", "rshoulder" }, { "shoulderright", "rshoulder" },
        { "elbowl", "lelbow" }, { "elbowleft", "lelbow" },
        { "elbowr", "relbow" }, { "elbowright", "relbow" },
        { "hipl", "lhip" }, { "hipleft", "lhip" },
        { "hipr", "rhip" }, { "hipright", "rhip" },
        { "head", "head" }, { "neck", "neck" }, { "chest", "chest" },
        { "root", "root" }, { "lshoulder", "lshoulder" },
        { "rshoulder", "rshoulder" }, { "lelbow", "lelbow" },
        { "relbow", "relbow" }, { "lhip", "lhip" }, { "rhip", "rhip" },
    };
    return aliases.value(norm, QString());
}

double skeletonHeight(const PuppetSkeleton &skel)
{
    double h = 0.0;
    for (const PuppetJoint &j : skel.joints) {
        if (j.parent >= 0) {
            const QPointF d = j.rest - skel.joints[j.parent].rest;
            h += hypot(d.x(), d.y());
        }
    }
    return h;
}

} // namespace

PuppetPose PuppetRetargeter::poseFromPositions(const PuppetSkeleton &skel,
                                               const QVector<QPointF> &world)
{
    PuppetPose pose = PuppetPose::rest(skel);
    const int n = skel.joints.size();
    if (world.size() != n || n == 0)
        return pose;
    QVector<double> acc(n, 0.0);
    for (int j = 0; j < n; ++j) {
        const PuppetJoint &jnt = skel.joints[j];
        if (jnt.parent < 0) {
            pose.rootOffset = world[j] - jnt.rest;
            acc[j] = 0.0;
            continue;
        }
        const QPointF restDir = jnt.rest - skel.joints[jnt.parent].rest;
        const QPointF worldDir = world[j] - world[jnt.parent];
        const double rl = hypot(restDir.x(), restDir.y());
        const double wl = hypot(worldDir.x(), worldDir.y());
        if (rl < 1e-6 || wl < 1e-6) {
            acc[j] = acc[jnt.parent];
            continue;
        }
        /* absolute rotation from the rest direction to the world direction */
        const double cross = restDir.x() * worldDir.y() - restDir.y() * worldDir.x();
        const double dot = restDir.x() * worldDir.x() + restDir.y() * worldDir.y();
        acc[j] = std::atan2(cross / (rl * wl), dot / (rl * wl));
        pose.anglesDeg[j] = float(qRadiansToDegrees(acc[j] - acc[jnt.parent]));
    }
    return pose;
}

PuppetRetargeter::PoseTrackResult PuppetRetargeter::poseTrackFromMasks(
    const QVector<QImage> &masks, qint64 stepMs, bool closeLoop)
{
    PoseTrackResult r;
    if (masks.isEmpty() || stepMs <= 0)
        return r;
    PuppetSkeleton restSkel = PuppetBuilder::fitDefaultHumanoid(masks.first());
    if (!restSkel.isValid())
        return r;
    QVector<QVector<QPointF>> fits;
    for (const QImage &m : masks) {
        PuppetSkeleton sk = PuppetBuilder::fitDefaultHumanoid(m);
        if (!sk.isValid() || sk.joints.size() != restSkel.joints.size())
            return r;
        QVector<QPointF> world(sk.joints.size());
        for (int j = 0; j < sk.joints.size(); ++j)
            world[j] = sk.joints[j].rest;
        fits.append(world);
    }
    r.sourceSkeleton = restSkel;
    for (int i = 0; i < fits.size(); ++i) {
        PuppetClipKey k;
        k.timeMs = i * stepMs;
        k.pose = poseFromPositions(restSkel, fits[i]);
        r.clip.keys.append(k);
    }
    if (closeLoop && !r.clip.keys.isEmpty()) {
        PuppetClipKey close = r.clip.keys.first();
        close.timeMs = r.clip.keys.last().timeMs + stepMs;
        r.clip.keys.append(close);
    }
    r.clip.loop = LoopMode::Loop;
    return r;
}

PuppetPose PuppetRetargeter::retarget(const PuppetSkeleton &source,
                                      const PuppetSkeleton &target,
                                      const PuppetPose &sourcePose)
{
    return PuppetRetargeter::retarget(source, target, sourcePose, Options());
}

PuppetPose PuppetRetargeter::retarget(const PuppetSkeleton &source,
                                      const PuppetSkeleton &target,
                                      const PuppetPose &sourcePose,
                                      const Options &opts)
{
    PuppetPose out = PuppetPose::rest(target);
    if (sourcePose.anglesDeg.size() != source.joints.size())
        return out;
    QHash<QString, int> targetByName;
    for (int j = 0; j < target.joints.size(); ++j) {
        const QString norm = normJointName(target.joints[j].name);
        targetByName.insert(norm, j);
        const QString alias = jointAlias(norm);
        if (!alias.isEmpty())
            targetByName.insert(alias, j);
    }
    for (int s = 0; s < source.joints.size(); ++s) {
        const QString norm = normJointName(source.joints[s].name);
        int t = targetByName.value(norm, -1);
        if (t < 0) {
            const QString alias = jointAlias(norm);
            if (!alias.isEmpty())
                t = targetByName.value(alias, -1);
        }
        if (t < 0)
            continue;
        float a = sourcePose.anglesDeg.value(s, 0.0f) * opts.amplitudeScale;
        if (opts.clamp)
            a = qBound(-opts.maxJointDeg, a, opts.maxJointDeg);
        out.anglesDeg[t] = a;
    }
    const double sh = skeletonHeight(source), th = skeletonHeight(target);
    if (sh > 1e-6)
        out.rootOffset = QPointF(sourcePose.rootOffset.x() * (th / sh),
                                 sourcePose.rootOffset.y() * (th / sh));
    return out;
}

PuppetMotionClip PuppetRetargeter::retargetClip(const PuppetSkeleton &source,
                                                const PuppetSkeleton &target,
                                                const PuppetMotionClip &clip)
{
    return PuppetRetargeter::retargetClip(source, target, clip, Options());
}

PuppetMotionClip PuppetRetargeter::retargetClip(const PuppetSkeleton &source,
                                                const PuppetSkeleton &target,
                                                const PuppetMotionClip &clip,
                                                const Options &opts)
{
    PuppetMotionClip out = clip;
    if (clip.keys.isEmpty() || source.joints.isEmpty() || target.joints.isEmpty())
        return out;
    /* per mapped joint: peak |angle| over the whole track (the loop-close
     * duplicate key excluded from the range scan) */
    QVector<float> peak(source.joints.size(), 0.0f);
    const int scanEnd = qMax(1, clip.keys.size() - (clip.loop == LoopMode::Loop ? 1 : 0));
    for (int i = 0; i < scanEnd; ++i) {
        const PuppetPose &p = clip.keys[i].pose;
        for (int s = 0; s < source.joints.size() && s < p.anglesDeg.size(); ++s)
            peak[s] = qMax(peak[s], std::abs(p.anglesDeg[s]));
    }
    /* photo-regime adaptation: scale each mapped joint so its peak fits
     * maxJointDeg (motion DIRECTION preserved, magnitude bounded) */
    QHash<QString, int> targetByName;
    for (int j = 0; j < target.joints.size(); ++j) {
        const QString norm = normJointName(target.joints[j].name);
        targetByName.insert(norm, j);
        const QString alias = jointAlias(norm);
        if (!alias.isEmpty())
            targetByName.insert(alias, j);
    }
    QVector<float> jointScale(source.joints.size(), 0.0f);
    for (int s = 0; s < source.joints.size(); ++s) {
        const QString norm = normJointName(source.joints[s].name);
        int t = targetByName.value(norm, -1);
        if (t < 0) {
            const QString alias = jointAlias(norm);
            if (!alias.isEmpty())
                t = targetByName.value(alias, -1);
        }
        if (t < 0)
            continue;
        float scale = opts.amplitudeScale;
        if (opts.clamp && peak[s] > 1e-3f)
            scale *= qMin(1.0f, opts.maxJointDeg / peak[s]);
        jointScale[s] = scale;
    }
    for (int i = 0; i < out.keys.size(); ++i) {
        PuppetPose p = out.keys[i].pose;
        for (int s = 0; s < source.joints.size() && s < p.anglesDeg.size(); ++s)
            p.anglesDeg[s] *= jointScale[s];
        const double sh = skeletonHeight(source), th = skeletonHeight(target);
        if (sh > 1e-6)
            p.rootOffset = QPointF(p.rootOffset.x() * (th / sh),
                                   p.rootOffset.y() * (th / sh));
        out.keys[i].pose = p;
    }
    /* root-displacement photo-regime bound: scale the whole root track so
     * its peak fits maxRootFrac of the target height (the absolute source
     * frame position of a subject is not "motion" — a sticker must bob,
     * not slide) */
    if (opts.clamp) {
        double peakRoot = 0.0;
        for (const PuppetClipKey &k : out.keys)
            peakRoot = qMax(peakRoot, hypot(k.pose.rootOffset.x(),
                                            k.pose.rootOffset.y()));
        const double rootBound = double(opts.maxRootFrac) * skeletonHeight(target);
        if (peakRoot > 1e-6 && peakRoot > rootBound) {
            const double rootScale = rootBound / peakRoot;
            for (PuppetClipKey &k : out.keys)
                k.pose.rootOffset = QPointF(k.pose.rootOffset.x() * rootScale,
                                            k.pose.rootOffset.y() * rootScale);
        }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* PuppetMotionScanner                                                 */
/* ------------------------------------------------------------------ */

PuppetMotionScanner::ScanResult PuppetMotionScanner::scan(
    const QVector<FrameSample> &samples, qint64 harvestPtsMs,
    const PuppetSkeleton &harvestSkeleton)
{
    return scan(samples, harvestPtsMs, harvestSkeleton, ScanOptions());
}

PuppetMotionScanner::ScanResult PuppetMotionScanner::scan(
    const QVector<FrameSample> &samples, qint64 harvestPtsMs,
    const PuppetSkeleton &harvestSkeleton, const ScanOptions &opts)
{
    ScanResult best;
    const int n = samples.size();
    if (n < opts.minSpanFrames + 1)
        return best;

    /* per-sample fits (positions only; a failed fit disqualifies the sample) */
    QVector<QVector<QPointF>> fits(n);
    QVector<double> areas(n, 0.0);
    for (int i = 0; i < n; ++i) {
        PuppetSkeleton sk = PuppetBuilder::fitDefaultHumanoid(samples[i].mask);
        if (!sk.isValid() || sk.joints.size() != harvestSkeleton.joints.size())
            continue;
        QVector<QPointF> world(sk.joints.size());
        for (int j = 0; j < sk.joints.size(); ++j)
            world[j] = sk.joints[j].rest;
        fits[i] = world;
        qint64 area = 0;
        for (int y = 0; y < samples[i].mask.height(); ++y) {
            const quint8 *row = samples[i].mask.constScanLine(y);
            for (int x = 0; x < samples[i].mask.width(); ++x)
                if (row[x] > 127)
                    ++area;
        }
        areas[i] = double(area);
    }

    /* anchor: the sample nearest the harvest PTS (t=0 == static figure) */
    int anchor = 0;
    qint64 bestDt = std::numeric_limits<qint64>::max();
    for (int i = 0; i < n; ++i) {
        const qint64 dt = std::llabs(samples[i].ptsMs - harvestPtsMs);
        if (dt < bestDt) { bestDt = dt; anchor = i; }
    }

    const int nj = harvestSkeleton.joints.size();
    const auto angleDist = [&](const PuppetPose &a, const PuppetPose &b) {
        double d = 0.0;
        for (int j = 0; j < nj; ++j) {
            const double dd = double(a.anglesDeg.value(j, 0.0f)
                                     - b.anglesDeg.value(j, 0.0f));
            d += dd * dd;
        }
        return std::sqrt(d / qMax(1, nj));
    };

    for (int s = 0; s + opts.minSpanFrames < n; ++s) {
        if (fits[s].isEmpty())
            continue;
        for (int span = opts.minSpanFrames; span <= opts.maxSpanFrames; ++span) {
            const int e = s + span;
            if (e >= n)
                break;
            bool allOk = true;
            for (int i = s; i <= e; ++i)
                if (fits[i].isEmpty()) { allOk = false; break; }
            if (!allOk)
                continue;

            /* poses relative to the window rest (fit of sample s) */
            QVector<PuppetPose> poses;
            double energy = 0.0;
            for (int i = s; i <= e; ++i) {
                PuppetPose p = PuppetRetargeter::poseFromPositions(harvestSkeleton, fits[i]);
                poses.append(p);
                for (int j = 0; j < nj; ++j)
                    energy += std::abs(p.anglesDeg.value(j, 0.0f));
            }
            energy /= double(poses.size() * qMax(1, nj));

            /* variation: mean per-joint std over the window (excl. close) */
            double variation = 0.0;
            for (int j = 0; j < nj; ++j) {
                double mean = 0.0;
                for (int i = 0; i < poses.size(); ++i)
                    mean += poses[i].anglesDeg.value(j, 0.0f);
                mean /= poses.size();
                double var = 0.0;
                for (int i = 0; i < poses.size(); ++i) {
                    const double d = poses[i].anglesDeg.value(j, 0.0f) - mean;
                    var += d * d;
                }
                variation += std::sqrt(var / poses.size());
            }
            variation /= double(qMax(1, nj));

            if (variation < opts.minVariation)
                continue;                        /* static window */

            /* closure cost: the last pose vs the rest (loop closes to the
             * window's first frame) */
            const double closure = angleDist(poses.last(),
                                             PuppetPose::rest(harvestSkeleton));
            /* character match: mean window pose vs the harvested pose
             * (angle-only — translation/scale invariant) */
            PuppetPose meanPose = PuppetPose::rest(harvestSkeleton);
            for (int i = 0; i < poses.size(); ++i)
                for (int j = 0; j < nj; ++j)
                    meanPose.anglesDeg[j] += poses[i].anglesDeg.value(j, 0.0f)
                                             / float(poses.size());
            const double character = angleDist(meanPose, PuppetPose::rest(harvestSkeleton));

            /* occlusion: mask-area variance relative to the window median */
            double med = areas[s];
            for (int i = s; i <= e; ++i)
                med = qMin(med, areas[i]);
            double occ = 0.0;
            if (med > 1.0)
                for (int i = s; i <= e; ++i)
                    occ += std::abs(areas[i] - med) / med;
            occ /= double(span + 1);

            double quality = 0.0;
            for (int i = s; i <= e; ++i)
                quality += samples[i].quality;
            quality /= double(span + 1);

            const double score = opts.variationWeight * qMin(variation, 12.0)
                + opts.characterWeight * qMax(0.0, 12.0 - character)
                + opts.closureWeight * qMax(0.0, 12.0 - closure)
                - opts.energyPenalty * qMax(0.0, energy - opts.maxEnergy)
                - opts.occlusionWeight * occ
                + quality * 0.5
                + (s == anchor ? opts.anchorBonus : 0.0);

            if (score > best.score || !best.ok) {
                best.ok = true;
                best.score = score;
                best.startIndex = s;
                best.spanFrames = span;
                best.sourceSkeleton = PuppetBuilder::fitDefaultHumanoid(samples[s].mask);
                best.clip.keys.clear();
                for (int i = s; i <= e; ++i) {
                    PuppetClipKey k;
                    k.timeMs = (i - s) * opts.stepMs;
                    k.pose = PuppetRetargeter::poseFromPositions(best.sourceSkeleton, fits[i]);
                    best.clip.keys.append(k);
                }
                PuppetClipKey close = best.clip.keys.first();
                close.timeMs = best.clip.keys.last().timeMs + opts.stepMs;
                best.clip.keys.append(close);
                best.clip.loop = LoopMode::Loop;
            }
        }
    }
    return best;
}

PuppetMotionClip PuppetMotionScanner::proceduralFallback(const PuppetRig &rig,
                                                         quint32 durationMs,
                                                         quint32 seed)
{
    PuppetMotionClip clip;
    clip.loop = LoopMode::Loop;
    const int keys = 8;
    /* the clip opens and closes at REST: time 0 with phase 0 renders the
     * static harvested figure exactly, and the loop is seamless by
     * construction (first key == close key) */
    clip.keys.append(PuppetClipKey{ 0, PuppetPose::rest(rig.skeleton) });
    for (int i = 1; i < keys; ++i) {
        PuppetClipKey k;
        k.timeMs = i * (qint64(durationMs) / keys);
        k.pose = PuppetMotion::sample(rig, MotionKind::Sway,
                                      float(i) / keys, 1.0f, seed);
        clip.keys.append(k);
    }
    PuppetClipKey close = clip.keys.first();
    close.timeMs = clip.keys.last().timeMs + qint64(durationMs) / keys;
    clip.keys.append(close);
    return clip;
}

QImage PuppetRuntime::renderAtTime(const PuppetAsset &asset,
                                   const PuppetEffect &effect, qint64 timeMs)
{
    if (!asset.isValid())
        return QImage();
    const qint64 period = asset.motion.durationMs();
    const qint64 phaseShift = qint64(double(period) * qBound(0.0f, effect.phase, 1.0f));
    PuppetPose pose = asset.motion.poseAt(asset.rig.skeleton,
                                          qint64(double(timeMs + phaseShift) * effect.speed));
    pose = pose.scaled(effect.amplitude);
    const QVector<QPointF> deformed = PuppetDeformer::deform(asset.rig, pose);
    return PuppetRenderer::render(asset.rig.texture, asset.rig.mask,
                                  asset.rig.mesh.verts, deformed, asset.rig.mesh.tris);
}

QString PuppetRuntime::rigId(const PuppetRig &rig)
{
    quint64 h = fnv1a64("pb-puppet-rig");
    if (!rig.mask.isNull()) {
        for (int y = 0; y < rig.mask.height(); ++y)
            h = fnv1a64(rig.mask.constScanLine(y), rig.mask.width(), h);
    }
    h = fnv1a64(reinterpret_cast<const quint8 *>(&h), sizeof(h), h);
    const quint32 vc = quint32(rig.mesh.verts.size());
    h = fnv1a64(reinterpret_cast<const quint8 *>(&vc), sizeof(vc), h);
    for (const QPointF &v : rig.mesh.verts)
        hashPoint(h, v, 1000.0);
    const quint32 tc = quint32(rig.mesh.tris.size());
    h = fnv1a64(reinterpret_cast<const quint8 *>(&tc), sizeof(tc), h);
    for (const auto &t : rig.mesh.tris)
        h = fnv1a64(reinterpret_cast<const quint8 *>(t.data()), int(sizeof(int) * 3), h);
    for (const PuppetJoint &j : rig.skeleton.joints) {
        h = fnv1a64(reinterpret_cast<const quint8 *>(&j.parent), sizeof(int), h);
        hashPoint(h, j.rest, 1000.0);
    }
    return QStringLiteral("rig-%1").arg(h, 16, 16, QLatin1Char('0'));
}

PuppetRig PuppetRuntime::build(const QImage &rgba, const QImage &mask,
                               const PuppetBuilderConfig &config)
{
    return PuppetBuilder::build(rgba, mask, config);
}

QImage PuppetRuntime::renderFrame(const PuppetRig &rig, const PuppetEffect &effect,
                                  qint64 timeMs)
{
    if (!rig.isValid())
        return QImage();
    const PuppetPose pose = PuppetMotion::poseAt(rig, effect, timeMs);
    const QVector<QPointF> deformed = PuppetDeformer::deform(rig, pose);
    return PuppetRenderer::render(rig.texture, rig.mask, rig.mesh.verts, deformed,
                                  rig.mesh.tris);
}

QImage PuppetRuntime::renderClipFrame(const PuppetRig &rig,
                                      const PuppetMotionClip &clip,
                                      const PuppetEffect &effect, qint64 timeMs)
{
    if (!rig.isValid())
        return QImage();
    PuppetPose pose = clip.poseAt(rig.skeleton, qint64(double(timeMs) * effect.speed));
    pose = pose.scaled(effect.amplitude);
    const QVector<QPointF> deformed = PuppetDeformer::deform(rig, pose);
    return PuppetRenderer::render(rig.texture, rig.mask, rig.mesh.verts, deformed,
                                  rig.mesh.tris);
}

} // namespace motion
} // namespace paintbrush
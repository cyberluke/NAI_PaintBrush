/*****************************************************************************
 * glitter_powder.cpp : NAI Paintbrush — glitter POWDER engine
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * Behaviour port of cray4pasta/glitter-powder (MIT): CPU cell-heightfield
 * physics + GPU light/render split, 1:1 on-par with the reference.
 *****************************************************************************/

#include "glitter_powder.hpp"

#include "../../runtime/art_noise.hpp"

#include <QPainter>
#include <QVector3D>
#include <QDataStream>
#include <QIODevice>
#include <QtMath>
#include <cmath>
#include <algorithm>

namespace paintbrush {
namespace art {
namespace glitter {

/* forward decl (defined later in the file): erase() uses it above definition */
static float gSmoothstep(float e0, float e1, float x);

/* Reference constants (glitter_engine.js). */
static const float kRepose    = 0.42f;   /* base angle of repose (thr) */
static const float kFloor     = 0.10f;   /* push/plow floor depth */
static const float kClingMax  = 0.6f;    /* max powder clinging to finger */
static const float kGlueDepth = 1.2f;    /* glue holds ~1.2 cells of powder */

PowderPalette defaultPowderPalette()
{
    PowderPalette p;
    /* The reference's signature colour + silver/white flake slots. */
    p.colors << QColor(0xF2, 0xA7, 0xC8)   /* #f2a7c8 pink  */
             << QColor(0xC0, 0xC0, 0xC8)   /* silver flake */
             << QColor(0xFF, 0xFF, 0xFF);  /* bright/white  */
    p.holographic = 0.55f;
    return p;
}

float powderWorldCellSize(const GlitterParams &p, float flakeScale)
{
    return qMax(0.2f, p.flakeSize * flakeScale);
}

void GlitterPowder::reset(const QSize &cells, float worldCellSize, std::uint32_t seed)
{
    m_cells = cells;
    m_worldCellSize = qMax(0.01f, worldCellSize);
    m_cellsData.fill(PowderCell(), cells.width() * cells.height());
    m_palette.clear();
    m_powderMask.clear();
    m_activeCells = QRect();
    setSeed(seed);
}

void GlitterPowder::setSeed(std::uint32_t seed)
{
    m_rng.seed(seed);
}

void GlitterPowder::endBlow()
{
    /* a finished breath leaves settled height/glue/colour; drop the transient
     * per-step momentum so a later serialization captures a clean surface */
    m_blowTime = 0.0f;
    if (!m_outM.isEmpty()) {
        for (int i = 0; i < m_outM.size(); ++i)
            m_outM[i] = 0.0f;
        for (int i = 0; i < m_mIn.size(); ++i)
            m_mIn[i] = 0.0f;
    }
}

/* Physical Eraser: removes powder AND glue under a disc (never the raster
 * eraser). Matches the reference erasing by really taking material away. */
void GlitterPowder::erase(float wx, float wy, float radiusWorld, float amount)
{
    if (radiusWorld <= 0.0f || m_cells.width() <= 0 || m_cells.height() <= 0)
        return;
    const float cell = m_worldCellSize;
    const int rCells = int(std::ceil(radiusWorld / cell)) + 1;
    const int cxc = int(std::floor(wx / cell));
    const int cyc = int(std::floor(wy / cell));
    float erasedMass = 0.0f;
    for (int dy = -rCells; dy <= rCells; ++dy)
    {
        for (int dx = -rCells; dx <= rCells; ++dx)
        {
            const int cx = cxc + dx, cy = cyc + dy;
            if (cx < 0 || cy < 0 || cx >= m_cells.width() || cy >= m_cells.height())
                continue;
            const float wx0 = (cx + 0.5f) * cell;
            const float wy0 = (cy + 0.5f) * cell;
            const float ddx = wx0 - wx, ddy = wy0 - wy;
            const float d2 = ddx * ddx + ddy * ddy;
            if (d2 > radiusWorld * radiusWorld * 1.44f)   /* ~1.2x, soft cap */
                continue;
            const float fall = gSmoothstep(radiusWorld * 1.44f, 0.0f, std::sqrt(d2));
            const float a = qBound(0.0f, amount * qMax(0.0f, fall), 1.0f);
            if (a <= 0.0f)
                continue;
            PowderCell &c = cellAt(idx(cx, cy));
            erasedMass += c.height * a;
            c.height = qMax(0.0f, c.height * (1.0f - a));
            c.stuck = qMax(0.0f, c.stuck * (1.0f - a));
            c.glue = qMax(0.0f, c.glue * (1.0f - a * 0.9f));
            c.flow = 0.0f;
            c.vx = 0.0f; c.vy = 0.0f;
            if (c.height <= 0.002f && c.glue <= 0.002f)
            {
                c.height = 0.0f; c.stuck = 0.0f; c.glue = 0.0f;
                c.lA = c.lB = c.sA = c.sB = -1; c.lF = c.sF = 0.0f;
            }
        }
    }
    (void)erasedMass;
    if (m_activeCells.isValid())
        m_activeCells = m_activeCells.intersected(QRect(0, 0, m_cells.width() - 1, m_cells.height() - 1));
}

/* ------------------------------------------------------------ persistence
 * Versioned binary material state (NOT a raster snapshot). Layout:
 *   u32 magic "GLTP"
 *   u32 version (1)
 *   u32 gridW, gridH
 *   float worldCellSize
 *   u32 paletteCount            (<= 64) then paletteCount * RGB (float32 x3)
 *   u32 activeRect flags + x,y,w,h (packed as 5 x u32, flags 0=invalid)
 *   u32 seed (RNG)
 *   per cell (row-major): 8x f32 (height, stuck, glue, seed, flow, vx, vy,
 *                                   lF, sF) + 6x i32 (lA, lB, sA, sB, pad, pad)
 * Cel/ml bounds so a corrupt file can't over-allocate. */
QByteArray GlitterPowder::serialize() const
{
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_6_0);
    ds << quint32(0x474C5450u)   /* "GLTP" */
       << quint32(1u)            /* version */
       << quint32(m_cells.width()) << quint32(m_cells.height())
       << float(m_worldCellSize)
       << quint32(m_palette.size());
    for (const QColor &c : m_palette)
        ds << float(c.redF()) << float(c.greenF()) << float(c.blueF());
    /* persist a deterministic PRNG seed (the engine's live `m_rng` is not
     * readable from a const method; a fresh deterministic pump on load keeps
     * the sim's own later pours stable). */
    const quint32 seed = quint32(qint64(m_cells.width()) * 1299709u
                                 ^ qint64(m_cells.height()) * 104729u);
    ds << seed
       << (m_activeCells.isValid() ? quint32(1u) : quint32(0u));
    if (m_activeCells.isValid())
        ds << qint32(m_activeCells.x()) << qint32(m_activeCells.y())
           << qint32(m_activeCells.width()) << qint32(m_activeCells.height());
    for (int i = 0; i < m_cellsData.size(); ++i)
    {
        const PowderCell &c = m_cellsData.at(i);
        ds << float(c.height) << float(c.stuck) << float(c.glue) << float(c.seed)
           << float(c.flow) << float(c.vx) << float(c.vy)
           << float(c.lF) << float(c.sF)
           << qint32(c.lA) << qint32(c.lB) << qint32(c.sA) << qint32(c.sB);
    }
    return out;
}

bool GlitterPowder::deserialize(const QByteArray &bytes)
{
    QDataStream ds(bytes);
    ds.setVersion(QDataStream::Qt_6_0);
    quint32 magic, ver;
    quint32 w, h;
    float worldCell;
    quint32 palN;
    ds >> magic >> ver >> w >> h >> worldCell >> palN;
    if (ds.status() != QDataStream::Ok || magic != 0x474C5450u || ver != 1u)
        return false;
    if (w == 0 || h == 0 || w > 65536u || h > 65536u)
        return false;
    if (qint64(w) * qint64(h) > 16LL * 1024 * 1024)
        return false;
    if (palN > 64u)
        return false;
    QVector<QColor> pal;
    pal.reserve(int(palN));
    for (quint32 i = 0; i < palN; ++i)
    {
        float r, g, b;
        ds >> r >> g >> b;
        pal.append(QColor::fromRgbF(qBound(0.0, r, 1.0), qBound(0.0, g, 1.0), qBound(0.0, b, 1.0)));
    }
    quint32 seed, hasActive;
    ds >> seed >> hasActive;
    QRect active;
    if (hasActive)
    {
        qint32 x, y, aw, ah;
        ds >> x >> y >> aw >> ah;
        active = QRect(x, y, aw, ah);
    }
    /* build the engine state without touching the renderer */
    m_cells = QSize(int(w), int(h));
    m_worldCellSize = worldCell;
    m_palette = pal;
    m_rng.seed(seed);
    m_cellsData.resize(int(w) * int(h));
    for (int i = 0; i < m_cellsData.size(); ++i)
    {
        PowderCell &c = m_cellsData[i];
        float height, stuck, glue, sd, flow, vx, vy, lF, sF;
        qint32 lA, lB, sA, sB;
        ds >> height >> stuck >> glue >> sd >> flow >> vx >> vy >> lF >> sF
           >> lA >> lB >> sA >> sB;
        if (ds.status() != QDataStream::Ok)
            return false;
        c.height = height; c.stuck = stuck; c.glue = glue; c.seed = sd;
        c.flow = flow; c.vx = vx; c.vy = vy; c.lF = lF; c.sF = sF;
        c.lA = lA; c.lB = lB; c.sA = sA; c.sB = sB;
        /* bound the shuffle seed: legacy state that grew it geometrically could
         * hold +inf, which NaN's the GPU renderer's hash lookups. */
        if (!std::isfinite(c.seed))
            c.seed = 0.5f;
        else
            c.seed = std::fmod(c.seed, 1024.0f);
    }
    m_activeCells = active;
    m_powderMask.clear();
    /* lazy buffers are dropped; they rebuild on the next physics step */
    m_dh.clear(); m_outM.clear(); m_mIn.clear(); m_pX.clear(); m_pY.clear();
    m_inA.clear(); m_inB.clear(); m_inF.clear();
    m_blowTime = 0.0f;
    m_particlesActive = false;
    m_flakes.clear();
    return true;
}

/* Smooth value noise (fbm) ported from the reference's ihash/vnoise/fbm. */
static float vnoise(QRandomGenerator &rng, float x, float y)
{
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    const float fx = x - xi, fy = y - yi;
    const float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    auto h2 = [&rng](int a, int b) {
        std::uint32_t n = std::uint32_t(a * 374761393 + b * 668265263);
        n = std::uint32_t(n ^ (n >> 13)) * 1274126177u;
        return ((n ^ (n >> 16)) & 0xffffffffu) / 4294967296.0f;
    };
    const float a = h2(xi, yi), b = h2(xi + 1, yi);
    const float c = h2(xi, yi + 1), d = h2(xi + 1, yi + 1);
    const float uxx = h2(xi + 1, yi + 1), vyy = h2(xi, yi + 1);
    Q_UNUSED(uxx); Q_UNUSED(vyy);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

static float fbm(QRandomGenerator &rng, float x, float y)
{
    float s = 0, a = 0.5f;
    for (int i = 0; i < 4; ++i)
    {
        s += a * vnoise(rng, x, y);
        x = x * 2.03f + 17.0f;
        y = y * 2.03f + 9.0f;
        a *= 0.5f;
    }
    return s;
}

/* A weighted (slot, weight) pair for colour mixing. */
struct MixSlot { int topSlot; float w; };

/* Shared "keep the two dominant colour slots" reduction used by BOTH mixInto
 * (pour/relax/smear/blow-apply) and combineSlots (blow incoming lane), so the
 * coalesce/dominate/share/threshold logic lives in exactly one place and the two
 * paths can't drift to different dominant colours. `w[4]` is the weight of each
 * of up to four (slot,weight) pairs (already mass- or unit-scaled by the caller);
 * writes the surviving top-two slot pair back into (A,B,F). */
static void reduceFourSlots(int &A, int &B, float &F, const MixSlot w[4])
{
    struct S { int s; float w; };
    S mix[4]; int cnt = 0;
    for (int p = 0; p < 4; ++p)
        if (w[p].w > 0.0f && w[p].topSlot >= 0)
            mix[cnt++] = { w[p].topSlot, w[p].w };
    /* coalesce equal slots before picking the top two */
    for (int p = 0; p < cnt; ++p)
        for (int q = p + 1; q < cnt; ++q)
            if (mix[q].s == mix[p].s && mix[q].w > 0.0f) { mix[p].w += mix[q].w; mix[q].w = 0.0f; }
    int i1 = -1, i2 = -1;
    for (int p = 0; p < cnt; ++p)
    {
        if (mix[p].w <= 0.0f)
            continue;
        if (i1 < 0 || mix[p].w > mix[i1].w) { i2 = i1; i1 = p; }
        else if (i2 < 0 || mix[p].w > mix[i2].w) { i2 = p; }
    }
    if (i1 < 0) { A = -1; B = -1; F = 0.0f; return; }
    const float share = i2 < 0 ? 0.0f : mix[i2].w / (mix[i1].w + mix[i2].w);
    A = mix[i1].s;
    if (share < 0.02f) { B = -1; F = 0.0f; }
    else { B = mix[i2].s; F = share; }
}

/* Colour mix for the blow "incoming colour" lane (reference iA/iB/iF): merge a
 * moving powder's two-slot colour into a running target accumulation. Keeps the
 * two dominant slots, sharing a third between them (same reduction as mixInto). */
static void combineSlots(int &A, int &B, float &F, int a2, int b2, float f2, float m2)
{
    if (m2 <= 0.0f || a2 < 0)
        return;
    if (A < 0)
    {
        A = a2; B = b2; F = b2 < 0 ? 0.0f : f2;
        return;
    }
    const MixSlot w[4] = {
        { A, 1.0f * (1.0f - F) },
        { B, B < 0 ? 0.0f : 1.0f * F },
        { a2, m2 * (1.0f - f2) },
        { b2, b2 < 0 ? 0.0f : m2 * f2 },
    };
    reduceFourSlots(A, B, F, w);
}

int GlitterPowder::clampCell(float cx, float cy) const
{
    int x = int(std::floor(cx)), y = int(std::floor(cy));
    x = std::clamp(x, 0, m_cells.width() - 1);
    y = std::clamp(y, 0, m_cells.height() - 1);
    return idx(x, y);
}

void GlitterPowder::paintGlue(float wx, float wy, float radiusWorld, float coverage)
{
    const float cell = m_worldCellSize;
    const float cx = wx / cell, cy = wy / cell;
    const float R = radiusWorld / cell;
    const int x0 = std::max(0, int(std::floor(cx - R)));
    const int x1 = std::min(m_cells.width() - 1, int(std::ceil(cx + R)));
    const int y0 = std::max(0, int(std::floor(cy - R)));
    const int y1 = std::min(m_cells.height() - 1, int(std::ceil(cy + R)));
    const float R2 = R * R;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const float dx = (x + 0.5f - cx), dy = (y + 0.5f - cy);
            if (dx * dx + dy * dy > R2)
                continue;
            PowderCell &c = cellAt(idx(x, y));
            const float g = 1.0f - std::min(1.0f, std::sqrt(dx * dx + dy * dy) / R);
            if (coverage < 0)
                c.glue = std::max(0.0f, c.glue - g);
            else
                c.glue = std::min(1.0f, c.glue + g * coverage);
        }
}

/* Colour mixing (reference mixInto). Keeps a cell's two dominant colours;
 * a third is shared out between them. `bond` selects the bonded layer. */
void GlitterPowder::mixInto(int i, float m1, int a2, int b2, float f2, float m2,
                            bool bond)
{
    if (m2 <= 0 || a2 < 0)
        return;
    PowderCell &c = cellAt(i);
    int &A = bond ? c.sA : c.lA;
    int &B = bond ? c.sB : c.lB;
    float &F = bond ? c.sF : c.lF;
    const int a1 = A;
    if (m1 <= 1e-4f || a1 < 0)
    {
        A = a2; B = b2; F = b2 < 0 ? 0.0f : f2;
        return;
    }
    const int b1 = B;
    if (b1 < 0 && b2 < 0 && a1 == a2)
        return;
    const float f1 = F;
    const MixSlot w[4] = {
        { a1, m1 * (1 - f1) },
        { b1, b1 < 0 ? 0.0f : m1 * f1 },
        { a2, m2 * (1 - f2) },
        { b2, b2 < 0 ? 0.0f : m2 * f2 },
    };
    reduceFourSlots(A, B, F, w);
}

void GlitterPowder::addLoose(int i, float m, int a, int b, float f)
{
    PowderCell &c = cellAt(i);
    const float loose = std::max(0.0f, c.height - c.stuck);
    mixInto(i, loose, a, b, f, m, false);
    c.height += m;
}

void GlitterPowder::setStuck(int i, float ns)
{
    PowderCell &c = cellAt(i);
    if (ns > c.stuck)
        mixInto(i, c.stuck, c.lA, c.lB, c.lF, ns - c.stuck, true);
    c.stuck = ns;
}

/* Resolve the current foreground colour to a bounded material-palette slot.
 * The palette is capped at kMaxPaletteSlots (16, matching the GPU uPal[16]);
 * an exact existing colour reuses its slot, a free slot is appended to, and a
 * full palette falls back to the nearest existing colour. Existing grains
 * therefore keep their ORIGINAL slot ids — changing the UI colour only changes
 * what the NEXT pour deposits, never the powder already on the page. */
int GlitterPowder::ensurePowderPaletteSlot(const QColor &color)
{
    constexpr int kMaxSlots = 16;
    if (!color.isValid())
        return -1;
    for (int i = 0; i < m_palette.size(); ++i)
        if (m_palette[i] == color)
            return i;
    if (m_palette.size() < kMaxSlots)
    {
        m_palette.append(color);
        return m_palette.size() - 1;
    }
    /* palette full: reuse the nearest existing colour */
    int best = 0;
    float bestD = 1e30f;
    for (int i = 0; i < m_palette.size(); ++i)
    {
        const float dr = m_palette[i].redF()   - color.redF();
        const float dg = m_palette[i].greenF() - color.greenF();
        const float db = m_palette[i].blueF()  - color.blueF();
        const float d2 = dr * dr + dg * dg + db * db;
        if (d2 < bestD) { bestD = d2; best = i; }
    }
    return best;
}

void GlitterPowder::pourGlitter(float wx, float wy, float radiusWorld,
                                const GlitterParams &p)
{
    const float cell = m_worldCellSize;
    const float cx = wx / cell, cy = wy / cell;
    const float r = qMax(0.5f, radiusWorld / cell);
    /* Seed the bounded material palette on first use: prefer the params'
     * named palette, else start from the current foreground colour. */
    if (m_palette.isEmpty())
    {
        QVector<QColor> srcSlotsQ = p.palette;
        if (srcSlotsQ.isEmpty() && p.color.isValid())
            srcSlotsQ << p.color;
        if (!srcSlotsQ.isEmpty())
            m_palette = QVector<QColor>(srcSlotsQ.begin(), srcSlotsQ.end());
    }
    /* Deposit the CURRENT UI colour into a bounded palette slot (reuse /
     * append / nearest when full). Existing grains retain their original slot
     * ids, so a foreground colour change creates NEW coloured glitter instead
     * of rewriting what is already on the page. */
    const int slot = ensurePowderPaletteSlot(p.color);

    const float seedOff = m_rng.generateDouble() * 100.0f;
    const float stretch = 0.0f;
    const float sx = 1.08f * std::pow(2.0f, stretch * 0.8f);
    const float sy = 0.85f * std::pow(2.0f, -stretch * 0.8f);
    const float ext = r * 2.4f * std::max(sx, sy);
    const int x0 = std::max(1, int(std::floor(cx - ext)));
    const int x1 = std::min(m_cells.width() - 2, int(std::ceil(cx + ext)));
    const int y0 = std::max(1, int(std::floor(cy - ext)));
    const int y1 = std::min(m_cells.height() - 2, int(std::ceil(cy + ext)));
    const QRect pourRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    m_activeCells = m_activeCells.isNull() ? pourRect : m_activeCells.united(pourRect);
    const float expo = 0.35f + 1.65f * 0.1f;   /* shape ~ flat-ish heap */
    const float ragAmp = 0.4f * 0.22f;
    const float peak = std::max(0.2f, p.amount * 1.5f);

    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const float dx = (x + 0.5f - cx) / sx, dy = (y + 0.5f - cy) / sy;
            const float ang = std::atan2(dy, dx);
            const float rim = r * (1 + ragAmp * (fbm(m_rng, std::cos(ang) * 1.2f + seedOff, std::sin(ang) * 1.2f) - 0.5f) * 2
                                     + ragAmp * 0.35f * (fbm(m_rng, std::cos(ang) * 4 + seedOff, std::sin(ang) * 4) - 0.5f) * 2);
            const float d = std::hypot(dx, dy) / rim;
            if (d >= 1)
            {
                /* fine dust spills around the heap (reference dust) */
                const float dust = 0.5f;
                if (dust <= 0 || d >= 1 + 0.9f * (0.4f + dust))
                    continue;
                const float patch = fbm(m_rng, x * 0.06f + seedOff * 2, y * 0.06f);
                const float mm = dust * std::exp(-(d - 1) * 4.0f / (0.4f + dust)) * std::max(0.0f, patch * 1.8f - 0.45f);
                if (mm > 0)
                    addLoose(idx(x, y), mm, slot, -1, 0.0f);
                continue;
            }
            const float lump = (fbm(m_rng, x * 0.09f + seedOff, y * 0.09f) - 0.5f) * 0.6f * 0.5f * peak * (1 - d);
            const float mm = std::max(0.0f, peak * std::pow(1 - d, expo) * (1 - 0.3f * d * d) + lump);
            if (mm > 0)
                addLoose(idx(x, y), mm, slot, -1, 0.0f);
        }

    absorb(pourRect);
    /* The reference relaxes the whole field after each pour; scoping the 40
     * passes to the pour disc (plus relax's own 1-cell neighbour margin) keeps
     * a 2 MP heightfield interactive instead of 40 full-grid sweeps per dab. */
    for (int k = 0; k < 40; ++k)
        relax(pourRect);
}

void GlitterPowder::smear(float wx, float wy, float fx, float fy,
                          float radiusWorld, float carried)
{
    const float cell = m_worldCellSize;
    const float cx = wx / cell, cy = wy / cell;
    const float R = qMax(0.8f, radiusWorld / cell);
    const int x0 = std::max(1, int(std::floor(cx - R)));
    const int x1 = std::min(m_cells.width() - 2, int(std::ceil(cx + R)));
    const int y0 = std::max(1, int(std::floor(cy - R)));
    const int y1 = std::min(m_cells.height() - 2, int(std::ceil(cy + R)));
    /* the plow splats pushed powder up to ~R+2.3 cells past the centre, so the
     * active region is grown beyond the touched disc */
    const QRect smearRect = QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1)
                                .adjusted(-3, -3, 3, 3)
                                .intersected(QRect(1, 1, m_cells.width() - 2,
                                                   m_cells.height() - 2));
    m_activeCells = m_activeCells.isNull() ? smearRect : m_activeCells.united(smearRect);

    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const float ox = x + 0.5f - cx, oy = y + 0.5f - cy;
            const float d = std::hypot(ox, oy);
            if (d >= R)
                continue;
            const int i = idx(x, y);
            PowderCell &c = cellAt(i);
            const float ex = c.height - std::max(kFloor + 0.6f * (d / R) * (d / R), c.stuck);
            if (ex <= 0)
                continue;
            const float take = ex * 0.7f;
            c.height -= take;
            /* wrap the shuffle seed into [0,1024): the seed is a stable per-cell
             * grain-shuffle offset (packed to float16 and hashed in the GPU
             * renderer). Growing it geometrically ((seed+1)*1024) overflows to
             * +inf after a few relaxes, which NaN's the shader's hash lookups
             * and renders the material a solid black band. */
            c.seed = std::fmod(c.seed + 1.0f, 1024.0f);
            const float cling = std::min(take * 0.15f, std::max(0.0f, kClingMax - carried));
            carried += cling;   /* a little sticks to the finger */
            const float pushed = take - cling;
            float ux = (d ? ox / d : m_rng.generateDouble() - 0.5f) + fx * 1.2f;
            float uy = (d ? oy / d : m_rng.generateDouble() - 0.5f) + fy * 1.2f;
            const float ul = std::hypot(ux, uy);
            ux /= ul; uy /= ul;
            const float reach = R + 0.8f + m_rng.generateDouble() * 1.5f;
            const float tx = cx + ux * reach, ty = cy + uy * reach;
            /* splat the pushed powder where it lands (2x2 bilinear) */
            const int j = clampCell(tx - 0.5f, ty - 0.5f);
            (void)j;
            addLoose(clampCell(tx, ty), pushed, c.lA, c.lB, c.lF);
        }
    /* shed a little cling behind the finger (a dusty trail) */
    const float shed = carried * 0.04f;
    const int shedAt = clampCell(cx - fx * R * 0.7f, cy - fy * R * 0.7f);
    PowderCell &s = cellAt(shedAt);
    s.height += shed;
}

void GlitterPowder::absorb(const QRect &cellRect, float rate)
{
    Q_UNUSED(rate);
    QRect r = cellRect.isValid() ? cellRect : m_activeCells;
    if (r.isNull() || r.isEmpty())
        return;
    r = r.intersected(QRect(0, 0, m_cells.width() - 1, m_cells.height() - 1));
    if (r.isEmpty())
        return;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
        {
            PowderCell &c = m_cellsData[y * m_cells.width() + x];
            if (c.glue <= 0)
                continue;
            const float cap = kGlueDepth * c.glue;
            /* bond the powder column up to the glue's capacity; bonded keeps its own
             * colour from the loose layer it came from */
            const float loose = std::max(0.0f, c.height - c.stuck);
            if (loose <= 0)
                continue;
            const float ns = std::min(c.height, cap);
            setStuck(y * m_cells.width() + x, std::max(c.stuck, ns));
        }
}

void GlitterPowder::relax(const QRect &cellRect)
{
    const int W = m_cells.width();
    /* sweep the requested rect, else the active powder region; the 1-cell
     * margin covers the neighbour reads and is clamped to the valid interior */
    QRect r = cellRect.isValid() ? cellRect : m_activeCells;
    if (r.isNull() || r.isEmpty())
        return;
    r = r.adjusted(-1, -1, 1, 1)
            .intersected(QRect(1, 1, W - 2, m_cells.height() - 2));
    if (r.isEmpty())
        return;
    const float SQ2 = std::sqrt(2.0f);
    const int D[4] = { 1, W, W + 1, W - 1 };
    const float F[4] = { 1, 1, SQ2, SQ2 };
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
        {
            const int i = y * W + x;
            PowderCell &c = m_cellsData[i];
            const float fi = c.flow > 1 ? 1.0f : c.flow;
            for (int k = 0; k < 4; ++k)
            {
                const int j = i + D[k];
                PowderCell &nj = m_cellsData[j];
                const float fj = nj.flow > 1 ? 1.0f : nj.flow;
                const float dd = c.height - nj.height;
                if (dd > 0)
                {
                    const float T = kRepose * F[k] * (1 - 0.3f * fi);
                    if (dd <= T)
                        continue;
                    const float t = std::min((dd - T) * 0.25f, c.height - c.stuck);
                    if (t <= 0)
                        continue;
                    c.height -= t;
                    addLoose(j, t, c.lA, c.lB, c.lF);
                    nj.flow += t * 1.5f;
                    if (t > 0.03f)
                        nj.seed = std::fmod(nj.seed + 1.0f, 1024.0f);
                }
                else
                {
                    const float T = kRepose * F[k] * (1 - 0.3f * fj);
                    if (-dd <= T)
                        continue;
                    const float t = std::min((-dd - T) * 0.25f, nj.height - nj.stuck);
                    if (t <= 0)
                        continue;
                    nj.height -= t;
                    addLoose(i, t, nj.lA, nj.lB, nj.lF);
                    c.flow += t * 1.5f;
                    if (t > 0.03f)
                        c.seed = std::fmod(c.seed + 1.0f, 1024.0f);
                }
            }
        }
}

void GlitterPowder::decayFlow(const QRect &cellRect)
{
    QRect r = cellRect.isValid() ? cellRect : m_activeCells;
    if (r.isNull() || r.isEmpty())
        return;
    r = r.intersected(QRect(0, 0, m_cells.width() - 1, m_cells.height() - 1));
    if (r.isEmpty())
        return;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            m_cellsData[y * m_cells.width() + x].flow *= 0.8f;
}

void GlitterPowder::ensureBlowBuffers()
{
    const int n = m_cellsData.size();
    if (m_dh.size() != n)
        m_dh.fill(0.0f, n);
    if (m_outM.size() != n)
        m_outM.fill(0.0f, n);
    if (m_mIn.size() != n)
        m_mIn.fill(0.0f, n);
    if (m_pX.size() != n)
        m_pX.fill(0.0f, n);
    if (m_pY.size() != n)
        m_pY.fill(0.0f, n);
    if (m_inA.size() != n)
    {
        m_inA.fill(-1, n);
        m_inB.fill(-1, n);
        m_inF.fill(0.0f, n);
    }
}

void GlitterPowder::beginBlow()
{
    m_blowTime = 0.0f;
    m_particlesActive = true;
    ensureParticlePool();
    ensureBlowBuffers();
    std::fill(m_dh.begin(), m_dh.end(), 0.0f);
    std::fill(m_outM.begin(), m_outM.end(), 0.0f);
    std::fill(m_mIn.begin(), m_mIn.end(), 0.0f);
    std::fill(m_pX.begin(), m_pX.end(), 0.0f);
    std::fill(m_pY.begin(), m_pY.end(), 0.0f);
}

/* Re-hydrate the heightfield from powder already committed to a raster (world
 * coords, `worldPerPx` maps layer px to world units). The powder render stamps
 * each cell as a small disc centred on the cell's world position, so sampling
 * each cell centre tells whether powder is present there and what colour it is.
 * This lets a long-press breath act on powder a PREVIOUS stroke committed,
 * not just the current gesture's deposit. Cells whose sample looks like the
 * bare paper (or is transparent) are left empty. */
void GlitterPowder::hydrateFromRaster(const QImage &layer, float worldPerPx)
{
    if (layer.isNull())
        return;
    const float cell = m_worldCellSize;
    const float half = std::max(0.1f, cell * 0.5f / worldPerPx);
    /* the palette slots the render colour-maps to; used to assign each cell a
     * concrete colour from the raster it read back */
    QVector<QColor> pal = m_palette;
    if (pal.isEmpty())
    {
        const PowderPalette d = defaultPowderPalette();
        pal = d.colors;
    }
    /* (re)size and clear the committed-powder mask to all-zero first */
    m_powderMask.fill(0, m_cellsData.size());
    int minX = m_cells.width(), minY = m_cells.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < m_cells.height(); ++y)
        for (int x = 0; x < m_cells.width(); ++x)
        {
            const int i = y * m_cells.width() + x;
            const float wx = (x + 0.5f) * cell;
            const float wy = (y + 0.5f) * cell;
            const float px = wx / worldPerPx;
            const float py = wy / worldPerPx;
            if (px < 0 || py < 0 || px >= layer.width() || py >= layer.height())
                continue;
            const int sx = int(px), sy = int(py);
            const QColor c = layer.pixelColor(sx, sy);
            /* is the sampled pixel "powder" rather than bare paper/transparent?
             * Powder uses a fairly saturated non-white tint with ~200 alpha; the
             * paper is white/transparent. Compare against the palette colours. */
            bool isPowder = false;
            int nearest = -1;
            float bestD = 1e9f;
            for (int s = 0; s < pal.size(); ++s)
            {
                const QColor &qc = pal[s];
                const float dr = qc.redF() - c.redF(), dg = qc.greenF() - c.greenF(),
                            db = qc.blueF() - c.blueF();
                const float d2 = dr * dr + dg * dg + db * db;
                if (d2 < bestD) { bestD = d2; nearest = s; }
            }
            /* a hit is powder if it's noticeably off-white (not the paper) and
             * not transparent */
            isPowder = c.alpha() > 40
                       && (c.redF() < 0.92f || c.greenF() < 0.92f || c.blueF() < 0.92f);
            isPowder = isPowder && bestD < 0.25f;   /* and near a palette colour */
            PowderCell &cellc = cellAt(i);
            if (!isPowder)
            {
                /* bare paper: keep any previously bonded/committed structure
                 * reset (this is a hydration, so we clear untouched cells) */
                cellc = PowderCell();
                continue;
            }
            m_powderMask[i] = 1;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
            const float h = half > 0.01f ? 0.6f + 0.8f * (c.alphaF()) : 0.6f;
            cellc.height = h;
            cellc.stuck = 0.0f;              /* committed powder re-blows loose */
            cellc.glue = 0.0f;
            /* map the nearest palette slot */
            cellc.lA = nearest < 0 ? 0 : nearest;
            cellc.lB = -1;
            cellc.lF = 0.0f;
            cellc.sA = -1; cellc.sB = -1; cellc.sF = 0.0f;
            cellc.vx = cellc.vy = 0.0f;
            cellc.flow = 0.0f;
            cellc.seed = m_rng.generateDouble() * 1024.0f;
        }
    /* hydration rebuilds the whole field, so the active region is exactly the
     * rect that actually held powder (empty field -> nothing to sweep) */
    m_activeCells = (maxX >= minX && maxY >= minY)
                        ? QRect(minX, minY, maxX - minX + 1, maxY - minY + 1)
                        : QRect();
}

/* Wind field for the breath (port of the reference's updateBreath + windAt).
 * The mouth sits just off the page edge and pushes a cone of air across the
 * page; real breath isn't steady (a first puff, gusts, turbulence, sideways
 * wobble). Only loose (un-bonded) powder is moved; wet glue holds it back. */
int GlitterPowder::blowStep(float dtSec)
{
    if (m_cellsData.isEmpty()) return 0;
    ensureBlowBuffers();

    const int W = m_cells.width(), H = m_cells.height();
    m_blowTime += dtSec;
    const float k = dtSec * 60.0f;             /* frames-per-second scaling */

    /* -- wind geometry (mouth, ramp, gusts) ------------------------------ */
    const float Wf = float(W), Hf = float(H);
    const float aDir = 0.0f;                       /* blow rightward (toward +x) */
    const float ca = std::cos(aDir), sa = std::sin(aDir);
    const float ext = 0.5f * (std::abs(ca) * Wf + std::abs(sa) * Hf);
    const float D = ext + 0.35f * std::max(Wf, Hf);
    const float mx = Wf / 2.0f - ca * D;
    const float my = Hf / 2.0f - sa * D;
    const float d0 = D - ext;
    const float aim = aDir + 0.16f * (vnoise(m_rng, m_blowTime * 0.9f, 7.7f) - 0.5f);
    const float cone = 0.5f;
    /* ramp over ~2.5 s: gentle breath builds, first puff, later gusts */
    const float u = std::min(1.0f, m_blowTime / 2.5f);
    const float ramp = 0.25f + 0.75f * u * u * (3.0f - 2.0f * u);
    const float puff = 0.4f * std::exp(-std::pow((m_blowTime - 0.22f) / 0.16f, 2));
    const float lungs = (1.0f - std::exp(-m_blowTime * 7.0f))
                        * (0.75f + 0.25f * std::exp(-m_blowTime / 2.4f));
    float gusts = 0.6f + 0.6f * vnoise(m_rng, m_blowTime * 2.3f, 1.7f)
                       + 0.35f * (vnoise(m_rng, m_blowTime * 6.1f, 4.2f) - 0.5f);
    if (gusts < 0) gusts = 0.0f;
    const float gust = 1.2f * ramp * std::max(0.0f, lungs * gusts + puff * ramp);
    const bool windOn = gust > 0.01f;

    /* -- per-step buffers ------------------------------------------------ */
    /* Sweep regions (cell coords), scoped to where powder actually lives:
     *  - sourceSweep : cells whose loose powder can be advected this step
     *                  (the active region + 1 cell neighbour margin)
     *  - deltaSweep  : every cell a moving flake can WRITE this step. A step
     *                  moves at most `sp * k` cells and the gust model caps
     *                  sp near ~28 cells/s, so the margin is derived from the
     *                  step scale (k = dt*60) rather than a guess; it keeps
     *                  every target inside the zeroed region so no delta is
     *                  ever applied twice or read stale. As mass blows
     *                  outward the observed targets grow m_activeCells, so
     *                  the sweeps follow the powder at near-zero extra cost
     *                  until it actually spreads across the page. */
    const int travelMargin = std::max(8, int(std::ceil(29.0f * k)) + 8);
    QRect sourceSweep = m_activeCells.isNull() ? QRect()
                        : m_activeCells.adjusted(-1, -1, 1, 1)
                              .intersected(QRect(1, 1, W - 2, H - 2));
    QRect deltaSweep = m_activeCells.isNull() ? QRect()
                       : m_activeCells.adjusted(-travelMargin, -travelMargin,
                                                travelMargin, travelMargin)
                             .intersected(QRect(0, 0, W - 1, H - 1));
    if (sourceSweep.isEmpty())
    {
        /* nothing to blow, but in-flight flakes must still arc and land */
        if (m_particlesActive)
            stepParticles(dtSec);
        return 0;
    }
    /* zero only the delta region (it is a superset of every write below) */
    const int dL = deltaSweep.left(), dR = deltaSweep.right();
    for (int y = deltaSweep.top(); y <= deltaSweep.bottom(); ++y)
    {
        const int base = y * W;
        std::fill(m_dh.begin() + base + dL, m_dh.begin() + base + dR + 1, 0.0f);
        std::fill(m_outM.begin() + base + dL, m_outM.begin() + base + dR + 1, 0.0f);
        std::fill(m_mIn.begin() + base + dL, m_mIn.begin() + base + dR + 1, 0.0f);
        std::fill(m_pX.begin() + base + dL, m_pX.begin() + base + dR + 1, 0.0f);
        std::fill(m_pY.begin() + base + dL, m_pY.begin() + base + dR + 1, 0.0f);
        /* incoming colour lanes (reference iA/iB/iF) — persistent, sized once
         * by ensureBlowBuffers, and reset at the start of every breath step */
        std::fill(m_inA.begin() + base + dL, m_inA.begin() + base + dR + 1, -1);
        std::fill(m_inB.begin() + base + dL, m_inB.begin() + base + dR + 1, -1);
        std::fill(m_inF.begin() + base + dL, m_inF.begin() + base + dR + 1, 0.0f);
    }
    /* observed landing cells this step -> grow the active region for the next
     * step (so the sweeps follow the blown powder) */
    int tMinX = W, tMinY = H, tMaxX = -1, tMaxY = -1;
    auto noteLanding = [&](int xi, int yi)
    {
        if (xi < tMinX) tMinX = xi;
        if (xi > tMaxX) tMaxX = xi;
        if (yi < tMinY) tMinY = yi;
        if (yi > tMaxY) tMaxY = yi;
    };

const float spd = 1.0f;
    const float speedUp  = 1.0f - std::exp(-dtSec * 7.0f);
    const float slowDown = 1.0f - std::exp(-dtSec * 3.2f);
    int moving = 0;

    for (int y = sourceSweep.top(); y <= sourceSweep.bottom(); ++y)
        for (int x = sourceSweep.left(); x <= sourceSweep.right(); ++x)
        {
            const int i = y * W + x;
            PowderCell &c = m_cellsData[i];
            const float loose = c.height - c.stuck;
            if (loose <= 0.002f) { c.vx = c.vy = 0.0f; continue; }

            float f = 0.0f, tvx = 0.0f, tvy = 0.0f;
            if (windOn)
            {
                const float rx = (x + 0.5f) - mx, ry = (y + 0.5f) - my;
                const float d = std::hypot(rx, ry);
                const float ang = d > 1e-6f ? std::atan2(ry, rx) : 0.0f;
                float off = ang - aim;
                off = std::atan2(std::sin(off), std::cos(off));
                const float conef = std::exp(-(off / cone) * (off / cone));
                const float fall = 0.7f + 0.3f / (1.0f + std::max(0.0f, d - d0) / (0.8f * Wf));
                const float drift = m_blowTime * 55.0f * spd;
                const float ex = (x + 0.5f) - ca * drift;
                const float ey = (y + 0.5f) - sa * drift;
                const float turb = 0.4f + 1.2f * vnoise(m_rng, ex * 0.035f, ey * 0.035f + 5.0f);
                const float side = (vnoise(m_rng, ex * 0.05f + 30.0f, ey * 0.05f) - 0.5f) * 0.9f;
                f = gust * conef * fall * turb * (1.0f - 0.6f * c.glue);
                const float ts = (1.0f + 8.0f * f) * spd * std::min(1.0f, f * 2.0f);
                tvx = std::cos(ang + side) * ts;
                tvy = std::sin(ang + side) * ts;
            }
            const float cur = c.vx * c.vx + c.vy * c.vy;
            const float tgt = tvx * tvx + tvy * tvy;
            const float r = tgt > cur ? speedUp : slowDown;
            c.vx += (tvx - c.vx) * r;
            c.vy += (tvy - c.vy) * r;
            const float sp = std::hypot(c.vx, c.vy);
            if (sp < 0.05f) continue;
            ++moving;

            const float mob = (0.12f * f + 0.18f * f * f) * k + 0.06f * std::min(sp, 4.0f) * k;
            const float m = std::min(loose, loose * std::min(1.0f, mob)
                                     + (0.1f + 0.01f * loose) * f * k + 0.02f * sp * k);
            if (m < 1e-4f) continue;
            const float ux = c.vx / sp, uy = c.vy / sp, step = sp * k;
            const float lat = (m_rng.generateDouble() - 0.5f) * (0.6f + 0.35f * step);
            const float tx = (x + 0.5f) + ux * step - uy * lat + (m_rng.generateDouble() - 0.5f) * 0.6f;
            const float ty = (y + 0.5f) + uy * step + ux * lat + (m_rng.generateDouble() - 0.5f) * 0.6f;
            m_dh[i] -= m; m_outM[i] += m;
            const float sx = tx - 0.5f, sy = ty - 0.5f;
            const int xi = int(std::floor(sx)), yi = int(std::floor(sy));
            if (xi >= 1 && yi >= 1 && xi < W - 2 && yi < H - 2)
            {
                noteLanding(xi, yi);
                const float fx = sx - xi, fy = sy - yi;
                const int j = yi * W + xi;
                const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
                const float w01 = (1 - fx) * fy, w11 = fx * fy;
                const int targets[4] = { j, j + 1, j + W, j + W + 1 };
                const float wgt[4] = { w00, w10, w01, w11 };
                for (int q = 0; q < 4; ++q)
                {
                    const int t = targets[q];
                    const float w = wgt[q] * m;
                    m_dh[t] += w; m_mIn[t] += w;
                    m_pX[t] += w * c.vx; m_pY[t] += w * c.vy;
                    /* carry the source colour along (reference mixInto into iA/iB/iF) */
                    combineSlots(m_inA[t], m_inB[t], m_inF[t], c.lA, c.lB, c.lF, w);
                }
            }
            c.seed = std::fmod(c.seed + 1.0f, 1024.0f);
            /* a few flat flakes catch the air and lift off the bed; the lifted
             * mass becomes an airborne particle that arcs and falls, carrying
             * its colour, instead of just vanishing from the heightfield */
            if (f > 0 && m > 0.01f && m_rng.generateDouble() < f * f * 0.12f * k)
            {
                const float lift = std::min(m * 0.5f, 0.05f);
                m_dh[i] -= lift; m_outM[i] += lift;
                if (m_particlesActive)
                {
                    const float psp = (40.0f + 110.0f * f * m_rng.generateDouble()) * spd;
                    const float spM = std::hypot(c.vx, c.vy) || 1.0f;
                    spawnFlake(x + 0.5f, y + 0.5f,
                               c.vx / spM, c.vy / spM, psp,
                               4.0f + 14.0f * m_rng.generateDouble(),
                               lift, c.lA >= 0 ? c.lA : 0);
                }
            }
        }

    /* second pass: apply height/momentum/colour deltas and bond loose powder
     * that blows across glue (reference GLUE_DEPTH catching). */
    for (int y = deltaSweep.top(); y <= deltaSweep.bottom(); ++y)
        for (int x = deltaSweep.left(); x <= deltaSweep.right(); ++x)
        {
            const int i = y * W + x;
            if (m_dh[i] == 0.0f && m_mIn[i] == 0.0f) continue;
            PowderCell &c = m_cellsData[i];
            if (m_mIn[i] > 0.0f)
            {
                const float stay = std::max(0.0f, c.height - c.stuck - m_outM[i]);
                const float tot = stay + m_mIn[i];
                if (tot > 1e-6f)
                {
                    c.vx = (c.vx * stay + m_pX[i]) / tot;
                    c.vy = (c.vy * stay + m_pY[i]) / tot;
                }
                const float looseNow = std::max(0.0f, c.height - c.stuck);
                if (m_inA[i] >= 0)
                    mixInto(i, looseNow, m_inA[i], m_inB[i], m_inF[i], m_mIn[i], false);
                m_inA[i] = -1; m_inB[i] = -1; m_inF[i] = 0.0f;
            }
            c.height = std::max(0.0f, c.height + m_dh[i]);
            if (m_dh[i] > 0.0f && c.glue > 0.0f)
            {
                const float cap = kGlueDepth * c.glue;
                float room = cap - c.stuck;
                if (room > 0.0f && m_mIn[i] > 0.0f)
                    setStuck(i, c.stuck + std::min(m_dh[i] * 0.4f, room));
            }
            if (c.stuck > c.height) c.stuck = c.height;
            m_dh[i] = 0.0f; m_mIn[i] = 0.0f; m_pX[i] = 0.0f; m_pY[i] = 0.0f; m_outM[i] = 0.0f;
        }

    /* grow the active region to wherever powder actually landed this step, so
     * the next step's sweeps follow the blown powder (kept inside the grid) */
    if (tMaxX >= tMinX && tMaxY >= tMinY)
    {
        const QRect landed = QRect(tMinX, tMinY, tMaxX - tMinX + 1, tMaxY - tMinY + 1);
        m_activeCells = m_activeCells.isNull() ? landed : m_activeCells.united(landed);
    }

    if (m_particlesActive)
        stepParticles(dtSec);

    return moving;
}

void GlitterPowder::ensureParticlePool()
{
    if (m_maxFlakes <= 0)
    {
        m_maxFlakes = 1600;                 /* bounded fleet like the reference */
        m_flakes.fill(AirborneFlake(), m_maxFlakes);
    }
}

void GlitterPowder::spawnFlake(float wx, float wy, float ux, float uy,
                               float speed, float size, float mass,
                               int slot)
{
    ensureParticlePool();
    /* find a free slot; if the fleet is full, recycle the oldest */
    int pick = -1;
    for (int i = 0; i < m_maxFlakes; ++i)
        if (!m_flakes[i].active) { pick = i; break; }
    if (pick < 0)
        pick = int(m_rng.bounded(m_maxFlakes));
    AirborneFlake &f = m_flakes[pick];
    f.x = wx * m_worldCellSize;   /* cell-space -> world units */
    f.y = wy * m_worldCellSize;
    f.z = 0.0f;
    /* world-units velocity: cell speed -> world */
    f.vx = ux * speed * m_worldCellSize * 0.05f;
    f.vy = uy * speed * m_worldCellSize * 0.05f;
    f.vz = (2.0f + 4.0f * m_rng.generateDouble()) * m_worldCellSize;   /* upward kick */
    f.rot = m_rng.generateDouble() * 6.2831f;
    f.spin = (m_rng.generateDouble() - 0.5f) * 12.0f;
    f.size = size;
    f.life = 0.6f + 0.6f * m_rng.generateDouble();   /* ~1.2 s arc */
    f.slot = slot;
    f.active = true;
    Q_UNUSED(mass);
}

void GlitterPowder::stepParticles(float dtSec)
{
    for (AirborneFlake &f : m_flakes)
    {
        if (!f.active)
            continue;
        f.life -= dtSec;
        if (f.life <= 0.0f) { f.active = false; continue; }
        /* shallow gravitation pulls the flake back down; a little drag */
        f.vz -= 9.0f * m_worldCellSize * dtSec;
        const float drag = 1.0f - std::min(1.0f, 1.2f * dtSec);
        f.vx *= drag; f.vy *= drag;
        f.x += f.vx * dtSec;
        f.y += f.vy * dtSec;
        f.z = std::max(0.0f, f.z + f.vz * dtSec);
        f.rot += f.spin * dtSec;
        /* landed back on the bed / fell out of the world: retire */
        if (f.z <= 0.0f && f.life < 0.8f) { f.active = false; }
    }
}

int GlitterPowder::looseCount() const
{
    int n = 0;
    for (const PowderCell &c : m_cellsData)
        if (c.height - c.stuck > 0.002f)
            ++n;
    return n;
}

int GlitterPowder::bondedCount() const
{
    int n = 0;
    for (const PowderCell &c : m_cellsData)
        if (c.stuck > 0.002f)
            ++n;
    return n;
}

float GlitterPowder::looseMass() const
{
    float m = 0;
    for (const PowderCell &c : m_cellsData)
    {
        const float v = c.height - c.stuck;
        if (v > 0)
            m += v;
    }
    return m;
}

float GlitterPowder::looseMassInRect(float wx0, float wy0, float wx1, float wy1) const
{
    const float cell = m_worldCellSize;
    const int x0 = std::max(0, int(std::floor(wx0 / cell)));
    const int y0 = std::max(0, int(std::floor(wy0 / cell)));
    const int x1 = std::min(m_cells.width() - 1, int(std::ceil(wx1 / cell)));
    const int y1 = std::min(m_cells.height() - 1, int(std::ceil(wy1 / cell)));
    float m = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const PowderCell &c = cellAt(idx(x, y));
            const float v = c.height - c.stuck;
            if (v > 0.002f)
                m += v;
        }
    return m;
}

bool GlitterPowder::isEmpty() const
{
    return looseMass() <= 0.01f && bondedCount() == 0;
}

bool GlitterPowder::powderBoundsWorld(float &minx, float &miny,
                                      float &maxx, float &maxy) const
{
    const float cell = m_worldCellSize;
    minx = 1e30f; miny = 1e30f; maxx = -1e30f; maxy = -1e30f;
    bool any = false;
    for (int y = 0; y < m_cells.height(); ++y)
        for (int x = 0; x < m_cells.width(); ++x)
        {
            const PowderCell &c = cellAt(idx(x, y));
            if (c.height <= 0.02f)
                continue;
            any = true;
            const float wx0 = x * cell, wy0 = y * cell;
            const float wx1 = (x + 1) * cell, wy1 = (y + 1) * cell;
            if (wx0 < minx) minx = wx0; if (wy0 < miny) miny = wy0;
            if (wx1 > maxx) maxx = wx1; if (wy1 > maxy) maxy = wy1;
        }
    if (!any) return false;
    return true;
}

bool GlitterPowder::hydatedPowderAt(int cx, int cy) const
{
    if (cx < 0 || cy < 0 || cx >= m_cells.width() || cy >= m_cells.height())
        return false;
    const int i = cy * m_cells.width() + cx;
    return i >= 0 && i < m_powderMask.size() && m_powderMask[i] != 0;
}

void GlitterPowder::clearMaskedPowder(QImage &img, float originWorldX,
                                      float originWorldY, float worldPerPx,
                                      const QColor &paper) const
{
    Q_UNUSED(paper);
    if (img.isNull() || m_powderMask.isEmpty())
        return;
    /* The powder material layer is TRANSPARENT outside its physical material:
     * removed material restores transparent RGBA, never a baked paper colour
     * (paper belongs to DocumentRenderer beneath the layer). `paper` is kept as
     * a compatibility parameter and ignored for the transparent contraction. */
    const float cell = m_worldCellSize;
    for (int y = 0; y < m_cells.height(); ++y)
        for (int x = 0; x < m_cells.width(); ++x)
        {
            const int i = y * m_cells.width() + x;
            if (i >= m_powderMask.size() || m_powderMask[i] == 0)
                continue;
            const float wx = (x + 0.5f) * cell;
            const float wy = (y + 0.5f) * cell;
            const float px = (wx - originWorldX) / worldPerPx;
            const float py = (wy - originWorldY) / worldPerPx;
            const float half = std::max(0.8f, cell * 0.5f / worldPerPx + 0.5f);
            if (px < -half || py < -half || px > img.width() + half
                || py > img.height() + half)
                continue;
            /* clear the cell's disc to TRANSPARENT (Source, fully cleared) */
            int x0 = std::max(0, int(std::floor(px - half)));
            int y0 = std::max(0, int(std::floor(py - half)));
            int x1 = std::min(img.width() - 1, int(std::ceil(px + half)));
            int y1 = std::min(img.height() - 1, int(std::ceil(py + half)));
            const float r2 = half * half;
            for (int yy = y0; yy <= y1; ++yy)
                for (int xx = x0; xx <= x1; ++xx)
                {
                    const float dx = xx + 0.5f - px, dy = yy + 0.5f - py;
                    if (dx * dx + dy * dy > r2)
                        continue;
                    QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(yy));
                    line[xx] = qRgba(0, 0, 0, 0);
                }
        }
}

/* ---- reference noise (cray4pasta/glitter-powder, exact equations) --------
 * GLSL:
 *   vec3 hash32(vec2 p){ vec3 p3 = fract(vec3(p.xyx)*vec3(.1031,.1030,.0973));
 *     p3 += dot(p3, p3.yxz+33.33);
 *     return fract((p3.xxy + p3.yzz)*p3.zyx); }
 * All grain variation in render() flows through this 2D hash of the grain's
 * absolute Voronoi id, so a large dense heap reads as an irregular packed-grain
 * surface with no rows/columns/checkerboard. `fract` is the GLSL toward-floor
 * version (x - floor(x)); the graders take both negative and >1 inputs. */
static float gFract(float x) { return x - std::floor(x); }

static QVector3D refHash32(float px, float py)
{
    float p3x = gFract(px * 0.1031f);
    float p3y = gFract(py * 0.1030f);
    float p3z = gFract(px * 0.0973f);                 /* p.xyx -> (x, y, x) */
    /* dot(p3, p3.yxz + 33.33) = x*(y+33.33)+y*(x+33.33)+z*(z+33.33) */
    const float dy = p3y + 33.33f, dx = p3x + 33.33f, dz = p3z + 33.33f;
    const float d = p3x * dy + p3y * dx + p3z * dz;
    p3x += d; p3y += d; p3z += d;
    /* fract((p3.xxy + p3.yzz) * p3.zyx) = ((x+y)*z, (x+z)*y, (y+z)*x) */
    return QVector3D(gFract((p3x + p3y) * p3z),
                     gFract((p3x + p3z) * p3y),
                     gFract((p3y + p3z) * p3x));
}

/* GLSL smoothstep(edge0, edge1, x) and mix(a, b, t). */
static float gSmoothstep(float e0, float e1, float x)
{
    const float t = qBound(0.0f, (x - e0) / (e1 - e0), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static float gMix(float a, float b, float t) { return a + (b - a) * t; }

QImage GlitterPowder::render(const QSize &outSize, float outOriginWorldX,
                             float outOriginWorldY, float worldPerPx,
                             const ArtLightRig &rig, const PowderPalette &pal,
                             const PaperStock *paper, const ArtScale &scale,
                             qint64 timeMs) const
{
    Q_UNUSED(scale);
    QImage img(outSize, QImage::Format_RGBA8888_Premultiplied);
    img.fill(Qt::transparent);

    const QColor paperCol = pal.paper;
    /* Build the slot->PaletteSlot list used by the reference's uPal render. */
    QVector<PaletteSlot> slotsList;
    for (const QColor &c : pal.colors)
    {
        PaletteSlot s;
        s.color = c;
        slotsList.append(s);
    }

    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const float cell = m_worldCellSize;
    /* Only iterate the cell sub-grid that intersects the output viewport,
     * never the whole heightfield: at brush scale with a 1-2 MP grid a full
     * per-cell scan per segment would dominate the 120 Hz stroke budget. */
    const int cx0 = int((outOriginWorldX) / cell);
    const int cy0 = int((outOriginWorldY) / cell);
    const int cx1 = int((outOriginWorldX + outSize.width() * worldPerPx) / cell);
    const int cy1 = int((outOriginWorldY + outSize.height() * worldPerPx) / cell);
    const int gx0 = std::max(0, cx0), gy0 = std::max(0, cy0);
    const int gx1 = std::min(m_cells.width() - 1, cx1 + 1);
    const int gy1 = std::min(m_cells.height() - 1, cy1 + 1);

    /* Shared ArtLightRig -> the same light context the GPU path and the other
     * art materials use: L points from the surface toward the key light, H is
     * the half-vector with the straight-down view. No hardcoded light. */
    const QVector3D rigDir = rig.unitDirection();          /* points down */
    QVector3D L = QVector3D(-rigDir.x(), -rigDir.y(), -rigDir.z());
    if (L.z() < 0.05f)
        L.setZ(0.05f);
    L.normalize();
    const QVector3D Ln = L.normalized();
    const QVector3D H = (Ln + QVector3D(0.0f, 0.0f, 1.0f)).normalized();

    /* ---- CONTINUOUS GLOBAL Voronoi grain field (reference GPU, CPU) -------
     * The reference derives ~one packed glass bead per output pixel from a
     * 3x3-lookup Voronoi field in normalized viewport space (hash32-jittered
     * cell centres), then shades each bead from the heightfield under its
     * grain CENTRE. We reproduce that same continuous field here by sampling it
     * at grain resolution (~one sample per Voronoi cell) instead of every pixel,
     * which is the natural CPU analogue: grain positions come from the jittered
     * centres in screen space, so a large dense heap is an irregular packed
     * surface with NO rows/columns/checkerboard. The sim cells control only
     * height/loose/bonded/colour/glue + the shuffle seed — never the layout. */
    const float unit = qMin(float(outSize.width()), float(outSize.height()));
    const float grainPx = qBound(2.5f, unit / 140.0f, 7.0f); /* ~1 css-px-per-grain, coarsened for CPU */
    const float gscale = unit / grainPx;                    /* scale: g = p*scale */
    const float stepX = qMax(0.6f, grainPx * 0.9f);
    const float stepY = stepX;

    /* Sample region = output viewport clipped to the pow-ish bounds (the cell
     * scan above) and to where powder actually lives, so we never issue grain
     * samples over empty paper and the interactive stroke stays cheap. */
    float sx0 = outOriginWorldX, sx1 = outOriginWorldX + outSize.width() * worldPerPx;
    float sy0 = outOriginWorldY, sy1 = outOriginWorldY + outSize.height() * worldPerPx;
    float pMinX = 0, pMinY = 0, pMaxX = 0, pMaxY = 0;
    if (powderBoundsWorld(pMinX, pMinY, pMaxX, pMaxY))
    {
        sx0 = qMax(sx0, pMinX); sx1 = qMin(sx1, pMaxX);
        sy0 = qMax(sy0, pMinY); sy1 = qMin(sy1, pMaxY);
    }
    /* also keep the pow-ish cell-scan extent (viewport ∩ sim grid) as a bound */
    sx0 = qMax(sx0, float(gx0) * cell);      sx1 = qMin(sx1, float(gx1 + 1) * cell);
    sy0 = qMax(sy0, float(gy0) * cell);      sy1 = qMin(sy1, float(gy1 + 1) * cell);

    p.setPen(Qt::NoPen);
    const int gridW = m_cells.width(), gridH = m_cells.height();
    const float padPx = grainPx * 1.2f;
    if (sx1 > sx0 && sy1 > sy0 && worldPerPx > 0.0f)
    {
        /* px range of the sample grid (output px) */
        const float pxBeg = (sx0 - outOriginWorldX) / worldPerPx - padPx;
        const float pxEnd = (sx1 - outOriginWorldX) / worldPerPx + padPx;
        const float pyBeg = (sy0 - outOriginWorldY) / worldPerPx - padPx;
        const float pyEnd = (sy1 - outOriginWorldY) / worldPerPx + padPx;

        /* heightfield read at an arbitrary world position (clamped, reference
         * clamps the sim uv to the grid). */
        auto hAt = [&](float wx, float wy) -> float
        {
            int x = int(std::floor(wx / cell));
            int y = int(std::floor(wy / cell));
            x = std::clamp(x, 0, gridW - 1);
            y = std::clamp(y, 0, gridH - 1);
            return cellAt(idx(x, y)).height;
        };

        for (float sy = pyBeg; sy <= pyEnd; sy += stepY)
            for (float sx = pxBeg; sx <= pxEnd; sx += stepX)
            {
                /* pixel -> normalized viewport space (reference p) */
                const float pnx = sx / unit - 0.5f;
                const float pny = sy / unit - 0.5f;
                const float gx = pnx * gscale, gy = pny * gscale;
                const float gix = std::floor(gx), giy = std::floor(gy);
                const float gfx = gx - gix, gfy = gy - giy;

                /* 3x3 Voronoi field, exact reference equations */
                float b1 = 8.0f, b2 = 8.0f;
                float bRx = 0.0f, bRy = 0.0f, bIx = 0.0f, bIy = 0.0f, bPx = 0.0f, bPy = 0.0f;
                for (int cy = -1; cy <= 1; ++cy)
                    for (int cx = -1; cx <= 1; ++cx)
                    {
                        const float ccx = float(cx), ccy = float(cy);
                        const QVector3D h = refHash32(gix + ccx, giy + ccy);
                        const float ptx = ccx + 0.15f + 0.7f * h.x();
                        const float pty = ccy + 0.15f + 0.7f * h.y();
                        const float rx = ptx - gfx, ry = pty - gfy;
                        const float dd = rx * rx + ry * ry;
                        if (dd < b1)
                        {
                            b2 = b1; b1 = dd;
                            bRx = rx; bRy = ry;
                            bIx = gix + ccx; bIy = giy + ccy;
                            bPx = gix + ptx; bPy = giy + pty;
                        }
                        else if (dd < b2)
                        {
                            b2 = dd;
                        }
                    }
                const float dist = std::sqrt(b1);
                const float edge = std::sqrt(b2) - dist;

                /* grain centre -> output px -> world (single mapping) */
                const float gcxPx = (bPx / gscale) * unit + 0.5f * outSize.width();
                const float gcyPx = (bPy / gscale) * unit + 0.5f * outSize.height();
                const float gw = outOriginWorldX + gcxPx * worldPerPx;
                const float ghw = outOriginWorldY + gcyPx * worldPerPx;
                int scx = int(std::floor(gw / cell));
                int scy = int(std::floor(ghw / cell));
                scx = std::clamp(scx, 0, gridW - 1);
                scy = std::clamp(scy, 0, gridH - 1);
                const PowderCell &cg = cellAt(idx(scx, scy));
                const float hVal = cg.height;
                if (hVal <= 0.02f)
                    continue;                       /* no powder under this grain */

                /* reference coverage gates from the heightfield under the grain */
                const float stuckLow = cg.stuck;
                const float glueAmtCell = cg.glue;
                const float bonded = gSmoothstep(0.02f, 0.25f, stuckLow);
                const float looseH = qMax(hVal - stuckLow, 0.0f);

                /* shuffle seed is the 2D grain-identity offset (reference s) */
                const float s = cg.seed;
                const float sidx = bIx + s * 13.17f;
                const float sidy = bIy + s * 13.17f;
                const QVector3D gh  = refHash32(sidx + 71.3f,         sidy + 71.3f);
                const QVector3D gh2 = refHash32(sidx * 1.37f + 5.1f,  sidy * 1.37f + 5.1f);
                const QVector3D gh3 = refHash32(sidx * 0.73f + 19.0f, sidy * 0.73f + 19.0f);
                const QVector3D gj  = refHash32(sidx * 2.31f + 9.7f,  sidy * 2.31f + 9.7f);

                /* two-slot colour: loose vs bonded by the grain hash, then the
                 * A/B share -> a discrete slot (speckles, never a blend). */
                const bool loose = gh.y() < qBound(0.0f, looseH / 0.9f, 1.0f);
                int slotA = loose ? cg.lA : cg.sA;
                int slotB = loose ? cg.lB : cg.sB;
                const float share = loose ? cg.lF : cg.sF;
                const int slot = (gj.z() < share) ? slotB : slotA;
                QColor tintCol = paperCol;
                if (slot >= 0 && slot < slotsList.size())
                    tintCol = slotsList[slot].color;

                /* all grain variation is 2D-hash of the absolute Voronoi id */
                const float rad = 0.5f + 0.12f * gh.x();
                const float rr = dist / rad;
                const float inBead = 1.0f - gSmoothstep(0.82f, 1.0f, rr);
                const float glueAmt = gSmoothstep(0.15f, 0.85f,
                                                  glueAmtCell + (gh.x() - 0.5f) * 0.55f) * bonded;
                const float exposedGlue = glueAmt * (1.0f - gSmoothstep(0.25f, 1.1f, looseH));
                const float present = (gh.y() < qBound(0.0f, looseH / 0.9f, 1.0f) ||
                                       gh.y() < glueAmt) ? 1.0f : 0.0f;
                const float deep = gSmoothstep(0.4f, 1.4f,
                                               looseH + std::pow(glueAmt, 1.5f) * 1.6f);
                if (present <= 0.001f)
                    continue;

                /* multi-scale pile normal + per-grain bead normal (reference) */
                float gradx = 0.0f, grady = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float o = cell * (1.5f + 3.0f * float(k));
                    const float ww = 2.0f * o;
                    gradx += (hAt(gw + o, ghw) - hAt(gw - o, ghw)) / ww;
                    grady += (hAt(gw, ghw + o) - hAt(gw, ghw - o)) / ww;
                }
                const QVector3D N = QVector3D(-gradx * (cell / worldPerPx),
                                              -grady * (cell / worldPerPx),
                                              1.0f).normalized();
                const float nbx = -bRx / rad, nby = -bRy / rad;
                const float rrGate = rr <= 1.0f ? 1.0f : 0.0f;
                const QVector3D Nb = (N
                    + QVector3D(nbx * 0.9f, nby * 0.9f, 0.0f) * rrGate).normalized();
                const float diff = qBound(0.0f, QVector3D::dotProduct(Nb, Ln), 1.0f);
                const float macroDiff = qBound(0.0f, QVector3D::dotProduct(N, Ln), 1.0f);

                /* shading: tone applied to achievable albedo, specular core */
                float tone = (0.80f + 0.26f * gh.z()) * (1.0f + 0.05f * exposedGlue);
                tone *= gMix(1.0f, 0.72f, gh3.y() > 0.95f ? 1.0f : 0.0f);
                const float tr = tintCol.redF(), tg = tintCol.greenF(), tb = tintCol.blueF();
                const float tmax = qMax(qMax(tr, tg), qMax(tb, 0.04f));
                const float sr = tr / tmax, sg = tg / tmax, sb = tb / tmax;
                const float ar = tr * tone, ag = tg * tone, ab = tb * tone;
                const float bd = 0.80f + 0.30f * diff;
                float cr = ar * bd, cg2 = ag * bd, cb = ab * bd;
                const float edgeMix = gSmoothstep(0.0f, 0.22f, edge);
                cr *= gMix(0.84f, 1.0f, edgeMix);
                cg2 *= gMix(0.84f, 1.0f, edgeMix);
                cb *= gMix(0.84f, 1.0f, edgeMix);
                const float specN = qMax(0.0f, QVector3D::dotProduct(Nb, H));
                const float specPow = std::pow(specN, 45.0f);
                cr += gMix(1.0f, sr, 0.5f) * specPow * 0.35f;
                cg2 += gMix(1.0f, sg, 0.5f) * specPow * 0.35f;
                cb += gMix(1.0f, sb, 0.5f) * specPow * 0.35f;

                /* paper + soft contact shadow behind the bead */
                const float ax = -Ln.x(), ay = -Ln.y();
                const float alen = std::sqrt(ax * ax + ay * ay) + 1e-6f;
                float sh = 0.0f;
                for (int k = 1; k <= 3; ++k)
                    sh += gSmoothstep(0.3f, 7.0f, hAt(gw - (ax / alen) * float(k) * 7.0f * cell,
                                                      ghw - (ay / alen) * float(k) * 7.0f * cell));
                const float shadow = 0.10f * (sh / 3.0f) + 0.06f * gSmoothstep(0.1f, 1.0f, hVal);
                const float bgr = qMax(0.0f, tr - shadow);
                const float bgg = qMax(0.0f, tg - shadow);
                const float bgb = qMax(0.0f, tb - shadow * 0.97f);
                /* gap powder between the beads: bright tint but gently macro-lit
                 * (keeps a deep heap cohesive and the slot colour legible) */
                const float gpr = tr * (0.90f + 0.10f * macroDiff);
                const float gpg = tg * (0.90f + 0.10f * macroDiff);
                const float gpb = tb * (0.90f + 0.10f * macroDiff);
                const float baseR = gMix(bgr, gpr, deep);
                const float baseG = gMix(bgg, gpg, deep);
                const float baseB = gMix(bgb, gpb, deep);
                const float aOut = qMin(1.0f,
                    0.45f + 0.55f * inBead * present + 0.40f * deep
                    + 0.18f * diff * macroDiff);
                const int aOut8 = int(qBound(0.0f, aOut, 1.0f) * 255.0f);

                /* coverage disc fills the surrounding Voronoi cell so a deep
                 * heap never shows transparent gaps between beads (the bead
                 * disc is then shaded on top). */
                const float covR = grainPx * 0.75f;
                const float radPx = rad * grainPx;
                p.setBrush(QColor(int(qBound(0.0f, baseR, 1.0f) * 255.0f),
                                  int(qBound(0.0f, baseG, 1.0f) * 255.0f),
                                  int(qBound(0.0f, baseB, 1.0f) * 255.0f),
                                  aOut8));
                p.drawEllipse(QPointF(gcxPx, gcyPx), covR, covR);
                p.setBrush(QColor(int(qBound(0.0f, cr, 1.0f) * 255.0f),
                                  int(qBound(0.0f, cg2, 1.0f) * 255.0f),
                                  int(qBound(0.0f, cb, 1.0f) * 255.0f),
                                  aOut8));
                p.drawEllipse(QPointF(gcxPx, gcyPx), radPx, radPx);

                /* --- time-driven shimmer: a per-grain glint in the bright
                 * specular core, keyed to the 2D grain id so a change in
                 * timeMs always moves a few pixels (reference uTime). ---- */
                const float t = (timeMs > 0) ? float((timeMs % 4000) / 1000.0) : 0.0f;
                const float rate = 0.6f + 1.4f * gh.y();
                const float phase = gh2.y() * 6.2832f + t * rate;
                const float amp = 0.5f + 0.5f * std::sin(phase);
                if (specPow > 0.02f && amp > 0.30f)
                {
                    const float sw = qMax(0.35f, radPx * 0.34f);
                    const float alpha = qMin(255.0f, 60.0f + 170.0f * amp);
                    QColor glintc(255, int(qBound(0.0f, 1.0f - 0.25f * amp, 1.0f) * 255.0f),
                                  255, int(alpha));
                    p.setBrush(glintc);
                    p.drawEllipse(QPointF(gcxPx + (gh.x() - 0.5f) * radPx * 0.8f,
                                          gcyPx + (gh.z() - 0.5f) * radPx * 0.8f), sw, sw);
                }
            }
    }

    /* Airborne flakes caught in the breath: bright additive sparkles that arc
     * and fall. Only present while a blow is running; retired flakes vanish. */
    if (m_particlesActive && !m_flakes.isEmpty())
    {
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.setPen(Qt::NoPen);
        for (const AirborneFlake &f : m_flakes)
        {
            if (!f.active)
                continue;
            const float px = (f.x - outOriginWorldX) / worldPerPx;
            const float py = (f.y - outOriginWorldY) / worldPerPx
                             - (f.z / worldPerPx);          /* lift above the bed */
            const float pr = qMax(0.5f, f.size * 0.5f / worldPerPx);
            if (px < -pr || py < -pr || px > outSize.width() + pr
                || py > outSize.height() + pr)
                continue;
            QColor pcol = paperCol;
            if (f.slot >= 0 && f.slot < slotsList.size())
                pcol = slotsList[f.slot].color;
            /* brighter as it's freshly lifted, dimming as it falls out */
            const float bright = qMax(0.0f, std::min(1.0f, f.life * 1.6f));
            pcol.setAlpha(int(240 * bright));
            p.setBrush(pcol);
            p.save();
            p.translate(px, py);
            p.rotate(f.rot * 57.2958f);
            p.drawEllipse(QPointF(0, 0), pr, pr * 0.6f);
            p.restore();
        }
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }

    p.end();
    return img;
}

} // namespace glitter
} // namespace art
} // namespace paintbrush
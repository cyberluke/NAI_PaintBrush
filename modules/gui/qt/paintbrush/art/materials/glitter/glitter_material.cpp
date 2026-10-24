/*****************************************************************************
 * glitter_material.cpp : NAI Paintbrush — flagship glitter art material
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Quality reference (principles only, no code embedded):
 *   cray4pasta/glitter-powder (MIT) — glue, loose vs bonded grains, granular
 *   accumulation, displacement, multi-color grains, height, shadowing, facet
 *   orientation, iridescence, lighting, paper interaction.
 *****************************************************************************/

#include "glitter_material.hpp"

#include "../../runtime/art_noise.hpp"

#include <QVector3D>
#include <QDataStream>
#include <QByteArray>
#include <QIODevice>
#include <cmath>
#include <algorithm>

namespace paintbrush {
namespace art {
namespace glitter {

/* ---------------------------------------------------------------- palette */

QVector<PaletteSlot> defaultPalette(std::uint32_t seed, int count)
{
    /* A bounded, harmonious grain palette: the base hue plus lighter/darker
     * and a complementary accent so multi-color glitter stays grain-like
     * (spec §14.4) instead of averaging into one muddy RGB. */
    QVector<PaletteSlot> out;
    count = qBound(1, count, 12);
    const float baseHue = artHashUnitf(seed, 11u) * 360.0f;
    for (int i = 0; i < count; ++i)
    {
        PaletteSlot s;
        float hue = baseHue;
        float sat = 0.75f;
        float val = 0.95f;
        switch (i % 6)
        {
        case 0: break;                                   /* base */
        case 1: val = 0.80f; break;                      /* darker */
        case 2: hue += 18.0f; val = 1.0f; sat = 0.6f; break; /* lighter */
        case 3: hue += 180.0f; sat = 0.55f; break;       /* complement */
        case 4: hue -= 24.0f; break;                     /* analog */
        default: val = 0.65f; sat = 0.9f; break;        /* deep */
        }
        hue = std::fmod(hue + 360.0f, 360.0f);
        s.color = QColor::fromHsvF(hue / 360.0f, sat, val);
        s.weight = (i == 0) ? 2.0f : 1.0f;
        s.name = QStringLiteral("slot%1").arg(i);
        out.append(s);
    }
    return out;
}

static QVector<PaletteSlot> paletteFor(const GlitterParams &p)
{
    QVector<PaletteSlot> out;
    if (!p.palette.isEmpty())
    {
        for (const QColor &c : p.palette)
        {
            PaletteSlot s;
            s.color = c;
            out.append(s);
        }
        return out;
    }
    /* Single-color glitter still gets identity variation: three tonal slots
     * around the chosen color so grains read as individual flakes. */
    int h, sa, v, a;
    p.color.getHsv(&h, &sa, &v, &a);
    if (h < 0)
        h = 0;
    const int tones[3] = { 0, -28, +18 };
    for (int i = 0; i < 3; ++i)
    {
        PaletteSlot s;
        s.color = QColor::fromHsv(h, qBound(0, sa + (i == 2 ? -30 : 0), 255),
                                  qBound(0, v + tones[i], 255));
        s.weight = (i == 0) ? 2.0f : 1.0f;
        out.append(s);
    }
    return out;
}

float glitterWorldCellSize(const GlitterParams &p, float flakeScale)
{
    /* `flakeScale` is the flake DIAMETER in world units at flakeSize == 1
     * (a fine craft glitter is ~1.5 px at 1x). A cell is ~2.2 flakes wide so
     * up to kMaxGrainsPerCell flakes can pile without stacking into a blob. */
    const float flake = std::max(0.05f, p.flakeSize * flakeScale);
    return flake * 2.2f;
}

/* ------------------------------------------------------------------ cells */

GlitterState::Cell &GlitterState::cell(int cx, int cy)
{
    return m_grid[cy * m_cells.width() + cx];
}

const GlitterState::Cell &GlitterState::cell(int cx, int cy) const
{
    return m_grid[cy * m_cells.width() + cx];
}

void GlitterState::reset(const QSize &cells, float worldCellSize)
{
    m_cells = QSize(std::max(1, cells.width()), std::max(1, cells.height()));
    m_worldCellSize = std::max(1e-3f, worldCellSize);
    m_grid.clear();
    m_grid.resize(m_cells.width() * m_cells.height());
    m_palette.clear();
}

static inline float discFalloff(float dx, float dy, float radius)
{
    const float d2 = dx * dx + dy * dy;
    const float r2 = radius * radius;
    if (d2 >= r2)
        return 0.0f;
    const float t = 1.0f - d2 / r2;   /* 1 at centre, 0 at rim */
    return t * t * (3.0f - 2.0f * t); /* smoothstep */
}

void GlitterState::paintGlue(float wx, float wy, float radiusWorld, float coverage)
{
    if (m_grid.isEmpty() || radiusWorld <= 0.0f)
        return;
    const float cs = m_worldCellSize;
    const int cx0 = std::max(0, int(std::floor((wx - radiusWorld) / cs)));
    const int cy0 = std::max(0, int(std::floor((wy - radiusWorld) / cs)));
    const int cx1 = std::min(m_cells.width() - 1, int(std::floor((wx + radiusWorld) / cs)));
    const int cy1 = std::min(m_cells.height() - 1, int(std::floor((wy + radiusWorld) / cs)));
    for (int cy = cy0; cy <= cy1; ++cy)
    {
        for (int cx = cx0; cx <= cx1; ++cx)
        {
            const float ccx = (cx + 0.5f) * cs;
            const float ccy = (cy + 0.5f) * cs;
            const float f = discFalloff(ccx - wx, ccy - wy, radiusWorld);
            if (f <= 0.0f)
                continue;
            Cell &c = cell(cx, cy);
            if (coverage < 0.0f)
                c.glue = std::max(0.0f, c.glue + coverage * f); /* dry/remove */
            else
                c.glue = std::min(1.0f, c.glue + coverage * f);
        }
    }
}

void GlitterState::applyGlitter(float wx, float wy, float radiusWorld,
                                const GlitterParams &p, const QPointF &transport,
                                float coverage)
{
    if (m_grid.isEmpty() || radiusWorld <= 0.0f)
        return;
    if (m_palette.isEmpty())
        m_palette = paletteFor(p);

    const float cs = m_worldCellSize;
    const int cx0 = std::max(0, int(std::floor((wx - radiusWorld) / cs)));
    const int cy0 = std::max(0, int(std::floor((wy - radiusWorld) / cs)));
    const int cx1 = std::min(m_cells.width() - 1, int(std::floor((wx + radiusWorld) / cs)));
    const int cy1 = std::min(m_cells.height() - 1, int(std::floor((wy + radiusWorld) / cs)));

    const std::uint32_t seed = p.seed;
    const float transportLen = std::sqrt(float(transport.x() * transport.x() +
                                               transport.y() * transport.y()));

    for (int cy = cy0; cy <= cy1; ++cy)
    {
        for (int cx = cx0; cx <= cx1; ++cx)
        {
            const float ccx = (cx + 0.5f) * cs;
            const float ccy = (cy + 0.5f) * cs;
            const float f = discFalloff(ccx - wx, ccy - wy, radiusWorld);
            if (f <= 0.0f)
                continue;
            Cell &c = cell(cx, cy);

            /* 1) transport: loose grains shear along the push vector; grains
             *    pushed out of the cell migrate to the neighbour (bounded) or
             *    are blown away. Bonded grains never move. */
            if (transportLen > 1e-4f)
            {
                const float k = f * std::min(1.0f, transportLen / cs) * 0.9f;
                for (int i = 0; i < c.count; ++i)
                {
                    Grain &g = c.grains[i];
                    if (g.bonded)
                        continue;
                    g.x += float(transport.x()) / cs * k;
                    g.y += float(transport.y()) / cs * k;
                    const int ncx = cx + int(std::floor(g.x));
                    const int ncy = cy + int(std::floor(g.y));
                    if (ncx != cx || ncy != cy)
                    {
                        Grain moved = g;
                        moved.x -= std::floor(moved.x);
                        moved.y -= std::floor(moved.y);
                        /* remove from this cell (swap-pop) */
                        c.grains[i] = c.grains[c.count - 1];
                        --c.count;
                        --i;
                        if (ncx >= 0 && ncy >= 0 && ncx < m_cells.width() &&
                            ncy < m_cells.height())
                        {
                            Cell &n = cell(ncx, ncy);
                            if (n.count < kMaxGrainsPerCell)
                            {
                                moved.height = std::uint16_t(std::min(1023,
                                    400 + int(n.count) * 60));
                                n.grains[n.count++] = moved;
                            }
                        }
                    }
                }
            }

            /* 2) deposition: fill toward the target count for this cell. The
             *    centre of the pour packs densely (a real pile), the rim
             *    scatters (dispersion controls how ragged the edge is). */
            const float disp = p.dispersion;
            const float jitter = 1.0f - disp * 0.6f +
                disp * 1.2f * artCellUnitf(seed ^ 0xD15Fu, cx, cy);
            const float pile = std::pow(f, 0.6f); /* flatter top, soft rim */
            int target = int(std::lround(p.amount * coverage * pile * jitter *
                                         float(kMaxGrainsPerCell) * 1.1f));
            target = qBound(0, target, int(kMaxGrainsPerCell));

            const float glue = std::max(c.glue, p.adhesion * coverage);
            const float bondProb = qBound(0.0f, glue * (1.0f - p.looseMix), 1.0f);

            while (c.count < target)
            {
                const std::uint32_t gi = artHash2(seed, artHash2(
                    std::uint32_t(cx * 92821 + cy), std::uint32_t(c.count)));
                Grain g;
                g.x = artHashUnitf(gi, 1u);
                g.y = artHashUnitf(gi, 2u);
                g.size = 0.6f + 0.8f * artHashUnitf(gi, 3u);
                g.rot = artHashUnitf(gi, 4u) * 6.2831853f;
                /* weighted palette slot */
                float total = 0.0f;
                for (const PaletteSlot &s : m_palette)
                    total += s.weight;
                float pick = artHashUnitf(gi, 5u) * std::max(1e-4f, total);
                int slot = 0;
                for (int s = 0; s < m_palette.size(); ++s)
                {
                    pick -= m_palette[s].weight;
                    if (pick <= 0.0f)
                    {
                        slot = s;
                        break;
                    }
                    slot = s;
                }
                g.slot = std::uint8_t(slot);
                g.bonded = artHashUnitf(gi, 6u) < bondProb;
                /* height: bonded grains stack low on the glue; loose grains sit
                 * on top of the bonded pile. */
                g.height = std::uint16_t(g.bonded ? 60 + 40 * int(c.count)
                                                  : 400 + 60 * int(c.count));
                c.grains[c.count++] = g;
            }
        }
    }
}

/* ----------------------------------------------------------------- render */

namespace {

struct LightCtx
{
    QVector3D L;        /* unit vector from surface toward light */
    QVector3D H;        /* half vector between L and viewer (0,0,1) */
    float specExp;      /* specular exponent (softness -> wider) */
    float keyIntensity;
    float ambient;
    QColor key;
    QColor amb;
};

inline LightCtx makeLight(const ArtLightRig &rig)
{
    LightCtx c;
    QVector3D d = rig.unitDirection();  /* points down onto the paper */
    c.L = QVector3D(-d.x(), -d.y(), -d.z()).normalized(); /* toward light */
    if (c.L.z() < 0.05f)
        c.L.setZ(0.05f);
    c.L.normalize();
    const QVector3D V(0.0f, 0.0f, 1.0f);
    c.H = (c.L + V).normalized();
    /* flakes are tiny mirrors: a tight lobe so only well-aligned facets flash */
    c.specExp = 60.0f - 40.0f * qBound(0.0f, rig.softness, 1.0f);
    c.keyIntensity = rig.intensity;
    c.ambient = rig.ambient;
    c.key = rig.keyColor();
    c.amb = rig.ambientColor();
    return c;
}

/* Facet normal for a grain: bonded flakes lie nearly flat (small tilt),
 * loose flakes sit at steeper random angles on the pile. */
inline QVector3D facetNormal(const Grain &g, std::uint32_t gi)
{
    /* flakes lie nearly flat with a spread of small tilts, so a fraction of
     * them always face the key light and twinkle; loose flakes on the pile
     * tilt more */
    const float tiltMax = g.bonded ? 0.55f : 0.9f;
    const float tilt = tiltMax * std::sqrt(artHashUnitf(gi, 21u));
    const float st = std::sin(tilt), ct = std::cos(tilt);
    return QVector3D(st * std::cos(g.rot), st * std::sin(g.rot), ct).normalized();
}

inline void blendOver(QRgb *dst, float r, float g, float b, float a)
{
    /* straight-alpha "over" into an ARGB32 pixel */
    a = qBound(0.0f, a, 1.0f);
    if (a <= 0.0f)
        return;
    const float da = qAlpha(*dst) / 255.0f;
    const float outA = a + da * (1.0f - a);
    if (outA <= 1e-5f)
    {
        *dst = qRgba(0, 0, 0, 0);
        return;
    }
    const float dr = qRed(*dst) / 255.0f, dg = qGreen(*dst) / 255.0f,
                db = qBlue(*dst) / 255.0f;
    const float orr = (r * a + dr * da * (1.0f - a)) / outA;
    const float og = (g * a + dg * da * (1.0f - a)) / outA;
    const float ob = (b * a + db * da * (1.0f - a)) / outA;
    *dst = qRgba(qBound(0, int(orr * 255.0f + 0.5f), 255),
                 qBound(0, int(og * 255.0f + 0.5f), 255),
                 qBound(0, int(ob * 255.0f + 0.5f), 255),
                 qBound(0, int(outA * 255.0f + 0.5f), 255));
}

/* Rasterize one flake footprint (rotated rounded square) into `img`. */
void drawFlake(QImage &img, float cxPx, float cyPx, float rPx, float rot,
               float r, float g, float b, float alpha, bool shadowPass)
{
    const int x0 = std::max(0, int(std::floor(cxPx - rPx - 1.0f)));
    const int y0 = std::max(0, int(std::floor(cyPx - rPx - 1.0f)));
    const int x1 = std::min(img.width() - 1, int(std::ceil(cxPx + rPx + 1.0f)));
    const int y1 = std::min(img.height() - 1, int(std::ceil(cyPx + rPx + 1.0f)));
    if (x1 < x0 || y1 < y0)
        return;
    const float cr = std::cos(-rot), sr = std::sin(-rot);
    const float edge = std::max(0.5f, rPx * 0.35f); /* AA band */
    for (int y = y0; y <= y1; ++y)
    {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = x0; x <= x1; ++x)
        {
            const float px = (x + 0.5f) - cxPx;
            const float py = (y + 0.5f) - cyPx;
            /* rotate into facet frame */
            const float fx = px * cr - py * sr;
            const float fy = px * sr + py * cr;
            /* rounded-square distance (Chebyshev blended with Euclid) */
            const float cheb = std::max(std::fabs(fx), std::fabs(fy));
            const float eucl = std::sqrt(fx * fx + fy * fy);
            const float d = cheb * 0.7f + eucl * 0.3f;
            float cov = (rPx - d) / edge;
            cov = qBound(0.0f, cov, 1.0f);
            if (cov <= 0.0f)
                continue;
            blendOver(&row[x], r, g, b, alpha * cov * (shadowPass ? 0.5f : 1.0f));
        }
    }
}

} // namespace

QImage GlitterState::render(const QSize &outSize, float outOriginWorldX,
                            float outOriginWorldY, float worldPerPx,
                            const ArtLightRig &rig, const PaperStock *paper,
                            const ArtScale &scale) const
{
    QImage img(outSize, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    if (m_grid.isEmpty() || outSize.isEmpty() || worldPerPx <= 0.0f)
        return img;
    (void)scale;

    const LightCtx light = makeLight(rig);
    const float cs = m_worldCellSize;
    const float pxPerWorld = 1.0f / worldPerPx;
    /* flake radius in world units: a cell is ~2.2 flakes wide */
    const float flakeRWorld = cs / 2.2f * 0.5f;

    /* visible cell range */
    const float visW = outSize.width() * worldPerPx;
    const float visH = outSize.height() * worldPerPx;
    const int cx0 = std::max(0, int(std::floor(outOriginWorldX / cs)) - 1);
    const int cy0 = std::max(0, int(std::floor(outOriginWorldY / cs)) - 1);
    const int cx1 = std::min(m_cells.width() - 1,
                             int(std::floor((outOriginWorldX + visW) / cs)) + 1);
    const int cy1 = std::min(m_cells.height() - 1,
                             int(std::floor((outOriginWorldY + visH) / cs)) + 1);

    const float toothShade = paper ? paper->tooth : 0.4f;
    const float absorption = paper ? paper->absorption : 0.4f;
    const QVector3D shadowDir(-light.L.x(), -light.L.y(), 0.0f);
    /* a state restored without palette slots still renders with grain
     * identity (never index into an empty palette) */
    const QVector<PaletteSlot> palette =
        m_palette.isEmpty() ? defaultPalette(0x9A1Du, 3) : m_palette;

    /* pass 0: contact shadows (all grains), pass 1: bonded, pass 2: loose */
    for (int pass = 0; pass < 3; ++pass)
    {
        for (int cy = cy0; cy <= cy1; ++cy)
        {
            for (int cx = cx0; cx <= cx1; ++cx)
            {
                const Cell &c = cell(cx, cy);
                for (int i = 0; i < c.count; ++i)
                {
                    const Grain &g = c.grains[i];
                    if (pass == 1 && !g.bonded) continue;
                    if (pass == 2 && g.bonded) continue;

                    const std::uint32_t gi = artHash2(
                        std::uint32_t(cx * 92821 + cy), std::uint32_t(i) ^ 0x9E37u);
                    const float wxg = (cx + g.x) * cs;
                    const float wyg = (cy + g.y) * cs;
                    const float rWorld = flakeRWorld * g.size;
                    const float rPx = std::max(0.6f, rWorld * pxPerWorld);
                    float pxX = (wxg - outOriginWorldX) * pxPerWorld;
                    float pxY = (wyg - outOriginWorldY) * pxPerWorld;

                    if (pass == 0)
                    {
                        /* contact shadow: a tight dark footprint offset away
                         * from the light; loose grains sit higher and cast a
                         * touch longer. Kept subtle so the pile reads as
                         * grains on paper, not as dots with drop shadows. */
                        const float h = g.height / 1023.0f;
                        const float len = rPx * (0.25f + 0.5f * h);
                        drawFlake(img, pxX + shadowDir.x() * len,
                                  pxY + shadowDir.y() * len, rPx * 0.9f, g.rot,
                                  0.0f, 0.0f, 0.0f,
                                  0.28f * (0.5f + 0.5f * h) * (1.0f - rig.softness * 0.5f),
                                  true);
                        continue;
                    }

                    /* lighting */
                    const QVector3D n = facetNormal(g, gi);
                    const float diff = std::max(0.0f, QVector3D::dotProduct(n, light.L));
                    const float ndh = std::max(0.0f, QVector3D::dotProduct(n, light.H));
                    const float spec = std::pow(ndh, light.specExp);

                    const PaletteSlot &slot =
                        palette.at(std::min<int>(g.slot, palette.size() - 1));
                    QColor base = slot.color;
                    float hr = base.redF(), hg = base.greenF(), hb = base.blueF();
                    /* iridescence: shift hue by facet angle; strength from the
                     * hash so neighbouring flakes differ */
                    const float holo = artHashUnitf(gi, 33u);
                    if (holo > 0.7f)
                    {
                        int h, s, v, a;
                        base.getHsv(&h, &s, &v, &a);
                        if (h >= 0)
                        {
                            const int shift = int((ndh - 0.5f) * 120.0f * (holo - 0.7f) * 3.3f);
                            QColor hc = QColor::fromHsv((h + shift + 360) % 360, s, v);
                            hr = hc.redF(); hg = hc.greenF(); hb = hc.blueF();
                        }
                    }

                    /* paper interaction: tooth micro-shadowing + glue sinking */
                    const float tooth = 1.0f - 0.18f * toothShade * artHashUnitf(gi, 44u);
                    const float sink = g.bonded ? (1.0f - 0.12f * absorption) : 1.0f;

                    const float amb = light.ambient;
                    /* metallic flakes: dark-ish body in shadow, bright facet
                     * when it catches the key — the contrast IS the sparkle */
                    const float kd = (amb * 0.75f + diff * light.keyIntensity * 1.1f) * tooth * sink;
                    /* a few flakes are tilted exactly into the light: those
                     * flash near white (the glitter "twinkle"); most only
                     * carry a tinted sheen. */
                    const float glint = spec * light.keyIntensity * (g.bonded ? 1.6f : 2.0f);
                    const float flash = glint > 0.55f ? (glint - 0.55f) * 2.2f : 0.0f;

                    const float kr = light.key.redF(), kg = light.key.greenF(), kb = light.key.blueF();
                    float r = qBound(0.0f, hr * kd * kr + glint * 0.35f * kr + flash, 1.0f);
                    float gg = qBound(0.0f, hg * kd * kg + glint * 0.35f * kg + flash, 1.0f);
                    float b = qBound(0.0f, hb * kd * kb + glint * 0.35f * kb + flash, 1.0f);

                    /* keep grain identity: never blend neighbours' colors */
                    drawFlake(img, pxX, pxY, rPx, g.rot, r, gg, b,
                              g.bonded ? 1.0f : 0.95f, false);
                }
            }
        }
    }
    return img;
}

/* ----------------------------------------------------------------- stats */

GlitterState::CellView GlitterState::cellView(int cx, int cy) const
{
    CellView v;
    if (m_grid.isEmpty() || cx < 0 || cy < 0 || cx >= m_cells.width() ||
        cy >= m_cells.height())
        return v;
    const Cell &c = cell(cx, cy);
    v.glue = c.glue;
    v.count = c.count;
    v.grains = c.grains;
    return v;
}

QVector<PaletteSlot> GlitterState::paletteForRender() const
{
    if (!m_palette.isEmpty())
        return m_palette;
    return defaultPalette(0x9A1Du, 3);
}

int GlitterState::grainCount() const
{
    int n = 0;
    for (const Cell &c : m_grid)
        n += c.count;
    return n;
}

int GlitterState::bondedCount() const
{
    int n = 0;
    for (const Cell &c : m_grid)
        for (int i = 0; i < c.count; ++i)
            if (c.grains[i].bonded)
                ++n;
    return n;
}

int GlitterState::looseCount() const
{
    return grainCount() - bondedCount();
}

/* ------------------------------------------------------------- persistence */

QByteArray GlitterState::serialize() const
{
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_6_0);
    ds << quint32(0x474C5431u) /* 'GLT1' */ << qint32(m_cells.width())
       << qint32(m_cells.height()) << float(m_worldCellSize)
       << qint32(m_palette.size());
    for (const PaletteSlot &s : m_palette)
        ds << s.color << s.weight << s.name;
    for (const Cell &c : m_grid)
    {
        ds << quint8(c.count) << c.glue;
        for (int i = 0; i < c.count; ++i)
        {
            const Grain &g = c.grains[i];
            ds << g.x << g.y << g.size << g.rot << quint16(g.height)
               << quint8(g.slot) << g.bonded;
        }
    }
    return out;
}

bool GlitterState::deserialize(const QByteArray &bytes)
{
    QDataStream ds(bytes);
    ds.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    qint32 w = 0, h = 0, np = 0;
    float cs = 1.0f;
    ds >> magic >> w >> h >> cs >> np;
    if (magic != 0x474C5431u || w <= 0 || h <= 0 || w * h > 16 * 1024 * 1024 ||
        np < 0 || np > 64)
        return false;
    reset(QSize(w, h), cs);
    for (int i = 0; i < np; ++i)
    {
        PaletteSlot s;
        ds >> s.color >> s.weight >> s.name;
        m_palette.append(s);
    }
    for (Cell &c : m_grid)
    {
        quint8 count = 0;
        ds >> count >> c.glue;
        c.count = std::min<quint8>(count, kMaxGrainsPerCell);
        for (int i = 0; i < count; ++i)
        {
            Grain g;
            quint16 height = 0;
            quint8 slot = 0;
            ds >> g.x >> g.y >> g.size >> g.rot >> height >> slot >> g.bonded;
            g.height = height;
            g.slot = slot;
            if (i < kMaxGrainsPerCell)
                c.grains[i] = g;
        }
        if (ds.status() != QDataStream::Ok)
            return false;
    }
    return ds.status() == QDataStream::Ok;
}

} // namespace glitter
} // namespace art
} // namespace paintbrush

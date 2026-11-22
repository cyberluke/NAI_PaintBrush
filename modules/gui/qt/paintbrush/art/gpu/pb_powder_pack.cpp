/*****************************************************************************
 * pb_powder_pack.cpp : CPU sim -> GPU powder texture data
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_powder_pack.hpp"

#include <cmath>
#include <cstring>
#include <vector>

namespace paintbrush {
namespace art {

namespace {

quint16 floatToHalf(float f)
{
    quint32 x = 0;
    std::memcpy(&x, &f, 4);
    const quint32 sign = (x >> 16) & 0x8000u;
    const int exp = int((x >> 23) & 0xFFu) - 127 + 15;
    quint32 mant = x & 0x7FFFFFu;
    if (exp >= 0x1F)
        return quint16(sign | 0x7C00u);
    if (exp <= 0)
    {
        if (exp < -10)
            return quint16(sign);
        mant = (mant | 0x800000u) >> (1 - exp);
        return quint16(sign | (mant + 0x1000u) >> 13);
    }
    quint32 m = mant >> 13;
    const quint32 rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (m & 1u)))
        ++m;
    if ((m & 0x7C00u) == 0x7C00u)
        return quint16(sign | 0x7C00u);
    return quint16(sign | (quint32(exp) << 10) | m);
}

} // namespace

PowderSceneData packPowderScene(
    const glitter::GlitterPowder &powder,
    const glitter::PowderPalette &pal,
    const ArtLightRig &rig, const PaperStock *paper,
    double worldPerPx, double timeMs, float flakeWorld, int dpr,
    const QRect &cellWindow, bool packGlue)
{
    PowderSceneData data;
    const int W = powder.cellW();
    const int H = powder.cellH();
    if (W <= 0 || H <= 0)
        return data;
    data.gridW = W;
    data.gridH = H;

    /* window clamp: pack only the cells the dirty union can touch */
    QRect win = cellWindow.isValid() ? cellWindow : QRect(0, 0, W, H);
    win = win.intersected(QRect(0, 0, W, H));
    if (win.isEmpty())
        win = QRect(0, 0, W, H);
    const int wx0 = win.left(), wy0 = win.top();
    const int ww = win.width(), wh = win.height();
    data.cellX = wx0;
    data.cellY = wy0;
    data.cellW = ww;
    data.cellH = wh;

    /* ---- heightfield + colours, window only --------------------------- */
    data.simHalf = QByteArray(ww * wh * 8, '\0');
    quint8 *sim8 = reinterpret_cast<quint8 *>(data.simHalf.data());
    data.col = QByteArray(ww * wh * 4, '\0');
    quint8 *col8 = reinterpret_cast<quint8 *>(data.col.data());

    const int palN = qMax(1, pal.colors.size());
    const int maxSlot = qMin(palN - 1, 15);
    for (int y = 0; y < wh; ++y)
    {
        const int gy = wy0 + y;
        for (int x = 0; x < ww; ++x)
        {
            const int gx = wx0 + x;
            const glitter::PowderCell &c = powder.powderCellAt(gy * W + gx);
            const float h  = std::isfinite(c.height) ? qMax(0.0f, c.height) : 0.0f;
            const float sd = std::isfinite(c.seed)   ? std::fmod(c.seed, 1024.0f) : 0.0f;
            const float st = std::isfinite(c.stuck)  ? qMax(0.0f, c.stuck)  : 0.0f;
            const float gl = std::isfinite(c.glue)   ? qMax(0.0f, c.glue)   : 0.0f;
            const quint16 vh = floatToHalf(h);
            const quint16 vs = floatToHalf(sd);
            const quint16 vb = floatToHalf(st);
            const quint16 va = floatToHalf(gl);
            quint8 *p = sim8 + size_t(y * ww + x) * 8;
            p[0] = quint8(vh & 0xFF); p[1] = quint8(vh >> 8);
            p[2] = quint8(vs & 0xFF); p[3] = quint8(vs >> 8);
            p[4] = quint8(vb & 0xFF); p[5] = quint8(vb >> 8);
            p[6] = quint8(va & 0xFF); p[7] = quint8(va >> 8);

            const size_t o = size_t(y * ww + x) * 4;
            const int loA = qBound(0, c.lA, maxSlot);
            const int loB = qBound(0, c.lB, maxSlot);
            const int bdA = qBound(0, c.sA, maxSlot);
            const int bdB = qBound(0, c.sB, maxSlot);
            col8[o + 0] = quint8((loA << 4) | (loB & 0x0F));
            col8[o + 1] = quint8(qBound(0.0f, c.lF, 1.0f) * 255.0f);
            col8[o + 2] = quint8((bdA << 4) | (bdB & 0x0F));
            col8[o + 3] = quint8(qBound(0.0f, c.sF, 1.0f) * 255.0f);
        }
    }

    /* ---- glue field: full dims in the frame, window bytes only when asked */
    const bool glueAny = powder.hasAnyGlue();
    data.glueAny = glueAny;
    data.glueRev = powder.glueRevision();
    if (glueAny)
    {
        const int sub = glitter::GlitterPowder::glueSubRes();
        data.glueW = W * sub;
        data.glueH = H * sub;
        if (packGlue)
        {
            const QVector<quint8> &field = powder.glueFieldBytes();
            const int fullW = data.glueW;
            const int sww = ww * sub, shh = wh * sub;
            if (!field.isEmpty() && qint64(field.size()) >= qint64(fullW) * data.glueH)
            {
                data.glue = QByteArray(sww * shh, '\0');
                quint8 *dst = reinterpret_cast<quint8 *>(data.glue.data());
                const quint8 *src = field.constData();
                const int sx0 = wx0 * sub, sy0 = wy0 * sub;
                for (int y = 0; y < shh; ++y)
                    std::memcpy(dst + size_t(y) * sww,
                                src + size_t(sy0 + y) * fullW + sx0,
                                size_t(sww));
            }
        }
    }

    data.worldPerPx = float(worldPerPx);
    data.worldCell = powder.worldCellSize();
    data.holo = pal.holographic;
    data.flakePx = float(5 * qMax(1, dpr));
    data.flakeWorld = flakeWorld;
    data.tooth = paper ? paper->tooth : 0.4f;
    data.mirror = 0.90f;
    data.exposure = 1.0f;
    data.timeMs = timeMs;
    data.paper = paper ? paper->baseColor : QColor(255, 255, 255);

    const QVector3D d = rig.unitDirection();
    QVector3D L(-d.x(), -d.y(), -d.z());
    if (L.z() < 0.05f)
        L.setZ(0.05f);
    L.normalize();
    data.lightDir = L;
    data.palette = pal.colors;
    return data;
}

} // namespace art
} // namespace paintbrush

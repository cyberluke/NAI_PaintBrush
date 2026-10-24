/*****************************************************************************
 * effects.cpp : NAI Paintbrush — Finish runtime (non-destructive layer effects)
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "effects.hpp"

#include <QPainter>
#include <QVector3D>
#include <cmath>
#include <algorithm>
#include <vector>

namespace paintbrush {
namespace art {

/* ------------------------------------------------------------------ roles */

QString layerRoleName(LayerRole role)
{
    switch (role)
    {
    case LayerRole::Hero: return QStringLiteral("hero");
    case LayerRole::Support: return QStringLiteral("support");
    case LayerRole::Ghost: return QStringLiteral("ghost");
    case LayerRole::Transition: return QStringLiteral("transition");
    case LayerRole::None:
    default: return QStringLiteral("none");
    }
}

LayerRole layerRoleFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("hero")) return LayerRole::Hero;
    if (n == QLatin1String("support")) return LayerRole::Support;
    if (n == QLatin1String("ghost")) return LayerRole::Ghost;
    if (n == QLatin1String("transition")) return LayerRole::Transition;
    return LayerRole::None;
}

/* ------------------------------------------------------------- EffectNode */

static QString effectTypeName(int t)
{
    switch (t)
    {
    case EffectNode::Shadow: return QStringLiteral("shadow");
    case EffectNode::Glow: return QStringLiteral("glow");
    case EffectNode::MotionBlur: return QStringLiteral("motion_blur");
    case EffectNode::Outline: return QStringLiteral("outline");
    case EffectNode::Ghost: return QStringLiteral("ghost");
    default: return QStringLiteral("none");
    }
}

static int effectTypeFromName(const QString &s)
{
    if (s == QLatin1String("shadow")) return EffectNode::Shadow;
    if (s == QLatin1String("glow")) return EffectNode::Glow;
    if (s == QLatin1String("motion_blur")) return EffectNode::MotionBlur;
    if (s == QLatin1String("outline")) return EffectNode::Outline;
    if (s == QLatin1String("ghost")) return EffectNode::Ghost;
    return EffectNode::None;
}

QVariantMap EffectNode::toVariant() const
{
    QVariantMap m;
    m.insert(QStringLiteral("type"), effectTypeName(type));
    m.insert(QStringLiteral("enabled"), enabled);
    m.insert(QStringLiteral("color"), color.name(QColor::HexArgb));
    m.insert(QStringLiteral("opacity"), double(opacity));
    m.insert(QStringLiteral("blur"), double(blur));
    m.insert(QStringLiteral("spread"), double(spread));
    m.insert(QStringLiteral("offsetX"), double(offset.x()));
    m.insert(QStringLiteral("offsetY"), double(offset.y()));
    m.insert(QStringLiteral("useLightRig"), useLightRig);
    m.insert(QStringLiteral("lightDistance"), double(lightDistance));
    m.insert(QStringLiteral("radius"), double(radius));
    m.insert(QStringLiteral("intensity"), double(intensity));
    m.insert(QStringLiteral("threshold"), double(threshold));
    m.insert(QStringLiteral("dirX"), double(direction.x()));
    m.insert(QStringLiteral("dirY"), double(direction.y()));
    m.insert(QStringLiteral("distance"), double(distance));
    m.insert(QStringLiteral("falloff"), double(falloff));
    m.insert(QStringLiteral("samples"), samples);
    m.insert(QStringLiteral("width"), double(width));
    m.insert(QStringLiteral("softness"), double(softness));
    m.insert(QStringLiteral("desaturate"), double(desaturate));
    m.insert(QStringLiteral("tint"), tint.name(QColor::HexArgb));
    return m;
}

EffectNode EffectNode::fromVariant(const QVariantMap &m)
{
    EffectNode e;
    e.type = effectTypeFromName(m.value(QStringLiteral("type")).toString());
    e.enabled = m.value(QStringLiteral("enabled"), true).toBool();
    e.color = QColor(m.value(QStringLiteral("color"), QStringLiteral("#ff000000")).toString());
    e.opacity = m.value(QStringLiteral("opacity"), 0.55).toFloat();
    e.blur = m.value(QStringLiteral("blur"), 8.0).toFloat();
    e.spread = m.value(QStringLiteral("spread"), 0.0).toFloat();
    e.offset = QPointF(m.value(QStringLiteral("offsetX"), 6.0).toDouble(),
                       m.value(QStringLiteral("offsetY"), 8.0).toDouble());
    e.useLightRig = m.value(QStringLiteral("useLightRig"), true).toBool();
    e.lightDistance = m.value(QStringLiteral("lightDistance"), 10.0).toFloat();
    e.radius = m.value(QStringLiteral("radius"), 20.0).toFloat();
    e.intensity = m.value(QStringLiteral("intensity"), 1.0).toFloat();
    e.threshold = m.value(QStringLiteral("threshold"), 0.2).toFloat();
    e.direction = QPointF(m.value(QStringLiteral("dirX"), 1.0).toDouble(),
                          m.value(QStringLiteral("dirY"), 0.0).toDouble());
    e.distance = m.value(QStringLiteral("distance"), 24.0).toFloat();
    e.falloff = m.value(QStringLiteral("falloff"), 0.6).toFloat();
    e.samples = m.value(QStringLiteral("samples"), 24).toInt();
    e.width = m.value(QStringLiteral("width"), 3.0).toFloat();
    e.softness = m.value(QStringLiteral("softness"), 0.2).toFloat();
    e.desaturate = m.value(QStringLiteral("desaturate"), 0.4).toFloat();
    e.tint = QColor(m.value(QStringLiteral("tint"), QStringLiteral("#00ffffff")).toString());
    return e;
}

EffectNode EffectNode::makeShadow(float blurWorld, float opacity, const QColor &color)
{
    EffectNode e;
    e.type = Shadow;
    e.blur = blurWorld;
    e.opacity = opacity;
    e.color = color;
    return e;
}

EffectNode EffectNode::makeGlow(const QColor &color, float radiusWorld, float intensity)
{
    EffectNode e;
    e.type = Glow;
    e.color = color;
    e.radius = radiusWorld;
    e.intensity = intensity;
    e.opacity = 1.0f;
    return e;
}

EffectNode EffectNode::makeMotionBlur(const QPointF &direction, float distanceWorld,
                                      float falloff, int samples)
{
    EffectNode e;
    e.type = MotionBlur;
    e.direction = direction;
    e.distance = distanceWorld;
    e.falloff = falloff;
    e.samples = samples;
    return e;
}

EffectNode EffectNode::makeOutline(const QColor &color, float widthWorld, float softness)
{
    EffectNode e;
    e.type = Outline;
    e.color = color;
    e.width = widthWorld;
    e.softness = softness;
    e.opacity = 1.0f;
    return e;
}

EffectNode EffectNode::makeGhost(float opacity, float desaturate, const QColor &tint)
{
    EffectNode e;
    e.type = Ghost;
    e.opacity = opacity;
    e.desaturate = desaturate;
    e.tint = tint;
    return e;
}

/* ------------------------------------------------------------- primitives */

QPointF FinishRuntime::shadowDirection(const ArtLightRig &rig)
{
    /* rig.direction points from the surface TOWARD the light (its xy is where
     * the light sits); a shadow falls on the opposite side. unitDirection()
     * keeps that azimuth, so negate its xy projection. */
    const QVector3D d = rig.unitDirection();
    QPointF s(-d.x(), -d.y());
    const double len = std::sqrt(s.x() * s.x() + s.y() * s.y());
    if (len < 1e-6)
        return QPointF(0.0, 1.0);
    return s / len;
}

static inline QImage toPremul(const QImage &src)
{
    if (src.format() == QImage::Format_ARGB32_Premultiplied)
        return src;
    return src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

/* One horizontal+vertical box pass with running sums on 4 channels. */
static void boxBlurPass(QImage &img, int r)
{
    if (r <= 0)
        return;
    const int w = img.width(), h = img.height();
    if (w == 0 || h == 0)
        return;
    const int win = 2 * r + 1;
    std::vector<quint32> tmp(size_t(w) * size_t(h));

    /* horizontal */
    for (int y = 0; y < h; ++y)
    {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        quint32 *out = &tmp[size_t(y) * size_t(w)];
        int sa = 0, sr = 0, sg = 0, sb = 0;
        for (int x = -r; x <= r; ++x)
        {
            const QRgb p = row[qBound(0, x, w - 1)];
            sa += qAlpha(p); sr += qRed(p); sg += qGreen(p); sb += qBlue(p);
        }
        for (int x = 0; x < w; ++x)
        {
            out[x] = qRgba(sr / win, sg / win, sb / win, sa / win);
            const QRgb pOut = row[qBound(0, x - r, w - 1)];
            const QRgb pIn = row[qBound(0, x + r + 1, w - 1)];
            sa += qAlpha(pIn) - qAlpha(pOut);
            sr += qRed(pIn) - qRed(pOut);
            sg += qGreen(pIn) - qGreen(pOut);
            sb += qBlue(pIn) - qBlue(pOut);
        }
    }
    /* vertical */
    for (int x = 0; x < w; ++x)
    {
        int sa = 0, sr = 0, sg = 0, sb = 0;
        for (int y = -r; y <= r; ++y)
        {
            const QRgb p = tmp[size_t(qBound(0, y, h - 1)) * size_t(w) + size_t(x)];
            sa += qAlpha(p); sr += qRed(p); sg += qGreen(p); sb += qBlue(p);
        }
        for (int y = 0; y < h; ++y)
        {
            QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
            row[x] = qRgba(sr / win, sg / win, sb / win, sa / win);
            const QRgb pOut = tmp[size_t(qBound(0, y - r, h - 1)) * size_t(w) + size_t(x)];
            const QRgb pIn = tmp[size_t(qBound(0, y + r + 1, h - 1)) * size_t(w) + size_t(x)];
            sa += qAlpha(pIn) - qAlpha(pOut);
            sr += qRed(pIn) - qRed(pOut);
            sg += qGreen(pIn) - qGreen(pOut);
            sb += qBlue(pIn) - qBlue(pOut);
        }
    }
}

void FinishRuntime::blurPremultiplied(QImage &premul, int radiusPx)
{
    if (radiusPx <= 0 || premul.isNull())
        return;
    if (premul.format() != QImage::Format_ARGB32_Premultiplied)
        premul = premul.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    /* three box passes with half-window r approximate a Gaussian of
     * sigma ~ r, so the visible extent (~2 sigma) matches `radiusPx` */
    const int r = std::max(1, int(std::lround(radiusPx / 2.0)));
    boxBlurPass(premul, r);
    boxBlurPass(premul, r);
    boxBlurPass(premul, r);
}

QImage FinishRuntime::alphaMask(const QImage &premul)
{
    QImage m(premul.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < premul.height(); ++y)
    {
        const QRgb *row = reinterpret_cast<const QRgb *>(premul.constScanLine(y));
        uchar *dst = m.scanLine(y);
        for (int x = 0; x < premul.width(); ++x)
            dst[x] = uchar(qAlpha(row[x]));
    }
    return m;
}

QImage FinishRuntime::directedBlur(const QImage &premulIn, const QPointF &direction,
                                   float distancePx, float falloff, int samples)
{
    QImage premul = toPremul(premulIn);
    if (distancePx <= 0.5f || samples < 2)
        return premul;
    QPointF dir = direction;
    const double len = std::sqrt(dir.x() * dir.x() + dir.y() * dir.y());
    if (len < 1e-6)
        return premul;
    dir /= len;

    const int w = premul.width(), h = premul.height();
    QImage out(premul.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);

    /* tap offsets from 0..distance along the direction (trailing streak);
     * weights taper with falloff so the subject's leading edge stays crisp. */
    const size_t nTaps = size_t(samples);
    std::vector<float> weights(nTaps, 0.0f);
    std::vector<QPointF> offsets(nTaps, QPointF());
    float wsum = 0.0f;
    /* The first tap is the subject itself; the trailing taps fade with an
     * exponential-ish taper so the streak thins into nothing instead of
     * ending in a hard-edged box. `falloff` 0 = uniform smear, 1 = tight. */
    for (int i = 0; i < samples; ++i)
    {
        const float t = float(i) / float(samples - 1);   /* 0..1 */
        const float k = 1.0f + 5.0f * qBound(0.0f, falloff, 1.0f);
        const float wgt = std::exp(-k * t);
        weights[size_t(i)] = std::max(0.01f, wgt);
        offsets[size_t(i)] = dir * (double(t) * double(distancePx));
        wsum += weights[size_t(i)];
    }

    for (int y = 0; y < h; ++y)
    {
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; ++x)
        {
            float a = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f;
            for (int i = 0; i < samples; ++i)
            {
                /* destination pixel x gathers source pixels AHEAD of it along
                 * the motion, so the smear extends behind the subject
                 * (opposite to the motion direction) */
                const int sx = int(std::lround(x + offsets[size_t(i)].x()));
                const int sy = int(std::lround(y + offsets[size_t(i)].y()));
                if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                    continue;
                const QRgb p = reinterpret_cast<const QRgb *>(premul.constScanLine(sy))[sx];
                const float wgt = weights[size_t(i)];
                a += qAlpha(p) * wgt; r += qRed(p) * wgt;
                g += qGreen(p) * wgt; b += qBlue(p) * wgt;
            }
            /* premultiplied averaging: dividing all four channels by the
             * weight sum keeps color/alpha consistent -> no dark halo. The
             * subject itself keeps full alpha (its own tap dominates); the
             * streak behind it fades with the taper. */
            dst[x] = qRgba(qBound(0, int(r / wsum + 0.5f), 255),
                           qBound(0, int(g / wsum + 0.5f), 255),
                           qBound(0, int(b / wsum + 0.5f), 255),
                           qBound(0, int(a / wsum + 0.5f), 255));
        }
    }
    return out;
}

/* ------------------------------------------------------------ chain apply */

int FinishRuntime::paddingFor(const QVector<EffectNode> &effects, const ArtScale &scale)
{
    float pad = 0.0f;
    for (const EffectNode &e : effects)
    {
        if (!e.enabled)
            continue;
        switch (e.type)
        {
        case EffectNode::Shadow:
        {
            const float off = e.useLightRig ? e.lightDistance
                : float(std::sqrt(e.offset.x() * e.offset.x() + e.offset.y() * e.offset.y()));
            pad = std::max(pad, scale.worldToDevice(e.blur * 1.5f + off + e.spread * e.blur));
            break;
        }
        case EffectNode::Glow:
            pad = std::max(pad, scale.worldToDevice(e.radius * 1.5f));
            break;
        case EffectNode::MotionBlur:
            pad = std::max(pad, scale.worldToDevice(e.distance));
            break;
        case EffectNode::Outline:
            pad = std::max(pad, scale.worldToDevice(e.width * 2.0f + 2.0f));
            break;
        default:
            break;
        }
    }
    return int(std::ceil(pad)) + 1;
}

namespace {

/* Colorize: replace rgb with `color` keeping (scaled) alpha; premultiplied. */
QImage colorizeAlpha(const QImage &premul, const QColor &color, float opacity)
{
    QImage out(premul.size(), QImage::Format_ARGB32_Premultiplied);
    const float cr = color.redF(), cg = color.greenF(), cb = color.blueF();
    for (int y = 0; y < premul.height(); ++y)
    {
        const QRgb *src = reinterpret_cast<const QRgb *>(premul.constScanLine(y));
        QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < premul.width(); ++x)
        {
            const float a = qAlpha(src[x]) / 255.0f * opacity;
            dst[x] = qRgba(int(cr * a * 255.0f + 0.5f), int(cg * a * 255.0f + 0.5f),
                           int(cb * a * 255.0f + 0.5f), int(a * 255.0f + 0.5f));
        }
    }
    return out;
}

/* Grow alpha by thresholding a blurred alpha (rounded dilation). */
void dilateAlpha(QImage &premul, int radiusPx, float softness)
{
    if (radiusPx <= 0)
        return;
    QImage grown = premul;
    FinishRuntime::blurPremultiplied(grown, radiusPx * 2);
    /* remap: anything above a low threshold becomes solid; softness widens
     * the ramp so the edge is not aliased */
    const float lo = 0.04f;
    const float hi = lo + 0.10f + 0.6f * qBound(0.0f, softness, 1.0f);
    for (int y = 0; y < grown.height(); ++y)
    {
        QRgb *row = reinterpret_cast<QRgb *>(grown.scanLine(y));
        for (int x = 0; x < grown.width(); ++x)
        {
            const float a = qAlpha(row[x]) / 255.0f;
            float t = (a - lo) / (hi - lo);
            t = qBound(0.0f, t, 1.0f);
            t = t * t * (3.0f - 2.0f * t);
            const float scaleF = a > 1e-5f ? t / a : 0.0f;
            row[x] = qRgba(qBound(0, int(qRed(row[x]) * scaleF + 0.5f), 255),
                           qBound(0, int(qGreen(row[x]) * scaleF + 0.5f), 255),
                           qBound(0, int(qBlue(row[x]) * scaleF + 0.5f), 255),
                           int(t * 255.0f + 0.5f));
        }
    }
    premul = grown;
}

void applyGhost(QImage &premul, const EffectNode &e)
{
    const float op = qBound(0.0f, e.opacity, 1.0f);
    const float ds = qBound(0.0f, e.desaturate, 1.0f);
    const float tintA = e.tint.alphaF();
    const float tr = e.tint.redF(), tg = e.tint.greenF(), tb = e.tint.blueF();
    for (int y = 0; y < premul.height(); ++y)
    {
        QRgb *row = reinterpret_cast<QRgb *>(premul.scanLine(y));
        for (int x = 0; x < premul.width(); ++x)
        {
            const int a = qAlpha(row[x]);
            if (a == 0)
                continue;
            /* un-premultiply, adjust, re-premultiply */
            const float af = a / 255.0f;
            float r = qRed(row[x]) / 255.0f / af;
            float g = qGreen(row[x]) / 255.0f / af;
            float b = qBlue(row[x]) / 255.0f / af;
            const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            r = r + (lum - r) * ds; g = g + (lum - g) * ds; b = b + (lum - b) * ds;
            r = r + (tr - r) * tintA; g = g + (tg - g) * tintA; b = b + (tb - b) * tintA;
            const float na = af * op;
            row[x] = qRgba(qBound(0, int(r * na * 255.0f + 0.5f), 255),
                           qBound(0, int(g * na * 255.0f + 0.5f), 255),
                           qBound(0, int(b * na * 255.0f + 0.5f), 255),
                           qBound(0, int(na * 255.0f + 0.5f), 255));
        }
    }
}

} // namespace

FinishResult FinishRuntime::apply(const QImage &source,
                                  const QVector<EffectNode> &effects,
                                  const ArtLightRig &rig, const ArtScale &scale)
{
    FinishResult res;
    bool any = false;
    for (const EffectNode &e : effects)
        if (e.enabled && e.type != EffectNode::None)
            any = true;
    if (!any || source.isNull())
    {
        res.image = source;
        res.originOffset = QPoint(0, 0);
        return res;
    }

    const int pad = paddingFor(effects, scale);
    QImage base(source.width() + 2 * pad, source.height() + 2 * pad,
                QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::transparent);
    {
        QPainter p(&base);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(pad, pad, toPremul(source));
    }

    /* 1) self effects (modify the subject pixels): motion blur, ghost */
    QImage self = base;
    for (const EffectNode &e : effects)
    {
        if (!e.enabled)
            continue;
        if (e.type == EffectNode::MotionBlur)
            self = directedBlur(self, e.direction, scale.worldToDevice(e.distance),
                                e.falloff, std::max(2, e.samples));
        else if (e.type == EffectNode::Ghost)
            applyGhost(self, e);
    }

    /* 2) under effects derived from the (processed) subject alpha */
    QImage under(base.size(), QImage::Format_ARGB32_Premultiplied);
    under.fill(Qt::transparent);
    QPainter up(&under);
    up.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (const EffectNode &e : effects)
    {
        if (!e.enabled)
            continue;
        switch (e.type)
        {
        case EffectNode::Shadow:
        {
            QImage sh = colorizeAlpha(self, e.color, qBound(0.0f, e.opacity, 1.0f));
            if (e.spread > 0.0f)
                dilateAlpha(sh, int(std::lround(scale.worldToDevice(e.spread * e.blur))), 0.3f);
            blurPremultiplied(sh, int(std::lround(scale.worldToDevice(e.blur))));
            QPointF off = e.offset;
            if (e.useLightRig)
                off = shadowDirection(rig) * double(e.lightDistance);
            up.drawImage(QPointF(scale.worldToDevice(float(off.x())),
                                 scale.worldToDevice(float(off.y()))), sh);
            break;
        }
        case EffectNode::Glow:
        {
            QImage gl = colorizeAlpha(self, e.color, 1.0f);
            /* threshold from alpha: faint fringes do not seed the glow */
            const int thr = int(qBound(0.0f, e.threshold, 1.0f) * 255.0f);
            for (int y = 0; y < gl.height(); ++y)
            {
                QRgb *row = reinterpret_cast<QRgb *>(gl.scanLine(y));
                for (int x = 0; x < gl.width(); ++x)
                    if (qAlpha(row[x]) < thr)
                        row[x] = 0;
            }
            blurPremultiplied(gl, int(std::lround(scale.worldToDevice(e.radius))));
            /* intensity: scale premultiplied channels, clamp */
            const float k = qBound(0.0f, e.intensity, 4.0f);
            for (int y = 0; y < gl.height(); ++y)
            {
                QRgb *row = reinterpret_cast<QRgb *>(gl.scanLine(y));
                for (int x = 0; x < gl.width(); ++x)
                {
                    const int a = qBound(0, int(qAlpha(row[x]) * k), 255);
                    row[x] = qRgba(qBound(0, int(qRed(row[x]) * k), a),
                                   qBound(0, int(qGreen(row[x]) * k), a),
                                   qBound(0, int(qBlue(row[x]) * k), a), a);
                }
            }
            up.drawImage(0, 0, gl);
            break;
        }
        case EffectNode::Outline:
        {
            QImage ol = colorizeAlpha(self, e.color, qBound(0.0f, e.opacity, 1.0f));
            dilateAlpha(ol, std::max(1, int(std::lround(scale.worldToDevice(e.width)))),
                        e.softness);
            up.drawImage(0, 0, ol);
            break;
        }
        default:
            break;
        }
    }
    up.end();

    /* 3) composite: under effects, then the subject */
    {
        QPainter p(&under);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.drawImage(0, 0, self);
    }
    res.image = under;
    res.originOffset = QPoint(-pad, -pad);
    return res;
}

QVector<EffectNode> FinishRuntime::defaultsForRole(LayerRole role, const QPointF &motion,
                                                   float motionDistanceWorld)
{
    QVector<EffectNode> out;
    switch (role)
    {
    case LayerRole::Hero:
        /* clarity first: no finish by default */
        break;
    case LayerRole::Support:
    {
        EffectNode s = EffectNode::makeShadow(6.0f, 0.32f);
        s.lightDistance = 6.0f;
        out.append(s);
        break;
    }
    case LayerRole::Ghost:
        out.append(EffectNode::makeGhost(0.38f, 0.45f));
        break;
    case LayerRole::Transition:
        /* blur the full silhouette first, then fade: the echo stays legible */
        out.append(EffectNode::makeMotionBlur(motion, std::max(4.0f, motionDistanceWorld),
                                              0.85f, 16));
        out.append(EffectNode::makeGhost(0.6f, 0.2f));
        break;
    case LayerRole::None:
    default:
        break;
    }
    return out;
}

} // namespace art
} // namespace paintbrush

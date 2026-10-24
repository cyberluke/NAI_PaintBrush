/*****************************************************************************
 * badges.cpp : NAI Paintbrush — tiny curated procedural badge library
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "badges.hpp"

#include "../runtime/art_noise.hpp"

#include <QPainter>
#include <QPen>
#include <QtMath>
#include <cmath>

namespace paintbrush {
namespace art {
namespace badges {

QString kindName(Kind k)
{
    switch (k)
    {
    case Kind::Heart: return QStringLiteral("heart");
    case Kind::Star: return QStringLiteral("star");
    case Kind::Sparkle: return QStringLiteral("sparkle");
    case Kind::Starburst: return QStringLiteral("starburst");
    case Kind::Tape: return QStringLiteral("tape");
    case Kind::Arrow: return QStringLiteral("arrow");
    case Kind::Underline: return QStringLiteral("underline");
    case Kind::Halo: return QStringLiteral("halo");
    case Kind::BrokenHeart: return QStringLiteral("broken_heart");
    case Kind::Crown: return QStringLiteral("crown");
    case Kind::GlossyLabel: return QStringLiteral("glossy_label");
    default: return QStringLiteral("heart");
    }
}

Kind kindFromName(const QString &name)
{
    for (int i = 0; i < int(Kind::Count); ++i)
        if (kindName(Kind(i)) == name)
            return Kind(i);
    return Kind::Heart;
}

QStringList allNames()
{
    QStringList out;
    for (int i = 0; i < int(Kind::Count); ++i)
        out.append(kindName(Kind(i)));
    return out;
}

namespace {

QPainterPath heartPath(const QSizeF &s)
{
    const qreal w = s.width(), h = s.height();
    QPainterPath p;
    p.moveTo(w * 0.5, h * 0.95);
    p.cubicTo(w * -0.05, h * 0.55, w * 0.02, h * 0.02, w * 0.5, h * 0.28);
    p.cubicTo(w * 0.98, h * 0.02, w * 1.05, h * 0.55, w * 0.5, h * 0.95);
    p.closeSubpath();
    return p;
}

QPainterPath starPath(const QSizeF &s, int points, qreal inner)
{
    const qreal cx = s.width() / 2, cy = s.height() / 2;
    const qreal R = std::min(cx, cy);
    QPainterPath p;
    for (int i = 0; i < points * 2; ++i)
    {
        const qreal r = (i % 2 == 0) ? R : R * inner;
        const qreal a = -M_PI / 2 + i * M_PI / points;
        const QPointF pt(cx + r * std::cos(a), cy + r * std::sin(a));
        if (i == 0)
            p.moveTo(pt);
        else
            p.lineTo(pt);
    }
    p.closeSubpath();
    return p;
}

QPainterPath sparklePath(const QSizeF &s)
{
    /* four-point sparkle with concave sides */
    const qreal cx = s.width() / 2, cy = s.height() / 2;
    const qreal R = std::min(cx, cy);
    const qreal k = R * 0.18;
    QPainterPath p;
    p.moveTo(cx, cy - R);
    p.quadTo(cx + k, cy - k, cx + R, cy);
    p.quadTo(cx + k, cy + k, cx, cy + R);
    p.quadTo(cx - k, cy + k, cx - R, cy);
    p.quadTo(cx - k, cy - k, cx, cy - R);
    p.closeSubpath();
    return p;
}

QPainterPath tapePath(const QSizeF &s, unsigned seed)
{
    /* a strip with torn ends; the tear is deterministic from the seed */
    const qreal w = s.width(), h = s.height();
    QPainterPath p;
    const int teeth = 6;
    p.moveTo(0, 0);
    p.lineTo(w, 0);
    for (int i = 0; i <= teeth; ++i)
    {
        const qreal t = qreal(i) / teeth;
        const qreal jag = (i % 2 == 0 ? 0.06 : -0.06) * w *
                          (0.6 + 0.8 * artHashUnitf(seed, 100u + unsigned(i)));
        p.lineTo(w + jag, h * t);
    }
    p.lineTo(0, h);
    for (int i = teeth; i >= 0; --i)
    {
        const qreal t = qreal(i) / teeth;
        const qreal jag = (i % 2 == 0 ? -0.06 : 0.06) * w *
                          (0.6 + 0.8 * artHashUnitf(seed, 200u + unsigned(i)));
        p.lineTo(jag, h * t);
    }
    p.closeSubpath();
    return p;
}

QPainterPath arrowPath(const QSizeF &s)
{
    const qreal w = s.width(), h = s.height();
    QPainterPath p;
    p.moveTo(0, h * 0.38);
    p.lineTo(w * 0.62, h * 0.38);
    p.lineTo(w * 0.62, h * 0.12);
    p.lineTo(w, h * 0.5);
    p.lineTo(w * 0.62, h * 0.88);
    p.lineTo(w * 0.62, h * 0.62);
    p.lineTo(0, h * 0.62);
    p.closeSubpath();
    return p;
}

QPainterPath underlinePath(const QSizeF &s, unsigned seed)
{
    /* a brushy wobble stroke rendered as a filled ribbon */
    const qreal w = s.width(), h = s.height();
    QPainterPath top, bottom;
    const int n = 12;
    for (int i = 0; i <= n; ++i)
    {
        const qreal t = qreal(i) / n;
        const qreal wob = (artHashUnitf(seed, 300u + unsigned(i)) - 0.5) * h * 0.35;
        const qreal thick = h * (0.28 + 0.22 * std::sin(t * M_PI));
        const QPointF a(w * t, h * 0.5 + wob - thick / 2);
        const QPointF b(w * t, h * 0.5 + wob + thick / 2);
        if (i == 0) { top.moveTo(a); bottom.moveTo(b); }
        else { top.lineTo(a); bottom.lineTo(b); }
    }
    QPainterPath p = top;
    for (int i = n; i >= 0; --i)
        p.lineTo(bottom.elementAt(i).x, bottom.elementAt(i).y);
    p.closeSubpath();
    return p;
}

QPainterPath haloPath(const QSizeF &s)
{
    const qreal w = s.width(), h = s.height();
    QPainterPath p;
    p.addEllipse(QRectF(0, 0, w, h));
    const qreal ix = w * 0.14, iy = h * 0.14;
    QPainterPath inner;
    inner.addEllipse(QRectF(ix, iy, w - 2 * ix, h - 2 * iy));
    return p.subtracted(inner);
}

QPainterPath brokenHeartPath(const QSizeF &s)
{
    QPainterPath heart = heartPath(s);
    const qreal w = s.width(), h = s.height();
    QPainterPath crack;
    crack.moveTo(w * 0.50, h * 0.22);
    crack.lineTo(w * 0.40, h * 0.42);
    crack.lineTo(w * 0.56, h * 0.56);
    crack.lineTo(w * 0.44, h * 0.74);
    crack.lineTo(w * 0.52, h * 1.0);
    crack.lineTo(w * 0.60, h * 1.0);
    crack.lineTo(w * 0.50, h * 0.76);
    crack.lineTo(w * 0.62, h * 0.56);
    crack.lineTo(w * 0.46, h * 0.42);
    crack.lineTo(w * 0.56, h * 0.22);
    crack.closeSubpath();
    return heart.subtracted(crack);
}

QPainterPath crownPath(const QSizeF &s)
{
    const qreal w = s.width(), h = s.height();
    QPainterPath p;
    p.moveTo(w * 0.05, h * 0.95);
    p.lineTo(w * 0.0, h * 0.35);
    p.lineTo(w * 0.27, h * 0.58);
    p.lineTo(w * 0.5, h * 0.05);
    p.lineTo(w * 0.73, h * 0.58);
    p.lineTo(w * 1.0, h * 0.35);
    p.lineTo(w * 0.95, h * 0.95);
    p.closeSubpath();
    return p;
}

QPainterPath labelPath(const QSizeF &s)
{
    QPainterPath p;
    const qreal r = std::min(s.width(), s.height()) * 0.28;
    p.addRoundedRect(QRectF(0, 0, s.width(), s.height()), r, r);
    return p;
}

} // namespace

QPainterPath path(Kind k, const QSizeF &size, unsigned seed)
{
    switch (k)
    {
    case Kind::Heart: return heartPath(size);
    case Kind::Star: return starPath(size, 5, 0.46);
    case Kind::Sparkle: return sparklePath(size);
    case Kind::Starburst: return starPath(size, 12, 0.72);
    case Kind::Tape: return tapePath(size, seed);
    case Kind::Arrow: return arrowPath(size);
    case Kind::Underline: return underlinePath(size, seed);
    case Kind::Halo: return haloPath(size);
    case Kind::BrokenHeart: return brokenHeartPath(size);
    case Kind::Crown: return crownPath(size);
    case Kind::GlossyLabel: return labelPath(size);
    default: return heartPath(size);
    }
}

QImage render(Kind k, const QSizeF &size, const QColor &fill, const QColor &stroke,
              float strokeWidth, float scale, unsigned seed)
{
    const qreal sc = std::max(0.05f, scale);
    const QSizeF px(size.width() * sc, size.height() * sc);
    const qreal pad = (stroke.alpha() > 0 ? strokeWidth * sc : 0.0) + 2.0;
    QImage img(int(std::ceil(px.width() + 2 * pad)), int(std::ceil(px.height() + 2 * pad)),
               QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(pad, pad);
    QPainterPath shape = path(k, px, seed);
    p.fillPath(shape, fill);
    if (stroke.alpha() > 0 && strokeWidth > 0.0f)
    {
        QPen pen(stroke, strokeWidth * sc, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.strokePath(shape, pen);
    }
    /* glossy label: a soft highlight band (procedural, recolorable) */
    if (k == Kind::GlossyLabel)
    {
        QLinearGradient g(0, 0, 0, px.height());
        g.setColorAt(0.0, QColor(255, 255, 255, 110));
        g.setColorAt(0.45, QColor(255, 255, 255, 20));
        g.setColorAt(0.5, QColor(255, 255, 255, 0));
        p.setClipPath(shape);
        p.fillRect(QRectF(0, 0, px.width(), px.height()), g);
    }
    p.end();
    return img;
}

QList<Kind> family(const QString &familyId)
{
    if (familyId == QLatin1String("pop"))
        return { Kind::Starburst, Kind::Star, Kind::Arrow, Kind::GlossyLabel };
    if (familyId == QLatin1String("y2k"))
        return { Kind::Heart, Kind::Sparkle, Kind::Star, Kind::GlossyLabel };
    if (familyId == QLatin1String("luxury"))
        return { Kind::Sparkle, Kind::Halo, Kind::Underline, Kind::Crown };
    if (familyId == QLatin1String("emo"))
        return { Kind::BrokenHeart, Kind::Tape, Kind::Underline, Kind::Halo };
    if (familyId == QLatin1String("chrome"))
        return { Kind::Sparkle, Kind::Starburst, Kind::Halo, Kind::Arrow };
    /* editorial */
    return { Kind::Underline, Kind::Tape, Kind::Arrow, Kind::Star };
}

} // namespace badges
} // namespace art
} // namespace paintbrush

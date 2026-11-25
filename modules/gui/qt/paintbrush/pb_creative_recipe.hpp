/*****************************************************************************
 * pb_creative_recipe.hpp : NAI Paintbrush — pure creative patch recipes
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_CREATIVE_RECIPE_HPP
#define PB_CREATIVE_RECIPE_HPP

/* The Creative Pad recipes are pure functions of a CreativeBuildInput value:
 * no QObject, controller, document or global state is read here. That is what
 * makes a full-resolution build safe to run on a worker thread while the GUI
 * keeps painting, and what keeps a committed/undone/replayed look identical
 * for the same seed and pad state.
 *
 * This header is deliberately VLC-free (QImage/QPainter + pb_creative_art.hpp)
 * so the standalone recipe harness can compile the exact production builder. */

#include "pb_blend.hpp"
#include "pb_creative_art.hpp"
#include "pb_creative_types.hpp"
#include "pb_creative_vector.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QLinearGradient>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>
#include <QVariantMap>
#include <QVector>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>

namespace paintbrush {

namespace creative {
namespace recipe {

/* ------------------------------------------------------------- primitives */

inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

namespace detail {

inline QImage tintPreserveAlpha(const QImage &img, const QColor &color)
{
    if (img.isNull())
        return {};
    QImage out = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < out.height(); ++y)
    {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x)
        {
            const int a = qAlpha(line[x]);
            line[x] = qRgba(color.red(), color.green(), color.blue(), a);
        }
    }
    return out;
}

inline void fadeHorizontally(QImage &img, bool leftStrong)
{
    if (img.isNull())
        return;
    QPainter p(&img);
    p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    QLinearGradient g(0, 0, img.width(), 0);
    if (leftStrong)
    {
        g.setColorAt(0.0, QColor(0, 0, 0, 255));
        g.setColorAt(0.55, QColor(0, 0, 0, 150));
        g.setColorAt(1.0, QColor(0, 0, 0, 0));
    }
    else
    {
        g.setColorAt(0.0, QColor(0, 0, 0, 0));
        g.setColorAt(0.45, QColor(0, 0, 0, 150));
        g.setColorAt(1.0, QColor(0, 0, 0, 255));
    }
    p.fillRect(img.rect(), g);
}

inline QRectF scaleRectAbout(const QRectF &r, qreal factor, qreal dx, qreal dy)
{
    const QPointF c = r.center();
    const QSizeF s = r.size() * factor;
    return QRectF(c.x() - s.width() / 2.0 + dx,
                  c.y() - s.height() / 2.0 + dy,
                  s.width(), s.height());
}

inline QImage stageCanvas(const QSize &size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

inline void drawScaled(QImage &dst, const QImage &src, const QRectF &rect)
{
    if (src.isNull() || rect.isEmpty())
        return;
    QPainter p(&dst);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(rect, src);
}

inline bool isHoloMaterialPatch(const QString &patchId)
{
    return patchId == QLatin1String("holo-liquid")
           || patchId == QLatin1String("holo-opal")
           || patchId == QLatin1String("holo-chrome")
           || patchId == QLatin1String("holo-prism")
           || patchId == QLatin1String("holo-wrap")
           || patchId == QLatin1String("holo-chameleon")
           || patchId == QLatin1String("holo-glitter")
           || patchId == QLatin1String("holo-livery");
}

/* Rotate a colour's hue by a fraction of the wheel. Used by the vector
 * destinations that shift particles/arcs without changing the base palette. */
inline QColor hueShifted(const QColor &src, float amount)
{
    float h = 0.0f, s = 0.0f, v = 0.0f, a = 0.0f;
    src.getHsvF(&h, &s, &v, &a);
    if (h < 0.0f)
        h = 0.0f;
    QColor out =
        QColor::fromHsvF(std::fmod(h + amount + 1.0f, 1.0f), s, v);
    out.setAlpha(src.alpha());
    return out;
}

inline bool isAutomotivePatch(const QString &patchId)
{
    return patchId == QLatin1String("holo-wrap")
           || patchId == QLatin1String("holo-chameleon")
           || patchId == QLatin1String("holo-glitter")
           || patchId == QLatin1String("holo-livery");
}

inline HoloMaterialId holoMaterialIdForPatch(const QString &patchId)
{
    if (patchId == QLatin1String("holo-opal"))
        return HoloMaterialId::OpalDream;
    if (patchId == QLatin1String("holo-chrome"))
        return HoloMaterialId::ChromaticChrome;
    if (patchId == QLatin1String("holo-prism"))
        return HoloMaterialId::PrismaticFantasy;
    if (patchId == QLatin1String("holo-wrap"))
        return HoloMaterialId::HolographicWrap;
    if (patchId == QLatin1String("holo-chameleon"))
        return HoloMaterialId::ChameleonPaint;
    if (patchId == QLatin1String("holo-glitter"))
        return HoloMaterialId::GlitterMetallic;
    if (patchId == QLatin1String("holo-livery"))
        return HoloMaterialId::ElectricLivery;
    return HoloMaterialId::LiquidHologram;
}

/* ------------------------------------------------------- paper theatre props */
/* Ported from master's pb_creative.cpp. These builders are pure Qt: every
 * mark is seeded geometry drawn with QPainter, so a saved Paper Theatre look
 * replays byte-identically and needs no external asset. */

constexpr qreal kTau = 6.283185307179586;

struct SceneRng
{
    std::mt19937 engine;
    explicit SceneRng(quint32 seed)
        : engine(seed ? seed : 0x9E3779B9u)
    {
    }
    quint32 next() { return quint32(engine()); }
    float unit() { return float(next() & 0xFFFFFFu) / float(0x1000000u); }
    float range(float a, float b) { return a + (b - a) * unit(); }
    int pick(int count) { return count <= 0 ? 0 : int(next() % quint32(count)); }
};

struct SceneProp
{
    QImage image;
    QPointF topLeft;
};

template <typename DrawFn>
SceneProp renderProp(const QSize &stage, const QRectF &bounds, DrawFn &&draw)
{
    if (!stage.isValid() || stage.isEmpty())
        return {};
    const QRectF clipped =
        bounds.intersected(QRectF(QPointF(0, 0), QSizeF(stage)));
    if (clipped.width() < 1.0 || clipped.height() < 1.0)
        return {};
    const QPointF topLeft(std::floor(clipped.left()),
                          std::floor(clipped.top()));
    const QSize size(
        qMax(1, int(std::ceil(clipped.right() - topLeft.x()))),
        qMax(1, int(std::ceil(clipped.bottom() - topLeft.y()))));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.translate(-topLeft);
    draw(painter);
    painter.end();
    return { image, topLeft };
}

inline QPainterPath fivePointStar(const QPointF &center, qreal radius,
                                  qreal innerRatio = 0.43,
                                  qreal rotationDeg = -90.0)
{
    QPainterPath path;
    for (int i = 0; i < 10; ++i)
    {
        const qreal r = (i % 2 == 0) ? radius : radius * innerRatio;
        const qreal a = qDegreesToRadians(rotationDeg + i * 36.0);
        const QPointF point(center.x() + std::cos(a) * r,
                            center.y() + std::sin(a) * r);
        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    path.closeSubpath();
    return path;
}

inline QPainterPath irregularBlob(const QPointF &center, qreal rx, qreal ry,
                                  SceneRng &rng, int points = 11,
                                  qreal jitter = 0.16)
{
    QPainterPath path;
    for (int i = 0; i < points; ++i)
    {
        const qreal a = kTau * qreal(i) / qreal(points);
        const qreal r = 1.0 + rng.range(float(-jitter), float(jitter));
        const QPointF point(center.x() + std::cos(a) * rx * r,
                            center.y() + std::sin(a) * ry * r);
        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    path.closeSubpath();
    return path;
}

/* A cut-paper horizon plane. Returns a tight strip; the caller places it at
 * `topLeft` and the bottom is always the stage bottom. */
inline SceneProp makePaperPlane(const QSize &stage, const QColor &paper,
                                const QColor &core, qreal horizon,
                                qreal amplitude, qreal phase, int waves)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal top = qMax<qreal>(0.0, horizon - amplitude * 1.9 - h * 0.012);
    QVector<QPointF> points;
    const int steps = 38;
    for (int i = 0; i <= steps; ++i)
    {
        const qreal t = qreal(i) / qreal(steps);
        const qreal x = w * t;
        const qreal y = horizon
                        + std::sin(t * waves * kTau + phase) * amplitude
                        + std::sin(t * waves * 2.7 * kTau + phase * 1.7)
                              * amplitude * 0.32;
        points.append(QPointF(x, y));
    }
    return renderProp(stage, QRectF(0, top, w, h - top),
                      [&](QPainter &p) {
        QPainterPath path;
        path.moveTo(0, h);
        path.lineTo(points.first());
        for (int i = 1; i < points.size(); ++i)
            path.lineTo(points[i]);
        path.lineTo(w, h);
        path.closeSubpath();

        const qreal thickness = qMax<qreal>(1.5, h * 0.0045);
        QPainterPath back = path;
        back.translate(0, -thickness);
        p.setPen(Qt::NoPen);
        p.setBrush(core);
        p.drawPath(back);

        QLinearGradient g(0, horizon - amplitude, 0, h);
        g.setColorAt(0.0, paper.lighter(108));
        g.setColorAt(0.55, paper);
        g.setColorAt(1.0, paper.darker(118));
        p.setBrush(g);
        p.drawPath(path);

        QPainterPath edge;
        edge.moveTo(points.first());
        for (int i = 1; i < points.size(); ++i)
            edge.lineTo(points[i]);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0024), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(edge);
    });
}

inline SceneProp makePaperTree(const QSize &stage, quint32 seed,
                               const QColor &paper, const QColor &core,
                               qreal x, qreal baseY, qreal height,
                               qreal spread)
{
    const qreal h = stage.height();
    const qreal top = baseY - height;
    SceneRng rng(seed);
    return renderProp(stage,
                      QRectF(x - spread * 1.35, top - height * 0.10,
                             spread * 2.7, height * 1.10),
                      [&](QPainter &p) {
        const qreal lean = rng.range(float(-spread * 0.10f),
                                     float(spread * 0.10f));
        const qreal trunkTop = baseY - height * 0.58;
        const qreal trunkW = qMax<qreal>(2.0, spread * 0.10);

        QPainterPath trunk;
        trunk.moveTo(x - trunkW * 0.62, baseY);
        trunk.quadTo(x - trunkW * 0.28 + lean * 0.4, baseY - height * 0.28,
                     x + lean - trunkW * 0.34, trunkTop);
        trunk.lineTo(x + lean + trunkW * 0.34, trunkTop);
        trunk.quadTo(x + trunkW * 0.30 + lean * 0.4, baseY - height * 0.28,
                     x + trunkW * 0.72, baseY);
        trunk.closeSubpath();
        QLinearGradient trunkFill(x, 0, x + trunkW, 0);
        trunkFill.setColorAt(0.0, paper.darker(132));
        trunkFill.setColorAt(0.55, paper.darker(112));
        trunkFill.setColorAt(1.0, paper.darker(142));
        p.setPen(Qt::NoPen);
        p.setBrush(trunkFill);
        p.drawPath(trunk);

        const QPointF canopyMain(x + lean * 0.35, baseY - height * 0.72);
        const QPointF canopyLeft(x - spread * 0.44, baseY - height * 0.57);
        const QPointF canopyRight(x + spread * 0.46, baseY - height * 0.60);
        const QPointF canopyTop(x + spread * 0.10, baseY - height * 0.90);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(paper.darker(125), qMax<qreal>(1.6, h * 0.0045),
                      Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(x + lean, trunkTop), canopyMain);
        p.drawLine(QPointF(x + lean * 0.7, baseY - height * 0.46),
                   canopyLeft);
        p.drawLine(QPointF(x + lean * 0.7, baseY - height * 0.50),
                   canopyRight);

        struct Blob
        {
            QPointF center;
            qreal rx;
            qreal ry;
        };
        const Blob blobs[] = {
            { canopyTop, spread * 0.42, height * 0.16 },
            { canopyLeft, spread * 0.47, height * 0.19 },
            { canopyRight, spread * 0.49, height * 0.20 },
            { canopyMain, spread * 0.62, height * 0.25 },
        };
        for (const Blob &blob : blobs)
        {
            const QPainterPath shape =
                irregularBlob(blob.center, blob.rx, blob.ry, rng);
            QLinearGradient fill(0, blob.center.y() - blob.ry, 0,
                                 blob.center.y() + blob.ry);
            fill.setColorAt(0.0, paper.lighter(110));
            fill.setColorAt(0.55, paper);
            fill.setColorAt(1.0, paper.darker(122));
            p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0022),
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(fill);
            p.drawPath(shape);
        }
    });
}

inline SceneProp makePaperCastle(const QSize &stage, quint32 seed,
                                 const QColor &paper, const QColor &core,
                                 qreal centerX, qreal baseY, qreal width,
                                 qreal height)
{
    const qreal h = stage.height();
    Q_UNUSED(seed);
    return renderProp(stage,
                      QRectF(centerX - width * 0.62, baseY - height * 1.32,
                             width * 1.24, height * 1.34),
                      [&](QPainter &p) {
        QLinearGradient fill(0, baseY - height, 0, baseY);
        fill.setColorAt(0.0, paper.lighter(108));
        fill.setColorAt(0.6, paper);
        fill.setColorAt(1.0, paper.darker(120));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);

        const qreal wallH = height * 0.44;
        const qreal wallTop = baseY - wallH;
        p.drawRect(QRectF(centerX - width * 0.44, wallTop,
                          width * 0.88, wallH));

        struct Tower
        {
            qreal cx;
            qreal tw;
            qreal th;
            qreal roof;
        };
        const Tower towers[] = {
            { centerX - width * 0.34, width * 0.18, height * 0.72,
              height * 0.13 },
            { centerX, width * 0.21, height * 0.98, height * 0.16 },
            { centerX + width * 0.34, width * 0.17, height * 0.68,
              height * 0.12 },
        };

        const auto crenellate = [&](qreal left, qreal right, qreal top) {
            const qreal cw = width * 0.030;
            const int count = qMax(2, int((right - left) / (cw * 1.9)));
            for (int i = 0; i < count; ++i)
            {
                const qreal cx = left + (right - left) * (qreal(i) + 0.5)
                                            / qreal(count);
                p.drawRect(QRectF(cx - cw / 2, top - cw * 0.75, cw,
                                  cw * 0.75));
            }
        };

        for (const Tower &tower : towers)
        {
            const qreal left = tower.cx - tower.tw / 2;
            const qreal top = baseY - tower.th;
            p.drawRect(QRectF(left, top, tower.tw, tower.th));
            crenellate(left, left + tower.tw, top);
            QPainterPath roof;
            roof.moveTo(left - tower.tw * 0.10, top);
            roof.lineTo(tower.cx, top - tower.roof);
            roof.lineTo(left + tower.tw + tower.tw * 0.10, top);
            roof.closeSubpath();
            p.setBrush(paper.darker(118));
            p.drawPath(roof);
            p.setBrush(fill);
        }
        crenellate(centerX - width * 0.44, centerX + width * 0.44, wallTop);

        /* cut-paper windows and gate: the backdrop must show through */
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setBrush(Qt::black);
        p.setPen(Qt::NoPen);
        for (const Tower &tower : towers)
        {
            const qreal ww = tower.tw * 0.30;
            const qreal wh = qMin<qreal>(height * 0.13, tower.th * 0.24);
            const qreal wy = baseY - tower.th * 0.56;
            p.drawRoundedRect(QRectF(tower.cx - ww / 2, wy, ww, wh),
                              ww / 2, ww / 2);
        }
        p.drawRoundedRect(QRectF(centerX - width * 0.055,
                                 baseY - height * 0.24, width * 0.11,
                                 height * 0.24),
                          width * 0.055, width * 0.055);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);

        /* paper edges */
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0022), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawLine(QPointF(centerX - width * 0.44, wallTop),
                   QPointF(centerX + width * 0.44, wallTop));
        for (const Tower &tower : towers)
        {
            const qreal left = tower.cx - tower.tw / 2;
            const qreal top = baseY - tower.th;
            p.drawLine(QPointF(left, top), QPointF(left + tower.tw, top));
            QPainterPath roof;
            roof.moveTo(left - tower.tw * 0.10, top);
            roof.lineTo(tower.cx, top - tower.roof);
            roof.lineTo(left + tower.tw + tower.tw * 0.10, top);
            p.drawPath(roof);
        }
        /* pennant on the tallest tower */
        const qreal flagBase = baseY - height * 0.98 - height * 0.16;
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.002)));
        p.drawLine(QPointF(centerX, flagBase),
                   QPointF(centerX, flagBase - height * 0.11));
        QPainterPath flag;
        flag.moveTo(centerX, flagBase - height * 0.11);
        flag.lineTo(centerX + width * 0.075, flagBase - height * 0.085);
        flag.lineTo(centerX, flagBase - height * 0.06);
        flag.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(core);
        p.drawPath(flag);
    });
}

inline SceneProp makePaperCurtain(const QSize &stage, const QColor &paper,
                                  const QColor &core, bool left)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal side = w * 0.34;
    const QRectF bounds = left ? QRectF(0, 0, side, h)
                               : QRectF(w - side, 0, side, h);
    return renderProp(stage, bounds, [&](QPainter &p) {
        const qreal outerX = left ? 0.0 : w;
        const qreal dir = left ? 1.0 : -1.0;
        const auto X = [&](qreal local) { return outerX + dir * local; };

        QPainterPath path;
        path.moveTo(X(0), 0);
        path.lineTo(X(w * 0.31), 0);
        path.cubicTo(X(w * 0.26), h * 0.20, X(w * 0.17), h * 0.40,
                     X(w * 0.19), h * 0.56);
        path.cubicTo(X(w * 0.21), h * 0.72, X(w * 0.31), h * 0.88,
                     X(w * 0.30), h);
        path.lineTo(X(0), h);
        path.closeSubpath();

        QLinearGradient fill(left ? 0 : w, 0, left ? w * 0.32 : w * 0.68, 0);
        fill.setColorAt(0.0, paper.darker(158));
        fill.setColorAt(0.55, paper.darker(120));
        fill.setColorAt(1.0, paper);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(path);

        p.save();
        p.setClipPath(path);
        for (int i = 0; i < 7; ++i)
        {
            const qreal lx = w * (0.035 + i * 0.042);
            const qreal sway = w * (i % 2 == 0 ? 0.016 : -0.014);
            QPainterPath fold;
            fold.moveTo(X(lx), 0);
            fold.cubicTo(X(lx + sway), h * 0.30, X(lx - sway), h * 0.62,
                         X(lx + sway * 0.5), h);
            QColor foldColor = (i % 2 == 0) ? paper.lighter(112)
                                            : paper.darker(132);
            foldColor.setAlpha(72);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(foldColor, qMax<qreal>(1.2, w * 0.010),
                          Qt::SolidLine, Qt::RoundCap));
            p.drawPath(fold);
        }
        p.restore();

        QPainterPath edge;
        edge.moveTo(X(w * 0.31), 0);
        edge.cubicTo(X(w * 0.26), h * 0.20, X(w * 0.17), h * 0.40,
                     X(w * 0.19), h * 0.56);
        edge.cubicTo(X(w * 0.21), h * 0.72, X(w * 0.31), h * 0.88,
                     X(w * 0.30), h);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.2, h * 0.0032), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(edge);

        /* tie-back: a paper band pinching the drape */
        p.save();
        p.translate(X(w * 0.185), h * 0.56);
        p.rotate(left ? -7.0 : 7.0);
        QLinearGradient band(-w * 0.07, 0, w * 0.07, 0);
        band.setColorAt(0.0, paper.darker(150));
        band.setColorAt(0.5, paper.darker(126));
        band.setColorAt(1.0, paper.darker(160));
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0018)));
        p.setBrush(band);
        p.drawRoundedRect(QRectF(-w * 0.075, -h * 0.013, w * 0.15,
                                 h * 0.026),
                          h * 0.008, h * 0.008);
        p.restore();
    });
}

inline SceneProp makePaperFan(const QSize &stage, quint32 seed,
                              const QColor &paper, const QColor &alt,
                              const QColor &core, const QPointF &center,
                              qreal radius, qreal startDeg, qreal spanDeg)
{
    SceneRng rng(seed);
    const QRectF bounds(center.x() - radius * 1.25,
                        center.y() - radius * 1.25, radius * 2.5,
                        radius * 1.85);
    return renderProp(stage, bounds, [&](QPainter &p) {
        const int sectors = 9;
        const qreal step = spanDeg / qreal(sectors);
        const QRectF disc(center.x() - radius, center.y() - radius,
                          radius * 2.0, radius * 2.0);
        for (int i = 0; i < sectors; ++i)
        {
            QPainterPath sector;
            sector.moveTo(center);
            sector.arcTo(disc, startDeg + i * step, step);
            sector.closeSubpath();
            QColor c = (i % 2 == 0) ? paper : alt;
            c = c.lighter(int(rng.range(-4.0f, 8.0f)));
            c.setAlpha(232);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawPath(sector);
        }
        p.setBrush(Qt::NoBrush);
        for (int i = 0; i <= sectors; ++i)
        {
            const qreal a = qDegreesToRadians(startDeg + i * step);
            const QPointF tick(center.x() + std::cos(a) * radius,
                               center.y() - std::sin(a) * radius);
            p.setPen(QPen(core, qMax<qreal>(0.9, radius * 0.012),
                          Qt::SolidLine, Qt::RoundCap));
            p.drawLine(center, tick);
        }
        p.setPen(QPen(core, qMax<qreal>(1.2, radius * 0.020),
                      Qt::SolidLine, Qt::RoundCap));
        p.drawArc(disc, int(std::lround(startDeg * 16.0)),
                  int(std::lround(spanDeg * 16.0)));
        p.setPen(QPen(core, qMax<qreal>(0.9, radius * 0.012)));
        p.setBrush(paper.darker(145));
        p.drawEllipse(center, radius * 0.055, radius * 0.055);

        /* tassel */
        p.setPen(QPen(core, qMax<qreal>(0.9, radius * 0.010)));
        p.drawLine(center + QPointF(0, radius * 0.05),
                   center + QPointF(0, radius * 0.30));
        QPainterPath drop;
        drop.moveTo(center + QPointF(0, radius * 0.28));
        drop.lineTo(center + QPointF(-radius * 0.035, radius * 0.34));
        drop.lineTo(center + QPointF(0, radius * 0.42));
        drop.lineTo(center + QPointF(radius * 0.035, radius * 0.34));
        drop.closeSubpath();
        p.setPen(QPen(core, qMax<qreal>(0.7, radius * 0.008)));
        p.setBrush(paper.darker(135));
        p.drawPath(drop);
    });
}

inline SceneProp makePaperRibbon(const QSize &stage, const QColor &paper,
                                 const QColor &core)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    return renderProp(stage, QRectF(0, 0, w, h), [&](QPainter &p) {
        QPainterPath upper;
        upper.moveTo(-w * 0.05, h * 0.33);
        upper.cubicTo(w * 0.18, h * 0.12, w * 0.36, h * 0.48,
                      w * 0.54, h * 0.40);
        upper.cubicTo(w * 0.72, h * 0.32, w * 0.82, h * 0.08,
                      w * 1.05, h * 0.24);

        QPainterPath lower;
        lower.moveTo(-w * 0.05, h * 0.47);
        lower.cubicTo(w * 0.20, h * 0.63, w * 0.40, h * 0.31,
                      w * 0.60, h * 0.45);
        lower.cubicTo(w * 0.78, h * 0.58, w * 0.92, h * 0.51,
                      w * 1.05, h * 0.63);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(paper.darker(150), h * 0.033, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(upper);
        p.setPen(QPen(paper, h * 0.026, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPath(upper);
        p.setPen(QPen(core, h * 0.005, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPath(upper);

        p.setPen(QPen(paper.darker(160), h * 0.023, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(lower);
        p.setPen(QPen(paper.darker(112), h * 0.018, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(lower);

        /* paper twist where the two ribbons cross */
        p.save();
        p.translate(w * 0.555, h * 0.425);
        p.rotate(-26.0);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0018)));
        p.setBrush(paper.darker(128));
        p.drawRoundedRect(QRectF(-w * 0.030, -h * 0.012, w * 0.060,
                                 h * 0.024),
                          h * 0.006, h * 0.006);
        p.restore();
    });
}

inline SceneProp makePaperFloral(const QSize &stage, quint32 seed,
                                 const QList<QColor> &palette,
                                 const QPointF &origin, qreal sizeScale,
                                 bool mirror)
{
    if (palette.isEmpty() || sizeScale <= 1.0)
        return {};
    const qreal dir = mirror ? -1.0 : 1.0;
    SceneRng rng(seed);
    const QRectF bounds(origin.x() - sizeScale * 2.45,
                        origin.y() - sizeScale * 3.05, sizeScale * 4.90,
                        sizeScale * 3.25);
    const QColor stem = palette.value(qMin(2, palette.size() - 1)).darker(138);
    const QColor leaf = palette.value(qMin(2, palette.size() - 1));
    return renderProp(stage, bounds, [&](QPainter &p) {
        p.setRenderHint(QPainter::Antialiasing, true);
        struct Stem
        {
            QPointF tip;
            qreal bend;
        };
        QVector<Stem> stems;
        for (int i = 0; i < 3; ++i)
        {
            const qreal spread = sizeScale * (0.55 + 0.55 * i);
            const qreal lift = sizeScale * (2.25 - 0.35 * i);
            const QPointF tip(origin.x() + dir * spread
                                  + rng.range(-sizeScale * 0.08f,
                                              sizeScale * 0.08f),
                              origin.y() - lift);
            QPainterPath path;
            path.moveTo(origin + QPointF(dir * sizeScale * 0.10 * i, 0));
            const QPointF control(origin.x() + dir * spread * 0.55
                                      + rng.range(-sizeScale * 0.15f,
                                                  sizeScale * 0.15f),
                                  origin.y() - lift * 0.55);
            path.quadTo(control, tip);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(stem, qMax<qreal>(1.2, sizeScale * 0.055),
                          Qt::SolidLine, Qt::RoundCap));
            p.drawPath(path);
            stems.append({ tip, spread });
        }

        const auto drawLeaf = [&](const QPointF &at, qreal len, qreal angle) {
            p.save();
            p.translate(at);
            p.rotate(angle);
            QPainterPath leafPath;
            leafPath.moveTo(0, 0);
            leafPath.quadTo(len * 0.55, -len * 0.36, len, 0);
            leafPath.quadTo(len * 0.55, len * 0.36, 0, 0);
            p.setPen(QPen(stem.lighter(118), qMax<qreal>(0.7,
                                                         len * 0.035)));
            p.setBrush(leaf);
            p.drawPath(leafPath);
            p.restore();
        };
        for (int i = 0; i < stems.size(); ++i)
        {
            const Stem &s = stems.at(i);
            for (int j = 0; j < 3; ++j)
            {
                const qreal t = 0.28 + 0.22 * j;
                const QPointF at(origin.x() + dir * s.bend * t
                                      + (j % 2 == 0 ? -1 : 1)
                                            * sizeScale * 0.16,
                                  origin.y() - (origin.y() - s.tip.y()) * t);
                drawLeaf(at, sizeScale * rng.range(0.42f, 0.62f),
                         dir * rng.range(-52.0f, 52.0f));
            }
        }

        const auto drawFlower = [&](const QPointF &at, qreal radius,
                                    const QColor &petal,
                                    const QColor &heart) {
            const int petals = 6;
            for (int i = 0; i < petals; ++i)
            {
                const qreal a = kTau * qreal(i) / qreal(petals)
                                + rng.range(-0.10f, 0.10f);
                p.save();
                p.translate(at);
                p.rotate(qRadiansToDegrees(a));
                QPainterPath petalPath;
                const qreal len = radius * (0.92 + rng.range(-0.10f, 0.16f));
                petalPath.addEllipse(QPointF(radius * 0.55, 0.0),
                                     len * 0.60, radius * 0.30);
                p.setPen(QPen(heart.lighter(118), qMax<qreal>(0.7,
                                                               radius * 0.035)));
                p.setBrush(petal);
                p.drawPath(petalPath);
                p.restore();
            }
            p.setPen(QPen(heart, qMax<qreal>(0.8, radius * 0.045)));
            p.setBrush(heart);
            p.drawEllipse(at, radius * 0.24, radius * 0.24);
        };
        for (int i = 0; i < stems.size(); ++i)
        {
            const QColor petal = palette.at(
                (i + 1) % qMax(1, palette.size()));
            const QColor heart = palette.at(
                (i + 2) % qMax(1, palette.size()));
            drawFlower(stems.at(i).tip,
                       sizeScale * (0.46 + 0.10 * (i % 2)), petal, heart);
        }
        /* one low bud between the stems */
        drawFlower(origin + QPointF(dir * sizeScale * 0.20,
                                    -sizeScale * 0.95),
                   sizeScale * 0.28, palette.at(0),
                   palette.value(qMin(1, palette.size() - 1)));
    });
}

inline SceneProp makePaperButterflies(const QSize &stage, quint32 seed,
                                      const QList<QColor> &palette,
                                      const QList<QPointF> &anchors, int count,
                                      qreal wingSize)
{
    if (palette.isEmpty() || anchors.isEmpty() || count <= 0)
        return {};
    SceneRng rng(seed);
    qreal minX = stage.width();
    qreal minY = stage.height();
    qreal maxX = 0.0;
    qreal maxY = 0.0;
    for (const QPointF &anchor : anchors)
    {
        minX = qMin(minX, anchor.x() - wingSize * 4.0);
        minY = qMin(minY, anchor.y() - wingSize * 4.0);
        maxX = qMax(maxX, anchor.x() + wingSize * 4.0);
        maxY = qMax(maxY, anchor.y() + wingSize * 4.0);
    }
    const QRectF bounds(minX, minY, maxX - minX, maxY - minY);
    return renderProp(stage, bounds, [&](QPainter &p) {
        for (int i = 0; i < count; ++i)
        {
            const QPointF anchor = anchors.at(i % anchors.size());
            const QPointF c(
                anchor.x() + rng.range(float(-wingSize * 2.2f),
                                       float(wingSize * 2.2f)),
                anchor.y() + rng.range(float(-wingSize * 2.2f),
                                       float(wingSize * 2.2f)));
            const qreal s = wingSize * rng.range(0.78f, 1.18f);
            const QColor base = palette.at(rng.pick(palette.size()));
            QColor paper = base;
            paper.setAlpha(236);
            QColor edge = base.lighter(138);
            edge.setAlpha(245);
            p.save();
            p.translate(c);
            p.rotate(rng.range(-32.0f, 32.0f));
            p.setPen(QPen(edge, qMax<qreal>(0.8, s * 0.035)));
            p.setBrush(paper);
            QPainterPath upperL;
            upperL.addEllipse(QPointF(-s * 0.52, -s * 0.34), s * 0.62,
                              s * 0.46);
            QPainterPath upperR;
            upperR.addEllipse(QPointF(s * 0.52, -s * 0.34), s * 0.62,
                              s * 0.46);
            QPainterPath lowerL;
            lowerL.addEllipse(QPointF(-s * 0.40, s * 0.34), s * 0.46,
                              s * 0.34);
            QPainterPath lowerR;
            lowerR.addEllipse(QPointF(s * 0.40, s * 0.34), s * 0.46,
                              s * 0.34);
            p.drawPath(upperL);
            p.drawPath(upperR);
            p.drawPath(lowerL);
            p.drawPath(lowerR);
            p.setPen(Qt::NoPen);
            QColor inner = edge.lighter(112);
            inner.setAlpha(150);
            p.setBrush(inner);
            p.drawEllipse(QPointF(-s * 0.52, -s * 0.30), s * 0.26,
                          s * 0.18);
            p.drawEllipse(QPointF(s * 0.52, -s * 0.30), s * 0.26, s * 0.18);
            p.setBrush(paper.darker(165));
            p.setPen(QPen(edge, qMax<qreal>(0.6, s * 0.02)));
            p.drawRoundedRect(QRectF(-s * 0.075, -s * 0.62, s * 0.15,
                                     s * 1.20),
                              s * 0.07, s * 0.07);
            p.setPen(QPen(edge, qMax<qreal>(0.6, s * 0.02),
                          Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(0, -s * 0.58),
                       QPointF(-s * 0.28, -s * 0.94));
            p.drawLine(QPointF(0, -s * 0.58),
                       QPointF(s * 0.28, -s * 0.94));
            p.restore();
        }
    });
}

inline SceneProp makePaperStarGarland(const QSize &stage, quint32 seed,
                                      const QColor &paper, const QColor &core,
                                      int count, qreal y, qreal sag)
{
    if (count <= 0)
        return {};
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal radius = qMax<qreal>(3.0, h * 0.018);
    SceneRng rng(seed);
    return renderProp(
        stage,
        QRectF(0, y - radius * 2.4, w, sag + radius * 5.0),
        [&](QPainter &p) {
        const qreal margin = w * 0.06;
        QPainterPath string;
        const int steps = 40;
        for (int i = 0; i <= steps; ++i)
        {
            const qreal t = qreal(i) / qreal(steps);
            const qreal x = margin + (w - margin * 2.0) * t;
            const qreal yy = y + std::sin(t * 3.141592653589793) * sag;
            if (i == 0)
                string.moveTo(x, yy);
            else
                string.lineTo(x, yy);
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(0.8, h * 0.0018)));
        p.drawPath(string);

        for (int i = 0; i < count; ++i)
        {
            const qreal t = (qreal(i) + 0.5) / qreal(count);
            const qreal x = margin + (w - margin * 2.0) * t;
            const qreal yy = y + std::sin(t * 3.141592653589793) * sag;
            const qreal r = radius * rng.range(0.70f, 1.10f);
            const QPointF c(x, yy + r * 1.35);
            p.setPen(QPen(core, qMax<qreal>(0.8, r * 0.10)));
            p.setBrush(paper);
            p.drawPath(fivePointStar(c, r, 0.43,
                                     rng.range(-16.0f, 16.0f)));
            p.setPen(QPen(core, qMax<qreal>(0.6, r * 0.06)));
            p.drawLine(QPointF(x, yy), QPointF(x, yy + r * 0.55));
        }
    });
}

inline SceneProp makePaperMoon(const QSize &stage, quint32 seed,
                               const QColor &paper, const QColor &core,
                               const QPointF &center, qreal radius)
{
    SceneRng rng(seed);
    return renderProp(stage,
                      QRectF(center.x() - radius * 1.25,
                             center.y() - radius * 1.25, radius * 2.5,
                             radius * 2.5),
                      [&](QPainter &p) {
        QRadialGradient disc(
            QPointF(center.x() - radius * 0.28,
                    center.y() - radius * 0.30),
            radius * 1.35);
        disc.setColorAt(0.0, paper.lighter(112));
        disc.setColorAt(0.62, paper);
        disc.setColorAt(1.0, paper.darker(114));
        p.setPen(QPen(core, qMax<qreal>(1.0, radius * 0.025)));
        p.setBrush(disc);
        p.drawEllipse(center, radius, radius);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(paper.darker(122), qMax<qreal>(0.8, radius * 0.012)));
        p.drawEllipse(center, radius * 0.82, radius * 0.82);
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 7; ++i)
        {
            const qreal a = rng.range(0.0f, float(kTau));
            const qreal d = radius * rng.range(0.06f, 0.68f);
            const qreal r = radius * rng.range(0.03f, 0.10f);
            QColor crater = paper.darker(112);
            crater.setAlpha(int(rng.range(32.0f, 76.0f)));
            p.setBrush(crater);
            p.drawEllipse(QPointF(center.x() + std::cos(a) * d,
                                  center.y() + std::sin(a) * d),
                          r, r * 0.86);
        }
    });
}

inline SceneProp makePaperForegroundLip(const QSize &stage, quint32 seed,
                                        const QColor &paper,
                                        const QColor &core, qreal topY)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    SceneRng rng(seed);
    return renderProp(stage,
                      QRectF(0, topY - h * 0.04, w, h - topY + h * 0.04),
                      [&](QPainter &p) {
        QVector<QPointF> edge;
        const int steps = 44;
        for (int i = 0; i <= steps; ++i)
        {
            const qreal t = qreal(i) / qreal(steps);
            const qreal x = w * t;
            const qreal y = topY + std::sin(t * 3.2 * kTau + 0.8) * h * 0.008
                            + rng.range(float(-h * 0.003f),
                                        float(h * 0.003f));
            edge.append(QPointF(x, y));
        }
        QPainterPath path;
        path.moveTo(0, h);
        path.lineTo(edge.first());
        for (int i = 1; i < edge.size(); ++i)
            path.lineTo(edge[i]);
        path.lineTo(w, h);
        path.closeSubpath();

        QLinearGradient fill(0, topY, 0, h);
        fill.setColorAt(0.0, paper.lighter(104));
        fill.setColorAt(0.25, paper);
        fill.setColorAt(1.0, paper.darker(158));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(path);

        QPainterPath rim;
        rim.moveTo(edge.first());
        for (int i = 1; i < edge.size(); ++i)
            rim.lineTo(edge[i]);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0030), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(rim);

        /* paper grass / board seams */
        p.setPen(Qt::NoPen);
        p.setBrush(paper.darker(128));
        for (int i = 0; i < 26; ++i)
        {
            const qreal t = (qreal(i) + rng.range(0.1f, 0.9f)) / 26.0;
            const int idx = qBound(0, int(t * (edge.size() - 1)),
                                   edge.size() - 1);
            const QPointF at = edge.at(idx);
            const qreal bh = h * rng.range(0.008f, 0.030f);
            const qreal bw = w * rng.range(0.0015f, 0.0035f);
            QPainterPath blade;
            blade.moveTo(at.x() - bw, at.y());
            blade.lineTo(at.x() + rng.range(-bw, bw), at.y() - bh);
            blade.lineTo(at.x() + bw, at.y());
            blade.closeSubpath();
            p.drawPath(blade);
        }
        p.setPen(QPen(paper.darker(175), qMax<qreal>(0.8, h * 0.0015)));
        p.drawLine(QPointF(0, topY + h * 0.045),
                   QPointF(w, topY + h * 0.045));
        p.setPen(QPen(paper.darker(185), qMax<qreal>(0.7, h * 0.0012)));
        p.drawLine(QPointF(0, topY + h * 0.085),
                   QPointF(w, topY + h * 0.085));
    });
}

inline SceneProp makePaperCornerFoliage(const QSize &stage, quint32 seed,
                                        const QColor &paper,
                                        const QColor &core, bool left)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal bw = w * 0.30;
    const qreal boundsW = w * 0.36;
    const qreal bh = h * 0.34;
    const QRectF bounds = left ? QRectF(0, 0, boundsW, bh)
                               : QRectF(w - boundsW, 0, boundsW, bh);
    SceneRng rng(seed);
    return renderProp(stage, bounds, [&](QPainter &p) {
        const qreal dir = left ? 1.0 : -1.0;
        const qreal ox = left ? 0.0 : w;
        const auto X = [&](qreal local) { return ox + dir * local; };
        QPainterPath branch;
        branch.moveTo(X(0), h * 0.06);
        branch.cubicTo(X(bw * 0.35), h * 0.04, X(bw * 0.62), h * 0.18,
                       X(bw * 0.94), h * 0.34);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(paper.darker(124), qMax<qreal>(1.4, h * 0.0060),
                      Qt::SolidLine, Qt::RoundCap));
        p.drawPath(branch);

        const int leaves = 11;
        for (int i = 0; i < leaves; ++i)
        {
            const qreal t = 0.12 + 0.82 * qreal(i) / qreal(leaves - 1);
            const QPointF at = branch.pointAtPercent(t);
            const qreal len = h * rng.range(0.022f, 0.046f);
            p.save();
            p.translate(at + QPointF(0, h * rng.range(-0.010f, 0.010f)));
            p.rotate(dir * rng.range(-62.0f, 58.0f));
            QPainterPath leaf;
            leaf.moveTo(0, 0);
            leaf.quadTo(len * 0.55, -len * 0.34, len, 0);
            leaf.quadTo(len * 0.55, len * 0.34, 0, 0);
            p.setPen(QPen(core, qMax<qreal>(0.6, len * 0.045)));
            p.setBrush(paper);
            p.drawPath(leaf);
            p.restore();
        }
    });
}

inline SceneProp makePaperProscenium(const QSize &stage, const QColor &paper,
                                     const QColor &core)
{
    const qreal w = stage.width();
    const qreal h = stage.height();
    const qreal t = w * 0.060;
    return renderProp(stage, QRectF(0, 0, w, h), [&](QPainter &p) {
        QPainterPath frame;
        frame.moveTo(0, h);
        frame.lineTo(0, h * 0.34);
        frame.cubicTo(0, h * 0.10, w * 0.22, h * 0.03, w * 0.5, h * 0.03);
        frame.cubicTo(w * 0.78, h * 0.03, w, h * 0.10, w, h * 0.34);
        frame.lineTo(w, h);
        frame.lineTo(w - t, h);
        frame.lineTo(w - t, h * 0.35);
        frame.cubicTo(w - t, h * 0.15, w * 0.74, h * 0.09, w * 0.5,
                      h * 0.09);
        frame.cubicTo(w * 0.26, h * 0.09, t, h * 0.15, t, h * 0.35);
        frame.lineTo(t, h);
        frame.closeSubpath();

        QLinearGradient fill(0, 0, 0, h);
        fill.setColorAt(0.0, paper.darker(112));
        fill.setColorAt(0.55, paper);
        fill.setColorAt(1.0, paper.darker(140));
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(frame);

        QPainterPath inner;
        inner.moveTo(w - t, h * 0.35);
        inner.cubicTo(w - t, h * 0.15, w * 0.74, h * 0.09, w * 0.5,
                      h * 0.09);
        inner.cubicTo(w * 0.26, h * 0.09, t, h * 0.15, t, h * 0.35);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.2, h * 0.0030), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(inner);

        /* scalloped valance */
        const qreal vh = h * 0.085;
        const int scallops = 9;
        QPainterPath valance;
        valance.moveTo(0, 0);
        valance.lineTo(w, 0);
        valance.lineTo(w, vh);
        for (int i = scallops; i > 0; --i)
        {
            const qreal x0 = w * qreal(i) / qreal(scallops);
            const qreal x1 = w * qreal(i - 1) / qreal(scallops);
            valance.quadTo((x0 + x1) / 2.0, vh + h * 0.018, x1, vh);
        }
        valance.closeSubpath();
        QLinearGradient vfill(0, 0, 0, vh + h * 0.02);
        vfill.setColorAt(0.0, paper.darker(128));
        vfill.setColorAt(1.0, paper.darker(106));
        p.setPen(Qt::NoPen);
        p.setBrush(vfill);
        p.drawPath(valance);

        QPainterPath vedge;
        vedge.moveTo(0, vh);
        for (int i = scallops; i > 0; --i)
        {
            const qreal x0 = w * qreal(i) / qreal(scallops);
            const qreal x1 = w * qreal(i - 1) / qreal(scallops);
            vedge.quadTo((x0 + x1) / 2.0, vh + h * 0.018, x1, vh);
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(core, qMax<qreal>(1.0, h * 0.0026), Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(vedge);
    });
}

/* Pure-Qt equivalent of master's art::typography condensed.impact title:
 * Bebas/Oswald/Anton/Impact fallback order, black weight, uppercase, 0.04 em
 * tracking. Kept local so the recipe harness needs no extra translation unit. */
inline QImage renderPaperTitleText(const QString &text, const QColor &color,
                                   qreal pixelSize)
{
    QFont font;
    font.setFamilies({ QStringLiteral("Bebas Neue"), QStringLiteral("Oswald"),
                       QStringLiteral("Anton"), QStringLiteral("Impact"),
                       QStringLiteral("Arial Narrow") });
    font.setStyleHint(QFont::SansSerif);
    font.setWeight(QFont::Black);
    font.setCapitalization(QFont::AllUppercase);
    font.setPixelSize(qMax(1, int(std::lround(pixelSize))));
    font.setLetterSpacing(QFont::AbsoluteSpacing,
                          qMax<qreal>(1.0, pixelSize * 0.04));
    const QFontMetricsF fm(font);
    const QRectF tight = fm.tightBoundingRect(text);
    const qreal pad = qMax<qreal>(2.0, pixelSize * 0.05);
    const QSize size(qMax(1, int(std::ceil(tight.width() + pad * 2.0))),
                     qMax(1, int(std::ceil(tight.height() + pad * 2.0))));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setFont(font);
    p.setPen(color);
    p.drawText(QPointF(pad - tight.left(), pad - tight.top()), text);
    p.end();
    return image;
}

inline SceneProp makePaperTitle(const QSize &stage, const QString &text,
                                const QColor &color, const QColor &shadowColor,
                                qreal heightFraction, qreal y)
{
    if (text.trimmed().isEmpty() || !stage.isValid() || stage.isEmpty())
        return {};
    qreal px = stage.height() * heightFraction;
    qreal usedPx = px;
    QImage textImage;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        usedPx = px;
        textImage = renderPaperTitleText(text, color, usedPx);
        if (textImage.isNull()
            || textImage.width() <= int(stage.width() * 0.90))
            break;
        px *= 0.84;
    }
    if (textImage.isNull())
        return {};
    QColor shadow(shadowColor.red(), shadowColor.green(), shadowColor.blue(),
                  150);
    QImage shadowImage = renderPaperTitleText(text, shadow, usedPx);

    const qreal maxWidth = stage.width() * 0.92;
    if (textImage.width() > maxWidth)
    {
        const int target = qMax(1, int(maxWidth));
        textImage = textImage.scaledToWidth(target, Qt::SmoothTransformation);
        if (!shadowImage.isNull())
            shadowImage =
                shadowImage.scaledToWidth(target, Qt::SmoothTransformation);
    }
    const qreal x = (stage.width() - textImage.width()) / 2.0;
    const qreal shadowOffset = stage.height() * 0.0045;
    return renderProp(
        stage,
        QRectF(x - stage.width() * 0.015, y - shadowOffset,
               textImage.width() + stage.width() * 0.03,
               textImage.height() + shadowOffset * 2.5),
        [&](QPainter &p) {
        if (!shadowImage.isNull())
            p.drawImage(QPointF(x, y + shadowOffset), shadowImage);
        p.drawImage(QPointF(x, y), textImage);
    });
}

/* Master's per-look stage light colour, kept pure so the recipe never reaches
 * into PaintbrushCreative. */
inline QColor stageLightColor(const QString &patchId)
{
    if (patchId == QLatin1String("neon-idol")
        || patchId == QLatin1String("electric-kpop")
        || patchId == QLatin1String("kaleido")
        || patchId == QLatin1String("motion-echo"))
        return QColor(130, 232, 255);
    if (patchId == QLatin1String("sticker-forge"))
        return QColor(255, 205, 90);
    if (patchId == QLatin1String("pastel-stage"))
        return QColor(180, 150, 255);
    if (patchId == QLatin1String("scene-forge")
        || patchId == QLatin1String("stage-mode")
        || patchId == QLatin1String("paper-theatre"))
        return QColor(255, 236, 196);
    if (patchId == QLatin1String("nocturne")
        || patchId == QLatin1String("holo-fashion"))
        return QColor(205, 160, 255);
    if (patchId == QLatin1String("holo-liquid")
        || patchId == QLatin1String("holo-opal"))
        return QColor(205, 160, 255);
    if (patchId == QLatin1String("holo-chrome"))
        return QColor(232, 246, 255);
    if (patchId == QLatin1String("holo-prism"))
        return QColor(255, 61, 218);
    if (patchId == QLatin1String("holo-wrap"))
        return QColor(0, 140, 255);
    if (patchId == QLatin1String("holo-chameleon"))
        return QColor(0, 195, 135);
    if (patchId == QLatin1String("holo-glitter"))
        return QColor(255, 224, 130);
    if (patchId == QLatin1String("holo-livery"))
        return QColor(0, 234, 255);
    if (patchId == QLatin1String("ink-creature"))
        return QColor(255, 230, 180);
    if (patchId == QLatin1String("auto-splatter"))
        return QColor(255, 194, 164);
    if (patchId == QLatin1String("spark-storm"))
        return QColor(255, 216, 142);
    return QColor(255, 244, 230);
}

} /* namespace detail */

/* ----------------------------------------------------------- stage policy */

/* Interactive composition size. The Play Pad recomposes on every pad move and
 * on its 20 Hz idle tick; at a 4K preview a full composite costs ~75 ms, which
 * cannot keep up. Motion frames therefore compose at a <=1 MP interactive
 * target, and the pad settles back to the full `previewStage` still after the
 * gesture ends. The pieces themselves are still built at full preview
 * resolution, so this is a display cost, not a generation downgrade. */
inline QSize interactivePreviewStage(const QSize &preview)
{
    if (!preview.isValid() || preview.isEmpty())
        return {};
    constexpr qint64 kMaxInteractivePixels = qint64(1280) * 720;
    const qint64 pixels =
        qint64(preview.width()) * qint64(preview.height());
    const qreal fit = pixels > kMaxInteractivePixels
                          ? std::sqrt(double(kMaxInteractivePixels)
                                      / double(pixels))
                          : 1.0;
    return QSize(qMax(1, int(std::lround(preview.width() * fit))),
                 qMax(1, int(std::lround(preview.height() * fit))));
}

/* Preview stage size for a canvas: native up to a 4K frame budget
 * (3840x2160 = 8.3 MP), uniformly scaled only when the canvas exceeds it.
 *
 * This is deliberately not the old 560x680 thumbnail fit. The Play Pad shows
 * the stage at panel size, but the pixels behind it are now real preview
 * detail, so the vector fields and materials stay legible while the user
 * drags. Commits still render at the full native canvas resolution. */
inline QSize previewStage(const QSize &canvas)
{
    if (!canvas.isValid() || canvas.isEmpty())
        return {};
    constexpr qint64 kMaxPreviewPixels = qint64(3840) * 2160;
    const qint64 pixels = qint64(canvas.width()) * qint64(canvas.height());
    const qreal fit = pixels > kMaxPreviewPixels
                          ? std::sqrt(double(kMaxPreviewPixels) / double(pixels))
                          : 1.0;
    return QSize(qMax(1, int(std::lround(canvas.width() * fit))),
                 qMax(1, int(std::lround(canvas.height() * fit))));
}

/* Rough upper bound used only for progress reporting; the recipes themselves
 * decide how many pieces they emit. */
inline int expectedPieceCount(const QString &patchId)
{
    if (patchId == QLatin1String("neon-idol")
        || patchId == QLatin1String("electric-kpop")
        || patchId == QLatin1String("sticker-forge"))
        return 12;
    if (patchId == QLatin1String("kaleido")
        || patchId == QLatin1String("motion-echo"))
        return 10;
    if (detail::isHoloMaterialPatch(patchId))
        return 11;
    return 10;
}

/* Full-resolution direct renderer (v1) memory bound.
 *
 * Commits build at the native canvas resolution, never at the legacy 1600 px
 * cap and never by upscaling a smaller build. Each recipe keeps several
 * full-stage ARGB32 pieces alive at once, so the direct path is bounded:
 * above the budget below the engine refuses the commit with an explicit
 * message instead of silently lowering the resolution.
 *
 * TILING GAP (documented, not approximated): a tiled renderer must evaluate
 * every stage-global primitive (gradients, procedural fields, blurs/bloom
 * halos, arcs, kaleidoscope wedges) in global coordinates and give each tile
 * a sufficient overlap halo so blur/bloom kernels are not clipped at tile
 * borders. That is deliberately not attempted here; the direct path keeps
 * global coordinates and one seed, so its output is deterministic. */
inline QString fullResolutionBudgetError(const QSize &canvas)
{
    if (!canvas.isValid() || canvas.isEmpty())
        return QStringLiteral("Open a canvas first.");
    constexpr qint64 kMaxNativePixels = qint64(64) * 1024 * 1024;
    constexpr qint64 kBudgetBytes = qint64(2560) * 1024 * 1024;
    constexpr qint64 kAssumedStageImages = 16;
    const qint64 pixels = qint64(canvas.width()) * qint64(canvas.height());
    const qint64 estimate = pixels * 4 * kAssumedStageImages;
    if (pixels > kMaxNativePixels || estimate > kBudgetBytes)
    {
        return QStringLiteral(
                   "This canvas is %1x%2 (%3 MP). A full-resolution direct "
                   "render needs about %4 GB and the bounded renderer refuses "
                   "rather than dropping resolution or upscaling a smaller "
                   "build; tiled rendering is the documented next step.")
            .arg(canvas.width())
            .arg(canvas.height())
            .arg(double(pixels) / 1000000.0, 0, 'f', 1)
            .arg(double(estimate) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
    return {};
}

/* --------------------------------------------------------------- builder */

/* Build the patch pieces. Pure: the same input and seed produce byte-identical
 * pixels on any thread. `progress` (optional) is invoked after every appended
 * piece with the pieces so far; returning false cancels the build at the next
 * checked boundary. */
inline QVector<CreativePiece> build(
    const CreativeBuildInput &input,
    const std::function<bool(const QVector<CreativePiece> &, int)> &progress = {})
{
    QVector<CreativePiece> pieces;
    const QString &patchId = input.patchId;
    if (input.subject.isNull() || !input.stage.isValid()
        || input.stage.isEmpty())
        return pieces;

    QImage subject = input.subject;
    const QRectF subjectRect = input.subjectRect;
    const QSize stage = input.stage;
    const quint32 seed = input.seed;

    /* One evaluation of the N-dimensional modulation graph per build. The
     * recipes below read named destinations from `vec`; no recipe invents its
     * own pad mapping and every mode shares the same engine. */
    const creative::vector::State vec = creative::vector::evaluateForPatch(
        patchId,
        creative::vector::Sources{ input.frame.x, input.frame.y,
                                   input.frame.energy,
                                   input.frame.hold ? 1.0f : 0.0f,
                                   input.frame.time });

    /* Preview builds reserve one dynamic piece for the live graph evaluation;
     * commits bake the same field at the final frame instead. */
    const bool preview = input.preview;

    bool cancelled = false;
    auto pieceAdded = [&]() -> bool {
        if (cancelled)
            return false;
        if (!progress)
            return true;
        if (!progress(pieces, pieces.size()))
        {
            cancelled = true;
            return false;
        }
        return true;
    };

    QImage s = subject;
    if (s.size() != subjectRect.size().toSize())
        s = s.scaled(subjectRect.size().toSize(), Qt::IgnoreAspectRatio,
                     Qt::SmoothTransformation);

    const qreal W = stage.width();
    const qreal H = stage.height();
    const QImage subjMask = creative::maskFromAlpha(s);

    struct Add
    {
        QVector<CreativePiece> *out;
        QPointF pivot;
        const std::function<bool(const QVector<CreativePiece> &, int)> *progress;
        bool *cancelled;
        void operator()(const QString &name, const QString &role,
                        const QImage &image, const QRectF &rect,
                        int blend = BLEND_NORMAL, float opacity = 1.0f,
                        const CreativeMotion &motion = CreativeMotion()) const
        {
            if (*cancelled)
                return;
            CreativePiece p;
            p.name = name;
            p.role = role;
            p.image = image;
            p.rect = rect;
            p.pivot = pivot;
            p.blendMode = blend;
            p.opacity = opacity;
            p.motion = motion;
            out->append(p);
            if (progress && *progress
                && !(*progress)(*out, out->size()))
                *cancelled = true;
        }
    } add{ &pieces, subjectRect.center(), &progress, &cancelled };

    const QRectF full(QPointF(0, 0), QSizeF(stage));

    auto drawAt = [](QImage &dst, const QImage &src, const QRectF &r) {
        detail::drawScaled(dst, src, r);
    };
    auto band = [&](qreal fraction, const QColor &color, const QString &name,
                    const QString &role, float opacity, int blend,
                    const CreativeMotion &motion) {
        if (cancelled)
            return;
        const int radius = qMax(1, int(std::lround(subjectRect.height() * fraction)));
        QImage bandMask = creative::bandFromMask(subjMask, radius);
        QImage fullBand = detail::stageCanvas(stage);
        drawAt(fullBand, creative::colorizeBand(bandMask, color, false, seed),
               subjectRect);
        CreativePiece p;
        p.name = name;
        p.role = role;
        p.image = fullBand;
        p.rect = full;
        p.pivot = subjectRect.center();
        p.blendMode = blend;
        p.opacity = opacity;
        p.motion = motion;
        pieces.append(p);
        pieceAdded();
    };
    auto subjectFull = [&](const QImage &graded, const QString &name,
                           float opacity, const CreativeMotion &motion) {
        if (cancelled)
            return;
        QImage fullSubj = detail::stageCanvas(stage);
        drawAt(fullSubj, graded, subjectRect);
        CreativePiece p;
        p.name = name;
        p.role = QStringLiteral("subject");
        p.image = fullSubj;
        p.rect = full;
        p.pivot = subjectRect.center();
        p.blendMode = BLEND_NORMAL;
        p.opacity = opacity;
        p.motion = motion;
        pieces.append(p);
        pieceAdded();
    };
    auto solid = [&](const QImage &src, const QColor &color) {
        return detail::tintPreserveAlpha(src, color);
    };

    if (patchId == QLatin1String("neon-idol"))
    {
        const QColor ink(4, 5, 16), cobalt(40, 90, 255), cyan(90, 235, 255),
            rose(255, 80, 160), coral(255, 110, 80), cream(255, 244, 248);
        add(QStringLiteral("Neon wash"), QStringLiteral("backdrop"),
            creative::gradientField(stage, ink, QColor(30, 10, 54),
                                    QColor(80, 45, 170, 130), 0.34),
            full);
        CreativeMotion dotsA;
        dotsA.opacityX = 0.22f;
        add(QStringLiteral("Neon halftone rose"), QStringLiteral("backdrop"),
            creative::halftoneField(stage, QColor(255, 110, 190, 85),
                                    H * 0.022, H * 0.0065, -18, 0.0),
            full, BLEND_SCREEN, 0.55f, dotsA);
        CreativeMotion dotsB;
        dotsB.opacityY = 0.18f;
        add(QStringLiteral("Neon halftone ice"), QStringLiteral("backdrop"),
            creative::halftoneField(stage, QColor(90, 210, 255, 75),
                                    H * 0.027, H * 0.006, 22, 0.5),
            full, BLEND_SCREEN, 0.40f, dotsB);
        if (cancelled)
            return pieces;

        QImage echo = creative::gradeImage(creative::softBlur(s, 7),
                                           creative::Grade{ 1.06f, 1.22f, 0.22f,
                                                            rose, 0.55f, cyan, 0.38f });
        QImage echoFull = detail::stageCanvas(stage);
        drawAt(echoFull, echo,
               detail::scaleRectAbout(subjectRect, 1.72, -W * 0.055, -H * 0.035));
        CreativeMotion echoM;
        echoM.opacityX = 0.16f;
        echoM.dxX = 0.03f;
        add(QStringLiteral("Neon echo"), QStringLiteral("echo"), echoFull, full,
            BLEND_NORMAL, 0.24f, echoM);

        CreativeMotion bloomM;
        bloomM.opacityY = 0.16f;
        bloomM.opacityHold = 0.22f;
        bloomM.scaleHold = 0.06f;
        add(QStringLiteral("Neon bloom"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 qMax(subjectRect.width(), subjectRect.height()) * 0.66,
                                 cobalt, 0.42f),
            full, BLEND_SCREEN, 0.62f, bloomM);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.05f, 1.12f, 1.20f, cobalt, 0.22f, coral, 0.10f });
        subjectFull(graded, QStringLiteral("Neon idol"), 1.0f, CreativeMotion());
        if (cancelled)
            return pieces;

        CreativeMotion rimCyanM;
        rimCyanM.opacityX = -0.34f;
        rimCyanM.opacityY = 0.16f;
        rimCyanM.opacityHold = 0.18f;
        band(0.012, cyan, QStringLiteral("Neon rim ice"),
             QStringLiteral("rim"), 0.85f, BLEND_SCREEN, rimCyanM);
        CreativeMotion rimRoseM;
        rimRoseM.opacityX = 0.34f;
        rimRoseM.opacityY = 0.16f;
        rimRoseM.opacityHold = 0.18f;
        band(0.012, rose, QStringLiteral("Neon rim rose"),
             QStringLiteral("rim"), 0.85f, BLEND_SCREEN, rimRoseM);

        /* split the two rims left/right so the pad reads as directional light */
        if (pieces.size() >= 2)
        {
            detail::fadeHorizontally(pieces[pieces.size() - 2].image, true);
            detail::fadeHorizontally(pieces[pieces.size() - 1].image, false);
        }

        CreativeMotion lightM;
        lightM.opacityY = 0.28f;
        add(QStringLiteral("Neon light"), QStringLiteral("finish"),
            creative::glossOverlay(stage, s, subjectRect, cream),
            full, BLEND_SCREEN, 0.30f, lightM);
        if (cancelled)
            return pieces;

        CreativeMotion sparkM;
        sparkM.opacityEnergy = 0.38f;
        sparkM.opacityHold = 0.18f;
        sparkM.scaleEnergy = 0.10f;
        sparkM.rotationEnergy = 26.0f;
        add(QStringLiteral("Neon sparks"), QStringLiteral("accent"),
            creative::bokehField(stage, seed,
                                 { cream, cyan, rose, coral }, 120,
                                 H * 0.006, H * 0.032),
            full, BLEND_SCREEN, 0.55f, sparkM);

        add(QStringLiteral("Neon scan"), QStringLiteral("finish"),
            creative::scanlineField(stage, QColor(8, 8, 20),
                                    qMax(3, int(H * 0.007)), 0.10),
            full, BLEND_OVERLAY, 0.14f);
    }

    else if (patchId == QLatin1String("electric-kpop"))
    {
        const QColor ink(8, 3, 18), cyan(0, 234, 255), rose(253, 48, 199),
            magenta(245, 0, 212), lime(183, 255, 24), white(244, 252, 255);

        /* scene: a real backdrop, not a sticker behind the cutout */
        add(QStringLiteral("Voltage wash"), QStringLiteral("backdrop"),
            creative::backdrop(stage, seed, { ink, rose, cyan }, 0.90f), full);
        CreativeMotion bloomM;
        bloomM.opacityHold = 0.20f;
        bloomM.scaleEnergy = 0.06f;
        add(QStringLiteral("Voltage bloom"), QStringLiteral("accent"),
            creative::radialGlow(
                stage, subjectRect.center(),
                qMax(subjectRect.width(), subjectRect.height()) * 0.52, rose,
                0.55f),
            full, BLEND_SCREEN, 0.62f, bloomM);
        if (cancelled)
            return pieces;

        /* persona projection: a real displaced silhouette whose offset and
         * opacity are vector destinations. It sits behind the recoloured
         * subject, so the persona reads as a second presence instead of a
         * fixed drop shadow. */
        {
            const qreal shadowDx =
                (vec[creative::vector::Dest::ShadowX] - 0.5f) * W * 0.075;
            const qreal shadowDy =
                (vec[creative::vector::Dest::ShadowY] - 0.5f) * H * 0.065;
            QImage projectionStage = detail::stageCanvas(stage);
            const QImage silhouette =
                detail::tintPreserveAlpha(s, QColor(5, 0, 12));
            drawAt(projectionStage, silhouette,
                   subjectRect.translated(shadowDx, shadowDy)
                       .adjusted(-W * 0.004, -H * 0.003, W * 0.004,
                                 H * 0.003));
            CreativeMotion projectionM;
            projectionM.opacityX = 0.30f;
            projectionM.opacityY = 0.26f;
            add(QStringLiteral("Voltage projection"),
                QStringLiteral("echo"), projectionStage, full, BLEND_NORMAL,
                clamp01(0.16f
                        + 0.60f
                              * vec[creative::vector::Dest::ShadowOpacity]),
                projectionM);
        }

        /* subject: luminance-preserving recolour, so the fabric keeps its
         * folds instead of turning into a flat gradient. Strength, hue drift
         * and texture retention are vector destinations, not constants. */
        creative::RecolorParams recolor;
        recolor.palette = { cyan, rose, magenta, lime };
        recolor.strength =
            clamp01(0.52f + 0.48f * vec[creative::vector::Dest::Recolor]);
        recolor.textureKeep =
            clamp01(0.08f + 0.24f * vec[creative::vector::Dest::Sheen]);
        recolor.skinGuard = 0.45f;
        recolor.hueDrift =
            (vec[creative::vector::Dest::HueShift] - 0.5f) * 0.65f;
        recolor.seed = seed;
        QImage voltageSubject =
            creative::recolorMasked(s, subjMask, recolor);
        CreativeMotion recolorM;
        recolorM.opacityX =
            0.06f + 0.22f * vec[creative::vector::Dest::ParticleBrightness];
        subjectFull(voltageSubject, QStringLiteral("Voltage idol"), 1.0f,
                    recolorM);
        if (cancelled)
            return pieces;

        /* light that moves across the highlights */
        QImage sheen = creative::spectralSheen(
            s, subjMask, { cyan, rose, lime, white }, 0.95f,
            62.0f + (vec[creative::vector::Dest::Sheen] - 0.5f) * 150.0f,
            float(seed % 628) / 100.0f
                + vec[creative::vector::Dest::HueShift] * 0.8f);
        QImage sheenFull = detail::stageCanvas(stage);
        drawAt(sheenFull, sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY =
            0.12f + 0.26f * vec[creative::vector::Dest::Sheen];
        sheenM.opacityHold =
            0.10f + 0.20f * vec[creative::vector::Dest::Bloom];
        add(QStringLiteral("Voltage sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN,
            0.65f + 0.30f * vec[creative::vector::Dest::Sheen], sheenM);

        CreativeMotion rimIceM;
        rimIceM.opacityX =
            -0.18f - 0.24f * vec[creative::vector::Dest::RimLeft];
        rimIceM.opacityY = 0.14f;
        band(0.009, cyan, QStringLiteral("Voltage rim ice"),
             QStringLiteral("rim"),
             0.70f + 0.28f * vec[creative::vector::Dest::Glow],
             BLEND_SCREEN, rimIceM);
        CreativeMotion rimRoseM;
        rimRoseM.opacityX =
            0.18f + 0.24f * vec[creative::vector::Dest::RimRight];
        rimRoseM.opacityY = 0.14f;
        band(0.016, rose, QStringLiteral("Voltage rim rose"),
             QStringLiteral("rim"),
             0.62f + 0.30f * vec[creative::vector::Dest::RimRight],
             BLEND_SCREEN, rimRoseM);
        /* directional light: each rim owns one side of the silhouette */
        if (pieces.size() >= 2)
        {
            detail::fadeHorizontally(pieces[pieces.size() - 2].image, true);
            detail::fadeHorizontally(pieces[pieces.size() - 1].image, false);
        }
        if (cancelled)
            return pieces;

        /* bolts land on the real silhouette, not on its bounding box */
        QImage subjectOnStage = detail::stageCanvas(stage);
        drawAt(subjectOnStage, s, subjectRect);
        const QImage stageMask = creative::maskFromAlpha(subjectOnStage);
        int leftX = stage.width(), rightX = -1;
        int leftY = int(subjectRect.center().y());
        int rightY = leftY;
        for (int y = 0; y < stageMask.height(); y += 2)
        {
            const uchar *row = stageMask.constScanLine(y);
            for (int x = 0; x < stageMask.width(); x += 2)
            {
                if (row[x] < 40)
                    continue;
                if (x < leftX) { leftX = x; leftY = y; }
                if (x > rightX) { rightX = x; rightY = y; }
            }
        }
        const QPointF targetL =
            rightX >= 0 ? QPointF(leftX + W * 0.004, leftY)
                        : QPointF(subjectRect.x()
                                      + subjectRect.width() * 0.14,
                                  subjectRect.y()
                                      + subjectRect.height() * 0.40);
        const QPointF targetR =
            rightX >= 0 ? QPointF(rightX - W * 0.004, rightY)
                        : QPointF(subjectRect.right()
                                      - subjectRect.width() * 0.10,
                                  subjectRect.y()
                                      + subjectRect.height() * 0.56);

        if (!preview)
        {
        /* The bolt field is generated from the vector graph: count, jitter,
         * glow and endpoints all move with the pad, and every arc is seeded
         * so preview, replay and commit match. */
        creative::ArcParams arc;
        arc.core = white;
        arc.glowWidth =
            qMax(5.0f, float(H)
                          * (0.008f
                             + 0.012f
                                   * vec[creative::vector::Dest::Glow]));
        arc.coreWidth = qMax(1.2f, float(H) * 0.0026f);
        arc.displacement =
            0.10f + 0.24f * vec[creative::vector::Dest::ArcJitter];
        arc.sparkCount =
            48 + int(120.0f * vec[creative::vector::Dest::ParticleCount]);

        const int boltCount = qBound(
            2,
            2 + int(std::lround(4.0f
                                    * vec[creative::vector::Dest::ArcCount]
                                + 3.0f * input.frame.energy)),
            5);
        const QPointF bend(
            (vec[creative::vector::Dest::ArcBendX] - 0.5f) * W * 0.05,
            (vec[creative::vector::Dest::ArcBendY] - 0.5f) * H * 0.05);
        const float particleHue =
            vec[creative::vector::Dest::ParticleHue] * 0.35f;
        const float particleSpeed =
            0.55f + 0.90f * vec[creative::vector::Dest::ParticleSpeed];
        CreativeMotion arcM;
        arcM.opacityHold = 0.15f;
        arcM.opacityY = 0.12f;
        for (int i = 0; i < boltCount; ++i)
        {
            const bool fromLeft = (i % 2) == 0;
            const QPointF from =
                fromLeft ? QPointF(-W * 0.02, H * (0.06 + 0.11 * i))
                         : QPointF(W * 1.02, H * (0.12 + 0.10 * i));
            const QPointF target =
                (fromLeft ? targetL : targetR)
                + bend * (0.5 + 0.15 * i);
            arc.halo = detail::hueShifted(
                (i % 3 == 1) ? rose : ((i % 3 == 2) ? magenta : cyan),
                particleHue);
            arc.core = (i % 3 == 0) ? white : QColor(255, 240, 250);
            add(QStringLiteral("Voltage bolt %1").arg(i + 1),
                QStringLiteral("accent"),
                creative::electricArc(stage, from, target,
                                      seed + 31
                                          + static_cast<quint32>(i) * 17,
                                      arc),
                full, BLEND_SCREEN,
                0.78f + 0.16f * vec[creative::vector::Dest::Glow], arcM);
        }
        if (cancelled)
            return pieces;

        /* sparks leave the real silhouette in the vector's wind direction */
        CreativeMotion sparkM;
        sparkM.opacityEnergy = 0.35f;
        sparkM.opacityHold = 0.20f;
        const QPointF sparkWind(
            (vec[creative::vector::Dest::ArcBendX] - 0.5f) * 2.2,
            (vec[creative::vector::Dest::ArcBendY] - 0.5f) * 2.2);
        const int sparkCount = qBound(
            40,
            int(60.0f + 260.0f * vec[creative::vector::Dest::ParticleCount]
                + 160.0f * input.frame.energy),
            480);
        add(QStringLiteral("Voltage sparks"), QStringLiteral("accent"),
            creative::sparkField(
                stageMask, stage, seed + 77,
                { detail::hueShifted(cyan, particleHue),
                  detail::hueShifted(rose, particleHue),
                  detail::hueShifted(lime, particleHue), white },
                sparkCount,
                0.45f
                    + 0.55f
                          * vec[creative::vector::Dest::ParticleBrightness],
                sparkWind,
                0.15f
                    + 0.75f
                          * vec[creative::vector::Dest::ParticleSpread],
                (0.55f
                 + 1.10f
                       * vec[creative::vector::Dest::ParticleLength])
                    * particleSpeed),
            full, BLEND_SCREEN,
            0.75f + 0.20f * vec[creative::vector::Dest::Glow], sparkM);
        }
        else
        {
            /* Preview-only dynamic field: bolts and sparks re-evaluate the
             * graph on every compose instead of staying baked. */
            CreativePiece live;
            live.name = QStringLiteral("Vector live");
            live.role = QStringLiteral("accent");
            live.dynamicVector = true;
            live.blendMode = BLEND_SCREEN;
            live.opacity = 0.95f;
            live.vectorOverlay.profileId = patchId;
            live.vectorOverlay.style = 1;
            live.vectorOverlay.mask = stageMask;
            live.vectorOverlay.palette = { cyan, rose, magenta, lime, white };
            live.vectorOverlay.subjectRect = subjectRect;
            live.vectorOverlay.stage = stage;
            live.vectorOverlay.seed = seed + 77;
            if (!stageMask.isNull())
                pieces.append(live);
        }

        CreativeMotion starM;
        starM.opacityHold = 0.30f;
        starM.scaleEnergy = 0.08f;
        add(QStringLiteral("Voltage stars"), QStringLiteral("accent"),
            creative::starField(stage, seed + 5, { white, lime, cyan }, 46),
            full, BLEND_SCREEN, 0.72f, starM);
    }
    else if (patchId == QLatin1String("sticker-forge"))
    {
        const QColor paper(255, 248, 236), ink(22, 20, 24), red(255, 59, 78),
            blue(43, 89, 255), yellow(255, 210, 63), mint(72, 224, 176);
        add(QStringLiteral("Sticker sheet"), QStringLiteral("backdrop"),
            creative::gradientField(stage, paper, QColor(255, 212, 208),
                                    QColor(255, 255, 255, 170), 0.18),
            full);
        add(QStringLiteral("Sticker grain"), QStringLiteral("backdrop"),
            creative::halftoneField(stage, QColor(30, 25, 35, 26),
                                    H * 0.030, H * 0.0032, 45, 0.0),
            full, BLEND_MULTIPLY, 0.8f);
        if (cancelled)
            return pieces;

        QImage card = detail::stageCanvas(stage);
        {
            QPainter p(&card);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 238));
            p.translate(subjectRect.center());
            p.rotate(-2.0);
            const QRectF r = detail::scaleRectAbout(
                QRectF(-subjectRect.width() / 2.0, -subjectRect.height() / 2.0,
                       subjectRect.width(), subjectRect.height()),
                1.06, 0, H * 0.018);
            p.drawRoundedRect(r, H * 0.03, H * 0.03);
        }
        add(QStringLiteral("Sticker backing"), QStringLiteral("backdrop"),
            card, full, BLEND_NORMAL, 0.85f);

        QImage shadowSubj = creative::softBlur(solid(s, ink), 6);
        QImage shadowFull = detail::stageCanvas(stage);
        drawAt(shadowFull, shadowSubj, subjectRect.translated(0, H * 0.012));
        add(QStringLiteral("Sticker shadow"), QStringLiteral("echo"),
            shadowFull, full, BLEND_NORMAL, 0.30f);
        if (cancelled)
            return pieces;

        CreativeMotion cutM;
        cutM.scaleY = 0.10f;
        cutM.scaleHold = 0.05f;
        band(0.020, QColor(255, 255, 255), QStringLiteral("Sticker die-cut"),
             QStringLiteral("rim"), 1.0f, BLEND_NORMAL, cutM);
        CreativeMotion colorM;
        colorM.opacityX = 0.28f;
        colorM.scaleHold = 0.12f;
        band(0.011, red, QStringLiteral("Sticker cut line"),
             QStringLiteral("rim"), 0.9f, BLEND_NORMAL, colorM);
        /* the accent band crossfades red -> blue with X */
        {
            const int radius = qMax(1, int(std::lround(subjectRect.height() * 0.011)));
            QImage bandMask = creative::bandFromMask(subjMask, radius);
            QImage blueBand = detail::stageCanvas(stage);
            drawAt(blueBand, creative::colorizeBand(bandMask, blue, false, seed + 9),
                   subjectRect);
            CreativeMotion blueM;
            blueM.opacityX = -0.28f;
            blueM.scaleHold = 0.12f;
            add(QStringLiteral("Sticker cut line ice"), QStringLiteral("rim"),
                blueBand, full, BLEND_NORMAL, 0.9f, blueM);
        }
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.07f, 1.05f, 1.18f, ink, 0.05f, yellow, 0.10f });
        subjectFull(graded, QStringLiteral("Sticker idol"), 1.0f, CreativeMotion());

        CreativeMotion glossM;
        glossM.opacityY = 0.30f;
        glossM.opacityHold = 0.15f;
        add(QStringLiteral("Sticker gloss"), QStringLiteral("finish"),
            creative::glossOverlay(stage, s, subjectRect, QColor(255, 255, 255)),
            full, BLEND_SCREEN, 0.38f, glossM);
        if (cancelled)
            return pieces;

        CreativeMotion confettiM;
        confettiM.opacityEnergy = 0.30f;
        confettiM.scaleEnergy = 0.10f;
        add(QStringLiteral("Sticker confetti"), QStringLiteral("accent"),
            creative::confettiField(stage, seed, { red, blue, yellow, mint },
                                    70, H * 0.006, H * 0.018),
            full, BLEND_NORMAL, 0.6f, confettiM);
        CreativeMotion starsM;
        starsM.opacityHold = 0.35f;
        add(QStringLiteral("Sticker sparkles"), QStringLiteral("accent"),
            creative::starField(stage, seed + 3, QColor(255, 255, 255), 55),
            full, BLEND_SCREEN, 0.4f, starsM);
    }
    else if (patchId == QLatin1String("pastel-stage"))
    {
        const QColor pink(255, 199, 230), lavender(199, 187, 255),
            mint(207, 247, 232), ink(90, 74, 107);
        add(QStringLiteral("Pastel sky"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(255, 230, 244),
                                    QColor(209, 202, 255),
                                    QColor(255, 255, 255, 190), 0.22),
            full);
        add(QStringLiteral("Pastel air"), QStringLiteral("accent"),
            creative::radialGlow(stage, QPointF(W * 0.72, H * 0.20),
                                 H * 0.52, QColor(255, 255, 255), 0.35f),
            full, BLEND_SCREEN, 0.55f);

        QImage floor = detail::stageCanvas(stage);
        {
            QPainter p(&floor);
            QLinearGradient g(0, H * 0.62, 0, H);
            g.setColorAt(0.0, QColor(255, 218, 238, 210));
            g.setColorAt(1.0, QColor(247, 168, 216, 235));
            p.fillRect(QRectF(0, H * 0.62, W, H * 0.38), g);
            QLinearGradient horizon(0, H * 0.60, 0, H * 0.66);
            horizon.setColorAt(0.0, QColor(255, 255, 255, 0));
            horizon.setColorAt(0.6, QColor(255, 255, 255, 170));
            horizon.setColorAt(1.0, QColor(255, 255, 255, 0));
            p.fillRect(QRectF(0, H * 0.595, W, H * 0.07), horizon);
        }
        add(QStringLiteral("Pastel floor"), QStringLiteral("backdrop"),
            floor, full);
        if (cancelled)
            return pieces;

        QImage mirrored = creative::reflection(s, 0.55, 0.38);
        QImage mirrorFull = detail::stageCanvas(stage);
        drawAt(mirrorFull, mirrored,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        add(QStringLiteral("Pastel reflection"), QStringLiteral("echo"),
            mirrorFull, full, BLEND_NORMAL, 0.55f);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.10f, 0.94f, 0.92f, lavender, 0.16f,
                                QColor(255, 255, 255), 0.12f });
        subjectFull(graded, QStringLiteral("Pastel idol"), 1.0f, CreativeMotion());

        CreativeMotion bloomM;
        bloomM.opacityY = 0.20f;
        bloomM.opacityHold = 0.30f;
        bloomM.scaleHold = 0.07f;
        add(QStringLiteral("Pastel bloom"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.30, QColor(255, 255, 255), 0.45f),
            full, BLEND_SCREEN, 0.5f, bloomM);
        if (cancelled)
            return pieces;

        CreativeMotion confettiM;
        confettiM.opacityEnergy = 0.30f;
        confettiM.scaleEnergy = 0.12f;
        add(QStringLiteral("Pastel confetti"), QStringLiteral("accent"),
            creative::confettiField(stage, seed, { pink, mint, lavender,
                                                   QColor(214, 236, 255) },
                                    60, H * 0.006, H * 0.017),
            full, BLEND_NORMAL, 0.55f, confettiM);
        add(QStringLiteral("Pastel grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 2, 26), full,
            BLEND_OVERLAY, 0.35f);
    }
    else if (patchId == QLatin1String("nocturne"))
    {
        const QColor night(6, 6, 14), violet(122, 77, 255),
            lilac(201, 167, 255), teal(75, 224, 208), silver(234, 230, 255);
        add(QStringLiteral("Nocturne sky"), QStringLiteral("backdrop"),
            creative::gradientField(stage, night, QColor(24, 14, 44),
                                    QColor(82, 52, 160, 110), 0.36),
            full);
        add(QStringLiteral("Nocturne stars"), QStringLiteral("backdrop"),
            creative::starField(stage, seed, silver, 220), full,
            BLEND_SCREEN, 0.75f);
        if (cancelled)
            return pieces;

        CreativeMotion auraM;
        auraM.opacityY = 0.16f;
        auraM.opacityHold = 0.28f;
        auraM.scaleHold = 0.06f;
        add(QStringLiteral("Nocturne aura"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.48, violet, 0.55f),
            full, BLEND_SCREEN, 0.6f, auraM);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 0.94f, 1.18f, 0.88f, QColor(40, 18, 90), 0.34f,
                                silver, 0.12f });
        subjectFull(graded, QStringLiteral("Nocturne figure"), 1.0f,
                    CreativeMotion());
        if (cancelled)
            return pieces;

        CreativeMotion rimVioletM;
        rimVioletM.opacityX = 0.30f;
        rimVioletM.opacityHold = 0.20f;
        band(0.010, lilac, QStringLiteral("Nocturne rim"), QStringLiteral("rim"),
             0.75f, BLEND_SCREEN, rimVioletM);
        CreativeMotion rimTealM;
        rimTealM.opacityX = -0.30f;
        band(0.010, teal, QStringLiteral("Nocturne rim teal"),
             QStringLiteral("rim"), 0.55f, BLEND_SCREEN, rimTealM);
        if (pieces.size() >= 2)
        {
            detail::fadeHorizontally(pieces[pieces.size() - 2].image, true);
            detail::fadeHorizontally(pieces[pieces.size() - 1].image, false);
        }
        if (cancelled)
            return pieces;

        CreativeMotion flightM;
        flightM.rotationEnergy = 18.0f;
        flightM.opacityHold = 0.22f;
        flightM.dxEnergy = 0.012f;
        add(QStringLiteral("Nocturne butterflies"), QStringLiteral("accent"),
            creative::butterflyField(
                stage, seed, { lilac, violet, teal, silver },
                { QPointF(subjectRect.x() + subjectRect.width() * 0.10,
                          subjectRect.y() + subjectRect.height() * 0.18),
                  QPointF(subjectRect.right() - subjectRect.width() * 0.08,
                          subjectRect.y() + subjectRect.height() * 0.36),
                  QPointF(subjectRect.x() + subjectRect.width() * 0.20,
                          subjectRect.bottom() - subjectRect.height() * 0.10) },
                7),
            full, BLEND_NORMAL, 0.8f, flightM);

        add(QStringLiteral("Nocturne petals"), QStringLiteral("accent"),
            creative::confettiField(stage, seed + 5,
                                    { QColor(120, 40, 90), lilac, teal },
                                    34, H * 0.008, H * 0.022),
            full, BLEND_NORMAL, 0.5f);

        add(QStringLiteral("Nocturne grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 9, 22), full,
            BLEND_OVERLAY, 0.3f);
    }

    else if (patchId == QLatin1String("holo-fashion"))
    {
        const QColor deep(4, 16, 26), cyan(0, 229, 255), violet(124, 77, 255),
            pink(255, 61, 218), gold(255, 230, 109), silver(232, 246, 255);

        add(QStringLiteral("Holo night"), QStringLiteral("backdrop"),
            creative::gradientField(stage, deep, QColor(4, 3, 10),
                                    QColor(40, 16, 80), 0.55),
            full);
        CreativeMotion auraM;
        auraM.opacityX = -0.18f;
        auraM.opacityY = 0.20f;
        add(QStringLiteral("Holo aura"), QStringLiteral("accent"),
            creative::radialGlow(
                stage, subjectRect.center(),
                qMax(subjectRect.width(), subjectRect.height()) * 0.66, violet,
                0.52f),
            full, BLEND_SCREEN, 0.70f, auraM);
        if (cancelled)
            return pieces;

        /* the material: hue and chroma move, the fabric's own lightness,
         * texture and shading stay exactly where they were */
        creative::RecolorParams recolor;
        recolor.palette = { cyan, violet, pink, gold };
        recolor.strength = 0.92f;
        recolor.textureKeep = 0.42f;
        recolor.skinGuard = 0.70f;
        recolor.hueDrift = 0.55f;
        recolor.seed = seed;
        QImage holoSubject = creative::recolorMasked(s, subjMask, recolor);
        CreativeMotion holoM;
        holoM.opacityX = 0.08f;
        subjectFull(holoSubject, QStringLiteral("Holo muse"), 1.0f, holoM);
        if (cancelled)
            return pieces;

        QImage sheen = creative::spectralSheen(
            s, subjMask, { cyan, pink, gold, silver }, 1.0f, -28.0f,
            float(seed % 628) / 100.0f);
        QImage sheenFull = detail::stageCanvas(stage);
        drawAt(sheenFull, sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY = 0.30f;
        sheenM.opacityHold = 0.20f;
        add(QStringLiteral("Liquid sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN, 0.92f, sheenM);
        if (cancelled)
            return pieces;

        CreativeMotion chromeM;
        chromeM.opacityY = 0.24f;
        band(0.007, silver, QStringLiteral("Chrome edge"),
             QStringLiteral("rim"), 1.0f, BLEND_SCREEN, chromeM);
        CreativeMotion iridescentM;
        iridescentM.opacityX = -0.26f;
        iridescentM.opacityY = 0.18f;
        band(0.020, pink, QStringLiteral("Chromatic bloom"),
             QStringLiteral("rim"), 0.55f, BLEND_SCREEN, iridescentM);

        QList<QPointF> anchors;
        anchors << QPointF(subjectRect.x() + subjectRect.width() * 0.12,
                           subjectRect.y() + subjectRect.height() * 0.20)
                << QPointF(subjectRect.right() - subjectRect.width() * 0.10,
                           subjectRect.y() + subjectRect.height() * 0.38)
                << QPointF(subjectRect.x() + subjectRect.width() * 0.18,
                           subjectRect.bottom() - subjectRect.height() * 0.14)
                << QPointF(subjectRect.right() - subjectRect.width() * 0.22,
                           subjectRect.y() + subjectRect.height() * 0.62)
                << QPointF(subjectRect.center());
        CreativeMotion flightM;
        flightM.dyY = 0.05f;
        flightM.rotationHold = 12.0f;
        flightM.opacityHold = 0.18f;
        add(QStringLiteral("Holo butterflies"), QStringLiteral("accent"),
            creative::butterflyField(stage, seed,
                                     { pink, cyan, violet, gold }, anchors, 9),
            full, BLEND_SCREEN, 0.90f, flightM);
        if (cancelled)
            return pieces;

        CreativeMotion prismM;
        prismM.opacityEnergy = 0.30f;
        prismM.opacityHold = 0.25f;
        add(QStringLiteral("Prism sparkle"), QStringLiteral("accent"),
            creative::starField(stage, seed + 4, { silver, gold, cyan }, 80),
            full, BLEND_SCREEN, 0.62f, prismM);

        QImage mirror = creative::reflection(s, 0.20, 0.15);
        QImage mirrorFull = detail::stageCanvas(stage);
        drawAt(mirrorFull, mirror,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        CreativeMotion mirrorM;
        mirrorM.opacityY = -0.10f;
        add(QStringLiteral("Holo reflection"), QStringLiteral("echo"),
            mirrorFull, full, BLEND_SCREEN, 0.45f, mirrorM);
    }
    else if (detail::isHoloMaterialPatch(patchId))
    {
        const creative::HoloMaterial material =
            creative::holoMaterialPreset(detail::holoMaterialIdForPatch(patchId));
        const bool automotive = detail::isAutomotivePatch(patchId);
        const QColor deep(4, 16, 26);
        const QColor c0 = material.palette.value(0, QColor(0, 229, 255));
        const QColor c1 = material.palette.value(1, QColor(124, 77, 255));
        const QColor c2 = material.palette.value(2, QColor(255, 61, 218));
        const QColor c3 = material.palette.value(3, QColor(255, 230, 109));

        add(QStringLiteral("Holo night"), QStringLiteral("backdrop"),
            creative::gradientField(stage, deep, QColor(4, 3, 10),
                                    c1.darker(260), 0.50),
            full);
        CreativeMotion auraM;
        auraM.opacityX = -0.18f;
        auraM.opacityY = 0.20f;
        add(QStringLiteral("Material aura"), QStringLiteral("accent"),
            creative::radialGlow(
                stage, subjectRect.center(),
                qMax(subjectRect.width(), subjectRect.height()) * 0.66, c1,
                0.52f),
            full, BLEND_SCREEN, 0.70f, auraM);
        if (cancelled)
            return pieces;

        QImage confidence;
        const QImage normals = creative::estimateNormals(
            s, subjMask, material.normalStrength, &confidence);
        if (cancelled)
            return pieces;
        /* Fashion semantic protection (FASHN garment/hair/skin masks) is a
         * detail-preservation plane, not a second coverage mask: the subject
         * alpha still defines what exists, while protection keeps skin/hair
         * pixels original. Automotive patches style the whole bodywork and
         * never receive the fashion plane. */
        QImage protection;
        if (!automotive && !input.protection.isNull())
        {
            protection = input.protection;
            if (protection.size() != s.size())
                protection = protection.scaled(s.size(),
                                               Qt::IgnoreAspectRatio,
                                               Qt::SmoothTransformation);
        }
        const creative::HoloMaterialResult mat =
            creative::holographicMaterial(
                s, subjMask, normals, material, 1.0f, seed,
                creative::CoverageMode::InheritedCoverage, confidence,
                protection);
        CreativeMotion holoM;
        holoM.opacityX = 0.06f;
        holoM.opacityY = 0.14f;
        subjectFull(mat.albedo, material.name, 1.0f, holoM);
        if (cancelled)
            return pieces;

        QImage sheenFull = detail::stageCanvas(stage);
        drawAt(sheenFull, mat.sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY = 0.30f;
        sheenM.opacityHold = 0.20f;
        add(QStringLiteral("Material sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN, 0.92f, sheenM);
        if (cancelled)
            return pieces;

        CreativeMotion edgeM;
        edgeM.opacityY = 0.24f;
        band(0.007, c3, QStringLiteral("Material edge"),
             QStringLiteral("rim"), 0.90f, BLEND_SCREEN, edgeM);
        CreativeMotion bloomM;
        bloomM.opacityX = -0.24f;
        bloomM.opacityY = 0.18f;
        band(0.020, c2, QStringLiteral("Material bloom"),
             QStringLiteral("rim"), 0.50f, BLEND_SCREEN, bloomM);

        if (!automotive)
        {
            QList<QPointF> anchors;
            anchors << QPointF(subjectRect.x() + subjectRect.width() * 0.12,
                               subjectRect.y() + subjectRect.height() * 0.20)
                    << QPointF(subjectRect.right()
                                   - subjectRect.width() * 0.10,
                               subjectRect.y() + subjectRect.height() * 0.38)
                    << QPointF(subjectRect.x() + subjectRect.width() * 0.18,
                               subjectRect.bottom()
                                   - subjectRect.height() * 0.14)
                    << QPointF(subjectRect.right()
                                   - subjectRect.width() * 0.22,
                               subjectRect.y() + subjectRect.height() * 0.62)
                    << QPointF(subjectRect.center());
            CreativeMotion flightM;
            flightM.dyY = 0.05f;
            flightM.rotationHold = 12.0f;
            flightM.opacityHold = 0.18f;
            add(QStringLiteral("Material butterflies"),
                QStringLiteral("accent"),
                creative::butterflyField(stage, seed, { c2, c0, c1, c3 },
                                         anchors, 9),
                full, BLEND_SCREEN, 0.90f, flightM);
            CreativeMotion sparkleM;
            sparkleM.opacityEnergy = 0.30f;
            sparkleM.opacityHold = 0.25f;
            add(QStringLiteral("Material sparkle"), QStringLiteral("accent"),
                creative::starField(stage, seed + 4, { c3, c0, c2 }, 70),
                full, BLEND_SCREEN, 0.62f, sparkleM);
        }
        else if (detail::holoMaterialIdForPatch(patchId)
                 == creative::HoloMaterialId::GlitterMetallic)
        {
            CreativeMotion flakeM;
            flakeM.opacityEnergy = 0.25f;
            add(QStringLiteral("Glitter sparkle"), QStringLiteral("accent"),
                creative::starField(stage, seed + 4, { c3, c0 }, 55),
                full, BLEND_SCREEN, 0.55f, flakeM);
        }
        if (cancelled)
            return pieces;

        QImage mirror = creative::reflection(s, 0.20, 0.15);
        QImage mirrorFull = detail::stageCanvas(stage);
        drawAt(mirrorFull, mirror,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        CreativeMotion mirrorM;
        mirrorM.opacityY = -0.10f;
        add(QStringLiteral("Material reflection"), QStringLiteral("echo"),
            mirrorFull, full, BLEND_SCREEN, 0.45f, mirrorM);
    }
    else if (patchId == QLatin1String("ink-creature"))
    {
        const QColor paper(246, 240, 228), ink(20, 18, 16), red(214, 69, 69);
        add(QStringLiteral("Ink paper"), QStringLiteral("backdrop"),
            creative::gradientField(stage, paper, QColor(235, 226, 208),
                                    QColor(255, 255, 255, 150), 0.25),
            full);
        add(QStringLiteral("Ink wash"), QStringLiteral("echo"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.42, QColor(40, 34, 28), 0.28f),
            full, BLEND_MULTIPLY, 0.5f);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.02f, 1.30f, 0.35f, ink, 0.20f,
                                QColor(255, 255, 255), 0.05f });
        subjectFull(graded, QStringLiteral("Ink figure"), 1.0f, CreativeMotion());
        if (cancelled)
            return pieces;

        CreativeMotion lineM;
        lineM.opacityX = 0.20f;
        lineM.opacityEnergy = 0.25f;
        band(0.006, ink, QStringLiteral("Ink line"), QStringLiteral("rim"),
             0.9f, BLEND_NORMAL, lineM);
        CreativeMotion lineSoftM;
        lineSoftM.opacityX = -0.20f;
        band(0.014, QColor(20, 18, 16, 120), QStringLiteral("Ink halo"),
             QStringLiteral("rim"), 0.35f, BLEND_NORMAL, lineSoftM);
        if (cancelled)
            return pieces;

        CreativeMotion splatM;
        splatM.opacityEnergy = 0.35f;
        splatM.scaleEnergy = 0.15f;
        add(QStringLiteral("Ink splatter"), QStringLiteral("accent"),
            creative::inkSplatter(stage, seed,
                                  subjectRect.adjusted(-W * 0.12, -H * 0.10,
                                                       W * 0.12, H * 0.10),
                                  ink, 80),
            full, BLEND_NORMAL, 0.7f, splatM);

        QImage eyes = detail::stageCanvas(stage);
        {
            QPainter p(&eyes);
            p.setRenderHint(QPainter::Antialiasing, true);
            const qreal er = H * 0.024;
            const QPointF left(subjectRect.x() + subjectRect.width() * 0.30,
                               subjectRect.y() + subjectRect.height() * 0.24);
            const QPointF right(subjectRect.x() + subjectRect.width() * 0.63,
                                subjectRect.y() + subjectRect.height() * 0.22);
            for (const QPointF &c : { left, right })
            {
                QRadialGradient glow(c, er * 2.6);
                glow.setColorAt(0.0, QColor(214, 69, 69, 150));
                glow.setColorAt(1.0, QColor(214, 69, 69, 0));
                p.setPen(Qt::NoPen);
                p.setBrush(glow);
                p.drawEllipse(c, er * 2.6, er * 2.6);
                p.setBrush(QColor(252, 248, 238, 245));
                p.setPen(QPen(QColor(20, 18, 16), qMax<qreal>(1.0, er * 0.18)));
                p.drawEllipse(c, er, er * 1.15);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(20, 18, 16));
                p.drawEllipse(c + QPointF(er * 0.15, 0), er * 0.42, er * 0.55);
                p.setBrush(QColor(255, 255, 255, 220));
                p.drawEllipse(c + QPointF(-er * 0.28, -er * 0.35), er * 0.16,
                              er * 0.16);
            }
        }
        CreativeMotion eyesM;
        eyesM.opacityY = 0.15f;
        eyesM.opacityHold = 0.55f;
        add(QStringLiteral("Ink creature eyes"), QStringLiteral("accent"),
            eyes, full, BLEND_NORMAL, 0.35f, eyesM);
        add(QStringLiteral("Ink grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 4, 34), full,
            BLEND_MULTIPLY, 0.4f);
    }
    else if (patchId == QLatin1String("spark-storm"))
    {
        const QColor night(7, 6, 13), gold(255, 211, 122), pink(255, 122, 200),
            cyan(122, 240, 255), white(255, 250, 240);
        add(QStringLiteral("Storm sky"), QStringLiteral("backdrop"),
            creative::gradientField(stage, night, QColor(24, 12, 40),
                                    QColor(70, 30, 110, 120), 0.30),
            full);
        add(QStringLiteral("Storm stars"), QStringLiteral("backdrop"),
            creative::starField(stage, seed, white, 150), full,
            BLEND_SCREEN, 0.6f);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.05f, 1.10f, 1.12f, QColor(40, 20, 80), 0.18f,
                                gold, 0.10f });
        subjectFull(graded, QStringLiteral("Storm figure"), 1.0f,
                    CreativeMotion());
        if (cancelled)
            return pieces;

        CreativeMotion goldRimM;
        goldRimM.opacityHold = 0.25f;
        band(0.010, gold, QStringLiteral("Storm rim gold"), QStringLiteral("rim"),
             0.8f, BLEND_SCREEN, goldRimM);

        const QPointF center = subjectRect.center();
        const qreal radius = qMax(subjectRect.width(), subjectRect.height()) * 0.68;
        CreativeMotion goldSpiralM;
        goldSpiralM.rotationEnergy = 42.0f;
        goldSpiralM.opacityHold = 0.20f;
        goldSpiralM.scaleEnergy = 0.10f;
        add(QStringLiteral("Storm spiral gold"), QStringLiteral("accent"),
            creative::spiralField(stage, center, radius, seed,
                                  { gold, pink, white }, 260),
            full, BLEND_SCREEN, 0.75f, goldSpiralM);
        if (cancelled)
            return pieces;
        CreativeMotion iceSpiralM;
        iceSpiralM.rotationEnergy = -34.0f;
        iceSpiralM.opacityX = 0.30f;
        add(QStringLiteral("Storm spiral ice"), QStringLiteral("accent"),
            creative::spiralField(stage, center, radius * 0.82, seed + 7,
                                  { cyan, white, QColor(170, 140, 255) }, 190),
            full, BLEND_SCREEN, 0.55f, iceSpiralM);
        add(QStringLiteral("Storm dust"), QStringLiteral("finish"),
            creative::bokehField(stage, seed + 2, { gold, pink, cyan }, 70,
                                 H * 0.004, H * 0.014),
            full, BLEND_SCREEN, 0.5f);
    }
    else if (patchId == QLatin1String("kaleido"))
    {
        const QColor navy(11, 16, 48), cyan(88, 224, 255), magenta(255, 94, 219),
            lime(200, 255, 106);
        add(QStringLiteral("Kaleido night"), QStringLiteral("backdrop"),
            creative::gradientField(stage, navy, QColor(24, 8, 46),
                                    QColor(60, 30, 140, 100), 0.5),
            full);
        if (cancelled)
            return pieces;
        QImage subjectCanvas = detail::stageCanvas(stage);
        drawAt(subjectCanvas, s, subjectRect);
        CreativeMotion k6M;
        k6M.rotationEnergy = 24.0f;
        k6M.opacityX = 0.15f;
        add(QStringLiteral("Kaleido six"), QStringLiteral("subject"),
            creative::wedgeKaleidoscope(subjectCanvas, stage, 6, 1.0, 0.0),
            full, BLEND_NORMAL, 0.9f, k6M);
        if (cancelled)
            return pieces;
        CreativeMotion k8M;
        k8M.rotationEnergy = -18.0f;
        k8M.opacityX = -0.15f;
        k8M.opacityHold = 0.25f;
        add(QStringLiteral("Kaleido eight"), QStringLiteral("subject"),
            creative::wedgeKaleidoscope(subjectCanvas, stage, 8, 1.22, 22.0),
            full, BLEND_SCREEN, 0.45f, k8M);
        for (CreativePiece &kp : pieces)
            if (kp.role == QLatin1String("subject"))
                kp.pivot = full.center();
        if (cancelled)
            return pieces;
        add(QStringLiteral("Kaleido ring"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(), H * 0.55,
                                 magenta, 0.30f),
            full, BLEND_SCREEN, 0.6f);
        add(QStringLiteral("Kaleido stars"), QStringLiteral("finish"),
            creative::starField(stage, seed + 4, cyan, 90), full,
            BLEND_SCREEN, 0.6f);
    }
    else if (patchId == QLatin1String("motion-echo"))
    {
        const QColor night(18, 19, 38), blue(78, 123, 255), pink(255, 111, 174);
        add(QStringLiteral("Echo stage"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(9, 9, 20), night,
                                    QColor(90, 60, 160, 90), 0.45),
            full);
        if (cancelled)
            return pieces;
        const qreal spacing = subjectRect.width() * 0.26;
        for (int i = 0; i < 6; ++i)
        {
            if (cancelled)
                return pieces;
            const int k = i - 3;
            if (k == 0)
                continue;
            QImage echo = creative::hueShift(s, k * 14);
            echo = creative::gradeImage(
                echo, creative::Grade{ 0.92f, 1.08f, 0.9f,
                                       k < 0 ? blue : pink, 0.22f,
                                       QColor(255, 255, 255), 0.05f });
            QImage echoFull = detail::stageCanvas(stage);
            drawAt(echoFull, echo,
                   subjectRect.translated(k * spacing, k * spacing * 0.05));
            CreativeMotion echoM;
            echoM.opacityX = 0.06f;
            echoM.dxX = 0.012f * k;
            add(QStringLiteral("Echo %1").arg(k > 0 ? k : -k),
                QStringLiteral("echo"), echoFull, full, BLEND_NORMAL,
                float(0.48 - qAbs(k) * 0.070), echoM);
        }
        if (cancelled)
            return pieces;
        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.03f, 1.10f, 1.14f, blue, 0.14f, pink, 0.10f });
        subjectFull(graded, QStringLiteral("Echo figure"), 1.0f, CreativeMotion());
        CreativeMotion trailM;
        trailM.opacityEnergy = 0.35f;
        add(QStringLiteral("Echo trail"), QStringLiteral("accent"),
            creative::bokehField(stage, seed, { blue, pink,
                                                QColor(255, 255, 255) },
                                 80, H * 0.004, H * 0.016),
            full, BLEND_SCREEN, 0.45f, trailM);
        add(QStringLiteral("Echo grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 3, 20), full,
            BLEND_OVERLAY, 0.3f);
    }
    else if (patchId == QLatin1String("scene-forge"))
    {
        const QColor cream(244, 230, 200), teal(34, 92, 84),
            ochre(214, 154, 64), ink(36, 27, 20), rust(178, 70, 44);
        add(QStringLiteral("Paper sky"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(248, 238, 214),
                                    QColor(226, 198, 156),
                                    QColor(255, 255, 255, 160), 0.18),
            full);

        const auto hill = [&](const QColor &color, qreal y, qreal amp,
                              int phase) {
            QImage img = detail::stageCanvas(stage);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            QPainterPath path;
            path.moveTo(0, H);
            path.lineTo(0, y);
            const int steps = 8;
            for (int i = 0; i < steps; ++i)
            {
                const qreal x0 = W * i / steps;
                const qreal x1 = W * (i + 1) / steps;
                const qreal yy = y + std::sin((i + phase) * 1.7) * amp;
                path.quadTo((x0 + x1) / 2.0,
                            y + std::sin((i + phase + 0.5) * 1.7) * amp * 1.2,
                            x1, yy);
            }
            path.lineTo(W, H);
            path.closeSubpath();
            p.drawPath(path);
            return img;
        };
        CreativeMotion hillBackM;
        hillBackM.dxX = 0.018f;
        add(QStringLiteral("Back hill"), QStringLiteral("backdrop"),
            hill(teal, H * 0.46, H * 0.045, 0), full, BLEND_NORMAL, 0.95f,
            hillBackM);
        CreativeMotion hillMidM;
        hillMidM.dxX = -0.022f;
        add(QStringLiteral("Mid hill"), QStringLiteral("backdrop"),
            hill(ochre, H * 0.57, H * 0.032, 1), full, BLEND_NORMAL, 0.95f,
            hillMidM);
        add(QStringLiteral("Front stage"), QStringLiteral("backdrop"),
            hill(ink, H * 0.72, H * 0.012, 2), full, BLEND_NORMAL, 1.0f);

        const auto curtain = [&](bool left) {
            QImage img = detail::stageCanvas(stage);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath path;
            if (left)
            {
                path.moveTo(0, 0);
                path.lineTo(W * 0.26, 0);
                path.cubicTo(W * 0.18, H * 0.35, W * 0.27, H * 0.70,
                             W * 0.16, H);
                path.lineTo(0, H);
            }
            else
            {
                path.moveTo(W, 0);
                path.lineTo(W * 0.74, 0);
                path.cubicTo(W * 0.82, H * 0.35, W * 0.73, H * 0.70,
                             W * 0.84, H);
                path.lineTo(W, H);
            }
            path.closeSubpath();
            QLinearGradient g(0, 0, W * (left ? 0.3 : 0.7), 0);
            g.setColorAt(0.0, rust.darker(170));
            g.setColorAt(0.55, rust);
            g.setColorAt(1.0, rust.lighter(125));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawPath(path);
            p.setPen(QPen(QColor(60, 12, 20, 110), qMax(1.0, W * 0.003)));
            for (int i = 1; i <= 3; ++i)
            {
                const qreal x = W * (left ? 0.06 + 0.05 * i
                                          : 0.94 - 0.05 * i);
                p.drawLine(QPointF(x, 0), QPointF(x + (left ? 8 : -8), H));
            }
            return img;
        };
        CreativeMotion curtainL;
        curtainL.dxX = -0.055f;
        add(QStringLiteral("Curtain left"), QStringLiteral("backdrop"),
            curtain(true), full, BLEND_NORMAL, 0.95f, curtainL);
        CreativeMotion curtainR;
        curtainR.dxX = 0.055f;
        add(QStringLiteral("Curtain right"), QStringLiteral("backdrop"),
            curtain(false), full, BLEND_NORMAL, 0.95f, curtainR);

        QImage cone = detail::stageCanvas(stage);
        {
            QPainter p(&cone);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath beam;
            beam.moveTo(W * 0.50, H * 0.04);
            beam.lineTo(W * 0.16, H * 0.82);
            beam.lineTo(W * 0.84, H * 0.82);
            beam.closeSubpath();
            QLinearGradient g(0, H * 0.04, 0, H * 0.82);
            g.setColorAt(0.0, QColor(255, 240, 190, 120));
            g.setColorAt(1.0, QColor(255, 230, 170, 0));
            p.fillPath(beam, g);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 240, 200, 90));
            p.drawEllipse(QPointF(W * 0.5, H * 0.82), W * 0.34, H * 0.035);
        }
        CreativeMotion spotM;
        spotM.opacityY = 0.20f;
        spotM.opacityHold = 0.15f;
        spotM.dxX = 0.035f;
        add(QStringLiteral("Spotlight"), QStringLiteral("accent"), cone, full,
            BLEND_SCREEN, 0.75f, spotM);

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.06f, 1.10f, 1.04f, teal, 0.12f, cream, 0.12f });
        subjectFull(graded, QStringLiteral("Paper figure"), 1.0f,
                    CreativeMotion());
        if (cancelled)
            return pieces;
        CreativeMotion starM;
        starM.opacityEnergy = 0.20f;
        add(QStringLiteral("Paper stars"), QStringLiteral("accent"),
            creative::starField(stage, seed, QColor(255, 244, 210), 80),
            full, BLEND_SCREEN, 0.5f, starM);
        add(QStringLiteral("Paper grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 5, 30), full,
            BLEND_OVERLAY, 0.35f);
    }
    else if (patchId == QLatin1String("auto-splatter"))
    {
        const QColor paper(244, 237, 226), ink(18, 16, 14), red(255, 77, 46);
        add(QStringLiteral("Torn paper"), QStringLiteral("backdrop"),
            creative::gradientField(stage, paper, QColor(226, 214, 196),
                                    QColor(255, 255, 255, 130), 0.18),
            full);
        add(QStringLiteral("Poster halftone"), QStringLiteral("backdrop"),
            creative::halftoneField(stage, QColor(20, 18, 16, 34),
                                    H * 0.022, H * 0.0045, 20, 0.0),
            full, BLEND_MULTIPLY, 0.8f);
        if (cancelled)
            return pieces;

        QImage spot = detail::stageCanvas(stage);
        {
            QPainter p(&spot);
            p.setRenderHint(QPainter::Antialiasing, true);
            QRadialGradient g(QPointF(W * 0.30, H * 0.32), H * 0.32);
            g.setColorAt(0.0, QColor(255, 77, 46, 230));
            g.setColorAt(0.72, QColor(255, 77, 46, 200));
            g.setColorAt(1.0, QColor(255, 77, 46, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(QPointF(W * 0.30, H * 0.32), H * 0.30, H * 0.24);
        }
        add(QStringLiteral("Poster spot"), QStringLiteral("backdrop"), spot,
            full, BLEND_MULTIPLY, 0.85f);

        CreativeMotion splatM;
        splatM.opacityEnergy = 0.25f;
        splatM.scaleEnergy = 0.12f;
        splatM.rotationEnergy = 8.0f;
        add(QStringLiteral("Ink blast"), QStringLiteral("accent"),
            creative::inkSplatter(stage, seed,
                                  subjectRect.adjusted(-W * 0.20, -H * 0.14,
                                                       W * 0.20, H * 0.14),
                                  ink, 150),
            full, BLEND_NORMAL, 0.85f, splatM);
        CreativeMotion redM;
        redM.opacityX = 0.20f;
        redM.opacityHold = 0.20f;
        add(QStringLiteral("Red blast"), QStringLiteral("accent"),
            creative::inkSplatter(stage, seed + 9,
                                  subjectRect.adjusted(-W * 0.14, -H * 0.10,
                                                       W * 0.14, H * 0.10),
                                  red, 80),
            full, BLEND_MULTIPLY, 0.75f, redM);

        QImage drips = detail::stageCanvas(stage);
        {
            QPainter p(&drips);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            QRandomGenerator rng(seed + 31);
            for (int i = 0; i < 9; ++i)
            {
                const qreal x = W * (0.12 + 0.76 * rng.bounded(1000) / 1000.0);
                const qreal y = H * (0.52 + 0.20 * rng.bounded(1000) / 1000.0);
                const qreal len = H * (0.06 + 0.16 * rng.bounded(1000) / 1000.0);
                const qreal r = qMax(2.0, H * 0.006);
                p.setBrush(QColor(i % 3 == 0 ? red : ink));
                p.drawRoundedRect(QRectF(x - r, y, r * 2.0, len), r, r);
                p.drawEllipse(QPointF(x, y + len), r * 1.4, r * 1.4);
            }
        }
        CreativeMotion dripM;
        dripM.opacityHold = 0.55f;
        add(QStringLiteral("Paint drips"), QStringLiteral("accent"), drips,
            full, BLEND_NORMAL, 0.0f, dripM);
        if (cancelled)
            return pieces;

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.04f, 1.22f, 0.90f, ink, 0.18f, red, 0.10f });
        subjectFull(graded, QStringLiteral("Poster figure"), 1.0f,
                    CreativeMotion());

        QImage tape = detail::stageCanvas(stage);
        {
            QPainter p(&tape);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 250, 235, 145));
            p.translate(W * 0.16, H * 0.10);
            p.rotate(-32);
            p.drawRect(QRectF(-W * 0.06, -H * 0.015, W * 0.24, H * 0.03));
            p.resetTransform();
            p.translate(W * 0.84, H * 0.88);
            p.rotate(28);
            p.drawRect(QRectF(-W * 0.06, -H * 0.015, W * 0.24, H * 0.03));
        }
        add(QStringLiteral("Tape"), QStringLiteral("finish"), tape, full,
            BLEND_NORMAL, 0.8f);
        add(QStringLiteral("Poster grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 7, 34), full,
            BLEND_OVERLAY, 0.4f);
    }
    else if (patchId == QLatin1String("stage-mode"))
    {
        const QColor night(8, 6, 12), gold(255, 211, 122),
            rose(184, 51, 106), cream(255, 244, 226);
        add(QStringLiteral("House lights"), QStringLiteral("backdrop"),
            creative::gradientField(stage, night, QColor(18, 10, 24),
                                    QColor(70, 20, 50, 120), 0.30),
            full);
        QImage floor = detail::stageCanvas(stage);
        {
            QPainter p(&floor);
            p.setRenderHint(QPainter::Antialiasing, true);
            QLinearGradient g(0, H * 0.68, 0, H);
            g.setColorAt(0.0, QColor(28, 16, 30, 230));
            g.setColorAt(1.0, QColor(6, 4, 8, 255));
            p.fillRect(QRectF(0, H * 0.68, W, H * 0.32), g);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 220, 170, 30));
            p.drawEllipse(QPointF(W * 0.5, H * 0.84), W * 0.40, H * 0.04);
        }
        add(QStringLiteral("Stage floor"), QStringLiteral("backdrop"), floor,
            full);
        const auto curtain = [&](bool left) {
            QImage img = detail::stageCanvas(stage);
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath path;
            if (left)
            {
                path.moveTo(0, 0);
                path.lineTo(W * 0.24, 0);
                path.cubicTo(W * 0.17, H * 0.38, W * 0.25, H * 0.72,
                             W * 0.14, H);
                path.lineTo(0, H);
            }
            else
            {
                path.moveTo(W, 0);
                path.lineTo(W * 0.76, 0);
                path.cubicTo(W * 0.83, H * 0.38, W * 0.75, H * 0.72,
                             W * 0.86, H);
                path.lineTo(W, H);
            }
            path.closeSubpath();
            QLinearGradient g(0, 0, W * (left ? 0.28 : 0.72), 0);
            g.setColorAt(0.0, rose.darker(190));
            g.setColorAt(0.5, rose.darker(135));
            g.setColorAt(1.0, rose);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawPath(path);
            return img;
        };
        CreativeMotion curtainL;
        curtainL.dxX = -0.05f;
        add(QStringLiteral("Stage curtain left"), QStringLiteral("backdrop"),
            curtain(true), full, BLEND_NORMAL, 0.95f, curtainL);
        CreativeMotion curtainR;
        curtainR.dxX = 0.05f;
        add(QStringLiteral("Stage curtain right"), QStringLiteral("backdrop"),
            curtain(false), full, BLEND_NORMAL, 0.95f, curtainR);

        QImage cone = detail::stageCanvas(stage);
        {
            QPainter p(&cone);
            p.setRenderHint(QPainter::Antialiasing, true);
            QPainterPath beam;
            beam.moveTo(W * 0.50, H * 0.03);
            beam.lineTo(W * 0.12, H * 0.86);
            beam.lineTo(W * 0.88, H * 0.86);
            beam.closeSubpath();
            QLinearGradient g(0, H * 0.03, 0, H * 0.86);
            g.setColorAt(0.0, QColor(255, 240, 200, 130));
            g.setColorAt(1.0, QColor(255, 220, 160, 0));
            p.fillPath(beam, g);
        }
        CreativeMotion spotM;
        spotM.opacityY = 0.20f;
        spotM.opacityHold = 0.20f;
        spotM.dxX = 0.04f;
        add(QStringLiteral("Stage spotlight"), QStringLiteral("accent"), cone,
            full, BLEND_SCREEN, 0.8f, spotM);
        add(QStringLiteral("Stage glow"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(), H * 0.45,
                                  gold, 0.36f),
            full, BLEND_SCREEN, 0.55f);
        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 0.98f, 1.16f, 1.04f, QColor(40, 18, 50), 0.22f,
                                gold, 0.08f });
        subjectFull(graded, QStringLiteral("Stage figure"), 1.0f,
                    CreativeMotion());
        if (cancelled)
            return pieces;
        add(QStringLiteral("Stage dust"), QStringLiteral("finish"),
            creative::starField(stage, seed, cream, 70), full,
            BLEND_SCREEN, 0.45f);
        add(QStringLiteral("Stage grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 2, 24), full,
            BLEND_OVERLAY, 0.3f);
    }
    else if (patchId == QLatin1String("paper-theatre"))
    {
        const QColor cream(246, 233, 208), sand(232, 210, 172),
            ochre(214, 154, 64), rust(172, 72, 52), teal(38, 96, 86),
            ink(38, 27, 20), rose(191, 86, 96), dusk(70, 40, 64);

        add(QStringLiteral("Paper stock"), QStringLiteral("backdrop"),
            creative::paperStockField(stage, seed, cream, ochre, 0.85f), full);
        add(QStringLiteral("Paper dusk wash"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(255, 248, 232, 0),
                                    QColor(66, 34, 52, 120),
                                    QColor(255, 226, 170, 96), 0.30),
            full, BLEND_MULTIPLY, 0.55f);
        if (cancelled)
            return pieces;

        auto addSceneProp = [&](const detail::SceneProp &prop,
                                const QString &name, const QString &role,
                                int blend, float opacity,
                                const CreativeMotion &motion,
                                const QString &propKind, float depth) {
            if (cancelled || prop.image.isNull())
                return;
            CreativePiece p;
            p.name = name;
            p.role = role;
            p.image = prop.image;
            p.rect = QRectF(prop.topLeft, QSizeF(prop.image.size()));
            p.pivot = p.rect.center();
            p.blendMode = blend;
            p.opacity = opacity;
            p.motion = motion;
            p.metadata[QStringLiteral("creative.prop")] = propKind;
            p.metadata[QStringLiteral("creative.depth")] = double(depth);
            p.metadata[QStringLiteral("creative.parallax")] = double(motion.dxX);
            pieces.append(p);
            pieceAdded();
        };
        auto addScenePropShadowed =
            [&](const detail::SceneProp &prop, const QString &name,
                const QString &role, int blend, float opacity,
                const CreativeMotion &motion, const QString &propKind,
                float depth, const QColor &shadowColor, qreal shadowDx,
                qreal shadowDy, float shadowOpacity) {
                if (cancelled || prop.image.isNull())
                    return;
                const QImage shadow = creative::silhouetteShadow(
                    prop.image, shadowColor, 1.0f);
                if (!shadow.isNull())
                {
                    detail::SceneProp shadowProp{
                        shadow,
                        prop.topLeft + QPointF(shadowDx, shadowDy)
                    };
                    addSceneProp(
                        shadowProp, name + QStringLiteral(" shadow"),
                        role == QLatin1String("backdrop")
                            ? QStringLiteral("backdrop")
                            : QStringLiteral("echo"),
                        BLEND_NORMAL, shadowOpacity, motion,
                        propKind + QStringLiteral("-shadow"), depth - 0.02f);
                }
                addSceneProp(prop, name, role, blend, opacity, motion,
                             propKind, depth);
            };

        CreativeMotion moonM;
        moonM.dxX = 0.018f;
        moonM.dyY = 0.008f;
        const detail::SceneProp moon = detail::makePaperMoon(
            stage, seed + 3, QColor(252, 242, 214), ochre,
            QPointF(W * 0.62, H * 0.21), H * 0.085);
        addScenePropShadowed(moon, QStringLiteral("Paper moon"),
                             QStringLiteral("backdrop"), BLEND_NORMAL, 0.92f,
                             moonM, QStringLiteral("moon"), -0.30f,
                             QColor(52, 30, 30), W * 0.006, H * 0.010, 0.30f);

        CreativeMotion archM;
        archM.dxX = 0.012f;
        archM.dyY = 0.006f;
        addSceneProp(detail::makePaperProscenium(stage, rust, cream),
                     QStringLiteral("Paper proscenium"),
                     QStringLiteral("backdrop"), BLEND_NORMAL, 1.0f, archM,
                     QStringLiteral("proscenium"), -0.45f);

        CreativeMotion castleM;
        castleM.dxX = 0.030f;
        castleM.dyY = 0.010f;
        addScenePropShadowed(
            detail::makePaperCastle(stage, seed + 5, ochre.darker(118), cream,
                                    W * 0.56, H * 0.61, W * 0.32, H * 0.20),
            QStringLiteral("Paper castle"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.92f, castleM, QStringLiteral("castle"), -0.48f,
            QColor(58, 32, 28), W * 0.005, H * 0.009, 0.34f);

        CreativeMotion backM;
        backM.dxX = 0.020f;
        backM.dyY = 0.008f;
        addScenePropShadowed(
            detail::makePaperPlane(stage, sand, cream, H * 0.50, H * 0.045,
                                   0.5, 2),
            QStringLiteral("Paper back plane"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.96f, backM, QStringLiteral("plane-back"), -0.60f,
            QColor(60, 34, 30), W * 0.004, H * 0.008, 0.30f);

        CreativeMotion treeLM;
        treeLM.dxX = 0.045f;
        treeLM.dyY = 0.014f;
        addScenePropShadowed(
            detail::makePaperTree(stage, seed + 7, teal.darker(108), cream,
                                  W * 0.30, H * 0.72, H * 0.42, W * 0.10),
            QStringLiteral("Paper tree left"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.95f, treeLM, QStringLiteral("tree"), -0.36f,
            QColor(40, 30, 28), W * 0.006, H * 0.010, 0.36f);
        CreativeMotion treeRM;
        treeRM.dxX = 0.050f;
        treeRM.dyY = 0.014f;
        addScenePropShadowed(
            detail::makePaperTree(stage, seed + 11, teal.darker(122), cream,
                                  W * 0.70, H * 0.74, H * 0.38, W * 0.10),
            QStringLiteral("Paper tree right"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.95f, treeRM, QStringLiteral("tree"), -0.35f,
            QColor(40, 30, 28), W * 0.006, H * 0.010, 0.36f);

        CreativeMotion midM;
        midM.dxX = -0.035f;
        midM.dyY = 0.012f;
        addScenePropShadowed(
            detail::makePaperPlane(stage, ochre, cream, H * 0.70, H * 0.030,
                                   1.7, 3),
            QStringLiteral("Paper mid plane"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.96f, midM, QStringLiteral("plane-mid"), -0.20f,
            QColor(52, 32, 28), W * 0.004, H * 0.008, 0.32f);

        CreativeMotion fanM;
        fanM.dxX = -0.055f;
        fanM.rotationHold = 3.0f;
        addScenePropShadowed(
            detail::makePaperFan(stage, seed + 13, rose, cream, ink,
                                 QPointF(W * 0.30, H * 0.73), H * 0.165, 195.0,
                                 150.0),
            QStringLiteral("Paper fan"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.95f, fanM, QStringLiteral("fan"), 0.05f,
            QColor(48, 28, 28), W * 0.005, H * 0.008, 0.32f);

        CreativeMotion ribbonM;
        ribbonM.dxX = 0.070f;
        ribbonM.dyY = 0.016f;
        ribbonM.rotationEnergy = 2.0f;
        addScenePropShadowed(detail::makePaperRibbon(stage, rust, cream),
                             QStringLiteral("Paper ribbon"),
                             QStringLiteral("accent"), BLEND_NORMAL, 0.82f,
                             ribbonM, QStringLiteral("ribbon"), 0.12f,
                             QColor(48, 26, 24), W * 0.005, H * 0.008, 0.28f);

        CreativeMotion garlandM;
        garlandM.dxX = 0.024f;
        garlandM.rotationEnergy = 1.2f;
        addSceneProp(detail::makePaperStarGarland(stage, seed + 23, cream,
                                                  ochre, 9, H * 0.155,
                                                  H * 0.035),
                     QStringLiteral("Paper star garland"),
                     QStringLiteral("accent"), BLEND_NORMAL, 0.95f, garlandM,
                     QStringLiteral("stars"), -0.05f);

        CreativeMotion curtainLM;
        curtainLM.dxX = -0.075f;
        addScenePropShadowed(
            detail::makePaperCurtain(stage, rust, cream, true),
            QStringLiteral("Paper curtain left"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.98f, curtainLM, QStringLiteral("curtain"),
            -0.15f, QColor(40, 20, 18), W * 0.006, H * 0.008, 0.35f);
        CreativeMotion curtainRM;
        curtainRM.dxX = 0.075f;
        addScenePropShadowed(
            detail::makePaperCurtain(stage, rust, cream, false),
            QStringLiteral("Paper curtain right"), QStringLiteral("backdrop"),
            BLEND_NORMAL, 0.98f, curtainRM, QStringLiteral("curtain"),
            -0.15f, QColor(40, 20, 18), W * 0.006, H * 0.008, 0.35f);
        if (cancelled)
            return pieces;

        /* floor shadow anchors the cutout on the paper stage */
        {
            QImage ellipse(stage, QImage::Format_ARGB32_Premultiplied);
            ellipse.fill(Qt::transparent);
            QPainter ep(&ellipse);
            ep.setRenderHint(QPainter::Antialiasing, true);
            ep.setPen(Qt::NoPen);
            ep.setBrush(QColor(0, 0, 0, 255));
            ep.drawEllipse(
                QRectF(subjectRect.center().x() - subjectRect.width() * 0.55,
                       subjectRect.bottom() - H * 0.012,
                       subjectRect.width() * 1.10, H * 0.038));
            ep.end();
            add(QStringLiteral("Paper floor shadow"), QStringLiteral("echo"),
                creative::softBlur(ellipse, 0), full, BLEND_NORMAL, 0.32f);
        }

        CreativeMotion subjectM;
        subjectM.scaleY = 0.060f;
        subjectM.dyY = -0.025f;
        subjectM.opacityY = 0.03f;
        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.03f, 1.08f, 1.00f, dusk, 0.14f, cream,
                                0.10f });
        subjectFull(graded, QStringLiteral("Paper figure"), 1.0f, subjectM);
        if (cancelled)
            return pieces;

        CreativeMotion lipM;
        lipM.dxX = 0.095f;
        lipM.dyY = 0.022f;
        addSceneProp(detail::makePaperForegroundLip(stage, seed + 29, rust,
                                                    cream, H * 0.91),
                     QStringLiteral("Paper stage lip"), QStringLiteral("accent"),
                     BLEND_NORMAL, 1.0f, lipM,
                     QStringLiteral("foreground"), 0.55f);

        CreativeMotion floralLM;
        floralLM.dxX = -0.060f;
        floralLM.dyY = 0.018f;
        addSceneProp(
            detail::makePaperFloral(stage, seed + 17, { rose, cream, teal,
                                                        ochre },
                                    QPointF(W * 0.105, H * 0.84), H * 0.075,
                                    false),
            QStringLiteral("Paper floral left"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.95f, floralLM, QStringLiteral("floral"), 0.10f);
        CreativeMotion floralRM;
        floralRM.dxX = 0.060f;
        floralRM.dyY = 0.018f;
        addSceneProp(
            detail::makePaperFloral(stage, seed + 19, { rose, cream, teal,
                                                        ochre },
                                    QPointF(W * 0.895, H * 0.86), H * 0.070,
                                    true),
            QStringLiteral("Paper floral right"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.95f, floralRM, QStringLiteral("floral"), 0.10f);

        CreativeMotion cornerLM;
        cornerLM.dxX = -0.110f;
        cornerLM.dyY = 0.030f;
        addSceneProp(detail::makePaperCornerFoliage(stage, seed + 31,
                                                    teal.darker(118), cream,
                                                    true),
                     QStringLiteral("Paper corner foliage left"),
                     QStringLiteral("accent"), BLEND_NORMAL, 0.94f, cornerLM,
                     QStringLiteral("foreground"), 0.50f);
        CreativeMotion cornerRM;
        cornerRM.dxX = 0.110f;
        cornerRM.dyY = 0.030f;
        addSceneProp(detail::makePaperCornerFoliage(stage, seed + 33,
                                                    teal.darker(118), cream,
                                                    false),
                     QStringLiteral("Paper corner foliage right"),
                     QStringLiteral("accent"), BLEND_NORMAL, 0.94f, cornerRM,
                     QStringLiteral("foreground"), 0.50f);

        CreativeMotion butterflyM;
        butterflyM.rotationEnergy = 7.0f;
        butterflyM.dxEnergy = 0.010f;
        addSceneProp(
            detail::makePaperButterflies(
                stage, seed + 37, { cream, rose, ochre, teal },
                { QPointF(subjectRect.x() + subjectRect.width() * 0.12,
                          subjectRect.y() + subjectRect.height() * 0.16),
                  QPointF(subjectRect.right() - subjectRect.width() * 0.10,
                          subjectRect.y() + subjectRect.height() * 0.34),
                  QPointF(subjectRect.x() + subjectRect.width() * 0.20,
                          subjectRect.bottom() - subjectRect.height() * 0.14) },
                5, H * 0.022),
            QStringLiteral("Paper butterflies"), QStringLiteral("accent"),
            BLEND_NORMAL, 0.90f, butterflyM, QStringLiteral("butterflies"),
            0.30f);

        addSceneProp(detail::makePaperTitle(stage,
                                            QStringLiteral("PAPER THEATRE"),
                                            cream, ink, 0.048, H * 0.022),
                     QStringLiteral("Paper title"), QStringLiteral("finish"),
                     BLEND_NORMAL, 1.0f, CreativeMotion(),
                     QStringLiteral("typography"), 0.60f);

        add(QStringLiteral("Paper grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 41, 28), full, BLEND_OVERLAY,
            0.34f);
    }
    else
    {
        /* Unknown/empty patch: keep the stage honest. */
        add(QStringLiteral("Stage"), QStringLiteral("backdrop"),
            creative::gradientField(stage, QColor(18, 18, 26),
                                    QColor(8, 8, 12), QColor(80, 60, 160, 90),
                                    0.4),
            full);
        subjectFull(s, QStringLiteral("Figure"), 1.0f, CreativeMotion());
    }

    /* Master's universal stage light is part of the four ported looks. It is
     * added only for those ids so the 18 existing recipes keep their exact
     * current output; the light itself is a normal piece, not a UI effect. */
    if (!cancelled
        && (patchId == QLatin1String("scene-forge")
            || patchId == QLatin1String("auto-splatter")
            || patchId == QLatin1String("stage-mode")
            || patchId == QLatin1String("paper-theatre")))
    {
        const QPointF lightCenter =
            subjectRect.isEmpty() ? full.center() : subjectRect.center();
        CreativeMotion lightM;
        lightM.opacityY = 0.30f;
        lightM.opacityHold = 0.30f;
        lightM.dxX = 0.040f;
        lightM.dxEnergy = 0.015f;
        lightM.scaleY = 0.18f;
        lightM.scaleHold = 0.10f;
        CreativePiece light;
        light.name = QStringLiteral("Stage light");
        light.role = QStringLiteral("finish");
        light.image = creative::radialGlow(
            stage, lightCenter,
            qMax(subjectRect.width(), subjectRect.height()) * 0.95,
            detail::stageLightColor(patchId), 0.30f);
        light.rect = full;
        light.pivot = lightCenter;
        light.blendMode = BLEND_SCREEN;
        light.opacity = 0.14f;
        light.motion = lightM;
        pieces.append(light);
        pieceAdded();
    }

    /* ---------------------------------------------------- vector decorator */
    /* Every mode gets the same vector-driven particle field. The profile
     * decides whether it is present (`OverlayAmount`) and supplies wind,
     * count, brightness, spread and trail length; the recipe only owns the
     * placement and the catalog palette. Voltage Idol already generates its
     * richer field above, so it is not duplicated here. */
    if (!cancelled && patchId != QLatin1String("electric-kpop")
        && vec[creative::vector::Dest::OverlayAmount] > 0.01f)
    {
        QImage subjectOnStage = detail::stageCanvas(stage);
        drawAt(subjectOnStage, s, subjectRect);
        const QImage stageMask = creative::maskFromAlpha(subjectOnStage);
        const QVector<QColor> basePalette{ input.accent, input.accentAlt,
                                           QColor(244, 252, 255) };

        if (preview && !stageMask.isNull())
        {
            /* Preview: one dynamic piece; compose() evaluates the graph and
             * redraws the field at the current pad frame. */
            CreativePiece live;
            live.name = QStringLiteral("Vector live");
            live.role = QStringLiteral("accent");
            live.dynamicVector = true;
            live.blendMode = BLEND_SCREEN;
            live.opacity = clamp01(
                0.30f + 0.55f * vec[creative::vector::Dest::OverlayAmount]);
            live.vectorOverlay.profileId = patchId;
            live.vectorOverlay.style = 0;
            live.vectorOverlay.mask = stageMask;
            live.vectorOverlay.palette = basePalette;
            live.vectorOverlay.subjectRect = subjectRect;
            live.vectorOverlay.stage = stage;
            live.vectorOverlay.seed = seed ^ 0x9E3779B9u;
            pieces.append(live);
        }
        else
        {
        const float hue =
            (vec[creative::vector::Dest::HueShift] - 0.5f) * 0.5f;
        QVector<QColor> palette;
        for (const QColor &c : basePalette)
            palette.append(detail::hueShifted(c, hue));
        const QPointF wind(
            (vec[creative::vector::Dest::ArcBendX] - 0.5f) * 2.0,
            (vec[creative::vector::Dest::ArcBendY] - 0.5f) * 2.0);
        const int count = qBound(
            18,
            int(24.0f + 220.0f * vec[creative::vector::Dest::ParticleCount]
                + 140.0f * input.frame.energy),
            420);
        const QImage particles = creative::sparkField(
            stageMask, stage, seed ^ 0x9E3779B9u, palette, count,
            0.35f + 0.65f * vec[creative::vector::Dest::ParticleBrightness],
            wind,
            0.15f + 0.75f * vec[creative::vector::Dest::ParticleSpread],
            (0.50f + 1.20f * vec[creative::vector::Dest::ParticleLength])
                * (0.55f
                   + 0.90f
                         * vec[creative::vector::Dest::ParticleSpeed]));
        if (!particles.isNull())
        {
            CreativeMotion vectorM;
            vectorM.opacityX = 0.18f;
            vectorM.opacityY = 0.18f;
            vectorM.opacityEnergy = 0.28f;
            vectorM.opacityHold = 0.18f;
            vectorM.dxX = 0.008f;
            vectorM.dyY = 0.008f;
            add(QStringLiteral("Vector discharge"),
                QStringLiteral("accent"), particles, full, BLEND_SCREEN,
                clamp01(0.30f
                        + 0.55f
                              * vec[creative::vector::Dest::OverlayAmount]),
                vectorM);
        }
        }
    }

    return pieces;
}

/* --------------------------------------------------------------- playback */

/* Bake the captured pad state into the piece geometry. Deterministic: the
 * frame is a value, never read from the live pad. Used by commits so the
 * layers store exactly what the user saw. */
inline QVector<CreativePiece> bake(QVector<CreativePiece> pieces,
                                   const CreativeFrame &frame,
                                   const QSize &stage)
{
    for (CreativePiece &p : pieces)
    {
        const CreativeMotion &m = p.motion;
        const float op = clamp01(p.opacity
                                 + m.opacityX * frame.x
                                 + m.opacityY * frame.y
                                 + m.opacityEnergy * frame.energy
                                 + m.opacityHold * (frame.hold ? 1.0f : 0.0f));
        const qreal dx = (m.dxX * (frame.x - 0.5f)
                          + m.dxEnergy * frame.energy) * stage.width();
        const qreal dy = (m.dyY * (frame.y - 0.5f)
                          + m.dyEnergy * frame.energy) * stage.height();
        const qreal sc = 1.0 + m.scaleY * (frame.y - 0.5f)
                         + m.scaleEnergy * frame.energy
                         + m.scaleHold * (frame.hold ? 1.0 : 0.0);
        const qreal rot = m.rotationEnergy * frame.energy
                          + m.rotationHold * (frame.hold ? 1.0 : 0.0);

        p.opacity = op;
        /* The bake has consumed the motion. Clearing it keeps the post-commit
         * compose() from applying the same translation/opacity a second time;
         * the committed layers already hold the baked values. */
        p.motion = CreativeMotion();
        if (qFuzzyIsNull(dx) && qFuzzyIsNull(dy) && qFuzzyIsNull(sc - 1.0)
            && qFuzzyIsNull(rot))
            continue;

        const QPointF pivot = p.pivot.isNull() ? p.rect.center() : p.pivot;
        if (qFuzzyIsNull(sc - 1.0) && qFuzzyIsNull(rot))
        {
            p.rect.translate(dx, dy);
            continue;
        }
        QTransform t;
        t.rotate(rot);
        t.scale(sc, sc);
        const QRectF local(0, 0, p.image.width(), p.image.height());
        const QRectF mapped = t.mapRect(local);
        const QSize outSize(qMax(1, int(std::ceil(mapped.width()))),
                            qMax(1, int(std::ceil(mapped.height()))));
        QImage out(outSize, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::transparent);
        QPainter painter(&out);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.translate(-mapped.x(), -mapped.y());
        painter.setTransform(t, true);
        painter.drawImage(0, 0, p.image);
        painter.end();
        const QPointF pivotOut = t.map(pivot) - mapped.topLeft();
        const QPointF target = pivot + QPointF(dx, dy);
        p.rect = QRectF(target.x() - pivotOut.x(), target.y() - pivotOut.y(),
                        outSize.width(), outSize.height());
        p.pivot = target;
        p.image = out;
    }
    return pieces;
}

namespace detail {

/* Evaluate the vector graph at the current frame and draw a preview-only
 * particle/bolt field. The mask is the subject-on-stage plane; the target is
 * the compose target, so this is a cheap interactive render, not a second
 * full-resolution recipe. */
inline QImage liveVectorOverlay(const CreativeVectorOverlay &spec,
                                const CreativeFrame &frame,
                                const QSize &target)
{
    if (spec.mask.isNull() || !target.isValid() || target.isEmpty())
        return {};
    const QSize source = spec.stage.isValid() && !spec.stage.isEmpty()
                             ? spec.stage
                             : spec.mask.size();
    const QImage mask = spec.mask.scaled(target, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
    const creative::vector::State state = creative::vector::evaluateForPatch(
        spec.profileId,
        creative::vector::Sources{ frame.x, frame.y, frame.energy,
                                   frame.hold ? 1.0f : 0.0f, frame.time });
    const float hue =
        spec.style == 1
            ? state[creative::vector::Dest::ParticleHue] * 0.15f
            : (state[creative::vector::Dest::HueShift] - 0.5f) * 0.5f;
    QVector<QColor> palette;
    for (const QColor &c : spec.palette)
        palette.append(hueShifted(c, hue));
    if (palette.isEmpty())
        palette.append(QColor(244, 252, 255));

    QImage out(target, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    /* Keep the individual arc colours; Plus here pushed overlapping arcs to
     * white before the final Screen blend. */
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    const float windScale = spec.style == 1 ? 2.2f : 2.0f;
    const QPointF wind(
        (state[creative::vector::Dest::ArcBendX] - 0.5f) * windScale,
        (state[creative::vector::Dest::ArcBendY] - 0.5f) * windScale);
    const float speed =
        0.55f + 0.90f * state[creative::vector::Dest::ParticleSpeed];
    const int count = qBound(
        spec.style == 1 ? 40 : 18,
        int((spec.style == 1 ? 60.0f : 24.0f)
            + (spec.style == 1 ? 260.0f : 220.0f)
                  * state[creative::vector::Dest::ParticleCount]
            + (spec.style == 1 ? 160.0f : 140.0f) * frame.energy),
        spec.style == 1 ? 480 : 420);
    p.drawImage(
        0, 0,
        creative::sparkField(
            mask, target, spec.seed, palette, count,
            (spec.style == 1 ? 0.45f : 0.35f)
                + (spec.style == 1 ? 0.55f : 0.65f)
                      * state[creative::vector::Dest::ParticleBrightness],
            wind, 0.15f + 0.75f * state[creative::vector::Dest::ParticleSpread],
            ((spec.style == 1 ? 0.55f : 0.50f)
             + (spec.style == 1 ? 1.10f : 1.20f)
                   * state[creative::vector::Dest::ParticleLength])
                * speed));

    if (spec.style == 1 && !spec.subjectRect.isEmpty() && source.width() > 0
        && source.height() > 0)
    {
        const qreal sx = double(target.width()) / source.width();
        const qreal sy = double(target.height()) / source.height();
        const QRectF sr(spec.subjectRect.x() * sx, spec.subjectRect.y() * sy,
                        spec.subjectRect.width() * sx,
                        spec.subjectRect.height() * sy);

        /* Use the actual silhouette edge, not the padded cutout bounds, so the
         * live bolts terminate where the baked bolts do. */
        int leftX = -1, rightX = -1, leftY = 0, rightY = 0;
        const int stride =
            qMax(1, qMin(mask.width(), mask.height()) / 180);
        for (int y = 0; y < mask.height(); y += stride)
        {
            const uchar *row = mask.constScanLine(y);
            for (int x = 0; x < mask.width(); x += stride)
            {
                if (row[x] < 40)
                    continue;
                if (leftX < 0 || x < leftX)
                {
                    leftX = x;
                    leftY = y;
                }
                if (x > rightX)
                {
                    rightX = x;
                    rightY = y;
                }
            }
        }
        const QPointF targetL =
            leftX >= 0
                ? QPointF(leftX + target.width() * 0.004, leftY)
                : QPointF(sr.left() + sr.width() * 0.14,
                          sr.top() + sr.height() * 0.40);
        const QPointF targetR =
            rightX >= 0
                ? QPointF(rightX - target.width() * 0.004, rightY)
                : QPointF(sr.right() - sr.width() * 0.10,
                          sr.top() + sr.height() * 0.56);
        const int boltCount = qBound(
            2,
            2 + int(std::lround(
                        4.0f * state[creative::vector::Dest::ArcCount]
                        + 3.0f * frame.energy)),
            5);
        creative::ArcParams arc;
        arc.core = QColor(255, 240, 250);
        arc.glowWidth =
            qMax(5.0f, float(target.height())
                          * (0.007f
                             + 0.010f
                                   * state[creative::vector::Dest::Glow]));
        arc.coreWidth = qMax(1.2f, float(target.height()) * 0.0026f);
        arc.displacement =
            0.08f + 0.18f * state[creative::vector::Dest::ArcJitter];
        arc.sparkCount =
            36 + int(90.0f
                     * state[creative::vector::Dest::ParticleCount]);
        for (int i = 0; i < boltCount; ++i)
        {
            const bool fromLeft = (i % 2) == 0;
            const QPointF from =
                fromLeft ? QPointF(-target.width() * 0.02,
                                   target.height() * (0.06 + 0.11 * i))
                         : QPointF(target.width() * 1.02,
                                   target.height() * (0.12 + 0.10 * i));
            const QPointF targetPoint =
                (fromLeft ? targetL : targetR)
                + QPointF((state[creative::vector::Dest::ArcBendX] - 0.5f)
                              * target.width() * 0.05 * (0.5 + 0.15 * i),
                          (state[creative::vector::Dest::ArcBendY] - 0.5f)
                              * target.height() * 0.05 * (0.5 + 0.15 * i));
            arc.halo = hueShifted(
                (i % 3 == 1)
                    ? QColor(253, 48, 199)
                    : ((i % 3 == 2) ? QColor(255, 0, 170)
                                    : QColor(0, 234, 255)),
                hue);
            p.drawImage(0, 0,
                        creative::electricArc(
                            target, from, targetPoint,
                            spec.seed + 31
                                + static_cast<quint32>(i) * 17,
                            arc));
        }
    }
    p.end();
    return out;
}

} // namespace detail

/* Compose pieces into a stage (or a bounded preview of a larger stage). The
 * pieces keep their global stage coordinates; a preview is a scaled render,
 * never a re-seeded/re-tiled recipe. */
inline QImage compose(const QVector<CreativePiece> &pieces,
                      const QSize &stage, const QSize &target,
                      const QColor &background, const CreativeFrame &frame)
{
    if (!target.isValid() || target.isEmpty())
        return {};
    QImage out(target, QImage::Format_ARGB32_Premultiplied);
    out.fill(background);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (stage != target && stage.isValid() && !stage.isEmpty())
        p.scale(double(target.width()) / double(stage.width()),
                double(target.height()) / double(stage.height()));
    const int count = pieces.size();
    for (int i = 0; i < count; ++i)
    {
        const CreativePiece &piece = pieces.at(i);
        if (piece.image.isNull() && !piece.dynamicVector)
            continue;
        const CreativeMotion &m = piece.motion;
        float op = piece.opacity
                   + m.opacityX * frame.x
                   + m.opacityY * frame.y
                   + m.opacityEnergy * frame.energy
                   + m.opacityHold * (frame.hold ? 1.0f : 0.0f);
        if (piece.role == QLatin1String("rim")
            || piece.role == QLatin1String("accent"))
            op *= 1.0f + 0.035f * float(std::sin(frame.time * 1.8 + i * 1.3));
        op = clamp01(op);
        if (op <= 0.002f)
            continue;

        if (piece.dynamicVector)
        {
            /* The painter is scaled to the preview target; the live field is
             * generated at the target and composited in target pixels. */
            const QImage overlay = detail::liveVectorOverlay(
                piece.vectorOverlay, frame, target);
            if (!overlay.isNull())
            {
                p.save();
                p.resetTransform();
                p.setOpacity(op);
                p.setCompositionMode(toCompositionMode(piece.blendMode));
                p.drawImage(0, 0, overlay);
                p.restore();
            }
            continue;
        }

        const qreal dx = (m.dxX * (frame.x - 0.5f)
                          + m.dxEnergy * frame.energy) * stage.width();
        const qreal dy = (m.dyY * (frame.y - 0.5f)
                          + m.dyEnergy * frame.energy) * stage.height();
        const qreal sc = 1.0 + m.scaleY * (frame.y - 0.5f)
                         + m.scaleEnergy * frame.energy
                         + m.scaleHold * (frame.hold ? 1.0 : 0.0);
        const qreal rot = m.rotationEnergy * frame.energy
                          + m.rotationHold * (frame.hold ? 1.0 : 0.0);

        p.setOpacity(op);
        p.setCompositionMode(toCompositionMode(piece.blendMode));
        p.save();
        const QPointF pivot = piece.pivot.isNull() ? piece.rect.center()
                                                   : piece.pivot;
        p.translate(pivot.x() + dx, pivot.y() + dy);
        if (!qFuzzyIsNull(rot))
            p.rotate(rot);
        if (!qFuzzyIsNull(sc - 1.0))
            p.scale(sc, sc);
        p.translate(-pivot.x(), -pivot.y());
        p.drawImage(piece.rect, piece.image);
        p.restore();
    }
    p.end();
    return out;
}

} /* namespace recipe */
} /* namespace creative */
} /* namespace paintbrush */

#endif /* PB_CREATIVE_RECIPE_HPP */

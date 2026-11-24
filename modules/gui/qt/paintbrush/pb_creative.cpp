/*****************************************************************************
 * pb_creative.cpp : NAI Paintbrush — Creative Play Pad / patch synthesis
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_creative.hpp"
#include "pb_controller.hpp"
#include "pb_document.hpp"
#include "pb_creative_art.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QTransform>
#include <QtMath>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace paintbrush {

namespace {

constexpr double kTau = 6.283185307179586;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

QImage tintPreserveAlpha(const QImage &img, const QColor &color)
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

void fadeHorizontally(QImage &img, bool leftStrong)
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

QRectF scaleRectAbout(const QRectF &r, qreal factor, qreal dx, qreal dy)
{
    const QPointF c = r.center();
    const QSizeF s = r.size() * factor;
    return QRectF(c.x() - s.width() / 2.0 + dx,
                  c.y() - s.height() / 2.0 + dy,
                  s.width(), s.height());
}

QImage stageCanvas(const QSize &size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

void drawScaled(QImage &dst, const QImage &src, const QRectF &rect)
{
    if (src.isNull() || rect.isEmpty())
        return;
    QPainter p(&dst);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(rect, src);
}

QString qcolorHex(const QColor &c)
{
    return c.isValid() ? c.name(QColor::HexRgb) : QString();
}

QString suggestionKindFor(const QString &patchId)
{
    if (patchId == QLatin1String("sticker-forge")
        || patchId == QLatin1String("spark-storm"))
        return QStringLiteral("sparkle");
    if (patchId == QLatin1String("nocturne")
        || patchId == QLatin1String("kaleido")
        || patchId == QLatin1String("stage-mode"))
        return QStringLiteral("halo");
    if (patchId == QLatin1String("ink-creature")
        || patchId == QLatin1String("auto-splatter"))
        return QStringLiteral("splat");
    return QStringLiteral("ribbon");
}

QColor stageLightColor(const QString &patchId)
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
        || patchId == QLatin1String("stage-mode"))
        return QColor(255, 236, 196);
    if (patchId == QLatin1String("nocturne")
        || patchId == QLatin1String("holo-fashion"))
        return QColor(205, 160, 255);
    if (patchId == QLatin1String("ink-creature"))
        return QColor(255, 230, 180);
    if (patchId == QLatin1String("auto-splatter"))
        return QColor(255, 194, 164);
    if (patchId == QLatin1String("spark-storm"))
        return QColor(255, 216, 142);
    return QColor(255, 244, 230);
}

} /* namespace */

/* ------------------------------------------------------------- image provider */

CreativePreviewProvider::CreativePreviewProvider(PaintbrushCreative *engine)
    : QQuickImageProvider(QQuickImageProvider::Image), m_engine(engine)
{
}

QImage CreativePreviewProvider::requestImage(const QString &id, QSize *size,
                                             const QSize &requestedSize)
{
    Q_UNUSED(id);
    QImage img = m_engine ? m_engine->previewImage() : QImage();
    if (img.isNull())
        return {};
    if (size)
        *size = img.size();
    if (requestedSize.isValid() && requestedSize.width() > 0
        && requestedSize.height() > 0
        && requestedSize != img.size())
        img = img.scaled(requestedSize, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
    return img;
}

/* --------------------------------------------------------------- the catalog */

const QVector<PaintbrushCreative::PatchDef> &PaintbrushCreative::catalog()
{
    static const QVector<PatchDef> defs = {
        { QStringLiteral("neon-idol"), QStringLiteral("Neon Idol"),
          QStringLiteral("Electric stage. Idol light."),
          QStringLiteral("Stage"),
          QStringLiteral("Drag for heat, lift for spotlight, hold for bloom."),
          QStringLiteral("Add one hand-drawn sparkle where the light hits."),
          QColor(90, 235, 255), QColor(255, 80, 160), QColor(10, 10, 24) },
        { QStringLiteral("electric-kpop"), QStringLiteral("Voltage Idol"),
          QStringLiteral("Main-character voltage."),
          QStringLiteral("Stage"),
          QStringLiteral("Drag for voltage, lift for the drop, hold for the bloom."),
          QStringLiteral("Draw one star where the arc lands."),
          QColor(0, 234, 255), QColor(253, 48, 199), QColor(8, 3, 18) },
        { QStringLiteral("sticker-forge"), QStringLiteral("Sticker Forge"),
          QStringLiteral("Die-cut your own merch."),
          QStringLiteral("Sticker"),
          QStringLiteral("Drag for the cut line, lift for gloss, hold for sparkle."),
          QStringLiteral("Add a tiny star sticker by the shoulder."),
          QColor(255, 210, 63), QColor(255, 59, 78), QColor(22, 20, 24) },
        { QStringLiteral("pastel-stage"), QStringLiteral("Pastel Stage"),
          QStringLiteral("Soft light. Main-character energy."),
          QStringLiteral("Soft"),
          QStringLiteral("Drag through the palette, lift for air, hold for glow."),
          QStringLiteral("Add one confetti ribbon that follows the pose."),
          QColor(255, 199, 230), QColor(199, 187, 255), QColor(90, 74, 107) },
        { QStringLiteral("nocturne"), QStringLiteral("Nocturne"),
          QStringLiteral("Midnight garden, luminous."),
          QStringLiteral("Fantasy"),
          QStringLiteral("Drag between violet and teal, lift for moonlight."),
          QStringLiteral("Add one butterfly resting on the silhouette edge."),
          QColor(122, 77, 255), QColor(75, 224, 208), QColor(10, 10, 22) },
        { QStringLiteral("holo-fashion"), QStringLiteral("Prism Muse"),
          QStringLiteral("Liquid chrome, zero filter."),
          QStringLiteral("Material"),
          QStringLiteral("Drag across the spectrum, lift for chrome, hold for the sheen."),
          QStringLiteral("Give the next pose a different colourway."),
          QColor(255, 61, 218), QColor(0, 229, 255), QColor(4, 16, 26) },
        { QStringLiteral("ink-creature"), QStringLiteral("Ink Creature"),
          QStringLiteral("Your silhouette grew a friend."),
          QStringLiteral("Mischief"),
          QStringLiteral("Drag for ink density, lift for wash, hold to wake it."),
          QStringLiteral("Add one ink splat that breaks the outline."),
          QColor(20, 18, 16), QColor(214, 69, 69), QColor(246, 240, 228) },
        { QStringLiteral("spark-storm"), QStringLiteral("Spark Storm"),
          QStringLiteral("A storm that sparkles."),
          QStringLiteral("Energy"),
          QStringLiteral("Drag to spin the storm, lift for density, hold for gold."),
          QStringLiteral("Add one extra spark ring around the waist."),
          QColor(255, 211, 122), QColor(255, 122, 200), QColor(7, 6, 13) },
        { QStringLiteral("kaleido"), QStringLiteral("Kaleido"),
          QStringLiteral("One pose, infinite angles."),
          QStringLiteral("Weird"),
          QStringLiteral("Drag for symmetry, lift for zoom, hold for bloom."),
          QStringLiteral("Add one mirrored echo in the empty corner."),
          QColor(88, 224, 255), QColor(255, 94, 219), QColor(11, 16, 48) },
        { QStringLiteral("motion-echo"), QStringLiteral("Motion Echo"),
          QStringLiteral("Every move leaves a ghost."),
          QStringLiteral("Motion"),
          QStringLiteral("Drag for spacing, lift for fade, hold to freeze."),
          QStringLiteral("Add one star trail behind the last echo."),
          QColor(78, 123, 255), QColor(255, 111, 174), QColor(18, 19, 38) },
        { QStringLiteral("scene-forge"), QStringLiteral("Scene Forge"),
          QStringLiteral("Build a little world."),
          QStringLiteral("Scene"),
          QStringLiteral("Drag for parallax, lift for spotlight, hold to open the curtains."),
          QStringLiteral("Add one paper star above the spotlight."),
          QColor(242, 181, 68), QColor(46, 139, 122), QColor(36, 27, 20) },
        { QStringLiteral("auto-splatter"), QStringLiteral("Auto Splatter"),
          QStringLiteral("Print it loud."),
          QStringLiteral("Street"),
          QStringLiteral("Drag for spray, lift for contrast, hold for drips."),
          QStringLiteral("Add one paint drip that cuts the outline."),
          QColor(255, 77, 46), QColor(18, 16, 14), QColor(244, 237, 226) },
        { QStringLiteral("stage-mode"), QStringLiteral("Stage Door"),
          QStringLiteral("House lights down."),
          QStringLiteral("Stage"),
          QStringLiteral("Drag the spotlight, lift the floor, hold for the reveal."),
          QStringLiteral("Add one halo over the spotlight."),
          QColor(255, 211, 122), QColor(184, 51, 106), QColor(8, 6, 12) },
    };
    return defs;
}

const PaintbrushCreative::PatchDef *PaintbrushCreative::patchDef(const QString &id) const
{
    for (const PatchDef &d : catalog())
        if (d.id == id)
            return &d;
    return nullptr;
}

QString PaintbrushCreative::defaultPatchId() const
{
    return QStringLiteral("neon-idol");
}

/* --------------------------------------------------------------- lifecycle */

PaintbrushCreative::PaintbrushCreative(PaintbrushController *controller,
                                       QObject *parent)
    : QObject(parent), m_controller(controller)
{
    m_patchId = defaultPatchId();
    m_status = QStringLiteral("Pick a cutout, then play.");
    m_idleTimer.setInterval(50);
    connect(&m_idleTimer, &QTimer::timeout, this, &PaintbrushCreative::onIdleTick);
    m_replayTimer.setInterval(33);
    connect(&m_replayTimer, &QTimer::timeout, this, &PaintbrushCreative::onReplayTick);
    m_recordUiTimer.setInterval(250);
    connect(&m_recordUiTimer, &QTimer::timeout, this,
            &PaintbrushCreative::stateChanged);
    m_seed = QRandomGenerator::global()->generate();
    if (m_seed == 0)
        m_seed = 1;
}

PaintbrushCreative::~PaintbrushCreative() = default;

QVariantList PaintbrushCreative::patchList() const
{
    QVariantList list;
    for (const PatchDef &d : catalog())
    {
        QVariantMap m;
        m[QStringLiteral("id")] = d.id;
        m[QStringLiteral("name")] = d.name;
        m[QStringLiteral("tagline")] = d.tagline;
        m[QStringLiteral("family")] = d.family;
        m[QStringLiteral("hint")] = d.hint;
        m[QStringLiteral("accent")] = qcolorHex(d.accent);
        m[QStringLiteral("secondary")] = qcolorHex(d.secondary);
        m[QStringLiteral("ink")] = qcolorHex(d.ink);
        list.append(m);
    }
    return list;
}

QVariantMap PaintbrushCreative::activePatch() const
{
    QVariantMap m;
    if (const PatchDef *d = patchDef(m_patchId))
    {
        m[QStringLiteral("id")] = d->id;
        m[QStringLiteral("name")] = d->name;
        m[QStringLiteral("tagline")] = d->tagline;
        m[QStringLiteral("family")] = d->family;
        m[QStringLiteral("hint")] = d->hint;
        m[QStringLiteral("accent")] = qcolorHex(d->accent);
        m[QStringLiteral("secondary")] = qcolorHex(d->secondary);
        m[QStringLiteral("ink")] = qcolorHex(d->ink);
    }
    return m;
}

QString PaintbrushCreative::activePatchId() const
{
    return m_patchId;
}

void PaintbrushCreative::setOpen(bool open)
{
    if (m_open == open)
        return;
    m_open = open;
    if (open)
    {
        startIdle();
        if (!hasSubject())
            setStatus(QStringLiteral("Pick a cutout first — or Keep a moment."));
    }
    else
    {
        stopIdle();
        if (m_replayTimer.isActive())
        {
            m_replayTimer.stop();
            emit stateChanged();
        }
    }
    emit openChanged();
}

void PaintbrushCreative::close()
{
    setOpen(false);
}

void PaintbrushCreative::startIdle()
{
    if (!m_idleTimer.isActive())
        m_idleTimer.start();
}

void PaintbrushCreative::stopIdle()
{
    m_idleTimer.stop();
}

void PaintbrushCreative::setStatus(const QString &text)
{
    if (m_status == text)
        return;
    m_status = text;
    emit stateChanged();
}

bool PaintbrushCreative::canCommit() const
{
    return hasSubject() && !m_patchId.isEmpty() && !m_busy && !m_committed;
}

double PaintbrushCreative::recordSeconds() const
{
    if (m_recording)
        return double(m_recordClock.elapsed()) / 1000.0;
    return m_samples.isEmpty() ? 0.0 : double(m_samples.last().tMs) / 1000.0;
}

double PaintbrushCreative::replayProgress() const
{
    if (!hasRecording())
        return 0.0;
    const qint64 dur = qMax<qint64>(1, m_samples.last().tMs);
    if (!m_replayTimer.isActive())
        return 0.0;
    return double(m_replayClock.elapsed() % dur) / double(dur);
}

QImage PaintbrushCreative::previewImage() const
{
    QMutexLocker locker(&m_previewMutex);
    return m_preview;
}

/* ------------------------------------------------------------------ subject */

bool PaintbrushCreative::adoptSubject(const QImage &cutout,
                                      const QRectF &canvasRect,
                                      int sourceLayerId, const QString &label,
                                      const QString &missingMessage)
{
    if (cutout.isNull())
    {
        setStatus(missingMessage);
        return false;
    }
    m_subjectNative = cutout.convertToFormat(QImage::Format_ARGB32);
    m_subjectCanvasRect = canvasRect;
    m_sourceLayerId = sourceLayerId;
    m_sourceHarvestId = -1;
    m_subjectLabel = label;
    m_seed = QRandomGenerator::global()->generate();
    if (m_seed == 0)
        m_seed = 1;
    m_committed = false;
    if (m_suggestionVisible || !m_suggestionText.isEmpty())
    {
        m_suggestionVisible = false;
        m_suggestionText.clear();
        emit suggestionChanged();
    }
    rebuildPreview();
    refreshHistory();
    emit subjectChanged();
    emit stateChanged();
    return true;
}

void PaintbrushCreative::clearSubject()
{
    m_subjectNative = QImage();
    m_subject = QImage();
    m_subjectCanvasRect = QRectF();
    m_subjectLabel.clear();
    m_sourceLayerId = -1;
    m_sourceHarvestId = -1;
    m_resumeGroup.clear();
    m_pieces.clear();
    {
        QMutexLocker locker(&m_previewMutex);
        m_preview = QImage();
    }
    ++m_previewRevision;
    emit subjectChanged();
    emit previewChanged();
    emit stateChanged();
}

void PaintbrushCreative::openFromHarvest(int harvestId)
{
    QRectF rect;
    QString label;
    const QImage cutout = m_controller
                              ? m_controller->creativePadHarvest(harvestId, &rect, &label)
                              : QImage();
    m_patchId = defaultPatchId();
    m_chooserOpen = true;
    m_resumeGroup.clear();
    if (!adoptSubject(cutout, rect, -1, label,
                      QStringLiteral("That kept moment has no cutout yet.")))
    {
        setOpen(true);
        return;
    }
    m_sourceHarvestId = harvestId;
    m_committed = false;
    setOpen(true);
    setStatus(QStringLiteral("Start with Neon Idol or Sticker Forge."));
    emit patchChanged();
}

void PaintbrushCreative::openForSelection()
{
    QRectF rect;
    QString label;
    int layerId = -1;
    const QImage cutout = m_controller
                              ? m_controller->creativePadSubject(&layerId, &rect, &label)
                              : QImage();
    m_chooserOpen = false;
    m_resumeGroup.clear();
    if (!adoptSubject(cutout, rect, layerId, label,
                      QStringLiteral("Pick a cutout first — or Keep a moment.")))
    {
        setOpen(true);
        return;
    }
    m_committed = false;
    setOpen(true);
    setStatus(QStringLiteral("Drag the pad. The stage is live."));
    emit patchChanged();
}

/* ------------------------------------------------------------------- preview */

void PaintbrushCreative::rebuildPreview()
{
    if (m_subjectNative.isNull() || !m_controller)
    {
        m_pieces.clear();
        return;
    }
    const QSize canvas = m_controller->creativePadCanvasSize();
    if (!canvas.isValid() || canvas.isEmpty())
        return;

    /* Fit the preview inside a comfortable panel size, preserving the real
     * document aspect so the preview is a scaled render of the commit. */
    const qreal fit = qMin(560.0 / canvas.width(), 680.0 / canvas.height());
    const QSize previewSize(qMax(160, int(std::lround(canvas.width() * fit))),
                            qMax(160, int(std::lround(canvas.height() * fit))));
    m_previewSize = previewSize;

    const qreal sx = double(previewSize.width()) / double(canvas.width());
    const qreal sy = double(previewSize.height()) / double(canvas.height());
    const QRectF previewRect(m_subjectCanvasRect.x() * sx,
                             m_subjectCanvasRect.y() * sy,
                             m_subjectCanvasRect.width() * sx,
                             m_subjectCanvasRect.height() * sy);
    m_previewSubjectRect = previewRect;
    const QSize subjSize(qMax(1, int(std::lround(previewRect.width()))),
                         qMax(1, int(std::lround(previewRect.height()))));
    m_subject = m_subjectNative.scaled(subjSize, Qt::IgnoreAspectRatio,
                                       Qt::SmoothTransformation);

    m_pieces = buildPatch(m_patchId, m_subject, previewRect, previewSize, m_seed);
    recompose();
}

void PaintbrushCreative::recompose()
{
    if (m_previewSize.isEmpty())
        return;
    QElapsedTimer frameClock;
    frameClock.start();

    QImage frame;
    if (m_compare)
    {
        /* Before: the untouched source on a neutral studio field. */
        frame = QImage(m_previewSize, QImage::Format_ARGB32_Premultiplied);
        frame.fill(QColor(12, 13, 20));
        QPainter p(&frame);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QLinearGradient bg(0, 0, 0, frame.height());
        bg.setColorAt(0.0, QColor(26, 28, 40));
        bg.setColorAt(1.0, QColor(8, 9, 14));
        p.fillRect(frame.rect(), bg);
        if (!m_subject.isNull())
        {
            const QRectF r = m_previewSubjectRect;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 70));
            p.drawEllipse(QPointF(r.center().x(), r.bottom() + r.height() * 0.03),
                          r.width() * 0.38, r.height() * 0.035);
            p.drawImage(r, m_subject);
        }
        p.end();
    }
    else
    {
        frame = composeStage(m_pieces, m_previewSize, QColor(12, 12, 18));
    }

    {
        QMutexLocker locker(&m_previewMutex);
        m_preview = frame;
    }
    ++m_previewRevision;

    m_previewTimes.append(double(frameClock.nsecsElapsed()) / 1.0e6);
    while (m_previewTimes.size() > 120)
        m_previewTimes.removeFirst();
    if (!m_previewTimes.isEmpty())
    {
        QVector<double> sorted = m_previewTimes;
        std::sort(sorted.begin(), sorted.end());
        const int idx = qBound(0, int(std::ceil(sorted.size() * 0.95)) - 1,
                               sorted.size() - 1);
        m_previewP95Ms = sorted.at(idx);
    }
    emit previewChanged();
    emit metricsChanged();
}

void PaintbrushCreative::setCompare(bool compare)
{
    if (m_compare == compare)
        return;
    m_compare = compare;
    recompose();
    emit compareChanged();
}

void PaintbrushCreative::onIdleTick()
{
    if (!m_open || m_subject.isNull())
        return;
    m_frame.time += 0.05f;
    /* idle energy decays so the stage settles after a gesture */
    m_frame.energy *= 0.965f;
    if (m_frame.energy < 0.002f)
        m_frame.energy = 0.0f;
    recompose();
    if (!m_recording && !m_replayTimer.isActive())
        emit padChanged();
}

QVector<CreativePiece> PaintbrushCreative::bakeFrame(QVector<CreativePiece> pieces,
                                                    const QSize &stage) const
{
    for (CreativePiece &p : pieces)
    {
        const CreativeMotion &m = p.motion;
        const float op = clamp01(p.opacity
                                 + m.opacityX * m_frame.x
                                 + m.opacityY * m_frame.y
                                 + m.opacityEnergy * m_frame.energy
                                 + m.opacityHold * (m_frame.hold ? 1.0f : 0.0f));
        const qreal dx = (m.dxX * (m_frame.x - 0.5f)
                          + m.dxEnergy * m_frame.energy
                          + m.dxHold * (m_frame.hold ? 1.0f : 0.0f))
                         * stage.width();
        const qreal dy = (m.dyY * (m_frame.y - 0.5f)
                          + m.dyEnergy * m_frame.energy
                          + m.dyHold * (m_frame.hold ? 1.0f : 0.0f))
                         * stage.height();
        const qreal sc = 1.0 + m.scaleY * (m_frame.y - 0.5f)
                         + m.scaleEnergy * m_frame.energy
                         + m.scaleHold * (m_frame.hold ? 1.0 : 0.0);
        const qreal rot = m.rotationEnergy * m_frame.energy
                          + m.rotationHold * (m_frame.hold ? 1.0 : 0.0);

        p.opacity = op;
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

void PaintbrushCreative::beginPad()
{
    if (!hasSubject())
        return;
    setStatus(QStringLiteral("Play."));
}

void PaintbrushCreative::movePad(double x, double y, double energy, bool hold)
{
    if (!hasSubject())
        return;
    const float nx = clamp01(float(x));
    const float ny = clamp01(float(y));
    const float ne = clamp01(float(energy));
    /* low-pass the energy so a fast flick blooms instead of flickering */
    m_frame.energy = m_frame.energy * 0.55f + ne * 0.45f;
    if (qAbs(nx - m_frame.x) < 0.0015f && qAbs(ny - m_frame.y) < 0.0015f
        && qAbs(ne - m_frame.energy) < 0.002f && hold == m_frame.hold)
        return;
    m_frame.x = nx;
    m_frame.y = ny;
    m_frame.hold = hold;
    if (m_recording)
    {
        CreativeSample s;
        s.x = nx;
        s.y = ny;
        s.energy = m_frame.energy;
        s.hold = hold;
        s.tMs = m_recordClock.elapsed();
        m_samples.append(s);
    }
    recompose();
    emit padChanged();
}

void PaintbrushCreative::endPad()
{
    /* The last frame is the committed look; nothing to do but let the idle
     * shimmer carry on. */
    emit padChanged();
}

void PaintbrushCreative::setHold(bool hold)
{
    if (m_frame.hold == hold)
        return;
    m_frame.hold = hold;
    if (m_recording)
    {
        CreativeSample s;
        s.x = m_frame.x;
        s.y = m_frame.y;
        s.energy = m_frame.energy;
        s.hold = hold;
        s.tMs = m_recordClock.elapsed();
        m_samples.append(s);
    }
    recompose();
    emit padChanged();
}

/* ---------------------------------------------------------------- recording */

void PaintbrushCreative::toggleRecord()
{
    if (!hasSubject())
        return;
    if (m_recording)
    {
        m_recording = false;
        m_recordUiTimer.stop();
        if (m_samples.size() < 2)
            m_samples.clear();
        setStatus(m_samples.isEmpty()
                      ? QStringLiteral("That move was too short. Try again.")
                      : QStringLiteral("Saved. Loop it, or make it real."));
    }
    else
    {
        if (m_replayTimer.isActive())
            m_replayTimer.stop();
        m_samples.clear();
        m_recordClock.start();
        m_recording = true;
        m_recordUiTimer.start();
        CreativeSample s;
        s.x = m_frame.x;
        s.y = m_frame.y;
        s.energy = m_frame.energy;
        s.hold = m_frame.hold;
        s.tMs = 0;
        m_samples.append(s);
        setStatus(QStringLiteral("Recording. Drag the pad, then stop."));
    }
    emit stateChanged();
}

void PaintbrushCreative::toggleReplay()
{
    if (m_replayTimer.isActive())
    {
        m_replayTimer.stop();
        setStatus(QStringLiteral("Loop stopped."));
        emit stateChanged();
        return;
    }
    if (m_samples.size() < 2)
    {
        setStatus(QStringLiteral("Record a move first."));
        return;
    }
    if (m_recording)
    {
        m_recording = false;
    }
    m_replayClock.start();
    m_replayTimer.start();
    setStatus(QStringLiteral("Looping your move."));
    emit stateChanged();
}

void PaintbrushCreative::onReplayTick()
{
    if (m_samples.size() < 2)
    {
        m_replayTimer.stop();
        emit stateChanged();
        return;
    }
    const qint64 duration = qMax<qint64>(1, m_samples.last().tMs);
    const qint64 t = m_replayClock.elapsed() % duration;
    int i = 1;
    while (i < m_samples.size() && m_samples.at(i).tMs < t)
        ++i;
    const CreativeSample &a = m_samples.at(qMax(0, i - 1));
    const CreativeSample &b = m_samples.at(qMin(m_samples.size() - 1, i));
    const qint64 span = qMax<qint64>(1, b.tMs - a.tMs);
    const float f = float(double(t - a.tMs) / double(span));
    m_frame.x = a.x + (b.x - a.x) * f;
    m_frame.y = a.y + (b.y - a.y) * f;
    m_frame.energy = a.energy + (b.energy - a.energy) * f;
    m_frame.hold = f < 0.5f ? a.hold : b.hold;
    recompose();
    emit padChanged();
    emit stateChanged(); /* progress + elapsed */
}

/* ------------------------------------------------------------------- actions */

void PaintbrushCreative::selectPatch(const QString &id)
{
    if (!patchDef(id))
        return;
    if (m_patchId == id && !m_chooserOpen)
        return;
    m_patchId = id;
    m_chooserOpen = false;
    m_committed = false;
    if (m_suggestionVisible || !m_suggestionText.isEmpty())
    {
        m_suggestionVisible = false;
        m_suggestionText.clear();
        emit suggestionChanged();
    }
    if (!m_subjectNative.isNull())
        rebuildPreview();
    if (const PatchDef *d = patchDef(id))
        setStatus(d->hint);
    emit patchChanged();
    emit stateChanged();
}

void PaintbrushCreative::reseed()
{
    m_seed = QRandomGenerator::global()->generate();
    if (m_seed == 0)
        m_seed = 1;
    m_committed = false;
    if (!m_subjectNative.isNull())
        rebuildPreview();
    setStatus(QStringLiteral("New seed. Same pose, new world."));
    emit stateChanged();
}

void PaintbrushCreative::surprise()
{
    const QVector<PatchDef> &all = catalog();
    if (all.isEmpty())
        return;
    QString next = m_patchId;
    while (next == m_patchId && all.size() > 1)
        next = all.at(int(QRandomGenerator::global()->bounded(all.size()))).id;
    m_patchId = next;
    m_chooserOpen = false;
    m_seed = QRandomGenerator::global()->generate();
    if (m_seed == 0)
        m_seed = 1;
    m_frame.x = 0.2f + float(QRandomGenerator::global()->bounded(60)) / 100.0f;
    m_frame.y = 0.2f + float(QRandomGenerator::global()->bounded(60)) / 100.0f;
    m_committed = false;
    if (!m_subjectNative.isNull())
        rebuildPreview();
    if (const PatchDef *d = patchDef(next))
        setStatus(QStringLiteral("Surprise: %1. %2").arg(d->name, d->tagline));
    emit patchChanged();
    emit padChanged();
    emit stateChanged();
}

QString PaintbrushCreative::commit()
{
    if (!hasSubject() || !m_controller)
        return QStringLiteral("Pick a cutout first.");
    if (m_patchId.isEmpty())
        return QStringLiteral("Choose a patch first.");
    if (m_committed)
        return QStringLiteral("Already committed. Pick a patch to start a new run.");
    const PatchDef *def = patchDef(m_patchId);
    if (!def)
        return QStringLiteral("Unknown patch.");

    if (m_replayTimer.isActive())
        m_replayTimer.stop();
    if (m_recording)
        m_recording = false;

    const QSize canvas = m_controller->creativePadCanvasSize();
    if (!canvas.isValid() || canvas.isEmpty())
        return QStringLiteral("Open a canvas first.");

    m_busy = true;
    emit stateChanged();
    QElapsedTimer commitClock;
    commitClock.start();
    const QString group = QStringLiteral("look-%1")
                              .arg(QDateTime::currentMSecsSinceEpoch());

    /* The build is capped so a 4K/8K canvas cannot stall the UI while the
     * look is synthesized. Pieces are authored at the capped stage and mapped
     * onto their real canvas rectangles below; the live preview already runs
     * on a smaller stage, so its geometry is unchanged. */
    constexpr int kMaxBuildSide = 1600;
    const qreal buildScale =
        qMin(1.0, double(kMaxBuildSide)
                      / double(qMax(canvas.width(), canvas.height())));
    const QSize buildStage(
        qMax(1, int(std::lround(canvas.width() * buildScale))),
        qMax(1, int(std::lround(canvas.height() * buildScale))));
    const QRectF buildRect(
        m_subjectCanvasRect.x() * buildScale,
        m_subjectCanvasRect.y() * buildScale,
        m_subjectCanvasRect.width() * buildScale,
        m_subjectCanvasRect.height() * buildScale);

    QImage buildSubject = m_subjectNative;
    const QSize subjectWant(
        qMax(1, int(std::lround(buildRect.width() * 1.25))),
        qMax(1, int(std::lround(buildRect.height() * 1.25))));
    if (buildSubject.width() > subjectWant.width()
        || buildSubject.height() > subjectWant.height())
        buildSubject = buildSubject.scaled(subjectWant, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);

    QVector<CreativePiece> pieces =
        buildPatch(m_patchId, buildSubject, buildRect, buildStage, m_seed);
    /* Motion offsets are proportional to the stage, so bake on the build stage
     * first and map the baked rectangles onto the real canvas afterwards. */
    pieces = bakeFrame(pieces, buildStage);
    if (buildScale < 1.0)
    {
        for (CreativePiece &p : pieces)
        {
            p.rect = QRectF(p.rect.x() / buildScale, p.rect.y() / buildScale,
                            p.rect.width() / buildScale,
                            p.rect.height() / buildScale);
            if (!p.pivot.isNull())
                p.pivot = QPointF(p.pivot.x() / buildScale,
                                  p.pivot.y() / buildScale);
        }
    }

    QList<CanvasLayer> layers;
    for (const CreativePiece &p : pieces)
    {
        if (p.image.isNull())
            continue;
        QPoint offset;
        QImage trimmed = creative::trimTransparent(p.image, &offset);
        if (trimmed.isNull())
            continue;
        /* The piece image can be authored below the canvas resolution (capped
         * build stage) or above it (native subject), so map it onto its real
         * canvas rectangle instead of assuming 1 image px == 1 canvas unit. */
        const qreal pxScale = p.rect.width() / qMax(1, p.image.width());
        const QPointF target =
            p.rect.topLeft() + QPointF(offset) * pxScale;
        CanvasLayer layer =
            CanvasLayer::createImage(trimmed, QPointF(0, 0), p.name);
        layer.setTransform(QTransform(pxScale, 0.0, 0.0, pxScale, target.x(),
                                      target.y()));
        layer.type = LAYER_IMAGE;
        layer.opacity = p.opacity;
        layer.blendMode = p.blendMode;
        layer.metadata[QStringLiteral("creative.patch")] = def->id;
        layer.metadata[QStringLiteral("creative.patchName")] = def->name;
        layer.metadata[QStringLiteral("creative.role")] = p.role;
        layer.metadata[QStringLiteral("creative.seed")] = qint64(m_seed);
        layer.metadata[QStringLiteral("creative.group")] = group;
        layer.metadata[QStringLiteral("creative.sourceLayerId")] = m_sourceLayerId;
        layer.metadata[QStringLiteral("creative.sourceHarvestId")] = m_sourceHarvestId;
        QVariantMap params;
        params[QStringLiteral("x")] = double(m_frame.x);
        params[QStringLiteral("y")] = double(m_frame.y);
        params[QStringLiteral("energy")] = double(m_frame.energy);
        params[QStringLiteral("hold")] = m_frame.hold;
        layer.metadata[QStringLiteral("creative.params")] = params;
        layers.append(layer);
    }
    if (layers.isEmpty())
    {
        m_busy = false;
        setStatus(QStringLiteral("Nothing to commit — try another patch."));
        return QStringLiteral("The patch produced no layers.");
    }

    int hideLayerId = -1;
    if (m_sourceLayerId > 0)
        hideLayerId = m_sourceLayerId;

    const QString error = m_controller->commitCreativeLayers(
        layers, hideLayerId, QStringLiteral("Create: %1").arg(def->name),
        m_resumeGroup);
    m_busy = false;
    if (!error.isEmpty())
    {
        setStatus(error);
        emit stateChanged();
        return error;
    }

    m_lastCommitMs = double(commitClock.nsecsElapsed()) / 1.0e6;
    m_committed = true;
    m_resumeGroup.clear();
    m_suggestionKind = suggestionKindFor(def->id);
    m_suggestionText = def->suggestion;
    m_suggestionVisible = !m_suggestionText.isEmpty();
    refreshHistory();
    setStatus(QStringLiteral("Committed. Every piece is an editable layer — one undo."));
    emit suggestionChanged();
    emit metricsChanged();
    emit stateChanged();
    return QString();
}

void PaintbrushCreative::acceptSuggestion()
{
    if (!m_suggestionVisible || m_subjectCanvasRect.isEmpty() || !m_controller)
        return;
    const PatchDef *def = patchDef(m_patchId);
    const QColor accent = def ? def->accent : QColor(255, 226, 120);
    QString done;
    if (m_suggestionKind == QLatin1String("ribbon")
        || m_suggestionKind == QLatin1String("halo")
        || m_suggestionKind == QLatin1String("splat"))
    {
        const int variant = m_suggestionKind == QLatin1String("halo")
                                ? 1
                                : (m_suggestionKind == QLatin1String("splat") ? 2 : 0);
        m_controller->addCreativeRibbon(accent, m_subjectCanvasRect, variant,
                                        m_suggestionKind == QLatin1String("splat"));
        done = QStringLiteral("Stroke added — drag or undo it like any layer.");
    }
    else
    {
        const QPointF at(m_subjectCanvasRect.x() + m_subjectCanvasRect.width() * 0.78,
                         m_subjectCanvasRect.y() + m_subjectCanvasRect.height() * 0.22);
        const qreal size = qMax<qreal>(24.0,
                                       m_subjectCanvasRect.height() * 0.11);
        m_controller->addCreativeSparkle(accent, at, size);
        done = QStringLiteral("Sparkle added — move or undo it like any layer.");
    }
    m_suggestionVisible = false;
    m_suggestionText.clear();
    setStatus(done);
    emit suggestionChanged();
    emit stateChanged();
}

void PaintbrushCreative::dismissSuggestion()
{
    m_suggestionVisible = false;
    emit suggestionChanged();
}

void PaintbrushCreative::refreshHistory()
{
    if (!m_controller)
        return;
    const QVariantList next = m_controller->creativeLookHistory();
    if (next == m_history)
        return;
    m_history = next;
    emit historyChanged();
}

bool PaintbrushCreative::restoreFromCommittedLayer(int layerIndex)
{
    if (!m_controller)
        return false;
    QVariantMap info;
    QImage cutout;
    QRectF rect;
    QString label;
    if (!m_controller->creativeLookInfo(layerIndex, &info, &cutout, &rect, &label))
    {
        setStatus(QStringLiteral("This layer has no resumable creative look."));
        return false;
    }
    const QString patch = info.value(QStringLiteral("patch")).toString();
    const PatchDef *def = patchDef(patch);
    if (!def)
        return false;
    m_patchId = patch;
    m_chooserOpen = false;
    m_resumeGroup = info.value(QStringLiteral("group")).toString();
    if (!adoptSubject(cutout, rect,
                      info.value(QStringLiteral("sourceLayerId")).toInt(),
                      label, QStringLiteral("The source cutout is gone.")))
        return false;
    m_sourceHarvestId =
        info.value(QStringLiteral("sourceHarvestId")).toInt();
    m_seed = quint32(info.value(QStringLiteral("seed")).toLongLong());
    if (m_seed == 0)
        m_seed = 1;
    const QVariantMap params =
        info.value(QStringLiteral("params")).toMap();
    m_frame.x = float(params.value(QStringLiteral("x"), 0.5).toDouble());
    m_frame.y = float(params.value(QStringLiteral("y"), 0.5).toDouble());
    m_frame.energy = float(params.value(QStringLiteral("energy"), 0.0).toDouble());
    m_frame.hold = params.value(QStringLiteral("hold"), false).toBool();
    m_committed = false;
    rebuildPreview();
    refreshHistory();
    setOpen(true);
    setStatus(QStringLiteral("Resumed %1. Adjust and make it real — the next "
                             "commit replaces this look in one undo.")
                  .arg(def->name));
    emit patchChanged();
    emit padChanged();
    emit stateChanged();
    return true;
}

QString PaintbrushCreative::applyLookToSelection()
{
    if (!hasSubject() || !m_controller)
        return QStringLiteral("Nothing to apply.");
    const QString patch = m_patchId;
    const quint32 seed = m_seed;
    const CreativeFrame frame = m_frame;
    openForSelection();
    if (!hasSubject())
        return QStringLiteral("Select a cutout layer first.");
    m_patchId = patch;
    m_seed = seed;
    m_frame = frame;
    m_resumeGroup.clear();
    m_committed = false;
    rebuildPreview();
    const QString error = commit();
    if (error.isEmpty())
        setStatus(QStringLiteral("Look applied to %1 — one undo.")
                      .arg(m_subjectLabel));
    return error;
}

bool PaintbrushCreative::dumpStagePieces(const QString &directory) const
{
    if (m_pieces.isEmpty())
        return false;
    QDir dir(directory);
    if (!dir.exists() && !QDir().mkpath(directory))
        return false;
    QFile meta(dir.filePath(QStringLiteral("pieces.txt")));
    if (!meta.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QTextStream out(&meta);
    for (int i = 0; i < m_pieces.size(); ++i)
    {
        const CreativePiece &p = m_pieces.at(i);
        const QString base = QStringLiteral("%1-%2")
                                 .arg(i, 2, 10, QLatin1Char('0'))
                                 .arg(p.name);
        const QString path = dir.filePath(base + QStringLiteral(".png"));
        p.image.save(path);
        out << i << " " << p.name << " rect=" << p.rect.x() << ","
            << p.rect.y() << " " << p.rect.width() << "x" << p.rect.height()
            << " pivot=" << p.pivot.x() << "," << p.pivot.y()
            << " blend=" << p.blendMode << " opacity=" << p.opacity
            << " image=" << p.image.width() << "x" << p.image.height()
            << "\n";
    }
    meta.close();
    return true;
}

/* --------------------------------------------------------------- composition */

QImage PaintbrushCreative::composeStage(const QVector<CreativePiece> &pieces,
                                        const QSize &stage,
                                        const QColor &background) const
{
    QImage out(stage, QImage::Format_ARGB32_Premultiplied);
    out.fill(background);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const int count = pieces.size();
    for (int i = 0; i < count; ++i)
    {
        const CreativePiece &piece = pieces.at(i);
        if (piece.image.isNull())
            continue;
        const CreativeMotion &m = piece.motion;
        float op = piece.opacity
                   + m.opacityX * m_frame.x
                   + m.opacityY * m_frame.y
                   + m.opacityEnergy * m_frame.energy
                   + m.opacityHold * (m_frame.hold ? 1.0f : 0.0f);
        if (piece.role == QLatin1String("rim")
            || piece.role == QLatin1String("accent"))
            op *= 1.0f + 0.035f * float(std::sin(m_frame.time * 1.8 + i * 1.3));
        op = clamp01(op);
        if (op <= 0.002f)
            continue;

        const qreal dx = (m.dxX * (m_frame.x - 0.5f)
                          + m.dxEnergy * m_frame.energy
                          + m.dxHold * (m_frame.hold ? 1.0f : 0.0f))
                         * stage.width();
        const qreal dy = (m.dyY * (m_frame.y - 0.5f)
                          + m.dyEnergy * m_frame.energy
                          + m.dyHold * (m_frame.hold ? 1.0f : 0.0f))
                         * stage.height();
        const qreal sc = 1.0 + m.scaleY * (m_frame.y - 0.5f)
                         + m.scaleEnergy * m_frame.energy
                         + m.scaleHold * (m_frame.hold ? 1.0 : 0.0);
        const qreal rot = m.rotationEnergy * m_frame.energy
                          + m.rotationHold * (m_frame.hold ? 1.0 : 0.0);

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

/* -------------------------------------------------------------- patch recipes */

QVector<CreativePiece> PaintbrushCreative::buildPatch(const QString &patchId,
                                                      const QImage &subject,
                                                      const QRectF &subjectRect,
                                                      const QSize &stage,
                                                      quint32 seed) const
{
    QVector<CreativePiece> pieces;
    if (subject.isNull() || !stage.isValid() || stage.isEmpty())
        return pieces;

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
        void operator()(const QString &name, const QString &role,
                        const QImage &image, const QRectF &rect,
                        int blend = BLEND_NORMAL, float opacity = 1.0f,
                        const CreativeMotion &motion = CreativeMotion()) const
        {
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
        }
    } add{ &pieces, subjectRect.center() };

    const QRectF full(QPointF(0, 0), QSizeF(stage));

    /* a real body response: the subject itself breathes and lifts with Y so
     * the pad is never just a subtle tint change */
    CreativeMotion subjectM;
    subjectM.scaleY = 0.060f;
    subjectM.dyY = -0.025f;
    subjectM.opacityY = 0.03f;

    auto drawAt = [](QImage &dst, const QImage &src, const QRectF &r) {
        drawScaled(dst, src, r);
    };
    auto band = [&](qreal fraction, const QColor &color, const QString &name,
                    const QString &role, float opacity, int blend,
                    const CreativeMotion &motion) {
        const int radius = qMax(1, int(std::lround(subjectRect.height() * fraction)));
        QImage bandMask = creative::bandFromMask(subjMask, radius);
        QImage fullBand = stageCanvas(stage);
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
    };
    auto subjectFull = [&](const QImage &graded, const QString &name,
                           float opacity, const CreativeMotion &motion) {
        QImage fullSubj = stageCanvas(stage);
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
    };
    auto solid = [&](const QImage &src, const QColor &color) {
        return tintPreserveAlpha(src, color);
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

        QImage echo = creative::gradeImage(creative::softBlur(s, 7),
                                           creative::Grade{ 1.06f, 1.22f, 0.22f,
                                                            rose, 0.55f, cyan, 0.38f });
        QImage echoFull = stageCanvas(stage);
        drawAt(echoFull, echo,
               scaleRectAbout(subjectRect, 1.72, -W * 0.055, -H * 0.035));
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

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.05f, 1.12f, 1.20f, cobalt, 0.22f, coral, 0.10f });
        subjectFull(graded, QStringLiteral("Neon idol"), 1.0f, subjectM);

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
        fadeHorizontally(pieces[pieces.size() - 2].image, true);
        fadeHorizontally(pieces[pieces.size() - 1].image, false);

        CreativeMotion lightM;
        lightM.opacityY = 0.28f;
        add(QStringLiteral("Neon light"), QStringLiteral("finish"),
            creative::glossOverlay(stage, s, subjectRect, cream),
            full, BLEND_SCREEN, 0.30f, lightM);

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

        /* subject: luminance-preserving recolour, so the fabric keeps its
         * folds instead of turning into a flat gradient */
        creative::RecolorParams recolor;
        recolor.palette = { cyan, rose, magenta, lime };
        recolor.strength = 0.88f;
        recolor.textureKeep = 0.18f;
        recolor.skinGuard = 0.45f;
        recolor.hueDrift = 0.0f;
        recolor.seed = seed;
        QImage voltageSubject =
            creative::recolorMasked(s, subjMask, recolor);
        CreativeMotion recolorM;
        recolorM.opacityX = 0.10f;
        recolorM.scaleY = 0.045f;
        recolorM.dyY = -0.020f;
        recolorM.opacityY = 0.03f;
        subjectFull(voltageSubject, QStringLiteral("Voltage idol"), 1.0f,
                    recolorM);

        /* light that moves across the highlights */
        QImage sheen = creative::spectralSheen(
            s, subjMask, { cyan, rose, lime, white }, 0.95f, 62.0f,
            float(seed % 628) / 100.0f);
        QImage sheenFull = stageCanvas(stage);
        drawAt(sheenFull, sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY = 0.22f;
        sheenM.opacityHold = 0.16f;
        add(QStringLiteral("Voltage sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN, 0.85f, sheenM);

        CreativeMotion rimIceM;
        rimIceM.opacityX = -0.24f;
        rimIceM.opacityY = 0.14f;
        band(0.009, cyan, QStringLiteral("Voltage rim ice"),
             QStringLiteral("rim"), 0.90f, BLEND_SCREEN, rimIceM);
        CreativeMotion rimRoseM;
        rimRoseM.opacityX = 0.24f;
        rimRoseM.opacityY = 0.14f;
        band(0.016, rose, QStringLiteral("Voltage rim rose"),
             QStringLiteral("rim"), 0.80f, BLEND_SCREEN, rimRoseM);
        /* directional light: each rim owns one side of the silhouette */
        fadeHorizontally(pieces[pieces.size() - 2].image, true);
        fadeHorizontally(pieces[pieces.size() - 1].image, false);

        /* bolts land on the real silhouette, not on its bounding box */
        QImage subjectOnStage = stageCanvas(stage);
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

        /* two real bolts, seeded so replay and commit match */
        creative::ArcParams arc;
        arc.halo = cyan;
        arc.core = white;
        arc.glowWidth = qMax(5.0f, float(H) * 0.016f);
        arc.coreWidth = qMax(1.2f, float(H) * 0.0026f);
        arc.sparkCount = 72;

        CreativeMotion arcM;
        arcM.opacityHold = 0.15f;
        arcM.opacityY = 0.12f;
        add(QStringLiteral("Voltage arc ice"), QStringLiteral("accent"),
            creative::electricArc(stage, QPointF(-W * 0.02, H * 0.12),
                                  targetL, seed + 31, arc),
            full, BLEND_SCREEN, 0.96f, arcM);
        arc.halo = rose;
        arc.core = QColor(255, 240, 250);
        add(QStringLiteral("Voltage arc rose"), QStringLiteral("accent"),
            creative::electricArc(stage, QPointF(W * 1.02, H * 0.20),
                                  targetR, seed + 47, arc),
            full, BLEND_SCREEN, 0.92f, arcM);

        /* sparks leave the real silhouette, not a random circle */
        CreativeMotion sparkM;
        sparkM.opacityEnergy = 0.35f;
        sparkM.opacityHold = 0.20f;
        add(QStringLiteral("Voltage sparks"), QStringLiteral("accent"),
            creative::sparkField(stageMask, stage, seed + 77,
                                 { cyan, rose, lime, white }, 90, 0.85f),
            full, BLEND_SCREEN, 0.85f, sparkM);

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

        QImage card = stageCanvas(stage);
        {
            QPainter p(&card);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 238));
            p.translate(subjectRect.center());
            p.rotate(-2.0);
            const QRectF r = scaleRectAbout(
                QRectF(-subjectRect.width() / 2.0, -subjectRect.height() / 2.0,
                       subjectRect.width(), subjectRect.height()),
                1.06, 0, H * 0.018);
            p.drawRoundedRect(r, H * 0.03, H * 0.03);
        }
        add(QStringLiteral("Sticker backing"), QStringLiteral("backdrop"),
            card, full, BLEND_NORMAL, 0.85f);

        QImage shadowSubj = creative::softBlur(solid(s, ink), 6);
        QImage shadowFull = stageCanvas(stage);
        drawAt(shadowFull, shadowSubj, subjectRect.translated(0, H * 0.012));
        add(QStringLiteral("Sticker shadow"), QStringLiteral("echo"),
            shadowFull, full, BLEND_NORMAL, 0.30f);

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
            QImage blueBand = stageCanvas(stage);
            drawAt(blueBand, creative::colorizeBand(bandMask, blue, false, seed + 9),
                   subjectRect);
            CreativeMotion blueM;
            blueM.opacityX = -0.28f;
            blueM.scaleHold = 0.12f;
            add(QStringLiteral("Sticker cut line ice"), QStringLiteral("rim"),
                blueBand, full, BLEND_NORMAL, 0.9f, blueM);
        }

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.07f, 1.05f, 1.18f, ink, 0.05f, yellow, 0.10f });
        subjectFull(graded, QStringLiteral("Sticker idol"), 1.0f, subjectM);

        CreativeMotion glossM;
        glossM.opacityY = 0.30f;
        glossM.opacityHold = 0.15f;
        add(QStringLiteral("Sticker gloss"), QStringLiteral("finish"),
            creative::glossOverlay(stage, s, subjectRect, QColor(255, 255, 255)),
            full, BLEND_SCREEN, 0.38f, glossM);

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

        QImage floor = stageCanvas(stage);
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

        QImage mirrored = creative::reflection(s, 0.55, 0.38);
        QImage mirrorFull = stageCanvas(stage);
        drawAt(mirrorFull, mirrored,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        add(QStringLiteral("Pastel reflection"), QStringLiteral("echo"),
            mirrorFull, full, BLEND_NORMAL, 0.55f);

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.10f, 0.94f, 0.92f, lavender, 0.16f,
                                QColor(255, 255, 255), 0.12f });
        subjectFull(graded, QStringLiteral("Pastel idol"), 1.0f, subjectM);

        CreativeMotion bloomM;
        bloomM.opacityY = 0.20f;
        bloomM.opacityHold = 0.30f;
        bloomM.scaleHold = 0.07f;
        bloomM.dxX = 0.035f;
        add(QStringLiteral("Pastel bloom"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.30, QColor(255, 255, 255), 0.45f),
            full, BLEND_SCREEN, 0.5f, bloomM);

        CreativeMotion confettiM;
        confettiM.opacityEnergy = 0.30f;
        confettiM.scaleEnergy = 0.12f;
        confettiM.opacityX = 0.25f;
        confettiM.dxX = 0.030f;
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

        CreativeMotion auraM;
        auraM.opacityY = 0.16f;
        auraM.opacityHold = 0.28f;
        auraM.scaleHold = 0.06f;
        add(QStringLiteral("Nocturne aura"), QStringLiteral("accent"),
            creative::radialGlow(stage, subjectRect.center(),
                                 H * 0.48, violet, 0.55f),
            full, BLEND_SCREEN, 0.6f, auraM);

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 0.94f, 1.18f, 0.88f, QColor(40, 18, 90), 0.34f,
                                silver, 0.12f });
        subjectFull(graded, QStringLiteral("Nocturne figure"), 1.0f,
                    CreativeMotion());

        CreativeMotion rimVioletM;
        rimVioletM.opacityX = 0.30f;
        rimVioletM.opacityHold = 0.20f;
        band(0.010, lilac, QStringLiteral("Nocturne rim"), QStringLiteral("rim"),
             0.75f, BLEND_SCREEN, rimVioletM);
        CreativeMotion rimTealM;
        rimTealM.opacityX = -0.30f;
        band(0.010, teal, QStringLiteral("Nocturne rim teal"),
             QStringLiteral("rim"), 0.55f, BLEND_SCREEN, rimTealM);
        fadeHorizontally(pieces[pieces.size() - 2].image, true);
        fadeHorizontally(pieces[pieces.size() - 1].image, false);

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

        QImage sheen = creative::spectralSheen(
            s, subjMask, { cyan, pink, gold, silver }, 1.0f, -28.0f,
            float(seed % 628) / 100.0f);
        QImage sheenFull = stageCanvas(stage);
        drawAt(sheenFull, sheen, subjectRect);
        CreativeMotion sheenM;
        sheenM.opacityY = 0.30f;
        sheenM.opacityHold = 0.20f;
        add(QStringLiteral("Liquid sheen"), QStringLiteral("finish"),
            sheenFull, full, BLEND_SCREEN, 0.92f, sheenM);

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

        CreativeMotion prismM;
        prismM.opacityEnergy = 0.30f;
        prismM.opacityHold = 0.25f;
        add(QStringLiteral("Prism sparkle"), QStringLiteral("accent"),
            creative::starField(stage, seed + 4, { silver, gold, cyan }, 80),
            full, BLEND_SCREEN, 0.62f, prismM);

        QImage mirror = creative::reflection(s, 0.20, 0.15);
        QImage mirrorFull = stageCanvas(stage);
        drawAt(mirrorFull, mirror,
               QRectF(subjectRect.x(), subjectRect.bottom() + H * 0.004,
                      subjectRect.width(), subjectRect.height()));
        CreativeMotion mirrorM;
        mirrorM.opacityY = -0.10f;
        add(QStringLiteral("Holo reflection"), QStringLiteral("echo"),
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

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.02f, 1.30f, 0.35f, ink, 0.20f,
                                QColor(255, 255, 255), 0.05f });
        subjectFull(graded, QStringLiteral("Ink figure"), 1.0f, CreativeMotion());

        CreativeMotion lineM;
        lineM.opacityX = 0.20f;
        lineM.opacityEnergy = 0.25f;
        band(0.006, ink, QStringLiteral("Ink line"), QStringLiteral("rim"),
             0.9f, BLEND_NORMAL, lineM);
        CreativeMotion lineSoftM;
        lineSoftM.opacityX = -0.20f;
        band(0.014, QColor(20, 18, 16, 120), QStringLiteral("Ink halo"),
             QStringLiteral("rim"), 0.35f, BLEND_NORMAL, lineSoftM);

        CreativeMotion splatM;
        splatM.opacityEnergy = 0.35f;
        splatM.scaleEnergy = 0.15f;
        add(QStringLiteral("Ink splatter"), QStringLiteral("accent"),
            creative::inkSplatter(stage, seed,
                                  subjectRect.adjusted(-W * 0.12, -H * 0.10,
                                                       W * 0.12, H * 0.10),
                                  ink, 80),
            full, BLEND_NORMAL, 0.7f, splatM);

        QImage eyes = stageCanvas(stage);
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

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.05f, 1.10f, 1.12f, QColor(40, 20, 80), 0.18f,
                                gold, 0.10f });
        subjectFull(graded, QStringLiteral("Storm figure"), 1.0f,
                    CreativeMotion());

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
        QImage subjectCanvas = stageCanvas(stage);
        drawAt(subjectCanvas, s, subjectRect);
        CreativeMotion k6M;
        k6M.rotationEnergy = 24.0f;
        k6M.opacityX = 0.15f;
        add(QStringLiteral("Kaleido six"), QStringLiteral("subject"),
            creative::wedgeKaleidoscope(subjectCanvas, stage, 6, 1.0, 0.0),
            full, BLEND_NORMAL, 0.9f, k6M);
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
        const qreal spacing = subjectRect.width() * 0.26;
        for (int i = 0; i < 6; ++i)
        {
            const int k = i - 3;
            if (k == 0)
                continue;
            QImage echo = creative::hueShift(s, k * 14);
            echo = creative::gradeImage(
                echo, creative::Grade{ 0.92f, 1.08f, 0.9f,
                                       k < 0 ? blue : pink, 0.22f,
                                       QColor(255, 255, 255), 0.05f });
            QImage echoFull = stageCanvas(stage);
            drawAt(echoFull, echo,
                   subjectRect.translated(k * spacing, k * spacing * 0.05));
            CreativeMotion echoM;
            echoM.opacityX = 0.06f;
            echoM.dxX = 0.012f * k;
            add(QStringLiteral("Echo %1").arg(k > 0 ? k : -k),
                QStringLiteral("echo"), echoFull, full, BLEND_NORMAL,
                float(0.48 - qAbs(k) * 0.070), echoM);
        }
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
            QImage img = stageCanvas(stage);
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
            QImage img = stageCanvas(stage);
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
        curtainL.dxHold = -0.045f;
        add(QStringLiteral("Curtain left"), QStringLiteral("backdrop"),
            curtain(true), full, BLEND_NORMAL, 0.95f, curtainL);
        CreativeMotion curtainR;
        curtainR.dxX = 0.055f;
        curtainR.dxHold = 0.045f;
        add(QStringLiteral("Curtain right"), QStringLiteral("backdrop"),
            curtain(false), full, BLEND_NORMAL, 0.95f, curtainR);

        QImage cone = stageCanvas(stage);
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

        QImage spot = stageCanvas(stage);
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

        QImage drips = stageCanvas(stage);
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

        const QImage graded = creative::gradeImage(
            s, creative::Grade{ 1.04f, 1.22f, 0.90f, ink, 0.18f, red, 0.10f });
        subjectFull(graded, QStringLiteral("Poster figure"), 1.0f,
                    CreativeMotion());

        QImage tape = stageCanvas(stage);
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
        QImage floor = stageCanvas(stage);
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
            QImage img = stageCanvas(stage);
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
        curtainL.dxHold = -0.06f;
        add(QStringLiteral("Stage curtain left"), QStringLiteral("backdrop"),
            curtain(true), full, BLEND_NORMAL, 0.95f, curtainL);
        CreativeMotion curtainR;
        curtainR.dxX = 0.05f;
        curtainR.dxHold = 0.06f;
        add(QStringLiteral("Stage curtain right"), QStringLiteral("backdrop"),
            curtain(false), full, BLEND_NORMAL, 0.95f, curtainR);

        QImage cone = stageCanvas(stage);
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
        add(QStringLiteral("Stage dust"), QStringLiteral("finish"),
            creative::starField(stage, seed, cream, 70), full,
            BLEND_SCREEN, 0.45f);
        add(QStringLiteral("Stage grain"), QStringLiteral("finish"),
            creative::grainField(stage, seed + 2, 24), full,
            BLEND_OVERLAY, 0.3f);
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

    /* Universal stage light: every patch visibly answers the pad. X sweeps
     * the light, Y lifts it, Hold blooms it, speed lets it breathe. It is a
     * normal committed layer, never a UI-only effect. */
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
            stageLightColor(patchId), 0.30f);
        light.rect = full;
        light.pivot = lightCenter;
        light.blendMode = BLEND_SCREEN;
        light.opacity = 0.14f;
        light.motion = lightM;
        pieces.append(light);
    }

    return pieces;
}

} /* namespace paintbrush */

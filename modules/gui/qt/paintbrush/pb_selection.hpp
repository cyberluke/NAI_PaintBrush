/*****************************************************************************
 * pb_selection.hpp : NAI Paintbrush — object selection context
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_SELECTION_HPP
#define PB_SELECTION_HPP

/* Pure-Qt object selection context shared by the controller and the Live
 * Harvest verification harness. Deliberately has NO VLC dependency (no
 * vlc_*.h, no Q_OBJECT) so the geometry mapping, session semantics and mask
 * space rules can be compiled and asserted by a standalone harness without
 * building VLC. */

#include "pb_media_logic.hpp"

#include <QImage>
#include <QSize>
#include <QRectF>
#include <QTransform>
#include <QPointF>
#include <QString>
#include <cmath>

namespace paintbrush {

/* One authoritative context for an object selection. EVERY object action
 * (Keep, add-to-canvas, cut out, export PNG, ask, track, sticker, remove /
 * generation) consumes this single selection context so no consumer ever
 * guesses which frame the current mask belongs to or which coordinates it
 * lives in.
 *
 * For a live-video selection the context keeps:
 *   - source kind = live video, with the exact FrameRef (MRL + verified PTS)
 *   - the native decoded frame (authoritative source pixels)
 *   - the native SAM mask (authoritative, in the SAME native frame space as
 *     `frame`; never destructively rescaled for display)
 *   - the frame<->viewport transform (the letterbox aspect-fit that maps
 *     between the displayed viewport and the native frame pixels)
 *   - a session/request identity so a stale async result is discarded.
 *
 * `mask` is ALWAYS in `frame` pixel space — the canonical native mask. It is
 * not the display overlay; consumers map it to the viewport (for display)
 * only when needed, never by mutating the authoritative copy. */
struct ObjectSelectionContext
{
    enum SourceKind {
        SourceNone = 0,
        SourceLiveVideo,  /* live media playing; media-backed native frame */
        SourceStills,     /* captured reference / compose canvas          */
    };

    SourceKind sourceKind = SourceNone; /* what produced this selection       */
    FrameRef ref;                 /* exact source frame when media-backed     */
    QImage frame;                 /* authoritative source pixels (native)     */
    QImage mask;                  /* authoritative mask, in `frame` space      */
    QSize viewportSize;           /* the viewport the selection was seeded on  */
    QRectF contentRect;           /* aspect-fit rect inside the viewport       */
    QTransform frameToItem;       /* native frame pixel -> viewport point      */
    QTransform itemToFrame;       /* viewport point -> native frame pixel      */
    QString sessionId;            /* opaque, unique per selection session      */
    double score = 0.0;           /* SAM confidence of the mask                */

    bool validFrame() const { return !frame.isNull(); }
    bool valid() const { return validFrame() && !contentRect.isEmpty(); }

    /* Map a viewport point into native frame space (NaN outside the video). */
    QPointF itemToFramePoint(const QPointF &p) const
    {
        if (!valid())
            return { qQNaN(), qQNaN() };
        return itemToFrame.map(p);
    }
    /* Map a native frame pixel into viewport space. */
    QPointF frameToItemPoint(const QPointF &p) const
    {
        if (!valid())
            return { qQNaN(), qQNaN() };
        return frameToItem.map(p);
    }
    /* True if this selection belongs to the given media+PTS (same frame). */
    bool matchesFrame(const FrameRef &other) const
    {
        return !ref.mrl.isEmpty() && ref.mrl == other.mrl && ref.ptsMs == other.ptsMs;
    }
};

/* Letterbox aspect-fit mapping (P0#2/P2#13): given a native frame size and a
 * viewport (item) size, fill `ctx` with the content rect and the frame<->item
 * transforms. The full viewport is cleared to transparent and the frame is
 * drawn scaled into its content rect leaving letterbox margins — the same
 * geometry the live VideoSurface displays. The mapping is a pure function so
 * callers — and the Live Harvest harness — can compute it identically. */
inline void buildSelectionTransforms(ObjectSelectionContext &ctx)
{
    const QSizeF fs = QSizeF(ctx.frame.size());
    const QSizeF vs = QSizeF(ctx.viewportSize);
    const qreal fit = (vs.isEmpty() || fs.isEmpty())
                          ? 1.0
                          : qMin(vs.width() / fs.width(), vs.height() / fs.height());
    const qreal cw = fs.width() * fit;
    const qreal ch = fs.height() * fit;
    ctx.contentRect = QRectF((vs.width() - cw) / 2.0,
                             (vs.height() - ch) / 2.0, cw, ch);

    /* viewport -> frame: translate the content origin to the origin, THEN
     * scale by 1/fit. QTransform applies the right-most op first, so
     * `.scale(...).translate(-left,-top)` maps p -> (p + (-left,-top)) then
     * scales: (x - left) * (1/fit). (The reverse ordering, translate then
     * scale, would pre-scale left, producing an off-by-fit error — this was
     * caught by the Live Harvest letterbox harness.) */
    ctx.itemToFrame = QTransform().scale(1.0 / fit, 1.0 / fit)
                                  .translate(-ctx.contentRect.left(),
                                             -ctx.contentRect.top());
    ctx.frameToItem = ctx.itemToFrame.inverted();
}

} // namespace paintbrush

#endif // PB_SELECTION_HPP
/*****************************************************************************
 * pb_sticker.hpp : NAI Paintbrush — sticker asset rendering and frame cache
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_STICKER_HPP
#define PB_STICKER_HPP

#include "pb_common.hpp"
#include "pb_document.hpp"

#include <QImage>
#include <QString>
#include <QColor>
#include <QPoint>
#include <QHash>
#include <QList>

namespace paintbrush {

/* Render a keyframe cutout with optional outline/shadow/scale and an
 * optional track effect. Shared by the sticker preview data URLs, canvas
 * placement and the optioned export so what you see is what exports.
 * `frame` must be the REAL source pixels for the keyframe (see
 * StickerFrameCache); a flat placeholder fill produces a flat silhouette,
 * which is exactly the A16 defect this module fixes. */
QImage renderStickerFrame(const QImage &frame, const QImage &mask, int outline,
                          const QColor &outlineColor, bool shadow, double scale,
                          const TrackEffect *effect = nullptr);

/* Trim an ARGB image to its non-transparent bounds. Returns the (possibly
 * empty) trimmed image and writes the offset of the trim within the source
 * into `offset` (when non-null), so callers can keep the trimmed content at
 * its original canvas position. */
QImage trimToContent(const QImage &image, QPoint *offset = nullptr);

/* Encode a rendered sticker frame as a PNG data: URL for QML Image sources.
 * This is the shared tail of stickerEditorPreviewDataUrl /
 * trackEffectPreviewDataUrl: real source pixels in, real preview out. */
QString stickerPreviewDataUrl(const QImage &frame, const QImage &mask,
                              const TrackEffect *effect, int outline,
                              const QColor &outlineColor, bool shadow,
                              double scale);

/* Bounded LRU cache of real source-frame pixels per (trackId, keyframe).
 * Previews and exports read through this cache so the main playback player
 * is not driven (seek/capture) for frames that are already known; the byte
 * budget keeps memory bounded on long tracks. A full offline decode
 * pipeline (EditDecodeSource / FrameProvider, Stream 3) replaces the
 * player-backed capture path entirely; this cache is the honest bridge
 * until then. */
class StickerFrameCache
{
public:
    struct Entry
    {
        qint64 timeMs = 0; /* media timestamp the pixels belong to   */
        QImage frame;      /* real source pixels (RGBA) for keyframe */
    };

    bool get(int trackId, int frameIndex, Entry *out) const;
    void put(int trackId, int frameIndex, qint64 timeMs, const QImage &frame);
    void remove(int trackId, int frameIndex);
    void clear();

    qint64 bytes() const { return m_bytes; }
    void setByteBudget(qint64 bytes)
    {
        m_budget = qMax<qint64>(1024, bytes);
        evict();
    }

private:
    static quint64 key(int trackId, int frameIndex)
    {
        return (quint64(quint32(trackId)) << 32) | quint32(frameIndex);
    }
    void evict();

    QHash<quint64, Entry> m_entries;
    /* mutable: get() touches the LRU order */
    mutable QList<quint64> m_order;       /* most-recently-used first      */
    qint64 m_bytes = 0;
    qint64 m_budget = 256LL * 1024 * 1024; /* 256 MiB default               */
};

} // namespace paintbrush

#endif // PB_STICKER_HPP
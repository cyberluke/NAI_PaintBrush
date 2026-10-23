/*****************************************************************************
 * pb_media_logic.hpp : NAI Paintbrush — pure media-pipeline decision logic
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_MEDIA_LOGIC_HPP
#define PB_MEDIA_LOGIC_HPP

/* Pure-Qt media pipeline primitives shared by the edit frame source, the
 * thumbnail service and the tracking service. This header deliberately has
 * NO VLC dependency (no vlc_*.h, no Q_OBJECT, no vlc_object_t) so the exact
 * decision logic — priority ordering, PTS tolerance gating, LRU eviction,
 * tracking pass state — can be compiled and asserted by a standalone
 * harness without building VLC. */

#include <QString>
#include <QImage>
#include <QHash>
#include <QList>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <cstdint>
#include <climits>
#include <functional>
#include <list>

namespace paintbrush {

/* ------------------------------------------------------------------ FrameRef */

/* Where a frame came from: media identity + exact timestamp. This is the
 * currency of the exact-frame pipeline (FrameProvider): every consumer
 * requests an explicit FrameRef and never snapshots the live vout. */
struct FrameRef
{
    QString mrl;      /* media URI */
    QString title;    /* display title (optional) */
    qint64 ptsMs = 0; /* requested presentation timestamp (ms) */
    int width = 0;    /* decoded frame size (filled by the provider) */
    int height = 0;

    bool isValid() const { return !mrl.isEmpty() && ptsMs >= 0; }
};

/* Result of one exact-frame decode request. `ptsMs` is the ACTUAL PTS of
 * the decoded frame (from the decoder, never a guess); `ptsVerified` is
 * true only when |actual - requested| is within the configured tolerance.
 * Consumers must gate publication/processing on `ok()`. */
struct DecodedFrame
{
    QImage image;
    qint64 requestedMs = 0;  /* what the consumer asked for */
    qint64 ptsMs = 0;        /* actual PTS of the decoded frame */
    bool ptsVerified = false;

    bool ok() const { return !image.isNull() && ptsVerified; }
};

/* PTS tolerance gate. The thumbnail/tracking/export paths publish a frame
 * only when this passes: |actual - requested| <= tolerance. A decoded frame
 * whose PTS drifts beyond tolerance is evidence of a wrong frame and must
 * not be used. */
inline bool ptsWithinTolerance(qint64 requestedMs, qint64 actualMs,
                               qint64 toleranceMs)
{
    if (actualMs < 0)
        return false;
    return qAbs(actualMs - requestedMs) <= qMax<qint64>(0, toleranceMs);
}

/* ------------------------------------------------------- FrameProvider */

/* Abstract exact-frame source. The edit pipeline (thumbnails, tracking,
 * sticker export, AI crops) talks to this interface only — never to the
 * main playback player. Implementations own their decode path and return
 * frames with concrete, verified PTS. */
class FrameProvider
{
public:
    virtual ~FrameProvider() = default;

    /* Decode the exact frame for `ref`. Blocks until the frame is decoded
     * (or the request fails/times out). May hit an internal cache. The
     * returned DecodedFrame carries the actual PTS and verification state;
     * it is safe to call from any thread (implementations serialize). */
    virtual DecodedFrame request(const FrameRef &ref) const = 0;

    /* Metadata of the most recent successfully decoded frame. */
    virtual FrameRef last() const = 0;

    /* Whether the source can currently decode (preparser alive, etc.). */
    virtual bool canDecode() const = 0;
};

/* ------------------------------------------------------------ LruCache */

/* LRU byte-budget store for decoded images (and other copyable payloads).
 * Entries are evicted least-recently-used first whenever the cumulative
 * bytes exceed the budget, so caches cannot grow without bound. Values are
 * addressed by an opaque key string; `get()` touches recency, `insert()`
 * replaces and marks most-recent. */
template <typename T>
class LruCache
{
public:
    explicit LruCache(qint64 budgetBytes)
        : m_budgetBytes(qMax<qint64>(0, budgetBytes))
    {
    }

    bool contains(const QString &key) const { return m_map.contains(key); }

    /* Returns a copy of the entry and marks it most-recently-used. A miss
     * returns a default-constructed T (call contains() first if the caller
     * needs to distinguish). */
    T get(const QString &key)
    {
        auto it = m_map.find(key);
        if (it == m_map.end())
            return T();
        m_order.erase(it->it);
        m_order.push_front(key);
        it->it = m_order.begin();
        return it->value;
    }

    void insert(const QString &key, const T &value)
    {
        const qint64 bytes = valueBytes(value);
        auto it = m_map.find(key);
        if (it != m_map.end())
        {
            m_order.erase(it->it);
            m_bytes -= it->bytes;
            m_map.erase(it);
        }
        m_order.push_front(key);
        Entry entry;
        entry.value = value;
        entry.bytes = bytes;
        entry.it = m_order.begin();
        m_map.insert(key, entry);
        m_bytes += bytes;
        evict();
    }

    void clear()
    {
        m_map.clear();
        m_order.clear();
        m_bytes = 0;
    }

    int size() const { return m_map.size(); }
    qint64 bytes() const { return m_bytes; }
    qint64 budgetBytes() const { return m_budgetBytes; }

    void setBudgetBytes(qint64 budgetBytes)
    {
        m_budgetBytes = qMax<qint64>(0, budgetBytes);
        evict();
    }

    /* Keys in most-recently-used-first order (diagnostics + tests). */
    QList<QString> keysLruOrder() const
    {
        QList<QString> keys;
        keys.reserve(m_order.size());
        for (const QString &key : m_order)
            keys.append(key);
        return keys;
    }

    static qint64 valueBytes(const T &value);

private:
    void evict()
    {
        while (m_bytes > m_budgetBytes && !m_order.empty())
        {
            const QString key = m_order.back();
            m_order.pop_back();
            const auto it = m_map.find(key);
            if (it != m_map.end())
            {
                m_bytes -= it->bytes;
                m_map.erase(it);
            }
        }
    }

    struct Entry
    {
        T value;
        qint64 bytes = 0;
        std::list<QString>::iterator it;
    };

    QHash<QString, Entry> m_map;
    std::list<QString> m_order; /* front = most recently used */
    qint64 m_bytes = 0;
    qint64 m_budgetBytes = 0;
};

template <>
inline qint64 LruCache<QImage>::valueBytes(const QImage &value)
{
    return value.isNull() ? 0 : qint64(value.sizeInBytes());
}

template <>
inline qint64 LruCache<DecodedFrame>::valueBytes(const DecodedFrame &value)
{
    return value.image.isNull() ? 0
                                : qint64(value.image.sizeInBytes()) + 64;
}

/* ------------------------------------------- ThumbnailPriorityQueue */

/* Timestamp priority queue for thumbnail batches: entries are popped
 * highest-priority first, where priority is the rank in the anchor-first
 * `ordered` list (index 0 = nearest to the anchor = highest priority,
 * tie-broken by earliest timestamp). This is the queue the `ordered` list
 * (previously computed and discarded) is actually fed into: candidates are
 * pushed and popHighest() returns them in exactly the priority order. */
class ThumbnailPriorityQueue
{
public:
    /* `priorityOrder` ranks timestamps: the first element has the highest
     * priority. `anchorMs` is only used to rank timestamps that are not in
     * the list (defensive; the batch flow pushes ordered members only). */
    ThumbnailPriorityQueue() = default;
    ThumbnailPriorityQueue(const QList<qint64> &priorityOrder, qint64 anchorMs)
        : m_anchor(anchorMs)
    {
        for (int i = 0; i < priorityOrder.size(); ++i)
            m_rank.insert(priorityOrder[i], i);
    }

    void push(qint64 timeMs)
    {
        int lo = 0, hi = m_items.size();
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (less(timeMs, m_items[mid]))
                hi = mid;
            else
                lo = mid + 1;
        }
        m_items.insert(lo, timeMs);
    }

    /* Pop and return the highest-priority timestamp (0 if empty). */
    qint64 popHighest()
    {
        if (m_items.isEmpty())
            return 0;
        const qint64 t = m_items.first();
        m_items.removeFirst();
        return t;
    }

    bool isEmpty() const { return m_items.isEmpty(); }
    int size() const { return m_items.size(); }
    void clear() { m_items.clear(); }

private:
    int rankOf(qint64 t) const
    {
        const auto it = m_rank.constFind(t);
        if (it != m_rank.constEnd())
            return it.value();
        /* unknown timestamp: rank by distance from the anchor */
        return m_rank.size() + int(qAbs(t - m_anchor));
    }

    bool less(qint64 a, qint64 b) const
    {
        const int ra = rankOf(a);
        const int rb = rankOf(b);
        if (ra != rb)
            return ra < rb;
        return a < b; /* tie: earliest first */
    }

    QHash<qint64, int> m_rank;   /* timestamp -> priority rank */
    qint64 m_anchor = 0;
    QVector<qint64> m_items;     /* kept sorted by (rank, timestamp) */
};

/* Build the anchor-first sampling order for a set of candidate timestamps:
 * the candidate nearest the anchor first, then alternating outward on both
 * sides (farthest first per side so the viewport fills in first). Pure —
 * harness-testable. */
inline QList<qint64> orderedThumbnailTimes(const QList<qint64> &times,
                                           qint64 anchorMs)
{
    QList<qint64> ordered;
    if (times.isEmpty())
        return ordered;

    /* nearest to the anchor wins the top priority slot */
    qint64 best = times.first();
    qint64 bestDist = INT64_MAX;
    for (qint64 t : times)
    {
        const qint64 d = qAbs(t - anchorMs);
        if (d < bestDist)
        {
            bestDist = d;
            best = t;
        }
    }
    ordered.append(best);

    QList<qint64> left, right;
    for (qint64 t : times)
    {
        if (t == best)
            continue;
        if (t < best)
            left.append(t);
        else
            right.append(t);
    }
    /* farthest first on each side so the viewport fills in first */
    std::sort(left.begin(), left.end(), std::greater<qint64>());
    std::sort(right.begin(), right.end());
    int li = 0, ri = 0;
    bool takeLeft = true;
    while (li < left.size() || ri < right.size())
    {
        if (takeLeft && li < left.size())
            ordered.append(left[li++]);
        else if (ri < right.size())
            ordered.append(right[ri++]);
        else if (li < left.size())
            ordered.append(left[li++]);
        takeLeft = !takeLeft;
    }
    return ordered;
}

/* --------------------------------------------- TrackingPassState */

/* Pure state machine of the two-pass tracking walk. A tracking run is one
 * or two passes over the timeline: the first pass walks from the seed PTS
 * in `direction` (forward/backward), and TRACK_BOTH appends a genuine
 * second pass that re-seeds from the SAME seed state and walks the other
 * way. Frames are addressed by PTS: frame k of a pass is at
 * seedPts + k * stepSign * stepMs, and every mask is keyed by the ACTUAL
 * decoded PTS (never "whatever the vout showed +40ms"). */
class TrackingPassState
{
public:
    enum Direction
    {
        TRACK_FORWARD = 0,
        TRACK_BACKWARD = 1,
        TRACK_BOTH = 2,
    };

    /* Configure for a new tracking run. */
    void reset(int direction, int totalFrames, qint64 seedPtsMs, qint64 stepMs)
    {
        m_direction = direction;
        m_total = qMax(1, totalFrames);
        m_seedPtsMs = qMax<qint64>(0, seedPtsMs);
        m_stepMs = qMax<qint64>(1, stepMs);
        m_stepSign = (direction == TRACK_BACKWARD) ? -1 : 1;
        m_bothPhase = 0;
        m_done = 0;
    }

    int direction() const { return m_direction; }
    int stepSign() const { return m_stepSign; }
    int bothPhase() const { return m_bothPhase; }
    int done() const { return m_done; }
    int total() const { return m_total; }
    qint64 seedPtsMs() const { return m_seedPtsMs; }
    qint64 stepMs() const { return m_stepMs; }

    /* Advance progress by one propagated keyframe. */
    void markDone(int stepIndex) { m_done = qMax(m_done, stepIndex); }

    /* The first pass has covered `total` frames and a reverse pass is
     * owed (TRACK_BOTH and not yet reversed). */
    bool needsReversePass() const
    {
        return m_direction == TRACK_BOTH && m_bothPhase == 0
               && m_done >= m_total;
    }

    /* The whole run is finished (first pass done and nothing owed). */
    bool finished() const
    {
        return m_done >= m_total && !needsReversePass();
    }

    /* Switch to the genuine second pass: reverse direction, restart the
     * counter at frame 0 of the pass (the seed frame again). Callers must
     * additionally re-add the SAM instance with the seed mask/points and
     * reset m_lastMask to the SEED mask. */
    void beginReversePass()
    {
        m_bothPhase = 1;
        m_done = 0;
        m_stepSign = -1;
    }

    /* PTS of the next frame to decode, relative to the actual PTS of the
     * last emitted keyframe (starts from the seed PTS). Negative results
     * mean the walk ran off the start of the media. */
    qint64 nextPtsMs(qint64 lastActualPtsMs) const
    {
        return lastActualPtsMs + qint64(m_stepSign) * m_stepMs;
    }

private:
    int m_direction = TRACK_FORWARD;
    int m_total = 1;
    qint64 m_seedPtsMs = 0;
    qint64 m_stepMs = 40;
    int m_stepSign = 1;
    int m_bothPhase = 0; /* 0 = first direction, 1 = reverse pass */
    int m_done = 0;
};

} // namespace paintbrush

#endif // PB_MEDIA_LOGIC_HPP
/*****************************************************************************
 * pb_sequential_source.hpp : persistent sequential video frame provider
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * Temporal tracking must not reopen/seek the media for every frame. This
 * provider owns one in-process libvlc media player with video callbacks
 * (vmem), seeks ONCE to the tracking start, pauses, and then steps actual
 * frames in presentation order with libvlc_media_player_next_frame().
 *
 * - One persistent decoder per tracking operation (reused across operations
 *   when the media and output size are unchanged).
 * - No window, no playback-position changes to the user's main player.
 * - Actual frame PTS from the player; sequential, so VFR media is supported
 *   without assuming a constant frame grid.
 * - One-frame bounded buffering: the tracking worker consumes a frame before
 *   the next step is requested.
 *****************************************************************************/

#ifndef PB_SEQUENTIAL_SOURCE_HPP
#define PB_SEQUENTIAL_SOURCE_HPP

#include "pb_media_logic.hpp"

#include <QImage>
#include <QString>
#include <QMutex>
#include <QWaitCondition>

#include <vector>

struct libvlc_instance_t;
struct libvlc_media_t;
struct libvlc_media_player_t;

namespace paintbrush {

class SequentialVideoSource : public FrameProvider
{
public:
    SequentialVideoSource();
    ~SequentialVideoSource() override;

    /* Called once on the tracking worker with the seed frame. Opens the
     * media once, seeks once to the seed PTS, pauses and stores the seed
     * frame. Returns false with the provider unusable on failure. */
    bool prepare(const FrameRef &seed) override;

    /* Abort a blocked step/wait. Safe from any thread. */
    void cancel() override;

    DecodedFrame request(const FrameRef &ref) const override;
    FrameRef last() const override;
    bool canDecode() const override;

    /* Release the player and the dedicated libvlc instance. */
    void shutdown();

private:
    static void *lockCb(void *opaque, void **planes);
    static void unlockCb(void *opaque, void *picture, void *const *planes);
    static void displayCb(void *opaque, void *picture);

    bool ensurePlayer(const FrameRef &seed);
    bool waitForDisplay(quint64 sinceSeq, int timeoutMs) const;
    bool copyCurrentFrame(QImage &out, qint64 &ptsMs) const;
    void releasePlayer();

    mutable QMutex m_mutex;
    mutable QWaitCondition m_cond;
    mutable quint64 m_seq = 0;
    mutable qint64 m_lastPtsMs = -1;
    mutable QImage m_lastImage;

    bool m_prepared = false;
    bool m_cancelled = false;
    QString m_mrl;
    int m_width = 0;
    int m_height = 0;
    std::vector<unsigned char> m_buffer;

    libvlc_instance_t *m_instance = nullptr;
    libvlc_media_t *m_media = nullptr;
    libvlc_media_player_t *m_player = nullptr;
};

} // namespace paintbrush

#endif /* PB_SEQUENTIAL_SOURCE_HPP */

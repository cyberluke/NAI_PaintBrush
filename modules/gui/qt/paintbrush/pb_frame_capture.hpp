/*****************************************************************************
 * pb_frame_capture.hpp : NAI Paintbrush — media frame bridge
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_FRAME_CAPTURE_HPP
#define PB_FRAME_CAPTURE_HPP

#include "pb_common.hpp"
#include "pb_media_logic.hpp"

#include <QImage>
#include <QString>
#include <QMutex>
#include <QWaitCondition>
#include <QObject>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_preparser.h>

struct vlc_player_t;
struct vlc_preparser_t;
struct picture_t;

namespace paintbrush {

/* Media-frame alias kept for source compatibility with the pre-split name.
 * The canonical type is paintbrush::FrameRef (pb_media_logic.hpp). */
using MediaFrameRef = FrameRef;

/* Generic viewer screenshot utility. This is the ONLY remaining user of the
 * vout snapshot machinery: the edit/AI pipeline (thumbnails, tracking,
 * sticker export, AI crops) never calls these — it decodes exact frames
 * through FrameProvider/EditFrameSource instead (vout_GetSnapshot PNG
 * encode/decode retired from the edit path, A9). */
class FrameCaptureService
{
public:
    /* Capture the currently displayed video frame as an RGBA QImage using
     * the existing vout snapshot machinery (no window screenshots). */
    static QImage captureCurrentFrame(vlc_player_t *player);

    /* Capture the frame at the current position and return metadata. */
    static FrameRef currentMediaRef(vlc_player_t *player);

    /* The MRL of the currently playing item, if any. */
    static QString currentMRL(vlc_player_t *player);

    /* Current media timestamp in milliseconds. */
    static qint64 currentTimeMs(vlc_player_t *player);
};

/* EditDecodeSource: the dedicated exact-frame decoder for the Paintbrush
 * edit pipeline. PlaybackSession (the main VLC player) is untouched by
 * batch work; every exact-frame consumer (thumbnails, tracking, sticker
 * export, SAM/VLM crops) requests frames here through the FrameProvider
 * interface.
 *
 * Decode path: one in-tree thumbnailing input per request
 * (vlc_preparser_req_NewThumbnail, INPUT_TYPE_THUMBNAILING). The input is
 * created for the requested MRL, seeks PRECISELY to the requested PTS,
 * decodes exactly one frame and returns the RAW decoded picture (no PNG
 * encode/decode round-trip, A9). The picture's date is the actual PTS; the
 * frame is only accepted when |actual - requested| <= the configured
 * tolerance (paintbrush-edit-pts-tolerance-ms).
 *
 * HW decode policy (A18): the generic avcodec-hwdec tri-state (0 = Auto,
 * 1 = Disabled/software, 2 = Required) is applied to THIS source only —
 * both through vlc_thumbnailer_arg::hw_dec and as a per-item
 * :avcodec-hwdec=<n> option on the edit input. Main playback is never
 * forced to software decode anymore. NOTE: the in-tree thumbnailer's
 * decoder uses a NULL device by design (thumbnailer_get_device in
 * src/input/decoder.c: "no hardware decoder on purpose"), so the edit
 * source currently decodes software; the tri-state plumbing is live and HW
 * activates automatically once the thumbnailer supports devices.
 *
 * Threading: request() serializes on an internal mutex and blocks until
 * the decode finishes or the request times out (8 s). Callers run on the
 * GUI thread; the actual decode runs on the preparser's thumbnailer
 * thread, so the UI thread only waits. */
class EditFrameSource : public QObject, public FrameProvider
{
    Q_OBJECT

public:
    explicit EditFrameSource(QObject *parent = nullptr);
    ~EditFrameSource() override;

    /* Create the in-tree thumbnailing preparser. `obj` is the parent
     * vlc_object_t (the qt interface). */
    void configure(vlc_object_t *obj);

    /* Tear down the preparser and drop caches. Safe to call even with no
     * request in flight; must be called from the same thread that issues
     * request() calls (the GUI thread). */
    void shutdown();

    /* -- FrameProvider -------------------------------------------------- */
    /* Decode the exact frame for `ref`. See class docs. */
    DecodedFrame request(const FrameRef &ref) const override;
    FrameRef last() const override;
    bool canDecode() const override;

    /* Clear the decoded-frame LRU cache. */
    void clearCache();

    /* Convert a raw decoded VLC picture to QImage (RGBA). Returns a null
     * image for unsupported chroma. Exposed for reuse/diagnostics. */
    static QImage pictureToQImage(const picture_t *pic, vlc_object_t *logObj = nullptr);

private:
    struct RequestCtx; /* pimpl for the thumbnailer callback */

    /* Invoked on the preparser thumbnailer thread when a request ends. */
    static void onThumbnailEnded(vlc_preparser_req *req, int status,
                                 picture_t *thumbnail, void *data);

    vlc_object_t *m_obj = nullptr;
    vlc_preparser_t *m_preparser = nullptr;
    qint64 m_ptsToleranceMs = 250;
    int m_hwdecPolicy = 0; /* 0 Auto, 1 Disabled(SW), 2 Required */

    mutable QMutex m_mutex;
    mutable QWaitCondition m_cond;
    mutable LruCache<DecodedFrame> m_cache;
    mutable FrameRef m_last;
};

} // namespace paintbrush

#endif // PB_FRAME_CAPTURE_HPP
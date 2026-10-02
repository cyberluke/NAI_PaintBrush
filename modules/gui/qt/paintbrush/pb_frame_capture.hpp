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

#include <QImage>
#include <QString>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

struct vlc_player_t;

namespace paintbrush {

/* Frame metadata describing where a captured frame came from. */
struct MediaFrameRef
{
    QString mrl;
    QString title;
    qint64 timeMs = 0;
    int width = 0;
    int height = 0;
};

class FrameCaptureService
{
public:
    /* Capture the currently displayed video frame as an RGBA QImage using
     * the existing vout snapshot machinery (no window screenshots). */
    static QImage captureCurrentFrame(vlc_player_t *player);

    /* Capture the frame at the current position and return metadata. */
    static MediaFrameRef currentMediaRef(vlc_player_t *player);

    /* The MRL of the currently playing item, if any. */
    static QString currentMRL(vlc_player_t *player);

    /* Current media timestamp in milliseconds. */
    static qint64 currentTimeMs(vlc_player_t *player);
};

} // namespace paintbrush

#endif // PB_FRAME_CAPTURE_HPP
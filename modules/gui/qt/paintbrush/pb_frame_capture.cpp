/*****************************************************************************
 * pb_frame_capture.cpp : NAI Paintbrush — media frame bridge
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_frame_capture.hpp"

#include <vlc_player.h>
#include <vlc_vout.h>
#include <vlc_block.h>
#include <vlc_input_item.h>

namespace paintbrush {

PlayerLock::PlayerLock(vlc_player_t *player_) noexcept
    : player(player_)
{
    if (player)
        vlc_player_Lock(player);
}

PlayerLock::~PlayerLock()
{
    if (player)
        vlc_player_Unlock(player);
}

QString FrameCaptureService::currentMRL(vlc_player_t *player)
{
    if (!player)
        return {};
    PlayerLock lock(player);
    input_item_t *item = vlc_player_GetCurrentMedia(player);
    if (!item)
        return {};
    char *mrl = input_item_GetURI(item);
    QString result = mrl ? QString::fromUtf8(mrl) : QString();
    free(mrl);
    return result;
}

qint64 FrameCaptureService::currentTimeMs(vlc_player_t *player)
{
    if (!player)
        return 0;
    PlayerLock lock(player);
    vlc_tick_t time = vlc_player_GetTime(player);
    return time == VLC_TICK_INVALID ? 0 : time / 1000;
}

QImage FrameCaptureService::captureCurrentFrame(vlc_player_t *player)
{
    if (!player)
        return {};

    vout_thread_t *vout = nullptr;
    {
        PlayerLock lock(player);
        vout = vlc_player_vout_Hold(player);
    }
    if (!vout)
        return {};

    block_t *block = nullptr;
    /* Request an encoded PNG snapshot; timeout generous for slow systems. */
    const int ret = vout_GetSnapshot(vout, &block, nullptr, nullptr, "png",
                                     VLC_TICK_FROM_MS(2000));
    vout_Release(vout);

    if (ret != VLC_SUCCESS || !block)
        return {};

    QImage image = QImage::fromData(block->p_buffer, block->i_buffer, "PNG");
    block_Release(block);
    return image;
}

MediaFrameRef FrameCaptureService::currentMediaRef(vlc_player_t *player)
{
    MediaFrameRef ref;
    if (!player)
        return ref;

    PlayerLock lock(player);

    input_item_t *item = vlc_player_GetCurrentMedia(player);
    if (item)
    {
        char *mrl = input_item_GetURI(item);
        ref.mrl = mrl ? QString::fromUtf8(mrl) : QString();
        free(mrl);
        char *title = input_item_GetTitleFbName(item);
        ref.title = title ? QString::fromUtf8(title) : QString();
        free(title);
    }

    vlc_tick_t time = vlc_player_GetTime(player);
    ref.timeMs = time == VLC_TICK_INVALID ? 0 : time / 1000;
    return ref;
}

} // namespace paintbrush
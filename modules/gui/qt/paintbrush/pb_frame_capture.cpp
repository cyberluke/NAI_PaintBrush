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
#include <vlc_preparser.h>
#include <vlc_picture.h>
#include <vlc_fourcc.h>

#include <QDeadlineTimer>

#include <cstring>
#include <cstdio>

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
    /* Generic viewer screenshot utility only: the edit/AI pipeline never
     * reaches this PNG snapshot path anymore (A9). */
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

FrameRef FrameCaptureService::currentMediaRef(vlc_player_t *player)
{
    FrameRef ref;
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
    ref.ptsMs = time == VLC_TICK_INVALID ? 0 : time / 1000;
    return ref;
}

/* ------------------------------------------------------- EditFrameSource */

namespace {

constexpr qint64 REQUEST_TIMEOUT_MS = 8000; /* preparser per-request deadline */
constexpr qint64 EDIT_CACHE_BUDGET_BYTES = 128LL * 1024 * 1024;

/* YUV -> RGB (BT.601 limited range) used by the direct picture converter.
 * No PNG encode/decode anywhere on this path (A9). */
inline int yuvToRgb(int c, int d, int e)
{
    const int v = (298 * c + 409 * e + 128) >> 8;
    return qBound(0, v, 255);
}

} // namespace

struct EditFrameSource::RequestCtx
{
    bool done = false;
    int status = VLC_EGENERIC;
    picture_t *pic = nullptr;
};

/* Callback invoked on the preparser thumbnailer thread with the raw
 * decoded picture (owned by the thumbnailer; hold to use past the
 * callback). Locks the source mutex so the waiting request() thread
 * observes `done` without a data race, then wakes it. `data` points at a
 * stack {EditFrameSource*, RequestCtx} pair owned by the waiting request().
 * Requests are serialized by the source mutex, so the pair outlives the
 * callback. */
void EditFrameSource::onThumbnailEnded(vlc_preparser_req *req, int status,
                                       picture_t *thumbnail, void *data)
{
    Q_UNUSED(req);
    struct CallbackData
    {
        const EditFrameSource *self;
        RequestCtx *ctx;
    };
    auto *cb = static_cast<CallbackData *>(data);
    QMutexLocker lock(&cb->self->m_mutex);
    if (thumbnail)
        cb->ctx->pic = picture_Hold(thumbnail);
    cb->ctx->status = status;
    cb->ctx->done = true;
    cb->self->m_cond.wakeAll();
}

EditFrameSource::EditFrameSource(QObject *parent)
    : QObject(parent)
    , m_cache(EDIT_CACHE_BUDGET_BYTES)
{
}

EditFrameSource::~EditFrameSource()
{
    shutdown();
}

void EditFrameSource::configure(vlc_object_t *obj)
{
    if (m_preparser)
        return;

    m_obj = obj;
    m_ptsToleranceMs = qBound<qint64>(
        qint64(20), qint64(pbConfigInt(obj, "paintbrush-edit-pts-tolerance-ms")),
        qint64(2000));
    m_hwdecPolicy = qBound(0, pbConfigInt(obj, "avcodec-hwdec"), 2);

    struct vlc_preparser_cfg cfg;
    cfg.types = VLC_PREPARSER_TYPE_THUMBNAIL;
    cfg.max_parser_threads = 0;
    cfg.max_thumbnailer_threads = 1; /* serialize exact-frame decodes */
    cfg.timeout = VLC_TICK_FROM_MS(REQUEST_TIMEOUT_MS);
    cfg.external_process = false;

    m_preparser = vlc_preparser_New(VLC_OBJECT(obj), &cfg);
    pbLogInfo(obj, "media", "edit frame source ready (pts tolerance %lld ms, "
              "avcodec-hwdec policy %d %s)",
              (long long)m_ptsToleranceMs, m_hwdecPolicy,
              m_hwdecPolicy == 1 ? "(software)" : "(auto/hardware)");
}

void EditFrameSource::shutdown()
{
    QMutexLocker lock(&m_mutex);
    if (m_preparser)
    {
        vlc_preparser_Delete(m_preparser);
        m_preparser = nullptr;
    }
    m_cache.clear();
    m_last = FrameRef();
}

bool EditFrameSource::canDecode() const
{
    QMutexLocker lock(&m_mutex);
    return m_preparser != nullptr;
}

FrameRef EditFrameSource::last() const
{
    QMutexLocker lock(&m_mutex);
    return m_last;
}

void EditFrameSource::clearCache()
{
    QMutexLocker lock(&m_mutex);
    m_cache.clear();
}

DecodedFrame EditFrameSource::request(const FrameRef &ref) const
{
    DecodedFrame out;
    out.requestedMs = ref.ptsMs;
    if (!ref.isValid())
        return out;

    const QString key = ref.mrl + QLatin1Char('|') + QString::number(ref.ptsMs);

    /* Serialize the whole request: cache access and the decode wait share
     * one mutex. The thumbnailer callback takes the same mutex to publish
     * its result, which is safe because wait() releases it while blocked. */
    QMutexLocker lock(&m_mutex);

    if (m_cache.contains(key))
    {
        out = m_cache.get(key);
        out.requestedMs = ref.ptsMs;
        if (out.ok())
        {
            m_last = FrameRef{ref.mrl, ref.title, out.ptsMs,
                              out.image.width(), out.image.height()};
        }
        return out;
    }

    if (!m_preparser)
        return out;

    RequestCtx ctx;
    struct CallbackData
    {
        EditFrameSource *self;
        RequestCtx *ctx;
    };
    CallbackData cbData{const_cast<EditFrameSource *>(this), &ctx};

    input_item_t *item = input_item_New(qUtf8Printable(ref.mrl),
                                        qUtf8Printable(ref.title));
    if (!item)
        return out;

    /* A18: apply the generic avcodec-hwdec tri-state to the EDIT decoder
     * only. The per-item option reaches the avcodec decoder inside the
     * thumbnailing input; arg.hw_dec mirrors the same policy for the input
     * pipeline itself. */
    {
        char opt[32];
        snprintf(opt, sizeof(opt), ":avcodec-hwdec=%d", m_hwdecPolicy);
        input_item_AddOption(item, opt, VLC_INPUT_OPTION_TRUSTED);
    }

    struct vlc_thumbnailer_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.seek.type = vlc_thumbnailer_arg::seek::VLC_THUMBNAILER_SEEK_TIME;
    arg.seek.time = VLC_TICK_FROM_MS(qMax<qint64>(0, ref.ptsMs));
    arg.seek.speed = vlc_thumbnailer_arg::seek::VLC_THUMBNAILER_SEEK_PRECISE;
    arg.hw_dec = (m_hwdecPolicy != 1); /* Disabled -> software; else allow HW */

    static const struct vlc_thumbnailer_cbs cbs = {
        .on_ended = EditFrameSource::onThumbnailEnded,
    };

    vlc_preparser_req *req =
        vlc_preparser_req_NewThumbnail(m_preparser, item, &arg, &cbs, &cbData);
    input_item_Release(item);
    if (!req)
        return out;

    if (vlc_preparser_Submit(m_preparser, req) != VLC_SUCCESS)
    {
        vlc_preparser_req_Release(req);
        return out;
    }

    /* Wait for the decode. The preparser guarantees the callback fires
     * within its timeout; this is a defense-in-depth deadline. */
    const QDeadlineTimer deadline(REQUEST_TIMEOUT_MS + 2000);
    while (!ctx.done)
    {
        if (!m_cond.wait(&m_mutex, deadline))
            break; /* defensive; the preparser should have fired already */
    }

    if (!ctx.done)
    {
        /* timed out against all odds: cancel and let the callback land */
        vlc_preparser_Cancel(m_preparser, req);
        while (!ctx.done)
            m_cond.wait(&m_mutex, QDeadlineTimer(1000));
    }
    vlc_preparser_req_Release(req); /* safe: request terminated */

    if (ctx.status != VLC_SUCCESS || !ctx.pic)
        return out;

    out.ptsMs = ctx.pic->date / 1000;
    out.image = pictureToQImage(ctx.pic, m_obj);
    picture_Release(ctx.pic);

    if (out.image.isNull())
        return out;

    out.ptsVerified = ptsWithinTolerance(ref.ptsMs, out.ptsMs,
                                         m_ptsToleranceMs);
    if (out.ptsVerified)
    {
        m_last = FrameRef{ref.mrl, ref.title, out.ptsMs,
                          out.image.width(), out.image.height()};
        m_cache.insert(key, out);
    }
    else if (m_obj)
    {
        pbLogWarn(m_obj, "media",
                  "decoded PTS %lld ms for requested %lld ms is outside the "
                  "%lld ms tolerance; frame rejected",
                  (long long)out.ptsMs, (long long)ref.ptsMs,
                  (long long)m_ptsToleranceMs);
    }
    return out;
}

QImage EditFrameSource::pictureToQImage(const picture_t *pic,
                                        vlc_object_t *logObj)
{
    if (!pic)
        return {};
    const video_format_t &fmt = pic->format;
    const int w = fmt.i_visible_width;
    const int h = fmt.i_visible_height;
    const int xo = fmt.i_x_offset;
    const int yo = fmt.i_y_offset;
    if (w <= 0 || h <= 0)
        return {};

    QImage out(w, h, QImage::Format_RGB32);

    switch (fmt.i_chroma)
    {
    case VLC_CODEC_I420:
    case VLC_CODEC_YV12:
    {
        /* planar YUV 4:2:0 */
        const plane_t &Y = pic->p[0];
        const plane_t &U = pic->p[1];
        const plane_t &V = pic->p[2];
        for (int y = 0; y < h; ++y)
        {
            const uchar *py = Y.p_pixels + (y + yo) * Y.i_pitch + xo;
            const uchar *pu = U.p_pixels + ((y + yo) / 2) * U.i_pitch + xo / 2;
            const uchar *pv = V.p_pixels + ((y + yo) / 2) * V.i_pitch + xo / 2;
            QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < w; ++x)
            {
                const int C = py[x] - 16;
                const int D = pu[x / 2] - 128;
                const int E = pv[x / 2] - 128;
                dst[x] = qRgb(yuvToRgb(C, D, E),
                              qBound(0, (298 * C - 100 * D - 208 * E + 128) >> 8,
                                     255),
                              qBound(0, (298 * C + 516 * D + 128) >> 8, 255));
            }
        }
        return out;
    }
    case VLC_CODEC_NV12:
    case VLC_CODEC_NV21:
    {
        const plane_t &Y = pic->p[0];
        const plane_t &UV = pic->p[1];
        const bool nv21 = fmt.i_chroma == VLC_CODEC_NV21;
        for (int y = 0; y < h; ++y)
        {
            const uchar *py = Y.p_pixels + (y + yo) * Y.i_pitch + xo;
            const uchar *puv =
                UV.p_pixels + ((y + yo) / 2) * UV.i_pitch + xo;
            QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < w; ++x)
            {
                const int C = py[x] - 16;
                const int D = puv[x / 2 * 2 + (nv21 ? 1 : 0)] - 128;
                const int E = puv[x / 2 * 2 + (nv21 ? 0 : 1)] - 128;
                dst[x] = qRgb(yuvToRgb(C, D, E),
                              qBound(0, (298 * C - 100 * D - 208 * E + 128) >> 8,
                                     255),
                              qBound(0, (298 * C + 516 * D + 128) >> 8, 255));
            }
        }
        return out;
    }
    case VLC_CODEC_UYVY:
    case VLC_CODEC_YUYV:
    {
        /* packed YUV 4:2:2. UYVY: U Y V Y per pair; YUYV: Y U Y V. The
         * chroma pair covers two pixels; Y is per-pixel. */
        const plane_t &pl = pic->p[0];
        const bool uyvy = fmt.i_chroma == VLC_CODEC_UYVY;
        for (int y = 0; y < h; ++y)
        {
            const uchar *p = pl.p_pixels + (y + yo) * pl.i_pitch + xo * 2;
            QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < w; ++x)
            {
                const uchar *px = p + (x / 2) * 4;
                const int C = px[(x & 1) * 2 + (uyvy ? 1 : 0)] - 16;
                const int D = (uyvy ? px[0] : px[1]) - 128;
                const int E = (uyvy ? px[2] : px[3]) - 128;
                dst[x] = qRgb(yuvToRgb(C, D, E),
                              qBound(0, (298 * C - 100 * D - 208 * E + 128) >> 8,
                                     255),
                              qBound(0, (298 * C + 516 * D + 128) >> 8, 255));
            }
        }
        return out;
    }
    case VLC_CODEC_RGBA:
    case VLC_CODEC_BGRA:
    {
        const plane_t &pl = pic->p[0];
        const bool rgba = fmt.i_chroma == VLC_CODEC_RGBA;
        for (int y = 0; y < h; ++y)
        {
            const uchar *p = pl.p_pixels + (y + yo) * pl.i_pitch + xo * 4;
            QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < w; ++x)
            {
                const uchar *px = p + x * 4;
                dst[x] = rgba ? qRgb(px[0], px[1], px[2])
                              : qRgb(px[2], px[1], px[0]);
            }
        }
        return out;
    }
    case VLC_CODEC_RGB24:
    {
        const plane_t &pl = pic->p[0];
        for (int y = 0; y < h; ++y)
        {
            const uchar *p = pl.p_pixels + (y + yo) * pl.i_pitch + xo * 3;
            QRgb *dst = reinterpret_cast<QRgb *>(out.scanLine(y));
            for (int x = 0; x < w; ++x)
                dst[x] = qRgb(p[x * 3], p[x * 3 + 1], p[x * 3 + 2]);
        }
        return out;
    }
    default:
        if (logObj)
        {
            char fourcc[5] = {0, 0, 0, 0, 0};
            memcpy(fourcc, &fmt.i_chroma, 4);
            pbLogWarn(logObj, "media",
                      "unsupported edit frame chroma %s", fourcc);
        }
        return {};
    }
}

} // namespace paintbrush
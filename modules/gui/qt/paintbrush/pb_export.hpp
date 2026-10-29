/*****************************************************************************
 * pb_export.hpp : NAI Paintbrush — export helpers
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_EXPORT_HPP
#define PB_EXPORT_HPP

#include "pb_common.hpp"

#include <QImage>
#include <QString>

namespace paintbrush {

class PaintbrushDocument;

/* Image export using Qt's image writers (PNG/JPEG/WebP as available). */
class ExportService
{
public:
    /* Flatten the document and write it to path. `format` may be empty to
     * guess from the extension. Returns an error message or empty. */
    static QString exportImage(PaintbrushDocument *doc, const QString &path,
                               const QString &format = {});

    /* Flatten with export options: scale multiplier (1 = original), crop
     * to content bounds, and a background fill for opaque formats. */
    static QString exportImageOptions(PaintbrushDocument *doc, const QString &path,
                                      const QString &format, double scale,
                                      bool cropToContent,
                                      const QColor &background = QColor(Qt::transparent));

    /* Write a single image to path (transparent PNG keeps alpha). */
    static QString exportImageFile(const QImage &image, const QString &path,
                                   const QString &format = {});

    /* Write a mask (binary, white foreground) to a PNG file. */
    static QString exportMask(const QImage &mask, const QString &path);

    /* Trim transparent bounds of an RGBA image and write it. */
    static QString exportCutout(const QImage &cutout, const QString &path);

    /* Living Harvest (stage 3): render a living poster loop as a PNG frame
     * sequence. Frames are deterministic for a fixed doc (same seed, same
     * time grid) and transparent (paper comes from the document renderer
     * when `paper` is true; `paper=false` yields transparent frames for
     * manual compositing). Writes `dir/<stem>_%04d.png`, 0-based.
     * Returns an error message or empty. */
    static QString exportLivingPoster(PaintbrushDocument *doc, const QString &dir,
                                      const QString &stem,
                                      qint64 durationMs, int fps,
                                      bool paper, double scale);

    /* Living Showroom: a REAL muxed video file over the canonical PNG
     * sequence (which is kept as the deterministic intermediate).
     *  - "mp4":  paper frames -> H.264/yuv420p (+faststart), share-ready;
     *  - "webm": transparent frames -> VP9/yuva420p (alpha-capable).
     * Requires an ffmpeg binary with the matching encoder; otherwise a
     * clear error is returned (no fake export). Returns error or empty. */
    static QString exportLivingVideo(PaintbrushDocument *doc, const QString &dir,
                                     const QString &stem, qint64 durationMs, int fps,
                                     const QString &format, const QString &outPath);
    /* True when exportLivingVideo(format) can produce a real file on this
     * machine (ffmpeg found + encoder available). Cached per format. */
    static bool canExportLivingVideo(const QString &format);
};

} // namespace paintbrush

#endif // PB_EXPORT_HPP
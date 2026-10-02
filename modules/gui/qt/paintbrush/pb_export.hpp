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
};

} // namespace paintbrush

#endif // PB_EXPORT_HPP
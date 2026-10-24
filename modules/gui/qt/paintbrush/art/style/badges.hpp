/*****************************************************************************
 * badges.hpp : NAI Paintbrush — tiny curated procedural badge library
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_BADGES_HPP
#define PB_ART_BADGES_HPP

#include <QString>
#include <QStringList>
#include <QPainterPath>
#include <QImage>
#include <QColor>
#include <QSizeF>

namespace paintbrush {
namespace art {
namespace badges {

/* The curated accent set (spec §22). Vector/procedural, recolorable,
 * scalable, and shaped so one badge can become flat, chrome, glitter or matte
 * through the Finish/material runtime without storing raster copies. */
enum class Kind
{
    Heart = 0,
    Star,
    Sparkle,
    Starburst,
    Tape,
    Arrow,
    Underline,
    Halo,
    BrokenHeart,
    Crown,
    GlossyLabel,
    Count
};

QString kindName(Kind k);
Kind kindFromName(const QString &name);
QStringList allNames();

/* The badge's outline path, fit to `size` with its origin at (0,0). `seed`
 * drives any organic variation (tape tear, underline wobble) so the same
 * badge reproduces identically (spec §37). */
QPainterPath path(Kind k, const QSizeF &size, unsigned seed = 1);

/* Rasterize the badge: fill color (+ optional stroke) at `scale`. Returns a
 * straight-alpha ARGB32 image sized `size * scale` (+ stroke padding). */
QImage render(Kind k, const QSizeF &size, const QColor &fill,
              const QColor &stroke = QColor(0, 0, 0, 0), float strokeWidth = 0.0f,
              float scale = 1.0f, unsigned seed = 1);

/* Badge families a Mood suggests (spec §23 "accent/badge family"). */
QList<Kind> family(const QString &familyId);

} // namespace badges
} // namespace art
} // namespace paintbrush

#endif // PB_ART_BADGES_HPP

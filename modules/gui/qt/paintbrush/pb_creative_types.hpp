/*****************************************************************************
 * pb_creative_types.hpp : NAI Paintbrush — Creative Pad value types
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_CREATIVE_TYPES_HPP
#define PB_CREATIVE_TYPES_HPP

/* Plain value types shared by the live pad, the pure recipe builder and the
 * asynchronous commit. Deliberately VLC/QObject-free and cheap to include:
 * only the recipe translation unit (and its standalone harness) needs the
 * 900-line builder itself. */

#include "pb_blend.hpp"

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

namespace paintbrush {

/* How one synthesized piece reacts to the pad. Offsets are normalized to the
 * stage size; opacity/scale/rotation are additive around the base value. */
struct CreativeMotion
{
    float opacityX = 0.0f;
    float opacityY = 0.0f;
    float opacityEnergy = 0.0f;
    float opacityHold = 0.0f;
    float dxX = 0.0f;
    float dyY = 0.0f;
    float dxEnergy = 0.0f;
    float dyEnergy = 0.0f;
    float scaleY = 0.0f;
    float scaleEnergy = 0.0f;
    float scaleHold = 0.0f;
    float rotationEnergy = 0.0f;
    float rotationHold = 0.0f;
};

/* One editable output piece. The same vector drives both the live preview
 * and the committed document layers. */
struct CreativePiece
{
    QString name;
    QString role; /* backdrop / echo / subject / rim / accent / finish */
    QImage image;
    QRectF rect; /* stage coordinates */
    QPointF pivot; /* motion pivot; null = rect center */
    int blendMode = BLEND_NORMAL;
    float opacity = 1.0f;
    CreativeMotion motion;
    QVariantMap metadata;
};

/* Live pad state captured for one build. */
struct CreativeFrame
{
    float x = 0.5f;
    float y = 0.5f;
    float energy = 0.0f;
    bool hold = false;
    float time = 0.0f;
};

/* Immutable snapshot of everything one creative build needs. `stage` is the
 * build resolution: the native canvas for a commit, the capped panel for the
 * live preview. `previewSize` is the bounded target for progressive and final
 * preview images (never the render resolution). */
struct CreativeBuildInput
{
    QString patchId;
    QString patchName;
    QImage subject;      /* cutout; QImage is implicitly shared + read-only */
    /* Grayscale detail-preservation mask in the same pixel space as
     * `subject`: 255 keeps the original pixels (skin, hair, accessories),
     * 0 receives the material. Empty means no protection. The fashion
     * recipes pass this to holographicMaterial(); it never changes output
     * alpha and never replaces the subject coverage. */
    QImage protection;
    QRectF subjectRect;  /* stage coordinates */
    QSize stage;         /* build stage (native canvas for commits) */
    QSize previewSize;   /* bounded preview target */
    quint32 seed = 1;
    CreativeFrame frame; /* exact pad state captured at request time */
};

} /* namespace paintbrush */

#endif /* PB_CREATIVE_TYPES_HPP */

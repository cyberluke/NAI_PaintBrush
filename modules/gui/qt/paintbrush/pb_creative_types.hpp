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

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVariantMap>
#include <QVector>
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
    float dxHold = 0.0f;
    float dyHold = 0.0f;
    float scaleY = 0.0f;
    float scaleEnergy = 0.0f;
    float scaleHold = 0.0f;
    float rotationEnergy = 0.0f;
    float rotationHold = 0.0f;
};

/* A preview-only vector field. The palette, mask and profile travel with the
 * piece so compose() can evaluate the graph at the current pad frame without a
 * rebuild. Commits never contain this piece; they use the baked field. */
struct CreativeVectorOverlay
{
    QString profileId;
    int style = 0; /* 0 = sparks, 1 = Voltage bolts + sparks */
    QImage mask;   /* subject-on-stage grayscale */
    QVector<QColor> palette;
    QRectF subjectRect;
    QSize stage;
    quint32 seed = 0;
};

/* One editable output piece. Baked pieces drive commits; preview builds may
 * also carry one dynamic vector overlay evaluated at compose time. */
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
    bool dynamicVector = false;
    CreativeVectorOverlay vectorOverlay;
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
 * build resolution: the native canvas for a commit, the 4K-class preview panel
 * for the live preview. `previewSize` is the target for the progressive/final
 * preview composite, which is a scaled view of the pieces, never a second
 * recipe render. */
struct CreativeBuildInput
{
    QString patchId;
    QString patchName;
    /* Catalog accents, so the generic vector overlay belongs to the selected
     * look instead of painting one global cyber palette over every mode. */
    QColor accent{ 0, 234, 255 };
    QColor accentAlt{ 253, 48, 199 };
    QImage subject;      /* cutout; QImage is implicitly shared + read-only */
    /* Grayscale detail-preservation mask in the same pixel space as
     * `subject`: 255 keeps the original pixels (skin, hair, accessories),
     * 0 receives the material. Empty means no protection. The fashion
     * recipes pass this to holographicMaterial(); it never changes output
     * alpha and never replaces the subject coverage. */
    QImage protection;
    QRectF subjectRect;  /* stage coordinates */
    QSize stage;         /* build stage (native canvas for commits) */
    QSize previewSize;   /* 4K-class preview target */
    quint32 seed = 1;
    CreativeFrame frame; /* exact pad state captured at request time */
    /* Commit grouping: every layer of one look shares this id so history,
     * resume/delete and one-undo can find it. */
    QString group;
    int sourceLayerId = -1;
    int sourceHarvestId = -1;
    /* Preview builds request a live vector overlay; commits request the baked
     * fields so the document layers stay deterministic and one-undo. */
    bool preview = false;
};

} /* namespace paintbrush */

#endif /* PB_CREATIVE_TYPES_HPP */

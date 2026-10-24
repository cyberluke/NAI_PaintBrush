/*****************************************************************************
 * mood.hpp : NAI Paintbrush — moods are art direction, not filters
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_MOOD_HPP
#define PB_ART_MOOD_HPP

#include <QString>
#include <QColor>
#include <QVector>
#include <QVariantMap>

#include "../runtime/light_rig.hpp"

namespace paintbrush {
namespace art {

/* Palette roles a mood assigns (spec §43 "palette roles"). Badges, text and
 * glitter pick colors by ROLE so one mood recolors the whole composition
 * coherently without touching raw artwork. */
struct MoodPalette
{
    QColor primary;     /* headline / main accent            */
    QColor secondary;   /* supporting accent                 */
    QColor accent;      /* small pops (badges, sparkle)      */
    QColor ink;         /* text / outline on this paper      */
    QColor glitter;     /* default glitter grain color       */
};

/* A Mood changes coherent defaults across PaperStock, palette, LightRig,
 * suggested typography, shadow style, glitter characteristics, accent/badge
 * family and motion finish strength (spec §23). It never flattens or
 * overwrites the user's raw artwork — it is a parameter system. */
struct Mood
{
    QString id;                 /* "saturated.pop" ... */
    QString displayName;
    QString paperId;            /* PaperStock id */
    ArtLightRig light;
    MoodPalette palette;
    QString typographyRole;     /* TypographyStyle role id */
    QString badgeFamily;        /* "pop" | "y2k" | "luxury" | "emo" | "chrome" | "editorial" */
    /* finish defaults */
    float shadowBlur = 8.0f;    /* world units */
    float shadowOpacity = 0.45f;
    float glowAllowance = 0.0f; /* 0 = Boost never adds glow; 1 = free */
    float motionStrength = 1.0f; /* multiplies Boost's directed-blur distance */
    /* glitter defaults */
    float glitterShimmer = 0.55f;
    float glitterFlake = 0.8f;
    float glitterHolographic = 0.0f;

    QVariantMap toVariant() const;
};

namespace mood {

/* The initial descriptive moods (spec §23): exactly six. */
QVector<const Mood *> presets();
const Mood *findById(const QString &id);
/* The default mood for a fresh document ("Warm Editorial"). */
const Mood &defaultMood();

} // namespace mood
} // namespace art
} // namespace paintbrush

#endif // PB_ART_MOOD_HPP

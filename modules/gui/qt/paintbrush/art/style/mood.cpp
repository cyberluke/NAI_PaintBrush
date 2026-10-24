/*****************************************************************************
 * mood.cpp : NAI Paintbrush — mood presets
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "mood.hpp"

namespace paintbrush {
namespace art {

QVariantMap Mood::toVariant() const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("name"), displayName);
    m.insert(QStringLiteral("paper"), paperId);
    m.insert(QStringLiteral("typography"), typographyRole);
    m.insert(QStringLiteral("badgeFamily"), badgeFamily);
    m.insert(QStringLiteral("primary"), palette.primary.name());
    m.insert(QStringLiteral("secondary"), palette.secondary.name());
    m.insert(QStringLiteral("accent"), palette.accent.name());
    m.insert(QStringLiteral("ink"), palette.ink.name());
    m.insert(QStringLiteral("glitter"), palette.glitter.name());
    m.insert(QStringLiteral("shadowBlur"), double(shadowBlur));
    m.insert(QStringLiteral("shadowOpacity"), double(shadowOpacity));
    m.insert(QStringLiteral("glowAllowance"), double(glowAllowance));
    m.insert(QStringLiteral("motionStrength"), double(motionStrength));
    return m;
}

namespace mood {

namespace {

ArtLightRig rig(float dx, float dy, float elevation, float softness,
                float ambient, float intensity, float temperature)
{
    ArtLightRig r;
    r.direction = QVector3D(dx, dy, 0.6f);
    r.elevationDeg = elevation;
    r.softness = softness;
    r.ambient = ambient;
    r.intensity = intensity;
    r.temperature = temperature;
    return r;
}

Mood make(const char *id, const char *name, const char *paper,
          const ArtLightRig &light, const MoodPalette &pal,
          const char *typo, const char *badges,
          float shadowBlur, float shadowOpacity, float glowAllowance,
          float motionStrength, float shimmer, float flake, float holo)
{
    Mood m;
    m.id = QString::fromLatin1(id);
    m.displayName = QString::fromLatin1(name);
    m.paperId = QString::fromLatin1(paper);
    m.light = light;
    m.palette = pal;
    m.typographyRole = QString::fromLatin1(typo);
    m.badgeFamily = QString::fromLatin1(badges);
    m.shadowBlur = shadowBlur;
    m.shadowOpacity = shadowOpacity;
    m.glowAllowance = glowAllowance;
    m.motionStrength = motionStrength;
    m.glitterShimmer = shimmer;
    m.glitterFlake = flake;
    m.glitterHolographic = holo;
    return m;
}

const QVector<Mood> &all()
{
    static const QVector<Mood> moods = {
        /* Saturated Pop: hot pink paper, hard bright key, black/white ink,
         * chrome accent, condensed impact type, strong motion. */
        make("saturated.pop", "Saturated Pop", "saturated.pink",
             rig(0.6f, -0.5f, 55.0f, 0.2f, 0.28f, 1.15f, 0.45f),
             { QColor(0xFF, 0xFF, 0xFF), QColor(0x12, 0x12, 0x14), QColor(0xFF, 0xE0, 0x3C),
               QColor(0x12, 0x12, 0x14), QColor(0xF5, 0xF5, 0xF7) },
             "condensed.impact", "pop", 6.0f, 0.55f, 0.2f, 1.25f, 0.7f, 0.7f, 0.15f),
        /* Bubblegum Y2K: soft pink, pearlescent, soft light, pastel pops,
         * holographic glitter, rounded type, sticker shadows. */
        make("bubblegum.y2k", "Bubblegum Y2K", "bubblegum.y2k",
             rig(0.4f, -0.6f, 60.0f, 0.55f, 0.36f, 1.0f, 0.6f),
             { QColor(0xFF, 0x5F, 0xA2), QColor(0x7D, 0xD3, 0xFF), QColor(0xFF, 0xF2, 0x5C),
               QColor(0x3A, 0x1F, 0x2E), QColor(0xFF, 0xB8, 0xE0) },
             "soft.y2k", "y2k", 5.0f, 0.5f, 0.6f, 0.9f, 0.8f, 0.9f, 0.6f),
        /* Luxury Greige Gold: greige cotton, warm low raking light, gold
         * glitter and chrome, editorial serif, restrained finish. */
        make("luxury.greige.gold", "Luxury Greige Gold", "luxury.greige",
             rig(0.75f, -0.3f, 32.0f, 0.4f, 0.30f, 1.0f, 0.75f),
             { QColor(0xC9, 0xA2, 0x4A), QColor(0x2B, 0x26, 0x22), QColor(0xE8, 0xC6, 0x6A),
               QColor(0x2B, 0x26, 0x22), QColor(0xD9, 0xB1, 0x4E) },
             "editorial", "luxury", 10.0f, 0.35f, 0.0f, 0.8f, 0.5f, 0.6f, 0.0f),
        /* Emo Ash: cool grey grain, hard cool light, black/silver, emo serif,
         * long soft shadows, silver glitter. */
        make("emo.ash", "Emo Ash", "emo.ash",
             rig(-0.5f, -0.6f, 40.0f, 0.3f, 0.22f, 0.95f, 0.3f),
             { QColor(0x0E, 0x0E, 0x10), QColor(0xC8, 0xC8, 0xCC), QColor(0x9A, 0x1F, 0x2E),
               QColor(0x0E, 0x0E, 0x10), QColor(0xBF, 0xC3, 0xC9) },
             "emo.serif", "emo", 12.0f, 0.5f, 0.1f, 1.1f, 0.45f, 0.6f, 0.0f),
        /* Midnight Chrome: matte black, cool high-contrast key, chrome/ice
         * accents, clean future type, glow allowed, holographic flakes. */
        make("midnight.chrome", "Midnight Chrome", "matte.black",
             rig(0.3f, -0.8f, 50.0f, 0.25f, 0.18f, 1.2f, 0.35f),
             { QColor(0xE6, 0xEE, 0xF5), QColor(0x6F, 0x8F, 0xB0), QColor(0x3C, 0xE0, 0xFF),
               QColor(0xF2, 0xF5, 0xF8), QColor(0xD7, 0xE3, 0xF0) },
             "clean.future", "chrome", 8.0f, 0.6f, 0.8f, 1.2f, 0.85f, 0.7f, 0.45f),
        /* Warm Editorial: cream art stock, warm soft daylight, ink + brick
         * accent, editorial type, gentle finish. */
        make("warm.editorial", "Warm Editorial", "warm.editorial.cream",
             rig(0.55f, -0.45f, 48.0f, 0.5f, 0.34f, 1.0f, 0.7f),
             { QColor(0x1F, 0x1B, 0x18), QColor(0xB3, 0x4A, 0x2F), QColor(0xE0, 0xA4, 0x3C),
               QColor(0x1F, 0x1B, 0x18), QColor(0xD1, 0xA3, 0x4B) },
             "editorial", "editorial", 8.0f, 0.4f, 0.0f, 0.9f, 0.45f, 0.7f, 0.0f),
    };
    return moods;
}

} // namespace

QVector<const Mood *> presets()
{
    QVector<const Mood *> out;
    for (const Mood &m : all())
        out.append(&m);
    return out;
}

const Mood *findById(const QString &id)
{
    for (const Mood &m : all())
        if (m.id == id)
            return &m;
    return nullptr;
}

const Mood &defaultMood()
{
    return *findById(QStringLiteral("warm.editorial"));
}

} // namespace mood
} // namespace art
} // namespace paintbrush

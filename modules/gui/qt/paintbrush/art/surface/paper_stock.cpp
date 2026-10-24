/*****************************************************************************
 * paper_stock.cpp : NAI Paintbrush — paper presets
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "paper_stock.hpp"

#include <QVector>

namespace paintbrush {
namespace art {
namespace paper {

namespace {

PaperStock base(const QString &id, const QString &name,
                const QColor &color, std::uint32_t seed,
                float fiberStrength, float tooth, float roughness,
                float absorption, float microHeight, float specular,
                float warmth, float fiberScale)
{
    PaperStock p;
    p.id = id;
    p.displayName = name;
    p.baseColor = color;
    p.seed = seed;
    p.fiberStrength = fiberStrength;
    p.tooth = tooth;
    p.roughness = roughness;
    p.absorption = absorption;
    p.microHeight = microHeight;
    p.specular = specular;
    p.warmth = warmth;
    p.fiberScale = fiberScale;
    return p;
}

} // namespace

const PaperStock &luxuryGreige() { return *findById(QStringLiteral("luxury.greige")); }
const PaperStock &saturatedPink() { return *findById(QStringLiteral("saturated.pink")); }
const PaperStock &bubblegumY2k() { return *findById(QStringLiteral("bubblegum.y2k")); }
const PaperStock &emoAsh() { return *findById(QStringLiteral("emo.ash")); }
const PaperStock &warmEditorialCream() { return *findById(QStringLiteral("warm.editorial.cream")); }
const PaperStock &matteBlack() { return *findById(QStringLiteral("matte.black")); }

const PaperStock *findById(const QString &id)
{
    static PaperStock s[6] = {
        base(QStringLiteral("luxury.greige"), QStringLiteral("Luxury Greige"),
             QColor(0xCF, 0xC6, 0xB8), 0xA11u, 0.62f, 0.55f, 0.80f,
             0.45f, 0.14f, 0.18f, +0.08f, 0.045f),
        base(QStringLiteral("saturated.pink"), QStringLiteral("K-Pop Pink"),
             QColor(0xF2, 0x3B, 0x63), 0xB22u, 0.28f, 0.30f, 0.35f,
             0.25f, 0.07f, 0.28f, +0.05f, 0.05f),
        base(QStringLiteral("bubblegum.y2k"), QStringLiteral("Bubblegum Y2K"),
             QColor(0xF6, 0xA6, 0xC1), 0xC33u, 0.22f, 0.24f, 0.30f,
             0.20f, 0.06f, 0.32f, +0.10f, 0.055f),
        base(QStringLiteral("emo.ash"), QStringLiteral("Emo Ash"),
             QColor(0x86, 0x85, 0x84), 0xD44u, 0.55f, 0.60f, 0.90f,
             0.50f, 0.16f, 0.10f, -0.12f, 0.04f),
        base(QStringLiteral("warm.editorial.cream"), QStringLiteral("Editorial Cream"),
             QColor(0xF3, 0xEB, 0xDA), 0xE55u, 0.45f, 0.42f, 0.60f,
             0.55f, 0.09f, 0.20f, +0.15f, 0.048f),
        base(QStringLiteral("matte.black"), QStringLiteral("Matte Black"),
             QColor(0x16, 0x16, 0x18), 0xF66u, 0.50f, 0.50f, 0.82f,
             0.60f, 0.05f, 0.30f, +0.02f, 0.042f),
    };
    static const int count = int(sizeof(s) / sizeof(s[0]));
    for (int i = 0; i < count; ++i)
    {
        if (s[i].id == id)
            return &s[i];
    }
    return nullptr;
}

QVector<const PaperStock *> presets()
{
    QVector<const PaperStock *> out;
    out.reserve(6);
    for (const char *id : { "luxury.greige", "saturated.pink", "bubblegum.y2k",
                            "emo.ash", "warm.editorial.cream", "matte.black" })
    {
        const PaperStock *p = findById(QString::fromLatin1(id));
        if (p)
            out.append(p);
    }
    return out;
}

} // namespace paper
} // namespace art
} // namespace paintbrush
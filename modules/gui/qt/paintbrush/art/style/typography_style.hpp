/*****************************************************************************
 * typography_style.hpp : NAI Paintbrush — curated typography roles
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_ART_TYPOGRAPHY_STYLE_HPP
#define PB_ART_TYPOGRAPHY_STYLE_HPP

#include <QString>
#include <QStringList>
#include <QFont>
#include <QColor>
#include <QImage>
#include <QVector>
#include <QVariantMap>

namespace paintbrush {
namespace art {

/* One curated style role (spec §21): not the OS's 400-font dump. Each role
 * lists preferred open fonts first and falls back gracefully to system
 * families, so the result is always legal and never blank. */
struct TypographyStyle
{
    QString id;                 /* "editorial", "condensed.impact", ... */
    QString displayName;
    QStringList families;       /* preference order; first installed wins */
    int weight = QFont::Normal;
    bool italic = false;
    bool uppercase = false;
    float tracking = 0.0f;      /* letter spacing, em fraction (-0.1 .. 0.5) */
    float lineHeight = 1.05f;
    QFont::StyleHint hint = QFont::SansSerif;

    /* Resolve to a concrete QFont at `pixelSize`. Picks the first installed
     * family, else the style hint. */
    QFont font(float pixelSize) const;
    /* The family that actually resolved (for the UI / diagnostics). */
    QString resolvedFamily() const;
    QVariantMap toVariant() const;
};

/* Text treatment request: what a text layer stores (text stays editable until
 * explicit rasterization, spec §57). */
struct TextTreatment
{
    QString text;
    QString styleId = QStringLiteral("editorial");
    float sizePx = 72.0f;
    QColor color{ 0x1F, 0x1B, 0x18 };
    int alignment = 1;          /* 0 left, 1 center, 2 right */
    float trackingOverride = -99.0f; /* -99 = use style */
    QVariantMap toVariant() const;
    static TextTreatment fromVariant(const QVariantMap &m);
};

namespace typography {

/* The seven roles (spec §21). */
QVector<const TypographyStyle *> styles();
const TypographyStyle *findById(const QString &id);

/* Rasterize a treatment into a tight ARGB32 image (straight alpha) at
 * `scale` (1x screen, 2x/4x export). Glyph coverage becomes alpha, so the
 * image can feed the Finish runtime (shadow/outline/glow) and the glitter
 * adhesion mask (spec §57). Multi-line text ('\n') is supported. */
QImage render(const TextTreatment &t, float scale = 1.0f);

} // namespace typography
} // namespace art
} // namespace paintbrush

#endif // PB_ART_TYPOGRAPHY_STYLE_HPP

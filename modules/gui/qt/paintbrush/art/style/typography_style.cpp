/*****************************************************************************
 * typography_style.cpp : NAI Paintbrush — curated typography roles
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "typography_style.hpp"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace paintbrush {
namespace art {

QString TypographyStyle::resolvedFamily() const
{
    const QStringList installed = QFontDatabase::families();
    for (const QString &f : families)
    {
        for (const QString &inst : installed)
            if (inst.compare(f, Qt::CaseInsensitive) == 0)
                return inst;
    }
    return QString();
}

QFont TypographyStyle::font(float pixelSize) const
{
    QFont f;
    const QString fam = resolvedFamily();
    if (!fam.isEmpty())
        f.setFamily(fam);
    else
        f.setStyleHint(hint);
    f.setPixelSize(std::max(1, int(std::lround(pixelSize))));
    f.setWeight(QFont::Weight(weight));
    f.setItalic(italic);
    f.setCapitalization(uppercase ? QFont::AllUppercase : QFont::MixedCase);
    f.setLetterSpacing(QFont::PercentageSpacing, 100.0 + double(tracking) * 100.0);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

QVariantMap TypographyStyle::toVariant() const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("name"), displayName);
    m.insert(QStringLiteral("family"), resolvedFamily());
    m.insert(QStringLiteral("weight"), weight);
    m.insert(QStringLiteral("italic"), italic);
    m.insert(QStringLiteral("uppercase"), uppercase);
    m.insert(QStringLiteral("tracking"), double(tracking));
    return m;
}

QVariantMap TextTreatment::toVariant() const
{
    QVariantMap m;
    m.insert(QStringLiteral("text"), text);
    m.insert(QStringLiteral("style"), styleId);
    m.insert(QStringLiteral("size"), double(sizePx));
    m.insert(QStringLiteral("color"), color.name(QColor::HexArgb));
    m.insert(QStringLiteral("align"), alignment);
    m.insert(QStringLiteral("tracking"), double(trackingOverride));
    return m;
}

TextTreatment TextTreatment::fromVariant(const QVariantMap &m)
{
    TextTreatment t;
    t.text = m.value(QStringLiteral("text")).toString();
    t.styleId = m.value(QStringLiteral("style"), QStringLiteral("editorial")).toString();
    t.sizePx = m.value(QStringLiteral("size"), 72.0).toFloat();
    t.color = QColor(m.value(QStringLiteral("color"), QStringLiteral("#ff1f1b18")).toString());
    t.alignment = m.value(QStringLiteral("align"), 1).toInt();
    t.trackingOverride = m.value(QStringLiteral("tracking"), -99.0).toFloat();
    return t;
}

namespace typography {

namespace {

TypographyStyle make(const char *id, const char *name, const QStringList &fams,
                     int weight, bool italic, bool upper, float tracking,
                     QFont::StyleHint hint, float lineHeight = 1.05f)
{
    TypographyStyle s;
    s.id = QString::fromLatin1(id);
    s.displayName = QString::fromLatin1(name);
    s.families = fams;
    s.weight = weight;
    s.italic = italic;
    s.uppercase = upper;
    s.tracking = tracking;
    s.hint = hint;
    s.lineHeight = lineHeight;
    return s;
}

const QVector<TypographyStyle> &all()
{
    /* Open fonts first (OFL/Apache), then common system fallbacks. Nothing is
     * bundled here, so no redistribution obligation; if none is installed
     * the style hint keeps the text legible. */
    static const QVector<TypographyStyle> styles = {
        make("editorial", "Editorial",
             { "Playfair Display", "Libre Baskerville", "Georgia", "Times New Roman" },
             QFont::DemiBold, false, false, 0.0f, QFont::Serif, 1.02f),
        make("condensed.impact", "Condensed Impact",
             { "Bebas Neue", "Oswald", "Anton", "Impact", "Arial Narrow" },
             QFont::Black, false, true, 0.04f, QFont::SansSerif, 0.95f),
        make("soft.y2k", "Soft Y2K",
             { "Fredoka", "Comfortaa", "Quicksand", "Segoe UI", "Verdana" },
             QFont::Bold, false, false, 0.02f, QFont::SansSerif, 1.08f),
        make("romantic.script", "Romantic Script",
             { "Great Vibes", "Pacifico", "Dancing Script", "Brush Script MT", "Segoe Script" },
             QFont::Normal, false, false, 0.0f, QFont::Cursive, 1.15f),
        make("emo.serif", "Emo Serif",
             { "Cormorant Garamond", "EB Garamond", "Garamond", "Times New Roman" },
             QFont::Medium, true, false, 0.06f, QFont::Serif, 1.0f),
        make("clean.future", "Clean Future",
             { "Space Grotesk", "Montserrat", "Inter", "Segoe UI", "Arial" },
             QFont::Medium, false, true, 0.18f, QFont::SansSerif, 1.1f),
        make("street", "Street",
             { "Anton", "Permanent Marker", "Impact", "Arial Black" },
             QFont::Black, false, true, -0.01f, QFont::SansSerif, 0.92f),
    };
    return styles;
}

} // namespace

QVector<const TypographyStyle *> styles()
{
    QVector<const TypographyStyle *> out;
    for (const TypographyStyle &s : all())
        out.append(&s);
    return out;
}

const TypographyStyle *findById(const QString &id)
{
    for (const TypographyStyle &s : all())
        if (s.id == id)
            return &s;
    return nullptr;
}

QImage render(const TextTreatment &t, float scale)
{
    const TypographyStyle *style = findById(t.styleId);
    if (!style)
        style = findById(QStringLiteral("editorial"));
    if (!style || t.text.trimmed().isEmpty())
        return QImage();

    TypographyStyle s = *style;
    if (t.trackingOverride > -98.0f)
        s.tracking = t.trackingOverride;
    const float px = std::max(4.0f, t.sizePx * std::max(0.05f, scale));
    const QFont font = s.font(px);
    const QFontMetricsF fm(font);

    const QStringList lines = t.text.split(QLatin1Char('\n'));
    qreal maxW = 1.0;
    for (const QString &ln : lines)
        maxW = std::max(maxW, fm.horizontalAdvance(ln));
    const qreal lineH = fm.height() * s.lineHeight;
    const int pad = int(std::ceil(px * 0.12f)) + 2;
    const int w = int(std::ceil(maxW)) + 2 * pad;
    const int h = int(std::ceil(lineH * lines.size())) + 2 * pad;

    QImage img(std::max(1, w), std::max(1, h), QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setFont(font);
    p.setPen(t.color);
    for (int i = 0; i < lines.size(); ++i)
    {
        const qreal lw = fm.horizontalAdvance(lines[i]);
        qreal x = pad;
        if (t.alignment == 1)
            x = pad + (maxW - lw) / 2.0;
        else if (t.alignment == 2)
            x = pad + (maxW - lw);
        const qreal baseline = pad + fm.ascent() + i * lineH;
        p.drawText(QPointF(x, baseline), lines[i]);
    }
    p.end();
    return img;
}

} // namespace typography
} // namespace art
} // namespace paintbrush

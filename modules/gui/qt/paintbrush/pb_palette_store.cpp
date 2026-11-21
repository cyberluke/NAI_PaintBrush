/*****************************************************************************
 * pb_palette_store.cpp : NAI Paintbrush — art-direction palette store
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_palette_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

namespace paintbrush
{

/* The shipped ramps. Kept as data (not code) so a palette can be added,
 * renamed or recoloured without touching logic. Identical to
 * palettes/builtin-palettes.json, which is also the export/import shape. */
static const char *const kBbuiltinsJson = R"JSON({
"palettes": [
 {"id":"editorial-warm","name":"Warm Editorial","description":"Magazine spread warmth: paper, ink, terracotta, one confident accent.","locked":true,"colors":["#F6F1E7","#E8DCC8","#C9B79C","#8C6A4F","#3D2E24","#1C1713","#C25439","#8E3B2A","#D98E5A","#EFC9A4"]},
 {"id":"bubblegum-y2k","name":"Bubblegum Y2K","description":"Soft pink, pearlescent lilac, candy chrome - the poster wall look.","locked":true,"colors":["#FFE3F1","#FFB7D5","#FF8FC0","#F45FA2","#C43C82","#7B2A5E","#E7D4FF","#B79CFF","#8AE1F0","#FFF6B0"]},
 {"id":"midnight-chrome","name":"Midnight Chrome","description":"Matte black with cold chrome and ice highlights.","locked":true,"colors":["#05070A","#0E1319","#1A222C","#2C3846","#4A5A6E","#7E90A6","#B9C6D4","#E6EDF5","#7FD3E8","#F2F7FF"]},
 {"id":"pastel-studio","name":"Pastel Studio","description":"Low-contrast pastels for gentle layouts and sticker packs.","locked":true,"colors":["#FFF7F0","#FDE2D0","#F8C8B6","#E9A9A0","#C98B96","#9A6E86","#B7D9C9","#8FBFB2","#A9C7E8","#7E9EC4"]},
 {"id":"neon-stage","name":"Neon Stage","description":"Stage-light neon on deep violet - high energy, high contrast.","locked":true,"colors":["#0B0614","#1A0B2E","#2E1149","#4B1A74","#FF2E9A","#FF6BD6","#38F2C8","#00C2FF","#C6FF3D","#FFF04D"]},
 {"id":"holo-foil","name":"Holo Foil","description":"Iridescent foil ramp for glitter and holographic mirror finishes.","locked":true,"colors":["#F7F9FF","#CFE4FF","#9BC8FF","#7AA8FF","#A78BFA","#E48BF0","#FF9AD5","#FFC2A8","#FFE9A8","#B6FFE3"]},
 {"id":"risograph","name":"Risograph","description":"Two-ink print feel: fluorescent pink, teal, and paper stock.","locked":true,"colors":["#F4F0E4","#DCD5C3","#B7AE99","#2F4B4A","#1B2E2D","#FF4F8B","#D93A72","#00A6A6","#00787A","#FFD166"]}
]}
)JSON";

static const QRegularExpression &hexRe()
{
    static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    return re;
}

/* ---------------------------------------------------------------- Palette */

bool Palette::isValid() const
{
    return PaletteStore::validate(*this, nullptr);
}

QVariantMap Palette::toVariantMap() const
{
    QVariantMap map;
    map.insert(QStringLiteral("id"), id);
    map.insert(QStringLiteral("name"), name);
    map.insert(QStringLiteral("description"), description);
    map.insert(QStringLiteral("author"), author);
    map.insert(QStringLiteral("locked"), locked);
    map.insert(QStringLiteral("colors"), colors);
    return map;
}

Palette Palette::fromVariantMap(const QVariantMap &map)
{
    Palette p;
    p.id = map.value(QStringLiteral("id")).toString().trimmed();
    p.name = map.value(QStringLiteral("name")).toString().trimmed();
    p.description = map.value(QStringLiteral("description")).toString();
    p.author = map.value(QStringLiteral("author"),
                         QStringLiteral("NAI")).toString();
    p.locked = map.value(QStringLiteral("locked"), false).toBool();
    for (const QString &c : map.value(QStringLiteral("colors")).toStringList())
        p.colors.append(c.trimmed());
    return p;
}

/* ----------------------------------------------------------------- store */

PaletteStore::PaletteStore()
    : m_builtins(builtins())
{
    reloadUser();
}

QVector<Palette> PaletteStore::builtins()
{
    QVector<Palette> out;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(kBbuiltinsJson));
    for (const QJsonValue &v : doc.object().value(QStringLiteral("palettes")).toArray())
    {
        const Palette p = Palette::fromVariantMap(v.toObject().toVariantMap());
        if (p.isValid())
            out.append(p);
    }
    return out;
}

QVector<Palette> PaletteStore::all() const
{
    QVector<Palette> out = m_builtins;
    out += m_user;
    return out;
}

const Palette *PaletteStore::find(const QString &id) const
{
    for (const Palette &p : m_builtins)
        if (p.id == id)
            return &p;
    for (const Palette &p : m_user)
        if (p.id == id)
            return &p;
    return nullptr;
}

QString PaletteStore::userDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/paintbrush/palettes");
}

bool PaletteStore::validate(const Palette &palette, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };

    if (palette.id.isEmpty())
        return fail(QStringLiteral("Palette id is required."));
    if (palette.name.isEmpty())
        return fail(QStringLiteral("Palette name is required."));
    if (palette.colors.size() < 4)
        return fail(QStringLiteral("A palette needs at least 4 colors."));
    for (const QString &c : palette.colors)
        if (!hexRe().match(c).hasMatch())
            return fail(QStringLiteral("Invalid color '%1' (expected #RRGGBB).").arg(c));
    if (error)
        error->clear();
    return true;
}

bool PaletteStore::save(const Palette &palette, QString *error)
{
    if (find(palette.id) && find(palette.id)->locked)
    {
        if (error)
            *error = QStringLiteral("'%1' is a built-in palette and cannot be edited.").arg(palette.name);
        return false;
    }
    if (!validate(palette, error))
        return false;

    Palette stored = palette;
    stored.locked = false;           /* user palettes are always editable  */
    if (!writeUserFile(stored, error))
        return false;
    reloadUser();
    return true;
}

bool PaletteStore::remove(const QString &id, QString *error)
{
    const Palette *p = find(id);
    if (!p)
    {
        if (error)
            *error = QStringLiteral("Palette not found.");
        return false;
    }
    if (p->locked)
    {
        if (error)
            *error = QStringLiteral("Built-in palettes cannot be deleted.");
        return false;
    }
    QFile f(userDir() + QLatin1Char('/') + id + QStringLiteral(".json"));
    if (!f.remove())
    {
        if (error)
            *error = QStringLiteral("Could not delete the palette file.");
        return false;
    }
    reloadUser();
    return true;
}

bool PaletteStore::exportTo(const QString &id, const QString &filePath,
                            QString *error) const
{
    const Palette *p = find(id);
    if (!p)
    {
        if (error)
            *error = QStringLiteral("Palette not found.");
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("$schema"), QStringLiteral("nai-paintbrush/palette/v1"));
    root.insert(QStringLiteral("version"), 1);
    QJsonArray arr;
    arr.append(QJsonObject::fromVariantMap(p->toVariantMap()));
    root.insert(QStringLiteral("palettes"), arr);

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (error)
            *error = QStringLiteral("Could not write '%1'.").arg(filePath);
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool PaletteStore::importFrom(const QString &filePath, QString *error)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
    {
        if (error)
            *error = QStringLiteral("Could not read '%1'.").arg(filePath);
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
    {
        if (error)
            *error = QStringLiteral("Not a palette file.");
        return false;
    }

    /* Accept both the single-palette export shape and the multi-palette
     * collection shape (so a user can import a whole pack). */
    QJsonArray arr = doc.object().value(QStringLiteral("palettes")).toArray();
    if (arr.isEmpty())
    {
        const Palette single = Palette::fromVariantMap(doc.object().toVariantMap());
        if (single.isValid())
            arr.append(QJsonObject::fromVariantMap(single.toVariantMap()));
    }
    if (arr.isEmpty())
    {
        if (error)
            *error = QStringLiteral("The file contains no palette.");
        return false;
    }

    QString lastError;
    int imported = 0;
    for (const QJsonValue &v : arr)
    {
        Palette p = Palette::fromVariantMap(v.toObject().toVariantMap());
        if (!p.isValid())
        {
            lastError = QStringLiteral("Skipped an invalid palette.");
            continue;
        }
        /* Imported presets become USER palettes; ids must not collide with
         * shipped presets (a pack may legitimately contain 'neon-stage'). */
        if (find(p.id) && find(p.id)->locked)
            p.id = p.id + QStringLiteral("-custom");
        p.locked = false;
        if (!writeUserFile(p, &lastError))
            continue;
        ++imported;
    }

    reloadUser();
    if (imported == 0)
    {
        if (error)
            *error = lastError.isEmpty() ? QStringLiteral("Nothing imported.") : lastError;
        return false;
    }
    return true;
}

void PaletteStore::reloadUser()
{
    m_user.clear();

    QDir dir(userDir());
    if (!dir.exists())
        return;

    QSet<QString> seen;
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.json")},
                                                  QDir::Files, QDir::Name);
    for (const QFileInfo &fi : files)
    {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject())
            continue;

        const Palette p = Palette::fromVariantMap(doc.object().toVariantMap());
        if (!p.isValid() || seen.contains(p.id))
            continue;
        if (find(p.id) && find(p.id)->locked)
            continue;               /* never shadow a shipped preset */
        seen.insert(p.id);
        m_user.append(p);
    }
}

bool PaletteStore::writeUserFile(const Palette &palette, QString *error) const
{
    QDir dir(userDir());
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
    {
        if (error)
            *error = QStringLiteral("Could not create the palette folder.");
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("$schema"), QStringLiteral("nai-paintbrush/palette/v1"));
    root.insert(QStringLiteral("version"), 1);
    const QVariantMap map = palette.toVariantMap();
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        root.insert(it.key(), QJsonValue::fromVariant(it.value()));

    QFile f(userDir() + QLatin1Char('/') + palette.id + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (error)
            *error = QStringLiteral("Could not write the palette file.");
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace paintbrush

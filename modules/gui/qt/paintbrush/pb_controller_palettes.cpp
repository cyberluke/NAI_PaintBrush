/*****************************************************************************
 * pb_controller_palettes.cpp : NAI Paintbrush — palette QML surface
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_controller.hpp"
#include "pb_palette_store.hpp"

namespace paintbrush
{

/* The palette store is process-wide and loads lazily on first use, so the
 * controller does not need a new member and the header stays free of the
 * store's implementation. */
static PaletteStore &paletteStore()
{
    static PaletteStore store;
    return store;
}

QVariantList PaintbrushController::paletteList() const
{
    QVariantList out;
    const QVector<Palette> all = paletteStore().all();
    out.reserve(all.size());
    for (const Palette &p : all)
        out.append(p.toVariantMap());
    return out;
}

QString PaintbrushController::saveCustomPalette(const QVariantMap &palette)
{
    QString error;
    if (!paletteStore().save(Palette::fromVariantMap(palette), &error))
        return error;
    return QString();               /* empty = applied cleanly */
}

QString PaintbrushController::deleteCustomPalette(const QString &id)
{
    QString error;
    if (!paletteStore().remove(id, &error))
        return error;
    return QString();
}

QString PaintbrushController::exportPalette(const QString &id,
                                            const QString &filePath)
{
    QString error;
    if (!paletteStore().exportTo(id, filePath, &error))
        return error;
    return QString();
}

QString PaintbrushController::importPalette(const QString &filePath)
{
    QString error;
    if (!paletteStore().importFrom(filePath, &error))
        return error;
    return QString();
}

} // namespace paintbrush

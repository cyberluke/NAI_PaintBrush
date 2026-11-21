/*****************************************************************************
 * pb_controller_palettes.cpp : NAI Paintbrush — palette QML surface
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_controller.hpp"
#include "pb_palette_store.hpp"
#include "pb_subject_store.hpp"

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

static SubjectStore &subjectStore()
{
    return sharedSubjectStore();
}

QVariantList PaintbrushController::subjectList() const
{
    QVariantList out;
    const QVector<SubjectAsset> all = subjectStore().all();
    for (const SubjectAsset &a : all)
        out.append(a.toVariantMap());
    return out;
}

QString PaintbrushController::saveSubject(const QVariantMap &subject)
{
    QString error;
    if (!subjectStore().save(SubjectAsset::fromVariantMap(subject), &error))
        return error;
    return QString();
}

QString PaintbrushController::deleteSubject(const QString &id)
{
    QString error;
    if (!subjectStore().remove(id, &error))
        return error;
    return QString();
}

QVariantList PaintbrushController::instanceList(const QString &compositionId) const
{
    QVariantList out;
    const QVector<SubjectInstance> all = subjectStore().instances(compositionId);
    for (const SubjectInstance &i : all)
        out.append(i.toVariantMap());
    return out;
}

QString PaintbrushController::saveInstance(const QVariantMap &instance)
{
    QString error;
    if (!subjectStore().saveInstance(SubjectInstance::fromVariantMap(instance), &error))
        return error;
    return QString();
}

QString PaintbrushController::deleteInstance(const QString &id)
{
    QString error;
    if (!subjectStore().removeInstance(id, &error))
        return error;
    return QString();
}

QVariantList PaintbrushController::artworkList(const QString &sourceSubjectId) const
{
    QVariantList out;
    const QVector<DerivedArtwork> all = subjectStore().artworks(sourceSubjectId);
    for (const DerivedArtwork &a : all)
        out.append(a.toVariantMap());
    return out;
}

QString PaintbrushController::saveArtwork(const QVariantMap &artwork)
{
    QString error;
    if (!subjectStore().saveArtwork(DerivedArtwork::fromVariantMap(artwork), &error))
        return error;
    return QString();
}

QString PaintbrushController::deleteArtwork(const QString &id)
{
    QString error;
    if (!subjectStore().removeArtwork(id, &error))
        return error;
    return QString();
}

QString PaintbrushController::placeSubject(const QString &subjectId,
                                           const QString &compositionId)
{
    SubjectStore &store = subjectStore();
    const SubjectAsset *asset = store.find(subjectId);
    if (!asset)
        return QStringLiteral("Subject not found.");

    const QString comp = compositionId.isEmpty()
                             ? QStringLiteral("main") : compositionId;

    QImage cutout(store.assetImagePath(subjectId));
    if (cutout.isNull())
        return QStringLiteral("This subject has no stored cut-out yet.");

    const QPointF pos((m_document.canvasWidth() - cutout.width()) / 2.0,
                      (m_document.canvasHeight() - cutout.height()) / 2.0);
    const int zBefore = m_document.layerCount();

    insertStickerLayer(cutout, pos, asset->name, asset->sourceMediaId,
                       asset->name, asset->sourcePtsMs, QImage());

    SubjectInstance inst;
    inst.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    inst.subjectAssetId = subjectId;
    inst.compositionId = comp;
    inst.sourceStartMs = asset->sourcePtsMs;
    inst.x = pos.x();
    inst.y = pos.y();
    inst.zIndex = zBefore;

    QString error;
    if (!store.saveInstance(inst, &error))
    {
        /* Roll back the visual insertion: never leave a stray layer behind a
         * failed instance save. */
        undo();
        return error;
    }

    emit objectSelectionChanged();
    setStatus(QStringLiteral("Placed subject \"%1\"").arg(asset->name));
    return QString();
}

} // namespace paintbrush
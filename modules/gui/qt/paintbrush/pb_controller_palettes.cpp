/*****************************************************************************
 * pb_controller_palettes.cpp : NAI Paintbrush — palette QML surface
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_controller.hpp"
#include "pb_palette_store.hpp"
#include "pb_subject_store.hpp"
#include "pb_canvas_item.hpp"
#include "qt.hpp"

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

    /* Cascade repeated placements so every instance is visibly independent —
     * placing twice at the same centre would look like a single subject. */
    const int placed = int(store.instances(comp).size());
    const double cascade = 36.0 * double(placed % 6);
    const QPointF pos((m_document.canvasWidth() - cutout.width()) / 2.0 + cascade,
                      (m_document.canvasHeight() - cutout.height()) / 2.0 + cascade);
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
    if (m_document.layerCount() > zBefore)
        m_document.layerAt(zBefore)->subjectInstanceId = inst.id;

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

QString PaintbrushController::updateInstanceTransform(const QString &instanceId,
                                                      double x, double y,
                                                      double scale,
                                                      double rotation)
{
    SubjectStore &store = subjectStore();
    const SubjectInstance *found = store.findInstance(instanceId);
    if (!found)
        return QStringLiteral("Instance not found.");

    SubjectInstance inst = *found;
    inst.x = x;
    inst.y = y;
    inst.scale = scale;
    inst.rotation = rotation;

    QString error;
    if (!store.saveInstance(inst, &error))
        return error;
    emit objectSelectionChanged();
    return QString();
}

int PaintbrushController::restoreCompositionInstances(const QString &compositionId)
{
    SubjectStore &store = subjectStore();
    const QVector<SubjectInstance> list = store.instances(compositionId);
    int restored = 0;

    for (const SubjectInstance &inst : list)
    {
        const SubjectAsset *asset = store.find(inst.subjectAssetId);
        if (!asset)
        {
            pbLogWarn(VLC_OBJECT(m_intf), "subject",
                      "restore: instance %s skipped (asset %s missing)",
                      inst.id.toUtf8().constData(),
                      inst.subjectAssetId.toUtf8().constData());
            continue;
        }
        QImage cutout(store.assetImagePath(asset->id));
        if (cutout.isNull())
        {
            pbLogWarn(VLC_OBJECT(m_intf), "subject",
                      "restore: instance %s skipped (cut-out missing)",
                      inst.id.toUtf8().constData());
            continue;
        }
        insertStickerLayer(cutout, QPointF(inst.x, inst.y), asset->name,
                           asset->sourceMediaId, asset->name,
                           inst.sourceStartMs, QImage());
        if (m_document.layerCount() > 0)
            m_document.layerAt(m_document.layerCount() - 1)->subjectInstanceId = inst.id;
        ++restored;
    }

    /* Opening a project must not create undo history. */
    m_undoStack.clear();
    if (m_canvas)
        m_canvas->markCanvasDirty();
    pbLogInfo(VLC_OBJECT(m_intf), "subject",
              "restore: %d/%d instances materialized for '%s'",
              restored, int(list.size()), compositionId.toUtf8().constData());
    return restored;
}

void PaintbrushController::syncInstanceTransforms()
{
    SubjectStore &store = subjectStore();
    for (int i = 0; i < m_document.layerCount(); ++i)
    {
        const CanvasLayer *layer = m_document.layerAt(i);
        if (!layer || layer->subjectInstanceId.isEmpty())
            continue;
        const SubjectInstance *found = store.findInstance(layer->subjectInstanceId);
        if (!found)
            continue;
        SubjectInstance inst = *found;
        if (qAbs(inst.x - layer->position.x()) < 0.01
            && qAbs(inst.y - layer->position.y()) < 0.01
            && qAbs(inst.scale - double(layer->scale)) < 0.001)
            continue;
        inst.x = layer->position.x();
        inst.y = layer->position.y();
        inst.scale = double(layer->scale);
        inst.rotation = double(layer->rotation);
        QString error;
        store.saveInstance(inst, &error);
    }
}

void PaintbrushController::beginTransaction(const QString &description)
{
    m_undoStack.beginComposite(description);
}

void PaintbrushController::endTransaction()
{
    m_undoStack.endComposite();
}

} // namespace paintbrush
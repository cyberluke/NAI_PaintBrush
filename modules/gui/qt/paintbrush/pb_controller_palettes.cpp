/*****************************************************************************
 * pb_controller_palettes.cpp : NAI Paintbrush — palette QML surface
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_controller.hpp"
#include "pb_palette_store.hpp"
#include "pb_subject_store.hpp"
#include "pb_layer.hpp"
#include "pb_history.hpp"
#include "art/finish/effects.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainterPath>
#include <QDateTime>
#include <QUuid>
#include <QPainterPath>
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
    SubjectStore &store = subjectStore();
    if (!store.findInstance(id))
        return QStringLiteral("Instance not found.");

    /* Find the layer this instance materializes BEFORE removing the record, so
     * a failed removal can never leave an orphaned layer behind. */
    int layerIndex = -1;
    for (int i = 0; i < m_document.layerCount(); ++i)
    {
        const CanvasLayer *layer = m_document.layerAt(i);
        if (layer && layer->subjectInstanceId == id)
        {
            layerIndex = i;
            break;
        }
    }

    QString error;
    if (!store.removeInstance(id, &error))
        return error;

    if (layerIndex >= 0)
    {
        /* Layer removal is itself an undoable command; the transaction keeps
         * the pair (layer + instance record) as one coherent step. */
        beginTransaction(QStringLiteral("Delete subject instance"));
        deleteLayer(layerIndex);
        endTransaction();
    }
    emit objectSelectionChanged();
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

QString PaintbrushController::applySubjectStyle(const QString &instanceId,
                                                const QString &recipeId)
{
    SubjectStore &store = subjectStore();
    if (!store.findInstance(instanceId))
        return QStringLiteral("Instance not found.");

    CanvasLayer *layer = nullptr;
    for (int i = 0; i < m_document.layerCount(); ++i)
    {
        CanvasLayer *candidate = m_document.layerAt(i);
        if (candidate && candidate->subjectInstanceId == instanceId)
        {
            layer = candidate;
            break;
        }
    }
    if (!layer)
        return QStringLiteral("This instance has no layer on the canvas.");

    art::EffectNode node;
    if (recipeId == QStringLiteral("ink"))
    {
        node.type = art::EffectNode::Outline;
        node.color = QColor(0x1B, 0x2A, 0x33);
        node.width = 3.5f;
        node.softness = 0.15f;
    }
    else if (recipeId == QStringLiteral("glow"))
    {
        node.type = art::EffectNode::Glow;
        node.color = QColor(0xFF, 0x6B, 0xD6);
        node.radius = 24.0f;
        node.intensity = 1.2f;
        node.threshold = 0.2f;
    }
    else if (recipeId == QStringLiteral("ghost"))
    {
        node.type = art::EffectNode::Ghost;
        node.desaturate = 0.6f;
        node.opacity = 0.35f;
        node.tint = QColor(0xFF, 0xFF, 0xFF, 90);
    }
    else
    {
        return QStringLiteral("Unknown style recipe.");
    }

    QVector<art::EffectNode> after = layer->effects;
    bool replaced = false;
    for (art::EffectNode &existing : after)
    {
        if (existing.type == node.type)
        {
            existing = node;        /* same recipe twice = idempotent */
            replaced = true;
            break;
        }
    }
    if (!replaced)
        after.append(node);

    SetEffectsCommand cmd;
    cmd.layerId = layer->id;
    cmd.before = layer->effects;
    cmd.after = after;
    cmd.desc = QStringLiteral("Apply %1 style").arg(recipeId);

    layer->effects = after;
    m_undoStack.pushCommand(std::make_unique<SetEffectsCommand>(cmd));
    if (m_canvas)
        m_canvas->markCanvasDirty();
    emit documentChanged();
    setStatus(QStringLiteral("Applied style: %1").arg(recipeId));
    return QString();
}

QString PaintbrushController::deriveArtwork(const QString &subjectId,
                                            const QString &recipeId)
{
    SubjectStore &store = subjectStore();
    const SubjectAsset *asset = store.find(subjectId);
    if (!asset)
        return QStringLiteral("Subject not found.");

    QImage src(store.assetImagePath(subjectId));
    if (src.isNull())
        return QStringLiteral("This subject has no stored cut-out yet.");
    src = src.convertToFormat(QImage::Format_ARGB32);

    /* Honest RASTER derivation: tone quantisation over the cut-out, recorded
     * with mode="raster". This is deliberately not presented as vector. */
    const int levels = (recipeId == QStringLiteral("ink")) ? 4 : 6;
    const int step = 255 / levels;
    const bool gloss = (recipeId == QStringLiteral("gloss"));
    for (int y = 0; y < src.height(); ++y)
    {
        for (int x = 0; x < src.width(); ++x)
        {
            QColor c = src.pixelColor(x, y);
            const int a = c.alpha();
            if (a == 0)
                continue;
            int l = qGray(c.rgb());
            l = (l / step) * step;
            c.setRgb(l, l, l);
            if (gloss && l > 190)
                c = c.lighter(125);
            c.setAlpha(a);
            src.setPixelColor(x, y, c);
        }
    }

    DerivedArtwork art;
    art.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    art.sourceSubjectId = subjectId;
    art.sourceRevisionId = asset->createdUtc;
    art.mode = QStringLiteral("raster");
    art.styleRecipeId = recipeId;
    art.fillLayerIds.append(art.id);
    art.provenanceId = QStringLiteral("derive:posterize-v1");
    art.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    if (!src.save(store.artworkImagePath(art.id), "PNG"))
        return QStringLiteral("Could not write the derived artwork image.");

    QString error;
    if (!store.saveArtwork(art, &error))
        return error;

    SubjectAsset updated = *asset;
    if (!updated.derivedArtworkIds.contains(art.id))
        updated.derivedArtworkIds.append(art.id);
    store.save(updated, &error);

    beginTransaction(QStringLiteral("Add derived artwork"));
    insertStickerLayer(src, QPointF(0, 0),
                       QStringLiteral("%1 (%2)").arg(asset->name, recipeId),
                       asset->sourceMediaId, asset->name,
                       asset->sourcePtsMs, QImage());
    endTransaction();

    emit objectSelectionChanged();
    setStatus(QStringLiteral("Derived %1 artwork").arg(recipeId));
    return QString();
}

QString PaintbrushController::deriveContour(const QString &subjectId)
{
    SubjectStore &store = subjectStore();
    const SubjectAsset *asset = store.find(subjectId);
    if (!asset)
        return QStringLiteral("Subject not found.");

    QImage cutout(store.assetImagePath(subjectId));
    if (cutout.isNull())
        return QStringLiteral("This subject has no stored cut-out yet.");
    if (!m_canvas)
        return QStringLiteral("Canvas is not ready.");

    /* Real VECTOR derivation: the cut-out alpha becomes the document selection,
     * which is a QPainterPath — editable geometry the existing selection tools
     * (grow/shrink/feather/invert) and Fill/Stroke operate on. The path is
     * persisted alongside the DerivedArtwork so it survives reopen. */
    const QImage alpha = cutout.convertToFormat(QImage::Format_Alpha8);
    m_canvas->setSelectionFromMask(alpha);

    const QPainterPath path = m_document.selectionPath();
    if (path.isEmpty())
        return QStringLiteral("The subject produced an empty contour.");

    DerivedArtwork art;
    art.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    art.sourceSubjectId = subjectId;
    art.sourceRevisionId = asset->createdUtc;
    art.mode = QStringLiteral("vector");
    art.styleRecipeId = QStringLiteral("contour");
    art.editableStrokeSetId = art.id;
    art.provenanceId = QStringLiteral("derive:contour-v1");
    art.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonArray elements;
    for (int i = 0; i < path.elementCount(); ++i)
    {
        const QPainterPath::Element e = path.elementAt(i);
        QJsonObject o;
        o.insert(QStringLiteral("t"), int(e.type));
        o.insert(QStringLiteral("x"), e.x);
        o.insert(QStringLiteral("y"), e.y);
        elements.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("$schema"),
                QStringLiteral("nai-paintbrush/contour/v1"));
    root.insert(QStringLiteral("sourceSubjectId"), subjectId);
    root.insert(QStringLiteral("elements"), elements);

    QFile f(store.artworkImagePath(art.id) + QStringLiteral(".path.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

    QString error;
    if (!store.saveArtwork(art, &error))
        return error;

    SubjectAsset updated = *asset;
    if (!updated.derivedArtworkIds.contains(art.id))
        updated.derivedArtworkIds.append(art.id);
    store.save(updated, &error);

    emit documentChanged();
    emit objectSelectionChanged();
    setStatus(QStringLiteral("Derived vector contour (%1 points)").arg(path.elementCount()));
    return QString();
}

} // namespace paintbrush
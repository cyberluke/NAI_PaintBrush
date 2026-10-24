/*****************************************************************************
 * pb_project_store.cpp : NAI Paintbrush — project persistence
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_project_store.hpp"
#include "pb_document.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDateTime>
#include <QDir>
#include <QMetaType>

namespace paintbrush {

ProjectStore::ProjectStore(QObject *parent)
    : QObject(parent)
{
}

static QString layerToJson(const CanvasLayer &layer)
{
    QJsonObject obj;
    obj[QStringLiteral("name")] = layer.name;
    obj[QStringLiteral("id")] = layer.id;  /* stable across reopen (provenance) */
    obj[QStringLiteral("type")] = layer.type;
    obj[QStringLiteral("visible")] = layer.visible;
    obj[QStringLiteral("locked")] = layer.locked;
    obj[QStringLiteral("alphaLocked")] = layer.alphaLocked;
    obj[QStringLiteral("opacity")] = layer.opacity;
    obj[QStringLiteral("blend")] = layer.blendMode;
    obj[QStringLiteral("text")] = layer.text;
    obj[QStringLiteral("posX")] = layer.position.x();
    obj[QStringLiteral("posY")] = layer.position.y();
    obj[QStringLiteral("scale")] = layer.scale;
    obj[QStringLiteral("rotation")] = layer.rotation;

    /* sticker provenance (FrameRef + seed mask). Only JSON-native values
     * are written; anything exotic is skipped. */
    if (!layer.metadata.isEmpty())
    {
        QJsonObject meta;
        for (auto it = layer.metadata.constBegin();
             it != layer.metadata.constEnd(); ++it)
        {
            const QVariant &v = it.value();
            switch (v.typeId())
            {
            case QMetaType::QString:
                meta[it.key()] = v.toString();
                break;
            case QMetaType::Bool:
                meta[it.key()] = v.toBool();
                break;
            case QMetaType::Int:
                meta[it.key()] = v.toInt();
                break;
            case QMetaType::Double:
                meta[it.key()] = v.toDouble();
                break;
            case QMetaType::LongLong:
                meta[it.key()] = v.toLongLong();
                break;
            default:
                break;
            }
        }
        if (!meta.isEmpty())
            obj[QStringLiteral("metadata")] = meta;
    }

    /* Paintbrush 2.0: non-destructive finish chain, composition role, material
     * identity/state and harvest provenance (spec §39, §20, §52, §54). */
    if (!layer.effects.isEmpty())
    {
        QJsonArray effects;
        for (const art::EffectNode &e : layer.effects)
            effects.append(QJsonObject::fromVariantMap(e.toVariant()));
        obj[QStringLiteral("effects")] = effects;
    }
    if (layer.role != 0)
        obj[QStringLiteral("role")] = art::layerRoleName(art::LayerRole(layer.role));
    if (!layer.materialPreset.isEmpty())
        obj[QStringLiteral("materialPreset")] = layer.materialPreset;
    if (!layer.materialState.isEmpty())
        obj[QStringLiteral("materialState")] =
            QString::fromLatin1(layer.materialState.toBase64());
    if (layer.harvestId > 0)
        obj[QStringLiteral("harvestId")] = layer.harvestId;

    if (!layer.raster.isNull())
    {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        layer.raster.save(&buffer, "PNG");
        obj[QStringLiteral("image")] = QString::fromLatin1(png.toBase64());
    }
    if (!layer.mask.isNull())
    {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        layer.mask.save(&buffer, "PNG");
        obj[QStringLiteral("mask")] = QString::fromLatin1(png.toBase64());
    }
    return QString::fromLatin1(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

static QJsonArray layersToJson(const QList<CanvasLayer> &layers)
{
    QJsonArray array;
    for (const CanvasLayer &layer : layers)
        array.append(QJsonDocument::fromJson(layerToJson(layer).toUtf8()).object());
    return array;
}

static QList<CanvasLayer> layersFromJson(const QJsonArray &array)
{
    QList<CanvasLayer> layers;
    for (const QJsonValue &value : array)
    {
        const QJsonObject obj = value.toObject();
        CanvasLayer layer;
        layer.name = obj[QStringLiteral("name")].toString();
        layer.id = qMax(0, obj[QStringLiteral("id")].toInt(0));
        layer.type = obj[QStringLiteral("type")].toInt(LAYER_RASTER);
        layer.visible = obj[QStringLiteral("visible")].toBool(true);
        layer.locked = obj[QStringLiteral("locked")].toBool(false);
        layer.alphaLocked = obj[QStringLiteral("alphaLocked")].toBool(false);
        layer.opacity = obj[QStringLiteral("opacity")].toDouble(1.0);
        layer.blendMode = obj[QStringLiteral("blend")].toInt(BLEND_NORMAL);
        layer.text = obj[QStringLiteral("text")].toString();
        layer.position = QPointF(obj[QStringLiteral("posX")].toDouble(0),
                                 obj[QStringLiteral("posY")].toDouble(0));
        layer.scale = float(obj[QStringLiteral("scale")].toDouble(1.0));
        layer.rotation = float(obj[QStringLiteral("rotation")].toDouble(0.0));

        const QByteArray png = QByteArray::fromBase64(
            obj[QStringLiteral("image")].toString().toLatin1());
        if (!png.isEmpty())
            layer.raster.loadFromData(png, "PNG");

        const QByteArray maskPng = QByteArray::fromBase64(
            obj[QStringLiteral("mask")].toString().toLatin1());
        if (!maskPng.isEmpty())
            layer.mask.loadFromData(maskPng, "PNG");

        const QJsonObject meta = obj[QStringLiteral("metadata")].toObject();
        for (auto it = meta.constBegin(); it != meta.constEnd(); ++it)
            layer.metadata[it.key()] = it.value().toVariant();

        for (const QJsonValue &ev : obj[QStringLiteral("effects")].toArray())
            layer.effects.append(art::EffectNode::fromVariant(ev.toObject().toVariantMap()));
        layer.role = int(art::layerRoleFromName(obj[QStringLiteral("role")].toString()));
        layer.materialPreset = obj[QStringLiteral("materialPreset")].toString();
        layer.materialState = QByteArray::fromBase64(
            obj[QStringLiteral("materialState")].toString().toLatin1());
        layer.harvestId = obj[QStringLiteral("harvestId")].toInt(0);

        layers.append(std::move(layer));
    }
    return layers;
}

static QString imageToBase64Png(const QImage &img)
{
    if (img.isNull())
        return QString();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return QString::fromLatin1(png.toBase64());
}

static QImage imageFromBase64Png(const QString &b64)
{
    const QByteArray png = QByteArray::fromBase64(b64.toLatin1());
    QImage img;
    if (!png.isEmpty())
        img.loadFromData(png, "PNG");
    return img;
}

/* Document art state: paper, mood, seed, light rig (spec §52). */
static QJsonObject artToJson(const PaintbrushDocument *doc)
{
    QJsonObject art;
    art[QStringLiteral("paper")] = doc->paperStockId();
    art[QStringLiteral("mood")] = doc->moodId();
    art[QStringLiteral("seed")] = double(doc->artSeed());
    const art::ArtLightRig &rig = doc->lightRig();
    QJsonObject light;
    light[QStringLiteral("dirX")] = double(rig.direction.x());
    light[QStringLiteral("dirY")] = double(rig.direction.y());
    light[QStringLiteral("dirZ")] = double(rig.direction.z());
    light[QStringLiteral("elevation")] = double(rig.elevationDeg);
    light[QStringLiteral("softness")] = double(rig.softness);
    light[QStringLiteral("ambient")] = double(rig.ambient);
    light[QStringLiteral("intensity")] = double(rig.intensity);
    light[QStringLiteral("temperature")] = double(rig.temperature);
    art[QStringLiteral("light")] = light;
    return art;
}

static void artFromJson(PaintbrushDocument *doc, const QJsonObject &art)
{
    if (art.isEmpty())
        return;
    if (art.contains(QStringLiteral("paper")))
        doc->setPaperStockId(art[QStringLiteral("paper")].toString());
    if (art.contains(QStringLiteral("mood")))
        doc->setMoodId(art[QStringLiteral("mood")].toString());
    if (art.contains(QStringLiteral("seed")))
        doc->setArtSeed(quint32(art[QStringLiteral("seed")].toDouble(1)));
    const QJsonObject light = art[QStringLiteral("light")].toObject();
    if (!light.isEmpty())
    {
        art::ArtLightRig rig = doc->lightRig();
        rig.direction = QVector3D(float(light[QStringLiteral("dirX")].toDouble(0.5)),
                                  float(light[QStringLiteral("dirY")].toDouble(-0.35)),
                                  float(light[QStringLiteral("dirZ")].toDouble(0.6)));
        rig.elevationDeg = float(light[QStringLiteral("elevation")].toDouble(-1.0));
        rig.softness = float(light[QStringLiteral("softness")].toDouble(0.35));
        rig.ambient = float(light[QStringLiteral("ambient")].toDouble(0.3));
        rig.intensity = float(light[QStringLiteral("intensity")].toDouble(1.0));
        rig.temperature = float(light[QStringLiteral("temperature")].toDouble(0.55));
        doc->setLightRig(rig);
    }
}

/* Harvest tray: manifest + cutout/mask assets per item (spec §52, §53). */
static QJsonArray harvestToJson(const HarvestTrayModel *tray)
{
    QJsonArray arr;
    for (const HarvestItem &h : tray->items())
    {
        QJsonObject o = QJsonObject::fromVariantMap(h.toManifest());
        o[QStringLiteral("cutout")] = imageToBase64Png(h.cutout);
        o[QStringLiteral("mask")] = imageToBase64Png(h.mask);
        arr.append(o);
    }
    return arr;
}

static QVector<HarvestItem> harvestFromJson(const QJsonArray &arr)
{
    QVector<HarvestItem> items;
    for (const QJsonValue &v : arr)
    {
        const QJsonObject o = v.toObject();
        HarvestItem h = HarvestItem::fromManifest(o.toVariantMap());
        h.cutout = imageFromBase64Png(o[QStringLiteral("cutout")].toString())
                       .convertToFormat(QImage::Format_ARGB32);
        h.mask = imageFromBase64Png(o[QStringLiteral("mask")].toString());
        if (!h.mask.isNull() && h.mask.format() != QImage::Format_Grayscale8)
            h.mask = h.mask.convertToFormat(QImage::Format_Grayscale8);
        if (h.id > 0 && !h.cutout.isNull())
            items.append(h);
    }
    return items;
}

QString ProjectStore::saveToBytes(PaintbrushDocument *doc, QByteArray *out) const
{
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("nAI-paintbrush-project");
    root[QStringLiteral("version")] = FORMAT_VERSION;
    root[QStringLiteral("savedAt")] =
        QDateTime::currentDateTime().toString(Qt::ISODate);

    QJsonObject canvas;
    canvas[QStringLiteral("width")] = doc->canvasSize().width();
    canvas[QStringLiteral("height")] = doc->canvasSize().height();
    root[QStringLiteral("canvas")] = canvas;

    root[QStringLiteral("layers")] = layersToJson(doc->layers());
    root[QStringLiteral("currentLayer")] = doc->currentLayerIndex();

    if (!doc->sourceMediaMRL().isEmpty())
    {
        QJsonObject media;
        media[QStringLiteral("mrl")] = doc->sourceMediaMRL();
        media[QStringLiteral("title")] = doc->sourceMediaTitle();
        media[QStringLiteral("timeMs")] = doc->sourceTimeMs();
        root[QStringLiteral("sourceMedia")] = media;
    }

    QJsonArray tracks;
    for (const ObjectTrack &track : doc->tracks())
    {
        QJsonObject t;
        t[QStringLiteral("id")] = track.id;
        t[QStringLiteral("sourceMRL")] = track.sourceMRL;
        t[QStringLiteral("startMs")] = track.startMs;
        t[QStringLiteral("endMs")] = track.endMs;
        t[QStringLiteral("label")] = track.label;
        t[QStringLiteral("complete")] = track.complete;
        QJsonObject effect;
        effect[QStringLiteral("type")] = track.effect.type;
        effect[QStringLiteral("intensity")] = track.effect.intensity;
        effect[QStringLiteral("color")] = track.effect.color;
        t[QStringLiteral("effect")] = effect;
        QJsonArray keyframes;
        for (const TrackKeyframe &kf : track.keyframes)
        {
            QJsonObject k;
            k[QStringLiteral("timeMs")] = kf.timeMs;
            k[QStringLiteral("frameIndex")] = kf.frameIndex;
            k[QStringLiteral("confidence")] = kf.confidence;
            k[QStringLiteral("corrected")] = kf.corrected;
            if (!kf.mask.isNull())
            {
                QByteArray png;
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                kf.mask.save(&buffer, "PNG");
                k[QStringLiteral("mask")] = QString::fromLatin1(png.toBase64());
            }
            keyframes.append(k);
        }
        t[QStringLiteral("keyframes")] = keyframes;
        tracks.append(t);
    }
    root[QStringLiteral("trackedObjects")] = tracks;

    root[QStringLiteral("documentSettings")] = QJsonObject();
    root[QStringLiteral("art")] = artToJson(doc);
    root[QStringLiteral("harvest")] = harvestToJson(doc->harvest());

    *out = QJsonDocument(root).toJson(QJsonDocument::Indented);
    return {};
}

QString ProjectStore::save(PaintbrushDocument *doc, const QString &path)
{
    QByteArray data;
    const QString err = saveToBytes(doc, &data);
    if (!err.isEmpty())
        return err;

    /* Atomic crash-safe write: QSaveFile writes a temp file in the same
     * directory and atomically replaces the destination on commit() — the
     * old file is never deleted before the new one is in place, so a
     * failed write can never lose the previous project. */
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
    if (file.write(data) != data.size() || !file.commit())
        return QStringLiteral("Cannot replace %1: %2").arg(path, file.errorString());
    return {};
}

QString ProjectStore::loadFromBytes(PaintbrushDocument *doc,
                                    const QByteArray &data) const
{
    const QJsonDocument docJson = QJsonDocument::fromJson(data);
    if (!docJson.isObject())
        return QStringLiteral("Not a valid Paintbrush project file.");

    const QJsonObject root = docJson.object();

    /* forward compatibility: unknown fields are simply ignored on load */
    const int version = root[QStringLiteral("version")].toInt(1);
    if (version > FORMAT_VERSION)
        return QStringLiteral("Project was saved by a newer Paintbrush "
                              "version (%1 > %2).").arg(version).arg(FORMAT_VERSION);

    QSize canvasSize(1280, 720);
    const QJsonObject canvas = root[QStringLiteral("canvas")].toObject();
    if (!canvas.isEmpty())
    {
        canvasSize = QSize(canvas[QStringLiteral("width")].toInt(1280),
                           canvas[QStringLiteral("height")].toInt(720));
    }
    doc->resetDocument(canvasSize);

    const QList<CanvasLayer> layers =
        layersFromJson(root[QStringLiteral("layers")].toArray());
    if (!layers.isEmpty())
        doc->setLayers(layers);

    doc->setCurrentLayerIndex(root[QStringLiteral("currentLayer")].toInt(0));

    const QJsonObject media = root[QStringLiteral("sourceMedia")].toObject();
    if (!media.isEmpty())
    {
        doc->setSourceMedia(media[QStringLiteral("mrl")].toString(),
                            media[QStringLiteral("title")].toString(),
                            qint64(media[QStringLiteral("timeMs")].toDouble(0)));
    }

    QVector<ObjectTrack> tracks;
    const QJsonArray trackArray = root[QStringLiteral("trackedObjects")].toArray();
    for (const QJsonValue &value : trackArray)
    {
        const QJsonObject t = value.toObject();
        ObjectTrack track;
        track.id = t[QStringLiteral("id")].toInt();
        track.sourceMRL = t[QStringLiteral("sourceMRL")].toString();
        track.startMs = qint64(t[QStringLiteral("startMs")].toDouble());
        track.endMs = qint64(t[QStringLiteral("endMs")].toDouble());
        track.label = t[QStringLiteral("label")].toString();
        track.complete = t[QStringLiteral("complete")].toBool(true);
        const QJsonObject effect = t[QStringLiteral("effect")].toObject();
        if (!effect.isEmpty())
        {
            track.effect.type = effect[QStringLiteral("type")].toInt();
            track.effect.intensity = effect[QStringLiteral("intensity")].toInt(50);
            track.effect.color = effect[QStringLiteral("color")].toString();
        }
        const QJsonArray kfArray = t[QStringLiteral("keyframes")].toArray();
        for (const QJsonValue &kfv : kfArray)
        {
            const QJsonObject k = kfv.toObject();
            TrackKeyframe kf;
            kf.timeMs = qint64(k[QStringLiteral("timeMs")].toDouble());
            kf.frameIndex = k[QStringLiteral("frameIndex")].toInt();
            kf.confidence = float(k[QStringLiteral("confidence")].toDouble(1.0));
            kf.corrected = k[QStringLiteral("corrected")].toBool(false);
            const QByteArray png = QByteArray::fromBase64(
                k[QStringLiteral("mask")].toString().toLatin1());
            if (!png.isEmpty())
                kf.mask.loadFromData(png, "PNG");
            track.keyframes.append(kf);
        }
        tracks.append(std::move(track));
    }
    doc->setTracks(tracks);

    /* Paintbrush 2.0 art state + harvest tray (absent in older projects:
     * the defaults from resetDocument stay) */
    artFromJson(doc, root[QStringLiteral("art")].toObject());
    doc->harvest()->setItems(harvestFromJson(root[QStringLiteral("harvest")].toArray()));

    doc->setDirty(false);
    return {};
}

QString ProjectStore::load(PaintbrushDocument *doc, const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("Cannot open %1: %2").arg(path, file.errorString());
    const QByteArray data = file.readAll();
    file.close();
    return loadFromBytes(doc, data);
}

} // namespace paintbrush
#include "pb_subject_store.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace paintbrush
{

QVariantMap DerivedArtwork::toVariantMap() const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("sourceSubjectId"), sourceSubjectId);
    m.insert(QStringLiteral("sourceRevisionId"), sourceRevisionId);
    m.insert(QStringLiteral("mode"), mode);
    m.insert(QStringLiteral("styleRecipeId"), styleRecipeId);
    m.insert(QStringLiteral("contourLayerIds"), contourLayerIds);
    m.insert(QStringLiteral("fillLayerIds"), fillLayerIds);
    m.insert(QStringLiteral("shadingLayerIds"), shadingLayerIds);
    m.insert(QStringLiteral("textureLayerIds"), textureLayerIds);
    m.insert(QStringLiteral("editableStrokeSetId"), editableStrokeSetId);
    m.insert(QStringLiteral("regionMapId"), regionMapId);
    m.insert(QStringLiteral("provenanceId"), provenanceId);
    m.insert(QStringLiteral("createdUtc"), createdUtc);
    return m;
}

DerivedArtwork DerivedArtwork::fromVariantMap(const QVariantMap &m)
{
    DerivedArtwork a;
    a.id = m.value(QStringLiteral("id")).toString().trimmed();
    a.sourceSubjectId = m.value(QStringLiteral("sourceSubjectId")).toString();
    a.sourceRevisionId = m.value(QStringLiteral("sourceRevisionId")).toString();
    a.mode = m.value(QStringLiteral("mode")).toString();
    a.styleRecipeId = m.value(QStringLiteral("styleRecipeId")).toString();
    a.contourLayerIds = m.value(QStringLiteral("contourLayerIds")).toStringList();
    a.fillLayerIds = m.value(QStringLiteral("fillLayerIds")).toStringList();
    a.shadingLayerIds = m.value(QStringLiteral("shadingLayerIds")).toStringList();
    a.textureLayerIds = m.value(QStringLiteral("textureLayerIds")).toStringList();
    a.editableStrokeSetId = m.value(QStringLiteral("editableStrokeSetId")).toString();
    a.regionMapId = m.value(QStringLiteral("regionMapId")).toString();
    a.provenanceId = m.value(QStringLiteral("provenanceId")).toString();
    a.createdUtc = m.value(QStringLiteral("createdUtc")).toString();
    return a;
}

QString SubjectStore::artworkDir() const
{
    return userDir() + QStringLiteral("/artwork");
}

bool SubjectStore::validateArtwork(const DerivedArtwork &a,
                                   const SubjectAsset *subject,
                                   QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (a.id.isEmpty())
        return fail(QStringLiteral("Artwork id is required."));
    if (!subject)
        return fail(QStringLiteral("Artwork references an unknown subject."));
    if (a.mode != QStringLiteral("raster") && a.mode != QStringLiteral("vector")
        && a.mode != QStringLiteral("hybrid"))
        return fail(QStringLiteral("Mode must be raster, vector or hybrid."));
    if (a.provenanceId.isEmpty())
        return fail(QStringLiteral("Artwork needs provenance."));
    if (a.mode != QStringLiteral("raster") && a.contourLayerIds.isEmpty()
        && a.fillLayerIds.isEmpty() && a.editableStrokeSetId.isEmpty())
        return fail(QStringLiteral("Vector/hybrid artwork needs contours, fills or strokes."));
    if (error)
        error->clear();
    return true;
}

QVector<DerivedArtwork> SubjectStore::artworks(const QString &sourceSubjectId) const
{
    QVector<DerivedArtwork> out;
    for (const DerivedArtwork &a : m_artworks)
        if (a.sourceSubjectId == sourceSubjectId)
            out.append(a);
    return out;
}

const DerivedArtwork *SubjectStore::findArtwork(const QString &id) const
{
    for (const DerivedArtwork &a : m_artworks)
        if (a.id == id)
            return &a;
    return nullptr;
}

bool SubjectStore::saveArtwork(const DerivedArtwork &artwork, QString *error)
{
    if (!validateArtwork(artwork, find(artwork.sourceSubjectId), error))
        return false;
    DerivedArtwork stored = artwork;
    if (stored.createdUtc.isEmpty())
        stored.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!writeArtworkFile(stored, error))
        return false;
    reloadArtworks();
    return true;
}

bool SubjectStore::removeArtwork(const QString &id, QString *error)
{
    if (!findArtwork(id))
    {
        if (error)
            *error = QStringLiteral("Artwork not found.");
        return false;
    }
    QFile f(artworkDir() + QLatin1Char('/') + id + QStringLiteral(".json"));
    if (!f.remove())
    {
        if (error)
            *error = QStringLiteral("Could not delete the artwork file.");
        return false;
    }
    reloadArtworks();
    return true;
}

void SubjectStore::reloadArtworks()
{
    m_artworks.clear();
    QDir dir(artworkDir());
    if (!dir.exists())
        return;
    const QFileInfoList files =
        dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo &fi : files)
    {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject())
            continue;
        const DerivedArtwork a =
            DerivedArtwork::fromVariantMap(doc.object().toVariantMap());
        if (validateArtwork(a, find(a.sourceSubjectId), nullptr))
            m_artworks.append(a);
    }
}

bool SubjectStore::writeArtworkFile(const DerivedArtwork &artwork,
                                    QString *error) const
{
    QDir dir(artworkDir());
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
    {
        if (error)
            *error = QStringLiteral("Could not create the artwork folder.");
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("$schema"),
                QStringLiteral("nai-paintbrush/derived-artwork/v1"));
    root.insert(QStringLiteral("version"), 1);
    const QVariantMap map = artwork.toVariantMap();
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        root.insert(it.key(), QJsonValue::fromVariant(it.value()));

    QFile f(artworkDir() + QLatin1Char('/') + artwork.id + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (error)
            *error = QStringLiteral("Could not write the artwork file.");
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace paintbrush

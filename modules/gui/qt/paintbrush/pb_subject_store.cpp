#include "pb_subject_store.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace paintbrush
{

bool SubjectAsset::isValid() const
{
    return SubjectStore::validate(*this, nullptr);
}

QVariantMap SubjectAsset::toVariantMap() const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("name"), name);
    m.insert(QStringLiteral("sourceMediaId"), sourceMediaId);
    m.insert(QStringLiteral("sourcePtsMs"), sourcePtsMs);
    m.insert(QStringLiteral("identityTrackId"), identityTrackId);
    m.insert(QStringLiteral("segmentationTrackId"), segmentationTrackId);
    m.insert(QStringLiteral("defaultEffectsTrackId"), defaultEffectsTrackId);
    m.insert(QStringLiteral("derivedArtworkIds"), derivedArtworkIds);
    m.insert(QStringLiteral("createdUtc"), createdUtc);
    return m;
}

SubjectAsset SubjectAsset::fromVariantMap(const QVariantMap &m)
{
    SubjectAsset a;
    a.id = m.value(QStringLiteral("id")).toString().trimmed();
    a.name = m.value(QStringLiteral("name")).toString().trimmed();
    a.sourceMediaId = m.value(QStringLiteral("sourceMediaId")).toString();
    a.sourcePtsMs = m.value(QStringLiteral("sourcePtsMs")).toLongLong();
    a.identityTrackId = m.value(QStringLiteral("identityTrackId")).toString();
    a.segmentationTrackId = m.value(QStringLiteral("segmentationTrackId")).toString();
    a.defaultEffectsTrackId = m.value(QStringLiteral("defaultEffectsTrackId")).toString();
    a.derivedArtworkIds = m.value(QStringLiteral("derivedArtworkIds")).toStringList();
    a.createdUtc = m.value(QStringLiteral("createdUtc")).toString();
    return a;
}

SubjectStore::SubjectStore()
{
    reload();
}

QString SubjectStore::userDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/paintbrush/subjects");
}

bool SubjectStore::validate(const SubjectAsset &a, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (a.id.isEmpty())
        return fail(QStringLiteral("Subject id is required."));
    if (a.name.isEmpty())
        return fail(QStringLiteral("Subject name is required."));
    if (a.sourceMediaId.isEmpty())
        return fail(QStringLiteral("Source media is required."));
    if (a.segmentationTrackId.isEmpty())
        return fail(QStringLiteral("A subject needs a segmentation track."));
    /* Composition placement must never live on the shared asset. */
    if (a.toVariantMap().contains(QStringLiteral("transformTrackId")))
        return fail(QStringLiteral("Transform tracks belong to SubjectInstance."));
    if (error)
        error->clear();
    return true;
}

QVector<SubjectAsset> SubjectStore::all() const
{
    return m_assets;
}

const SubjectAsset *SubjectStore::find(const QString &id) const
{
    for (const SubjectAsset &a : m_assets)
        if (a.id == id)
            return &a;
    return nullptr;
}

bool SubjectStore::save(const SubjectAsset &asset, QString *error)
{
    if (!validate(asset, error))
        return false;
    SubjectAsset stored = asset;
    if (stored.createdUtc.isEmpty())
        stored.createdUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!writeFile(stored, error))
        return false;
    reload();
    return true;
}

bool SubjectStore::remove(const QString &id, QString *error)
{
    if (!find(id))
    {
        if (error)
            *error = QStringLiteral("Subject not found.");
        return false;
    }
    QFile f(userDir() + QLatin1Char('/') + id + QStringLiteral(".json"));
    if (!f.remove())
    {
        if (error)
            *error = QStringLiteral("Could not delete the subject file.");
        return false;
    }
    reload();
    return true;
}

void SubjectStore::reload()
{
    m_assets.clear();
    QDir dir(userDir());
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
        const SubjectAsset a = SubjectAsset::fromVariantMap(doc.object().toVariantMap());
        if (a.isValid())
            m_assets.append(a);
    }
}

bool SubjectStore::writeFile(const SubjectAsset &asset, QString *error) const
{
    QDir dir(userDir());
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
    {
        if (error)
            *error = QStringLiteral("Could not create the subject folder.");
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("$schema"), QStringLiteral("nai-paintbrush/subject/v1"));
    root.insert(QStringLiteral("version"), 1);
    const QVariantMap map = asset.toVariantMap();
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        root.insert(it.key(), QJsonValue::fromVariant(it.value()));

    QFile f(userDir() + QLatin1Char('/') + asset.id + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (error)
            *error = QStringLiteral("Could not write the subject file.");
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace paintbrush

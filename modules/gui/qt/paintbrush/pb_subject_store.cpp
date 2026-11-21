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
    reloadInstances();
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

QVariantMap SubjectInstance::toVariantMap() const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("subjectAssetId"), subjectAssetId);
    m.insert(QStringLiteral("compositionId"), compositionId);
    m.insert(QStringLiteral("sourceStartMs"), sourceStartMs);
    m.insert(QStringLiteral("sourceDurationMs"), sourceDurationMs);
    m.insert(QStringLiteral("x"), x);
    m.insert(QStringLiteral("y"), y);
    m.insert(QStringLiteral("scale"), scale);
    m.insert(QStringLiteral("rotation"), rotation);
    m.insert(QStringLiteral("zIndex"), zIndex);
    m.insert(QStringLiteral("visible"), visible);
    m.insert(QStringLiteral("instanceEffectsTrackId"), instanceEffectsTrackId);
    return m;
}

SubjectInstance SubjectInstance::fromVariantMap(const QVariantMap &m)
{
    SubjectInstance i;
    i.id = m.value(QStringLiteral("id")).toString().trimmed();
    i.subjectAssetId = m.value(QStringLiteral("subjectAssetId")).toString();
    i.compositionId = m.value(QStringLiteral("compositionId")).toString();
    i.sourceStartMs = m.value(QStringLiteral("sourceStartMs")).toLongLong();
    i.sourceDurationMs = m.value(QStringLiteral("sourceDurationMs")).toLongLong();
    i.x = m.value(QStringLiteral("x")).toDouble();
    i.y = m.value(QStringLiteral("y")).toDouble();
    i.scale = m.value(QStringLiteral("scale"), 1.0).toDouble();
    i.rotation = m.value(QStringLiteral("rotation")).toDouble();
    i.zIndex = m.value(QStringLiteral("zIndex")).toInt();
    i.visible = m.value(QStringLiteral("visible"), true).toBool();
    i.instanceEffectsTrackId = m.value(QStringLiteral("instanceEffectsTrackId")).toString();
    return i;
}

QString SubjectStore::instanceDir() const
{
    return userDir() + QStringLiteral("/instances");
}

bool SubjectStore::validateInstance(const SubjectInstance &i,
                                    const SubjectAsset *asset,
                                    QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (i.id.isEmpty())
        return fail(QStringLiteral("Instance id is required."));
    if (i.compositionId.isEmpty())
        return fail(QStringLiteral("Instance needs a composition."));
    if (!asset)
        return fail(QStringLiteral("Instance references an unknown subject."));
    if (i.scale <= 0.0)
        return fail(QStringLiteral("Instance scale must be positive."));
    if (error)
        error->clear();
    return true;
}

QVector<SubjectInstance> SubjectStore::instances(const QString &compositionId) const
{
    QVector<SubjectInstance> out;
    for (const SubjectInstance &i : m_instances)
        if (i.compositionId == compositionId)
            out.append(i);
    return out;
}

const SubjectInstance *SubjectStore::findInstance(const QString &id) const
{
    for (const SubjectInstance &i : m_instances)
        if (i.id == id)
            return &i;
    return nullptr;
}

bool SubjectStore::saveInstance(const SubjectInstance &instance, QString *error)
{
    if (!validateInstance(instance, find(instance.subjectAssetId), error))
        return false;
    if (!writeInstanceFile(instance, error))
        return false;
    reloadInstances();
    return true;
}

bool SubjectStore::removeInstance(const QString &id, QString *error)
{
    if (!findInstance(id))
    {
        if (error)
            *error = QStringLiteral("Instance not found.");
        return false;
    }
    QFile f(instanceDir() + QLatin1Char('/') + id + QStringLiteral(".json"));
    if (!f.remove())
    {
        if (error)
            *error = QStringLiteral("Could not delete the instance file.");
        return false;
    }
    reloadInstances();
    return true;
}

void SubjectStore::reloadInstances()
{
    m_instances.clear();
    QDir dir(instanceDir());
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
        const SubjectInstance i =
            SubjectInstance::fromVariantMap(doc.object().toVariantMap());
        if (validateInstance(i, find(i.subjectAssetId), nullptr))
            m_instances.append(i);
    }
}

bool SubjectStore::writeInstanceFile(const SubjectInstance &instance, QString *error) const
{
    QDir dir(instanceDir());
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
    {
        if (error)
            *error = QStringLiteral("Could not create the instance folder.");
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("$schema"), QStringLiteral("nai-paintbrush/subject-instance/v1"));
    root.insert(QStringLiteral("version"), 1);
    const QVariantMap map = instance.toVariantMap();
    for (auto it = map.cbegin(); it != map.cend(); ++it)
        root.insert(it.key(), QJsonValue::fromVariant(it.value()));

    QFile f(instanceDir() + QLatin1Char('/') + instance.id + QStringLiteral(".json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (error)
            *error = QStringLiteral("Could not write the instance file.");
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace paintbrush
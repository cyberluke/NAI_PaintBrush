#ifndef PB_SUBJECT_STORE_HPP
#define PB_SUBJECT_STORE_HPP

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace paintbrush
{

struct SubjectAsset
{
    QString id;
    QString name;
    QString sourceMediaId;
    qint64 sourcePtsMs = 0;
    QString identityTrackId;
    QString segmentationTrackId;
    QString defaultEffectsTrackId;
    QStringList derivedArtworkIds;
    QString createdUtc;

    bool isValid() const;
    QVariantMap toVariantMap() const;
    static SubjectAsset fromVariantMap(const QVariantMap &map);
};

/* SubjectInstance (ADR 1.2): one placement of an asset inside a composition.
 * All composition-specific state lives HERE, never on the asset, so two
 * instances of one performer transform and style independently. */
struct SubjectInstance
{
    QString id;
    QString subjectAssetId;
    QString compositionId;
    qint64 sourceStartMs = 0;       /* trim in-point in source time          */
    qint64 sourceDurationMs = 0;    /* 0 = to the end of the source          */
    double x = 0.0;
    double y = 0.0;
    double scale = 1.0;
    double rotation = 0.0;
    int zIndex = 0;
    bool visible = true;
    QString instanceEffectsTrackId;

    QVariantMap toVariantMap() const;
    static SubjectInstance fromVariantMap(const QVariantMap &map);
};

class SubjectStore
{
public:
    SubjectStore();

    QVector<SubjectAsset> all() const;
    const SubjectAsset *find(const QString &id) const;

    bool save(const SubjectAsset &asset, QString *error = nullptr);
    bool remove(const QString &id, QString *error = nullptr);

    /* Instances are scoped to a composition. */
    QVector<SubjectInstance> instances(const QString &compositionId) const;
    const SubjectInstance *findInstance(const QString &id) const;
    bool saveInstance(const SubjectInstance &instance, QString *error = nullptr);
    bool removeInstance(const QString &id, QString *error = nullptr);

    QString userDir() const;
    static bool validate(const SubjectAsset &asset, QString *error);
    static bool validateInstance(const SubjectInstance &instance,
                                 const SubjectAsset *asset,
                                 QString *error);

private:
    void reload();
    bool writeFile(const SubjectAsset &asset, QString *error) const;
    bool writeInstanceFile(const SubjectInstance &instance, QString *error) const;
    QString instanceDir() const;
    void reloadInstances();

    QVector<SubjectAsset> m_assets;
    QVector<SubjectInstance> m_instances;
};

} // namespace paintbrush

namespace paintbrush { SubjectStore &sharedSubjectStore(); }

#endif

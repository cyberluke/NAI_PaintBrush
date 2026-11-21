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

class SubjectStore
{
public:
    SubjectStore();

    QVector<SubjectAsset> all() const;
    const SubjectAsset *find(const QString &id) const;

    bool save(const SubjectAsset &asset, QString *error = nullptr);
    bool remove(const QString &id, QString *error = nullptr);

    QString userDir() const;
    static bool validate(const SubjectAsset &asset, QString *error);

private:
    void reload();
    bool writeFile(const SubjectAsset &asset, QString *error) const;

    QVector<SubjectAsset> m_assets;
};

} // namespace paintbrush

#endif

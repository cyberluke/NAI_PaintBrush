/*****************************************************************************
 * pb_lookup_service.hpp : NAI Paintbrush — object/product lookup extension
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_LOOKUP_SERVICE_HPP
#define PB_LOOKUP_SERVICE_HPP

#include <QObject>
#include <QString>
#include <QImage>
#include <QVariantList>

struct vlc_object_t;

namespace paintbrush {

/* Object lookup extension point ("what handbag is this?").
 *
 * The editor itself is deliberately decoupled from any shopping/search
 * vendor: a future provider plugs in here and returns candidate matches.
 * The default state is *unavailable unless configured*, so the "Find
 * product" action is never a silent fake.
 *
 * Input : object crop + VLM description + optional OCR + optional query.
 * Output: candidate matches with confidence/evidence/metadata/source URLs.
 */
class LookupService : public QObject
{
    Q_OBJECT

public:
    struct Match
    {
        QString title;
        QString description;
        QString sourceUrl;
        QString imageUrl;
        double confidence = 0.0;
        QString evidence;
        QVariantMap metadata;
    };

    explicit LookupService(QObject *parent = nullptr);

    void configure(vlc_object_t *obj);
    bool isConfigured() const { return m_configured; }
    QString unavailableReason() const { return m_unavailableReason; }
    QString providerName() const { return m_providerName; }

    /* Returns false when the provider is unavailable; when available, the
     * provider should emit lookupResult/errorOccurred asynchronously. */
    bool lookup(const QImage &objectCrop, const QString &vlmDescription,
                const QString &ocrText, const QString &query);

    QVariantList matches() const;

signals:
    void lookupResult(int requestId, const QVariantList &matches);
    void errorOccurred(const QString &message);
    void availabilityChanged(bool available, const QString &reason);

private:
    bool m_configured = false;
    QString m_providerName;
    QString m_unavailableReason;
    QVariantList m_matches;
};

} // namespace paintbrush

#endif // PB_LOOKUP_SERVICE_HPP